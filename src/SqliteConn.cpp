#include "SqliteConn.h"
#include "ErrorLog.h"
#include "Platform.h"
#include "Money.h"
#include "Rf.h"
#include "Sha256.h"
#include "TextFold.h"
#include <map>
#include <set>
#include <cstdlib>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <sstream>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <unordered_map>

// Helper to convert struct to JSON string manually for audit
static std::string userToJson(const User& u) {
    return "{ \"id\": " + std::to_string(u.id) + ", \"full_name\": \"" + u.full_name + "\", \"phone\": \"" + u.phone +
           "\", \"special_code\": \"" + u.special_code + "\", \"apartment_id\": " + std::to_string(u.apartment_id) + " }";
}

static std::string ColText(sqlite3_stmt* s, int i) {
    const unsigned char* t = sqlite3_column_text(s, i);
    return t ? (const char*)t : "";
}

static bool ColumnExists(sqlite3* db, const std::string& table, const std::string& col) {
    bool found = false;
    sqlite3_stmt* stmt = nullptr;
    std::string sql = "PRAGMA table_info(" + table + ")";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (ColText(stmt, 1) == col) { found = true; break; }
        }
    }
    sqlite3_finalize(stmt);
    return found;
}

// ONE select + ONE row-mapper for every query that returns Users. The users table
// no longer stores address/area/ΤΚ/apartment -- they are derived here from
// users -> apartments -> buildings, so "where does this client live" is always
// read through the same join and can't drift between code paths (the old
// per-query positional column mapping is exactly what made earlier schema
// additions so error-prone). Append a WHERE/ORDER clause to use it.
static const char* kUserSelect =
    "SELECT u.id, u.full_name, u.phone, u.special_code, u.created_at, u.apartment_id, "
    "       a.label, b.id, b.address, b.area, b.postal_code "
    "FROM users u "
    "LEFT JOIN apartments a ON a.id = u.apartment_id "
    "LEFT JOIN buildings  b ON b.id = a.building_id ";

static User MapUserRow(sqlite3_stmt* s) {
    User u;
    u.id           = sqlite3_column_int(s, 0);
    u.full_name    = ColText(s, 1);
    u.phone        = ColText(s, 2);
    u.special_code = ColText(s, 3);
    u.created_at   = ColText(s, 4);
    u.apartment_id = sqlite3_column_type(s, 5) == SQLITE_NULL ? -1 : sqlite3_column_int(s, 5);
    u.apartment    = ColText(s, 6);
    u.building_id  = sqlite3_column_type(s, 7) == SQLITE_NULL ? -1 : sqlite3_column_int(s, 7);
    u.address      = ColText(s, 8);
    u.area         = ColText(s, 9);
    u.postal_code  = ColText(s, 10);
    return u;
}

static std::string serviceToJson(const Service& s) {
    return "{ \"id\": " + std::to_string(s.id) + ", \"label\": \"" + s.label + "\", \"suggested_price\": " + FormatMoney(s.suggested_price) + " }";
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

// One column list + one reader for every payment query, so no function depends on the table's physical
// column order (they used SELECT * with hard-coded positions). Timestamps are stored UTC and returned in
// local time; the date is one ISO column split back into the parts the UI uses.
static const char* kPaymentCols =
    "p.id, p.service_id, p.user_id, p.apartment_id, p.pay_date, p.amount_due, p.amount_paid, p.balance, "
    "p.status, p.direction, p.notes, COALESCE(p.resolution_date,''), "
    "COALESCE(datetime(p.resolved_at,'localtime'),''), COALESCE(datetime(p.created_at,'localtime'),''), "
    // the receipt covering the money paid on this charge (an issued one first), -1 = none
    "COALESCE((SELECT m.receipt_id FROM allocations a JOIN payments m ON m.id = a.payment_id JOIN receipts r ON r.id = m.receipt_id "
    "          WHERE a.charge_id = p.id ORDER BY (r.status = 'issued') DESC, m.receipt_id DESC LIMIT 1), -1), "
    "COALESCE(p.batch_id,-1), "
    // the joint charge (κοινόχρηστο) this line is a share of, -1 = none (joint_ops.batch_id is unique)
    "COALESCE((SELECT o.id FROM joint_ops o WHERE o.batch_id = p.batch_id), -1)";

static Payment ReadPayment(sqlite3_stmt* st) {
    auto txt = [&](int c) { const unsigned char* t = sqlite3_column_text(st, c); return std::string(t ? (const char*)t : ""); };
    auto optInt = [&](int c) { return sqlite3_column_type(st, c) == SQLITE_NULL ? -1 : sqlite3_column_int(st, c); };
    Payment p;
    p.id = sqlite3_column_int(st, 0);
    p.service_id = sqlite3_column_int(st, 1);
    p.user_id = optInt(2);
    p.apartment_id = optInt(3);
    std::sscanf(txt(4).c_str(), "%d-%d-%d", &p.year, &p.month, &p.day);
    p.amount_due = sqlite3_column_double(st, 5);
    p.amount_paid = sqlite3_column_double(st, 6);
    p.balance = sqlite3_column_double(st, 7);
    p.status = txt(8);
    p.direction = NormalizeDirection(sqlite3_column_type(st, 9) == SQLITE_NULL ? "in" : txt(9).c_str());
    p.notes = txt(10);
    p.resolution_date = txt(11);
    p.resolved_at = txt(12);
    p.created_at = txt(13);
    p.receipt_id = sqlite3_column_int(st, 14);
    p.batch_id = sqlite3_column_int(st, 15);
    p.joint_op_id = sqlite3_column_int(st, 16);
    return p;
}

static std::string PayDate(const Payment& p) {
    char d[16]; std::snprintf(d, sizeof(d), "%04d-%02d-%02d", p.year, p.month, p.day);
    return d;
}

// Runs a payment query (sql must select kPaymentCols FROM payments p ...) with int binds.
static std::vector<Payment> QueryPayments(sqlite3* db, const std::string& sql, std::initializer_list<int> binds = {}) {
    std::vector<Payment> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) { std::cerr << "SQL: " << sqlite3_errmsg(db) << "\n"; return out; }
    int i = 1;
    for (int b : binds) sqlite3_bind_int(st, i++, b);
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(ReadPayment(st));
    sqlite3_finalize(st);
    return out;
}

static std::string paymentToJson(const Payment& p) {
    return "{ \"id\": " + std::to_string(p.id) + ", \"service_id\": " + std::to_string(p.service_id) + ", \"user_id\": " + std::to_string(p.user_id) + ", \"year\": " + std::to_string(p.year) + ", \"month\": " + std::to_string(p.month) + ", \"amount_due\": " + FormatMoney(p.amount_due) + ", \"amount_paid\": " + FormatMoney(p.amount_paid) + ", \"status\": \"" + p.status + "\", \"direction\": \"" + p.direction + "\", \"resolution_date\": \"" + p.resolution_date + "\", \"resolved_at\": \"" + p.resolved_at + "\" }";
}

static std::string AuditCanonical(const std::string& prev, const std::string& id, const std::string& at,
                                  const std::string& actor, const std::string& table, const std::string& recordId,
                                  const std::string& action, const std::string& oldData, const std::string& newData,
                                  const std::string& reason, const std::string& category);
static std::string JsonRowExpr(sqlite3* db, const std::string& table, const std::string& prefix);
static const char* kAuditedTables[] = {
    "buildings", "apartments", "users", "residencies", "services", "service_users", "master_services",
    "master_service_components", "payment_batches", "charges", "payments", "allocations", "receipts", "notes",
    "attachments", "joint_services", "joint_ops", "joint_shares", "payment_plans", "offers", "offer_uses",
};

static bool TableExists(sqlite3* db, const std::string& t) {
    sqlite3_stmt* st = nullptr; bool yes = false;
    sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, t.c_str(), -1, SQLITE_TRANSIENT);
    yes = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return yes;
}

static DataConn::AuditCheck VerifyAudit(sqlite3* db, const std::string& dbPath);
static std::string QuickAuditCheck(sqlite3* db, const std::string& dbPath);

static int UserVersion(sqlite3* db) {
    int v = 0; sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return v;
}

