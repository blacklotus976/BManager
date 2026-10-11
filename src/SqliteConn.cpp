#include "SqliteConn.h"
#include "Platform.h"
#include "Money.h"
#include "Rf.h"
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

static std::string paymentToJson(const Payment& p) {
    return "{ \"id\": " + std::to_string(p.id) + ", \"service_id\": " + std::to_string(p.service_id) + ", \"user_id\": " + std::to_string(p.user_id) + ", \"year\": " + std::to_string(p.year) + ", \"month\": " + std::to_string(p.month) + ", \"amount_due\": " + FormatMoney(p.amount_due) + ", \"amount_paid\": " + FormatMoney(p.amount_paid) + ", \"status\": \"" + p.status + "\", \"direction\": \"" + p.direction + "\", \"resolution_date\": \"" + p.resolution_date + "\", \"resolved_at\": \"" + p.resolved_at + "\" }";
}

static int UserVersion(sqlite3* db) {
    int v = 0; sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return v;
}

SqliteConn::SqliteConn(const std::string& dbPath) {
    dbPath_ = dbPath;
    int rc = sqlite3_open(dbPath.c_str(), &db);
    if (rc) {
        std::cerr << "Can't open database: " << sqlite3_errmsg(db) << std::endl;
        return;
    }

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
    {
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

    greekHeaders["users"] = {"ΚΩΔ", "ΟΝΟΜΑ", "ΤΗΛ", "ΗΜ/ΝΙΑ ΔΗΜ.", "ΕΙΔ. ΚΩΔ.", "ΚΩΔ ΔΙΑΜΕΡΙΣΜΑΤΟΣ"};
    greekHeaders["buildings"] = {"ΚΩΔ", "ΔΙΕΥΘΥΝΣΗ", "ΠΕΡΙΟΧΗ", "ΤΚ", "ΑΦΜ", "ΗΜ/ΝΙΑ ΔΗΜ."};
    greekHeaders["apartments"] = {"ΚΩΔ", "ΚΩΔ ΚΤΙΡΙΟΥ", "ΔΙΑΜΕΡΙΣΜΑ"};
    greekHeaders["services"] = {"ΚΩΔ ΥΠΗΡ.", "ΠΕΡΙΓΡΑΦΗ", "ΣΗΜΕΙΩΣΕΙΣ", "ΗΜ/ΝΙΑ ΔΗΜ."};
    greekHeaders["service_users"] = {"ΚΩΔ ΥΠΗΡ.", "ΚΩΔ ΧΡΗΣΤΗ"};
    greekHeaders["payments"] = {"ΚΩΔ ΠΛΗΡ.", "ΚΩΔ ΥΠΗΡ.", "ΕΤΟΣ", "ΜΗΝΑΣ", "ΗΜΕΡΑ", "ΠΟΣΟ ΟΦΕΙΛ.", "ΠΟΣΟ ΠΛΗΡ.", "ΥΠΟΛΟΙΠΟ", "ΚΑΤΑΣΤΑΣΗ", "ΣΧΟΛΙΑ", "ΚΩΔ ΧΡΗΣΤΗ", "ΤΥΠΟΣ", "ΗΜ/ΝΙΑ ΕΠΙΛΥΣΗΣ", "ΕΠΙΛΥΘΗΚΕ ΣΤΙΣ"};
    greekHeaders["master_services"] = {"ΚΩΔ", "ΕΤΙΚΕΤΑ", "ΗΜ/ΝΙΑ ΔΗΜ."};
    greekHeaders["master_service_components"] = {"ΚΩΔ ΠΑΚΕΤΟΥ", "ΚΩΔ ΥΠΗΡ."};
    greekHeaders["receipts"] = {"ΚΩΔ", "ΣΕΙΡΑ", "ΑΡΙΘΜΟΣ", "ΗΜ/ΝΙΑ ΕΚΔ.", "ΚΩΔ ΧΡΗΣΤΗ", "ΚΩΔ ΚΤΙΡΙΟΥ", "ΣΥΝΟΛΟ",
                                "ΤΡΟΠΟΣ", "ΚΩΔ. RF", "ΑΡ. ΧΑΡΤΙΝΗΣ", "ΤΡΑΠ. ΑΝΑΦΟΡΑ", "ΣΗΜΕΙΩΣΕΙΣ", "ΚΑΤΑΣΤΑΣΗ",
                                "ΑΙΤΙΑ ΑΚΥΡΩΣΗΣ", "ΑΚΥΡΩΘΗΚΕ ΣΤΙΣ", "ΗΜ/ΝΙΑ ΔΗΜ."};
    greekHeaders["notes"] = {"ΚΩΔ", "ΤΙΤΛΟΣ", "ΠΕΡΙΕΧΟΜΕΝΟ", "ΗΜ/ΝΙΑ ΔΗΜ."};
}

SqliteConn::~SqliteConn() { if(db) sqlite3_close(db); }

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
        logAction("users", newId, "INSERT", "", userToJson(u), "business");
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
        logAction("users", id, "UPDATE", userToJson(old), userToJson(u), "business"); return true;
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
    if (rc == SQLITE_DONE) { executeQuery("DELETE FROM residencies WHERE user_id = " + std::to_string(id) + ";"); logAction("users", id, "DELETE", "", "", "business"); return true; }
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
    logAction("buildings", id, "INSERT", "", BuildingToJson(b), "business");
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
    logAction("buildings", id, "UPDATE", BuildingToJson(old), BuildingToJson(b), "business");
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
    logAction("buildings", id, "DELETE", BuildingToJson(old), "", "business");
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
    logAction("apartments", id, "INSERT", "",
              "{ \"id\": " + std::to_string(id) + ", \"building_id\": " + std::to_string(buildingId) + ", \"label\": \"" + label + "\" }", "business");
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
    logAction("apartments", id, "UPDATE", "", "{ \"id\": " + std::to_string(id) + ", \"label\": \"" + label + "\" }", "business");
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
    logAction("apartments", id, "DELETE", "", "", "business");
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
    // before any recorded stay: fall back to where the client lives now
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
    sql += "UPDATE payments SET apartment_id = COALESCE(("
           "SELECT r.apartment_id FROM residencies r WHERE r.user_id = payments.user_id "
           "AND r.from_date <= printf('%04d-%02d-%02d', payments.year, payments.month, payments.day) "
           "AND (r.to_date IS NULL OR r.to_date >= printf('%04d-%02d-%02d', payments.year, payments.month, payments.day)) "
           "ORDER BY r.from_date DESC, r.id DESC LIMIT 1), apartment_id) WHERE user_id = " + uid +
           // receipted payments keep the apartment printed on their receipt
           " AND (receipt_id IS NULL OR (SELECT status FROM receipts WHERE id = payments.receipt_id) = 'void');COMMIT;";
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        lastError_ = err ? err : "setResidencyHistory failed";
        sqlite3_free(err);
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
        return false;
    }
    logAction("residencies", userId, "UPDATE", "", "history replaced", "business");
    return true;
}

