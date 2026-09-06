#include "SqliteConn.h"
#include <iostream>
#include <sstream>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <unordered_map>

// Helper to convert struct to JSON string manually for audit
static std::string userToJson(const User& u) {
    return "{ \"id\": " + std::to_string(u.id) + ", \"full_name\": \"" + u.full_name + "\", \"phone\": \"" + u.phone + "\", \"address\": \"" + u.address + "\" }";
}

static std::string serviceToJson(const Service& s) {
    return "{ \"id\": " + std::to_string(s.id) + ", \"label\": \"" + s.label + "\" }";
}

// Direction is a 2-value attribute (in/out) after the client's revert away from
// "comment" as a pseudo-direction (comment/notes is now just the always-visible
// `notes` text field, not a payment type). Any stray 'comment' row left over from
// before that revert (local seed/test data, not production) is read defensively
// as 'in' rather than migrated in the DB, so it doesn't silently vanish from sums.
static std::string NormalizeDirection(const char* raw) {
    std::string d = raw ? raw : "in";
    if (d == "comment") return "in";
    return d;
}

static std::string paymentToJson(const Payment& p) {
    return "{ \"id\": " + std::to_string(p.id) + ", \"service_id\": " + std::to_string(p.service_id) + ", \"user_id\": " + std::to_string(p.user_id) + ", \"year\": " + std::to_string(p.year) + ", \"month\": " + std::to_string(p.month) + ", \"amount_due\": " + std::to_string(p.amount_due) + ", \"amount_paid\": " + std::to_string(p.amount_paid) + ", \"status\": \"" + p.status + "\", \"direction\": \"" + p.direction + "\", \"resolution_date\": \"" + p.resolution_date + "\", \"resolved_at\": \"" + p.resolved_at + "\" }";
}