SqliteConn::SqliteConn(const std::string& dbPath) {
    dbPath_ = dbPath;
    errlog::SetFolder(std::filesystem::u8path(dbPath).parent_path().u8string());
    int rc = sqlite3_open(dbPath.c_str(), &db);
    if (rc) {
        std::cerr << "Can't open database: " << sqlite3_errmsg(db) << std::endl;
        return;
    }
    registerSqlFunctions();   // audit_hash / app_actor / app_reason / fold -- the triggers and searches need them
    // WAL: a reader (the background integrity check) never blocks the app's writes, and a crash mid-write
    // can't corrupt the file. busy_timeout: wait briefly instead of failing if the file is momentarily locked.
    executeQuery("PRAGMA journal_mode=WAL;");
    sqlite3_busy_timeout(db, 5000);
    const int versionAtOpen = UserVersion(db);

    // Create tables if not exist
    std::string schema = R"(
        CREATE TABLE IF NOT EXISTS buildings (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            address TEXT NOT NULL,
            area TEXT,
            postal_code TEXT,
            afm TEXT,
            created_at TEXT DEFAULT (datetime('now'))
        );
        CREATE TABLE IF NOT EXISTS apartments (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            building_id INTEGER NOT NULL,
            label TEXT NOT NULL,
            FOREIGN KEY (building_id) REFERENCES buildings(id) ON DELETE CASCADE
        );
        -- Column order is deliberate: id, full_name, phone, created_at, special_code,
        -- apartment_id is exactly what a LEGACY users table ends up as after the
        -- restructure migration below drops its old columns, so fresh and migrated
        -- databases are identical (raw view / CSV headers match by position).
        CREATE TABLE IF NOT EXISTS users (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            full_name TEXT NOT NULL,
            phone TEXT,
            created_at TEXT DEFAULT (datetime('now')),
            special_code TEXT,
            apartment_id INTEGER REFERENCES apartments(id) ON DELETE SET NULL
        );
        CREATE TABLE IF NOT EXISTS services (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            label TEXT NOT NULL,
            extra_notes TEXT,
            created_at TEXT DEFAULT (datetime('now')),
            suggested_price REAL DEFAULT 0
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
    // The original schema and the ad-hoc migrations below date from before v7. A database at v7+ has been through
    // them; running them again would misfire since v9, where `payments` means money (they'd add line columns to it).
    if (UserVersion(db) < 7) {
    executeQuery(schema);

    if (!ColumnExists(db, "services", "suggested_price"))
        executeQuery("ALTER TABLE services ADD COLUMN suggested_price REAL DEFAULT 0;");

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


    // --- Migration: buildings + apartments become real tables; users stop carrying
    // an address and just point at an apartment (users.apartment_id) ---
    //
    // A LEGACY users table is recognised by still having its own `address` column.
    // For those, in order:
    //   1. snapshot the whole DB first (VACUUM INTO -- a consistent copy), because
    //      the last step irreversibly drops columns,
    //   2. build one `buildings` row per distinct (address, area, ΤΚ) found among
    //      the users, one `apartments` row per distinct (building, apartment label),
    //      and point each user at theirs -- NOTHING about where people live is lost,
    //   3. drop the old per-user columns. address/area/postal_code/apartment now
    //      live in buildings/apartments; `afm` and `contract_code` (ΣΥΜΒΟΛΑΙΟ) are
    //      dropped on purpose -- the client confirmed they are not needed.
    // Users with no address stay unassigned (apartment_id NULL) rather than being
    // parked in a fake "no address" building. Runs inside one transaction: if any
    // step fails it rolls back and the legacy table is left exactly as it was.
    // A fresh/already-migrated DB has no `address` column, so this is skipped.
    if (!ColumnExists(db, "users", "apartment_id")) {
        executeQuery("ALTER TABLE users ADD COLUMN apartment_id INTEGER REFERENCES apartments(id) ON DELETE SET NULL;");
    }
    if (ColumnExists(db, "users", "address")) {
        // Very old DBs predate the free-text apartment column; the SQL below reads it.
        if (!ColumnExists(db, "users", "apartment")) {
            executeQuery("ALTER TABLE users ADD COLUMN apartment TEXT;");
        }

        std::string bak = dbPath + ".pre-buildings.bak";
        {
            std::ifstream exists(bak, std::ios::binary);
            if (!exists.good()) {
                std::string q;
                for (char c : bak) { q += c; if (c == '\'') q += '\''; }
                executeQuery("VACUUM INTO '" + q + "';");
            }
        }

        std::string mig =
            "BEGIN;"
            "INSERT INTO buildings (address, area, postal_code, afm) "
            "  SELECT DISTINCT TRIM(address), COALESCE(TRIM(area),''), COALESCE(TRIM(postal_code),''), '' "
            "  FROM users WHERE address IS NOT NULL AND TRIM(address) <> '';"
            "INSERT INTO apartments (building_id, label) "
            "  SELECT DISTINCT b.id, COALESCE(NULLIF(TRIM(u.apartment),''),'—') "
            "  FROM users u JOIN buildings b ON b.address = TRIM(u.address) "
            "       AND b.area = COALESCE(TRIM(u.area),'') AND b.postal_code = COALESCE(TRIM(u.postal_code),'') "
            "  WHERE u.address IS NOT NULL AND TRIM(u.address) <> '';"
            "UPDATE users SET apartment_id = ("
            "  SELECT a.id FROM apartments a JOIN buildings b ON b.id = a.building_id "
            "  WHERE b.address = TRIM(users.address) AND b.area = COALESCE(TRIM(users.area),'') "
            "    AND b.postal_code = COALESCE(TRIM(users.postal_code),'') "
            "    AND a.label = COALESCE(NULLIF(TRIM(users.apartment),''),'—') LIMIT 1) "
            "  WHERE address IS NOT NULL AND TRIM(address) <> '';"
            "ALTER TABLE users DROP COLUMN address;"
            "ALTER TABLE users DROP COLUMN area;"
            "ALTER TABLE users DROP COLUMN postal_code;"
            "ALTER TABLE users DROP COLUMN apartment;";
        if (ColumnExists(db, "users", "contract_code")) mig += "ALTER TABLE users DROP COLUMN contract_code;";
        if (ColumnExists(db, "users", "afm"))           mig += "ALTER TABLE users DROP COLUMN afm;";
        mig += "COMMIT;";

        char* err = nullptr;
        if (sqlite3_exec(db, mig.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
            std::cerr << "Buildings migration failed, rolling back: " << (err ? err : "?") << std::endl;
            sqlite3_free(err);
            sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        }
    }
    executeQuery("CREATE INDEX IF NOT EXISTS idx_users_apartment ON users(apartment_id);");
    executeQuery("CREATE INDEX IF NOT EXISTS idx_apartments_building ON apartments(building_id);");

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
        if (hasNextDueDate) {
            // Old DBs carry an index on this column; SQLite refuses to drop an indexed column.
            executeQuery("DROP INDEX IF EXISTS idx_services_due;");
            executeQuery("ALTER TABLE services DROP COLUMN next_due_date;");
        }
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
    // users: id, full_name, phone, created_at, special_code, apartment_id (see schema note)
    // --- Migration: apartment history. residencies = who lived where and when; payments get the
    // apartment they were made from (snapshot at the payment's date) and the minute they were
    // entered. Done once; old data is back-filled from the current assignments. ---
    if (UserVersion(db) < 2) {
        executeQuery("CREATE TABLE IF NOT EXISTS residencies ("
                     "id INTEGER PRIMARY KEY AUTOINCREMENT, user_id INTEGER NOT NULL, apartment_id INTEGER NOT NULL, "
                     "from_date TEXT NOT NULL, to_date TEXT);"
                     "CREATE INDEX IF NOT EXISTS idx_res_apartment ON residencies(apartment_id);"
                     "CREATE INDEX IF NOT EXISTS idx_res_user ON residencies(user_id);");
        if (!ColumnExists(db, "payments", "apartment_id")) executeQuery("ALTER TABLE payments ADD COLUMN apartment_id INTEGER;");
        if (!ColumnExists(db, "payments", "created_at"))   executeQuery("ALTER TABLE payments ADD COLUMN created_at TEXT;");
        int nRes = 0;
        { sqlite3_stmt* st; sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM residencies", -1, &st, nullptr);
          if (sqlite3_step(st) == SQLITE_ROW) nRes = sqlite3_column_int(st, 0); sqlite3_finalize(st); }
        if (nRes == 0) {
            // one open stay per client who currently has an apartment, starting no later than their first payment
            executeQuery("INSERT INTO residencies (user_id, apartment_id, from_date) "
                         "SELECT u.id, u.apartment_id, "
                         "  MIN(COALESCE(date(u.created_at), date('now','localtime')), "
                         "      COALESCE((SELECT MIN(printf('%04d-%02d-%02d', year, month, day)) FROM payments WHERE user_id = u.id), '9999-12-31')) "
                         "FROM users u WHERE u.apartment_id IS NOT NULL;");
        }
        executeQuery("UPDATE payments SET apartment_id = ("
                     "  SELECT r.apartment_id FROM residencies r WHERE r.user_id = payments.user_id "
                     "  AND r.from_date <= printf('%04d-%02d-%02d', payments.year, payments.month, payments.day) "
                     "  AND (r.to_date IS NULL OR r.to_date >= printf('%04d-%02d-%02d', payments.year, payments.month, payments.day)) "
                     "  ORDER BY r.from_date DESC, r.id DESC LIMIT 1) WHERE apartment_id IS NULL;"
                     "UPDATE payments SET apartment_id = (SELECT apartment_id FROM users WHERE users.id = payments.user_id) WHERE apartment_id IS NULL;"
                     "UPDATE payments SET created_at = printf('%04d-%02d-%02d 00:00:00', year, month, day) WHERE created_at IS NULL;"
                     "CREATE INDEX IF NOT EXISTS idx_payments_apartment ON payments(apartment_id);");
        if (UserVersion(db) < 2) executeQuery("PRAGMA user_version = 2;");
    }

    }   // end of pre-v7 schema + migrations

    // --- Migration v3: money is euros with exactly two decimals. Older rows could hold float noise
    // (e.g. 182.52499999999998) or a balance that drifted from due - paid; snap every amount to whole
    // cents and re-derive balance in cents. One transaction, logged once, never re-run. ---
    if (UserVersion(db) < 3) {
        auto fixed = [&](const char* sql) { executeQuery(sql); return sqlite3_changes(db); };
        executeQuery("BEGIN;");
        int nPay = fixed("UPDATE payments SET "
                         " amount_due  = ROUND(COALESCE(amount_due,0), 2),"
                         " amount_paid = ROUND(COALESCE(amount_paid,0), 2),"
                         " balance     = (CAST(ROUND(COALESCE(amount_due,0)*100) AS INTEGER) - CAST(ROUND(COALESCE(amount_paid,0)*100) AS INTEGER)) / 100.0 "
                         "WHERE amount_due IS NOT ROUND(COALESCE(amount_due,0), 2) OR amount_paid IS NOT ROUND(COALESCE(amount_paid,0), 2) "
                         "   OR balance IS NOT (CAST(ROUND(COALESCE(amount_due,0)*100) AS INTEGER) - CAST(ROUND(COALESCE(amount_paid,0)*100) AS INTEGER)) / 100.0;");
        int nSvc = fixed("UPDATE services SET suggested_price = ROUND(suggested_price, 2) WHERE suggested_price IS NOT ROUND(suggested_price, 2);");
        executeQuery("PRAGMA user_version = 3;");
        executeQuery("COMMIT;");
        if (nPay + nSvc > 0)
            logAction("migration", 3, "MONEY_TO_CENTS", "",
                      "{ \"payments_fixed\": " + std::to_string(nPay) + ", \"services_fixed\": " + std::to_string(nSvc) + " }", "db");
    }

    // --- Migration v4: receipts. N payments -> 1 receipt via payments.receipt_id (one column, so a
    // payment can never be on two receipts). Triggers make an ISSUED receipt and the money on its
    // payments unchangeable from anywhere, the SQL console included; the only way out is voiding. ---
    if (UserVersion(db) < 4) {
        executeQuery("BEGIN;");
        executeQuery(R"(
            CREATE TABLE IF NOT EXISTS receipts (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                series      TEXT    NOT NULL DEFAULT 'A',
                number      INTEGER NOT NULL,
                issue_date  TEXT    NOT NULL,
                user_id     INTEGER REFERENCES users(id),
                building_id INTEGER REFERENCES buildings(id),
                total       REAL    NOT NULL CHECK (total > 0),
                method      TEXT    NOT NULL CHECK (method IN ('cash','collector','bank','ebanking','card','iris','other')),
                rf_code     TEXT,
                paper_ref   TEXT,
                bank_ref    TEXT,
                notes       TEXT,
                status      TEXT    NOT NULL DEFAULT 'issued' CHECK (status IN ('issued','void')),
                void_reason TEXT,
                voided_at   TEXT,
                created_at  TEXT    DEFAULT (datetime('now','localtime')),
                UNIQUE (series, number)
            );
            CREATE INDEX IF NOT EXISTS idx_receipts_user ON receipts(user_id);
            CREATE INDEX IF NOT EXISTS idx_receipts_date ON receipts(issue_date);
            CREATE UNIQUE INDEX IF NOT EXISTS idx_receipts_rf ON receipts(rf_code) WHERE rf_code IS NOT NULL AND status = 'issued';

            CREATE TRIGGER IF NOT EXISTS trg_receipts_no_delete BEFORE DELETE ON receipts
            BEGIN SELECT RAISE(ABORT, 'Οι αποδείξεις δεν διαγράφονται - ακυρώστε την απόδειξη'); END;

            CREATE TRIGGER IF NOT EXISTS trg_receipts_locked BEFORE UPDATE ON receipts
            WHEN OLD.status = 'void'
              OR NEW.series IS NOT OLD.series OR NEW.number IS NOT OLD.number OR NEW.issue_date IS NOT OLD.issue_date
              OR NEW.user_id IS NOT OLD.user_id OR NEW.building_id IS NOT OLD.building_id OR NEW.total IS NOT OLD.total
              OR NEW.method IS NOT OLD.method OR NEW.rf_code IS NOT OLD.rf_code OR NEW.created_at IS NOT OLD.created_at
              OR (NEW.status = 'void' AND TRIM(COALESCE(NEW.void_reason,'')) = '')
            BEGIN SELECT RAISE(ABORT, 'Η απόδειξη έχει εκδοθεί και δεν αλλάζει - μόνο ακύρωση με αιτιολογία'); END;
        )");
        if (!ColumnExists(db, "payments", "receipt_id"))
            executeQuery("ALTER TABLE payments ADD COLUMN receipt_id INTEGER REFERENCES receipts(id);");
        executeQuery(R"(
            CREATE INDEX IF NOT EXISTS idx_payments_receipt ON payments(receipt_id);

            CREATE TRIGGER IF NOT EXISTS trg_payments_receipted_update BEFORE UPDATE ON payments
            WHEN OLD.receipt_id IS NOT NULL
             AND (SELECT status FROM receipts WHERE id = OLD.receipt_id) = 'issued'
             AND (NEW.amount_due IS NOT OLD.amount_due OR NEW.amount_paid IS NOT OLD.amount_paid
                  OR NEW.user_id IS NOT OLD.user_id OR NEW.service_id IS NOT OLD.service_id
                  OR NEW.direction IS NOT OLD.direction OR NEW.status IS NOT OLD.status
                  OR NEW.receipt_id IS NOT OLD.receipt_id)
            BEGIN SELECT RAISE(ABORT, 'Η πληρωμή έχει απόδειξη - ακυρώστε πρώτα την απόδειξη για να την αλλάξετε'); END;

            CREATE TRIGGER IF NOT EXISTS trg_payments_receipted_delete BEFORE DELETE ON payments
            WHEN OLD.receipt_id IS NOT NULL AND (SELECT status FROM receipts WHERE id = OLD.receipt_id) = 'issued'
            BEGIN SELECT RAISE(ABORT, 'Η πληρωμή έχει απόδειξη - ακυρώστε πρώτα την απόδειξη για να τη διαγράψετε'); END;
        )");
        executeQuery("PRAGMA user_version = 4;");
        executeQuery("COMMIT;");
    }

    // --- Migration v5: payment <-> apartment history is permanent. A payment records the apartment it
    // was made for (snapshot at its date, or chosen by hand for e.g. arrears paid after moving out).
    // Deleting an apartment, building or client that payments point at would erase "who paid for which
    // apartment", so the DB refuses it; the apartment of a receipted payment is frozen like its amounts. ---
    if (UserVersion(db) < 5) {
        executeQuery("BEGIN;");
        executeQuery(R"(
            CREATE TRIGGER IF NOT EXISTS trg_apartments_keep_payments BEFORE DELETE ON apartments
            WHEN EXISTS (SELECT 1 FROM payments WHERE apartment_id = OLD.id)
            BEGIN SELECT RAISE(ABORT, 'Το διαμέρισμα έχει πληρωμές στο ιστορικό του και δεν διαγράφεται'); END;

            CREATE TRIGGER IF NOT EXISTS trg_users_keep_payments BEFORE DELETE ON users
            WHEN EXISTS (SELECT 1 FROM payments WHERE user_id = OLD.id)
            BEGIN SELECT RAISE(ABORT, 'Ο πελάτης έχει πληρωμές στο ιστορικό του και δεν διαγράφεται'); END;

            DROP TRIGGER IF EXISTS trg_payments_receipted_update;
            CREATE TRIGGER trg_payments_receipted_update BEFORE UPDATE ON payments
            WHEN OLD.receipt_id IS NOT NULL
             AND (SELECT status FROM receipts WHERE id = OLD.receipt_id) = 'issued'
             AND (NEW.amount_due IS NOT OLD.amount_due OR NEW.amount_paid IS NOT OLD.amount_paid
                  OR NEW.user_id IS NOT OLD.user_id OR NEW.service_id IS NOT OLD.service_id
                  OR NEW.apartment_id IS NOT OLD.apartment_id
                  OR NEW.direction IS NOT OLD.direction OR NEW.status IS NOT OLD.status
                  OR NEW.receipt_id IS NOT OLD.receipt_id)
            BEGIN SELECT RAISE(ABORT, 'Η πληρωμή έχει απόδειξη - ακυρώστε πρώτα την απόδειξη για να την αλλάξετε'); END;
        )");
        executeQuery("PRAGMA user_version = 5;");
        executeQuery("COMMIT;");
    }

    // --- Migration v6: indexes for the queries the screens actually run (verified with EXPLAIN QUERY PLAN):
    // the payments date-range filter (expression must match BuildPaymentPageWhere's text exactly), the
    // timeline by date, "history of record X", and residency-on-date lookups. ---
    if (UserVersion(db) < 6) {
        executeQuery(
            "CREATE INDEX IF NOT EXISTS idx_payments_isodate ON payments(printf('%04d-%02d-%02d', year, month, day));"
            "CREATE INDEX IF NOT EXISTS idx_audit_changed ON audit_log(changed_at);"
            "CREATE INDEX IF NOT EXISTS idx_audit_record ON audit_log(table_name, record_id);"
            "CREATE INDEX IF NOT EXISTS idx_res_user_from ON residencies(user_id, from_date);"
            "PRAGMA user_version = 6;");
    }

    // --- Migration v7: real date/datetime columns, payment batches, trustworthy audit log. ---
    //  * payments.year/month/day -> pay_date ('YYYY-MM-DD', CHECKed); timestamps stored UTC ('YYYY-MM-DD HH:MM:SS')
    //    and shown in local time; the table is rebuilt with an explicit column order.
    //  * payment_batches + payments.batch_id: payments created together (a package charge).
    //  * receipts rebuilt the same way (UTC timestamps, ISO date CHECK).
    //  * audit_log rebuilt with actor / reason / prev_hash / hash; legacy entries kept and chained, then one
    //    BASELINE snapshot per existing row so every row has a known starting state.
    if (UserVersion(db) < 7) {
        executeQuery("BEGIN;");
        executeQuery(
            "DROP TRIGGER IF EXISTS trg_payments_receipted_update;"
            "DROP TRIGGER IF EXISTS trg_payments_receipted_delete;"
            "DROP TRIGGER IF EXISTS trg_receipts_locked;"
            "DROP TRIGGER IF EXISTS trg_receipts_no_delete;"
            "DROP TRIGGER IF EXISTS trg_apartments_keep_payments;"
            "DROP TRIGGER IF EXISTS trg_users_keep_payments;"
            "CREATE TABLE IF NOT EXISTS payment_batches ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  label TEXT,"
            "  created_at TEXT NOT NULL DEFAULT (datetime('now')) CHECK (created_at IS datetime(created_at)));");

        executeQuery(R"(
            CREATE TABLE receipts_v7 (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                series      TEXT    NOT NULL DEFAULT 'A',
                number      INTEGER NOT NULL,
                issue_date  TEXT    NOT NULL CHECK (issue_date IS date(issue_date)),
                user_id     INTEGER REFERENCES users(id),
                building_id INTEGER REFERENCES buildings(id),
                total       REAL    NOT NULL CHECK (total > 0),
                method      TEXT    NOT NULL CHECK (method IN ('cash','collector','bank','ebanking','card','iris','other')),
                rf_code     TEXT,
                paper_ref   TEXT,
                bank_ref    TEXT,
                notes       TEXT,
                status      TEXT    NOT NULL DEFAULT 'issued' CHECK (status IN ('issued','void')),
                void_reason TEXT,
                voided_at   TEXT CHECK (voided_at IS NULL OR voided_at IS datetime(voided_at)),
                created_at  TEXT NOT NULL DEFAULT (datetime('now')) CHECK (created_at IS datetime(created_at)),
                UNIQUE (series, number)
            );
            INSERT INTO receipts_v7 SELECT id, series, number, issue_date, user_id, building_id, total, method, rf_code, paper_ref,
                   bank_ref, notes, status, void_reason, datetime(voided_at, 'utc'), COALESCE(datetime(created_at, 'utc'), datetime('now'))
            FROM receipts;
            DROP TABLE receipts;
            ALTER TABLE receipts_v7 RENAME TO receipts;
            CREATE INDEX IF NOT EXISTS idx_receipts_user ON receipts(user_id);
            CREATE INDEX IF NOT EXISTS idx_receipts_date ON receipts(issue_date);
            CREATE UNIQUE INDEX IF NOT EXISTS idx_receipts_rf ON receipts(rf_code) WHERE rf_code IS NOT NULL AND status = 'issued';
        )");

        int fixedDates = 0;
        { sqlite3_stmt* st = nullptr;
          sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM payments WHERE date(printf('%04d-%02d-%02d', year, month, day)) IS NOT printf('%04d-%02d-%02d', year, month, day)", -1, &st, nullptr);
          if (sqlite3_step(st) == SQLITE_ROW) fixedDates = sqlite3_column_int(st, 0);
          sqlite3_finalize(st); }
        executeQuery(R"(
            CREATE TABLE payments_v7 (
                id              INTEGER PRIMARY KEY AUTOINCREMENT,
                service_id      INTEGER NOT NULL REFERENCES services(id),
                user_id         INTEGER REFERENCES users(id),
                apartment_id    INTEGER REFERENCES apartments(id),
                pay_date        TEXT    NOT NULL CHECK (pay_date IS date(pay_date)),
                amount_due      REAL    NOT NULL DEFAULT 0,
                amount_paid     REAL    NOT NULL DEFAULT 0,
                balance         REAL    NOT NULL DEFAULT 0,
                status          TEXT    NOT NULL DEFAULT 'paid',
                direction       TEXT    NOT NULL DEFAULT 'in' CHECK (direction IN ('in','out')),
                notes           TEXT,
                resolution_date TEXT CHECK (resolution_date IS NULL OR resolution_date IS date(resolution_date)),
                resolved_at     TEXT CHECK (resolved_at IS NULL OR resolved_at IS datetime(resolved_at)),
                created_at      TEXT NOT NULL DEFAULT (datetime('now')) CHECK (created_at IS datetime(created_at)),
                receipt_id      INTEGER REFERENCES receipts(id),
                batch_id        INTEGER REFERENCES payment_batches(id)
            );
            INSERT INTO payments_v7
            SELECT id, service_id, user_id, apartment_id,
                   COALESCE(date(printf('%04d-%02d-%02d', year, month, day)), '1970-01-01'),
                   COALESCE(amount_due, 0), COALESCE(amount_paid, 0), COALESCE(balance, 0),
                   COALESCE(status, 'paid'),
                   CASE WHEN direction IN ('in','out') THEN direction ELSE 'in' END,
                   notes,
                   date(NULLIF(resolution_date, '')),
                   datetime(NULLIF(resolved_at, ''), 'utc'),
                   COALESCE(datetime(created_at, 'utc'), datetime(printf('%04d-%02d-%02d', year, month, day)), datetime('now')),
                   receipt_id, NULL
            FROM payments;
            DROP TABLE payments;
            ALTER TABLE payments_v7 RENAME TO payments;
            CREATE INDEX IF NOT EXISTS idx_payments_date      ON payments(pay_date);
            CREATE INDEX IF NOT EXISTS idx_payments_user_date ON payments(user_id, pay_date);
            CREATE INDEX IF NOT EXISTS idx_payments_service   ON payments(service_id);
            CREATE INDEX IF NOT EXISTS idx_payments_apartment ON payments(apartment_id);
            CREATE INDEX IF NOT EXISTS idx_payments_receipt   ON payments(receipt_id);
            CREATE INDEX IF NOT EXISTS idx_payments_batch     ON payments(batch_id);
            CREATE INDEX IF NOT EXISTS idx_payments_status    ON payments(status, pay_date);

            CREATE TRIGGER trg_receipts_no_delete BEFORE DELETE ON receipts
            BEGIN SELECT RAISE(ABORT, 'Οι αποδείξεις δεν διαγράφονται - ακυρώστε την απόδειξη'); END;

            CREATE TRIGGER trg_receipts_locked BEFORE UPDATE ON receipts
            WHEN OLD.status = 'void'
              OR NEW.series IS NOT OLD.series OR NEW.number IS NOT OLD.number OR NEW.issue_date IS NOT OLD.issue_date
              OR NEW.user_id IS NOT OLD.user_id OR NEW.building_id IS NOT OLD.building_id OR NEW.total IS NOT OLD.total
              OR NEW.method IS NOT OLD.method OR NEW.rf_code IS NOT OLD.rf_code OR NEW.created_at IS NOT OLD.created_at
              OR (NEW.status = 'void' AND TRIM(COALESCE(NEW.void_reason,'')) = '')
            BEGIN SELECT RAISE(ABORT, 'Η απόδειξη έχει εκδοθεί και δεν αλλάζει - μόνο ακύρωση με αιτιολογία'); END;

            CREATE TRIGGER trg_payments_receipted_update BEFORE UPDATE ON payments
            WHEN OLD.receipt_id IS NOT NULL
             AND (SELECT status FROM receipts WHERE id = OLD.receipt_id) = 'issued'
             AND (NEW.amount_due IS NOT OLD.amount_due OR NEW.amount_paid IS NOT OLD.amount_paid
                  OR NEW.user_id IS NOT OLD.user_id OR NEW.service_id IS NOT OLD.service_id
                  OR NEW.apartment_id IS NOT OLD.apartment_id OR NEW.pay_date IS NOT OLD.pay_date
                  OR NEW.direction IS NOT OLD.direction OR NEW.status IS NOT OLD.status
                  OR NEW.receipt_id IS NOT OLD.receipt_id OR NEW.batch_id IS NOT OLD.batch_id)
            BEGIN SELECT RAISE(ABORT, 'Η πληρωμή έχει απόδειξη - ακυρώστε πρώτα την απόδειξη για να την αλλάξετε'); END;

            CREATE TRIGGER trg_payments_receipted_delete BEFORE DELETE ON payments
            WHEN OLD.receipt_id IS NOT NULL AND (SELECT status FROM receipts WHERE id = OLD.receipt_id) = 'issued'
            BEGIN SELECT RAISE(ABORT, 'Η πληρωμή έχει απόδειξη - ακυρώστε πρώτα την απόδειξη για να τη διαγράψετε'); END;

            CREATE TRIGGER trg_payments_batch_fixed BEFORE UPDATE OF batch_id ON payments
            WHEN NEW.batch_id IS NOT OLD.batch_id
            BEGIN SELECT RAISE(ABORT, 'Η ομάδα μιας πληρωμής ορίζεται μόνο κατά τη δημιουργία της'); END;

            CREATE TRIGGER trg_apartments_keep_payments BEFORE DELETE ON apartments
            WHEN EXISTS (SELECT 1 FROM payments WHERE apartment_id = OLD.id)
            BEGIN SELECT RAISE(ABORT, 'Το διαμέρισμα έχει πληρωμές στο ιστορικό του και δεν διαγράφεται'); END;

            CREATE TRIGGER trg_users_keep_payments BEFORE DELETE ON users
            WHEN EXISTS (SELECT 1 FROM payments WHERE user_id = OLD.id)
            BEGIN SELECT RAISE(ABORT, 'Ο πελάτης έχει πληρωμές στο ιστορικό του και δεν διαγράφεται'); END;
        )");

        // audit_log: rebuild with the trust columns, keep legacy rows
        executeQuery(R"(
            CREATE TABLE audit_log_v7 (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                changed_at  TEXT    NOT NULL DEFAULT (datetime('now')),
                actor       TEXT    NOT NULL DEFAULT '',
                table_name  TEXT    NOT NULL,
                record_id   INTEGER,
                action_type TEXT    NOT NULL,
                old_data    TEXT,
                new_data    TEXT,
                reason      TEXT    NOT NULL DEFAULT '',
                category    TEXT    NOT NULL DEFAULT 'business',
                prev_hash   TEXT,
                hash        TEXT
            );
            INSERT INTO audit_log_v7 (id, changed_at, actor, table_name, record_id, action_type, old_data, new_data, reason, category)
            SELECT id, COALESCE(datetime(changed_at), datetime('now')), 'πριν την v7', COALESCE(table_name, ''), record_id,
                   COALESCE(action_type, ''), old_data, new_data, '', COALESCE(category, 'db')
            FROM audit_log ORDER BY id;
            DROP TABLE audit_log;
            ALTER TABLE audit_log_v7 RENAME TO audit_log;
            CREATE INDEX IF NOT EXISTS idx_audit_changed ON audit_log(changed_at);
            CREATE INDEX IF NOT EXISTS idx_audit_record  ON audit_log(table_name, record_id, id);
        )");
        for (const char* t : kAuditedTables) {
            if (!TableExists(db, t)) continue;   // tables added by later migrations
            executeQuery(std::string("INSERT INTO audit_log (table_name, record_id, action_type, old_data, new_data, category, actor, reason) "
                         "SELECT '") + t + "', rowid, 'BASELINE', NULL, " + JsonRowExpr(db, t, "") + ", 'db', 'migration v7', "
                         "'Αρχική κατάσταση κατά την ενεργοποίηση του αμετάβλητου ιστορικού' FROM " + t + ";");
        }
        // chain everything that exists so far, in order
        {
            sqlite3_stmt* rd = nullptr; sqlite3_stmt* up = nullptr;
            sqlite3_prepare_v2(db, "SELECT id, changed_at, actor, table_name, record_id, action_type, old_data, new_data, reason, category "
                                   "FROM audit_log ORDER BY id", -1, &rd, nullptr);
            sqlite3_prepare_v2(db, "UPDATE audit_log SET prev_hash = ?, hash = ? WHERE id = ?", -1, &up, nullptr);
            auto txt = [&](int c) { const unsigned char* t = sqlite3_column_text(rd, c); return std::string(t ? (const char*)t : ""); };
            std::string prev;
            while (sqlite3_step(rd) == SQLITE_ROW) {
                std::string h = Sha256Hex(AuditCanonical(prev, txt(0), txt(1), txt(2), txt(3), txt(4), txt(5), txt(6), txt(7), txt(8), txt(9)));
                sqlite3_bind_text(up, 1, prev.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(up, 2, h.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(up, 3, sqlite3_column_int64(rd, 0));
                sqlite3_step(up); sqlite3_reset(up);
                prev = h;
            }
            sqlite3_finalize(rd); sqlite3_finalize(up);
        }
        ensureAuditTriggers();
        logAction("migration", 7, "SCHEMA_V7", "",
                  "{\"pay_date\": true, \"utc_timestamps\": true, \"audit_chain\": true, \"normalized_invalid_dates\": " + std::to_string(fixedDates) + "}", "db");
        executeQuery("PRAGMA user_version = 7;");
        if (!executeQuery("COMMIT;")) { executeQuery("ROLLBACK;"); std::cerr << "migration v7 failed\n"; }
    }


    // --- Migration v8: the date index also carries the amounts, so per-day / per-period totals (dashboard,
    // reports) read the index in date order instead of sorting the table: 2.2 s -> 0.46 s at 1M payments. ---
    if (UserVersion(db) < 8) {
        executeQuery("BEGIN;"
                     "DROP INDEX IF EXISTS idx_payments_date;"
                     "CREATE INDEX idx_payments_date ON payments(pay_date, status, direction, amount_due, amount_paid, balance);"
                     "PRAGMA user_version = 8;"
                     "COMMIT;");
    }


    // --- Migration v9: bills and payments are separate. ---
    //  * The old "payments" lines are what is OWED: renamed to `charges` (ids and history kept). amount_paid /
    //    balance stay on the charge but are now maintained by the database from allocations.
    //  * `payments` is now MONEY that moved: date, amount, method, reference, optional receipt.
    //  * `allocations` say how much of which payment settles which charge (partial payments, one transfer for
    //    several bills, instalments). A payment's unallocated remainder is the client's credit.
    //  * Receipts belong to money (payments). An issued receipt freezes its payments, their allocations and the
    //    money fields of the charges they settle.
    //  * Every previously paid line becomes one payment + one allocation; receipts move to those payments.
    if (UserVersion(db) < 9) {
        executeQuery("BEGIN;");
        bool v9ok = executeQuery(R"(
            DROP TRIGGER IF EXISTS trg_payments_receipted_update;
            DROP TRIGGER IF EXISTS trg_payments_receipted_delete;
            DROP TRIGGER IF EXISTS trg_payments_batch_fixed;
            DROP TRIGGER IF EXISTS audit_payments_ins;
            DROP TRIGGER IF EXISTS audit_payments_upd;
            DROP TRIGGER IF EXISTS audit_payments_del;
            DROP TRIGGER IF EXISTS trg_users_keep_payments;
            DROP TRIGGER IF EXISTS trg_apartments_keep_payments;
            ALTER TABLE payments RENAME TO charges;

            CREATE TABLE payments (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                user_id     INTEGER REFERENCES users(id),
                pay_date    TEXT    NOT NULL CHECK (pay_date IS date(pay_date)),
                amount      REAL    NOT NULL CHECK (amount > 0),
                direction   TEXT    NOT NULL DEFAULT 'in' CHECK (direction IN ('in','out')),
                method      TEXT    NOT NULL DEFAULT 'unspecified'
                            CHECK (method IN ('unspecified','cash','collector','bank','ebanking','card','iris','other')),
                reference   TEXT,
                notes       TEXT,
                receipt_id  INTEGER REFERENCES receipts(id),
                batch_id    INTEGER REFERENCES payment_batches(id),
                legacy_line INTEGER,
                created_at  TEXT    NOT NULL DEFAULT (datetime('now')) CHECK (created_at IS datetime(created_at))
            );
            CREATE TABLE allocations (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                payment_id  INTEGER NOT NULL REFERENCES payments(id),
                charge_id   INTEGER NOT NULL REFERENCES charges(id),
                amount      REAL    NOT NULL CHECK (amount > 0),
                created_at  TEXT    NOT NULL DEFAULT (datetime('now')),
                UNIQUE (payment_id, charge_id)
            );

            INSERT INTO payments (user_id, pay_date, amount, direction, method, reference, notes, receipt_id, batch_id, legacy_line, created_at)
            SELECT c.user_id, c.pay_date, c.amount_paid, c.direction,
                   COALESCE((SELECT r.method FROM receipts r WHERE r.id = c.receipt_id), 'unspecified'),
                   (SELECT r.bank_ref FROM receipts r WHERE r.id = c.receipt_id),
                   NULL, c.receipt_id, c.batch_id, c.id, COALESCE(c.resolved_at, c.created_at)
            FROM charges c WHERE c.status != 'future' AND c.amount_paid > 0 ORDER BY c.id;

            INSERT INTO allocations (payment_id, charge_id, amount, created_at)
            SELECT p.id, c.id, MIN(c.amount_paid, c.amount_due), p.created_at
            FROM payments p JOIN charges c ON c.id = p.legacy_line
            WHERE MIN(c.amount_paid, c.amount_due) > 0;

            UPDATE charges SET
                amount_paid = COALESCE((SELECT CAST(ROUND(SUM(a.amount) * 100) AS INTEGER) FROM allocations a WHERE a.charge_id = charges.id), 0) / 100.0;
            UPDATE charges SET balance = (CAST(ROUND(amount_due * 100) AS INTEGER) - CAST(ROUND(amount_paid * 100) AS INTEGER)) / 100.0;

            -- rebuild charges without the old receipt_id (receipts now belong to money); same ids, same rows
            CREATE TABLE charges_v9 (
                id              INTEGER PRIMARY KEY AUTOINCREMENT,
                service_id      INTEGER NOT NULL REFERENCES services(id),
                user_id         INTEGER REFERENCES users(id),
                apartment_id    INTEGER REFERENCES apartments(id),
                pay_date        TEXT    NOT NULL CHECK (pay_date IS date(pay_date)),
                amount_due      REAL    NOT NULL DEFAULT 0,
                amount_paid     REAL    NOT NULL DEFAULT 0,
                balance         REAL    NOT NULL DEFAULT 0,
                status          TEXT    NOT NULL DEFAULT 'paid',
                direction       TEXT    NOT NULL DEFAULT 'in' CHECK (direction IN ('in','out')),
                notes           TEXT,
                resolution_date TEXT CHECK (resolution_date IS NULL OR resolution_date IS date(resolution_date)),
                resolved_at     TEXT CHECK (resolved_at IS NULL OR resolved_at IS datetime(resolved_at)),
                created_at      TEXT NOT NULL DEFAULT (datetime('now')) CHECK (created_at IS datetime(created_at)),
                batch_id        INTEGER REFERENCES payment_batches(id)
            );
            INSERT INTO charges_v9 SELECT id, service_id, user_id, apartment_id, pay_date, amount_due, amount_paid, balance, status,
                   direction, notes, resolution_date, resolved_at, created_at, batch_id FROM charges ORDER BY id;
            DROP TABLE charges;
            ALTER TABLE charges_v9 RENAME TO charges;
            CREATE INDEX IF NOT EXISTS idx_charges_date      ON charges(pay_date, status, direction, amount_due, amount_paid, balance);
            CREATE INDEX IF NOT EXISTS idx_charges_user_date ON charges(user_id, pay_date);
            CREATE INDEX IF NOT EXISTS idx_charges_service   ON charges(service_id);
            CREATE INDEX IF NOT EXISTS idx_charges_apartment ON charges(apartment_id);
            CREATE INDEX IF NOT EXISTS idx_charges_batch     ON charges(batch_id);
            CREATE INDEX IF NOT EXISTS idx_charges_status    ON charges(status, pay_date);

            CREATE INDEX IF NOT EXISTS idx_money_user_date ON payments(user_id, pay_date);
            CREATE INDEX IF NOT EXISTS idx_money_receipt   ON payments(receipt_id);
            CREATE INDEX IF NOT EXISTS idx_money_date      ON payments(pay_date);
            CREATE INDEX IF NOT EXISTS idx_alloc_charge    ON allocations(charge_id);
            CREATE INDEX IF NOT EXISTS idx_alloc_payment   ON allocations(payment_id);

            -- charges.amount_paid / balance follow the allocations, in exact cents
            CREATE TRIGGER trg_alloc_sync_ins AFTER INSERT ON allocations BEGIN
                UPDATE charges SET amount_paid = (SELECT COALESCE(SUM(CAST(ROUND(amount*100) AS INTEGER)),0) FROM allocations WHERE charge_id = NEW.charge_id) / 100.0
                WHERE id = NEW.charge_id;
                UPDATE charges SET balance = (CAST(ROUND(amount_due*100) AS INTEGER) - CAST(ROUND(amount_paid*100) AS INTEGER)) / 100.0 WHERE id = NEW.charge_id;
            END;
            CREATE TRIGGER trg_alloc_sync_upd AFTER UPDATE ON allocations BEGIN
                UPDATE charges SET amount_paid = (SELECT COALESCE(SUM(CAST(ROUND(amount*100) AS INTEGER)),0) FROM allocations WHERE charge_id = charges.id) / 100.0
                WHERE id IN (NEW.charge_id, OLD.charge_id);
                UPDATE charges SET balance = (CAST(ROUND(amount_due*100) AS INTEGER) - CAST(ROUND(amount_paid*100) AS INTEGER)) / 100.0
                WHERE id IN (NEW.charge_id, OLD.charge_id);
            END;
            CREATE TRIGGER trg_alloc_sync_del AFTER DELETE ON allocations BEGIN
                UPDATE charges SET amount_paid = (SELECT COALESCE(SUM(CAST(ROUND(amount*100) AS INTEGER)),0) FROM allocations WHERE charge_id = OLD.charge_id) / 100.0
                WHERE id = OLD.charge_id;
                UPDATE charges SET balance = (CAST(ROUND(amount_due*100) AS INTEGER) - CAST(ROUND(amount_paid*100) AS INTEGER)) / 100.0 WHERE id = OLD.charge_id;
            END;
            -- balance also follows a change of the amount owed
            CREATE TRIGGER trg_charge_balance AFTER UPDATE OF amount_due ON charges BEGIN
                UPDATE charges SET balance = (CAST(ROUND(amount_due*100) AS INTEGER) - CAST(ROUND(amount_paid*100) AS INTEGER)) / 100.0 WHERE id = NEW.id;
            END;

            -- an allocation never exceeds what the payment brought or what the charge asks, and matches direction
            CREATE TRIGGER trg_alloc_check_ins BEFORE INSERT ON allocations BEGIN
                SELECT RAISE(ABORT, 'Η κατανομή ξεπερνά το ποσό της πληρωμής')
                WHERE (SELECT COALESCE(SUM(CAST(ROUND(amount*100) AS INTEGER)),0) FROM allocations WHERE payment_id = NEW.payment_id)
                      + CAST(ROUND(NEW.amount*100) AS INTEGER) > (SELECT CAST(ROUND(amount*100) AS INTEGER) FROM payments WHERE id = NEW.payment_id);
                SELECT RAISE(ABORT, 'Η κατανομή ξεπερνά το υπόλοιπο της χρέωσης')
                WHERE (SELECT COALESCE(SUM(CAST(ROUND(amount*100) AS INTEGER)),0) FROM allocations WHERE charge_id = NEW.charge_id)
                      + CAST(ROUND(NEW.amount*100) AS INTEGER) > (SELECT CAST(ROUND(amount_due*100) AS INTEGER) FROM charges WHERE id = NEW.charge_id);
                SELECT RAISE(ABORT, 'Η πληρωμή και η χρέωση πρέπει να έχουν την ίδια κατεύθυνση')
                WHERE (SELECT direction FROM payments WHERE id = NEW.payment_id) IS NOT (SELECT direction FROM charges WHERE id = NEW.charge_id);
                SELECT RAISE(ABORT, 'Η πληρωμή έχει απόδειξη - δεν αλλάζει η κατανομή της')
                WHERE (SELECT r.status FROM payments p JOIN receipts r ON r.id = p.receipt_id WHERE p.id = NEW.payment_id) = 'issued';
            END;
            CREATE TRIGGER trg_alloc_check_upd BEFORE UPDATE ON allocations BEGIN
                SELECT RAISE(ABORT, 'Η πληρωμή έχει απόδειξη - δεν αλλάζει η κατανομή της')
                WHERE (SELECT r.status FROM payments p JOIN receipts r ON r.id = p.receipt_id WHERE p.id = OLD.payment_id) = 'issued';
                SELECT RAISE(ABORT, 'Η κατανομή ξεπερνά το ποσό της πληρωμής')
                WHERE (SELECT COALESCE(SUM(CAST(ROUND(amount*100) AS INTEGER)),0) FROM allocations WHERE payment_id = NEW.payment_id AND id != OLD.id)
                      + CAST(ROUND(NEW.amount*100) AS INTEGER) > (SELECT CAST(ROUND(amount*100) AS INTEGER) FROM payments WHERE id = NEW.payment_id);
                SELECT RAISE(ABORT, 'Η κατανομή ξεπερνά το υπόλοιπο της χρέωσης')
                WHERE (SELECT COALESCE(SUM(CAST(ROUND(amount*100) AS INTEGER)),0) FROM allocations WHERE charge_id = NEW.charge_id AND id != OLD.id)
                      + CAST(ROUND(NEW.amount*100) AS INTEGER) > (SELECT CAST(ROUND(amount_due*100) AS INTEGER) FROM charges WHERE id = NEW.charge_id);
            END;
            CREATE TRIGGER trg_alloc_check_del BEFORE DELETE ON allocations BEGIN
                SELECT RAISE(ABORT, 'Η πληρωμή έχει απόδειξη - δεν αλλάζει η κατανομή της')
                WHERE (SELECT r.status FROM payments p JOIN receipts r ON r.id = p.receipt_id WHERE p.id = OLD.payment_id) = 'issued';
            END;

            -- money on an issued receipt is frozen
            CREATE TRIGGER trg_money_receipted_update BEFORE UPDATE ON payments
            WHEN OLD.receipt_id IS NOT NULL AND (SELECT status FROM receipts WHERE id = OLD.receipt_id) = 'issued'
             AND (NEW.amount IS NOT OLD.amount OR NEW.user_id IS NOT OLD.user_id OR NEW.pay_date IS NOT OLD.pay_date
                  OR NEW.direction IS NOT OLD.direction OR NEW.method IS NOT OLD.method OR NEW.receipt_id IS NOT OLD.receipt_id
                  OR NEW.batch_id IS NOT OLD.batch_id)
            BEGIN SELECT RAISE(ABORT, 'Η πληρωμή έχει απόδειξη - ακυρώστε πρώτα την απόδειξη για να την αλλάξετε'); END;
            CREATE TRIGGER trg_money_receipted_delete BEFORE DELETE ON payments
            WHEN OLD.receipt_id IS NOT NULL AND (SELECT status FROM receipts WHERE id = OLD.receipt_id) = 'issued'
            BEGIN SELECT RAISE(ABORT, 'Η πληρωμή έχει απόδειξη - ακυρώστε πρώτα την απόδειξη για να τη διαγράψετε'); END;
            CREATE TRIGGER trg_money_keep_allocated BEFORE DELETE ON payments
            WHEN EXISTS (SELECT 1 FROM allocations WHERE payment_id = OLD.id)
            BEGIN SELECT RAISE(ABORT, 'Η πληρωμή έχει κατανομές - αφαιρέστε τις πρώτα'); END;

            -- a charge settled (even partly) by receipted money keeps what was billed
            CREATE TRIGGER trg_charge_receipted_update BEFORE UPDATE OF amount_due, user_id, service_id, apartment_id, pay_date, direction, status ON charges
            WHEN EXISTS (SELECT 1 FROM allocations a JOIN payments p ON p.id = a.payment_id JOIN receipts r ON r.id = p.receipt_id
                         WHERE a.charge_id = OLD.id AND r.status = 'issued')
             AND (NEW.amount_due IS NOT OLD.amount_due OR NEW.user_id IS NOT OLD.user_id OR NEW.service_id IS NOT OLD.service_id
                  OR NEW.apartment_id IS NOT OLD.apartment_id OR NEW.pay_date IS NOT OLD.pay_date OR NEW.direction IS NOT OLD.direction
                  OR NEW.status IS NOT OLD.status)
            BEGIN SELECT RAISE(ABORT, 'Η χρέωση έχει εξοφληθεί με απόδειξη - ακυρώστε πρώτα την απόδειξη για να την αλλάξετε'); END;
            CREATE TRIGGER trg_charge_keep_allocated BEFORE DELETE ON charges
            WHEN EXISTS (SELECT 1 FROM allocations WHERE charge_id = OLD.id)
            BEGIN SELECT RAISE(ABORT, 'Η χρέωση έχει πληρωμές - αφαιρέστε τις πρώτα'); END;
            CREATE TRIGGER trg_charge_batch_fixed BEFORE UPDATE OF batch_id ON charges
            WHEN NEW.batch_id IS NOT OLD.batch_id
            BEGIN SELECT RAISE(ABORT, 'Η ομάδα μιας χρέωσης ορίζεται μόνο κατά τη δημιουργία της'); END;
            CREATE TRIGGER trg_money_batch_fixed BEFORE UPDATE OF batch_id ON payments
            WHEN NEW.batch_id IS NOT OLD.batch_id
            BEGIN SELECT RAISE(ABORT, 'Η ομάδα μιας πληρωμής ορίζεται μόνο κατά τη δημιουργία της'); END;

            CREATE TRIGGER trg_apartments_keep_payments BEFORE DELETE ON apartments
            WHEN EXISTS (SELECT 1 FROM charges WHERE apartment_id = OLD.id)
            BEGIN SELECT RAISE(ABORT, 'Το διαμέρισμα έχει πληρωμές στο ιστορικό του και δεν διαγράφεται'); END;
            CREATE TRIGGER trg_users_keep_payments BEFORE DELETE ON users
            WHEN EXISTS (SELECT 1 FROM charges WHERE user_id = OLD.id) OR EXISTS (SELECT 1 FROM payments WHERE user_id = OLD.id)
            BEGIN SELECT RAISE(ABORT, 'Ο πελάτης έχει πληρωμές στο ιστορικό του και δεν διαγράφεται'); END;
        )");
        // rows were reshaped: a new baseline generation for the integrity check, then this schema's triggers
        for (const char* t : kAuditedTables) {
            if (!v9ok || !TableExists(db, t)) continue;
            v9ok = executeQuery(std::string("INSERT INTO audit_log (table_name, record_id, action_type, old_data, new_data, category, actor, reason) "
                         "SELECT '") + t + "', rowid, 'BASELINE', NULL, " + JsonRowExpr(db, t, "") + ", 'db', 'migration v9', "
                         "'Διαχωρισμός χρεώσεων και πληρωμών' FROM " + t + ";");
        }
        if (v9ok) {
            ensureAuditTriggers();
            logAction("migration", 9, "SCHEMA_V9", "", "{\"charges\": true, \"payments\": \"money\", \"allocations\": true}", "db");
            v9ok = executeQuery("PRAGMA user_version = 9;") && executeQuery("COMMIT;");
        }
        if (!v9ok) {
            std::string why = lastError_;
            executeQuery("ROLLBACK;");
            lastError_ = "Η αναβάθμιση της βάσης (v9) απέτυχε και αναιρέθηκε, τα δεδομένα δεν άλλαξαν: " + why;
            std::cerr << lastError_ << "\n";
        }
    }


    // --- Migration v10: attachments (files live next to the database, the rows are here and audited). ---
    if (UserVersion(db) < 10) {
        bool ok = executeQuery(R"(
            BEGIN;
            CREATE TABLE IF NOT EXISTS attachments (
                id             INTEGER PRIMARY KEY AUTOINCREMENT,
                entity         TEXT    NOT NULL CHECK (entity IN ('receipt','payment','charge','client','building','apartment')),
                entity_id      INTEGER NOT NULL,
                sha256         TEXT    NOT NULL CHECK (length(sha256) = 64),
                file_name      TEXT    NOT NULL,
                kind           TEXT    NOT NULL CHECK (kind IN ('image','video','pdf','other')),
                size_bytes     INTEGER NOT NULL CHECK (size_bytes >= 0),
                stored_path    TEXT    NOT NULL,
                note           TEXT,
                created_at     TEXT    NOT NULL DEFAULT (datetime('now')),
                removed_at     TEXT,
                removed_reason TEXT
            );
            CREATE INDEX IF NOT EXISTS idx_attachments_entity ON attachments(entity, entity_id);
            CREATE TRIGGER IF NOT EXISTS trg_attachments_no_delete BEFORE DELETE ON attachments
            BEGIN SELECT RAISE(ABORT, 'Τα συνημμένα δεν διαγράφονται - αφαιρέστε τα με αιτιολογία'); END;
            CREATE TRIGGER IF NOT EXISTS trg_attachments_fixed BEFORE UPDATE ON attachments
            WHEN NEW.entity IS NOT OLD.entity OR NEW.entity_id IS NOT OLD.entity_id OR NEW.sha256 IS NOT OLD.sha256
              OR NEW.file_name IS NOT OLD.file_name OR NEW.kind IS NOT OLD.kind OR NEW.size_bytes IS NOT OLD.size_bytes
              OR NEW.stored_path IS NOT OLD.stored_path OR NEW.created_at IS NOT OLD.created_at
              OR OLD.removed_at IS NOT NULL
              OR (NEW.removed_at IS NOT NULL AND (OLD.entity = 'receipt' OR TRIM(COALESCE(NEW.removed_reason,'')) = ''))
            BEGIN SELECT RAISE(ABORT, 'Το συνημμένο δεν αλλάζει (αφαίρεση μόνο με αιτιολογία, ποτέ από απόδειξη)'); END;
        )");
        if (ok) { ensureAuditTriggers(); ok = executeQuery("PRAGMA user_version = 10; COMMIT;"); }
        if (!ok) { std::string why = lastError_; executeQuery("ROLLBACK;"); lastError_ = "Η αναβάθμιση της βάσης (v10) απέτυχε: " + why; }
    }

    // --- Migration v11: Κοινόχρηστα = a cost split across a collection of apartments. ---
    // joint_services: catalog (a services row + split kind). joint_ops: one charge of a cost to apartments;
    // joint_shares: every apartment/resident it considered, included with its amount or excluded with a
    // reason. The included shares' charges carry the op's batch_id and the share's apartment.
    // Ops and shares are a record of what was decided: never updated or deleted (fix the charges instead).
    if (UserVersion(db) < 11) {
        bool ok = executeQuery(R"(
            BEGIN;
            CREATE TABLE IF NOT EXISTS joint_services (
                id         INTEGER PRIMARY KEY AUTOINCREMENT,
                service_id INTEGER NOT NULL UNIQUE REFERENCES services(id),
                kind       TEXT    NOT NULL DEFAULT 'equal',
                active     INTEGER NOT NULL DEFAULT 1 CHECK (active IN (0,1)),
                created_at TEXT    NOT NULL DEFAULT (datetime('now'))
            );
            CREATE TABLE IF NOT EXISTS joint_ops (
                id          INTEGER PRIMARY KEY AUTOINCREMENT,
                building_id INTEGER REFERENCES buildings(id),
                source_type TEXT    NOT NULL CHECK (source_type IN ('joint','service','package')),
                source_id   INTEGER NOT NULL,
                kind        TEXT    NOT NULL DEFAULT 'equal',
                label       TEXT    NOT NULL,
                total       REAL    NOT NULL CHECK (total > 0),
                op_date     TEXT    NOT NULL CHECK (op_date IS date(op_date)),
                params_json TEXT    NOT NULL DEFAULT '{}' CHECK (json_valid(params_json)),
                note        TEXT,
                batch_id    INTEGER NOT NULL UNIQUE REFERENCES payment_batches(id),
                created_at  TEXT    NOT NULL DEFAULT (datetime('now'))
            );
            CREATE INDEX IF NOT EXISTS idx_joint_ops_building ON joint_ops(building_id, op_date);
            CREATE TABLE IF NOT EXISTS joint_shares (
                id             INTEGER PRIMARY KEY AUTOINCREMENT,
                op_id          INTEGER NOT NULL REFERENCES joint_ops(id),
                apartment_id   INTEGER NOT NULL REFERENCES apartments(id),
                user_id        INTEGER REFERENCES users(id),
                amount         REAL    NOT NULL DEFAULT 0,
                included       INTEGER NOT NULL CHECK (included IN (0,1)),
                exclude_reason TEXT,
                CHECK (included = 0 OR (amount > 0 AND user_id IS NOT NULL)),
                CHECK (included = 1 OR TRIM(COALESCE(exclude_reason,'')) <> '')
            );
            CREATE INDEX IF NOT EXISTS idx_joint_shares_op ON joint_shares(op_id);
            CREATE INDEX IF NOT EXISTS idx_joint_shares_apt ON joint_shares(apartment_id);
            CREATE TRIGGER IF NOT EXISTS trg_joint_ops_fixed BEFORE UPDATE ON joint_ops
            BEGIN SELECT RAISE(ABORT, 'Η κοινόχρηστη χρέωση δεν αλλάζει - διορθώστε τις χρεώσεις των ενοίκων'); END;
            CREATE TRIGGER IF NOT EXISTS trg_joint_ops_no_delete BEFORE DELETE ON joint_ops
            BEGIN SELECT RAISE(ABORT, 'Η κοινόχρηστη χρέωση δεν διαγράφεται'); END;
            CREATE TRIGGER IF NOT EXISTS trg_joint_shares_fixed BEFORE UPDATE ON joint_shares
            BEGIN SELECT RAISE(ABORT, 'Η κατανομή κοινοχρήστων δεν αλλάζει'); END;
            CREATE TRIGGER IF NOT EXISTS trg_joint_shares_no_delete BEFORE DELETE ON joint_shares
            BEGIN SELECT RAISE(ABORT, 'Η κατανομή κοινοχρήστων δεν διαγράφεται'); END;
        )");
        if (ok) {
            ensureAuditTriggers();
            // the old "joint" services (one service shared by several clients) join the catalog; their past
            // charges stay as they are
            ok = executeQuery(R"(
                INSERT OR IGNORE INTO joint_services (service_id, kind)
                SELECT s.id, CASE WHEN s.label LIKE '%ΠΕΤΡΕΛ%' OR s.label LIKE '%πετρελ%' THEN 'heating_oil' ELSE 'equal' END
                FROM services s WHERE (SELECT COUNT(*) FROM service_users su WHERE su.service_id = s.id) > 1;
                PRAGMA user_version = 11; COMMIT;)");
        }
        if (!ok) { std::string why = lastError_; executeQuery("ROLLBACK;"); lastError_ = "Η αναβάθμιση της βάσης (v11) απέτυχε: " + why; }
    }

    // --- Migration v12: installment plans (δόσεις). One row per plan; its charges are the plan's batch. ---
    if (UserVersion(db) < 12) {
        bool ok = executeQuery(R"(
            BEGIN;
            CREATE TABLE IF NOT EXISTS payment_plans (
                id           INTEGER PRIMARY KEY AUTOINCREMENT,
                batch_id     INTEGER NOT NULL UNIQUE REFERENCES payment_batches(id),
                installments INTEGER NOT NULL CHECK (installments >= 2),
                every        TEXT    NOT NULL CHECK (every IN ('month','week')),
                receipt_mode TEXT    NOT NULL CHECK (receipt_mode IN ('end','each')),
                total        REAL    NOT NULL CHECK (total > 0),
                created_at   TEXT    NOT NULL DEFAULT (datetime('now'))
            );
            CREATE TRIGGER IF NOT EXISTS trg_payment_plans_fixed BEFORE UPDATE ON payment_plans
            BEGIN SELECT RAISE(ABORT, 'Το πρόγραμμα δόσεων δεν αλλάζει - διορθώστε τις δόσεις'); END;
            CREATE TRIGGER IF NOT EXISTS trg_payment_plans_no_delete BEFORE DELETE ON payment_plans
            BEGIN SELECT RAISE(ABORT, 'Το πρόγραμμα δόσεων δεν διαγράφεται'); END;
        )");
        if (ok) { ensureAuditTriggers(); ok = executeQuery("PRAGMA user_version = 12; COMMIT;"); }
        if (!ok) { std::string why = lastError_; executeQuery("ROLLBACK;"); lastError_ = "Η αναβάθμιση της βάσης (v12) απέτυχε: " + why; }
    }

    // --- Migration v13: offers (discounts) and which charges used them. ---
    if (UserVersion(db) < 13) {
        bool ok = executeQuery(R"(
            BEGIN;
            CREATE TABLE IF NOT EXISTS offers (
                id            INTEGER PRIMARY KEY AUTOINCREMENT,
                name          TEXT    NOT NULL CHECK (TRIM(name) <> ''),
                kind          TEXT    NOT NULL CHECK (kind IN ('percent','fixed')),
                value         REAL    NOT NULL CHECK (value > 0 AND (kind = 'fixed' OR value <= 100)),
                user_id       INTEGER REFERENCES users(id),
                target_type   TEXT    NOT NULL CHECK (target_type IN ('all','service','package')),
                target_id     INTEGER,
                valid_from    TEXT    NOT NULL CHECK (valid_from IS date(valid_from)),
                valid_to      TEXT    NOT NULL CHECK (valid_to IS date(valid_to) AND valid_to >= valid_from),
                status        TEXT    NOT NULL DEFAULT 'active' CHECK (status IN ('active','cancelled')),
                note          TEXT,
                cancel_reason TEXT,
                created_at    TEXT    NOT NULL DEFAULT (datetime('now')),
                CHECK (target_type = 'all' OR target_id IS NOT NULL),
                CHECK (status = 'active' OR TRIM(COALESCE(cancel_reason,'')) <> '')
            );
            CREATE TRIGGER IF NOT EXISTS trg_offers_no_delete BEFORE DELETE ON offers
            BEGIN SELECT RAISE(ABORT, 'Οι προσφορές δεν διαγράφονται - ακυρώνονται με αιτιολογία'); END;
            CREATE TRIGGER IF NOT EXISTS trg_offers_cancelled_fixed BEFORE UPDATE ON offers WHEN OLD.status = 'cancelled'
            BEGIN SELECT RAISE(ABORT, 'Η προσφορά έχει ακυρωθεί και δεν αλλάζει'); END;
            CREATE TABLE IF NOT EXISTS offer_uses (
                id         INTEGER PRIMARY KEY AUTOINCREMENT,
                offer_id   INTEGER NOT NULL REFERENCES offers(id),
                charge_id  INTEGER NOT NULL,
                list_price REAL    NOT NULL CHECK (list_price >= 0),
                discount   REAL    NOT NULL CHECK (discount > 0 AND discount <= list_price),
                created_at TEXT    NOT NULL DEFAULT (datetime('now'))
            );
            CREATE INDEX IF NOT EXISTS idx_offer_uses_offer ON offer_uses(offer_id);
            CREATE INDEX IF NOT EXISTS idx_offer_uses_charge ON offer_uses(charge_id);
            CREATE TRIGGER IF NOT EXISTS trg_offer_uses_fixed BEFORE UPDATE ON offer_uses
            BEGIN SELECT RAISE(ABORT, 'Η χρήση προσφοράς δεν αλλάζει'); END;
            CREATE TRIGGER IF NOT EXISTS trg_offer_uses_no_delete BEFORE DELETE ON offer_uses
            BEGIN SELECT RAISE(ABORT, 'Η χρήση προσφοράς δεν διαγράφεται'); END;
        )");
        if (ok) { ensureAuditTriggers(); ok = executeQuery("PRAGMA user_version = 13; COMMIT;"); }
        if (!ok) { std::string why = lastError_; executeQuery("ROLLBACK;"); lastError_ = "Η αναβάθμιση της βάσης (v13) απέτυχε: " + why; }
    }

    executeQuery("CREATE INDEX IF NOT EXISTS idx_joint_shares_user ON joint_shares(user_id);");

    // Audit triggers must exist and be exactly ours. After a schema migration they are regenerated as part of
    // it; on a plain start any difference means someone dropped or altered them -> restore, record it, warn.
    {
        bool migratedNow = versionAtOpen < 13;
        int restored = ensureAuditTriggers();
        if (restored > 0 && !migratedNow) {
            logAction("audit", 0, "TRIGGERS_RESTORED", "", "{\"count\": " + std::to_string(restored) + "}", "db");
            lastError_ = "Ο μηχανισμός καταγραφής ιστορικού είχε αφαιρεθεί ή αλλοιωθεί και αποκαταστάθηκε (" +
                         std::to_string(restored) + "). Ελέγξτε το ιστορικό αλλαγών.";
        }
        // Startup stays fast at any size: the chain since the last daily checkpoint is verified now; the full
        // check (every entry + every live row) runs on its own read-only connection in the background and
        // reports through the same error banner when it finds something.
        std::string quick = QuickAuditCheck(db, dbPath_);
        if (!quick.empty()) lastError_ = "Έλεγχος ακεραιότητας ιστορικού: " + quick;
        writeAuditAnchor();
        std::string pathCopy = dbPath_;
        bgVerify_ = std::thread([this, pathCopy] {
            sqlite3* ro = nullptr;
            if (sqlite3_open_v2(pathCopy.c_str(), &ro, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) { sqlite3_close(ro); return; }
            sqlite3_busy_timeout(ro, 5000);
            bgDb_.store(ro);
            AuditCheck chk = VerifyAudit(ro, pathCopy);
            bgDb_.store(nullptr);
            sqlite3_close(ro);
            if (!chk.ok) {
                std::lock_guard<std::mutex> lk(bgMutex_);
                bgMessage_ = "Έλεγχος ακεραιότητας ιστορικού: " + chk.problems.front() +
                             (chk.problems.size() > 1 ? " (+" + std::to_string(chk.problems.size() - 1) + " ακόμη)" : "");
            }
        });
    }

    greekHeaders["users"] = {"ΚΩΔ", "ΟΝΟΜΑ", "ΤΗΛ", "ΗΜ/ΝΙΑ ΔΗΜ.", "ΕΙΔ. ΚΩΔ.", "ΚΩΔ ΔΙΑΜΕΡΙΣΜΑΤΟΣ"};
    greekHeaders["buildings"] = {"ΚΩΔ", "ΔΙΕΥΘΥΝΣΗ", "ΠΕΡΙΟΧΗ", "ΤΚ", "ΑΦΜ", "ΗΜ/ΝΙΑ ΔΗΜ."};
    greekHeaders["apartments"] = {"ΚΩΔ", "ΚΩΔ ΚΤΙΡΙΟΥ", "ΔΙΑΜΕΡΙΣΜΑ"};
    greekHeaders["services"] = {"ΚΩΔ ΥΠΗΡ.", "ΠΕΡΙΓΡΑΦΗ", "ΣΗΜΕΙΩΣΕΙΣ", "ΗΜ/ΝΙΑ ΔΗΜ."};
    greekHeaders["service_users"] = {"ΚΩΔ ΥΠΗΡ.", "ΚΩΔ ΧΡΗΣΤΗ"};
    greekHeaders["charges"] = {"ΚΩΔ", "ΚΩΔ ΥΠΗΡ.", "ΚΩΔ ΧΡΗΣΤΗ", "ΚΩΔ ΔΙΑΜ.", "ΗΜ/ΝΙΑ", "ΟΦΕΙΛΗ", "ΠΛΗΡΩΘΗΚΑΝ", "ΥΠΟΛΟΙΠΟ",
                               "ΚΑΤΑΣΤΑΣΗ", "ΤΥΠΟΣ", "ΣΧΟΛΙΑ", "ΗΜ/ΝΙΑ ΕΠΙΛΥΣΗΣ", "ΕΠΙΛΥΘΗΚΕ (UTC)", "ΚΑΤΑΧΩΡΗΘΗΚΕ (UTC)", "ΟΜΑΔΑ"};
    greekHeaders["payments"] = {"ΚΩΔ", "ΚΩΔ ΧΡΗΣΤΗ", "ΗΜ/ΝΙΑ", "ΠΟΣΟ", "ΤΥΠΟΣ", "ΤΡΟΠΟΣ", "ΑΝΑΦΟΡΑ", "ΣΧΟΛΙΑ", "ΚΩΔ ΑΠΟΔΕΙΞΗΣ",
                                "ΟΜΑΔΑ", "ΑΡΧΙΚΗ ΓΡΑΜΜΗ", "ΚΑΤΑΧΩΡΗΘΗΚΕ (UTC)"};
    greekHeaders["allocations"] = {"ΚΩΔ", "ΚΩΔ ΠΛΗΡΩΜΗΣ", "ΚΩΔ ΧΡΕΩΣΗΣ", "ΠΟΣΟ", "ΚΑΤΑΧΩΡΗΘΗΚΕ (UTC)"};
    greekHeaders["master_services"] = {"ΚΩΔ", "ΕΤΙΚΕΤΑ", "ΗΜ/ΝΙΑ ΔΗΜ."};
    greekHeaders["master_service_components"] = {"ΚΩΔ ΠΑΚΕΤΟΥ", "ΚΩΔ ΥΠΗΡ."};
    greekHeaders["receipts"] = {"ΚΩΔ", "ΣΕΙΡΑ", "ΑΡΙΘΜΟΣ", "ΗΜ/ΝΙΑ ΕΚΔ.", "ΚΩΔ ΧΡΗΣΤΗ", "ΚΩΔ ΚΤΙΡΙΟΥ", "ΣΥΝΟΛΟ",
                                "ΤΡΟΠΟΣ", "ΚΩΔ. RF", "ΑΡ. ΧΑΡΤΙΝΗΣ", "ΤΡΑΠ. ΑΝΑΦΟΡΑ", "ΣΗΜΕΙΩΣΕΙΣ", "ΚΑΤΑΣΤΑΣΗ",
                                "ΑΙΤΙΑ ΑΚΥΡΩΣΗΣ", "ΑΚΥΡΩΘΗΚΕ ΣΤΙΣ", "ΗΜ/ΝΙΑ ΔΗΜ."};
    greekHeaders["notes"] = {"ΚΩΔ", "ΤΙΤΛΟΣ", "ΠΕΡΙΕΧΟΜΕΝΟ", "ΗΜ/ΝΙΑ ΔΗΜ."};
}

SqliteConn::~SqliteConn() {
    if (sqlite3* ro = bgDb_.load()) sqlite3_interrupt(ro);     // stop a background check still running
    if (bgVerify_.joinable()) bgVerify_.join();
    if (db) sqlite3_close(db);
}

double SqliteConn::priceOf(int serviceId) {
    sqlite3_stmt* st = nullptr; double v = 0.0;
    sqlite3_prepare_v2(db, "SELECT COALESCE(suggested_price,0) FROM services WHERE id=?", -1, &st, nullptr);
    sqlite3_bind_int(st, 1, serviceId);
    if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_double(st, 0);
    sqlite3_finalize(st);
    return v;
}

bool SqliteConn::executeQuery(const std::string& sql) {
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errMsg);
    if(rc != SQLITE_OK) { std::cerr << "SQL Error: " << errMsg << std::endl; lastError_ = errMsg ? errMsg : "SQL error"; sqlite3_free(errMsg); return false; }
    return true;
}

void SqliteConn::logAction(const std::string& table, int recordId, const std::string& action, const std::string& oldData, const std::string& newData, const std::string& category) {
    std::string sql = "INSERT INTO audit_log (table_name, record_id, action_type, old_data, new_data, category, actor, reason) "
                      "VALUES (?, ?, ?, ?, ?, ?, app_actor(), app_reason())";
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
// Every read goes through kUserSelect / MapUserRow (top of file), so a client's
// address / area / ΤΚ / apartment are always the ones DERIVED from their apartment
// (users -> apartments -> buildings). Writes only touch what is actually stored on
// the users table: name, phone, special code, and the apartment they point to.

std::vector<User> SqliteConn::searchUsers(const std::string& query, int offset, int limit) {
    std::vector<User> results;
    // Free-text: what identifies a client AND where they live (location columns
    // belong to the building/apartment now, reached through the join).
    std::string sql = std::string(kUserSelect) +
        "WHERE u.full_name LIKE '%' || ?1 || '%' OR u.phone LIKE '%' || ?1 || '%' "
        "   OR u.special_code LIKE '%' || ?1 || '%' OR a.label LIKE '%' || ?1 || '%' "
        "   OR b.address LIKE '%' || ?1 || '%' OR b.area LIKE '%' || ?1 || '%' "
        "   OR b.postal_code LIKE '%' || ?1 || '%' "
        "ORDER BY u.id LIMIT ?2 OFFSET ?3";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, query.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);
    sqlite3_bind_int(stmt, 3, offset);
    while (sqlite3_step(stmt) == SQLITE_ROW) results.push_back(MapUserRow(stmt));
    sqlite3_finalize(stmt);
    return results;
}

// Multi-field AND search for the Αναζήτηση tab: each NON-EMPTY field of `criteria`
// becomes one AND-ed case-insensitive LIKE clause (LOWER() on both sides, matching
// how the rest of the app treats Greek text -- SQLite's built-in LIKE case-folding
// is ASCII-only). address/area/postal_code/apartment are matched against the
// client's building/apartment through the join. Parameterized: every user-supplied
// value is bound via '?', never concatenated into the SQL text.
std::vector<User> SqliteConn::advancedSearchUsers(const User& criteria, const std::string& idQuery) {
    std::vector<User> results;
    std::string sql = std::string(kUserSelect) + "WHERE 1=1";
    std::vector<std::string> binds;
    auto addClause = [&](const char* col, const std::string& val) {
        if (val.empty()) return;
        sql += std::string(" AND LOWER(") + col + ") LIKE '%' || LOWER(?) || '%'";
        binds.push_back(val);
    };
    addClause("CAST(u.id AS TEXT)", idQuery);
    addClause("u.full_name", criteria.full_name);
    addClause("u.phone", criteria.phone);
    addClause("b.address", criteria.address);
    addClause("b.area", criteria.area);
    addClause("b.postal_code", criteria.postal_code);
    addClause("a.label", criteria.apartment);
    addClause("u.special_code", criteria.special_code);
    sql += " ORDER BY u.full_name LIMIT 2000";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    for (size_t i = 0; i < binds.size(); i++) sqlite3_bind_text(stmt, (int)i + 1, binds[i].c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) results.push_back(MapUserRow(stmt));
    sqlite3_finalize(stmt);
    return results;
}

static void BindApartment(sqlite3_stmt* stmt, int idx, int apartmentId) {
    if (apartmentId < 0) sqlite3_bind_null(stmt, idx);
    else sqlite3_bind_int(stmt, idx, apartmentId);
}

bool SqliteConn::addUser(const User& u) {
    std::string sql = "INSERT INTO users (full_name, phone, special_code, apartment_id) VALUES (?,?,?,?)";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, u.full_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, u.phone.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, u.special_code.c_str(), -1, SQLITE_TRANSIENT);
    BindApartment(stmt, 4, u.apartment_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) {
        int newId = (int)sqlite3_last_insert_rowid(db);
        if (u.apartment_id > 0)
            executeQuery("INSERT INTO residencies (user_id, apartment_id, from_date) VALUES (" + std::to_string(newId) + "," + std::to_string(u.apartment_id) + ", date('now','localtime'));");
        return true;
    }
    return false;
}

bool SqliteConn::editUser(int id, const User& u) {
    User old = getUserById(id);
    std::string sql = "UPDATE users SET full_name=?, phone=?, special_code=?, apartment_id=? WHERE id=?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, u.full_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, u.phone.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, u.special_code.c_str(), -1, SQLITE_TRANSIENT);
    BindApartment(stmt, 4, u.apartment_id);
    sqlite3_bind_int(stmt, 5, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) {
        if (old.apartment_id != u.apartment_id) {          // moved: close the old stay, open the new one
            executeQuery("UPDATE residencies SET to_date = date('now','localtime') WHERE user_id = " + std::to_string(id) + " AND to_date IS NULL;");
            if (u.apartment_id > 0)
                executeQuery("INSERT INTO residencies (user_id, apartment_id, from_date) VALUES (" + std::to_string(id) + "," + std::to_string(u.apartment_id) + ", date('now','localtime'));");
        }
         return true;
    }
    return false;
}

bool SqliteConn::removeUser(int id) {
    // Cascade warning is handled in UI, we just delete.
    std::string sql = "DELETE FROM users WHERE id=?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) lastError_ = sqlite3_errmsg(db);   // refused: the client has payments (history)
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) { executeQuery("DELETE FROM residencies WHERE user_id = " + std::to_string(id) + ";");  return true; }
    return false;
}

User SqliteConn::getUserById(int id) {
    User u;
    std::string sql = std::string(kUserSelect) + "WHERE u.id=?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    if (sqlite3_step(stmt) == SQLITE_ROW) u = MapUserRow(stmt);
    sqlite3_finalize(stmt);
    return u;
}

// --- BUILDINGS & APARTMENTS ---
std::vector<Building> SqliteConn::getBuildings() {
    std::vector<Building> out;
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, "SELECT id, address, area, postal_code, afm, created_at FROM buildings ORDER BY address, area, id", -1, &stmt, nullptr);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Building b;
        b.id = sqlite3_column_int(stmt, 0);
        b.address = ColText(stmt, 1);
        b.area = ColText(stmt, 2);
        b.postal_code = ColText(stmt, 3);
        b.afm = ColText(stmt, 4);
        b.created_at = ColText(stmt, 5);
        out.push_back(b);
    }
    sqlite3_finalize(stmt);
    return out;
}