std::vector<Payment> SqliteConn::getPaymentsForApartment(int apartmentId) {
    std::vector<Payment> out;
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, "SELECT id, service_id, year, month, day, amount_due, amount_paid, balance, status, notes, user_id, direction, "
                           "resolution_date, resolved_at, apartment_id, created_at FROM payments WHERE apartment_id=? ORDER BY year, month, day, id", -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, apartmentId);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Payment p;
        p.id = sqlite3_column_int(stmt, 0); p.service_id = sqlite3_column_int(stmt, 1);
        p.year = sqlite3_column_int(stmt, 2); p.month = sqlite3_column_int(stmt, 3); p.day = sqlite3_column_int(stmt, 4);
        p.amount_due = sqlite3_column_double(stmt, 5); p.amount_paid = sqlite3_column_double(stmt, 6); p.balance = sqlite3_column_double(stmt, 7);
        p.status = ColText(stmt, 8); p.notes = ColText(stmt, 9);
        p.user_id = sqlite3_column_type(stmt, 10) == SQLITE_NULL ? -1 : sqlite3_column_int(stmt, 10);
        p.direction = NormalizeDirection((const char*)sqlite3_column_text(stmt, 11));
        p.resolution_date = ColText(stmt, 12); p.resolved_at = ColText(stmt, 13);
        p.apartment_id = apartmentId; p.created_at = ColText(stmt, 15);
        out.push_back(p);
    }
    sqlite3_finalize(stmt);
    return out;
}