SqliteConn::SqliteConn(const std::string& dbPath) {
    int rc = sqlite3_open(dbPath.c_str(), &db);
    if (rc) {
        std::cerr << "Can't open database: " << sqlite3_errmsg(db) << std::endl;
        return;
    }

    // Create tables if not exist
    std::string schema = R"(
        CREATE TABLE IF NOT EXISTS users (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            full_name TEXT NOT NULL,
            phone TEXT,
            address TEXT NOT NULL,
            area TEXT,
            postal_code TEXT,
            contract_code TEXT,
            created_at TEXT DEFAULT (datetime('now'))
        );
        CREATE TABLE IF NOT EXISTS services (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            label TEXT NOT NULL,
            extra_notes TEXT,
            created_at TEXT DEFAULT (datetime('now'))
        );
        CREATE TABLE IF NOT EXISTS service_users (
            service_id INTEGER NOT NULL,
            user_id INTEGER NOT NULL,
            PRIMARY KEY (service_id, user_id),
            FOREIGN KEY (service_id) REFERENCES services(id) ON DELETE CASCADE,
            FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
        );
        CREATE TABLE IF NOT EXISTS master_services (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            label TEXT NOT NULL,
            created_at TEXT DEFAULT (datetime('now'))
        );
        CREATE TABLE IF NOT EXISTS master_service_components (
            master_service_id INTEGER NOT NULL,
            service_id INTEGER NOT NULL,
            PRIMARY KEY (master_service_id, service_id),
            FOREIGN KEY (master_service_id) REFERENCES master_services(id) ON DELETE CASCADE,
            FOREIGN KEY (service_id) REFERENCES services(id) ON DELETE CASCADE
        );
        CREATE TABLE IF NOT EXISTS payments (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            service_id INTEGER NOT NULL,
            year INTEGER NOT NULL,
            month INTEGER NOT NULL,
            day INTEGER DEFAULT 1,
            amount_due REAL DEFAULT 0,
            amount_paid REAL DEFAULT 0,
            balance REAL DEFAULT 0,
            status TEXT DEFAULT 'pending',
            notes TEXT,
            user_id INTEGER,
            direction TEXT DEFAULT 'in',
            FOREIGN KEY (service_id) REFERENCES services(id) ON DELETE CASCADE,
            FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
        );
        CREATE TABLE IF NOT EXISTS notes (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            title TEXT,
            content TEXT,
            created_at TEXT DEFAULT (datetime('now'))
        );
        CREATE TABLE IF NOT EXISTS audit_log (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            table_name TEXT,
            record_id INTEGER,
            action_type TEXT,
            old_data TEXT,
            new_data TEXT,
            changed_at TEXT DEFAULT (datetime('now'))
        );
        CREATE INDEX IF NOT EXISTS idx_users_name ON users(full_name);
        CREATE INDEX IF NOT EXISTS idx_payments_service ON payments(service_id);
        CREATE INDEX IF NOT EXISTS idx_payments_date ON payments(year, month, day);
        CREATE INDEX IF NOT EXISTS idx_service_users_user ON service_users(user_id);
        CREATE INDEX IF NOT EXISTS idx_payments_user ON payments(user_id);
    )";
    executeQuery(schema);

    // --- Migration for DBs created before service_users / payments.user_id existed ---
    // (services used to carry a single "user_id" FK; payments had no user_id at all)
    {
        bool servicesHasUserId = false;
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db, "PRAGMA table_info(services)", -1, &stmt, nullptr);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string colName = (const char*)sqlite3_column_text(stmt, 1);
            if (colName == "user_id") { servicesHasUserId = true; break; }
        }
        sqlite3_finalize(stmt);
        if (servicesHasUserId) {
            executeQuery("INSERT OR IGNORE INTO service_users (service_id, user_id) "
                         "SELECT id, user_id FROM services WHERE user_id IS NOT NULL;");
            executeQuery("ALTER TABLE services DROP COLUMN user_id;");
        }
    }
    {
        bool paymentsHasUserId = false;
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db, "PRAGMA table_info(payments)", -1, &stmt, nullptr);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string colName = (const char*)sqlite3_column_text(stmt, 1);
            if (colName == "user_id") { paymentsHasUserId = true; break; }
        }
        sqlite3_finalize(stmt);
        if (!paymentsHasUserId) {
            executeQuery("ALTER TABLE payments ADD COLUMN user_id INTEGER REFERENCES users(id);");
            // Best-effort backfill: assign each payment to its service's (then-single) owner.
            executeQuery("UPDATE payments SET user_id = "
                         "(SELECT su.user_id FROM service_users su WHERE su.service_id = payments.service_id LIMIT 1) "
                         "WHERE user_id IS NULL;");
        }
    }

    // --- Migration: payments.direction (in = collect from client, out = manager
    // paid on client's behalf) added after initial ship ---
    {
        bool paymentsHasDirection = false;
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db, "PRAGMA table_info(payments)", -1, &stmt, nullptr);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string colName = (const char*)sqlite3_column_text(stmt, 1);
            if (colName == "direction") { paymentsHasDirection = true; break; }
        }
        sqlite3_finalize(stmt);
        if (!paymentsHasDirection) {
            executeQuery("ALTER TABLE payments ADD COLUMN direction TEXT DEFAULT 'in';");
        }
    }

    // --- Migration: payments.resolution_date / resolved_at (future-payment
    // resolution) added after initial ship ---
    {
        bool hasResolutionDate = false, hasResolvedAt = false;
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db, "PRAGMA table_info(payments)", -1, &stmt, nullptr);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string colName = (const char*)sqlite3_column_text(stmt, 1);
            if (colName == "resolution_date") hasResolutionDate = true;
            if (colName == "resolved_at") hasResolvedAt = true;
        }
        sqlite3_finalize(stmt);
        if (!hasResolutionDate) {
            executeQuery("ALTER TABLE payments ADD COLUMN resolution_date TEXT;");
        }
        if (!hasResolvedAt) {
            executeQuery("ALTER TABLE payments ADD COLUMN resolved_at TEXT;");
        }
    }

    // --- Migration: drop payments.repeat_days (confirmed dead code, never read
    // anywhere) and payments.is_future (superseded by status='future' -- the
    // simplified status model already treats "future" as the single source of
    // truth; is_future was a second, redundant flag that had to be kept in sync
    // by hand on every write path). Same PRAGMA table_info(...) + DROP COLUMN
    // pattern as the services.user_id migration above -- the vendored SQLite
    // supports DROP COLUMN directly, so no rebuild-the-table dance is needed. ---
    {
        bool hasRepeatDays = false, hasIsFuture = false;
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db, "PRAGMA table_info(payments)", -1, &stmt, nullptr);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string colName = (const char*)sqlite3_column_text(stmt, 1);
            if (colName == "repeat_days") hasRepeatDays = true;
            if (colName == "is_future") hasIsFuture = true;
        }
        sqlite3_finalize(stmt);
        if (hasRepeatDays) {
            executeQuery("ALTER TABLE payments DROP COLUMN repeat_days;");
        }
        if (hasIsFuture) {
            executeQuery("ALTER TABLE payments DROP COLUMN is_future;");
        }
    }

    // --- Migration: users.special_code (client's own legacy code, second key,
    // no enforced uniqueness) added after initial ship ---
    {
        bool usersHasSpecialCode = false;
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db, "PRAGMA table_info(users)", -1, &stmt, nullptr);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string colName = (const char*)sqlite3_column_text(stmt, 1);
            if (colName == "special_code") { usersHasSpecialCode = true; break; }
        }
        sqlite3_finalize(stmt);
        if (!usersHasSpecialCode) {
            executeQuery("ALTER TABLE users ADD COLUMN special_code TEXT;");
        }
    }

    // --- Migration: audit_log.category (business vs db) added after initial ship ---
    {
        bool auditHasCategory = false;
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db, "PRAGMA table_info(audit_log)", -1, &stmt, nullptr);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string colName = (const char*)sqlite3_column_text(stmt, 1);
            if (colName == "category") { auditHasCategory = true; break; }
        }
        sqlite3_finalize(stmt);
        if (!auditHasCategory) {
            executeQuery("ALTER TABLE audit_log ADD COLUMN category TEXT DEFAULT 'db';");
        }
    }

    // --- Migration: drop the abandoned auto-scheduling / recurrence concept.
    // Client's decision: the app logs/registers payments (including scheduling
    // ahead as future payments, which already exists) -- it is not the app's
    // job to automatically re-run anything periodical, that's on the user.
    // services.service_type / period_days / last_run_date / next_due_date were
    // all tied to that abandoned concept. Same PRAGMA table_info(...) + DROP
    // COLUMN pattern as the services.user_id migration above. ---
    {
        bool hasServiceType = false, hasPeriodDays = false, hasLastRunDate = false, hasNextDueDate = false;
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db, "PRAGMA table_info(services)", -1, &stmt, nullptr);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string colName = (const char*)sqlite3_column_text(stmt, 1);
            if (colName == "service_type") hasServiceType = true;
            if (colName == "period_days") hasPeriodDays = true;
            if (colName == "last_run_date") hasLastRunDate = true;
            if (colName == "next_due_date") hasNextDueDate = true;
        }
        sqlite3_finalize(stmt);
        if (hasServiceType) executeQuery("ALTER TABLE services DROP COLUMN service_type;");
        if (hasPeriodDays) executeQuery("ALTER TABLE services DROP COLUMN period_days;");
        if (hasLastRunDate) executeQuery("ALTER TABLE services DROP COLUMN last_run_date;");
        if (hasNextDueDate) executeQuery("ALTER TABLE services DROP COLUMN next_due_date;");
    }

    // --- Undo: the "service sub-items" feature (service_items table +
    // payments.service_item_id) from a previous session was rejected by the
    // client in favor of the master_services/master_service_components design
    // below -- a clean removal of an unreleased feature, not data-preserving. ---
    {
        bool paymentsHasServiceItemId = false;
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db, "PRAGMA table_info(payments)", -1, &stmt, nullptr);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string colName = (const char*)sqlite3_column_text(stmt, 1);
            if (colName == "service_item_id") { paymentsHasServiceItemId = true; break; }
        }
        sqlite3_finalize(stmt);
        if (paymentsHasServiceItemId) {
            executeQuery("ALTER TABLE payments DROP COLUMN service_item_id;");
        }
        executeQuery("DROP TABLE IF EXISTS service_items;");
    }

    // Greek Headers Map
    greekHeaders["users"] = {"ΚΩΔ", "ΟΝΟΜΑ", "ΤΗΛ", "ΔΙΕΥΘΥΝΣΗ", "ΠΕΡΙΟΧΗ", "ΤΚ", "ΣΥΜΒ", "ΗΜ/ΝΙΑ ΔΗΜ.", "ΕΙΔ. ΚΩΔ."};
    greekHeaders["services"] = {"ΚΩΔ ΥΠΗΡ.", "ΠΕΡΙΓΡΑΦΗ", "ΣΗΜΕΙΩΣΕΙΣ", "ΗΜ/ΝΙΑ ΔΗΜ."};
    greekHeaders["service_users"] = {"ΚΩΔ ΥΠΗΡ.", "ΚΩΔ ΧΡΗΣΤΗ"};
    greekHeaders["payments"] = {"ΚΩΔ ΠΛΗΡ.", "ΚΩΔ ΥΠΗΡ.", "ΕΤΟΣ", "ΜΗΝΑΣ", "ΗΜΕΡΑ", "ΠΟΣΟ ΟΦΕΙΛ.", "ΠΟΣΟ ΠΛΗΡ.", "ΥΠΟΛΟΙΠΟ", "ΚΑΤΑΣΤΑΣΗ", "ΣΧΟΛΙΑ", "ΚΩΔ ΧΡΗΣΤΗ", "ΤΥΠΟΣ", "ΗΜ/ΝΙΑ ΕΠΙΛΥΣΗΣ", "ΕΠΙΛΥΘΗΚΕ ΣΤΙΣ"};
    greekHeaders["master_services"] = {"ΚΩΔ", "ΕΤΙΚΕΤΑ", "ΗΜ/ΝΙΑ ΔΗΜ."};
    greekHeaders["master_service_components"] = {"ΚΩΔ ΠΑΚΕΤΟΥ", "ΚΩΔ ΥΠΗΡ."};
    greekHeaders["notes"] = {"ΚΩΔ", "ΤΙΤΛΟΣ", "ΠΕΡΙΕΧΟΜΕΝΟ", "ΗΜ/ΝΙΑ ΔΗΜ."};
}

SqliteConn::~SqliteConn() { if(db) sqlite3_close(db); }

bool SqliteConn::executeQuery(const std::string& sql) {
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errMsg);
    if(rc != SQLITE_OK) { std::cerr << "SQL Error: " << errMsg << std::endl; sqlite3_free(errMsg); return false; }
    return true;
}

