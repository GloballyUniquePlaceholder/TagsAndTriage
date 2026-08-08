CREATE TABLE patients (
    patient_id TEXT PRIMARY KEY,
    zone_position TEXT,       -- current location, e.g. "Red-7" — set only via
                               -- /api/assign-position, AFTER a priority exists.
                               -- Never set during check-in/recheck.
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
    next_check_minutes INTEGER,   -- staff override for reassessment interval
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

-- ============================================================
-- MIGRATION for an existing triage.db (run these, don't recreate):
--
--   ALTER TABLE patients ADD COLUMN zone_position TEXT;
--
-- The old `checks.zone_position` column (if you added it previously) is no
-- longer written to — SQLite can't drop columns easily, so it's safe to just
-- leave it in place, unused.
-- ============================================================