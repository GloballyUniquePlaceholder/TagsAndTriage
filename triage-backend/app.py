from flask import Flask, request, jsonify, send_from_directory
import sqlite3
from datetime import datetime

app = Flask(__name__)
DB = "triage.db"

# --- Schema note ---
# If you already created your DB from the earlier schema, run this once to add
# the new column used for staff-overridden recheck intervals:
#
#   ALTER TABLE checks ADD COLUMN next_check_minutes INTEGER;
#
# Full schema for reference (new DBs):
#
# CREATE TABLE patients (
#     patient_id TEXT PRIMARY KEY,
#     zone TEXT,
#     checked_out_at TEXT,
#     checkout_reason TEXT,
#     created_at TEXT
# );
#
# CREATE TABLE checks (
#     check_id INTEGER PRIMARY KEY AUTOINCREMENT,
#     patient_id TEXT,
#     can_walk INTEGER,
#     initial_breathing INTEGER,
#     breathing_after_reposition INTEGER,
#     breathing_rate INTEGER,
#     pulse_present INTEGER,
#     responsive INTEGER,
#     notes TEXT,
#     priority_result TEXT,
#     next_check_minutes INTEGER,
#     timestamp TEXT,
#     FOREIGN KEY (patient_id) REFERENCES patients(patient_id)
# );
#
# CREATE TABLE treatments (
#     treatment_id INTEGER PRIMARY KEY AUTOINCREMENT,
#     patient_id TEXT,
#     treatment_type TEXT,
#     interval_minutes INTEGER,
#     last_given TEXT,
#     FOREIGN KEY (patient_id) REFERENCES patients(patient_id)
# );

# Default reassessment intervals by priority — staff can override per-check.
# NOT a universal clinical standard, just sensible defaults for this system.
REASSESSMENT_INTERVALS_MIN = {
    "red": 10,
    "yellow": 30,
    "green": 120
    # "black" intentionally excluded — not applicable
}


def get_db():
    conn = sqlite3.connect(DB)
    conn.row_factory = sqlite3.Row
    return conn


def calculate_priority(can_walk, initial_breathing, breathing_after_reposition,
                        breathing_rate, pulse_present, responsive):
    if can_walk:
        return "green"
    if not initial_breathing:
        return "red" if breathing_after_reposition else "black"
    if breathing_rate is not None and breathing_rate > 30:
        return "red"
    if not pulse_present:
        return "red"
    if not responsive:
        return "red"
    return "yellow"


def validate_checkin(data):
    """Mirrors the frontend's branching validation server-side — never trust
    the client alone. Returns an error string, or None if the payload is valid
    for the branch it's on."""
    if not data.get("patient_id"):
        return "patient_id is required"
    if data.get("can_walk") is None:
        return "can_walk is required"

    if data["can_walk"]:
        return None  # Green — no further questions required

    if data.get("initial_breathing") is None:
        return "initial_breathing is required"

    if not data["initial_breathing"]:
        if data.get("breathing_after_reposition") is None:
            return "breathing_after_reposition is required"
        return None

    # initial_breathing is truthy -> need rate, pulse, responsive
    if data.get("breathing_rate") is None:
        return "breathing_rate is required"
    if data.get("pulse_present") is None:
        return "pulse_present is required"
    if data.get("responsive") is None:
        return "responsive is required"
    return None


def get_overdue_by_patient(conn):
    """Returns { patient_id: [ {treatment_id, treatment_type, minutes_overdue}, ... ] }"""
    rows = conn.execute("""
        SELECT treatment_id, patient_id, treatment_type, interval_minutes, last_given
        FROM treatments
    """).fetchall()
    now = datetime.utcnow()
    overdue = {}
    for r in rows:
        if r["last_given"] is None:
            minutes_overdue = None
            is_overdue = True
        else:
            last = datetime.strptime(r["last_given"], "%Y-%m-%d %H:%M:%S")
            elapsed_minutes = (now - last).total_seconds() / 60
            is_overdue = elapsed_minutes >= r["interval_minutes"]
            minutes_overdue = round(elapsed_minutes - r["interval_minutes"]) if is_overdue else None
        if is_overdue:
            overdue.setdefault(r["patient_id"], []).append({
                "treatment_id": r["treatment_id"],
                "treatment_type": r["treatment_type"],
                "minutes_overdue": minutes_overdue
            })
    return overdue


def get_recheck_status(priority, last_check_str, next_check_minutes=None):
    """Computes vitals-recheck status from priority + last check time.
       Uses a staff-entered override interval when present, otherwise falls
       back to the priority-based default. Derived on read, same pattern as
       overdue treatments — no separate "due" table needed."""
    if priority == "black" or priority is None or last_check_str is None:
        return {"status": "n/a", "minutes": None}

    interval = next_check_minutes if next_check_minutes else REASSESSMENT_INTERVALS_MIN.get(priority)
    if interval is None:
        return {"status": "n/a", "minutes": None}

    last_check = datetime.strptime(last_check_str, "%Y-%m-%d %H:%M:%S")
    elapsed = (datetime.utcnow() - last_check).total_seconds() / 60
    remaining = round(interval - elapsed)

    if remaining <= 0:
        return {"status": "overdue", "minutes": abs(remaining)}
    elif remaining <= 5:
        return {"status": "due_soon", "minutes": remaining}
    else:
        return {"status": "ok", "minutes": remaining}