void SqliteConn::logAction(const std::string& table, int recordId, const std::string& action, const std::string& oldData, const std::string& newData, const std::string& category) {
    std::string sql = "INSERT INTO audit_log (table_name, record_id, action_type, old_data, new_data, category) VALUES (?, ?, ?, ?, ?, ?)";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, table.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, recordId);
    sqlite3_bind_text(stmt, 3, action.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, oldData.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, newData.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, category.c_str(), -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

// --- USERS ---
std::vector<User> SqliteConn::searchUsers(const std::string& query, int offset, int limit) {
    std::vector<User> results;
    std::string sql = "SELECT id, full_name, phone, address, area, postal_code, contract_code, created_at, special_code FROM users WHERE full_name LIKE '%' || ? || '%' OR address LIKE '%' || ? || '%' OR phone LIKE '%' || ? || '%' OR area LIKE '%' || ? || '%' OR postal_code LIKE '%' || ? || '%' OR special_code LIKE '%' || ? || '%' LIMIT ? OFFSET ?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, query.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, query.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, query.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, query.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, query.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, query.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 7, limit);
    sqlite3_bind_int(stmt, 8, offset);
    while(sqlite3_step(stmt) == SQLITE_ROW) {
        User u; u.id = sqlite3_column_int(stmt,0); u.full_name = (const char*)sqlite3_column_text(stmt,1); u.phone = (const char*)sqlite3_column_text(stmt,2); u.address = (const char*)sqlite3_column_text(stmt,3); u.area = (const char*)sqlite3_column_text(stmt,4); u.postal_code = (const char*)sqlite3_column_text(stmt,5) ? (const char*)sqlite3_column_text(stmt,5) : ""; u.contract_code = (const char*)sqlite3_column_text(stmt,6) ? (const char*)sqlite3_column_text(stmt,6) : ""; u.created_at = (const char*)sqlite3_column_text(stmt,7) ? (const char*)sqlite3_column_text(stmt,7) : ""; u.special_code = sqlite3_column_text(stmt,8) ? (const char*)sqlite3_column_text(stmt,8) : ""; results.push_back(u);
    }
    sqlite3_finalize(stmt);
    return results;
}

// Advanced multi-field search: only non-empty criteria fields are AND-ed in, each as a
// case-insensitive LIKE (LOWER() on both sides, matching how the rest of the app treats
// Greek text -- SQLite's built-in LIKE case-folding is ASCII-only). Parameterized: every
// user-supplied value is bound via '?', never concatenated into the SQL text.
std::vector<User> SqliteConn::advancedSearchUsers(const User& criteria, const std::string& idQuery) {
    std::vector<User> results;
    std::string sql = "SELECT id, full_name, phone, address, area, postal_code, contract_code, created_at, special_code FROM users WHERE 1=1";
    std::vector<std::string> binds;
    auto addClause = [&](const char* col, const std::string& val) {
        if (val.empty()) return;
        sql += std::string(" AND LOWER(") + col + ") LIKE '%' || LOWER(?) || '%'";
        binds.push_back(val);
    };
    addClause("CAST(id AS TEXT)", idQuery);
    addClause("full_name", criteria.full_name);
    addClause("phone", criteria.phone);
    addClause("address", criteria.address);
    addClause("area", criteria.area);
    addClause("postal_code", criteria.postal_code);
    addClause("contract_code", criteria.contract_code);
    addClause("special_code", criteria.special_code);
    sql += " ORDER BY full_name LIMIT 2000";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    for (size_t i = 0; i < binds.size(); i++) sqlite3_bind_text(stmt, (int)i + 1, binds[i].c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        User u;
        u.id = sqlite3_column_int(stmt,0);
        u.full_name = (const char*)sqlite3_column_text(stmt,1);
        u.phone = sqlite3_column_text(stmt,2) ? (const char*)sqlite3_column_text(stmt,2) : "";
        u.address = sqlite3_column_text(stmt,3) ? (const char*)sqlite3_column_text(stmt,3) : "";
        u.area = sqlite3_column_text(stmt,4) ? (const char*)sqlite3_column_text(stmt,4) : "";
        u.postal_code = sqlite3_column_text(stmt,5) ? (const char*)sqlite3_column_text(stmt,5) : "";
        u.contract_code = sqlite3_column_text(stmt,6) ? (const char*)sqlite3_column_text(stmt,6) : "";
        u.created_at = sqlite3_column_text(stmt,7) ? (const char*)sqlite3_column_text(stmt,7) : "";
        u.special_code = sqlite3_column_text(stmt,8) ? (const char*)sqlite3_column_text(stmt,8) : "";
        results.push_back(u);
    }
    sqlite3_finalize(stmt);
    return results;
}

bool SqliteConn::addUser(const User& u) {
    std::string sql = "INSERT INTO users (full_name, phone, address, area, postal_code, contract_code, special_code) VALUES (?,?,?,?,?,?,?)";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_text(stmt,1,u.full_name.c_str(),-1,SQLITE_STATIC);
    sqlite3_bind_text(stmt,2,u.phone.c_str(),-1,SQLITE_STATIC);
    sqlite3_bind_text(stmt,3,u.address.c_str(),-1,SQLITE_STATIC);
    sqlite3_bind_text(stmt,4,u.area.c_str(),-1,SQLITE_STATIC);
    sqlite3_bind_text(stmt,5,u.postal_code.c_str(),-1,SQLITE_STATIC);
    sqlite3_bind_text(stmt,6,u.contract_code.c_str(),-1,SQLITE_STATIC);
    sqlite3_bind_text(stmt,7,u.special_code.c_str(),-1,SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if(rc == SQLITE_DONE) {
        int newId = sqlite3_last_insert_rowid(db);
        logAction("users", newId, "INSERT", "", userToJson(u), "business");
        return true;
    }
    return false;
}

bool SqliteConn::editUser(int id, const User& u) {
    User old = getUserById(id);
    std::string sql = "UPDATE users SET full_name=?, phone=?, address=?, area=?, postal_code=?, contract_code=?, special_code=? WHERE id=?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_text(stmt,1,u.full_name.c_str(),-1,SQLITE_STATIC);
    sqlite3_bind_text(stmt,2,u.phone.c_str(),-1,SQLITE_STATIC);
    sqlite3_bind_text(stmt,3,u.address.c_str(),-1,SQLITE_STATIC);
    sqlite3_bind_text(stmt,4,u.area.c_str(),-1,SQLITE_STATIC);
    sqlite3_bind_text(stmt,5,u.postal_code.c_str(),-1,SQLITE_STATIC);
    sqlite3_bind_text(stmt,6,u.contract_code.c_str(),-1,SQLITE_STATIC);
    sqlite3_bind_text(stmt,7,u.special_code.c_str(),-1,SQLITE_STATIC);
    sqlite3_bind_int(stmt,8,id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if(rc == SQLITE_DONE) { logAction("users", id, "UPDATE", userToJson(old), userToJson(u), "business"); return true; }
    return false;
}

bool SqliteConn::removeUser(int id) {
    // Cascade warning is handled in UI, we just delete.
    std::string sql = "DELETE FROM users WHERE id=?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_int(stmt,1,id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if(rc == SQLITE_DONE) { logAction("users", id, "DELETE", "", "", "business"); return true; }
    return false;
}

User SqliteConn::getUserById(int id) {
    User u; std::string sql = "SELECT * FROM users WHERE id=?";
    sqlite3_stmt* stmt; sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr); sqlite3_bind_int(stmt,1,id);
    if(sqlite3_step(stmt) == SQLITE_ROW) { u.id=sqlite3_column_int(stmt,0); u.full_name=(const char*)sqlite3_column_text(stmt,1); u.phone=(const char*)sqlite3_column_text(stmt,2); u.address=(const char*)sqlite3_column_text(stmt,3); u.area=(const char*)sqlite3_column_text(stmt,4); u.postal_code=(const char*)sqlite3_column_text(stmt,5)?(const char*)sqlite3_column_text(stmt,5):""; u.contract_code=(const char*)sqlite3_column_text(stmt,6)?(const char*)sqlite3_column_text(stmt,6):""; u.created_at=(const char*)sqlite3_column_text(stmt,7)?(const char*)sqlite3_column_text(stmt,7):""; u.special_code=sqlite3_column_text(stmt,8)?(const char*)sqlite3_column_text(stmt,8):""; }
    sqlite3_finalize(stmt); return u;
}

// --- SERVICES ---
// NOTE: The source transcript explicitly deferred these implementations
// ("similar patterns, omitted for brevity") and never provided them in
// a "final dump" later in the chat. These are STUB implementations
// (minimal but functional pass-through queries) added to make the
// project compile and link; they were not present in the transcript.
std::vector<Service> SqliteConn::searchServices(const std::string& query, int offset, int limit) {
    std::vector<Service> results;
    std::string sql = "SELECT id, label, extra_notes, created_at FROM services WHERE label LIKE '%' || ? || '%' OR extra_notes LIKE '%' || ? || '%' LIMIT ? OFFSET ?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_text(stmt,1,query.c_str(),-1,SQLITE_STATIC);
    sqlite3_bind_text(stmt,2,query.c_str(),-1,SQLITE_STATIC);
    sqlite3_bind_int(stmt,3,limit);
    sqlite3_bind_int(stmt,4,offset);
    while(sqlite3_step(stmt) == SQLITE_ROW) {
        Service s;
        s.id = sqlite3_column_int(stmt,0);
        s.label = (const char*)sqlite3_column_text(stmt,1);
        s.extra_notes = sqlite3_column_text(stmt,2) ? (const char*)sqlite3_column_text(stmt,2) : "";
        s.created_at = sqlite3_column_text(stmt,3) ? (const char*)sqlite3_column_text(stmt,3) : "";
        results.push_back(s);
    }
    sqlite3_finalize(stmt);
    return results;
}

std::vector<Service> SqliteConn::advancedSearchServices(const std::string& label, const std::string& extra_notes,
                                                          const std::string& idQuery) {
    std::vector<Service> results;
    std::string sql = "SELECT id, label, extra_notes, created_at FROM services WHERE 1=1";
    std::vector<std::string> binds;
    auto addClause = [&](const char* colExpr, const std::string& val) {
        if (val.empty()) return;
        sql += std::string(" AND LOWER(") + colExpr + ") LIKE '%' || LOWER(?) || '%'";
        binds.push_back(val);
    };
    addClause("CAST(id AS TEXT)", idQuery);
    addClause("label", label);
    addClause("extra_notes", extra_notes);
    sql += " ORDER BY label LIMIT 2000";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    for (size_t i = 0; i < binds.size(); i++) sqlite3_bind_text(stmt, (int)i + 1, binds[i].c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Service s;
        s.id = sqlite3_column_int(stmt,0);
        s.label = (const char*)sqlite3_column_text(stmt,1);
        s.extra_notes = sqlite3_column_text(stmt,2) ? (const char*)sqlite3_column_text(stmt,2) : "";
        s.created_at = sqlite3_column_text(stmt,3) ? (const char*)sqlite3_column_text(stmt,3) : "";
        results.push_back(s);
    }
    sqlite3_finalize(stmt);
    return results;
}

bool SqliteConn::addService(int userId, const Service& s) {
    return addServiceGetId(userId, s) >= 0;
}

int SqliteConn::addServiceGetId(int userId, const Service& s) {
    std::string sql = "INSERT INTO services (label, extra_notes) VALUES (?,?)";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_text(stmt,1,s.label.c_str(),-1,SQLITE_STATIC);
    sqlite3_bind_text(stmt,2,s.extra_notes.c_str(),-1,SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if(rc != SQLITE_DONE) return -1;
    int id = sqlite3_last_insert_rowid(db);
    logAction("services", id, "INSERT", "", serviceToJson(s), "business");
    if (!addServiceUser(id, userId)) return -1;
    return id;
}

bool SqliteConn::addServiceUser(int serviceId, int userId) {
    std::string sql = "INSERT OR IGNORE INTO service_users (service_id, user_id) VALUES (?,?)";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_int(stmt,1,serviceId);
    sqlite3_bind_int(stmt,2,userId);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if(rc == SQLITE_DONE) { logAction("service_users", serviceId, "INSERT", "", "{ \"user_id\": " + std::to_string(userId) + " }", "business"); return true; }
    return false;
}

std::vector<User> SqliteConn::getUsersForService(int serviceId) {
    std::vector<User> results;
    std::string sql = "SELECT u.* FROM users u JOIN service_users su ON su.user_id = u.id WHERE su.service_id=? ORDER BY u.full_name";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_int(stmt,1,serviceId);
    while(sqlite3_step(stmt) == SQLITE_ROW) {
        User u;
        u.id = sqlite3_column_int(stmt,0);
        u.full_name = sqlite3_column_text(stmt,1) ? (const char*)sqlite3_column_text(stmt,1) : "";
        u.phone = sqlite3_column_text(stmt,2) ? (const char*)sqlite3_column_text(stmt,2) : "";
        u.address = sqlite3_column_text(stmt,3) ? (const char*)sqlite3_column_text(stmt,3) : "";
        u.area = sqlite3_column_text(stmt,4) ? (const char*)sqlite3_column_text(stmt,4) : "";
        u.postal_code = sqlite3_column_text(stmt,5) ? (const char*)sqlite3_column_text(stmt,5) : "";
        u.contract_code = sqlite3_column_text(stmt,6) ? (const char*)sqlite3_column_text(stmt,6) : "";
        u.created_at = sqlite3_column_text(stmt,7) ? (const char*)sqlite3_column_text(stmt,7) : "";
        u.special_code = sqlite3_column_text(stmt,8) ? (const char*)sqlite3_column_text(stmt,8) : "";
        results.push_back(u);
    }
    sqlite3_finalize(stmt);
    return results;
}

bool SqliteConn::editService(int id, const Service& s) {
    Service old = getServiceById(id);
    std::string sql = "UPDATE services SET label=?, extra_notes=? WHERE id=?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_text(stmt,1,s.label.c_str(),-1,SQLITE_STATIC);
    sqlite3_bind_text(stmt,2,s.extra_notes.c_str(),-1,SQLITE_STATIC);
    sqlite3_bind_int(stmt,3,id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if(rc == SQLITE_DONE) { logAction("services", id, "UPDATE", serviceToJson(old), serviceToJson(s), "business"); return true; }
    return false;
}

bool SqliteConn::removeService(int id) {
    std::string sql = "DELETE FROM services WHERE id=?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_int(stmt,1,id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if(rc == SQLITE_DONE) { logAction("services", id, "DELETE", "", "", "business"); return true; }
    return false;
}

Service SqliteConn::getServiceById(int id) {
    Service s; std::string sql = "SELECT * FROM services WHERE id=?";
    sqlite3_stmt* stmt; sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr); sqlite3_bind_int(stmt,1,id);
    if(sqlite3_step(stmt) == SQLITE_ROW) {
        s.id = sqlite3_column_int(stmt,0);
        s.label = (const char*)sqlite3_column_text(stmt,1);
        s.extra_notes = sqlite3_column_text(stmt,2) ? (const char*)sqlite3_column_text(stmt,2) : "";
        s.created_at = sqlite3_column_text(stmt,3) ? (const char*)sqlite3_column_text(stmt,3) : "";
    }
    sqlite3_finalize(stmt);
    return s;
}

std::vector<Service> SqliteConn::getServicesForUser(int userId) {
    std::vector<Service> results;
    std::string sql = "SELECT s.* FROM services s JOIN service_users su ON su.service_id = s.id WHERE su.user_id=?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_int(stmt,1,userId);
    while(sqlite3_step(stmt) == SQLITE_ROW) {
        Service s;
        s.id = sqlite3_column_int(stmt,0);
        s.label = (const char*)sqlite3_column_text(stmt,1);
        s.extra_notes = sqlite3_column_text(stmt,2) ? (const char*)sqlite3_column_text(stmt,2) : "";
        s.created_at = sqlite3_column_text(stmt,3) ? (const char*)sqlite3_column_text(stmt,3) : "";
        results.push_back(s);
    }
    sqlite3_finalize(stmt);
    return results;
}

// --- MASTER SERVICES (named packages grouping several real, independent
// services -- an organizational/viewing grouping only; payments still attach
// to one real component service + one user exactly as before, unchanged). ---
std::vector<MasterService> SqliteConn::getMasterServices() {
    std::vector<MasterService> results;
    std::string sql = "SELECT id, label FROM master_services ORDER BY label";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        MasterService ms;
        ms.id = sqlite3_column_int(stmt,0);
        ms.label = sqlite3_column_text(stmt,1) ? (const char*)sqlite3_column_text(stmt,1) : "";
        results.push_back(ms);
    }
    sqlite3_finalize(stmt);
    return results;
}

std::vector<Service> SqliteConn::getMasterServiceComponents(int masterServiceId) {
    std::vector<Service> results;
    std::string sql = "SELECT s.id, s.label, s.extra_notes, s.created_at FROM services s "
                       "JOIN master_service_components msc ON msc.service_id = s.id "
                       "WHERE msc.master_service_id=? ORDER BY s.label";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_int(stmt,1,masterServiceId);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Service s;
        s.id = sqlite3_column_int(stmt,0);
        s.label = sqlite3_column_text(stmt,1) ? (const char*)sqlite3_column_text(stmt,1) : "";
        s.extra_notes = sqlite3_column_text(stmt,2) ? (const char*)sqlite3_column_text(stmt,2) : "";
        s.created_at = sqlite3_column_text(stmt,3) ? (const char*)sqlite3_column_text(stmt,3) : "";
        results.push_back(s);
    }
    sqlite3_finalize(stmt);
    return results;
}

int SqliteConn::addMasterService(const std::string& label) {
    std::string sql = "INSERT INTO master_services (label) VALUES (?)";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_text(stmt,1,label.c_str(),-1,SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    int id = sqlite3_last_insert_rowid(db);
    logAction("master_services", id, "INSERT", "", "{ \"label\": \"" + label + "\" }", "business");
    return id;
}

bool SqliteConn::addMasterServiceComponent(int masterServiceId, int serviceId) {
    std::string sql = "INSERT OR IGNORE INTO master_service_components (master_service_id, service_id) VALUES (?,?)";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_int(stmt,1,masterServiceId);
    sqlite3_bind_int(stmt,2,serviceId);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) {
        logAction("master_service_components", masterServiceId, "INSERT", "", "{ \"service_id\": " + std::to_string(serviceId) + " }", "business");
        return true;
    }
    return false;
}

bool SqliteConn::removeMasterServiceComponent(int masterServiceId, int serviceId) {
    std::string sql = "DELETE FROM master_service_components WHERE master_service_id=? AND service_id=?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_int(stmt,1,masterServiceId);
    sqlite3_bind_int(stmt,2,serviceId);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) {
        logAction("master_service_components", masterServiceId, "DELETE", "", "{ \"service_id\": " + std::to_string(serviceId) + " }", "business");
        return true;
    }
    return false;
}

