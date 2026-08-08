CREATE TABLE patients (
    patient_id TEXT PRIMARY KEY,
    zone TEXT,
    checked_out_at TEXT,
    checkout_reason TEXT,
    created_at TEXT
);

CREATE TABLE checks (
    check_id INTEGER PRIMARY KEY AUTOINCREMENT,
    patient_id TEXT,
    can_walk INTEGER,
    initial_breathing INTEGER,
    breathing_after_reposition INTEGER,
    breathing_rate INTEGER,
    pulse_present INTEGER,
    responsive INTEGER,
    notes TEXT,
    priority_result TEXT,
    timestamp TEXT,
    FOREIGN KEY (patient_id) REFERENCES patients(patient_id)
);

CREATE TABLE treatments (
    treatment_id INTEGER PRIMARY KEY AUTOINCREMENT,
    patient_id TEXT,
    treatment_type TEXT,
    interval_minutes INTEGER,
    last_given TEXT,
    FOREIGN KEY (patient_id) REFERENCES patients(patient_id)
);