# --- 1. Check-in: called when a patient is triaged or rechecked ---
@app.route("/api/checkin", methods=["POST"])
def checkin():
    data = request.json or {}

    error = validate_checkin(data)
    if error:
        return jsonify({"error": error}), 400

    priority = calculate_priority(
        data["can_walk"], data.get("initial_breathing"),
        data.get("breathing_after_reposition"), data.get("breathing_rate"),
        data.get("pulse_present"), data.get("responsive")
    )

    conn = get_db()
    conn.execute(
        "INSERT OR IGNORE INTO patients (patient_id, created_at) VALUES (?, datetime('now'))",
        (data["patient_id"],)
    )
    conn.execute("""
        INSERT INTO checks (patient_id, can_walk, initial_breathing,
            breathing_after_reposition, breathing_rate, pulse_present,
            responsive, notes, priority_result, next_check_minutes, timestamp)
        VALUES (?,?,?,?,?,?,?,?,?,?,datetime('now'))
    """, (data["patient_id"], data["can_walk"], data.get("initial_breathing"),
          data.get("breathing_after_reposition"), data.get("breathing_rate"),
          data.get("pulse_present"), data.get("responsive"), data.get("notes"),
          priority, data.get("next_check_minutes")))
    conn.commit()
    conn.close()
    return jsonify({"priority": priority})


# --- 2. Schedule a treatment for a patient ---
@app.route("/api/treatments", methods=["POST"])
def add_treatment():
    data = request.json or {}
    if not data.get("patient_id") or not data.get("treatment_type") or not data.get("interval_minutes"):
        return jsonify({"error": "patient_id, treatment_type, and interval_minutes are required"}), 400

    conn = get_db()
    exists = conn.execute("SELECT 1 FROM patients WHERE patient_id = ?",
                           (data["patient_id"],)).fetchone()
    if not exists:
        conn.close()
        return jsonify({"error": "Unknown patient_id"}), 400
    conn.execute("""
        INSERT INTO treatments (patient_id, treatment_type, interval_minutes, last_given)
        VALUES (?, ?, ?, NULL)
    """, (data["patient_id"], data["treatment_type"], data["interval_minutes"]))
    conn.commit()
    conn.close()
    return jsonify({"status": "ok"})


# --- 3. Mark a treatment as given ---
@app.route("/api/treatments/given", methods=["POST"])
def mark_treatment_given():
    data = request.json or {}
    if not data.get("treatment_id"):
        return jsonify({"error": "treatment_id is required"}), 400
    conn = get_db()
    conn.execute(
        "UPDATE treatments SET last_given = datetime('now') WHERE treatment_id = ?",
        (data["treatment_id"],)
    )
    conn.commit()
    conn.close()
    return jsonify({"status": "ok"})


# --- 4. Distinct treatment types already in use — powers the autocomplete list ---
@app.route("/api/treatment-types", methods=["GET"])
def treatment_types():
    conn = get_db()
    rows = conn.execute(
        "SELECT DISTINCT treatment_type FROM treatments ORDER BY treatment_type"
    ).fetchall()
    conn.close()
    return jsonify([r["treatment_type"] for r in rows])


# --- 5. Get all patients, with priority/notes, overdue treatments, and recheck status ---
@app.route("/api/patients", methods=["GET"])
def get_patients():
    conn = get_db()
    rows = conn.execute("""
        SELECT p.patient_id,
               c.priority_result AS priority,
               c.timestamp AS last_check,
               c.notes AS notes,
               c.next_check_minutes AS next_check_minutes
        FROM patients p
        LEFT JOIN checks c ON c.check_id = (
            SELECT check_id FROM checks
            WHERE patient_id = p.patient_id
            ORDER BY timestamp DESC LIMIT 1
        )
    """).fetchall()

    overdue_by_patient = get_overdue_by_patient(conn)
    conn.close()

    result = []
    for row in rows:
        patient = dict(row)
        patient["overdue_treatments"] = overdue_by_patient.get(row["patient_id"], [])
        patient["recheck"] = get_recheck_status(row["priority"], row["last_check"], row["next_check_minutes"])
        result.append(patient)
    return jsonify(result)


# --- 6. Get full detail for ONE patient: current status + full check history + treatments ---
@app.route("/api/patients/<patient_id>", methods=["GET"])
def get_patient_detail(patient_id):
    conn = get_db()
    patient = conn.execute(
        "SELECT * FROM patients WHERE patient_id = ?", (patient_id,)
    ).fetchone()
    if not patient:
        conn.close()
        return jsonify({"error": "Patient not found"}), 404

    checks = conn.execute(
        "SELECT * FROM checks WHERE patient_id = ? ORDER BY timestamp DESC",
        (patient_id,)
    ).fetchall()
    treatments = conn.execute(
        "SELECT * FROM treatments WHERE patient_id = ?", (patient_id,)
    ).fetchall()

    latest = checks[0] if checks else None
    priority = latest["priority_result"] if latest else None
    last_check = latest["timestamp"] if latest else None
    notes = latest["notes"] if latest else None
    next_check_minutes = latest["next_check_minutes"] if latest else None

    overdue_by_patient = get_overdue_by_patient(conn)
    conn.close()

    return jsonify({
        "patient_id": patient_id,
        "priority": priority,
        "last_check": last_check,
        "notes": notes,
        "recheck": get_recheck_status(priority, last_check, next_check_minutes),
        "overdue_treatments": overdue_by_patient.get(patient_id, []),
        "checks": [dict(c) for c in checks],
        "treatments": [dict(t) for t in treatments]
    })


# --- 7. Serve the dashboards ---
@app.route("/")
def dashboard():
    return send_from_directory("static", "dashboard.html")


@app.route("/patient/<patient_id>")
def patient_page(patient_id):
    return send_from_directory("static", "patient.html")


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5001, debug=True)