Building SqliteConn::getBuildingById(int id) {
    Building b;
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, "SELECT id, address, area, postal_code, afm, created_at FROM buildings WHERE id=?", -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        b.id = sqlite3_column_int(stmt, 0);
        b.address = ColText(stmt, 1);
        b.area = ColText(stmt, 2);
        b.postal_code = ColText(stmt, 3);
        b.afm = ColText(stmt, 4);
        b.created_at = ColText(stmt, 5);
    }
    sqlite3_finalize(stmt);
    return b;
}

static std::string BuildingToJson(const Building& b) {
    return "{ \"id\": " + std::to_string(b.id) + ", \"address\": \"" + b.address + "\", \"area\": \"" + b.area +
           "\", \"postal_code\": \"" + b.postal_code + "\", \"afm\": \"" + b.afm + "\" }";
}

int SqliteConn::addBuilding(const Building& b) {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, "INSERT INTO buildings (address, area, postal_code, afm) VALUES (?,?,?,?)", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, b.address.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, b.area.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, b.postal_code.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, b.afm.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    int id = (int)sqlite3_last_insert_rowid(db);
    return id;
}

bool SqliteConn::editBuilding(int id, const Building& b) {
    Building old = getBuildingById(id);
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, "UPDATE buildings SET address=?, area=?, postal_code=?, afm=? WHERE id=?", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, b.address.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, b.area.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, b.postal_code.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, b.afm.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return false;
    return true;
}

// FK cascades are not enforced in this app, so the cleanup is explicit and runs in
// one transaction: residents of this building's apartments are UNASSIGNED (never
// deleted), then its apartments, then the building itself.
bool SqliteConn::removeBuilding(int id) {
    Building old = getBuildingById(id);
    char* err = nullptr;
    std::string sid = std::to_string(id);
    std::string sql =
        "BEGIN;"
        "UPDATE users SET apartment_id = NULL WHERE apartment_id IN (SELECT id FROM apartments WHERE building_id = " + sid + ");"
        "DELETE FROM residencies WHERE apartment_id IN (SELECT id FROM apartments WHERE building_id = " + sid + ");"
        "DELETE FROM apartments WHERE building_id = " + sid + ";"
        "DELETE FROM buildings WHERE id = " + sid + ";"
        "COMMIT;";
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "removeBuilding failed: " << (err ? err : "?") << std::endl;
        lastError_ = err ? err : "removeBuilding failed";
        sqlite3_free(err);
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    return true;
}

std::vector<Apartment> SqliteConn::getApartmentsForBuilding(int buildingId) {
    std::vector<Apartment> out;
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, "SELECT id, building_id, label FROM apartments WHERE building_id=? ORDER BY id", -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, buildingId);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Apartment a;
        a.id = sqlite3_column_int(stmt, 0);
        a.building_id = sqlite3_column_int(stmt, 1);
        a.label = ColText(stmt, 2);
        out.push_back(a);
    }
    sqlite3_finalize(stmt);
    return out;
}

int SqliteConn::addApartment(int buildingId, const std::string& label) {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, "INSERT INTO apartments (building_id, label) VALUES (?,?)", -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, buildingId);
    sqlite3_bind_text(stmt, 2, label.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    int id = (int)sqlite3_last_insert_rowid(db);
    return id;
}

bool SqliteConn::editApartment(int id, const std::string& label) {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, "UPDATE apartments SET label=? WHERE id=?", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, label.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return false;
    return true;
}

// Residents are unassigned, not deleted (see removeBuilding).
bool SqliteConn::removeApartment(int id) {
    char* err = nullptr;
    std::string sid = std::to_string(id);
    std::string sql =
        "BEGIN;"
        "UPDATE users SET apartment_id = NULL WHERE apartment_id = " + sid + ";"
        "DELETE FROM residencies WHERE apartment_id = " + sid + ";"
        "DELETE FROM apartments WHERE id = " + sid + ";"
        "COMMIT;";
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "removeApartment failed: " << (err ? err : "?") << std::endl;
        lastError_ = err ? err : "removeApartment failed";
        sqlite3_free(err);
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    return true;
}

// --- APARTMENT HISTORY ---
int SqliteConn::apartmentAt(int userId, int y, int m, int d) {
    if (userId <= 0) return -1;
    char date[16]; std::snprintf(date, sizeof(date), "%04d-%02d-%02d", y, m, d);
    int apt = -1;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "SELECT apartment_id FROM residencies WHERE user_id=?1 AND from_date<=?2 AND (to_date IS NULL OR to_date>=?2) "
                           "ORDER BY from_date DESC, id DESC LIMIT 1", -1, &st, nullptr);
    sqlite3_bind_int(st, 1, userId); sqlite3_bind_text(st, 2, date, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) apt = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    if (apt > 0) return apt;
    // No stay covers the date. Before the client's FIRST recorded stay (e.g. older payments of a client added today):
    // the first stay's apartment. Between two stays or after the last one ended: no apartment (-1), so a charge is
    // never put in an apartment the client did not live in at that date. No history at all: where they live now.
    int first = -1; std::string firstFrom; bool any = false;
    sqlite3_prepare_v2(db, "SELECT apartment_id, from_date FROM residencies WHERE user_id=? ORDER BY from_date, id LIMIT 1", -1, &st, nullptr);
    sqlite3_bind_int(st, 1, userId);
    if (sqlite3_step(st) == SQLITE_ROW) { any = true; first = sqlite3_column_int(st, 0); firstFrom = ColText(st, 1); }
    sqlite3_finalize(st);
    if (any) return std::string(date) < firstFrom ? first : -1;
    sqlite3_prepare_v2(db, "SELECT apartment_id FROM users WHERE id=?", -1, &st, nullptr);
    sqlite3_bind_int(st, 1, userId);
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL) apt = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return apt;
}

