// Audit log trust: triggers record everything, the log is append-only and hash-chained, and tampering
// done outside the app (a raw sqlite3 connection, as with any DB tool) is refused or detected.
#include "SqliteConn.h"
#include <sqlite3.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } } while (0)

static bool raw(const std::string& path, const std::string& sql) {   // an outside tool: no app functions
    sqlite3* d = nullptr; sqlite3_open(path.c_str(), &d);
    char* err = nullptr;
    bool ok = sqlite3_exec(d, sql.c_str(), nullptr, nullptr, &err) == SQLITE_OK;
    sqlite3_free(err); sqlite3_close(d);
    return ok;
}
static std::string one(SqliteConn& db, const std::string& sql) {
    std::vector<std::string> c; std::vector<std::vector<std::string>> rows; std::string err;
    db.executeRawQuery(sql, c, rows, err);
    return rows.empty() || rows[0].empty() ? "" : rows[0][0];
}

int main() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "bmanager_audit_test";
    fs::remove_all(dir); fs::create_directories(dir);
    std::string path = (dir / "data.db").string();
    int pid = -1;
    {
        SqliteConn db(path);
        CHECK(db.takeLastError().empty());
        db.setAuditActor("γραμματεία@pc1");
        User u; u.full_name = "ΠΕΛΑΤΗΣ"; db.addUser(u);
        Service s; s.label = "ΔΙΑΧΕΙΡΙΣΗ"; s.suggested_price = 40; int svc = db.addServiceGetId(1, s);
        Payment p; p.user_id = 1; p.service_id = svc; p.year = 2026; p.month = 10; p.day = 11;
        p.amount_due = 40; p.amount_paid = 40; p.status = "paid"; p.notes = "με \"εισαγωγικά\" και \\ backslash";
        db.addPayment(p);
        pid = std::atoi(one(db, "SELECT MAX(id) FROM charges").c_str());
        db.setAuditReason("Διόρθωση ποσού μετά από τηλεφώνημα");
        p = db.getPaymentById(pid); p.amount_paid = 35;
        CHECK(db.editPayment(pid, p));
        db.setAuditReason("");

        // dates are one ISO column; the app still sees the parts
        CHECK(one(db, "SELECT pay_date FROM charges WHERE id = " + std::to_string(pid)) == "2026-10-11");
        CHECK(db.getPaymentById(pid).year == 2026 && db.getPaymentById(pid).day == 11);
        CHECK(db.getPaymentById(pid).created_at.size() == 19);

        // the edit was recorded by the DB, with actor, reason, valid JSON before/after
        // the bill's paid amount follows the money: recorded on the charge, by the DB, with actor + reason
        std::string where = " FROM audit_log WHERE table_name='charges' AND action_type='UPDATE' AND record_id=" + std::to_string(pid) +
                            " AND json_extract(new_data,'$.amount_paid') = 35.0";
        CHECK(one(db, "SELECT actor" + where) == "γραμματεία@pc1");
        CHECK(one(db, "SELECT reason" + where) == "Διόρθωση ποσού μετά από τηλεφώνημα");
        CHECK(one(db, "SELECT json_extract(old_data,'$.amount_paid') || '>' || json_extract(new_data,'$.amount_paid')" + where) == "40.0>35.0");
        CHECK(one(db, "SELECT COUNT(*) FROM audit_log WHERE json_valid(COALESCE(new_data,'{}')) = 0 AND actor != 'πριν την v7'") == "0");
        CHECK(one(db, "SELECT COUNT(*) FROM audit_log WHERE hash IS NULL") == "0");
        auto chk = db.verifyAuditLog();
        CHECK(chk.ok);
        CHECK(chk.entries > 5);
    }

    // outside tools can neither rewrite nor delete history, and can't even write business rows
    // (the audit triggers need the app's functions)
    CHECK(!raw(path, "UPDATE audit_log SET new_data = '{}' WHERE id = 3"));
    CHECK(!raw(path, "DELETE FROM audit_log"));
    CHECK(!raw(path, "INSERT INTO audit_log (table_name, action_type, hash) VALUES ('x','y','forged')"));
    CHECK(!raw(path, "UPDATE payments SET amount_paid = 1"));

    // a determined attacker drops the guards and edits history -> detected on next start
    CHECK(raw(path, "DROP TRIGGER audit_log_append_only; UPDATE audit_log SET reason = 'πλαστό' WHERE id = 3;"));
    {
        SqliteConn db(path);
        std::string banner = db.takeLastError();
        CHECK(!banner.empty());
        auto chk = db.verifyAuditLog();
        CHECK(!chk.ok);
        bool chainBroken = false;
        for (auto& pr : chk.problems) if (pr.find("αλυσίδα") != std::string::npos) chainBroken = true;
        CHECK(chainBroken);
        CHECK(one(db, "SELECT COUNT(*) FROM sqlite_master WHERE name = 'audit_log_append_only'") == "1");   // restored
    }

    // ... or drops the audit trigger and changes a payment silently -> detected (row != last snapshot)
    fs::path path2 = dir / "data2.db";
    {
        SqliteConn db(path2.string());
        User u; u.full_name = "Α"; db.addUser(u);
        Service s; s.label = "Β"; s.suggested_price = 10; int svc = db.addServiceGetId(1, s);
        Payment p; p.user_id = 1; p.service_id = svc; p.year = 2026; p.month = 1; p.day = 1;
        p.amount_due = 10; p.amount_paid = 10; p.status = "paid"; db.addPayment(p);
        CHECK(db.verifyAuditLog().ok);
    }
    CHECK(raw(path2.string(), "DROP TRIGGER audit_charges_upd; UPDATE charges SET notes = 'σιωπηλή αλλαγή';"));
    {
        SqliteConn db(path2.string());
        CHECK(!db.takeLastError().empty());
        auto chk = db.verifyAuditLog();
        bool silent = false;
        for (auto& pr : chk.problems) if (pr.find("charges #1") != std::string::npos) silent = true;
        CHECK(!chk.ok && silent);
    }

    // payment dates must be real ISO dates
    CHECK(!raw(path2.string(), "DROP TRIGGER audit_charges_ins; INSERT INTO charges (service_id, pay_date) VALUES (1, '2026-2-30')"));

    fs::remove_all(dir);
    std::printf(fails ? "%d FAILED\n" : "all audit tests passed\n", fails);
    return fails ? 1 : 0;
}
