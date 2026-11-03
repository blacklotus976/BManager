// Receipts must belong to the right entity: a later payment of a joint share is a JOINT receipt (managed list),
// and two entities with the same name never share receipts.
#include "SqliteConn.h"
#include "Money.h"
#include <cstdio>
#include <filesystem>

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } } while (0)

static bool Has(const std::vector<Receipt>& v, int id) { for (auto& r : v) if (r.id == id) return true; return false; }

int main() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "bmanager_receipt_probe";
    fs::remove_all(dir); fs::create_directories(dir);
    {
        SqliteConn c((dir / "data.db").string());
        Building b; b.address = "Πατησίων 1"; int bid = c.addBuilding(b);
        c.setBuildingManaged(bid, true);
        int a1 = c.addApartment(bid, "Α1");
        // a person and a building client with the SAME name, plus a resident
        User p; p.full_name = "ΠΑΠΑΔΟΠΟΥΛΟΣ"; p.apartment_id = a1; c.addUser(p); int pid = c.countUsers("", 0, {});
        User q; q.full_name = "ΠΑΠΑΔΟΠΟΥΛΟΣ"; q.kind = "business"; c.addUser(q); int qid = pid + 1;
        Service s; s.label = "ΔΟΚΙΜΗ"; s.suggested_price = 10; int svc = c.addServiceGetId(-1, s);

        JointOp op; op.building_id = bid; op.source_type = "service"; op.source_id = svc; op.label = "Κοινόχρηστο"; op.total = 100; op.op_date = "2026-10-01";
        JointShare sh; sh.apartment_id = a1; sh.user_id = pid; sh.amount = 100;
        int opId = c.chargeJoint(op, {{svc, 100}}, {sh});
        if (opId <= 0) std::printf("joint refused: %s\n", c.takeLastError().c_str());
        CHECK(opId > 0);
        auto charges = c.getJointOpCharges(opId);
        CHECK(!charges.empty());

        // later the resident pays his share: a payment recorded afterwards, with a receipt
        MoneyPayment m; m.user_id = pid; m.pay_date = "2026-10-05"; m.amount = 100; m.direction = "in"; m.method = "cash";
        Receipt r1; r1.user_id = pid; r1.method = "cash";
        std::vector<Allocation> al; for (int cid : charges) { Allocation a; a.charge_id = cid; a.amount = 100; al.push_back(a); }
        int rid1 = c.recordPayment(m, al, &r1);
        if (rid1 <= 0) std::printf("recordPayment refused: %s\n", c.takeLastError().c_str());
        CHECK(rid1 > 0);

        // the namesake pays for his own service
        Payment own; own.user_id = qid; own.service_id = svc; own.year = 2026; own.month = 10; own.day = 6; own.amount_due = 20; own.amount_paid = 20; own.status = "paid"; own.direction = "in"; own.apartment_id = 0;
        Receipt r2; r2.user_id = qid; r2.method = "cash";
        CHECK(c.addPaymentsWithReceipt({own}, "", &r2) > 0);
        auto all = c.getReceipts(0, 100);
        int rid2 = -1; for (auto& r : all) if (r.user_id == qid) rid2 = r.id;
        CHECK(rid2 > 0);

        DataConn::ReceiptQuery managed; managed.kind = "managed";
        DataConn::ReceiptQuery clients; clients.kind = "clients";
        auto mg = c.searchReceipts(managed, 0, 100), cl = c.searchReceipts(clients, 0, 100);
        // the joint-share receipt is a managed one, whatever batch the money itself was recorded under
        CHECK(Has(mg, rid1) && !Has(cl, rid1));
        CHECK(!Has(mg, rid2) && Has(cl, rid2));

        // by client id: each namesake sees only his own receipt
        DataConn::ReceiptQuery byP; DataConn::ReceiptCond cp; cp.field = DataConn::ReceiptCond::Client; cp.id = pid; byP.conds.push_back(cp);
        DataConn::ReceiptQuery byQ; DataConn::ReceiptCond cq; cq.field = DataConn::ReceiptCond::Client; cq.id = qid; byQ.conds.push_back(cq);
        auto rp = c.searchReceipts(byP, 0, 100), rq = c.searchReceipts(byQ, 0, 100);
        CHECK(Has(rp, rid1) && !Has(rp, rid2));
        CHECK(Has(rq, rid2) && !Has(rq, rid1));
        CHECK(Has(c.getReceiptsForUser(pid), rid1) && !Has(c.getReceiptsForUser(pid), rid2));

        // the "kind" shown on each receipt row
        for (auto& r : c.searchReceipts(DataConn::ReceiptQuery{}, 0, 100)) {
            if (r.id == rid1) CHECK(r.kind == "joint");
            if (r.id == rid2) CHECK(r.kind == "personal");
        }
    }
    std::printf(fails ? "receipt_probe: %d FAILED\n" : "receipt_probe: ok\n", fails);
    return fails ? 1 : 0;
}