bool SqliteConn::removeMasterService(int id) {
    std::string sql = "DELETE FROM master_services WHERE id=?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_int(stmt,1,id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) { logAction("master_services", id, "DELETE", "", "", "business"); return true; }
    return false;
}

std::vector<DataConn::MasterServiceSummary> SqliteConn::getMasterServiceSummaries() {
    std::vector<DataConn::MasterServiceSummary> results;
    for (auto& ms : getMasterServices()) {
        DataConn::MasterServiceSummary sum;
        sum.masterService = ms;
        sum.components = getMasterServiceComponents(ms.id);
        for (auto& comp : sum.components) {
            sum.componentRosters.push_back(getUsersForService(comp.id));
        }
        results.push_back(sum);
    }
    return results;
}

// --- PAYMENTS (Including Future) ---
bool SqliteConn::addPayment(const Payment& p) {
    std::string sql = "INSERT INTO payments (service_id, year, month, day, amount_due, amount_paid, balance, status, notes, user_id, direction, resolution_date, resolved_at) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?)";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_int(stmt,1,p.service_id); sqlite3_bind_int(stmt,2,p.year); sqlite3_bind_int(stmt,3,p.month); sqlite3_bind_int(stmt,4,p.day);
    sqlite3_bind_double(stmt,5,p.amount_due); sqlite3_bind_double(stmt,6,p.amount_paid); sqlite3_bind_double(stmt,7,p.balance);
    sqlite3_bind_text(stmt,8,p.status.c_str(),-1,SQLITE_STATIC);
    sqlite3_bind_text(stmt,9,p.notes.c_str(),-1,SQLITE_STATIC);
    if (p.user_id > 0) sqlite3_bind_int(stmt,10,p.user_id); else sqlite3_bind_null(stmt,10);
    sqlite3_bind_text(stmt,11,p.direction.empty() ? "in" : p.direction.c_str(),-1,SQLITE_STATIC);
    if (p.resolution_date.empty()) sqlite3_bind_null(stmt,12); else sqlite3_bind_text(stmt,12,p.resolution_date.c_str(),-1,SQLITE_STATIC);
    if (p.resolved_at.empty()) sqlite3_bind_null(stmt,13); else sqlite3_bind_text(stmt,13,p.resolved_at.c_str(),-1,SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if(rc == SQLITE_DONE) { int id = sqlite3_last_insert_rowid(db); logAction("payments", id, "INSERT", "", paymentToJson(p), "business"); return true; }
    return false;
}

// NOTE: STUB (not present in transcript) — added to satisfy DataConn interface.
bool SqliteConn::editPayment(int id, const Payment& p) {
    Payment old = getPaymentById(id);
    std::string sql = "UPDATE payments SET year=?, month=?, day=?, amount_due=?, amount_paid=?, balance=?, status=?, notes=?, user_id=?, direction=?, resolution_date=?, resolved_at=? WHERE id=?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_int(stmt,1,p.year); sqlite3_bind_int(stmt,2,p.month); sqlite3_bind_int(stmt,3,p.day);
    sqlite3_bind_double(stmt,4,p.amount_due); sqlite3_bind_double(stmt,5,p.amount_paid); sqlite3_bind_double(stmt,6,p.balance);
    sqlite3_bind_text(stmt,7,p.status.c_str(),-1,SQLITE_STATIC);
    sqlite3_bind_text(stmt,8,p.notes.c_str(),-1,SQLITE_STATIC);
    if (p.user_id > 0) sqlite3_bind_int(stmt,9,p.user_id); else sqlite3_bind_null(stmt,9);
    sqlite3_bind_text(stmt,10,p.direction.empty() ? "in" : p.direction.c_str(),-1,SQLITE_STATIC);
    if (p.resolution_date.empty()) sqlite3_bind_null(stmt,11); else sqlite3_bind_text(stmt,11,p.resolution_date.c_str(),-1,SQLITE_STATIC);
    if (p.resolved_at.empty()) sqlite3_bind_null(stmt,12); else sqlite3_bind_text(stmt,12,p.resolved_at.c_str(),-1,SQLITE_STATIC);
    sqlite3_bind_int(stmt,13,id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if(rc == SQLITE_DONE) { logAction("payments", id, "UPDATE", paymentToJson(old), paymentToJson(p), "business"); return true; }
    return false;
}

// NOTE: STUB (not present in transcript).
bool SqliteConn::removePayment(int id) {
    std::string sql = "DELETE FROM payments WHERE id=?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_int(stmt,1,id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if(rc == SQLITE_DONE) { logAction("payments", id, "DELETE", "", "", "business"); return true; }
    return false;
}

// NOTE: STUB (not present in transcript).
Payment SqliteConn::getPaymentById(int id) {
    Payment p; std::string sql = "SELECT * FROM payments WHERE id=?";
    sqlite3_stmt* stmt; sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr); sqlite3_bind_int(stmt,1,id);
    if(sqlite3_step(stmt) == SQLITE_ROW) {
        p.id = sqlite3_column_int(stmt,0);
        p.service_id = sqlite3_column_int(stmt,1);
        p.year = sqlite3_column_int(stmt,2);
        p.month = sqlite3_column_int(stmt,3);
        p.day = sqlite3_column_int(stmt,4);
        p.amount_due = sqlite3_column_double(stmt,5);
        p.amount_paid = sqlite3_column_double(stmt,6);
        p.balance = sqlite3_column_double(stmt,7);
        p.status = sqlite3_column_text(stmt,8) ? (const char*)sqlite3_column_text(stmt,8) : "";
        p.notes = sqlite3_column_text(stmt,9) ? (const char*)sqlite3_column_text(stmt,9) : "";
        p.user_id = sqlite3_column_type(stmt,10) == SQLITE_NULL ? -1 : sqlite3_column_int(stmt,10);
        p.direction = NormalizeDirection((const char*)sqlite3_column_text(stmt,11));
        p.resolution_date = sqlite3_column_text(stmt,12) ? (const char*)sqlite3_column_text(stmt,12) : "";
        p.resolved_at = sqlite3_column_text(stmt,13) ? (const char*)sqlite3_column_text(stmt,13) : "";
    }
    sqlite3_finalize(stmt);
    return p;
}

// NOTE: STUB (not present in transcript).
std::vector<Payment> SqliteConn::getPaymentsForService(int serviceId) {
    std::vector<Payment> results;
    std::string sql = "SELECT * FROM payments WHERE service_id=?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_int(stmt,1,serviceId);
    while(sqlite3_step(stmt) == SQLITE_ROW) {
        Payment p;
        p.id = sqlite3_column_int(stmt,0);
        p.service_id = sqlite3_column_int(stmt,1);
        p.year = sqlite3_column_int(stmt,2);
        p.month = sqlite3_column_int(stmt,3);
        p.day = sqlite3_column_int(stmt,4);
        p.amount_due = sqlite3_column_double(stmt,5);
        p.amount_paid = sqlite3_column_double(stmt,6);
        p.balance = sqlite3_column_double(stmt,7);
        p.status = sqlite3_column_text(stmt,8) ? (const char*)sqlite3_column_text(stmt,8) : "";
        p.notes = sqlite3_column_text(stmt,9) ? (const char*)sqlite3_column_text(stmt,9) : "";
        p.user_id = sqlite3_column_type(stmt,10) == SQLITE_NULL ? -1 : sqlite3_column_int(stmt,10);
        p.direction = NormalizeDirection((const char*)sqlite3_column_text(stmt,11));
        p.resolution_date = sqlite3_column_text(stmt,12) ? (const char*)sqlite3_column_text(stmt,12) : "";
        p.resolved_at = sqlite3_column_text(stmt,13) ? (const char*)sqlite3_column_text(stmt,13) : "";
        results.push_back(p);
    }
    sqlite3_finalize(stmt);
    return results;
}

// NOTE: STUB (not present in transcript).
std::vector<Payment> SqliteConn::getPaymentsForUser(int userId) {
    std::vector<Payment> results;
    // payments.user_id records which specific owner this payment is for, so this is
    // correct even for a shared service (unlike summing all payments on that service).
    std::string sql = "SELECT * FROM payments WHERE user_id=?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_int(stmt,1,userId);
    while(sqlite3_step(stmt) == SQLITE_ROW) {
        Payment p;
        p.id = sqlite3_column_int(stmt,0);
        p.service_id = sqlite3_column_int(stmt,1);
        p.year = sqlite3_column_int(stmt,2);
        p.month = sqlite3_column_int(stmt,3);
        p.day = sqlite3_column_int(stmt,4);
        p.amount_due = sqlite3_column_double(stmt,5);
        p.amount_paid = sqlite3_column_double(stmt,6);
        p.balance = sqlite3_column_double(stmt,7);
        p.status = sqlite3_column_text(stmt,8) ? (const char*)sqlite3_column_text(stmt,8) : "";
        p.notes = sqlite3_column_text(stmt,9) ? (const char*)sqlite3_column_text(stmt,9) : "";
        p.user_id = sqlite3_column_type(stmt,10) == SQLITE_NULL ? -1 : sqlite3_column_int(stmt,10);
        p.direction = NormalizeDirection((const char*)sqlite3_column_text(stmt,11));
        p.resolution_date = sqlite3_column_text(stmt,12) ? (const char*)sqlite3_column_text(stmt,12) : "";
        p.resolved_at = sqlite3_column_text(stmt,13) ? (const char*)sqlite3_column_text(stmt,13) : "";
        results.push_back(p);
    }
    sqlite3_finalize(stmt);
    return results;
}

std::vector<Payment> SqliteConn::getRecentPayments(int limit) {
    std::vector<Payment> results;
    // Non-future payments, most recent first -- the "past/current" half of the dashboard's
    // bank-statement view. Future ones are covered separately by getFuturePayments().
    std::string sql = "SELECT * FROM payments WHERE status != 'future' ORDER BY year DESC, month DESC, day DESC, id DESC LIMIT ?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, limit);
    while(sqlite3_step(stmt) == SQLITE_ROW) {
        Payment p;
        p.id = sqlite3_column_int(stmt,0);
        p.service_id = sqlite3_column_int(stmt,1);
        p.year = sqlite3_column_int(stmt,2);
        p.month = sqlite3_column_int(stmt,3);
        p.day = sqlite3_column_int(stmt,4);
        p.amount_due = sqlite3_column_double(stmt,5);
        p.amount_paid = sqlite3_column_double(stmt,6);
        p.balance = sqlite3_column_double(stmt,7);
        p.status = sqlite3_column_text(stmt,8) ? (const char*)sqlite3_column_text(stmt,8) : "";
        p.notes = sqlite3_column_text(stmt,9) ? (const char*)sqlite3_column_text(stmt,9) : "";
        p.user_id = sqlite3_column_type(stmt,10) == SQLITE_NULL ? -1 : sqlite3_column_int(stmt,10);
        p.direction = NormalizeDirection((const char*)sqlite3_column_text(stmt,11));
        p.resolution_date = sqlite3_column_text(stmt,12) ? (const char*)sqlite3_column_text(stmt,12) : "";
        p.resolved_at = sqlite3_column_text(stmt,13) ? (const char*)sqlite3_column_text(stmt,13) : "";
        results.push_back(p);
    }
    sqlite3_finalize(stmt);
    return results;
}

// Relational search: payment's own columns plus a JOIN against users/services so the
// caller can filter by owner name / service label too (a payment carries user_id/service_id
// but not the text itself). All params are optional; year/month/day <= 0 and empty strings
// are simply not filtered on. Fully parameterized -- no raw concatenation of user input.
std::vector<Payment> SqliteConn::advancedSearchPayments(const std::string& year, const std::string& month, const std::string& day,
                                                          const std::string& amount_due_query, const std::string& amount_paid_query,
                                                          const std::string& status, const std::string& direction,
                                                          const std::string& notes, const std::string& resolution_date,
                                                          const std::string& userName, const std::string& serviceLabel,
                                                          const std::string& idQuery, const std::string& serviceIdQuery,
                                                          const std::string& userIdQuery) {
    std::vector<Payment> results;
    std::string sql =
        "SELECT p.id, p.service_id, p.year, p.month, p.day, p.amount_due, p.amount_paid, p.balance, "
        "p.status, p.notes, p.user_id, p.direction, p.resolution_date, p.resolved_at "
        "FROM payments p "
        "LEFT JOIN users u ON u.id = p.user_id "
        "LEFT JOIN services s ON s.id = p.service_id "
        "WHERE 1=1";
    std::vector<std::string> params;
    // All numeric-ish fields (own id, year/month/day, foreign keys) are matched as
    // substrings against the stringified column, same as every text field, so
    // half-written numbers narrow results the same way half-written text does.
    auto addTextClause = [&](const char* colExpr, const std::string& val) {
        if (val.empty()) return;
        sql += std::string(" AND LOWER(") + colExpr + ") LIKE '%' || LOWER(?) || '%'";
        params.push_back(val);
    };
    addTextClause("CAST(p.id AS TEXT)", idQuery);
    addTextClause("CAST(p.year AS TEXT)", year);
    addTextClause("CAST(p.month AS TEXT)", month);
    addTextClause("CAST(p.day AS TEXT)", day);
    addTextClause("CAST(p.amount_due AS TEXT)", amount_due_query);
    addTextClause("CAST(p.amount_paid AS TEXT)", amount_paid_query);
    addTextClause("p.status", status);
    addTextClause("p.direction", direction);
    addTextClause("p.notes", notes);
    addTextClause("p.resolution_date", resolution_date);
    addTextClause("u.full_name", userName);
    addTextClause("s.label", serviceLabel);
    addTextClause("CAST(p.service_id AS TEXT)", serviceIdQuery);
    addTextClause("CAST(p.user_id AS TEXT)", userIdQuery);
    sql += " ORDER BY p.year DESC, p.month DESC, p.day DESC, p.id DESC LIMIT 2000";

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    for (size_t i = 0; i < params.size(); i++) {
        sqlite3_bind_text(stmt, (int)i + 1, params[i].c_str(), -1, SQLITE_TRANSIENT);
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Payment p;
        p.id = sqlite3_column_int(stmt,0);
        p.service_id = sqlite3_column_int(stmt,1);
        p.year = sqlite3_column_int(stmt,2);
        p.month = sqlite3_column_int(stmt,3);
        p.day = sqlite3_column_int(stmt,4);
        p.amount_due = sqlite3_column_double(stmt,5);
        p.amount_paid = sqlite3_column_double(stmt,6);
        p.balance = sqlite3_column_double(stmt,7);
        p.status = sqlite3_column_text(stmt,8) ? (const char*)sqlite3_column_text(stmt,8) : "";
        p.notes = sqlite3_column_text(stmt,9) ? (const char*)sqlite3_column_text(stmt,9) : "";
        p.user_id = sqlite3_column_type(stmt,10) == SQLITE_NULL ? -1 : sqlite3_column_int(stmt,10);
        p.direction = NormalizeDirection((const char*)sqlite3_column_text(stmt,11));
        p.resolution_date = sqlite3_column_text(stmt,12) ? (const char*)sqlite3_column_text(stmt,12) : "";
        p.resolved_at = sqlite3_column_text(stmt,13) ? (const char*)sqlite3_column_text(stmt,13) : "";
        results.push_back(p);
    }
    sqlite3_finalize(stmt);
    return results;
}

std::vector<Payment> SqliteConn::getAllPayments() {
    std::vector<Payment> results;
    std::string sql = "SELECT * FROM payments ORDER BY year, month, day, id";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    while(sqlite3_step(stmt) == SQLITE_ROW) {
        Payment p;
        p.id = sqlite3_column_int(stmt,0);
        p.service_id = sqlite3_column_int(stmt,1);
        p.year = sqlite3_column_int(stmt,2);
        p.month = sqlite3_column_int(stmt,3);
        p.day = sqlite3_column_int(stmt,4);
        p.amount_due = sqlite3_column_double(stmt,5);
        p.amount_paid = sqlite3_column_double(stmt,6);
        p.balance = sqlite3_column_double(stmt,7);
        p.status = sqlite3_column_text(stmt,8) ? (const char*)sqlite3_column_text(stmt,8) : "";
        p.notes = sqlite3_column_text(stmt,9) ? (const char*)sqlite3_column_text(stmt,9) : "";
        p.user_id = sqlite3_column_type(stmt,10) == SQLITE_NULL ? -1 : sqlite3_column_int(stmt,10);
        p.direction = NormalizeDirection((const char*)sqlite3_column_text(stmt,11));
        p.resolution_date = sqlite3_column_text(stmt,12) ? (const char*)sqlite3_column_text(stmt,12) : "";
        p.resolved_at = sqlite3_column_text(stmt,13) ? (const char*)sqlite3_column_text(stmt,13) : "";
        results.push_back(p);
    }
    sqlite3_finalize(stmt);
    return results;
}

std::vector<Payment> SqliteConn::getFuturePayments() {
    std::vector<Payment> results;
    std::string sql = "SELECT * FROM payments WHERE status = 'future'";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    while(sqlite3_step(stmt) == SQLITE_ROW) {
        Payment p;
        p.id = sqlite3_column_int(stmt,0);
        p.service_id = sqlite3_column_int(stmt,1);
        p.year = sqlite3_column_int(stmt,2);
        p.month = sqlite3_column_int(stmt,3);
        p.day = sqlite3_column_int(stmt,4);
        p.amount_due = sqlite3_column_double(stmt,5);
        p.amount_paid = sqlite3_column_double(stmt,6);
        p.balance = sqlite3_column_double(stmt,7);
        p.status = sqlite3_column_text(stmt,8) ? (const char*)sqlite3_column_text(stmt,8) : "";
        p.notes = sqlite3_column_text(stmt,9) ? (const char*)sqlite3_column_text(stmt,9) : "";
        p.user_id = sqlite3_column_type(stmt,10) == SQLITE_NULL ? -1 : sqlite3_column_int(stmt,10);
        p.direction = NormalizeDirection((const char*)sqlite3_column_text(stmt,11));
        p.resolution_date = sqlite3_column_text(stmt,12) ? (const char*)sqlite3_column_text(stmt,12) : "";
        p.resolved_at = sqlite3_column_text(stmt,13) ? (const char*)sqlite3_column_text(stmt,13) : "";
        results.push_back(p);
    }
    sqlite3_finalize(stmt);
    return results;
}

// --- TIMELINE ---
// Table/action -> a human-readable Greek notice, for the "friendly" (App-notices)
// rendering mode. Falls back to a generic phrasing for anything not special-cased.
static std::string friendlyNotice(const std::string& table, const std::string& action, int recordId) {
    static const std::unordered_map<std::string, std::string> tableNames = {
        {"users", "έναν χρήστη"}, {"services", "μια υπηρεσία"},
        {"service_users", "μια σύνδεση χρήστη-υπηρεσίας"}, {"payments", "μια πληρωμή"}
    };
    std::string what = tableNames.count(table) ? tableNames.at(table) : ("κάτι στο " + table);
    std::string verb = action == "INSERT" ? "Προσθέσατε" : action == "UPDATE" ? "Επεξεργαστήκατε" : action == "DELETE" ? "Διαγράψατε" : action;
    return verb + " " + what + " (#" + std::to_string(recordId) + ")";
}

std::vector<std::string> SqliteConn::getTimelineActions(const std::string& dateFilter, int offset, int limit,
                                                          const std::string& category, bool friendly) {
    std::vector<std::string> actions;
    std::string sql = "SELECT changed_at, table_name, action_type, old_data, new_data, record_id, category FROM audit_log WHERE changed_at LIKE ?";
    if (!category.empty()) sql += " AND category = ?";
    sql += " ORDER BY changed_at DESC LIMIT ? OFFSET ?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    std::string filter = dateFilter.empty() ? "%" : dateFilter + "%";
    int idx = 1;
    sqlite3_bind_text(stmt, idx++, filter.c_str(), -1, SQLITE_STATIC);
    if (!category.empty()) sqlite3_bind_text(stmt, idx++, category.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, idx++, limit);
    sqlite3_bind_int(stmt, idx++, offset);
    while(sqlite3_step(stmt) == SQLITE_ROW) {
        std::string date = (const char*)sqlite3_column_text(stmt,0);
        std::string table = (const char*)sqlite3_column_text(stmt,1);
        std::string action = (const char*)sqlite3_column_text(stmt,2);
        int recordId = sqlite3_column_int(stmt,5);
        if (friendly) {
            actions.push_back(date + " | " + friendlyNotice(table, action, recordId));
        } else {
            actions.push_back(date + " | " + table + " " + action + " (#" + std::to_string(recordId) + ")");
        }
    }
    sqlite3_finalize(stmt);
    return actions;
}

// --- METRICS ---
double SqliteConn::getTotalPaid(int year, int month, int userId, int serviceId) {
    double total = 0;
    std::string sql = "SELECT SUM(amount_paid) FROM payments WHERE year=? AND month=? AND status != 'future'";
    if(userId != -1) sql += " AND user_id=" + std::to_string(userId);
    if(serviceId != -1) sql += " AND service_id=" + std::to_string(serviceId);
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_int(stmt,1,year); sqlite3_bind_int(stmt,2,month);
    if(sqlite3_step(stmt) == SQLITE_ROW) total = sqlite3_column_double(stmt,0);
    sqlite3_finalize(stmt);
    return total;
}

double SqliteConn::getTotalPending(int year, int month, int userId, int serviceId) {
    double total = 0;
    std::string sql = "SELECT SUM(amount_due - amount_paid) FROM payments WHERE year=? AND month=? AND status='pending'";
    if(userId != -1) sql += " AND user_id=" + std::to_string(userId);
    if(serviceId != -1) sql += " AND service_id=" + std::to_string(serviceId);
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_int(stmt,1,year); sqlite3_bind_int(stmt,2,month);
    if(sqlite3_step(stmt) == SQLITE_ROW) total = sqlite3_column_double(stmt,0);
    sqlite3_finalize(stmt);
    return total;
}

// --- NOTES ---
std::vector<Note> SqliteConn::getAllNotes() {
    std::vector<Note> results;
    std::string sql = "SELECT id, title, content, created_at FROM notes ORDER BY id DESC";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Note n;
        n.id = sqlite3_column_int(stmt, 0);
        n.title = sqlite3_column_text(stmt, 1) ? (const char*)sqlite3_column_text(stmt, 1) : "";
        n.content = sqlite3_column_text(stmt, 2) ? (const char*)sqlite3_column_text(stmt, 2) : "";
        n.created_at = sqlite3_column_text(stmt, 3) ? (const char*)sqlite3_column_text(stmt, 3) : "";
        results.push_back(n);
    }
    sqlite3_finalize(stmt);
    return results;
}