static std::vector<Residency> ReadResidencies(sqlite3* db, const std::string& where, int arg) {
    std::vector<Residency> out;
    sqlite3_stmt* st = nullptr;
    std::string sql = "SELECT id, user_id, apartment_id, from_date, COALESCE(to_date,'') FROM residencies WHERE " + where + " ORDER BY from_date, id";
    sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr);
    sqlite3_bind_int(st, 1, arg);
    while (sqlite3_step(st) == SQLITE_ROW) {
        Residency r;
        r.id = sqlite3_column_int(st, 0); r.user_id = sqlite3_column_int(st, 1); r.apartment_id = sqlite3_column_int(st, 2);
        r.from_date = ColText(st, 3); r.to_date = ColText(st, 4);
        out.push_back(r);
    }
    sqlite3_finalize(st);
    return out;
}
std::vector<Residency> SqliteConn::getResidencies(int apartmentId) { return ReadResidencies(db, "apartment_id=?", apartmentId); }
std::vector<Residency> SqliteConn::getUserResidencies(int userId)  { return ReadResidencies(db, "user_id=?", userId); }

bool SqliteConn::setResidencyHistory(int userId, const std::vector<Residency>& history) {
    char* err = nullptr;
    std::string uid = std::to_string(userId);
    std::string sql = "BEGIN; DELETE FROM residencies WHERE user_id = " + uid + ";";
    int currentApt = -1;
    for (auto& r : history) {
        std::string from = r.from_date.empty() ? "1970-01-01" : r.from_date;
        sql += "INSERT INTO residencies (user_id, apartment_id, from_date, to_date) VALUES (" + uid + "," + std::to_string(r.apartment_id) + ",'" + from + "',"
             + (r.to_date.empty() ? std::string("NULL") : "'" + r.to_date + "'") + ");";
        if (r.to_date.empty()) currentApt = r.apartment_id;
    }
    sql += "UPDATE users SET apartment_id = " + (currentApt > 0 ? std::to_string(currentApt) : std::string("NULL")) + " WHERE id = " + uid + ";";
    sql += "UPDATE charges SET apartment_id = COALESCE(("
           "SELECT r.apartment_id FROM residencies r WHERE r.user_id = charges.user_id "
           "AND r.from_date <= charges.pay_date "
           "AND (r.to_date IS NULL OR r.to_date >= charges.pay_date) "
           "ORDER BY r.from_date DESC, r.id DESC LIMIT 1), apartment_id) WHERE user_id = " + uid +
           // receipted payments keep the apartment printed on their receipt
           " AND NOT EXISTS (SELECT 1 FROM allocations a JOIN payments m ON m.id = a.payment_id JOIN receipts rc ON rc.id = m.receipt_id "
           "                 WHERE a.charge_id = charges.id AND rc.status = 'issued');COMMIT;";
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        lastError_ = err ? err : "setResidencyHistory failed";
        sqlite3_free(err);
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    return true;
}

std::vector<Payment> SqliteConn::getPaymentsForApartment(int apartmentId) {
    return QueryPayments(db, std::string("SELECT ") + kPaymentCols + " FROM charges p WHERE p.apartment_id = ? ORDER BY p.pay_date, p.id", {apartmentId});
}

std::string SqliteConn::takeLastError() {
    std::string e; e.swap(lastError_);
    if (e.empty()) { std::lock_guard<std::mutex> lk(bgMutex_); e.swap(bgMessage_); }
    if (!e.empty()) errlog::Write("database", e);
    return e;
}

std::string SqliteConn::errorLogPath() { return errlog::Path(); }

void SqliteConn::autoBackup(int keep) {
    namespace fs = std::filesystem;
    try {
        fs::path dbp(dbPath_);
        if (!fs::exists(dbp) || fs::file_size(dbp) == 0) return;
        fs::path dir = dbp.parent_path() / "backups";
        fs::create_directories(dir);
        auto t = std::time(nullptr); std::tm tmv{}; localtime_s(&tmv, &t);
        char name[48]; std::strftime(name, sizeof(name), "%Y%m%d", &tmv);
        fs::path target = dir / (dbp.stem().string() + "_" + name + ".db");
        if (!fs::exists(target)) {
            std::string p = target.generic_string();
            for (size_t i = 0; (i = p.find('\'', i)) != std::string::npos; i += 2) p.insert(i, "'");
            executeQuery("VACUUM INTO '" + p + "';");
        }
        std::vector<fs::path> mine;
        for (auto& e : fs::directory_iterator(dir))
            if (e.path().extension() == ".db" && e.path().filename().string().rfind(dbp.stem().string() + "_", 0) == 0) mine.push_back(e.path());
        std::sort(mine.begin(), mine.end());
        while ((int)mine.size() > keep) { fs::remove(mine.front()); mine.erase(mine.begin()); }
    } catch (const std::exception& e) { lastError_ = std::string("Αντίγραφο ασφαλείας: ") + e.what(); }
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
        s.suggested_price = priceOf(s.id);
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
        s.suggested_price = priceOf(s.id);
        results.push_back(s);
    }
    sqlite3_finalize(stmt);
    return results;
}

bool SqliteConn::addService(int userId, const Service& s) {
    return addServiceGetId(userId, s) >= 0;
}

int SqliteConn::addServiceGetId(int userId, const Service& in) {
    Service s = in; s.suggested_price = RoundMoney(s.suggested_price);
    if (s.suggested_price <= 0.0) return -1;   // every service must carry a price
    std::string sql = "INSERT INTO services (label, extra_notes, suggested_price) VALUES (?,?,?)";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_text(stmt,1,s.label.c_str(),-1,SQLITE_STATIC);
    sqlite3_bind_text(stmt,2,s.extra_notes.c_str(),-1,SQLITE_STATIC);
    sqlite3_bind_double(stmt,3,s.suggested_price);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if(rc != SQLITE_DONE) return -1;
    int id = sqlite3_last_insert_rowid(db);
    s.id = id;
    if (userId >= 0 && !addServiceUser(id, userId)) return -1;   // userId < 0: independent service, no roster row
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
    if(rc == SQLITE_DONE) {  return true; }
    return false;
}

std::vector<User> SqliteConn::getUsersForService(int serviceId) {
    std::vector<User> results;
    std::string sql = std::string(kUserSelect) +
        "JOIN service_users su ON su.user_id = u.id WHERE su.service_id=? ORDER BY u.full_name";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, serviceId);
    while (sqlite3_step(stmt) == SQLITE_ROW) results.push_back(MapUserRow(stmt));
    sqlite3_finalize(stmt);
    return results;
}

bool SqliteConn::editService(int id, const Service& in) {
    Service s = in; s.suggested_price = RoundMoney(s.suggested_price);
    if (s.suggested_price <= 0.0) return false;
    Service old = getServiceById(id);
    std::string sql = "UPDATE services SET label=?, extra_notes=?, suggested_price=? WHERE id=?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_text(stmt,1,s.label.c_str(),-1,SQLITE_STATIC);
    sqlite3_bind_text(stmt,2,s.extra_notes.c_str(),-1,SQLITE_STATIC);
    sqlite3_bind_double(stmt,3,s.suggested_price);
    sqlite3_bind_int(stmt,4,id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if(rc == SQLITE_DONE) {  return true; }
    return false;
}

bool SqliteConn::removeService(int id) {
    std::string sql = "DELETE FROM services WHERE id=?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_int(stmt,1,id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if(rc == SQLITE_DONE) {  return true; }
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
        s.suggested_price = priceOf(s.id);
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
        s.suggested_price = priceOf(s.id);
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
        s.suggested_price = priceOf(s.id);
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
    if (rc == SQLITE_DONE) {  return true; }
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

// Inserts one payment (money snapped to cents, balance = due - paid) and returns its id, -1 on failure.


// NOTE: STUB (not present in transcript) — added to satisfy DataConn interface.

// NOTE: STUB (not present in transcript).

Payment SqliteConn::getPaymentById(int id) {
    auto v = QueryPayments(db, std::string("SELECT ") + kPaymentCols + " FROM charges p WHERE p.id = ?", {id});
    return v.empty() ? Payment{} : v[0];
}

std::vector<Payment> SqliteConn::getPaymentsForService(int serviceId) {
    return QueryPayments(db, std::string("SELECT ") + kPaymentCols + " FROM charges p WHERE p.service_id = ? ORDER BY p.pay_date, p.id", {serviceId});
}

std::vector<Payment> SqliteConn::getPaymentsForUser(int userId) {
    return QueryPayments(db, std::string("SELECT ") + kPaymentCols + " FROM charges p WHERE p.user_id = ? ORDER BY p.pay_date, p.id", {userId});
}

std::vector<Payment> SqliteConn::getRecentPayments(int limit) {
    return QueryPayments(db, std::string("SELECT ") + kPaymentCols + " FROM charges p WHERE p.status != 'future' ORDER BY p.pay_date DESC, p.id DESC LIMIT ?", {limit});
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
        std::string("SELECT ") + kPaymentCols + " FROM charges p "
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
    addTextClause("CAST(CAST(strftime('%Y', p.pay_date) AS INTEGER) AS TEXT)", year);
    addTextClause("CAST(CAST(strftime('%m', p.pay_date) AS INTEGER) AS TEXT)", month);
    addTextClause("CAST(CAST(strftime('%d', p.pay_date) AS INTEGER) AS TEXT)", day);
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
    sql += " ORDER BY p.pay_date DESC, p.id DESC LIMIT 2000";

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    for (size_t i = 0; i < params.size(); i++) {
        sqlite3_bind_text(stmt, (int)i + 1, params[i].c_str(), -1, SQLITE_TRANSIENT);
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) results.push_back(ReadPayment(stmt));
    sqlite3_finalize(stmt);
    return results;
}

std::vector<Payment> SqliteConn::getAllPayments() {
    return QueryPayments(db, std::string("SELECT ") + kPaymentCols + " FROM charges p ORDER BY p.pay_date, p.id");
}

std::vector<Payment> SqliteConn::getFuturePayments() {
    return QueryPayments(db, std::string("SELECT ") + kPaymentCols + " FROM charges p WHERE p.status = 'future' ORDER BY p.pay_date, p.id");
}

// --- TIMELINE ---
// Table/action -> a human-readable Greek notice, for the "friendly" (App-notices)
// rendering mode. Falls back to a generic phrasing for anything not special-cased.
static std::string friendlyNotice(const std::string& table, const std::string& action, int recordId) {
    static const std::unordered_map<std::string, std::string> tableNames = {
        {"users", "έναν χρήστη"}, {"services", "μια υπηρεσία"},
        {"service_users", "μια σύνδεση χρήστη-υπηρεσίας"}, {"payments", "μια είσπραξη/πληρωμή"}, {"charges", "μια χρέωση"}, {"allocations", "μια εξόφληση χρέωσης"}
    };
    std::string what = tableNames.count(table) ? tableNames.at(table) : ("κάτι στο " + table);
    std::string verb = action == "INSERT" ? "Προσθέσατε" : action == "UPDATE" ? "Επεξεργαστήκατε" : action == "DELETE" ? "Διαγράψατε" : action;
    return verb + " " + what + " (#" + std::to_string(recordId) + ")";
}

std::vector<std::string> SqliteConn::getTimelineActions(const std::string& dateFilter, int offset, int limit,
                                                          const std::string& category, bool friendly) {
    std::vector<std::string> actions;
    // stored UTC, shown local; the optional filter is a UTC prefix (only "" is used today)
    std::string sql = "SELECT datetime(changed_at, 'localtime'), table_name, action_type, old_data, new_data, record_id, category FROM audit_log WHERE changed_at GLOB ?";
    if (!category.empty()) sql += " AND category = ?";
    sql += " ORDER BY changed_at DESC LIMIT ? OFFSET ?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    std::string filter = dateFilter.empty() ? "*" : dateFilter + "*";
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
    std::string sql = "SELECT TOTAL(CAST(ROUND(amount_paid*100) AS INTEGER)) / 100.0 FROM charges WHERE pay_date >= ?1 AND pay_date < date(?1, '+1 month') AND status != 'future'";
    if(userId != -1) sql += " AND user_id=" + std::to_string(userId);
    if(serviceId != -1) sql += " AND service_id=" + std::to_string(serviceId);
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    char first[16]; std::snprintf(first, sizeof(first), "%04d-%02d-01", year, month);
    sqlite3_bind_text(stmt, 1, first, -1, SQLITE_TRANSIENT);
    if(sqlite3_step(stmt) == SQLITE_ROW) total = sqlite3_column_double(stmt,0);
    sqlite3_finalize(stmt);
    return total;
}

double SqliteConn::getTotalPending(int year, int month, int userId, int serviceId) {
    double total = 0;
    std::string sql = "SELECT TOTAL(CAST(ROUND(amount_due*100) AS INTEGER) - CAST(ROUND(amount_paid*100) AS INTEGER)) / 100.0 FROM charges WHERE pay_date >= ?1 AND pay_date < date(?1, '+1 month') AND status='pending'";
    if(userId != -1) sql += " AND user_id=" + std::to_string(userId);
    if(serviceId != -1) sql += " AND service_id=" + std::to_string(serviceId);
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    char first[16]; std::snprintf(first, sizeof(first), "%04d-%02d-01", year, month);
    sqlite3_bind_text(stmt, 1, first, -1, SQLITE_TRANSIENT);
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
    if (rc == SQLITE_DONE) {  return true; }
    return false;
}

bool SqliteConn::removeNote(int id) {
    std::string sql = "DELETE FROM notes WHERE id=?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc == SQLITE_DONE) {  return true; }
    return false;
}

// --- BACKUP ---
bool SqliteConn::exportAllTablesToCSV(const std::string& folder) {
    std::vector<std::string> tables = {"buildings", "apartments", "users", "services", "service_users", "master_services", "master_service_components", "charges", "payments", "allocations", "receipts", "notes"};
    for(const auto& t : tables) {
        std::string path = folder + "/" + t + "_backup.csv";
        std::ofstream file(path);
        if(!file.is_open()) return false;
        auto cols = getTableColumns(t);
        auto greek = greekHeaders[t];
        // one header per real column: Greek name where we have one, the column name otherwise
        for(size_t i=0; i<cols.size(); ++i) file << (i?",":"") << "\"" << (i < greek.size() ? greek[i] : cols[i]) << "\"";
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
        s.suggested_price = priceOf(s.id);

        // Get users for this service
        jss.users = getUsersForService(s.id);

        // Sum payments per user
        jss.userTotalPaid.clear();
        jss.userTotalDue.clear();
        jss.userNet.clear();
        long long totDue = 0, totPaid = 0, totNet = 0;    // cents: the net must hit exactly 0
        for (auto& u : jss.users) {
            auto payments = getPaymentsForUser(u.id); // only this user's payments
            long long due = 0, paid = 0, paidOut = 0, collected = 0;
            for (auto& p : payments) {
                if (p.service_id == s.id && p.status != "future") { // only payments for this service, excl. future
                    due += ToCents(p.amount_due);
                    paid += ToCents(p.amount_paid);
                    if (p.direction == "out") paidOut += ToCents(p.amount_paid); else collected += ToCents(p.amount_paid);
                }
            }
            jss.userTotalDue.push_back(FromCents(due));
            jss.userTotalPaid.push_back(FromCents(paid));
            jss.userNet.push_back(FromCents(paidOut - collected));
            totDue += due; totPaid += paid; totNet += paidOut - collected;
        }
        jss.totalDue = FromCents(totDue);
        jss.totalPaid = FromCents(totPaid);
        jss.totalNet = FromCents(totNet);
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
    // Read-only by design: writes here would bypass the app's rules and are what an audit trail must not
    // allow (DROP TRIGGER, editing history). Every change goes through the app's own screens.
    if (!sqlite3_stmt_readonly(stmt)) {
        outError = "Η κονσόλα SQL είναι μόνο για ανάγνωση (SELECT). Οι αλλαγές γίνονται από τις οθόνες της εφαρμογής.";
        sqlite3_finalize(stmt);
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
    return true;
}


// --- PAGED "PHONE BOOK" PICKERS (dashboard filters) ------------------------
// These fetch only one page at a time. LIKE-based, fully parameterized.
// The letter filter matches the exact form the caller passes -- for Greek
// stored in uppercase (the seed convention) the caller passes uppercase, and
// LIKE is byte-exact for those 2-byte sequences. For ASCII, LIKE is already
// case-insensitive, so "A%" matches both "ALEX" and "alex".

std::vector<User> SqliteConn::pickerUsers(const std::vector<std::string>& letters,
                                           const std::string& nameSearch,
                                           int offset, int limit,
                                           bool reverseSort) {
    std::vector<User> results;
    std::string sql = std::string(kUserSelect) + "WHERE 1=1";
    std::vector<std::string> binds;
    if (!letters.empty()) {
        sql += " AND (";
        for (size_t i = 0; i < letters.size(); i++) {
            if (i > 0) sql += " OR ";
            sql += "u.full_name LIKE ? || '%'";
            binds.push_back(letters[i]);
        }
        sql += ")";
    }
    if (!nameSearch.empty()) {
        sql += " AND LOWER(u.full_name) LIKE '%' || LOWER(?) || '%'";
        binds.push_back(nameSearch);
    }
    sql += reverseSort ? " ORDER BY u.full_name DESC" : " ORDER BY u.full_name ASC";
    sql += " LIMIT ? OFFSET ?";

    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    int idx = 1;
    for (auto& b : binds) sqlite3_bind_text(stmt, idx++, b.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, idx++, limit);
    sqlite3_bind_int(stmt, idx++, offset);
    while (sqlite3_step(stmt) == SQLITE_ROW) results.push_back(MapUserRow(stmt));
    sqlite3_finalize(stmt);
    return results;
}

int SqliteConn::pickerUsersCount(const std::vector<std::string>& letters,
                                  const std::string& nameSearch) {
    std::string sql = "SELECT COUNT(*) FROM users WHERE 1=1";
    std::vector<std::string> binds;
    if (!letters.empty()) {
        sql += " AND (";
        for (size_t i = 0; i < letters.size(); i++) {
            if (i > 0) sql += " OR ";
            sql += "full_name LIKE ? || '%'";
            binds.push_back(letters[i]);
        }
        sql += ")";
    }
    if (!nameSearch.empty()) {
        sql += " AND LOWER(full_name) LIKE '%' || LOWER(?) || '%'";
        binds.push_back(nameSearch);
    }
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    int idx = 1;
    for (auto& b : binds) sqlite3_bind_text(stmt, idx++, b.c_str(), -1, SQLITE_TRANSIENT);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}





std::vector<Service> SqliteConn::pickerServices(const std::string& nameSearch,
                                                 int offset, int limit) {
    std::vector<Service> results;
    std::string sql = "SELECT id, label, extra_notes, created_at FROM services WHERE 1=1";
    std::vector<std::string> binds;
    if (!nameSearch.empty()) {
        sql += " AND LOWER(label) LIKE '%' || LOWER(?) || '%'";
        binds.push_back(nameSearch);
    }
    sql += " ORDER BY label ASC LIMIT ? OFFSET ?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    int idx = 1;
    for (auto& b : binds) sqlite3_bind_text(stmt, idx++, b.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, idx++, limit);
    sqlite3_bind_int(stmt, idx++, offset);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Service s;
        s.id = sqlite3_column_int(stmt, 0);
        s.label = sqlite3_column_text(stmt, 1) ? (const char*)sqlite3_column_text(stmt, 1) : "";
        s.extra_notes = sqlite3_column_text(stmt, 2) ? (const char*)sqlite3_column_text(stmt, 2) : "";
        s.created_at = sqlite3_column_text(stmt, 3) ? (const char*)sqlite3_column_text(stmt, 3) : "";
        s.suggested_price = priceOf(s.id);
        results.push_back(s);
    }
    sqlite3_finalize(stmt);
    return results;
}

int SqliteConn::pickerServicesCount(const std::string& nameSearch) {
    std::string sql = "SELECT COUNT(*) FROM services WHERE 1=1";
    std::vector<std::string> binds;
    if (!nameSearch.empty()) {
        sql += " AND LOWER(label) LIKE '%' || LOWER(?) || '%'";
        binds.push_back(nameSearch);
    }
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    int idx = 1;
    for (auto& b : binds) sqlite3_bind_text(stmt, idx++, b.c_str(), -1, SQLITE_TRANSIENT);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

// --- PAGED LIST QUERIES (offset/limit) -------------------------------------
// These build a WHERE clause from the caller's filter set and return only the
// requested slice. The corresponding countX functions build the same WHERE
// with SELECT COUNT(*) so the pager can show "X-Y of Z" without fetching
// anything extra.

// Shared builder for pageUsers / countUsers so both stay in lockstep.
static std::string BuildUserPageWhere(const std::string& query, int scope,
                                       const std::vector<std::string>& letters,
                                       std::vector<std::string>& bindValues) {
    std::string where = " WHERE 1=1";
    if (!query.empty()) {
        bindValues.push_back(query);
        switch (scope) {
            case 1:  where += " AND u.full_name    LIKE '%' || ? || '%'"; break;
            case 2:  where += " AND u.special_code LIKE '%' || ? || '%'"; break;
            case 3:  where += " AND u.phone        LIKE '%' || ? || '%'"; break;
            case 4:  where += " AND b.address      LIKE '%' || ? || '%'"; break;
            default:
                where += " AND (u.full_name LIKE '%' || ? || '%'"
                         " OR u.phone LIKE '%' || ? || '%'"
                         " OR u.special_code LIKE '%' || ? || '%'"
                         " OR a.label LIKE '%' || ? || '%'"
                         " OR b.address LIKE '%' || ? || '%'"
                         " OR b.area LIKE '%' || ? || '%'"
                         " OR b.postal_code LIKE '%' || ? || '%')";
                // Push the query six more times to fill the extra placeholders.
                for (int k = 0; k < 6; k++) bindValues.push_back(query);
                break;
        }
    }
    if (!letters.empty()) {
        where += " AND (";
        for (size_t i = 0; i < letters.size(); i++) {
            if (i > 0) where += " OR ";
            where += "u.full_name LIKE ? || '%'";
            bindValues.push_back(letters[i]);
        }
        where += ")";
    }
    return where;
}

// ORDER BY for the list-header sort. Column numbers are the Πελάτες table's own
// (0 id, 1 address, 2 apartment, 3 name, 4 phone, 5 area, 6 ΤΚ, 7 special code); anything
// else falls back to the name. Whitelisted, never built from user text.
static std::string UserOrderBy(const SortSpec& spec) {
    std::string out;
    for (auto& k : spec) {
        const char* key = nullptr;
        switch (k.first) {
            case 0: key = "u.id"; break;           case 1: key = "b.address"; break;
            case 2: key = "a.label"; break;        case 3: key = "u.full_name"; break;
            case 4: key = "u.phone"; break;        case 5: key = "b.area"; break;
            case 6: key = "b.postal_code"; break;  case 7: key = "u.special_code"; break;
            default: break;
        }
        if (key) out += std::string(out.empty() ? "" : ", ") + key + (k.second ? " ASC" : " DESC");
    }
    if (out.empty()) out = "u.full_name ASC";
    return out + ", u.id ASC";
}

std::vector<User> SqliteConn::pageUsers(const std::string& query, int scope,
                                         const std::vector<std::string>& letters,
                                         int offset, int limit, const SortSpec& sort) {
    std::vector<std::string> binds;
    std::string where = BuildUserPageWhere(query, scope, letters, binds);
    std::string sql = std::string(kUserSelect) + where + " ORDER BY " + UserOrderBy(sort) + " LIMIT ? OFFSET ?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    int idx = 1;
    for (auto& b : binds) sqlite3_bind_text(stmt, idx++, b.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, idx++, limit);
    sqlite3_bind_int(stmt, idx++, offset);
    std::vector<User> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) results.push_back(MapUserRow(stmt));
    sqlite3_finalize(stmt);
    return results;
}

int SqliteConn::countUsers(const std::string& query, int scope,
                            const std::vector<std::string>& letters) {
    std::vector<std::string> binds;
    std::string where = BuildUserPageWhere(query, scope, letters, binds);
    // kUserSelect's FROM/JOINs are reused so a.label/b.* exist for the WHERE
    // clauses above; COUNT(*) is applied on top of that same join.
    std::string sql =
        "SELECT COUNT(*) FROM users u "
        "LEFT JOIN apartments a ON a.id = u.apartment_id "
        "LEFT JOIN buildings  b ON b.id = a.building_id " + where;
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    int idx = 1;
    for (auto& b : binds) sqlite3_bind_text(stmt, idx++, b.c_str(), -1, SQLITE_TRANSIENT);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

// Services table columns: 0 id, 1 label, 2 price, 3 notes.
static std::string ServiceOrderBy(const SortSpec& spec) {
    std::string out;
    for (auto& k : spec) {
        const char* key = nullptr;
        switch (k.first) {
            case 0: key = "s.id"; break;  case 1: key = "s.label"; break;
            case 2: key = "s.suggested_price"; break;  case 3: key = "s.extra_notes"; break;
            default: break;
        }
        if (key) out += std::string(out.empty() ? "" : ", ") + key + (k.second ? " ASC" : " DESC");
    }
    if (out.empty()) out = "s.label ASC";
    return out + ", s.id ASC";
}

std::vector<Service> SqliteConn::pageServices(const std::string& query, int offset, int limit, const SortSpec& sort) {
    // Joint services (>=2 owners) live on their own tab -- the subquery excludes
    // them so the pager total reflects only what this tab actually shows.
    std::string sql =
        "SELECT s.id, s.label, s.extra_notes, s.created_at FROM services s "
        "WHERE (SELECT COUNT(*) FROM service_users su WHERE su.service_id = s.id) <= 1 "
        "AND NOT EXISTS (SELECT 1 FROM joint_services j WHERE j.service_id = s.id)";
    std::vector<std::string> binds;
    if (!query.empty()) {
        sql += " AND (instr(fold(s.label), fold(?)) > 0 OR instr(fold(COALESCE(s.extra_notes,'')), fold(?)) > 0)";
        binds.push_back(query); binds.push_back(query);
    }
    sql += " ORDER BY " + ServiceOrderBy(sort) + " LIMIT ? OFFSET ?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    int idx = 1;
    for (auto& b : binds) sqlite3_bind_text(stmt, idx++, b.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, idx++, limit);
    sqlite3_bind_int(stmt, idx++, offset);
    std::vector<Service> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Service s;
        s.id = sqlite3_column_int(stmt, 0);
        s.label = ColText(stmt, 1);
        s.extra_notes = ColText(stmt, 2);
        s.created_at = ColText(stmt, 3);
        s.suggested_price = priceOf(s.id);
        results.push_back(s);
    }
    sqlite3_finalize(stmt);
    return results;
}

int SqliteConn::countServices(const std::string& query) {
    std::string sql =
        "SELECT COUNT(*) FROM services s "
        "WHERE (SELECT COUNT(*) FROM service_users su WHERE su.service_id = s.id) <= 1 "
        "AND NOT EXISTS (SELECT 1 FROM joint_services j WHERE j.service_id = s.id)";
    std::vector<std::string> binds;
    if (!query.empty()) {
        sql += " AND (instr(fold(s.label), fold(?)) > 0 OR instr(fold(COALESCE(s.extra_notes,'')), fold(?)) > 0)";
        binds.push_back(query); binds.push_back(query);
    }
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    int idx = 1;
    for (auto& b : binds) sqlite3_bind_text(stmt, idx++, b.c_str(), -1, SQLITE_TRANSIENT);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

// Shared builder for pagePayments / countPayments. Text binds are pushed first
// (startDate/endDate/direction), then int binds (userId/serviceId), so the
// placeholder order in the SQL matches the bind order below.
static const char* kMissingReceiptSql =
    " AND EXISTS (SELECT 1 FROM allocations a JOIN payments m ON m.id = a.payment_id LEFT JOIN receipts rc ON rc.id = m.receipt_id"
    "             WHERE a.charge_id = p.id AND m.direction = 'in' AND (m.receipt_id IS NULL OR rc.status = 'void'))";

static std::string BuildPaymentPageWhere(const std::string& startDate, const std::string& endDate,
                                          int userId, int serviceId,
                                          const std::string& status, const std::string& direction,
                                          std::vector<std::string>& textBinds,
                                          std::vector<int>& intBinds, bool missingReceipt = false) {
    std::string where = " WHERE 1=1";
    if (missingReceipt) where += kMissingReceiptSql;
    if (!startDate.empty()) {
        where += " AND pay_date >= ?";
        textBinds.push_back(startDate);
    }
    if (!endDate.empty()) {
        where += " AND pay_date <= ?";
        textBinds.push_back(endDate);
    }
    // Placeholder order must equal bind order: every text placeholder first (dates, direction),
    // then the int ones -- direction used to come after user/service, so "client + type" bound
    // 'in' to user_id and matched nothing.
    if (direction == "in" || direction == "out") {
        where += " AND direction = ?";
        textBinds.push_back(direction);
    }
    if (status == "future")        where += " AND status = 'future'";
    else if (status == "resolved") where += " AND status != 'future'";
    if (userId >= 0)    { where += " AND user_id = ?";    intBinds.push_back(userId); }
    if (serviceId >= 0) { where += " AND service_id = ?"; intBinds.push_back(serviceId); }
    return where;
}

std::vector<Payment> SqliteConn::pagePayments(const std::string& startDate, const std::string& endDate,
                                               int userId, int serviceId,
                                               const std::string& status, const std::string& direction,
                                               int offset, int limit, const SortSpec& sort, bool missingReceiptOnly) {
    std::vector<std::string> textBinds;
    std::vector<int> intBinds;
    std::string where = BuildPaymentPageWhere(startDate, endDate, userId, serviceId, status, direction,
                                               textBinds, intBinds, missingReceiptOnly);
    // Payments table columns: 0 id, 1 date, 2 client, 3 service, 4 type, 5 status,
    // 6 due, 7 paid, 8 notes. Client/service sort by NAME (sub-select), not by id.
    std::string order;
    for (auto& k : sort) {
        std::string d = k.second ? " ASC" : " DESC";
        std::string part;
        switch (k.first) {
            case 0: part = "id" + d; break;
            case 1: part = "pay_date" + d; break;
            case 2: part = "(SELECT full_name FROM users WHERE users.id = p.user_id)" + d; break;
            case 3: part = "(SELECT label FROM services WHERE services.id = p.service_id)" + d; break;
            case 4: part = "direction" + d; break;
            case 5: part = "status" + d; break;
            case 6: part = "amount_due" + d; break;
            case 7: part = "amount_paid" + d; break;
            case 8: part = "notes" + d; break;
            case 9: part = "(SELECT MAX(r.number) FROM allocations a JOIN payments m ON m.id = a.payment_id "
                           "JOIN receipts r ON r.id = m.receipt_id WHERE a.charge_id = p.id AND r.status = 'issued')" + d; break;
            default: break;
        }
        if (!part.empty()) order += std::string(order.empty() ? "" : ", ") + part;
    }
    if (order.empty()) order = "pay_date DESC";
    order += ", id DESC";
    std::string sql = std::string("SELECT ") + kPaymentCols + " FROM charges p" + where + " ORDER BY " + order + " LIMIT ? OFFSET ?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    int idx = 1;
    for (auto& b : textBinds) sqlite3_bind_text(stmt, idx++, b.c_str(), -1, SQLITE_TRANSIENT);
    for (auto i : intBinds)   sqlite3_bind_int (stmt, idx++, i);
    sqlite3_bind_int(stmt, idx++, limit);
    sqlite3_bind_int(stmt, idx++, offset);
    std::vector<Payment> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) results.push_back(ReadPayment(stmt));
    sqlite3_finalize(stmt);
    return results;
}

int SqliteConn::countPayments(const std::string& startDate, const std::string& endDate,
                               int userId, int serviceId,
                               const std::string& status, const std::string& direction, bool missingReceiptOnly) {
    std::vector<std::string> textBinds;
    std::vector<int> intBinds;
    std::string where = BuildPaymentPageWhere(startDate, endDate, userId, serviceId, status, direction,
                                               textBinds, intBinds, missingReceiptOnly);
    std::string sql = "SELECT COUNT(*) FROM charges p" + where;
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    int idx = 1;
    for (auto& b : textBinds) sqlite3_bind_text(stmt, idx++, b.c_str(), -1, SQLITE_TRANSIENT);
    for (auto i : intBinds)   sqlite3_bind_int (stmt, idx++, i);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

// --- RECEIPTS -------------------------------------------------------------
static const char* kReceiptCols =
    "id, series, number, issue_date, COALESCE(user_id,-1), COALESCE(building_id,-1), total, method, "
    "COALESCE(rf_code,''), COALESCE(paper_ref,''), COALESCE(bank_ref,''), COALESCE(notes,''), status, "
    "COALESCE(void_reason,''), COALESCE(datetime(voided_at,'localtime'),''), COALESCE(datetime(created_at,'localtime'),'')";

static Receipt ReadReceipt(sqlite3_stmt* st) {
    auto txt = [&](int c) { const unsigned char* t = sqlite3_column_text(st, c); return std::string(t ? (const char*)t : ""); };
    Receipt r;
    r.id = sqlite3_column_int(st, 0);       r.series = txt(1);               r.number = sqlite3_column_int(st, 2);
    r.issue_date = txt(3);                  r.user_id = sqlite3_column_int(st, 4);
    r.building_id = sqlite3_column_int(st, 5);
    r.total = RoundMoney(sqlite3_column_double(st, 6));
    r.method = txt(7); r.rf_code = txt(8); r.paper_ref = txt(9); r.bank_ref = txt(10); r.notes = txt(11);
    r.status = txt(12); r.void_reason = txt(13); r.voided_at = txt(14); r.created_at = txt(15);
    return r;
}

static std::string JsonEsc(const std::string& in) {
    std::string o;
    for (char c : in) {
        switch (c) {
            case '"': o += "\\\""; break;  case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;  case '\r': o += "\\r"; break;  case '\t': o += "\\t"; break;
            default: if ((unsigned char)c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; } else o += c;
        }
    }
    return o;
}

static std::string receiptToJson(const Receipt& r, const std::vector<int>& paymentIds) {
    std::string ids;
    for (size_t i = 0; i < paymentIds.size(); i++) ids += (i ? "," : "") + std::to_string(paymentIds[i]);
    return "{ \"id\": " + std::to_string(r.id) + ", \"series\": \"" + JsonEsc(r.series) + "\", \"number\": " + std::to_string(r.number) +
           ", \"issue_date\": \"" + JsonEsc(r.issue_date) + "\", \"user_id\": " + std::to_string(r.user_id) +
           ", \"building_id\": " + std::to_string(r.building_id) + ", \"total\": " + FormatMoney(r.total) +
           ", \"method\": \"" + JsonEsc(r.method) + "\", \"rf_code\": \"" + JsonEsc(r.rf_code) +
           "\", \"paper_ref\": \"" + JsonEsc(r.paper_ref) + "\", \"bank_ref\": \"" + JsonEsc(r.bank_ref) +
           "\", \"status\": \"" + JsonEsc(r.status) + "\", \"payments\": [" + ids + "] }";
}




int SqliteConn::nextReceiptNumber(const std::string& series) {
    int n = 1;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "SELECT COALESCE(MAX(number),0) + 1 FROM receipts WHERE series = ?", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, (series.empty() ? std::string("A") : series).c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}


bool SqliteConn::voidReceipt(int id, const std::string& reason) {
    std::string why = reason;
    while (!why.empty() && std::isspace((unsigned char)why.back())) why.pop_back();
    if (why.empty()) { lastError_ = "Η ακύρωση απόδειξης χρειάζεται αιτιολογία."; return false; }
    Receipt old = getReceiptById(id);
    if (old.id < 0) { lastError_ = "Η απόδειξη δεν υπάρχει."; return false; }
    if (old.status == "void") { lastError_ = "Η απόδειξη είναι ήδη ακυρωμένη."; return false; }

    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "UPDATE receipts SET status='void', void_reason=?, voided_at=datetime('now') WHERE id=?", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, why.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { lastError_ = sqlite3_errmsg(db); return false; }
    auto ids = getReceiptPaymentIds(id);
    Receipt now = getReceiptById(id);
    return true;
}

Receipt SqliteConn::getReceiptById(int id) {
    Receipt r;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, (std::string("SELECT ") + kReceiptCols + " FROM receipts WHERE id = ?").c_str(), -1, &st, nullptr);
    sqlite3_bind_int(st, 1, id);
    if (sqlite3_step(st) == SQLITE_ROW) r = ReadReceipt(st);
    sqlite3_finalize(st);
    return r;
}

std::vector<Receipt> SqliteConn::getReceipts(int offset, int limit) {
    std::vector<Receipt> out;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, (std::string("SELECT ") + kReceiptCols + " FROM receipts ORDER BY issue_date DESC, id DESC LIMIT ? OFFSET ?").c_str(), -1, &st, nullptr);
    sqlite3_bind_int(st, 1, limit); sqlite3_bind_int(st, 2, offset);
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(ReadReceipt(st));
    sqlite3_finalize(st);
    return out;
}

std::vector<Receipt> SqliteConn::getReceiptsForUser(int userId) {
    std::vector<Receipt> out;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, (std::string("SELECT ") + kReceiptCols + " FROM receipts WHERE user_id = ? ORDER BY issue_date DESC, id DESC").c_str(), -1, &st, nullptr);
    sqlite3_bind_int(st, 1, userId);
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(ReadReceipt(st));
    sqlite3_finalize(st);
    return out;
}



// --- AUDIT TRUST ------------------------------------------------------------------------------------
// The audit log is written by triggers inside the database, so every write path is covered (app code,
// SQL console, a future server). Rows are append-only (UPDATE/DELETE refused) and hash-chained:
//   hash = SHA-256(prev_hash ␟ id ␟ changed_at ␟ actor ␟ table ␟ record_id ␟ action ␟ old ␟ new ␟ reason ␟ category)
// so changing or removing any past entry breaks every hash after it. verifyAuditLog() also compares each
// live row with its last audited snapshot, which catches changes made after someone removed the triggers.


static std::string AuditCanonical(const std::string& prev, const std::string& id, const std::string& at,
                                  const std::string& actor, const std::string& table, const std::string& recordId,
                                  const std::string& action, const std::string& oldData, const std::string& newData,
                                  const std::string& reason, const std::string& category) {
    const char US = '\x1f';
    return prev + US + id + US + at + US + actor + US + table + US + recordId + US + action + US +
           oldData + US + newData + US + reason + US + category;
}

static void SqlAuditHash(sqlite3_context* ctx, int, sqlite3_value** argv) {
    const unsigned char* t = sqlite3_value_text(argv[0]);
    std::string h = Sha256Hex(t ? std::string((const char*)t, sqlite3_value_bytes(argv[0])) : std::string());
    sqlite3_result_text(ctx, h.c_str(), -1, SQLITE_TRANSIENT);
}
static void SqlFold(sqlite3_context* ctx, int, sqlite3_value** argv) {
    const unsigned char* t = sqlite3_value_text(argv[0]);
    std::string f = FoldGreek(t ? (const char*)t : "");
    sqlite3_result_text(ctx, f.c_str(), (int)f.size(), SQLITE_TRANSIENT);
}
static void SqlAppActor(sqlite3_context* ctx, int, sqlite3_value**) {
    auto* self = static_cast<SqliteConn*>(sqlite3_user_data(ctx));
    sqlite3_result_text(ctx, self->auditActor_.c_str(), -1, SQLITE_TRANSIENT);
}
static void SqlAppReason(sqlite3_context* ctx, int, sqlite3_value**) {
    auto* self = static_cast<SqliteConn*>(sqlite3_user_data(ctx));
    sqlite3_result_text(ctx, self->auditReason_.c_str(), -1, SQLITE_TRANSIENT);
}

static std::string DefaultActor() {
    const char* u = std::getenv("USERNAME"); if (!u) u = std::getenv("USER");
    const char* h = std::getenv("COMPUTERNAME"); if (!h) h = std::getenv("HOSTNAME");
    std::string host = h ? h : "";
#ifndef _WIN32
    if (host.empty()) { char b[256] = {0}; if (gethostname(b, sizeof(b) - 1) == 0) host = b; }
#endif
    return std::string(u ? u : "unknown") + "@" + (host.empty() ? "unknown" : host);
}

void SqliteConn::registerSqlFunctions() {
    if (auditActor_.empty()) auditActor_ = DefaultActor();
    sqlite3_create_function_v2(db, "audit_hash", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, SqlAuditHash, nullptr, nullptr, nullptr);
    sqlite3_create_function_v2(db, "fold", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, SqlFold, nullptr, nullptr, nullptr);
    sqlite3_create_function_v2(db, "app_actor", 0, SQLITE_UTF8, this, SqlAppActor, nullptr, nullptr, nullptr);
    sqlite3_create_function_v2(db, "app_reason", 0, SQLITE_UTF8, this, SqlAppReason, nullptr, nullptr, nullptr);
}

// json_object('c1', X."c1", ...) over the table's current columns.
static std::string JsonRowExpr(sqlite3* db, const std::string& table, const std::string& prefix) {
    std::string cols;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, ("PRAGMA table_info(" + table + ")").c_str(), -1, &st, nullptr);
    while (sqlite3_step(st) == SQLITE_ROW) {
        std::string c = (const char*)sqlite3_column_text(st, 1);
        cols += (cols.empty() ? "" : ", ") + std::string("'") + c + "', " + prefix + "\"" + c + "\"";
    }
    sqlite3_finalize(st);
    return "json_object(" + cols + ")";
}

