DROP TABLE IF EXISTS users;
DROP TABLE IF EXISTS services;
DROP TABLE IF EXISTS payments;
DROP TABLE IF EXISTS audit_log;

CREATE TABLE users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    full_name TEXT NOT NULL,
    phone TEXT,
    address TEXT NOT NULL,
    area TEXT,
    postal_code TEXT,
    contract_code TEXT,
    created_at TEXT DEFAULT (datetime('now'))
);

CREATE TABLE services (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER NOT NULL,
    label TEXT NOT NULL,
    service_type TEXT CHECK(service_type IN ('recurring', 'one_off')),
    period_days INTEGER DEFAULT 0,
    last_run_date TEXT,
    next_due_date TEXT,
    extra_notes TEXT,
    created_at TEXT DEFAULT (datetime('now')),
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
);

CREATE TABLE payments (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    service_id INTEGER NOT NULL,
    year INTEGER NOT NULL,
    month INTEGER NOT NULL,
    day INTEGER DEFAULT 1,
    amount_due REAL DEFAULT 0,
    amount_paid REAL DEFAULT 0,
    balance REAL DEFAULT 0,
    status TEXT DEFAULT 'pending',      -- pending, paid, overdue, future
    is_future INTEGER DEFAULT 0,
    repeat_days INTEGER DEFAULT 0,
    notes TEXT,
    FOREIGN KEY (service_id) REFERENCES services(id) ON DELETE CASCADE
);

CREATE TABLE audit_log (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    table_name TEXT,
    record_id INTEGER,
    action_type TEXT,                   -- INSERT, UPDATE, DELETE
    old_data TEXT,                      -- JSON
    new_data TEXT,                      -- JSON
    changed_at TEXT DEFAULT (datetime('now'))
);

CREATE INDEX idx_users_name ON users(full_name);
CREATE INDEX idx_services_due ON services(next_due_date);
CREATE INDEX idx_payments_service ON payments(service_id);
CREATE INDEX idx_payments_date ON payments(year, month, day);