bool SqliteConn::addNote(const Note& n) {
    std::string sql = "INSERT INTO notes (title, content) VALUES (?,?)";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, n.title.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, n.content.c_str(), -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) {
        int id = sqlite3_last_insert_rowid(db);
        logAction("notes", id, "INSERT", "", "{ \"title\": \"" + n.title + "\" }", "business");
        return true;
    }
    return false;
}

bool SqliteConn::editNote(int id, const Note& n) {
    std::string sql = "UPDATE notes SET title=?, content=? WHERE id=?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, n.title.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, n.content.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) { logAction("notes", id, "UPDATE", "", "{ \"title\": \"" + n.title + "\" }", "business"); return true; }
    return false;
}

bool SqliteConn::removeNote(int id) {
    std::string sql = "DELETE FROM notes WHERE id=?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) { logAction("notes", id, "DELETE", "", "", "business"); return true; }
    return false;
}

// --- BACKUP ---
bool SqliteConn::exportAllTablesToCSV(const std::string& folder) {
    std::vector<std::string> tables = {"users", "services", "service_users", "master_services", "master_service_components", "payments", "notes"};
    for(const auto& t : tables) {
        std::string path = folder + "/" + t + "_backup.csv";
        std::ofstream file(path);
        if(!file.is_open()) return false;
        auto cols = getTableColumns(t);
        auto greek = greekHeaders[t];
        for(size_t i=0; i<greek.size(); ++i) file << (i?",":"") << "\"" << greek[i] << "\"";
        file << "\n";
        auto rows = getPaginatedData(t, 0, 999999); // get all
        for(auto& row : rows) {
            for(size_t i=0; i<row.size(); ++i) file << (i?",":"") << "\"" << row[i] << "\"";
            file << "\n";
        }
        file.close();
    }
    return true;
}