static std::map<std::string, std::string> ExpectedAuditTriggers(sqlite3* db) {
    std::map<std::string, std::string> out;
    for (const char* t : kAuditedTables) {
        if (!TableExists(db, t)) continue;       // created by a later migration
        std::string T = t, oldJ = JsonRowExpr(db, T, "OLD."), newJ = JsonRowExpr(db, T, "NEW.");
        auto ins = [&](const std::string& action, const std::string& rid, const std::string& o, const std::string& n) {
            return "INSERT INTO audit_log (table_name, record_id, action_type, old_data, new_data, category, actor, reason) "
                   "VALUES ('" + T + "', " + rid + ", '" + action + "', " + o + ", " + n + ", 'business', app_actor(), app_reason());";
        };
        out["audit_" + T + "_ins"] = "CREATE TRIGGER audit_" + T + "_ins AFTER INSERT ON " + T + " BEGIN " + ins("INSERT", "NEW.rowid", "NULL", newJ) + " END";
        out["audit_" + T + "_upd"] = "CREATE TRIGGER audit_" + T + "_upd AFTER UPDATE ON " + T + " WHEN " + oldJ + " IS NOT " + newJ +
                                     " BEGIN " + ins("UPDATE", "NEW.rowid", oldJ, newJ) + " END";
        out["audit_" + T + "_del"] = "CREATE TRIGGER audit_" + T + "_del AFTER DELETE ON " + T + " BEGIN " + ins("DELETE", "OLD.rowid", oldJ, "NULL") + " END";
    }
    // the log itself: hash-chain each new entry; nothing may be edited or removed afterwards
    out["audit_log_chain"] =
        "CREATE TRIGGER audit_log_chain AFTER INSERT ON audit_log BEGIN "
        "UPDATE audit_log SET prev_hash = COALESCE((SELECT hash FROM audit_log WHERE id < NEW.id ORDER BY id DESC LIMIT 1), ''), "
        "hash = audit_hash(COALESCE((SELECT hash FROM audit_log WHERE id < NEW.id ORDER BY id DESC LIMIT 1), '') || char(31) || NEW.id || char(31) || "
        "COALESCE(NEW.changed_at,'') || char(31) || COALESCE(NEW.actor,'') || char(31) || COALESCE(NEW.table_name,'') || char(31) || "
        "COALESCE(NEW.record_id,'') || char(31) || COALESCE(NEW.action_type,'') || char(31) || COALESCE(NEW.old_data,'') || char(31) || "
        "COALESCE(NEW.new_data,'') || char(31) || COALESCE(NEW.reason,'') || char(31) || COALESCE(NEW.category,'')) "
        "WHERE id = NEW.id; END";
    out["audit_log_no_forged_hash"] =
        "CREATE TRIGGER audit_log_no_forged_hash BEFORE INSERT ON audit_log WHEN NEW.hash IS NOT NULL OR NEW.prev_hash IS NOT NULL "
        "BEGIN SELECT RAISE(ABORT, 'Το ιστορικό αλλαγών συμπληρώνεται μόνο από τη βάση'); END";
    out["audit_log_append_only"] =
        "CREATE TRIGGER audit_log_append_only BEFORE UPDATE ON audit_log WHEN OLD.hash IS NOT NULL "
        "OR NEW.id IS NOT OLD.id OR NEW.changed_at IS NOT OLD.changed_at OR NEW.actor IS NOT OLD.actor "
        "OR NEW.table_name IS NOT OLD.table_name OR NEW.record_id IS NOT OLD.record_id OR NEW.action_type IS NOT OLD.action_type "
        "OR NEW.old_data IS NOT OLD.old_data OR NEW.new_data IS NOT OLD.new_data OR NEW.reason IS NOT OLD.reason "
        "OR NEW.category IS NOT OLD.category "
        "BEGIN SELECT RAISE(ABORT, 'Το ιστορικό αλλαγών δεν τροποποιείται'); END";
    out["audit_log_no_delete"] =
        "CREATE TRIGGER audit_log_no_delete BEFORE DELETE ON audit_log "
        "BEGIN SELECT RAISE(ABORT, 'Το ιστορικό αλλαγών δεν διαγράφεται'); END";
    return out;
}

int SqliteConn::ensureAuditTriggers() {
    int fixed = 0;
    auto expected = ExpectedAuditTriggers(db);
    {
        std::vector<std::string> stale;
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='trigger' AND name GLOB 'audit_*'", -1, &st, nullptr);
        while (sqlite3_step(st) == SQLITE_ROW) { std::string n = (const char*)sqlite3_column_text(st, 0); if (!expected.count(n)) stale.push_back(n); }
        sqlite3_finalize(st);
        for (auto& n : stale) { executeQuery("DROP TRIGGER IF EXISTS " + n + ";"); fixed++; }
    }
    for (auto& [name, sql] : expected) {
        std::string current;
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, "SELECT sql FROM sqlite_master WHERE type='trigger' AND name=?", -1, &st, nullptr);
        sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) current = (const char*)sqlite3_column_text(st, 0);
        sqlite3_finalize(st);
        if (current == sql) continue;
        fixed++;
        executeQuery("DROP TRIGGER IF EXISTS " + name + ";");
        executeQuery(sql + ";");
    }
    return fixed;
}

// Appends "<local date> <last audit id> <its hash>" to backups/audit_anchor.txt once a day. The file lives
// outside the database, so rewriting the whole chain inside the DB no longer matches the anchors.
void SqliteConn::writeAuditAnchor() {
    namespace fs = std::filesystem;
    try {
        fs::path dir = fs::path(dbPath_).parent_path() / "backups";
        fs::create_directories(dir);
        fs::path anchor = dir / (fs::path(dbPath_).stem().string() + "_audit_anchor.txt");
        auto t = std::time(nullptr); std::tm tmv{}; localtime_s(&tmv, &t);
        char today[16]; std::strftime(today, sizeof(today), "%Y-%m-%d", &tmv);
        std::string last;
        { std::ifstream in(anchor); std::string line; while (std::getline(in, line)) if (!line.empty()) last = line; }
        if (last.rfind(today, 0) == 0) return;
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, "SELECT id, hash FROM audit_log WHERE hash IS NOT NULL ORDER BY id DESC LIMIT 1", -1, &st, nullptr);
        if (sqlite3_step(st) == SQLITE_ROW) {
            std::ofstream out(anchor, std::ios::app);
            out << today << " " << sqlite3_column_int64(st, 0) << " " << (const char*)sqlite3_column_text(st, 1) << "\n";
        }
        sqlite3_finalize(st);
    } catch (const std::exception&) {}
}

DataConn::AuditCheck SqliteConn::verifyAuditLog() { return VerifyAudit(db, dbPath_); }

// Full integrity check on any connection (the app's own, or a read-only one in a background thread).
static DataConn::AuditCheck VerifyAudit(sqlite3* db, const std::string& dbPath_) {
    DataConn::AuditCheck r;
    auto problem = [&](const std::string& p) { r.ok = false; if (r.problems.size() < 50) r.problems.push_back(p); };

    // 1. triggers present and exactly as installed
    for (auto& [name, sql] : ExpectedAuditTriggers(db)) {
        sqlite3_stmt* st = nullptr; std::string current;
        sqlite3_prepare_v2(db, "SELECT sql FROM sqlite_master WHERE type='trigger' AND name=?", -1, &st, nullptr);
        sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) current = (const char*)sqlite3_column_text(st, 0);
        sqlite3_finalize(st);
        if (current != sql) problem("Ο μηχανισμός καταγραφής " + name + (current.empty() ? " λείπει" : " έχει αλλοιωθεί"));
    }

    // 2. hash chain
    {
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, "SELECT id, changed_at, actor, table_name, record_id, action_type, old_data, new_data, reason, category, "
                               "prev_hash, hash FROM audit_log ORDER BY id", -1, &st, nullptr);
        auto txt = [&](int c) { const unsigned char* t = sqlite3_column_text(st, c); return std::string(t ? (const char*)t : ""); };
        std::string prev;
        while (sqlite3_step(st) == SQLITE_ROW) {
            r.entries++;
            std::string id = txt(0), prevHash = txt(10), hash = txt(11);
            std::string expect = Sha256Hex(AuditCanonical(prev, id, txt(1), txt(2), txt(3), txt(4), txt(5), txt(6), txt(7), txt(8), txt(9)));
            if (prevHash != prev || hash != expect) { problem("Η αλυσίδα του ιστορικού σπάει στην εγγραφή #" + id); break; }
            prev = hash;
        }
        sqlite3_finalize(st);
    }

    // 3. every live row equals its last audited snapshot; nothing vanished without a DELETE entry
    long long sinceId = 0;
    {
        sqlite3_stmt* st = nullptr;
        // the most recent baseline generation (each schema change that reshapes rows takes a new one)
        sqlite3_prepare_v2(db, "SELECT COALESCE(MIN(id), 0) FROM audit_log WHERE action_type = 'BASELINE' AND actor = "
                               "(SELECT actor FROM audit_log WHERE action_type = 'BASELINE' ORDER BY id DESC LIMIT 1)", -1, &st, nullptr);
        if (sqlite3_step(st) == SQLITE_ROW) sinceId = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    for (const char* t : kAuditedTables) {
        std::string T = t;
        if (!TableExists(db, T)) continue;
        std::string sql =
            "SELECT x.rowid FROM " + T + " x WHERE " + JsonRowExpr(db, T, "x.") + " IS NOT "
            "(SELECT new_data FROM audit_log a WHERE a.table_name = '" + T + "' AND a.record_id = x.rowid ORDER BY a.id DESC LIMIT 1) "
            "UNION ALL "
            // only entries written by the triggers (from the v7 baseline on): older app-written entries used
            // record_id loosely (a client id for residencies, a service id for service_users)
            "SELECT a.record_id FROM audit_log a WHERE a.table_name = '" + T + "' AND a.id >= " + std::to_string(sinceId) + " AND a.id = (SELECT MAX(id) FROM audit_log b "
            "WHERE b.table_name = a.table_name AND b.record_id = a.record_id) AND a.action_type != 'DELETE' "
            "AND NOT EXISTS (SELECT 1 FROM " + T + " y WHERE y.rowid = a.record_id) LIMIT 20";
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) { problem("Έλεγχος " + T + ": " + sqlite3_errmsg(db)); continue; }
        while (sqlite3_step(st) == SQLITE_ROW)
            problem("Η εγγραφή " + T + " #" + std::to_string(sqlite3_column_int64(st, 0)) + " άλλαξε χωρίς καταγραφή στο ιστορικό");
        sqlite3_finalize(st);
    }

    // 3b. attachment files are still there, with the recorded size (the content hash is their file name)
    if (TableExists(db, "attachments")) {
        namespace fs = std::filesystem;
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, "SELECT id, stored_path, size_bytes, file_name FROM attachments WHERE removed_at IS NULL", -1, &st, nullptr);
        while (sqlite3_step(st) == SQLITE_ROW) {
            fs::path f = fs::path(dbPath_).parent_path() / fs::u8path((const char*)sqlite3_column_text(st, 1));
            std::error_code ec;
            if (!fs::exists(f, ec) || (long long)fs::file_size(f, ec) != sqlite3_column_int64(st, 2))
                problem(std::string("Το συνημμένο «") + (const char*)sqlite3_column_text(st, 3) + "» λείπει ή έχει αλλοιωθεί");
        }
        sqlite3_finalize(st);
    }

    // 4. daily anchors written outside the database still match
    namespace fs = std::filesystem;
    fs::path anchor = fs::path(dbPath_).parent_path() / "backups" / (fs::path(dbPath_).stem().string() + "_audit_anchor.txt");
    std::ifstream in(anchor);
    std::string date, hash; long long id = 0;
    while (in >> date >> id >> hash) {
        sqlite3_stmt* st = nullptr; std::string have;
        sqlite3_prepare_v2(db, "SELECT hash FROM audit_log WHERE id = ?", -1, &st, nullptr);
        sqlite3_bind_int64(st, 1, id);
        if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0)) have = (const char*)sqlite3_column_text(st, 0);
        sqlite3_finalize(st);
        if (have != hash) problem("Το σημείο ελέγχου της " + date + " (εγγραφή #" + std::to_string(id) + ") δεν ταιριάζει με το ιστορικό");
    }
    return r;
}

// --- PAYMENT QUERIES & TOTALS IN SQL -----------------------------------------------------------------
namespace {
struct Bind { bool text; std::string s; double d; };

// WHERE clause for a PaymentFilter over "payments p" (+ the joins it needs). Binds are kept in one ordered
// list next to their placeholders, so they can never get out of step.
struct PayWhere {
    std::string joins, where = " WHERE 1=1";
    std::vector<Bind> binds;
    void text(const std::string& clause, const std::string& v) { where += clause; binds.push_back({true, v, 0}); }
    void num(const std::string& clause, double v) { where += clause; binds.push_back({false, "", v}); }
};

PayWhere BuildPayWhere(const DataConn::PaymentFilter& f) {
    PayWhere w;
    if (!f.fromDate.empty()) w.text(" AND p.pay_date >= ?", f.fromDate);
    if (!f.toDate.empty())   w.text(" AND p.pay_date <= ?", f.toDate);
    if (f.userId >= 0)       w.num(" AND p.user_id = ?", f.userId);
    if (f.serviceId >= 0)    w.num(" AND p.service_id = ?", f.serviceId);
    if (f.apartmentId >= 0)  w.num(" AND p.apartment_id = ?", f.apartmentId);
    if (f.direction == "in" || f.direction == "out") w.text(" AND p.direction = ?", f.direction);
    if (f.status == 1) w.where += " AND p.status != 'future'";
    if (f.status == 2) w.where += " AND p.status = 'future'";
    if (f.owed == 1) w.where += " AND p.status != 'future' AND p.balance <= 0.005";
    if (f.owed == 2) w.where += " AND p.status != 'future' AND p.balance > 0.005";
    if (f.missingReceipt) w.where += kMissingReceiptSql;
    if (f.minDue > 0) w.num(" AND p.amount_due >= ?", f.minDue);
    if (f.maxDue > 0) w.num(" AND p.amount_due <= ?", f.maxDue);
    bool needNames = !f.text.empty() || !f.clientText.empty() || !f.serviceText.empty() || !f.buildingText.empty();
    if (needNames)
        w.joins = " LEFT JOIN users u ON u.id = p.user_id LEFT JOIN services s ON s.id = p.service_id"
                  " LEFT JOIN apartments ua ON ua.id = u.apartment_id LEFT JOIN buildings ub ON ub.id = ua.building_id";
    auto has = [&](const std::string& expr, const std::string& needle) {
        if (!needle.empty()) w.text(" AND instr(fold(" + expr + "), fold(?)) > 0", needle);
    };
    has("COALESCE(u.full_name,'') || ' ' || COALESCE(s.label,'') || ' ' || COALESCE(p.notes,'') || ' ' || p.status || ' ' || p.pay_date || ' ' || COALESCE(ub.address,'')", f.text);
    has("COALESCE(u.full_name,'')", f.clientText);
    has("COALESCE(s.label,'')", f.serviceText);
    has("COALESCE(p.notes,'')", f.notesText);
    has("COALESCE(ub.address,'')", f.buildingText);
    return w;
}

void BindAll(sqlite3_stmt* st, const std::vector<Bind>& binds, int first = 1) {
    int i = first;
    for (auto& b : binds) {
        if (b.text) sqlite3_bind_text(st, i++, b.s.c_str(), -1, SQLITE_TRANSIENT);
        else if (b.d == (double)(long long)b.d) sqlite3_bind_int64(st, i++, (long long)b.d);
        else sqlite3_bind_double(st, i++, b.d);
    }
}

// cents of an amount column
std::string C(const std::string& col) { return "CAST(ROUND(" + col + " * 100) AS INTEGER)"; }

const std::string kTotalsCols =
    "COUNT(*), "
    "TOTAL(CASE WHEN p.status != 'future' AND p.direction = 'in'  THEN " + C("p.amount_paid") + " END), "
    "TOTAL(CASE WHEN p.status != 'future' AND p.direction = 'out' THEN " + C("p.amount_paid") + " END), "
    "TOTAL(CASE WHEN p.status != 'future' AND p.direction = 'in'  AND p.balance > 0.005 THEN " + C("p.balance") + " END), "
    "TOTAL(CASE WHEN p.status != 'future' AND p.direction = 'out' AND p.balance > 0.005 THEN " + C("p.balance") + " END), "
    "TOTAL(CASE WHEN p.status  = 'future' AND p.direction = 'in'  THEN " + C("p.amount_due") + " END), "
    "TOTAL(CASE WHEN p.status  = 'future' AND p.direction = 'out' THEN " + C("p.amount_due") + " END), "
    "TOTAL(" + C("p.amount_due") + "), TOTAL(" + C("p.amount_paid") + ")";

DataConn::MoneyTotals ReadTotals(sqlite3_stmt* st, int c0) {
    DataConn::MoneyTotals t;
    auto e = [&](int c) { return FromCents((long long)sqlite3_column_double(st, c)); };
    t.count = sqlite3_column_int64(st, c0);
    t.collected = e(c0 + 1); t.paidOut = e(c0 + 2); t.owedIn = e(c0 + 3); t.owedOut = e(c0 + 4);
    t.futureIn = e(c0 + 5); t.futureOut = e(c0 + 6); t.dueAll = e(c0 + 7); t.paidAll = e(c0 + 8);
    return t;
}
} // namespace

std::vector<Payment> SqliteConn::findPayments(const PaymentFilter& f, PayOrder order, int offset, int limit) {
    PayWhere w = BuildPayWhere(f);
    const char* ord = order == PayOrder::DateAsc ? " ORDER BY p.pay_date, p.id"
                    : order == PayOrder::ResolutionDesc ? " ORDER BY p.resolution_date DESC, p.id DESC"
                    : " ORDER BY p.pay_date DESC, p.id DESC";
    std::string sql = std::string("SELECT ") + kPaymentCols + " FROM charges p" + w.joins + w.where + ord + " LIMIT ? OFFSET ?";
    std::vector<Payment> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) { lastError_ = sqlite3_errmsg(db); return out; }
    BindAll(st, w.binds);
    sqlite3_bind_int(st, (int)w.binds.size() + 1, limit);
    sqlite3_bind_int(st, (int)w.binds.size() + 2, offset);
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(ReadPayment(st));
    sqlite3_finalize(st);
    return out;
}

long long SqliteConn::countPaymentsWhere(const PaymentFilter& f) {
    return paymentTotals(f).count;
}

DataConn::MoneyTotals SqliteConn::paymentTotals(const PaymentFilter& f) {
    PayWhere w = BuildPayWhere(f);
    std::string sql = "SELECT " + kTotalsCols + " FROM charges p" + w.joins + w.where;
    MoneyTotals t;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) { lastError_ = sqlite3_errmsg(db); return t; }
    BindAll(st, w.binds);
    if (sqlite3_step(st) == SQLITE_ROW) t = ReadTotals(st, 0);
    sqlite3_finalize(st);
    return t;
}