std::string SqliteConn::takeLastError() { std::string e; e.swap(lastError_); return e; }

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
    logAction("services", id, "INSERT", "", serviceToJson(s), "business");
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
    if(rc == SQLITE_DONE) { logAction("service_users", serviceId, "INSERT", "", "{ \"user_id\": " + std::to_string(userId) + " }", "business"); return true; }
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
bool SqliteConn::addPayment(const Payment& in) {
    Payment p = in;                                   // money snapped to cents; balance always due - paid
    p.amount_due = RoundMoney(p.amount_due);
    p.amount_paid = RoundMoney(p.amount_paid);
    p.balance = MoneySub(p.amount_due, p.amount_paid);
    int aptId = p.apartment_id >= 0 ? p.apartment_id : apartmentAt(p.user_id, p.year, p.month, p.day);
    std::string sql = "INSERT INTO payments (service_id, year, month, day, amount_due, amount_paid, balance, status, notes, user_id, direction, resolution_date, resolved_at, apartment_id, created_at) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,COALESCE(?15, datetime('now','localtime')))";
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
    if (aptId > 0) sqlite3_bind_int(stmt,14,aptId); else sqlite3_bind_null(stmt,14);
    if (p.created_at.empty()) sqlite3_bind_null(stmt,15); else sqlite3_bind_text(stmt,15,p.created_at.c_str(),-1,SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if(rc == SQLITE_DONE) { int id = sqlite3_last_insert_rowid(db); p.id = id; logAction("payments", id, "INSERT", "", paymentToJson(p), "business"); return true; }
    return false;
}

// NOTE: STUB (not present in transcript) — added to satisfy DataConn interface.
bool SqliteConn::editPayment(int id, const Payment& in) {
    Payment p = in;                                   // money snapped to cents; balance always due - paid
    p.amount_due = RoundMoney(p.amount_due);
    p.amount_paid = RoundMoney(p.amount_paid);
    p.balance = MoneySub(p.amount_due, p.amount_paid);
    Payment old = getPaymentById(id);
    int aptId = p.apartment_id;
    if (aptId < 0) {
        bool moved = old.user_id != p.user_id || old.year != p.year || old.month != p.month || old.day != p.day;
        aptId = moved ? apartmentAt(p.user_id, p.year, p.month, p.day) : old.apartment_id;
    }
    std::string sql = "UPDATE payments SET year=?, month=?, day=?, amount_due=?, amount_paid=?, balance=?, status=?, notes=?, user_id=?, direction=?, resolution_date=?, resolved_at=?, apartment_id=? WHERE id=?";
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
    if (aptId > 0) sqlite3_bind_int(stmt,13,aptId); else sqlite3_bind_null(stmt,13);
    sqlite3_bind_int(stmt,14,id);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) lastError_ = sqlite3_errmsg(db);   // e.g. refused: payment is on an issued receipt
    sqlite3_finalize(stmt);
    if(rc == SQLITE_DONE) { p.id = id; logAction("payments", id, "UPDATE", paymentToJson(old), paymentToJson(p), "business"); return true; }
    return false;
}