// --- RAW VIEW ---
std::vector<std::vector<std::string>> SqliteConn::getPaginatedData(const std::string& table, int offset, int limit) {
    std::vector<std::vector<std::string>> rows;
    std::string sql = "SELECT * FROM " + table + " LIMIT ? OFFSET ?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_int(stmt,1,limit); sqlite3_bind_int(stmt,2,offset);
    while(sqlite3_step(stmt) == SQLITE_ROW) {
        std::vector<std::string> row;
        for(int i=0; i<sqlite3_column_count(stmt); ++i) {
            const char* val = (const char*)sqlite3_column_text(stmt,i);
            row.push_back(val ? val : "");
        }
        rows.push_back(row);
    }
    sqlite3_finalize(stmt);
    return rows;
}

std::vector<std::string> SqliteConn::getTableColumns(const std::string& table) {
    std::vector<std::string> cols;
    std::string sql = "PRAGMA table_info(" + table + ")";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    while(sqlite3_step(stmt) == SQLITE_ROW) cols.push_back((const char*)sqlite3_column_text(stmt,1));
    sqlite3_finalize(stmt);
    return cols;
}

int SqliteConn::getRowCount(const std::string& table) {
    int count = 0;
    std::string sql = "SELECT COUNT(*) FROM " + table;
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    if(sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt,0);
    sqlite3_finalize(stmt);
    return count;
}