std::vector<DataConn::GroupedTotals> SqliteConn::paymentTotalsBy(PayGroup group, const PaymentFilter& f) {
    PayWhere w = BuildPayWhere(f);
    std::string key = group == PayGroup::User ? "COALESCE(p.user_id,-1)"
                    : group == PayGroup::Service ? "p.service_id"
                    : group == PayGroup::Apartment ? "COALESCE(p.apartment_id,-1)"
                    : group == PayGroup::Day ? "p.pay_date"
                    : group == PayGroup::ApartmentUser ? "COALESCE(p.apartment_id,-1) || '|' || COALESCE(p.user_id,-1)"
                    : "COALESCE(p.user_id,-1) || '|' || p.service_id";
    // group by the real columns (not the text key) so SQLite can stream an index instead of sorting
    std::string groupCols = group == PayGroup::User ? "p.user_id" : group == PayGroup::Service ? "p.service_id"
                          : group == PayGroup::Apartment ? "p.apartment_id" : group == PayGroup::Day ? "p.pay_date"
                          : group == PayGroup::ApartmentUser ? "p.apartment_id, p.user_id"
                          : "p.user_id, p.service_id";
    std::string sql = "SELECT " + key + ", " + kTotalsCols + " FROM charges p" + w.joins + w.where + " GROUP BY " + groupCols;
    std::vector<GroupedTotals> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) { lastError_ = sqlite3_errmsg(db); return out; }
    BindAll(st, w.binds);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char* k = sqlite3_column_text(st, 0);
        out.push_back({k ? (const char*)k : "", ReadTotals(st, 1)});
    }
    sqlite3_finalize(st);
    return out;
}

// Fast startup check: the newest daily checkpoint still matches, and every entry after it chains correctly.
// O(entries since the last checkpoint). Returns "" when fine, else the problem.
static std::string QuickAuditCheck(sqlite3* db, const std::string& dbPath) {
    namespace fs = std::filesystem;
    long long fromId = 0; std::string fromHash, lastLine;
    {
        std::ifstream in(fs::path(dbPath).parent_path() / "backups" / (fs::path(dbPath).stem().string() + "_audit_anchor.txt"));
        std::string date, hash; long long id = 0;
        while (in >> date >> id >> hash) { fromId = id; fromHash = hash; }
    }
    sqlite3_stmt* st = nullptr;
    if (fromId > 0) {
        sqlite3_prepare_v2(db, "SELECT hash FROM audit_log WHERE id = ?", -1, &st, nullptr);
        sqlite3_bind_int64(st, 1, fromId);
        std::string have;
        if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0)) have = (const char*)sqlite3_column_text(st, 0);
        sqlite3_finalize(st);
        if (have != fromHash) return "το τελευταίο σημείο ελέγχου (εγγραφή #" + std::to_string(fromId) + ") δεν ταιριάζει";
    }
    sqlite3_prepare_v2(db, "SELECT id, changed_at, actor, table_name, record_id, action_type, old_data, new_data, reason, category, "
                           "prev_hash, hash FROM audit_log WHERE id > ? ORDER BY id", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, fromId);
    auto txt = [&](int c) { const unsigned char* t = sqlite3_column_text(st, c); return std::string(t ? (const char*)t : ""); };
    std::string prev = fromHash, problem;
    bool first = true;
    while (sqlite3_step(st) == SQLITE_ROW) {
        std::string id = txt(0);
        if (first && fromId == 0) prev = txt(10);           // no checkpoint yet: start from the first row's own prev
        first = false;
        std::string expect = Sha256Hex(AuditCanonical(prev, id, txt(1), txt(2), txt(3), txt(4), txt(5), txt(6), txt(7), txt(8), txt(9)));
        if (txt(10) != prev || txt(11) != expect) { problem = "η αλυσίδα του ιστορικού σπάει στην εγγραφή #" + id; break; }
        prev = txt(11);
    }
    sqlite3_finalize(st);
    return problem;
}

// --- BILLS (charges) AND MONEY (payments) ---------------------------------------------------------------
// Savepoints instead of BEGIN so these operations nest (e.g. editPaymentWithReceipt = editPayment + receipt).
bool SqliteConn::savepoint(const char* name) { return executeQuery(std::string("SAVEPOINT ") + name + ";"); }
bool SqliteConn::release(const char* name)   { return executeQuery(std::string("RELEASE ") + name + ";"); }
void SqliteConn::rollbackTo(const char* name) {
    std::string why = lastError_;
    executeQuery(std::string("ROLLBACK TO ") + name + "; RELEASE " + name + ";");
    lastError_ = why;
}


int SqliteConn::insertCharge(const Payment& in, int batchId) {
    Payment p = in;
    p.amount_due = RoundMoney(p.amount_due);
    int aptId = p.apartment_id >= 0 ? p.apartment_id : apartmentAt(p.user_id, p.year, p.month, p.day);
    std::string date = PayDate(p);
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "INSERT INTO charges (service_id, user_id, apartment_id, pay_date, amount_due, amount_paid, balance, status, "
                           "direction, notes, resolution_date, resolved_at, created_at, batch_id) "
                           "VALUES (?,?,?,?,?,0,?,?,?,?,?, datetime(?11,'utc'), COALESCE(datetime(?12,'utc'), datetime('now')), ?13)", -1, &st, nullptr);
    sqlite3_bind_int(st, 1, p.service_id);
    if (p.user_id > 0) sqlite3_bind_int(st, 2, p.user_id); else sqlite3_bind_null(st, 2);
    if (aptId > 0) sqlite3_bind_int(st, 3, aptId); else sqlite3_bind_null(st, 3);
    sqlite3_bind_text(st, 4, date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(st, 5, p.amount_due);
    sqlite3_bind_double(st, 6, p.amount_due);                       // balance before any money
    sqlite3_bind_text(st, 7, p.status.empty() ? "paid" : p.status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 8, p.direction.empty() ? "in" : p.direction.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 9, p.notes.c_str(), -1, SQLITE_TRANSIENT);
    if (p.resolution_date.empty()) sqlite3_bind_null(st, 10); else sqlite3_bind_text(st, 10, p.resolution_date.c_str(), -1, SQLITE_TRANSIENT);
    if (p.resolved_at.empty()) sqlite3_bind_null(st, 11); else sqlite3_bind_text(st, 11, p.resolved_at.c_str(), -1, SQLITE_TRANSIENT);
    if (p.created_at.empty()) sqlite3_bind_null(st, 12); else sqlite3_bind_text(st, 12, p.created_at.c_str(), -1, SQLITE_TRANSIENT);
    if (batchId > 0) sqlite3_bind_int(st, 13, batchId); else sqlite3_bind_null(st, 13);
    int rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) lastError_ = sqlite3_errmsg(db);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return -1;
    int cid = (int)sqlite3_last_insert_rowid(db);
    if (in.offer_id > 0) {                     // the claimed offer: list price and discount, next to the charge
        double list = RoundMoney(in.list_price), disc = MoneySub(list, p.amount_due);
        if (ToCents(disc) > 0) {
            sqlite3_prepare_v2(db, "INSERT INTO offer_uses (offer_id, charge_id, list_price, discount) VALUES (?,?,?,?)", -1, &st, nullptr);
            sqlite3_bind_int(st, 1, in.offer_id); sqlite3_bind_int(st, 2, cid);
            sqlite3_bind_double(st, 3, list); sqlite3_bind_double(st, 4, disc);
            rc = sqlite3_step(st);
            if (rc != SQLITE_DONE) lastError_ = sqlite3_errmsg(db);
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) return -1;
        }
    }
    return cid;
}

int SqliteConn::insertMoney(const MoneyPayment& in) {
    MoneyPayment m = in;
    m.amount = RoundMoney(m.amount);
    if (ToCents(m.amount) <= 0) { lastError_ = "Το ποσό της πληρωμής πρέπει να είναι μεγαλύτερο από 0."; return -1; }
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "INSERT INTO payments (user_id, pay_date, amount, direction, method, reference, notes, batch_id, created_at) "
                           "VALUES (?,?,?,?,?,?,?,?, COALESCE(datetime(?9,'utc'), datetime('now')))", -1, &st, nullptr);
    if (m.user_id > 0) sqlite3_bind_int(st, 1, m.user_id); else sqlite3_bind_null(st, 1);
    sqlite3_bind_text(st, 2, m.pay_date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(st, 3, m.amount);
    sqlite3_bind_text(st, 4, m.direction.empty() ? "in" : m.direction.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, m.method.empty() ? "unspecified" : m.method.c_str(), -1, SQLITE_TRANSIENT);
    if (m.reference.empty()) sqlite3_bind_null(st, 6); else sqlite3_bind_text(st, 6, m.reference.c_str(), -1, SQLITE_TRANSIENT);
    if (m.notes.empty()) sqlite3_bind_null(st, 7); else sqlite3_bind_text(st, 7, m.notes.c_str(), -1, SQLITE_TRANSIENT);
    if (m.batch_id > 0) sqlite3_bind_int(st, 8, m.batch_id); else sqlite3_bind_null(st, 8);
    if (m.created_at.empty()) sqlite3_bind_null(st, 9); else sqlite3_bind_text(st, 9, m.created_at.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) lastError_ = sqlite3_errmsg(db);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? (int)sqlite3_last_insert_rowid(db) : -1;
}

bool SqliteConn::allocate(int paymentId, int chargeId, double amount) {
    amount = RoundMoney(amount);
    if (ToCents(amount) <= 0) return true;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "INSERT INTO allocations (payment_id, charge_id, amount) VALUES (?,?,?) "
                           "ON CONFLICT(payment_id, charge_id) DO UPDATE SET amount = (CAST(ROUND(amount*100) AS INTEGER) + CAST(ROUND(excluded.amount*100) AS INTEGER)) / 100.0",
                       -1, &st, nullptr);
    sqlite3_bind_int(st, 1, paymentId); sqlite3_bind_int(st, 2, chargeId); sqlite3_bind_double(st, 3, amount);
    int rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) lastError_ = sqlite3_errmsg(db);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

// Moves the money paid on a charge to `desiredPaid` the way the old single-line screens expect:
// more -> a new payment (same client/date/direction, in the charge's batch); less -> trims payments that pay only
// this charge and have no issued receipt (newest first). Anything else (receipted money, money shared with other
// bills) is refused with a message pointing to the client's payments.
bool SqliteConn::reconcilePaid(int chargeId, double desiredPaid) {
    Payment c = getPaymentById(chargeId);
    long long want = ToCents(desiredPaid), have = ToCents(c.amount_paid), due = ToCents(c.amount_due);
    if (want == have) return true;
    if (want > have) {
        long long addMoney = want - have;
        long long addAlloc = std::min(want, std::max(due, 0LL)) - have;
        MoneyPayment m;
        m.user_id = c.user_id; m.direction = c.direction; m.batch_id = c.batch_id;
        char d[16]; std::snprintf(d, sizeof(d), "%04d-%02d-%02d", c.year, c.month, c.day); m.pay_date = d;
        m.amount = FromCents(addMoney);
        int mid = insertMoney(m);
        if (mid < 0) return false;
        return addAlloc <= 0 || allocate(mid, chargeId, FromCents(addAlloc));
    }
    long long toRemove = have - want;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT a.id, a.payment_id, CAST(ROUND(a.amount*100) AS INTEGER), CAST(ROUND(m.amount*100) AS INTEGER), "
        "       (SELECT COUNT(*) FROM allocations x WHERE x.payment_id = a.payment_id), "
        "       COALESCE((SELECT status FROM receipts r WHERE r.id = m.receipt_id), '') "
        "FROM allocations a JOIN payments m ON m.id = a.payment_id WHERE a.charge_id = ? ORDER BY a.payment_id DESC", -1, &st, nullptr);
    sqlite3_bind_int(st, 1, chargeId);
    struct Row { long long allocId, payId, alloc, money, nAlloc; std::string rstatus; };
    std::vector<Row> rows;
    while (sqlite3_step(st) == SQLITE_ROW)
        rows.push_back({sqlite3_column_int64(st, 0), sqlite3_column_int64(st, 1), sqlite3_column_int64(st, 2),
                        sqlite3_column_int64(st, 3), sqlite3_column_int64(st, 4), (const char*)sqlite3_column_text(st, 5)});
    sqlite3_finalize(st);
    for (auto& r : rows) {
        if (toRemove <= 0) break;
        if (r.rstatus == "issued" || r.nAlloc != 1) continue;
        long long cut = std::min(toRemove, r.alloc);
        long long newAlloc = r.alloc - cut, newMoney = r.money - cut;
        std::string sql = newAlloc > 0
            ? "UPDATE allocations SET amount = " + std::to_string(newAlloc) + " / 100.0 WHERE id = " + std::to_string(r.allocId) + ";"
            : "DELETE FROM allocations WHERE id = " + std::to_string(r.allocId) + ";";
        sql += newMoney > 0
            ? "UPDATE payments SET amount = " + std::to_string(newMoney) + " / 100.0 WHERE id = " + std::to_string(r.payId) + ";"
            : "DELETE FROM payments WHERE id = " + std::to_string(r.payId) + ";";
        if (!executeQuery(sql)) return false;
        toRemove -= cut;
    }
    if (toRemove > 0) {
        lastError_ = "Η μείωση του πληρωμένου ποσού αφορά πληρωμή με απόδειξη ή πληρωμή που εξοφλεί κι άλλες χρεώσεις. "
                     "Ακυρώστε την απόδειξη ή διορθώστε την πληρωμή από την καρτέλα του πελάτη.";
        return false;
    }
    return true;
}

bool SqliteConn::addPayment(const Payment& in) {
    return addPaymentsWithReceipt({in}, "", nullptr) > 0;
}

int SqliteConn::insertPayment(const Payment& in, int batchId) {   // a charge + the money already paid on it
    int cid = insertCharge(in, batchId);
    if (cid < 0) return -1;
    if (in.status != "future" && ToCents(in.amount_paid) > 0 && !reconcilePaid(cid, in.amount_paid)) return -1;
    return cid;
}

int SqliteConn::addPaymentBatch(const std::vector<Payment>& lines, const std::string& label) {
    if (addPaymentsWithReceipt(lines, label, nullptr) < 0) return -1;
    if (lines.size() < 2) return 0;
    sqlite3_stmt* st = nullptr; int id = -1;
    sqlite3_prepare_v2(db, "SELECT MAX(id) FROM payment_batches", -1, &st, nullptr);
    if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return id;
}

// Charges (one, or several as one batch) + the money paid now as ONE payment per client & direction (a package
// paid at once is one transfer), allocated per charge; optionally its receipt. All or nothing.
int SqliteConn::addPaymentsWithReceipt(const std::vector<Payment>& lines, const std::string& batchLabel, const Receipt* receipt) {
    if (lines.empty()) { lastError_ = "Δεν υπάρχουν πληρωμές για καταχώριση."; return -1; }
    if (!savepoint("add_lines")) return -1;
    auto fail = [&]() { rollbackTo("add_lines"); lastError_ = "Δεν αποθηκεύτηκε τίποτα: " + lastError_; return -1; };
    int batchId = -1;
    if (lines.size() > 1) {
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, "INSERT INTO payment_batches (label) VALUES (?)", -1, &st, nullptr);
        sqlite3_bind_text(st, 1, batchLabel.c_str(), -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) { lastError_ = sqlite3_errmsg(db); return fail(); }
        batchId = (int)sqlite3_last_insert_rowid(db);
    }
    std::vector<int> charges;
    std::map<std::pair<int, std::string>, std::vector<std::pair<int, Payment>>> paidNow;   // (client, direction) -> lines
    for (const auto& p : lines) {
        int cid = insertCharge(p, batchId);
        if (cid < 0) return fail();
        charges.push_back(cid);
        if (p.status != "future" && ToCents(p.amount_paid) > 0)
            paidNow[{p.user_id, p.direction.empty() ? "in" : p.direction}].push_back({cid, p});
    }
    std::vector<int> moneyIds;
    for (auto& [key, group] : paidNow) {
        long long total = 0;
        for (auto& [cid, p] : group) total += ToCents(p.amount_paid);
        MoneyPayment m;
        m.user_id = key.first; m.direction = key.second; m.amount = FromCents(total); m.batch_id = batchId;
        m.pay_date = PayDate(group.front().second);
        if (receipt) { m.method = receipt->method; m.reference = receipt->bank_ref; }
        int mid = insertMoney(m);
        if (mid < 0) return fail();
        moneyIds.push_back(mid);
        for (auto& [cid, p] : group) {
            double alloc = std::min(RoundMoney(p.amount_paid), RoundMoney(p.amount_due));
            if (!allocate(mid, cid, alloc)) return fail();
        }
    }
    if (receipt) {
        if (moneyIds.empty()) { lastError_ = "Η απόδειξη χρειάζεται πληρωμένο ποσό μεγαλύτερο από 0."; return fail(); }
        if (issueReceiptInTx(*receipt, moneyIds) < 0) return fail();
    }
    if (!release("add_lines")) return fail();
    return charges.front();
}

bool SqliteConn::editPayment(int id, const Payment& in) {
    Payment p = in;
    p.amount_due = RoundMoney(p.amount_due);
    Payment old = getPaymentById(id);
    int aptId = p.apartment_id;
    if (aptId < 0) {
        bool moved = old.user_id != p.user_id || old.year != p.year || old.month != p.month || old.day != p.day;
        aptId = moved ? apartmentAt(p.user_id, p.year, p.month, p.day) : old.apartment_id;
    }
    if (!savepoint("edit_line")) return false;
    std::string date = PayDate(p);
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "UPDATE charges SET pay_date=?, amount_due=?, status=?, notes=?, user_id=?, direction=?, "
                           "resolution_date=?, resolved_at=datetime(?8,'utc'), apartment_id=? WHERE id=?", -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt, 2, p.amount_due);
    sqlite3_bind_text(stmt, 3, p.status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, p.notes.c_str(), -1, SQLITE_TRANSIENT);
    if (p.user_id > 0) sqlite3_bind_int(stmt, 5, p.user_id); else sqlite3_bind_null(stmt, 5);
    sqlite3_bind_text(stmt, 6, p.direction.empty() ? "in" : p.direction.c_str(), -1, SQLITE_TRANSIENT);
    if (p.resolution_date.empty()) sqlite3_bind_null(stmt, 7); else sqlite3_bind_text(stmt, 7, p.resolution_date.c_str(), -1, SQLITE_TRANSIENT);
    if (p.resolved_at.empty()) sqlite3_bind_null(stmt, 8); else sqlite3_bind_text(stmt, 8, p.resolved_at.c_str(), -1, SQLITE_TRANSIENT);
    if (aptId > 0) sqlite3_bind_int(stmt, 9, aptId); else sqlite3_bind_null(stmt, 9);
    sqlite3_bind_int(stmt, 10, id);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) lastError_ = sqlite3_errmsg(db);   // e.g. refused: settled with a receipted payment
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) { rollbackTo("edit_line"); return false; }
    // money paid only for this line follows the line's client / date / direction (as the old single-row model did)
    std::string sid = std::to_string(id);
    if (!executeQuery("UPDATE payments SET user_id = (SELECT user_id FROM charges WHERE id = " + sid + "), "
                      "pay_date = (SELECT pay_date FROM charges WHERE id = " + sid + "), "
                      "direction = (SELECT direction FROM charges WHERE id = " + sid + ") "
                      "WHERE id IN (SELECT a.payment_id FROM allocations a WHERE a.charge_id = " + sid + ") "
                      "AND (SELECT COUNT(*) FROM allocations x WHERE x.payment_id = payments.id) = 1 "
                      "AND (receipt_id IS NULL OR (SELECT status FROM receipts r WHERE r.id = payments.receipt_id) = 'void') "
                      "AND (user_id IS NOT (SELECT user_id FROM charges WHERE id = " + sid + ") "
                      "  OR pay_date IS NOT (SELECT pay_date FROM charges WHERE id = " + sid + ") "
                      "  OR direction IS NOT (SELECT direction FROM charges WHERE id = " + sid + "));"))
        { rollbackTo("edit_line"); return false; }
    double desired = p.status == "future" ? 0.0 : p.amount_paid;
    if (!reconcilePaid(id, desired)) { rollbackTo("edit_line"); return false; }
    if (!closePlanIfPaid(old.batch_id)) { rollbackTo("edit_line"); return false; }
    if (!release("edit_line")) { rollbackTo("edit_line"); return false; }
    return true;
}

bool SqliteConn::removePayment(int id) {
    int batch = getPaymentById(id).batch_id;
    if (!savepoint("remove_line")) return false;
    std::string sid = std::to_string(id);
    // the money that paid only this line goes with it; money on an issued receipt is refused by the triggers
    bool ok = executeQuery(
        "CREATE TEMP TABLE IF NOT EXISTS _gone(id INTEGER); DELETE FROM _gone;"
        "INSERT INTO _gone SELECT a.payment_id FROM allocations a WHERE a.charge_id = " + sid +
        "  AND (SELECT COUNT(*) FROM allocations x WHERE x.payment_id = a.payment_id) = 1;"
        "DELETE FROM allocations WHERE charge_id = " + sid + ";"
        "DELETE FROM payments WHERE id IN (SELECT id FROM _gone);"
        "DELETE FROM charges WHERE id = " + sid + ";");
    if (!ok) { rollbackTo("remove_line"); return false; }
    // removing the last open installment can complete a plan -> its one receipt
    if (!closePlanIfPaid(batch)) { rollbackTo("remove_line"); return false; }
    return release("remove_line");
}

// Receipt over money payments. One payment, or payments recorded together (same batch) of one client.
int SqliteConn::issueReceiptInTx(const Receipt& in, const std::vector<int>& moneyIdsIn) {
    auto refuse = [&](const std::string& why) { lastError_ = why; return -1; };
    std::vector<int> ids = moneyIdsIn;
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    if (ids.empty()) return refuse("Δεν υπάρχει είσπραξη για να εκδοθεί απόδειξη (μελλοντική ή απλήρωτη χρέωση;).");
    static const char* kMethods[] = {"cash", "collector", "bank", "ebanking", "card", "iris", "other"};
    if (std::none_of(std::begin(kMethods), std::end(kMethods), [&](const char* m) { return in.method == m; }))
        return refuse("Άγνωστος τρόπος πληρωμής: " + in.method);
    std::string series = in.series.empty() ? "A" : in.series;
    std::string rf = NormalizeRfCode(in.rf_code);
    if (!rf.empty() && !IsValidRfCode(rf)) return refuse("Μη έγκυρος κωδικός RF: " + in.rf_code);

    long long totalCents = 0;
    std::set<int> batches, users;
    bool anyUnbatched = false;
    for (int mid : ids) {
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, "SELECT m.direction, CAST(ROUND(m.amount*100) AS INTEGER), COALESCE(r.status,''), COALESCE(m.batch_id,-1), "
                               "COALESCE(m.user_id,-1) FROM payments m LEFT JOIN receipts r ON r.id = m.receipt_id WHERE m.id = ?", -1, &st, nullptr);
        sqlite3_bind_int(st, 1, mid);
        if (sqlite3_step(st) != SQLITE_ROW) { sqlite3_finalize(st); return refuse("Η πληρωμή #" + std::to_string(mid) + " δεν υπάρχει."); }
        std::string direction = (const char*)sqlite3_column_text(st, 0);
        long long cents = sqlite3_column_int64(st, 1);
        std::string onReceipt = (const char*)sqlite3_column_text(st, 2);
        int batch = sqlite3_column_int(st, 3);
        users.insert(sqlite3_column_int(st, 4));
        sqlite3_finalize(st);
        if (direction != "in")     return refuse("Η πληρωμή #" + std::to_string(mid) + " είναι πληρωμή προς τρίτους, όχι είσπραξη.");
        if (onReceipt == "issued") return refuse("Η πληρωμή #" + std::to_string(mid) + " έχει ήδη απόδειξη.");
        if (batch > 0) batches.insert(batch); else anyUnbatched = true;
        totalCents += cents;
    }
    if (totalCents <= 0) return refuse("Το σύνολο της απόδειξης πρέπει να είναι μεγαλύτερο από 0.");
    if (ids.size() > 1 && (anyUnbatched || batches.size() != 1 || users.size() != 1))
        return refuse("Κοινή απόδειξη επιτρέπεται μόνο για πληρωμές που καταχωρίστηκαν μαζί (π.χ. χρέωση πακέτου) για τον ίδιο πελάτη.");

    int number = nextReceiptNumber(series);
    if (rf.empty()) { char ref[32]; std::snprintf(ref, sizeof(ref), "%s%07d", series.c_str(), number); rf = MakeRfCode(ref); }
    Receipt r = in;
    r.series = series; r.number = number; r.total = FromCents(totalCents); r.rf_code = rf;
    if (r.user_id < 0 && users.size() == 1) r.user_id = *users.begin();
    if (r.issue_date.empty()) {
        std::time_t t = std::time(nullptr); std::tm tm{}; localtime_s(&tm, &t);
        char d[16]; std::strftime(d, sizeof(d), "%Y-%m-%d", &tm); r.issue_date = d;
    }
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "INSERT INTO receipts (series, number, issue_date, user_id, building_id, total, method, rf_code, paper_ref, bank_ref, notes) "
                           "VALUES (?,?,?,?,?,?,?,?,?,?,?)", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, r.series.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (st, 2, r.number);
    sqlite3_bind_text(st, 3, r.issue_date.c_str(), -1, SQLITE_TRANSIENT);
    if (r.user_id > 0) sqlite3_bind_int(st, 4, r.user_id); else sqlite3_bind_null(st, 4);
    if (r.building_id > 0) sqlite3_bind_int(st, 5, r.building_id); else sqlite3_bind_null(st, 5);
    sqlite3_bind_double(st, 6, r.total);
    sqlite3_bind_text(st, 7, r.method.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 8, r.rf_code.c_str(), -1, SQLITE_TRANSIENT);
    auto bindOpt = [&](int i, const std::string& v) { if (v.empty()) sqlite3_bind_null(st, i); else sqlite3_bind_text(st, i, v.c_str(), -1, SQLITE_TRANSIENT); };
    bindOpt(9, r.paper_ref); bindOpt(10, r.bank_ref); bindOpt(11, r.notes);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return refuse(std::string("Αποτυχία έκδοσης απόδειξης: ") + sqlite3_errmsg(db));
    r.id = (int)sqlite3_last_insert_rowid(db);
    std::string list;
    for (int mid : ids) list += (list.empty() ? "" : ",") + std::to_string(mid);
    // the receipt records how the money came in: the payments take its method / reference
    if (!executeQuery("UPDATE payments SET receipt_id = " + std::to_string(r.id) + ", method = '" + r.method + "'" +
                      (r.bank_ref.empty() ? std::string() : ", reference = " + std::string("'") + [&] { std::string e; for (char c : r.bank_ref) { if (c == '\'') e += '\''; e += c; } return e; }() + "'") +
                      " WHERE id IN (" + list + ");"))
        return refuse(std::string("Αποτυχία σύνδεσης πληρωμής: ") + lastError_);
    return r.id;
}

int SqliteConn::issueReceiptForMoney(const Receipt& r, const std::vector<int>& moneyIds) {
    if (!savepoint("receipt")) return -1;
    int id = issueReceiptInTx(r, moneyIds);
    if (id < 0 || !release("receipt")) { rollbackTo("receipt"); return -1; }
    return id;
}

// Receipt for the incoming money paid on these charge lines (money that has no issued receipt yet).
int SqliteConn::issueReceipt(const Receipt& r, const std::vector<int>& chargeIds) {
    std::vector<int> money;
    for (int cid : chargeIds)
        for (auto& m : getMoneyForCharge(cid)) {
            if (m.receipt_id <= 0 || getReceiptById(m.receipt_id).status != "issued") money.push_back(m.id);
        }
    // refusals that are about the lines themselves read better than "no money"
    for (int cid : chargeIds) {
        Payment c = getPaymentById(cid);
        if (c.id < 0) { lastError_ = "Η πληρωμή #" + std::to_string(cid) + " δεν υπάρχει."; return -1; }
        if (c.status == "future") { lastError_ = "Η πληρωμή #" + std::to_string(cid) + " είναι μελλοντική - επιλύστε την πρώτα."; return -1; }
        if (c.direction != "in") { lastError_ = "Η πληρωμή #" + std::to_string(cid) + " είναι πληρωμή προς τρίτους, όχι είσπραξη."; return -1; }
    }
    return issueReceiptForMoney(r, money);
}

bool SqliteConn::editPaymentWithReceipt(int id, const Payment& payment, const Receipt& receipt) {
    if (!savepoint("edit_receipt")) return false;
    if (!editPayment(id, payment)) { rollbackTo("edit_receipt"); lastError_ = "Δεν αποθηκεύτηκε τίποτα: " + lastError_; return false; }
    std::vector<int> money;
    for (auto& m : getMoneyForCharge(id)) {
        if (m.receipt_id <= 0 || getReceiptById(m.receipt_id).status != "issued") money.push_back(m.id);
    }
    if (issueReceiptInTx(receipt, money) < 0 || !release("edit_receipt")) {
        rollbackTo("edit_receipt"); lastError_ = "Δεν αποθηκεύτηκε τίποτα: " + lastError_; return false;
    }
    return true;
}

int SqliteConn::recordPayment(const MoneyPayment& money, const std::vector<Allocation>& allocations, const Receipt* receipt) {
    if (!savepoint("record_money")) return -1;
    auto fail = [&]() { rollbackTo("record_money"); lastError_ = "Δεν αποθηκεύτηκε τίποτα: " + lastError_; return -1; };
    MoneyPayment m = money;
    if (receipt) { m.method = receipt->method; if (m.reference.empty()) m.reference = receipt->bank_ref; }
    if (m.batch_id <= 0 && !allocations.empty()) {   // money that pays only one batch's bills belongs to that batch
        std::set<int> b;
        for (auto& a : allocations) b.insert(getPaymentById(a.charge_id).batch_id);
        if (b.size() == 1 && *b.begin() > 0) m.batch_id = *b.begin();
    }
    int mid = insertMoney(m);
    if (mid < 0) return fail();
    for (auto& a : allocations) {
        // money on a scheduled installment makes it real (resolved today)
        if (!executeQuery("UPDATE charges SET status = 'paid', resolved_at = datetime('now') WHERE id = " +
                          std::to_string(a.charge_id) + " AND status = 'future';")) return fail();
        if (!allocate(mid, a.charge_id, a.amount)) return fail();
    }
    if (receipt && issueReceiptInTx(*receipt, {mid}) < 0) return fail();
    {
        std::set<int> batches;
        for (auto& a : allocations) { int b = getPaymentById(a.charge_id).batch_id; if (b > 0) batches.insert(b); }
        for (int b : batches) if (!closePlanIfPaid(b)) return fail();
    }
    if (!release("record_money")) return fail();
    return mid;
}

