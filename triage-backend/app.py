from flask import Flask, request, jsonify, send_from_directory
import sqlite3
import re
from datetime import datetime

app = Flask(__name__)
DB = "triage.db"

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
    device_number = data.get("device_number")

    if device_number is not None:
        try:
            int(device_number)
        except (ValueError, TypeError):
            return "device_number must be a number"
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

    if data.get("breathing_rate") is None:
        return "breathing_rate is required"
    if data.get("pulse_present") is None:
        return "pulse_present is required"
    if data.get("responsive") is None:
        return "responsive is required"
    return None


def generate_patient_id(conn, device_number=0):
    """Generate a new patient ID in the form HDDNNN.

    If device_number is provided, the ID is H{device_number:02d}{sequence:03d}.
    If no device_number is provided, the online form uses device 0 and creates
    IDs like H000001.
    """
    device_number = int(device_number)
    prefix = f"H{device_number:02d}"
    rows = conn.execute(
        "SELECT patient_id FROM patients WHERE patient_id LIKE ?",
        (prefix + '%',)
    ).fetchall()

    max_seq = 0
    for row in rows:
        match = re.match(rf"^{re.escape(prefix)}(\d{{3}})$", row["patient_id"])
        if match:
            max_seq = max(max_seq, int(match.group(1)))

    return f"{prefix}{max_seq + 1:03d}"


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
       back to the priority-based default."""
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


def zone_mismatch(zone_position, priority):
    """True if the patient's assigned zone no longer matches their current
    priority — e.g. they were placed at Yellow-3, then a recheck escalated
    them to Red. Signals staff to physically move + reassign, not just a
    data inconsistency to silently ignore."""
    if not zone_position or not priority:
        return False
    current_zone_label = zone_position.split("-")[0].strip().lower()
    return current_zone_label != priority.lower()


# --- 1. Check-in: called when a patient is triaged or rechecked.
#        Deliberately has NO zone/position field — priority isn't known
#        until this returns, so a zone can't be chosen before this runs. ---
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
    patient_id = data.get("patient_id")
    if not patient_id or str(patient_id).strip() == "":
        patient_id = generate_patient_id(conn, data.get("device_number", 0))
    else:
        patient_id = str(patient_id).strip()

    conn.execute(
        "INSERT OR IGNORE INTO patients (patient_id, created_at) VALUES (?, datetime('now'))",
        (patient_id,)
    )
    conn.execute("""
        INSERT INTO checks (patient_id, can_walk, initial_breathing,
            breathing_after_reposition, breathing_rate, pulse_present,
            responsive, notes, priority_result, next_check_minutes, timestamp)
        VALUES (?,?,?,?,?,?,?,?,?,?,datetime('now'))
    """, (patient_id, data["can_walk"], data.get("initial_breathing"),
          data.get("breathing_after_reposition"), data.get("breathing_rate"),
          data.get("pulse_present"), data.get("responsive"), data.get("notes"),
          priority, data.get("next_check_minutes")))
    conn.commit()
    conn.close()
    return jsonify({"priority": priority, "patient_id": patient_id})


# --- 2. Assign / move a patient's physical position — ONLY callable once a
#        priority exists. Staff supply just a spot number; the zone letter is
#        derived from the patient's current priority, so it can never be
#        typed wrong or set before triage happens. ---
@app.route("/api/assign-position", methods=["POST"])
def assign_position():
    data = request.json or {}
    patient_id = data.get("patient_id")
    position_number = data.get("position_number")
    if not patient_id or not position_number:
        return jsonify({"error": "patient_id and position_number are required"}), 400

    conn = get_db()
    latest = conn.execute("""
        SELECT priority_result FROM checks
        WHERE patient_id = ? ORDER BY timestamp DESC LIMIT 1
    """, (patient_id,)).fetchone()

    if not latest or not latest["priority_result"]:
        conn.close()
        return jsonify({"error": "Patient must be triaged before a zone position can be assigned"}), 400

    zone_label = latest["priority_result"].capitalize()
    new_position = f"{zone_label}-{position_number}"

    conn.execute("UPDATE patients SET zone_position = ? WHERE patient_id = ?",
                 (new_position, patient_id))
    conn.commit()
    conn.close()
    return jsonify({"zone_position": new_position})


# --- 3. Schedule a treatment for a patient ---
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


# --- 4. Mark a treatment as given ---
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


# --- 5. Distinct treatment types already in use — powers the autocomplete list ---
@app.route("/api/treatment-types", methods=["GET"])
def treatment_types():
    conn = get_db()
    rows = conn.execute(
        "SELECT DISTINCT treatment_type FROM treatments ORDER BY treatment_type"
    ).fetchall()
    conn.close()
    return jsonify([r["treatment_type"] for r in rows])


# --- 6. Get all patients, with priority/notes/zone, overdue treatments, and recheck status ---
@app.route("/api/patients", methods=["GET"])
def get_patients():
    conn = get_db()
    rows = conn.execute("""
        SELECT p.patient_id,
               p.zone_position AS zone_position,
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
        WHERE p.checked_out_at IS NULL
    """).fetchall()

    overdue_by_patient = get_overdue_by_patient(conn)
    conn.close()

    result = []
    for row in rows:
        patient = dict(row)
        patient["overdue_treatments"] = overdue_by_patient.get(row["patient_id"], [])
        patient["recheck"] = get_recheck_status(row["priority"], row["last_check"], row["next_check_minutes"])
        patient["zone_mismatch"] = zone_mismatch(row["zone_position"], row["priority"])
        result.append(patient)
    return jsonify(result)


# --- 7. Get full detail for ONE patient: current status + full check history + treatments ---
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
        "zone_position": patient["zone_position"],
        "zone_mismatch": zone_mismatch(patient["zone_position"], priority),
        "recheck": get_recheck_status(priority, last_check, next_check_minutes),
        "overdue_treatments": overdue_by_patient.get(patient_id, []),
        "checks": [dict(c) for c in checks],
        "treatments": [dict(t) for t in treatments]
    })


# --- 8. Serve the dashboards ---
@app.route("/")
def dashboard():
    return send_from_directory("static", "dashboard.html")


@app.route("/patient/<patient_id>")
def patient_page(patient_id):
    return send_from_directory("static", "patient.html")


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5001, debug=True)