std::vector<DataConn::JointServiceSummary> SqliteConn::getJointServices() {
    std::vector<JointServiceSummary> results;
    // Find services that have more than one user in service_users
    std::string sql = R"(
        SELECT s.id, s.label, s.extra_notes, s.created_at,
               COUNT(su.user_id) as owner_count
        FROM services s
        JOIN service_users su ON su.service_id = s.id
        GROUP BY s.id
        HAVING COUNT(su.user_id) > 1
        ORDER BY s.label
    )";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        JointServiceSummary jss;
        Service& s = jss.service;
        s.id = sqlite3_column_int(stmt, 0);
        s.label = (const char*)sqlite3_column_text(stmt, 1);
        s.extra_notes = sqlite3_column_text(stmt, 2) ? (const char*)sqlite3_column_text(stmt, 2) : "";
        s.created_at = sqlite3_column_text(stmt, 3) ? (const char*)sqlite3_column_text(stmt, 3) : "";

        // Get users for this service
        jss.users = getUsersForService(s.id);

        // Sum payments per user
        jss.userTotalPaid.clear();
        jss.userTotalDue.clear();
        jss.userNet.clear();
        jss.totalDue = 0.0;
        jss.totalPaid = 0.0;
        jss.totalNet = 0.0;
        for (auto& u : jss.users) {
            auto payments = getPaymentsForUser(u.id); // only this user's payments
            double due = 0.0, paid = 0.0, paidOut = 0.0, collected = 0.0;
            for (auto& p : payments) {
                if (p.service_id == s.id && p.status != "future") { // only payments for this service, excl. future
                    due += p.amount_due;
                    paid += p.amount_paid;
                    if (p.direction == "out") paidOut += p.amount_paid; else collected += p.amount_paid;
                }
            }
            jss.userTotalDue.push_back(due);
            jss.userTotalPaid.push_back(paid);
            jss.userNet.push_back(paidOut - collected);
            jss.totalDue += due;
            jss.totalPaid += paid;
            jss.totalNet += (paidOut - collected);
        }
        results.push_back(jss);
    }
    sqlite3_finalize(stmt);
    return results;
}