static const char* kMoneyCols =
    "m.id, COALESCE(m.user_id,-1), m.pay_date, m.amount, m.direction, m.method, COALESCE(m.reference,''), COALESCE(m.notes,''), "
    "COALESCE(m.receipt_id,-1), COALESCE(m.batch_id,-1), COALESCE(datetime(m.created_at,'localtime'),''), ";

static std::vector<MoneyPayment> QueryMoney(sqlite3* db, const std::string& sql, int bind) {
    std::vector<MoneyPayment> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return out;
    sqlite3_bind_int(st, 1, bind);
    auto txt = [&](int c) { const unsigned char* t = sqlite3_column_text(st, c); return std::string(t ? (const char*)t : ""); };
    while (sqlite3_step(st) == SQLITE_ROW) {
        MoneyPayment m;
        m.id = sqlite3_column_int(st, 0); m.user_id = sqlite3_column_int(st, 1); m.pay_date = txt(2);
        m.amount = RoundMoney(sqlite3_column_double(st, 3)); m.direction = txt(4); m.method = txt(5); m.reference = txt(6);
        m.notes = txt(7); m.receipt_id = sqlite3_column_int(st, 8); m.batch_id = sqlite3_column_int(st, 9); m.created_at = txt(10);
        m.allocated = RoundMoney(sqlite3_column_double(st, 11));
        out.push_back(m);
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<MoneyPayment> SqliteConn::getMoneyForUser(int userId) {
    return QueryMoney(db, std::string("SELECT ") + kMoneyCols +
        "COALESCE((SELECT SUM(CAST(ROUND(a.amount*100) AS INTEGER)) FROM allocations a WHERE a.payment_id = m.id), 0) / 100.0 "
        "FROM payments m WHERE m.user_id = ? ORDER BY m.pay_date DESC, m.id DESC", userId);
}
std::vector<MoneyPayment> SqliteConn::getRecentMoney(int limit) {
    return QueryMoney(db, std::string("SELECT ") + kMoneyCols +
        "COALESCE((SELECT SUM(CAST(ROUND(a.amount*100) AS INTEGER)) FROM allocations a WHERE a.payment_id = m.id), 0) / 100.0 "
        "FROM payments m ORDER BY m.pay_date DESC, m.created_at DESC, m.id DESC LIMIT ?", limit);
}
std::vector<MoneyPayment> SqliteConn::getMoneyForCharge(int chargeId) {
    return QueryMoney(db, std::string("SELECT ") + kMoneyCols + "a.amount "
        "FROM allocations a JOIN payments m ON m.id = a.payment_id WHERE a.charge_id = ? ORDER BY m.pay_date, m.id", chargeId);
}
std::vector<MoneyPayment> SqliteConn::getReceiptMoney(int receiptId) {
    return QueryMoney(db, std::string("SELECT ") + kMoneyCols +
        "COALESCE((SELECT SUM(CAST(ROUND(a.amount*100) AS INTEGER)) FROM allocations a WHERE a.payment_id = m.id), 0) / 100.0 "
        "FROM payments m WHERE m.receipt_id = ? ORDER BY m.id", receiptId);
}

std::vector<Allocation> SqliteConn::getAllocationsForMoney(int paymentId) {
    std::vector<Allocation> out;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "SELECT payment_id, charge_id, amount FROM allocations WHERE payment_id = ? ORDER BY charge_id", -1, &st, nullptr);
    sqlite3_bind_int(st, 1, paymentId);
    while (sqlite3_step(st) == SQLITE_ROW)
        out.push_back({sqlite3_column_int(st, 0), sqlite3_column_int(st, 1), RoundMoney(sqlite3_column_double(st, 2))});
    sqlite3_finalize(st);
    return out;
}

std::vector<Payment> SqliteConn::getOpenCharges(int userId) {
    return QueryPayments(db, std::string("SELECT ") + kPaymentCols +
        // open bills, plus the not-yet-due installments of payment plans (they can be paid early)
        " FROM charges p WHERE p.user_id = ? AND p.balance > 0.005 AND (p.status != 'future' "
        "   OR p.batch_id IN (SELECT batch_id FROM payment_plans)) ORDER BY p.pay_date, p.id", {userId});
}

double SqliteConn::getClientCredit(int userId) {
    double credit = 0.0;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "SELECT COALESCE(SUM(CAST(ROUND(m.amount*100) AS INTEGER) - "
                           "  COALESCE((SELECT SUM(CAST(ROUND(a.amount*100) AS INTEGER)) FROM allocations a WHERE a.payment_id = m.id), 0)), 0) "
                           "FROM payments m WHERE m.user_id = ? AND m.direction = 'in'", -1, &st, nullptr);
    sqlite3_bind_int(st, 1, userId);
    if (sqlite3_step(st) == SQLITE_ROW) credit = FromCents(sqlite3_column_int64(st, 0));
    sqlite3_finalize(st);
    return credit;
}

// charge ids a receipt covers (through its money's allocations)
std::vector<int> SqliteConn::getReceiptPaymentIds(int receiptId) {
    std::vector<int> out;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "SELECT DISTINCT a.charge_id FROM allocations a JOIN payments m ON m.id = a.payment_id "
                           "WHERE m.receipt_id = ? ORDER BY a.charge_id", -1, &st, nullptr);
    sqlite3_bind_int(st, 1, receiptId);
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(sqlite3_column_int(st, 0));
    sqlite3_finalize(st);
    return out;
}

int SqliteConn::getReceiptIdForPayment(int chargeId) {
    Payment c = getPaymentById(chargeId);
    return c.id < 0 ? -1 : c.receipt_id;
}

std::vector<DataConn::ReceiptLine> SqliteConn::getReceiptLines(int receiptId) {
    std::vector<ReceiptLine> out;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "SELECT a.charge_id, SUM(CAST(ROUND(a.amount*100) AS INTEGER)) FROM allocations a JOIN payments m ON m.id = a.payment_id "
                           "WHERE m.receipt_id = ? GROUP BY a.charge_id ORDER BY a.charge_id", -1, &st, nullptr);
    sqlite3_bind_int(st, 1, receiptId);
    std::vector<std::pair<int, long long>> rows;
    while (sqlite3_step(st) == SQLITE_ROW) rows.push_back({sqlite3_column_int(st, 0), sqlite3_column_int64(st, 1)});
    sqlite3_finalize(st);
    for (auto& [cid, cents] : rows) out.push_back({getPaymentById(cid), FromCents(cents)});
    return out;
}

// --- RECEIPTS SEARCH: EXISTS sub-queries over receipt -> money -> allocations -> charges, all indexed ---
static std::string ReceiptWhere(const DataConn::ReceiptQuery& q, std::vector<std::string>& binds) {
    std::string w = " WHERE 1=1";
    if (!q.fromDate.empty()) { w += " AND r.issue_date >= ?"; binds.push_back(q.fromDate); }
    if (!q.toDate.empty())   { w += " AND r.issue_date <= ?"; binds.push_back(q.toDate); }
    if (q.status == 1) w += " AND r.status = 'issued'";
    if (q.status == 2) w += " AND r.status = 'void'";
    if (!q.number.empty()) {
        w += " AND instr(fold(r.series || '-' || r.number || ' ' || COALESCE(r.paper_ref,'') || ' ' || COALESCE(r.bank_ref,'') || ' ' || COALESCE(r.rf_code,'')), fold(?)) > 0";
        binds.push_back(q.number);
    }
    if (!q.text.empty()) {
        w += " AND (instr(fold(r.series || '-' || r.number || ' ' || printf('%06d', r.number) || ' ' || COALESCE(r.paper_ref,'') || ' ' || "
             "COALESCE(r.bank_ref,'') || ' ' || COALESCE(r.rf_code,'')), fold(?)) > 0 "
             "OR instr(fold(COALESCE((SELECT full_name FROM users u WHERE u.id = r.user_id),'')), fold(?)) > 0)";
        binds.push_back(q.text); binds.push_back(q.text);
    }
    const std::string covers = "SELECT 1 FROM payments m JOIN allocations a ON a.payment_id = m.id JOIN charges c ON c.id = a.charge_id WHERE m.receipt_id = r.id";
    std::string conds;
    for (auto& c : q.conds) {
        std::string e;
        std::string id = std::to_string(c.id);
        switch (c.field) {
            case DataConn::ReceiptCond::Client:   e = "(r.user_id = " + id + " OR EXISTS (" + covers + " AND c.user_id = " + id + "))"; break;
            case DataConn::ReceiptCond::Service:  e = "EXISTS (" + covers + " AND c.service_id = " + id + ")"; break;
            case DataConn::ReceiptCond::Package:  e = "EXISTS (" + covers + " AND c.service_id IN (SELECT service_id FROM master_service_components WHERE master_service_id = " + id + "))"; break;
            case DataConn::ReceiptCond::Building: e = "EXISTS (" + covers + " AND c.apartment_id IN (SELECT id FROM apartments WHERE building_id = " + id + "))"; break;
            case DataConn::ReceiptCond::Method:   e = "r.method = ?"; binds.push_back(c.text); break;
        }
        if (c.negate) e = "NOT " + e;
        conds += (conds.empty() ? "" : (q.matchAll ? " AND " : " OR ")) + e;
    }
    if (!conds.empty()) w += " AND (" + conds + ")";
    return w;
}

std::vector<Receipt> SqliteConn::searchReceipts(const ReceiptQuery& q, int offset, int limit) {
    std::vector<std::string> binds;
    std::string sql = std::string("SELECT ") + kReceiptCols + " FROM receipts r" + ReceiptWhere(q, binds) +
                      " ORDER BY r.issue_date DESC, r.id DESC LIMIT ? OFFSET ?";
    std::vector<Receipt> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) { lastError_ = sqlite3_errmsg(db); return out; }
    int i = 1;
    for (auto& b : binds) sqlite3_bind_text(st, i++, b.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, i++, limit); sqlite3_bind_int(st, i++, offset);
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(ReadReceipt(st));
    sqlite3_finalize(st);
    return out;
}

long long SqliteConn::countReceipts(const ReceiptQuery& q, double* total) {
    std::vector<std::string> binds;
    std::string sql = "SELECT COUNT(*), TOTAL(CASE WHEN r.status = 'issued' THEN CAST(ROUND(r.total*100) AS INTEGER) END) FROM receipts r" + ReceiptWhere(q, binds);
    long long n = 0;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) { lastError_ = sqlite3_errmsg(db); return 0; }
    int i = 1;
    for (auto& b : binds) sqlite3_bind_text(st, i++, b.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) { n = sqlite3_column_int64(st, 0); if (total) *total = FromCents((long long)sqlite3_column_double(st, 1)); }
    sqlite3_finalize(st);
    return n;
}

// --- ATTACHMENTS ------------------------------------------------------------------------------------------
static std::string LowerExt(const std::string& path) {
    std::string e = std::filesystem::path(path).extension().string();
    for (auto& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}
static std::string KindOf(const std::string& ext) {
    static const char* img[] = { ".jpg", ".jpeg", ".png", ".bmp", ".gif", ".tga", ".psd", ".pnm", ".ppm" };
    static const char* vid[] = { ".mp4", ".mov", ".m4v", ".avi", ".mkv", ".webm", ".3gp", ".wmv", ".mpg", ".mpeg" };
    for (auto* e : img) if (ext == e) return "image";
    for (auto* e : vid) if (ext == e) return "video";
    if (ext == ".pdf") return "pdf";
    return "other";
}

// SHA-256 of a file, read in 1 MB chunks (videos can be large)
static std::string FileSha256(const std::string& path, long long& size) {
    std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
    if (!in) return "";
    Sha256 h; std::vector<char> buf(1 << 20); size = 0;
    while (in) {
        in.read(buf.data(), (std::streamsize)buf.size());
        std::streamsize got = in.gcount();
        if (got <= 0) break;
        h.Update(buf.data(), (size_t)got); size += got;
    }
    return h.HexDigest();
}

int SqliteConn::addAttachment(const std::string& entity, int entityId, const std::string& sourcePath, const std::string& note) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_regular_file(fs::u8path(sourcePath), ec)) { lastError_ = "Το αρχείο δεν βρέθηκε: " + sourcePath; return -1; }
    long long size = 0;
    std::string hash = FileSha256(sourcePath, size);
    if (hash.empty()) { lastError_ = "Το αρχείο δεν διαβάζεται: " + sourcePath; return -1; }
    std::string ext = LowerExt(sourcePath);
    std::string rel = "attachments/" + hash.substr(0, 2) + "/" + hash + ext;
    fs::path base = fs::path(dbPath_).parent_path();
    fs::path dst = base / fs::u8path(rel);
    fs::create_directories(dst.parent_path(), ec);
    if (!fs::exists(dst, ec)) {
        fs::copy_file(fs::u8path(sourcePath), dst, ec);
        if (ec) { lastError_ = "Αποτυχία αντιγραφής του αρχείου: " + ec.message(); return -1; }
    }
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "INSERT INTO attachments (entity, entity_id, sha256, file_name, kind, size_bytes, stored_path, note) VALUES (?,?,?,?,?,?,?,?)", -1, &st, nullptr);
    std::string name = fs::u8path(sourcePath).filename().u8string();
    std::string kind = KindOf(ext);
    sqlite3_bind_text(st, 1, entity.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, entityId);
    sqlite3_bind_text(st, 3, hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 6, size);
    sqlite3_bind_text(st, 7, rel.c_str(), -1, SQLITE_TRANSIENT);
    if (note.empty()) sqlite3_bind_null(st, 8); else sqlite3_bind_text(st, 8, note.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) lastError_ = sqlite3_errmsg(db);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? (int)sqlite3_last_insert_rowid(db) : -1;
}

std::vector<Attachment> SqliteConn::getAttachments(const std::string& entity, int entityId, bool includeRemoved) {
    std::vector<Attachment> out;
    sqlite3_stmt* st = nullptr;
    std::string sql = "SELECT id, entity, entity_id, sha256, file_name, kind, size_bytes, stored_path, COALESCE(note,''), "
                      "COALESCE(datetime(created_at,'localtime'),''), COALESCE(datetime(removed_at,'localtime'),''), COALESCE(removed_reason,'') "
                      "FROM attachments WHERE entity = ? AND entity_id = ?";
    if (!includeRemoved) sql += " AND removed_at IS NULL";
    sql += " ORDER BY id";
    sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr);
    sqlite3_bind_text(st, 1, entity.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, entityId);
    auto txt = [&](int c) { const unsigned char* t = sqlite3_column_text(st, c); return std::string(t ? (const char*)t : ""); };
    while (sqlite3_step(st) == SQLITE_ROW) {
        Attachment a;
        a.id = sqlite3_column_int(st, 0); a.entity = txt(1); a.entity_id = sqlite3_column_int(st, 2); a.sha256 = txt(3);
        a.file_name = txt(4); a.kind = txt(5); a.size_bytes = sqlite3_column_int64(st, 6); a.stored_path = txt(7);
        a.note = txt(8); a.created_at = txt(9); a.removed_at = txt(10); a.removed_reason = txt(11);
        out.push_back(a);
    }
    sqlite3_finalize(st);
    return out;
}

bool SqliteConn::removeAttachment(int id, const std::string& reason) {
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "UPDATE attachments SET removed_at = datetime('now'), removed_reason = ? WHERE id = ?", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, reason.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, id);
    int rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) lastError_ = sqlite3_errmsg(db);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

std::string SqliteConn::attachmentFullPath(const Attachment& a) {
    return (std::filesystem::path(dbPath_).parent_path() / std::filesystem::u8path(a.stored_path)).u8string();
}

// --- ΚΟΙΝΟΧΡΗΣΤΑ: a cost split across a collection of apartments ------------------------------------------
std::vector<JointService> SqliteConn::getJointCatalog(bool includeInactive) {
    std::vector<JointService> out;
    sqlite3_stmt* st = nullptr;
    std::string sql = "SELECT j.id, s.id, s.label, s.extra_notes, s.created_at, j.kind, j.active FROM joint_services j "
                      "JOIN services s ON s.id = j.service_id";
    if (!includeInactive) sql += " WHERE j.active = 1";
    sql += " ORDER BY s.label";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) { lastError_ = sqlite3_errmsg(db); return out; }
    while (sqlite3_step(st) == SQLITE_ROW) {
        JointService j;
        j.id = sqlite3_column_int(st, 0);
        j.service.id = sqlite3_column_int(st, 1);
        j.service.label = ColText(st, 2); j.service.extra_notes = ColText(st, 3); j.service.created_at = ColText(st, 4);
        j.kind = ColText(st, 5); j.active = sqlite3_column_int(st, 6) != 0;
        out.push_back(j);
    }
    sqlite3_finalize(st);
    for (auto& j : out) j.service.suggested_price = priceOf(j.service.id);
    return out;
}

int SqliteConn::addJointService(const Service& s, const std::string& kind) {
    if (s.label.empty()) { lastError_ = "Η κοινόχρηστη υπηρεσία χρειάζεται όνομα."; return -1; }
    Service in = s;
    if (in.suggested_price <= 0.0) in.suggested_price = 0.01;   // services carry a price hint; the real total is asked when charging
    if (!savepoint("joint_svc")) return -1;
    int sid = addServiceGetId(-1, in);
    if (sid < 0) { if (lastError_.empty()) lastError_ = sqlite3_errmsg(db); rollbackTo("joint_svc"); return -1; }
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "INSERT INTO joint_services (service_id, kind) VALUES (?, ?)", -1, &st, nullptr);
    sqlite3_bind_int(st, 1, sid);
    sqlite3_bind_text(st, 2, kind.empty() ? "equal" : kind.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) lastError_ = sqlite3_errmsg(db);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { rollbackTo("joint_svc"); return -1; }
    int id = (int)sqlite3_last_insert_rowid(db);
    if (!release("joint_svc")) { rollbackTo("joint_svc"); return -1; }
    return id;
}

bool SqliteConn::editJointService(int id, const Service& s, const std::string& kind, bool active) {
    int sid = -1;
    {
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, "SELECT service_id FROM joint_services WHERE id = ?", -1, &st, nullptr);
        sqlite3_bind_int(st, 1, id);
        if (sqlite3_step(st) == SQLITE_ROW) sid = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    if (sid < 0) { lastError_ = "Η κοινόχρηστη υπηρεσία δεν υπάρχει."; return false; }
    if (!savepoint("joint_svc_edit")) return false;
    Service in = s;
    if (in.suggested_price <= 0.0) in.suggested_price = 0.01;
    bool ok = editService(sid, in);
    if (ok) {
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, "UPDATE joint_services SET kind = ?, active = ? WHERE id = ?", -1, &st, nullptr);
        sqlite3_bind_text(st, 1, kind.empty() ? "equal" : kind.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, active ? 1 : 0);
        sqlite3_bind_int(st, 3, id);
        ok = sqlite3_step(st) == SQLITE_DONE;
        if (!ok) lastError_ = sqlite3_errmsg(db);
        sqlite3_finalize(st);
    }
    if (!ok) { rollbackTo("joint_svc_edit"); return false; }
    return release("joint_svc_edit");
}

int SqliteConn::chargeJoint(const JointOp& opIn, const std::vector<JointLine>& lines, const std::vector<JointShare>& shares) {
    JointOp op = opIn;
    long long total = ToCents(op.total);
    auto refuse = [&](const std::string& why) { lastError_ = why; return -1; };
    if (total <= 0) return refuse("Το ποσό του κοινόχρηστου πρέπει να είναι μεγαλύτερο από 0.");
    if (op.label.empty()) return refuse("Η κοινόχρηστη χρέωση χρειάζεται περιγραφή.");
    if (op.op_date.size() != 10) return refuse("Μη έγκυρη ημερομηνία χρέωσης.");
    long long linesSum = 0;
    for (auto& l : lines) { if (l.service_id <= 0 || ToCents(l.total) < 0) return refuse("Μη έγκυρη γραμμή χρέωσης."); linesSum += ToCents(l.total); }
    if (lines.empty() || linesSum != total) return refuse("Οι γραμμές της χρέωσης δεν αθροίζουν στο σύνολο.");
    long long sharesSum = 0; int nIncluded = 0;
    std::set<std::pair<int,int>> seen;
    for (auto& sh : shares) {
        if (sh.apartment_id <= 0) return refuse("Κάθε μερίδιο χρειάζεται διαμέρισμα.");
        if (!seen.insert({sh.apartment_id, sh.user_id}).second) return refuse("Το ίδιο διαμέρισμα/ένοικος εμφανίζεται δύο φορές.");
        if (sh.included) {
            if (sh.user_id <= 0) return refuse("Μερίδιο χωρίς ένοικο δεν μπορεί να χρεωθεί.");
            if (ToCents(sh.amount) <= 0) return refuse("Κάθε μερίδιο που χρεώνεται πρέπει να είναι μεγαλύτερο από 0.");
            sharesSum += ToCents(sh.amount); nIncluded++;
        } else if (sh.exclude_reason.find_first_not_of(" \t") == std::string::npos) {
            return refuse("Κάθε εξαίρεση χρειάζεται αιτιολογία.");
        }
    }
    if (nIncluded == 0) return refuse("Δεν επιλέχθηκε κανένα διαμέρισμα για χρέωση.");
    if (sharesSum != total)
        return refuse("Τα μερίδια αθροίζουν " + FormatMoney(FromCents(sharesSum)) + " € αντί για " + FormatMoney(op.total) +
                      " €. Το κοινόχρηστο μοιράζεται, δεν πολλαπλασιάζεται.");

    if (!savepoint("joint_charge")) return -1;
    auto fail = [&]() { rollbackTo("joint_charge"); lastError_ = "Δεν αποθηκεύτηκε τίποτα: " + lastError_; return -1; };
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "INSERT INTO payment_batches (label) VALUES (?)", -1, &st, nullptr);
    std::string batchLabel = "Κοινόχρηστο: " + op.label;
    sqlite3_bind_text(st, 1, batchLabel.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st); sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { lastError_ = sqlite3_errmsg(db); return fail(); }
    int batchId = (int)sqlite3_last_insert_rowid(db);

    sqlite3_prepare_v2(db, "INSERT INTO joint_ops (building_id, source_type, source_id, kind, label, total, op_date, params_json, note, batch_id) "
                           "VALUES (?,?,?,?,?,?,?,?,?,?)", -1, &st, nullptr);
    if (op.building_id > 0) sqlite3_bind_int(st, 1, op.building_id); else sqlite3_bind_null(st, 1);
    sqlite3_bind_text(st, 2, op.source_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, op.source_id);
    sqlite3_bind_text(st, 4, op.kind.empty() ? "equal" : op.kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, op.label.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(st, 6, FromCents(total));
    sqlite3_bind_text(st, 7, op.op_date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 8, op.params_json.empty() ? "{}" : op.params_json.c_str(), -1, SQLITE_TRANSIENT);
    if (op.note.empty()) sqlite3_bind_null(st, 9); else sqlite3_bind_text(st, 9, op.note.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 10, batchId);
    rc = sqlite3_step(st); sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { lastError_ = sqlite3_errmsg(db); return fail(); }
    int opId = (int)sqlite3_last_insert_rowid(db);

    int y = std::atoi(op.op_date.substr(0, 4).c_str()), m = std::atoi(op.op_date.substr(5, 2).c_str()), d = std::atoi(op.op_date.substr(8, 2).c_str());
    for (auto& sh : shares) {
        sqlite3_prepare_v2(db, "INSERT INTO joint_shares (op_id, apartment_id, user_id, amount, included, exclude_reason) VALUES (?,?,?,?,?,?)", -1, &st, nullptr);
        sqlite3_bind_int(st, 1, opId);
        sqlite3_bind_int(st, 2, sh.apartment_id);
        if (sh.user_id > 0) sqlite3_bind_int(st, 3, sh.user_id); else sqlite3_bind_null(st, 3);
        sqlite3_bind_double(st, 4, sh.included ? RoundMoney(sh.amount) : 0.0);
        sqlite3_bind_int(st, 5, sh.included ? 1 : 0);
        if (sh.included || sh.exclude_reason.empty()) sqlite3_bind_null(st, 6); else sqlite3_bind_text(st, 6, sh.exclude_reason.c_str(), -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(st); sqlite3_finalize(st);
        if (rc != SQLITE_DONE) { lastError_ = sqlite3_errmsg(db); return fail(); }
        if (!sh.included) continue;

        // the share is spread over the lines in proportion to their totals, in cents, remainder to the first lines
        long long shareC = ToCents(sh.amount), given = 0;
        std::vector<long long> parts(lines.size());
        for (size_t i = 0; i < lines.size(); i++) { parts[i] = shareC * ToCents(lines[i].total) / total; given += parts[i]; }
        for (size_t i = 0; given < shareC; i = (i + 1) % lines.size())
            if (ToCents(lines[i].total) > 0) { parts[i]++; given++; }
        for (size_t i = 0; i < lines.size(); i++) {
            if (parts[i] <= 0) continue;
            Payment p;
            p.service_id = lines[i].service_id; p.user_id = sh.user_id; p.apartment_id = sh.apartment_id;
            p.year = y; p.month = m; p.day = d;
            p.amount_due = FromCents(parts[i]); p.amount_paid = 0.0;
            p.status = "pending"; p.direction = "in";
            p.notes = "Κοινόχρηστο: " + op.label;
            if (insertCharge(p, batchId) < 0) return fail();
        }
    }
    if (!release("joint_charge")) return fail();
    return opId;
}

static const char* kJointOpCols =
    "o.id, COALESCE(o.building_id, -1), o.source_type, o.source_id, o.kind, o.label, o.total, o.op_date, o.params_json, "
    "COALESCE(o.note,''), o.batch_id, datetime(o.created_at,'localtime'), COALESCE(b.address,''), "
    "(SELECT COUNT(*) FROM joint_shares s WHERE s.op_id = o.id AND s.included = 1), "
    "(SELECT COUNT(*) FROM joint_shares s WHERE s.op_id = o.id AND s.included = 0), "
    "(SELECT COALESCE(SUM(CAST(ROUND(c.amount_paid*100) AS INTEGER)), 0) FROM charges c WHERE c.batch_id = o.batch_id) ";

static JointOp ReadJointOp(sqlite3_stmt* st) {
    JointOp o;
    o.id = sqlite3_column_int(st, 0); o.building_id = sqlite3_column_int(st, 1);
    o.source_type = ColText(st, 2); o.source_id = sqlite3_column_int(st, 3); o.kind = ColText(st, 4);
    o.label = ColText(st, 5); o.total = RoundMoney(sqlite3_column_double(st, 6)); o.op_date = ColText(st, 7);
    o.params_json = ColText(st, 8); o.note = ColText(st, 9); o.batch_id = sqlite3_column_int(st, 10);
    o.created_at = ColText(st, 11); o.building_address = ColText(st, 12);
    o.included = sqlite3_column_int(st, 13); o.excluded = sqlite3_column_int(st, 14);
    o.paid = FromCents(sqlite3_column_int64(st, 15));
    return o;
}

std::vector<JointOp> SqliteConn::getJointOps(int buildingId, int offset, int limit) {
    std::vector<JointOp> out;
    std::string sql = std::string("SELECT ") + kJointOpCols + "FROM joint_ops o LEFT JOIN buildings b ON b.id = o.building_id ";
    if (buildingId > 0) sql += "WHERE o.building_id = ?1 ";
    sql += "ORDER BY o.op_date DESC, o.id DESC LIMIT ?2 OFFSET ?3";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) { lastError_ = sqlite3_errmsg(db); return out; }
    if (buildingId > 0) sqlite3_bind_int(st, 1, buildingId);
    sqlite3_bind_int(st, 2, limit); sqlite3_bind_int(st, 3, offset);
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(ReadJointOp(st));
    sqlite3_finalize(st);
    return out;
}

int SqliteConn::countJointOps(int buildingId) {
    sqlite3_stmt* st = nullptr; int n = 0;
    sqlite3_prepare_v2(db, buildingId > 0 ? "SELECT COUNT(*) FROM joint_ops WHERE building_id = ?" : "SELECT COUNT(*) FROM joint_ops", -1, &st, nullptr);
    if (buildingId > 0) sqlite3_bind_int(st, 1, buildingId);
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

JointOp SqliteConn::getJointOpById(int id) {
    JointOp o;
    std::string sql = std::string("SELECT ") + kJointOpCols + "FROM joint_ops o LEFT JOIN buildings b ON b.id = o.building_id WHERE o.id = ?";
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr);
    sqlite3_bind_int(st, 1, id);
    if (sqlite3_step(st) == SQLITE_ROW) o = ReadJointOp(st);
    sqlite3_finalize(st);
    return o;
}

static const char* kJointShareSql =
    "SELECT s.id, s.op_id, s.apartment_id, COALESCE(s.user_id, -1), s.amount, s.included, COALESCE(s.exclude_reason,''), "
    "       COALESCE(a.label,''), COALESCE(u.full_name,''), "
    "       (SELECT COALESCE(SUM(CAST(ROUND(c.amount_paid*100) AS INTEGER)), 0) FROM charges c "
    "        WHERE c.batch_id = o.batch_id AND c.apartment_id = s.apartment_id AND c.user_id IS s.user_id), "
    "       o.label, o.op_date, COALESCE(b.address,''), o.total "
    "FROM joint_shares s JOIN joint_ops o ON o.id = s.op_id "
    "LEFT JOIN apartments a ON a.id = s.apartment_id LEFT JOIN users u ON u.id = s.user_id "
    "LEFT JOIN buildings b ON b.id = o.building_id ";