// NOTE: STUB (not present in transcript).
bool SqliteConn::removePayment(int id) {
    std::string sql = "DELETE FROM payments WHERE id=?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    sqlite3_bind_int(stmt,1,id);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) lastError_ = sqlite3_errmsg(db);   // e.g. refused: payment is on an issued receipt
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
        p.apartment_id = sqlite3_column_type(stmt,14) == SQLITE_NULL ? -1 : sqlite3_column_int(stmt,14);
        p.created_at = ColText(stmt,15);
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
        p.apartment_id = sqlite3_column_type(stmt,14) == SQLITE_NULL ? -1 : sqlite3_column_int(stmt,14);
        p.created_at = ColText(stmt,15);
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
        p.apartment_id = sqlite3_column_type(stmt,14) == SQLITE_NULL ? -1 : sqlite3_column_int(stmt,14);
        p.created_at = ColText(stmt,15);
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
        p.apartment_id = sqlite3_column_type(stmt,14) == SQLITE_NULL ? -1 : sqlite3_column_int(stmt,14);
        p.created_at = ColText(stmt,15);
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
        p.apartment_id = sqlite3_column_type(stmt,14) == SQLITE_NULL ? -1 : sqlite3_column_int(stmt,14);
        p.created_at = ColText(stmt,15);
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
        p.apartment_id = sqlite3_column_type(stmt,14) == SQLITE_NULL ? -1 : sqlite3_column_int(stmt,14);
        p.created_at = ColText(stmt,15);
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
        p.apartment_id = sqlite3_column_type(stmt,14) == SQLITE_NULL ? -1 : sqlite3_column_int(stmt,14);
        p.created_at = ColText(stmt,15);
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
    std::string sql = "SELECT TOTAL(CAST(ROUND(amount_paid*100) AS INTEGER)) / 100.0 FROM payments WHERE year=? AND month=? AND status != 'future'";
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
    std::string sql = "SELECT TOTAL(CAST(ROUND(amount_due*100) AS INTEGER) - CAST(ROUND(amount_paid*100) AS INTEGER)) / 100.0 FROM payments WHERE year=? AND month=? AND status='pending'";
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
    std::vector<std::string> tables = {"buildings", "apartments", "users", "services", "service_users", "master_services", "master_service_components", "payments", "receipts", "notes"};
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
        "WHERE (SELECT COUNT(*) FROM service_users su WHERE su.service_id = s.id) <= 1";
    std::vector<std::string> binds;
    if (!query.empty()) {
        sql += " AND (s.label LIKE '%' || ? || '%' OR s.extra_notes LIKE '%' || ? || '%')";
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
        "WHERE (SELECT COUNT(*) FROM service_users su WHERE su.service_id = s.id) <= 1";
    std::vector<std::string> binds;
    if (!query.empty()) {
        sql += " AND (s.label LIKE '%' || ? || '%' OR s.extra_notes LIKE '%' || ? || '%')";
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
static std::string BuildPaymentPageWhere(const std::string& startDate, const std::string& endDate,
                                          int userId, int serviceId,
                                          const std::string& status, const std::string& direction,
                                          std::vector<std::string>& textBinds,
                                          std::vector<int>& intBinds) {
    std::string where = " WHERE 1=1";
    if (!startDate.empty()) {
        where += " AND printf('%04d-%02d-%02d', year, month, day) >= ?";
        textBinds.push_back(startDate);
    }
    if (!endDate.empty()) {
        where += " AND printf('%04d-%02d-%02d', year, month, day) <= ?";
        textBinds.push_back(endDate);
    }
    if (userId >= 0)    { where += " AND user_id = ?";    intBinds.push_back(userId); }
    if (serviceId >= 0) { where += " AND service_id = ?"; intBinds.push_back(serviceId); }
    if (status == "future")        where += " AND status = 'future'";
    else if (status == "resolved") where += " AND status != 'future'";
    if (direction == "in" || direction == "out") {
        where += " AND direction = ?";
        textBinds.push_back(direction);
    }
    return where;
}

std::vector<Payment> SqliteConn::pagePayments(const std::string& startDate, const std::string& endDate,
                                               int userId, int serviceId,
                                               const std::string& status, const std::string& direction,
                                               int offset, int limit, const SortSpec& sort) {
    std::vector<std::string> textBinds;
    std::vector<int> intBinds;
    std::string where = BuildPaymentPageWhere(startDate, endDate, userId, serviceId, status, direction,
                                               textBinds, intBinds);
    // Payments table columns: 0 id, 1 date, 2 client, 3 service, 4 type, 5 status,
    // 6 due, 7 paid, 8 notes. Client/service sort by NAME (sub-select), not by id.
    std::string order;
    for (auto& k : sort) {
        std::string d = k.second ? " ASC" : " DESC";
        std::string part;
        switch (k.first) {
            case 0: part = "id" + d; break;
            case 1: part = "year" + d + ", month" + d + ", day" + d; break;
            case 2: part = "(SELECT full_name FROM users WHERE users.id = payments.user_id)" + d; break;
            case 3: part = "(SELECT label FROM services WHERE services.id = payments.service_id)" + d; break;
            case 4: part = "direction" + d; break;
            case 5: part = "status" + d; break;
            case 6: part = "amount_due" + d; break;
            case 7: part = "amount_paid" + d; break;
            case 8: part = "notes" + d; break;
            default: break;
        }
        if (!part.empty()) order += std::string(order.empty() ? "" : ", ") + part;
    }
    if (order.empty()) order = "year DESC, month DESC, day DESC";
    order += ", id DESC";
    std::string sql = "SELECT * FROM payments" + where + " ORDER BY " + order + " LIMIT ? OFFSET ?";
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
    int idx = 1;
    for (auto& b : textBinds) sqlite3_bind_text(stmt, idx++, b.c_str(), -1, SQLITE_TRANSIENT);
    for (auto i : intBinds)   sqlite3_bind_int (stmt, idx++, i);
    sqlite3_bind_int(stmt, idx++, limit);
    sqlite3_bind_int(stmt, idx++, offset);
    std::vector<Payment> results;
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
        p.status = ColText(stmt,8);
        p.notes = ColText(stmt,9);
        p.user_id = sqlite3_column_type(stmt,10) == SQLITE_NULL ? -1 : sqlite3_column_int(stmt,10);
        p.direction = NormalizeDirection(sqlite3_column_text(stmt,11) ? (const char*)sqlite3_column_text(stmt,11) : "in");
        p.resolution_date = ColText(stmt,12);
        p.resolved_at = ColText(stmt,13);
        p.apartment_id = sqlite3_column_type(stmt,14) == SQLITE_NULL ? -1 : sqlite3_column_int(stmt,14);
        p.created_at = ColText(stmt,15);
        results.push_back(p);
    }
    sqlite3_finalize(stmt);
    return results;
}

int SqliteConn::countPayments(const std::string& startDate, const std::string& endDate,
                               int userId, int serviceId,
                               const std::string& status, const std::string& direction) {
    std::vector<std::string> textBinds;
    std::vector<int> intBinds;
    std::string where = BuildPaymentPageWhere(startDate, endDate, userId, serviceId, status, direction,
                                               textBinds, intBinds);
    std::string sql = "SELECT COUNT(*) FROM payments" + where;
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
    "COALESCE(void_reason,''), COALESCE(voided_at,''), COALESCE(created_at,'')";

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

int SqliteConn::issueReceipt(const Receipt& in, const std::vector<int>& paymentIdsIn) {
    auto refuse = [&](const std::string& why) { lastError_ = why; return -1; };
    std::vector<int> paymentIds = paymentIdsIn;
    std::sort(paymentIds.begin(), paymentIds.end());
    paymentIds.erase(std::unique(paymentIds.begin(), paymentIds.end()), paymentIds.end());
    if (paymentIds.empty()) return refuse("Επιλέξτε τουλάχιστον μία πληρωμή για την απόδειξη.");

    static const char* kMethods[] = {"cash", "collector", "bank", "ebanking", "card", "iris", "other"};
    if (std::none_of(std::begin(kMethods), std::end(kMethods), [&](const char* m) { return in.method == m; }))
        return refuse("Άγνωστος τρόπος πληρωμής: " + in.method);
    std::string series = in.series.empty() ? "A" : in.series;
    std::string rf = NormalizeRfCode(in.rf_code);
    if (!rf.empty() && !IsValidRfCode(rf)) return refuse("Μη έγκυρος κωδικός RF: " + in.rf_code);

    // BEGIN IMMEDIATE: takes the write lock now, so the receipt number cannot be handed out twice.
    if (!executeQuery("BEGIN IMMEDIATE;")) return -1;
    auto rollback = [&](const std::string& why) { executeQuery("ROLLBACK;"); lastError_ = why; return -1; };

    long long totalCents = 0;
    for (int pid : paymentIds) {
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, "SELECT p.status, COALESCE(p.direction,'in'), p.amount_paid, COALESCE(r.status,'') "
                               "FROM payments p LEFT JOIN receipts r ON r.id = p.receipt_id WHERE p.id = ?", -1, &st, nullptr);
        sqlite3_bind_int(st, 1, pid);
        if (sqlite3_step(st) != SQLITE_ROW) { sqlite3_finalize(st); return rollback("Η πληρωμή #" + std::to_string(pid) + " δεν υπάρχει."); }
        std::string status = (const char*)sqlite3_column_text(st, 0);
        std::string direction = (const char*)sqlite3_column_text(st, 1);
        double paid = sqlite3_column_double(st, 2);
        std::string onReceipt = (const char*)sqlite3_column_text(st, 3);
        sqlite3_finalize(st);
        if (status == "future")    return rollback("Η πληρωμή #" + std::to_string(pid) + " είναι μελλοντική - επιλύστε την πρώτα.");
        if (direction != "in")     return rollback("Η πληρωμή #" + std::to_string(pid) + " είναι πληρωμή προς τρίτους, όχι είσπραξη.");
        if (onReceipt == "issued") return rollback("Η πληρωμή #" + std::to_string(pid) + " έχει ήδη απόδειξη.");
        totalCents += ToCents(paid);
    }
    if (totalCents <= 0) return rollback("Το σύνολο της απόδειξης πρέπει να είναι μεγαλύτερο από 0.");

    int number = 1;
    {
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db, "SELECT COALESCE(MAX(number),0) + 1 FROM receipts WHERE series = ?", -1, &st, nullptr);
        sqlite3_bind_text(st, 1, series.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) number = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    if (rf.empty()) {
        char ref[32]; std::snprintf(ref, sizeof(ref), "%s%07d", series.c_str(), number);
        rf = MakeRfCode(ref);
    }

    Receipt r = in;
    r.series = series; r.number = number; r.total = FromCents(totalCents); r.rf_code = rf; r.status = "issued";
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
    if (rc != SQLITE_DONE) return rollback(std::string("Αποτυχία έκδοσης απόδειξης: ") + sqlite3_errmsg(db));
    r.id = (int)sqlite3_last_insert_rowid(db);

    for (int pid : paymentIds) {
        sqlite3_stmt* up = nullptr;
        sqlite3_prepare_v2(db, "UPDATE payments SET receipt_id = ? WHERE id = ?", -1, &up, nullptr);
        sqlite3_bind_int(up, 1, r.id); sqlite3_bind_int(up, 2, pid);
        rc = sqlite3_step(up);
        sqlite3_finalize(up);
        if (rc != SQLITE_DONE) return rollback(std::string("Αποτυχία σύνδεσης πληρωμής: ") + sqlite3_errmsg(db));
    }
    logAction("receipts", r.id, "INSERT", "", receiptToJson(r, paymentIds), "business");
    if (!executeQuery("COMMIT;")) return rollback("Αποτυχία ολοκλήρωσης της απόδειξης.");
    return r.id;
}

bool SqliteConn::voidReceipt(int id, const std::string& reason) {
    std::string why = reason;
    while (!why.empty() && std::isspace((unsigned char)why.back())) why.pop_back();
    if (why.empty()) { lastError_ = "Η ακύρωση απόδειξης χρειάζεται αιτιολογία."; return false; }
    Receipt old = getReceiptById(id);
    if (old.id < 0) { lastError_ = "Η απόδειξη δεν υπάρχει."; return false; }
    if (old.status == "void") { lastError_ = "Η απόδειξη είναι ήδη ακυρωμένη."; return false; }

    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "UPDATE receipts SET status='void', void_reason=?, voided_at=datetime('now','localtime') WHERE id=?", -1, &st, nullptr);
    sqlite3_bind_text(st, 1, why.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 2, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { lastError_ = sqlite3_errmsg(db); return false; }
    auto ids = getReceiptPaymentIds(id);
    Receipt now = getReceiptById(id);
    logAction("receipts", id, "VOID", receiptToJson(old, ids), receiptToJson(now, ids), "business");
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

std::vector<int> SqliteConn::getReceiptPaymentIds(int receiptId) {
    std::vector<int> out;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "SELECT id FROM payments WHERE receipt_id = ? ORDER BY id", -1, &st, nullptr);
    sqlite3_bind_int(st, 1, receiptId);
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back(sqlite3_column_int(st, 0));
    sqlite3_finalize(st);
    return out;
}

int SqliteConn::getReceiptIdForPayment(int paymentId) {
    int id = -1;
    sqlite3_stmt* st = nullptr;
    sqlite3_prepare_v2(db, "SELECT COALESCE(receipt_id,-1) FROM payments WHERE id = ?", -1, &st, nullptr);
    sqlite3_bind_int(st, 1, paymentId);
    if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return id;
}
