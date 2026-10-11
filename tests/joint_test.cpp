// Κοινόχρηστα: a cost split across apartments -- shares must add up, exclusions need a reason, the record is fixed.
#include "SqliteConn.h"
#include "ErrorLog.h"
#include "Money.h"
#include <sqlite3.h>
#include <cstdio>
#include <filesystem>

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } } while (0)

int main() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "bmanager_joint_test";
    fs::remove_all(dir); fs::create_directories(dir);
    std::string db = (dir / "data.db").string();
    {
        SqliteConn c(db);
        Building b; b.address = "Πατησίων 205"; int bid = c.addBuilding(b);
        int a1 = c.addApartment(bid, "Α1"), a2 = c.addApartment(bid, "Α2"), a3 = c.addApartment(bid, "Β1");
        int users[2];
        for (int i = 0; i < 2; i++) { User u; u.full_name = i ? "ΒΑΣΙΛΕΙΟΥ" : "ΑΛΕΞΙΟΥ"; u.apartment_id = i ? a2 : a1; c.addUser(u); users[i] = i + 1; }

        Service hs; hs.label = "ΘΕΡΜΑΝΣΗ"; int jid = c.addJointService(hs, "heating_oil");
        CHECK(jid > 0);
        auto cat = c.getJointCatalog();
        CHECK(cat.size() == 1 && cat[0].kind == "heating_oil");
        CHECK(c.countServices("") == 0);                       // catalog entries are not ordinary services

        JointOp op; op.building_id = bid; op.source_type = "joint"; op.source_id = jid; op.kind = "heating_oil";
        op.label = "Πετρέλαιο Οκτ."; op.total = 100.00; op.op_date = "2026-10-11"; op.params_json = "{\"litres\":80}";
        std::vector<JointLine> lines = {{cat[0].service.id, 100.00}};
        JointShare s1; s1.apartment_id = a1; s1.user_id = users[0]; s1.amount = 60.00;
        JointShare s2; s2.apartment_id = a2; s2.user_id = users[1]; s2.amount = 40.00;
        JointShare s3; s3.apartment_id = a3; s3.included = false; s3.exclude_reason = "κενό";

        // a split, never a multiplication
        JointShare big = s2; big.amount = 100.00;
        CHECK(c.chargeJoint(op, lines, {s1, big, s3}) == -1); c.takeLastError();
        JointShare noReason = s3; noReason.exclude_reason = " ";
        CHECK(c.chargeJoint(op, lines, {s1, s2, noReason}) == -1); c.takeLastError();
        CHECK(c.countJointOps(-1) == 0);

        int opId = c.chargeJoint(op, lines, {s1, s2, s3});
        CHECK(opId > 0);
        auto o = c.getJointOpById(opId);
        CHECK(o.included == 2 && o.excluded == 1 && o.total == 100.00 && o.building_address == "Πατησίων 205");
        auto sh = c.getJointShares(opId);
        CHECK(sh.size() == 3 && !sh[2].included && sh[2].exclude_reason == "κενό");
        auto ch = c.getJointOpCharges(opId);
        CHECK(ch.size() == 2);
        double sum = 0; for (int id : ch) { auto p = c.getPaymentById(id); sum += p.amount_due; CHECK(p.status == "pending" && p.apartment_id > 0); }
        CHECK(ToCents(sum) == 10000);

        // a package: each share is spread over the components, to the cent
        Service x; x.label = "Α"; x.suggested_price = 10; int sa = c.addServiceGetId(-1, x);
        x.label = "Β"; int sb = c.addServiceGetId(-1, x);
        JointOp pk = op; pk.source_type = "package"; pk.kind = "equal"; pk.label = "Πακέτο"; pk.total = 10.00; pk.params_json = "{}";
        JointShare p1 = s1; p1.amount = 3.33; JointShare p2 = s2; p2.amount = 6.67;
        int pkId = c.chargeJoint(pk, {{sa, 3.00}, {sb, 7.00}}, {p1, p2});
        CHECK(pkId > 0);
        long long pc = 0; for (int id : c.getJointOpCharges(pkId)) pc += ToCents(c.getPaymentById(id).amount_due);
        CHECK(pc == 1000);
        CHECK(c.getJointOps(bid, 0, 10).size() == 2);

        CHECK(c.verifyAuditLog().ok);

        // errors end up in the CSV
        CHECK(c.chargeJoint(op, lines, {}) == -1);
        std::string e = c.takeLastError();
        CHECK(!e.empty());
        auto logged = errlog::ReadAll();
        CHECK(!logged.empty() && logged[0].message == e && logged[0].source == "database");
    }
    {   // the immutability triggers, directly
        sqlite3* raw = nullptr; sqlite3_open(db.c_str(), &raw);
        CHECK(sqlite3_exec(raw, "UPDATE joint_ops SET total = 1", nullptr, nullptr, nullptr) != SQLITE_OK);
        CHECK(sqlite3_exec(raw, "DELETE FROM joint_shares", nullptr, nullptr, nullptr) != SQLITE_OK);
        sqlite3_close(raw);
    }
    fs::remove_all(dir);
    std::printf(fails ? "joint_test: %d FAILED\n" : "joint_test: all passed\n", fails);
    return fails ? 1 : 0;
}