static std::vector<JointShare> ReadJointShares(sqlite3_stmt* st) {
    std::vector<JointShare> out;
    while (sqlite3_step(st) == SQLITE_ROW) {
        JointShare s;
        s.id = sqlite3_column_int(st, 0); s.op_id = sqlite3_column_int(st, 1); s.apartment_id = sqlite3_column_int(st, 2);
        s.user_id = sqlite3_column_int(st, 3); s.amount = RoundMoney(sqlite3_column_double(st, 4));
        s.included = sqlite3_column_int(st, 5) != 0; s.exclude_reason = ColText(st, 6);
        s.apartment_label = ColText(st, 7); s.user_name = ColText(st, 8); s.paid = FromCents(sqlite3_column_int64(st, 9));
        s.op_label = ColText(st, 10); s.op_date = ColText(st, 11); s.building_address = ColText(st, 12);
        s.op_total = RoundMoney(sqlite3_column_double(st, 13));
        out.push_back(s);
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<JointShare> SqliteConn::getJointShares(int opId) {
    sqlite3_stmt* st = nullptr;
    std::string sql = std::string(kJointShareSql) + "WHERE s.op_id = ? ORDER BY s.included DESC, a.label, s.id";
    sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr);
    sqlite3_bind_int(st, 1, opId);
    return ReadJointShares(st);
}

std::vector<JointShare> SqliteConn::getJointSharesFor(int userId, int apartmentId) {
    sqlite3_stmt* st = nullptr;
    std::string sql = std::string(kJointShareSql) + "WHERE (?1 < 0 OR s.user_id = ?1) AND (?2 < 0 OR s.apartment_id = ?2) "
                      "ORDER BY o.op_date DESC, o.id DESC, s.id";
    sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr);
    sqlite3_bind_int(st, 1, userId); sqlite3_bind_int(st, 2, apartmentId);
    return ReadJointShares(st);
}

std::vector<int> SqliteConn::getJointOpCharges(int opId) {
    std::vector<int> out;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "SELECT c.id FROM charges c JOIN joint_ops o ON o.batch_id = c.batch_id WHERE o.id = ? ORDER BY c.id", -1, &st, nullptr);
    sqlite3_bind_int(st, 1, opId);
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(sqlite3_column_int(st, 0));
    sqlite3_finalize(st);
    return out;
}

// --- INSTALLMENT PLANS -----------------------------------------------------------------------------------------
int SqliteConn::addInstallmentPlan(const std::vector<Payment>& lines, const std::string& label, int installments,
                                   const std::string& every, const std::string& receiptMode, const Receipt* firstReceipt) {
    if (installments < 2) { lastError_ = "Οι δόσεις πρέπει να είναι τουλάχιστον 2."; return -1; }
    if (lines.size() < 2) { lastError_ = "Το πρόγραμμα δόσεων χρειάζεται τουλάχιστον 2 χρεώσεις."; return -1; }
    long long total = 0; for (auto& l : lines) total += ToCents(l.amount_due);
    if (total <= 0) { lastError_ = "Το ποσό πρέπει να είναι μεγαλύτερο από 0."; return -1; }
    if (!savepoint("plan")) return -1;
    auto fail = [&]() { rollbackTo("plan"); lastError_ = "Δεν αποθηκεύτηκε τίποτα: " + lastError_; return -1; };
    bool each = receiptMode == "each";
    int first = addPaymentsWithReceipt(lines, "Δόσεις: " + label, each ? firstReceipt : nullptr);
    if (first < 0) return fail();
    int batchId = getPaymentById(first).batch_id;
    if (!each && firstReceipt) {          // keep how the first installment was paid; its receipt comes at the end
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, "UPDATE payments SET method = ?, reference = NULLIF(?, '') WHERE batch_id = ? AND receipt_id IS NULL", -1, &st, nullptr);
        sqlite3_bind_text(st, 1, firstReceipt->method.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, firstReceipt->bank_ref.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 3, batchId);
        int rc = sqlite3_step(st); sqlite3_finalize(st);
        if (rc != SQLITE_DONE) { lastError_ = sqlite3_errmsg(db); return fail(); }
    }
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "INSERT INTO payment_plans (batch_id, installments, every, receipt_mode, total) VALUES (?,?,?,?,?)", -1, &st, nullptr);
    sqlite3_bind_int(st, 1, batchId); sqlite3_bind_int(st, 2, installments);
    sqlite3_bind_text(st, 3, every.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, each ? "each" : "end", -1, SQLITE_STATIC);
    sqlite3_bind_double(st, 5, FromCents(total));
    int rc = sqlite3_step(st); sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { lastError_ = sqlite3_errmsg(db); return fail(); }
    int planId = (int)sqlite3_last_insert_rowid(db);
    if (!closePlanIfPaid(batchId)) return fail();
    if (!release("plan")) return fail();
    return planId;
}

// An "end"-mode plan whose installments are all paid gets its ONE receipt now, for all the money paid on it that has
// no receipt yet. Called inside the caller's transaction after money is allocated.
bool SqliteConn::closePlanIfPaid(int batchId) {
    if (batchId <= 0) return true;
    PaymentPlan plan = getPlanForBatch(batchId);
    if (plan.id < 0 || plan.receipt_mode != "end" || plan.receipt_id > 0) return true;
    sqlite3_stmt* st = nullptr; int open = 0;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM charges WHERE batch_id = ? AND (status = 'future' OR CAST(ROUND(balance*100) AS INTEGER) > 0)", -1, &st, nullptr);
    sqlite3_bind_int(st, 1, batchId);
    if (sqlite3_step(st) == SQLITE_ROW) open = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    if (open > 0) return true;
    std::vector<int> ids; int user = -1; std::string method = "cash", date;
    sqlite3_prepare_v2(db,
        "SELECT DISTINCT m.id, COALESCE(m.user_id,-1), m.method, m.pay_date FROM payments m "
        "JOIN allocations a ON a.payment_id = m.id JOIN charges c ON c.id = a.charge_id "
        "WHERE c.batch_id = ? AND m.direction = 'in' "
        "AND (m.receipt_id IS NULL OR (SELECT status FROM receipts r WHERE r.id = m.receipt_id) = 'void') ORDER BY m.pay_date, m.id", -1, &st, nullptr);
    sqlite3_bind_int(st, 1, batchId);
    while (sqlite3_step(st) == SQLITE_ROW) {
        ids.push_back(sqlite3_column_int(st, 0)); user = sqlite3_column_int(st, 1);
        std::string mth = ColText(st, 2); if (mth != "unspecified" && !mth.empty()) method = mth;
        date = ColText(st, 3);
    }
    sqlite3_finalize(st);
    if (ids.empty()) return true;
    Receipt r; r.user_id = user; r.issue_date = date; r.method = method;
    r.notes = "Εξόφληση σε " + std::to_string(plan.installments) + " δόσεις — " + plan.label;
    return issueReceiptInTx(r, ids) > 0;
}

DataConn::PaymentPlan SqliteConn::getPlanForBatch(int batchId) {
    PaymentPlan p;
    if (batchId <= 0) return p;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT pl.id, pl.batch_id, pl.installments, pl.every, pl.receipt_mode, COALESCE(b.label,''), datetime(pl.created_at,'localtime'), pl.total, "
        "  (SELECT COALESCE(SUM(CAST(ROUND(c.amount_paid*100) AS INTEGER)),0) FROM charges c WHERE c.batch_id = pl.batch_id), "
        "  (SELECT COUNT(DISTINCT c.pay_date) FROM charges c WHERE c.batch_id = pl.batch_id AND c.status != 'future' "
        "     AND NOT EXISTS (SELECT 1 FROM charges x WHERE x.batch_id = c.batch_id AND x.pay_date = c.pay_date "
        "                     AND (x.status = 'future' OR CAST(ROUND(x.balance*100) AS INTEGER) > 0))), "
        "  COALESCE((SELECT m.receipt_id FROM payments m JOIN allocations a ON a.payment_id = m.id JOIN charges c ON c.id = a.charge_id "
        "            JOIN receipts r ON r.id = m.receipt_id WHERE c.batch_id = pl.batch_id AND r.status = 'issued' "
        "            AND r.notes LIKE 'Εξόφληση σε %' LIMIT 1), -1) "
        "FROM payment_plans pl LEFT JOIN payment_batches b ON b.id = pl.batch_id WHERE pl.batch_id = ?", -1, &st, nullptr);
    sqlite3_bind_int(st, 1, batchId);
    if (sqlite3_step(st) == SQLITE_ROW) {
        p.id = sqlite3_column_int(st, 0); p.batch_id = sqlite3_column_int(st, 1); p.installments = sqlite3_column_int(st, 2);
        p.every = ColText(st, 3); p.receipt_mode = ColText(st, 4); p.label = ColText(st, 5); p.created_at = ColText(st, 6);
        p.total = RoundMoney(sqlite3_column_double(st, 7)); p.paid = FromCents(sqlite3_column_int64(st, 8));
        p.paidInstallments = sqlite3_column_int(st, 9); p.receipt_id = sqlite3_column_int(st, 10);
        if (p.label.rfind("Δόσεις: ", 0) == 0) p.label = p.label.substr(std::string("Δόσεις: ").size());
    }
    sqlite3_finalize(st);
    return p;
}

// --- REPORT QUERIES ----------------------------------------------------------------------------------------------
std::vector<DataConn::MethodTotal> SqliteConn::moneyByMethod(const std::string& fromDate, const std::string& toDate) {
    std::vector<MethodTotal> out;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT m.method, COUNT(*), SUM(CAST(ROUND(m.amount*100) AS INTEGER)), "
        "  SUM(CASE WHEN EXISTS (SELECT 1 FROM receipts r WHERE r.id = m.receipt_id AND r.status = 'issued') "
        "      THEN CAST(ROUND(m.amount*100) AS INTEGER) ELSE 0 END) "
        "FROM payments m WHERE m.direction = 'in' AND (?1 = '' OR m.pay_date >= ?1) AND (?2 = '' OR m.pay_date <= ?2) "
        "GROUP BY m.method ORDER BY 3 DESC", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, fromDate.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, toDate.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        MethodTotal t; t.method = ColText(st, 0); t.count = sqlite3_column_int64(st, 1);
        t.total = FromCents(sqlite3_column_int64(st, 2)); t.receipted = FromCents(sqlite3_column_int64(st, 3));
        out.push_back(t);
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<DataConn::DebtRow> SqliteConn::debtAging(const std::string& asOf) {
    std::vector<DebtRow> out;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db,
        "SELECT c.user_id, "
        "  SUM(CASE WHEN julianday(?1) - julianday(c.pay_date) <= 30 THEN CAST(ROUND(c.balance*100) AS INTEGER) ELSE 0 END), "
        "  SUM(CASE WHEN julianday(?1) - julianday(c.pay_date) > 30 AND julianday(?1) - julianday(c.pay_date) <= 90 "
        "      THEN CAST(ROUND(c.balance*100) AS INTEGER) ELSE 0 END), "
        "  SUM(CASE WHEN julianday(?1) - julianday(c.pay_date) > 90 THEN CAST(ROUND(c.balance*100) AS INTEGER) ELSE 0 END), "
        "  SUM(CAST(ROUND(c.balance*100) AS INTEGER)), MIN(c.pay_date) "
        "FROM charges c WHERE c.status != 'future' AND c.direction = 'in' AND CAST(ROUND(c.balance*100) AS INTEGER) > 0 "
        "  AND c.user_id IS NOT NULL AND c.pay_date <= ?1 "
        "GROUP BY c.user_id ORDER BY 5 DESC", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, asOf.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        DebtRow d; d.user_id = sqlite3_column_int(st, 0);
        d.upTo30 = FromCents(sqlite3_column_int64(st, 1)); d.upTo90 = FromCents(sqlite3_column_int64(st, 2));
        d.over90 = FromCents(sqlite3_column_int64(st, 3)); d.total = FromCents(sqlite3_column_int64(st, 4)); d.oldest = ColText(st, 5);
        out.push_back(d);
    }
    sqlite3_finalize(st);
    return out;
}

// --- OFFERS ------------------------------------------------------------------------------------------------------
static const char* kOfferCols =
    "o.id, o.name, o.kind, o.value, COALESCE(o.user_id,-1), o.target_type, COALESCE(o.target_id,-1), o.valid_from, o.valid_to, "
    "o.status, COALESCE(o.note,''), COALESCE(o.cancel_reason,''), datetime(o.created_at,'localtime'), "
    "(SELECT COUNT(*) FROM offer_uses u WHERE u.offer_id = o.id), "
    "(SELECT COALESCE(SUM(CAST(ROUND(u.discount*100) AS INTEGER)),0) FROM offer_uses u WHERE u.offer_id = o.id) ";

static std::vector<Offer> ReadOffers(sqlite3_stmt* st) {
    std::vector<Offer> out;
    while (sqlite3_step(st) == SQLITE_ROW) {
        Offer o;
        o.id = sqlite3_column_int(st, 0); o.name = ColText(st, 1); o.kind = ColText(st, 2); o.value = sqlite3_column_double(st, 3);
        o.user_id = sqlite3_column_int(st, 4); o.target_type = ColText(st, 5); o.target_id = sqlite3_column_int(st, 6);
        o.valid_from = ColText(st, 7); o.valid_to = ColText(st, 8); o.status = ColText(st, 9); o.note = ColText(st, 10);
        o.cancel_reason = ColText(st, 11); o.created_at = ColText(st, 12); o.uses = sqlite3_column_int(st, 13);
        o.given = FromCents(sqlite3_column_int64(st, 14));
        out.push_back(o);
    }
    sqlite3_finalize(st);
    return out;
}

std::vector<Offer> SqliteConn::getOffers() {
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, (std::string("SELECT ") + kOfferCols + "FROM offers o ORDER BY o.id DESC").c_str(), -1, &st, nullptr);
    return ReadOffers(st);
}

int SqliteConn::saveOffer(const Offer& in) {
    Offer o = in;
    if (o.name.find_first_not_of(" \t") == std::string::npos) { lastError_ = "Η προσφορά χρειάζεται όνομα."; return -1; }
    if (o.value <= 0 || (o.kind == "percent" && o.value > 100)) { lastError_ = "Μη έγκυρη τιμή έκπτωσης."; return -1; }
    if (o.valid_to < o.valid_from) { lastError_ = "Η λήξη είναι πριν από την έναρξη."; return -1; }
    if (o.kind == "fixed") o.value = RoundMoney(o.value);
    sqlite3_stmt* st = nullptr;
    if (o.id <= 0)
        sqlite3_prepare_v2(db, "INSERT INTO offers (name, kind, value, user_id, target_type, target_id, valid_from, valid_to, note) "
                               "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9)", -1, &st, nullptr);
    else
        sqlite3_prepare_v2(db, "UPDATE offers SET name=?1, kind=?2, value=?3, user_id=?4, target_type=?5, target_id=?6, valid_from=?7, "
                               "valid_to=?8, note=?9 WHERE id=?10", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, o.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, o.kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(st, 3, o.value);
    if (o.user_id > 0) sqlite3_bind_int(st, 4, o.user_id); else sqlite3_bind_null(st, 4);
    sqlite3_bind_text(st, 5, o.target_type.c_str(), -1, SQLITE_TRANSIENT);
    if (o.target_type != "all" && o.target_id > 0) sqlite3_bind_int(st, 6, o.target_id); else sqlite3_bind_null(st, 6);
    sqlite3_bind_text(st, 7, o.valid_from.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 8, o.valid_to.c_str(), -1, SQLITE_TRANSIENT);
    if (o.note.empty()) sqlite3_bind_null(st, 9); else sqlite3_bind_text(st, 9, o.note.c_str(), -1, SQLITE_TRANSIENT);
    if (o.id > 0) sqlite3_bind_int(st, 10, o.id);
    int rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) lastError_ = sqlite3_errmsg(db);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return -1;
    return o.id > 0 ? o.id : (int)sqlite3_last_insert_rowid(db);
}

bool SqliteConn::cancelOffer(int id, const std::string& reason) {
    if (reason.find_first_not_of(" \t") == std::string::npos) { lastError_ = "Η ακύρωση χρειάζεται αιτιολογία."; return false; }
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "UPDATE offers SET status = 'cancelled', cancel_reason = ? WHERE id = ? AND status = 'active'", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, reason.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, id);
    int rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) lastError_ = sqlite3_errmsg(db);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE && sqlite3_changes(db) == 1;
}

std::vector<Offer> SqliteConn::findOffers(int userId, int serviceId, int packageId, const std::string& date) {
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, (std::string("SELECT ") + kOfferCols +
        "FROM offers o WHERE o.status = 'active' AND o.valid_from <= ?1 AND o.valid_to >= ?1 "
        "AND (o.user_id IS NULL OR o.user_id = ?2) "
        "AND (o.target_type = 'all' OR (o.target_type = 'service' AND o.target_id = ?3) OR (o.target_type = 'package' AND o.target_id = ?4)) "
        "ORDER BY o.user_id IS NULL, o.id").c_str(), -1, &st, nullptr);
    sqlite3_bind_text(st, 1, date.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, userId); sqlite3_bind_int(st, 3, serviceId); sqlite3_bind_int(st, 4, packageId);
    return ReadOffers(st);
}

// --- PAGED LOOKUPS -------------------------------------------------------------------------------------------------
// Everything below searches and pages in SQL, so the screens stay instant whatever the number of rows.
static void BindText(sqlite3_stmt* st, int i, const std::string& s) { sqlite3_bind_text(st, i, s.c_str(), -1, SQLITE_TRANSIENT); }

static const char* kBldWhere = " WHERE (?1 = '' OR instr(fold(address || ' ' || COALESCE(area,'') || ' ' || COALESCE(postal_code,'') || ' ' || COALESCE(afm,'')), fold(?1)) > 0) ";
std::vector<Building> SqliteConn::pageBuildings(const std::string& text, int offset, int limit) {
    std::vector<Building> out; sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, (std::string("SELECT id, address, area, postal_code, afm, created_at FROM buildings") + kBldWhere +
                            "ORDER BY address, id LIMIT ?2 OFFSET ?3").c_str(), -1, &st, nullptr);
    BindText(st, 1, text); sqlite3_bind_int(st, 2, limit); sqlite3_bind_int(st, 3, offset);
    while (sqlite3_step(st) == SQLITE_ROW) {
        Building b; b.id = sqlite3_column_int(st, 0); b.address = ColText(st, 1); b.area = ColText(st, 2);
        b.postal_code = ColText(st, 3); b.afm = ColText(st, 4); b.created_at = ColText(st, 5); out.push_back(b);
    }
    sqlite3_finalize(st); return out;
}
int SqliteConn::countBuildings(const std::string& text) {
    sqlite3_stmt* st = nullptr; int n = 0;
    sqlite3_prepare_v2(db, (std::string("SELECT COUNT(*) FROM buildings") + kBldWhere).c_str(), -1, &st, nullptr);
    BindText(st, 1, text);
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st); return n;
}

static const char* kPkgFrom =
    " FROM master_services m LEFT JOIN master_service_components mc ON mc.master_service_id = m.id "
    " LEFT JOIN services s ON s.id = mc.service_id GROUP BY m.id "
    " HAVING (?1 = '' OR instr(fold(m.label || ' ' || COALESCE(group_concat(s.label, ' '), '')), fold(?1)) > 0) ";
std::vector<DataConn::PackageRow> SqliteConn::pagePackages(const std::string& text, int sortCol, bool asc, int offset, int limit) {
    std::vector<PackageRow> out; sqlite3_stmt* st = nullptr;
    std::string order = sortCol == 1 ? "n" : sortCol == 2 ? "tc" : "fold(m.label)";
    std::string sql = std::string("SELECT m.id, m.label, m.created_at, COUNT(s.id) AS n, "
        "COALESCE(SUM(CAST(ROUND(s.suggested_price*100) AS INTEGER)), 0) AS tc, "
        "COALESCE(group_concat(s.label || ' (' || replace(printf('%.2f', s.suggested_price), '.', ',') || ' €)', ', '), '')") + kPkgFrom +
        "ORDER BY " + order + (asc ? " ASC" : " DESC") + ", m.id LIMIT ?2 OFFSET ?3";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) { lastError_ = sqlite3_errmsg(db); return out; }
    BindText(st, 1, text); sqlite3_bind_int(st, 2, limit); sqlite3_bind_int(st, 3, offset);
    while (sqlite3_step(st) == SQLITE_ROW) {
        PackageRow r; r.pkg.id = sqlite3_column_int(st, 0); r.pkg.label = ColText(st, 1);
        r.components = sqlite3_column_int(st, 3); r.total = FromCents(sqlite3_column_int64(st, 4)); r.parts = ColText(st, 5);
        out.push_back(r);
    }
    sqlite3_finalize(st); return out;
}
int SqliteConn::countPackages(const std::string& text) {
    sqlite3_stmt* st = nullptr; int n = 0;
    sqlite3_prepare_v2(db, (std::string("SELECT COUNT(*) FROM (SELECT m.id") + kPkgFrom + ")").c_str(), -1, &st, nullptr);
    BindText(st, 1, text);
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st); return n;
}

static const char* kJcWhere = " WHERE (?1 = '' OR instr(fold(s.label || ' ' || COALESCE(s.extra_notes,'')), fold(?1)) > 0) ";
std::vector<JointService> SqliteConn::pageJointCatalog(const std::string& text, int offset, int limit) {
    std::vector<JointService> out; sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, (std::string("SELECT j.id, s.id, s.label, s.extra_notes, s.created_at, j.kind, j.active, s.suggested_price "
                            "FROM joint_services j JOIN services s ON s.id = j.service_id") + kJcWhere +
                            "ORDER BY fold(s.label), j.id LIMIT ?2 OFFSET ?3").c_str(), -1, &st, nullptr);
    BindText(st, 1, text); sqlite3_bind_int(st, 2, limit); sqlite3_bind_int(st, 3, offset);
    while (sqlite3_step(st) == SQLITE_ROW) {
        JointService j; j.id = sqlite3_column_int(st, 0); j.service.id = sqlite3_column_int(st, 1); j.service.label = ColText(st, 2);
        j.service.extra_notes = ColText(st, 3); j.service.created_at = ColText(st, 4); j.kind = ColText(st, 5);
        j.active = sqlite3_column_int(st, 6) != 0; j.service.suggested_price = RoundMoney(sqlite3_column_double(st, 7));
        out.push_back(j);
    }
    sqlite3_finalize(st); return out;
}
int SqliteConn::countJointCatalog(const std::string& text) {
    sqlite3_stmt* st = nullptr; int n = 0;
    sqlite3_prepare_v2(db, (std::string("SELECT COUNT(*) FROM joint_services j JOIN services s ON s.id = j.service_id") + kJcWhere).c_str(), -1, &st, nullptr);
    BindText(st, 1, text);
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st); return n;
}

// same columns as kJointOpCols (ReadJointOp), plus sortable aliases
static const char* kJointOpPaged =
    "SELECT o.id, COALESCE(o.building_id, -1), o.source_type, o.source_id, o.kind, o.label, o.total, o.op_date, o.params_json, "
    "COALESCE(o.note,''), o.batch_id, datetime(o.created_at,'localtime'), COALESCE(b.address,'') AS addr, "
    "(SELECT COUNT(*) FROM joint_shares s WHERE s.op_id = o.id AND s.included = 1) AS inc, "
    "(SELECT COUNT(*) FROM joint_shares s WHERE s.op_id = o.id AND s.included = 0) AS exc, "
    "(SELECT COALESCE(SUM(CAST(ROUND(c.amount_paid*100) AS INTEGER)), 0) FROM charges c WHERE c.batch_id = o.batch_id) AS paidc, "
    "CAST(ROUND(o.total*100) AS INTEGER) AS totc "
    "FROM joint_ops o LEFT JOIN buildings b ON b.id = o.building_id "
    "WHERE (?1 <= 0 OR o.building_id = ?1) AND (?2 = '' OR instr(fold(o.label || ' ' || COALESCE(b.address,'')), fold(?2)) > 0) ";
std::vector<JointOp> SqliteConn::pageJointOps(int buildingId, const std::string& text, int sortCol, bool asc, int offset, int limit) {
    static const char* cols[] = { "o.op_date", "fold(addr)", "fold(o.label)", "o.kind", "totc", "MIN(paidc, totc)", "(totc - MIN(paidc, totc))",
                                  "(CASE WHEN totc > 0 THEN MIN(paidc, totc) * 1.0 / totc ELSE 1 END)", "inc", "exc" };
    std::string order = (sortCol >= 0 && sortCol < 10) ? cols[sortCol] : "o.op_date";
    std::vector<JointOp> out; sqlite3_stmt* st = nullptr;
    std::string sql = std::string(kJointOpPaged) + "ORDER BY " + order + (asc ? " ASC" : " DESC") + ", o.id DESC LIMIT ?3 OFFSET ?4";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) { lastError_ = sqlite3_errmsg(db); return out; }
    sqlite3_bind_int(st, 1, buildingId); BindText(st, 2, text); sqlite3_bind_int(st, 3, limit); sqlite3_bind_int(st, 4, offset);
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(ReadJointOp(st));
    sqlite3_finalize(st); return out;
}
int SqliteConn::countJointOps(int buildingId, const std::string& text, double* total, double* paid) {
    sqlite3_stmt* st = nullptr; int n = 0;
    std::string sql = std::string("SELECT COUNT(*), COALESCE(SUM(totc),0), COALESCE(SUM(MIN(paidc, totc)),0) FROM (") + kJointOpPaged + ")";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) { lastError_ = sqlite3_errmsg(db); return 0; }
    sqlite3_bind_int(st, 1, buildingId); BindText(st, 2, text);
    if (sqlite3_step(st) == SQLITE_ROW) {
        n = sqlite3_column_int(st, 0);
        if (total) *total = FromCents(sqlite3_column_int64(st, 1));
        if (paid) *paid = FromCents(sqlite3_column_int64(st, 2));
    }
    sqlite3_finalize(st); return n;
}

static const char* kBuyersFrom =
    " FROM charges ch JOIN master_service_components mc ON mc.service_id = ch.service_id AND mc.master_service_id = ?1 "
    " JOIN users u ON u.id = ch.user_id JOIN services s ON s.id = ch.service_id "
    " GROUP BY ch.user_id "
    " HAVING (?2 = '' OR instr(fold(u.full_name), fold(?2)) > 0) "
    " AND (?3 = 0 OR (?3 = 1 AND COUNT(DISTINCT ch.service_id) = (SELECT COUNT(*) FROM master_service_components x WHERE x.master_service_id = ?1)) "
    "            OR (?3 = 2 AND COUNT(DISTINCT ch.service_id) < (SELECT COUNT(*) FROM master_service_components x WHERE x.master_service_id = ?1))) ";
std::vector<DataConn::PackageBuyer> SqliteConn::pagePackageBuyers(int pkgId, const std::string& text, int show, int sortCol, bool asc, int offset, int limit) {
    static const char* cols[] = { "fold(u.full_name)", "cnt", "cnt", "duec", "paidc", "(duec - paidc)" };
    std::string order = (sortCol >= 0 && sortCol < 6) ? cols[sortCol] : "cnt";
    std::vector<PackageBuyer> out; sqlite3_stmt* st = nullptr;
    std::string sql = std::string("SELECT ch.user_id, u.full_name, COUNT(DISTINCT ch.service_id) AS cnt, "
        "(SELECT group_concat(label, ', ') FROM (SELECT DISTINCT s2.label FROM charges c2 JOIN services s2 ON s2.id = c2.service_id "
        "   JOIN master_service_components m2 ON m2.service_id = c2.service_id AND m2.master_service_id = ?1 WHERE c2.user_id = ch.user_id)), "
        "SUM(CAST(ROUND(ch.amount_due*100) AS INTEGER)) AS duec, SUM(CAST(ROUND(ch.amount_paid*100) AS INTEGER)) AS paidc") + kBuyersFrom +
        "ORDER BY " + order + (asc ? " ASC" : " DESC") + ", fold(u.full_name) LIMIT ?4 OFFSET ?5";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) { lastError_ = sqlite3_errmsg(db); return out; }
    sqlite3_bind_int(st, 1, pkgId); BindText(st, 2, text); sqlite3_bind_int(st, 3, show); sqlite3_bind_int(st, 4, limit); sqlite3_bind_int(st, 5, offset);
    while (sqlite3_step(st) == SQLITE_ROW) {
        PackageBuyer b; b.user_id = sqlite3_column_int(st, 0); b.name = ColText(st, 1); b.count = sqlite3_column_int(st, 2);
        b.parts = ColText(st, 3); b.due = FromCents(sqlite3_column_int64(st, 4)); b.paid = FromCents(sqlite3_column_int64(st, 5));
        out.push_back(b);
    }
    sqlite3_finalize(st); return out;
}
int SqliteConn::countPackageBuyers(int pkgId, const std::string& text, int show) {
    sqlite3_stmt* st = nullptr; int n = 0;
    if (sqlite3_prepare_v2(db, (std::string("SELECT COUNT(*) FROM (SELECT ch.user_id") + kBuyersFrom + ")").c_str(), -1, &st, nullptr) != SQLITE_OK) {
        lastError_ = sqlite3_errmsg(db); return 0; }
    sqlite3_bind_int(st, 1, pkgId); BindText(st, 2, text); sqlite3_bind_int(st, 3, show);
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st); return n;
}

std::vector<User> SqliteConn::getBuildingResidents(int buildingId) {
    std::vector<User> out; sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, (std::string(kUserSelect) + "WHERE b.id = ? ORDER BY a.label, u.full_name").c_str(), -1, &st, nullptr);
    sqlite3_bind_int(st, 1, buildingId);
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(MapUserRow(st));
    sqlite3_finalize(st); return out;
}

std::string SqliteConn::getPackageLabel(int pkgId) {
    sqlite3_stmt* st = nullptr; std::string l;
    sqlite3_prepare_v2(db, "SELECT label FROM master_services WHERE id = ?", -1, &st, nullptr);
    sqlite3_bind_int(st, 1, pkgId);
    if (sqlite3_step(st) == SQLITE_ROW) l = ColText(st, 0);
    sqlite3_finalize(st); return l;
}
JointService SqliteConn::getJointServiceById(int id) {
    JointService j; sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "SELECT j.id, s.id, s.label, s.extra_notes, s.created_at, j.kind, j.active, s.suggested_price "
                           "FROM joint_services j JOIN services s ON s.id = j.service_id WHERE j.id = ?", -1, &st, nullptr);
    sqlite3_bind_int(st, 1, id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        j.id = sqlite3_column_int(st, 0); j.service.id = sqlite3_column_int(st, 1); j.service.label = ColText(st, 2);
        j.service.extra_notes = ColText(st, 3); j.service.created_at = ColText(st, 4); j.kind = ColText(st, 5);
        j.active = sqlite3_column_int(st, 6) != 0; j.service.suggested_price = RoundMoney(sqlite3_column_double(st, 7));
    }
    sqlite3_finalize(st); return j;
}
