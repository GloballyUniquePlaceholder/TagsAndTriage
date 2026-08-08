from flask import Flask, request, jsonify, send_from_directory
import sqlite3
from datetime import datetime

app = Flask(__name__)
DB = "triage.db"

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
            minutes_overdue = None  # never given yet - always overdue
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

# --- 1. Check-in: called when a patient is triaged ---
@app.route("/api/checkin", methods=["POST"])
def checkin():
    data = request.json
    priority = calculate_priority(
        data["can_walk"], data["initial_breathing"],
        data.get("breathing_after_reposition"), data.get("breathing_rate"),
        data["pulse_present"], data["responsive"]
    )
    conn = get_db()
    conn.execute(
        "INSERT OR IGNORE INTO patients (patient_id, created_at) VALUES (?, datetime('now'))",
        (data["patient_id"],)
    )
    conn.execute("""
        INSERT INTO checks (patient_id, can_walk, initial_breathing,
            breathing_after_reposition, breathing_rate, pulse_present,
            responsive, notes, priority_result, timestamp)
        VALUES (?,?,?,?,?,?,?,?,?,datetime('now'))
    """, (data["patient_id"], data["can_walk"], data["initial_breathing"],
          data.get("breathing_after_reposition"), data.get("breathing_rate"),
          data["pulse_present"], data["responsive"], data.get("notes"), priority))
    conn.commit()
    conn.close()
    return jsonify({"priority": priority})

# --- 2. Schedule a treatment for a patient ---
@app.route("/api/treatments", methods=["POST"])
def add_treatment():
    data = request.json
    conn = get_db()
    conn.execute("""
        INSERT INTO treatments (patient_id, treatment_type, interval_minutes, last_given)
        VALUES (?, ?, ?, NULL)
    """, (data["patient_id"], data["treatment_type"], data["interval_minutes"]))
    conn.commit()
    conn.close()
    return jsonify({"status": "ok"})

# --- 3. Mark a treatment as given (resets the overdue clock) ---
@app.route("/api/treatments/given", methods=["POST"])
def mark_treatment_given():
    data = request.json
    conn = get_db()
    conn.execute(
        "UPDATE treatments SET last_given = datetime('now') WHERE treatment_id = ?",
        (data["treatment_id"],)
    )
    conn.commit()
    conn.close()
    return jsonify({"status": "ok"})

# --- 4. Get all patients, with latest priority/notes + overdue treatment info ---
@app.route("/api/patients", methods=["GET"])
def get_patients():
    conn = get_db()
    rows = conn.execute("""
        SELECT p.patient_id,
               c.priority_result AS priority,
               c.timestamp AS last_check,
               c.notes AS notes
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
        result.append(patient)
    return jsonify(result)

# --- 5. Serve the dashboard ---
@app.route("/")
def dashboard():
    return send_from_directory("static", "dashboard.html")

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5001, debug=True)