bool SqliteConn::executeRawQuery(const std::string& sql, std::vector<std::string>& outColumns,
                                  std::vector<std::vector<std::string>>& outRows, std::string& outError) {
    outColumns.clear();
    outRows.clear();
    outError.clear();
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        outError = sqlite3_errmsg(db);
        return false;
    }
    int colCount = sqlite3_column_count(stmt);
    for (int i = 0; i < colCount; i++) {
        const char* name = sqlite3_column_name(stmt, i);
        outColumns.push_back(name ? name : "");
    }
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        std::vector<std::string> row;
        for (int i = 0; i < colCount; i++) {
            const unsigned char* txt = sqlite3_column_text(stmt, i);
            row.push_back(txt ? (const char*)txt : "");
        }
        outRows.push_back(row);
    }
    if (rc != SQLITE_DONE) {
        outError = sqlite3_errmsg(db);
        sqlite3_finalize(stmt);
        return false;
    }
    int changes = sqlite3_changes(db);
    sqlite3_finalize(stmt);
    if (colCount == 0) {
        outColumns.push_back("result");
        std::vector<std::string> row;
        row.push_back(std::to_string(changes) + " row(s) affected");
        outRows.push_back(row);
    }
    if (changes > 0) {
        // Raw SQL console writes are logged unconditionally as 'db' category -
        // never 'business', regardless of which table they touched.
        logAction("raw_sql", 0, "EXEC", "", sql, "db");
    }
    return true;
}

