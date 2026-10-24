// Residents (Διαχείριση) are not clients: a stay in an apartment with name, phone, email, start and end; coming back is a
// new stay of the same person; his contributions, payments and receipts hang on his own record, which is never listed,
// searched or rated as a client.
#include "SqliteConn.h"
#include "Money.h"
#include <cstdio>
#include <filesystem>

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } } while (0)

int main() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "bmanager_residents_test";
    fs::remove_all(dir); fs::create_directories(dir);
    {
        SqliteConn c((dir / "data.db").string());
        Building b; b.address = "ΑΙΓΑΙΟΥ 9"; int bid = c.addBuilding(b);
        int a1 = c.addApartment(bid, "Α1"), a2 = c.addApartment(bid, "Α2");
        CHECK(c.setBuildingManaged(bid, true));
        User client; client.full_name = "ΠΕΛΑΤΗΣ ΜΑΣ"; CHECK(c.addUser(client));
        const int clientsBefore = c.countUsers("", 0, {});

        DataConn::Resident r; r.apartment_id = a1; r.full_name = "ΑΛΕΞΙΟΥ ΜΑΡΙΑ"; r.phone = "6900000001"; r.email = "m@x.gr"; r.start_date = "2025-01-01";
        int s1 = c.addResident(r);
        CHECK(s1 > 0);
        DataConn::Resident bad = r; bad.full_name = "  "; CHECK(c.addResident(bad) < 0); c.takeLastError();
        bad = r; bad.apartment_id = 99999; CHECK(c.addResident(bad) < 0); c.takeLastError();
        DataConn::Resident r2; r2.apartment_id = a2; r2.full_name = "ΒΑΣΙΛΕΙΟΥ ΝΙΚΟΣ"; r2.start_date = "2025-03-01";
        int s2 = c.addResident(r2);
        CHECK(s2 > 0);

        // not clients: not in the list, not counted, not found by search
        CHECK(c.countUsers("", 0, {}) == clientsBefore && c.countUsers("ΑΛΕΞΙΟΥ", 0, {}) == 0);
        // but the building knows them, and they can be charged like residents always were
        auto now = c.getResidents(bid, false);
        CHECK(now.size() == 2 && now[0].full_name == "ΑΛΕΞΙΟΥ ΜΑΡΙΑ" && now[0].apartment == "Α1" && now[0].building == "ΑΙΓΑΙΟΥ 9" && now[0].active());
        auto br = c.getBuildingResidents(bid);
        CHECK(br.size() == 2);
        DataConn::Resident st1 = c.getResidentById(s1);
        User acc = c.getUserById(st1.user_id);
        CHECK(acc.resident && acc.apartment_id == a1 && acc.email == "m@x.gr");

        // she pays a contribution with a receipt
        Service fee; fee.label = "ΕΙΣΦΟΡΑ"; fee.suggested_price = 30; int sFee = c.addServiceGetId(-1, fee);
        JointOp op; op.building_id = bid; op.source_type = "service"; op.source_id = sFee; op.kind = "contribution"; op.label = "Εισφορά"; op.total = 30; op.op_date = "2025-02-01";
        JointShare sh; sh.apartment_id = a1; sh.user_id = st1.user_id; sh.amount = 30; sh.paid = 30;
        Receipt rc; rc.method = "cash";
        CHECK(c.chargeJoint(op, {{sFee, 30}}, {sh}, 0, -1, -1, &rc) > 0);
        auto mv = c.residentFundMoves(st1.user_id);
        CHECK(mv.size() == 1 && mv[0].kind == "contribution" && ToCents(mv[0].amount) == 3000 && mv[0].receipt_id > 0 && mv[0].who == "ΑΙΓΑΙΟΥ 9");

        // she moves out: the stay ends, she no longer "lives" anywhere, her history stays
        CHECK(!c.endResident(s1, "2024-12-31")); c.takeLastError();          // before she came
        // she is charged 20 for a repair and pays nothing: that is what she owes when she leaves
        JointOp rp; rp.building_id = bid; rp.source_type = "service"; rp.source_id = sFee; rp.label = "Επισκευή"; rp.total = 20; rp.op_date = "2025-05-01";
        JointShare rs; rs.apartment_id = a1; rs.user_id = st1.user_id; rs.amount = 20;
        CHECK(c.chargeJoint(rp, {{sFee, 20}}, {rs}) > 0);
        // a management fee is OUR money: charged and paid, it never enters the building's fund
        double before = c.getFundBalance(bid);
        JointOp fee2; fee2.building_id = bid; fee2.source_type = "service"; fee2.source_id = sFee; fee2.kind = "fee"; fee2.label = "Αμοιβή διαχείρισης"; fee2.total = 5; fee2.op_date = "2025-05-02";
        JointShare fs; fs.apartment_id = a1; fs.user_id = st1.user_id; fs.amount = 5; fs.paid = 5;
        CHECK(c.chargeJoint(fee2, {{sFee, 5}}, {fs}, 0, -1, -1, &rc) > 0);
        CHECK(ToCents(c.getFundBalance(bid)) == ToCents(before));
        CHECK(c.endResident(s1, "2025-06-30"));
        CHECK(ToCents(c.getResidentById(s1).exit_owed) == 2000);
        CHECK(!c.endResident(s1, "2025-07-01")); c.takeLastError();
        CHECK(c.getResidents(bid, false).size() == 1 && c.getResidents(bid, true).size() == 2);
        CHECK(c.getUserById(st1.user_id).apartment_id == -1 && c.getBuildingResidents(bid).size() == 1);

        // she comes back, to Α2 this time: a new stay of the SAME person (her record and history continue)
        DataConn::Resident again = st1; again.apartment_id = a2; again.start_date = "2026-01-10"; again.end_date = "";
        int s3 = c.addResident(again, s1);
        CHECK(s3 > 0 && s3 != s1);
        CHECK(c.getResidentById(s3).user_id == st1.user_id && c.getResidentStays(st1.user_id).size() == 2);
        CHECK(c.getUserById(st1.user_id).apartment_id == a2);
        CHECK(c.residentFundMoves(st1.user_id).size() == 1);
        CHECK(c.addResident(again, s3) < 0); c.takeLastError();                 // still living there

        // contact details: changed on the stay and on her record
        CHECK(c.editResident(s3, "ΑΛΕΞΙΟΥ Μ. ΜΑΡΙΑ", "6900000009", "new@x.gr", "κλειδί στο θυρωρείο"));
        CHECK(c.getResidentById(s3).phone == "6900000009" && c.getUserById(st1.user_id).full_name == "ΑΛΕΞΙΟΥ Μ. ΜΑΡΙΑ");

        // apartments with their current residents, the previous ones apart, one page at a time
        CHECK(c.countAptResidents(bid, false, "") == 3);                         // Α1 (empty now), Α2: Βασιλείου and Αλεξίου
        auto cur = c.pageAptResidents(bid, false, "", {{1, true}}, 0, 10);
        CHECK(cur.size() >= 2);
        CHECK(c.countAptResidents(bid, true, "") == 1 && c.pageAptResidents(bid, true, "", {}, 0, 10)[0].r.end_date == "2025-06-30");
        CHECK(c.countAptResidents(bid, false, "ΒΑΣΙΛ") == 1);
        CHECK(c.countManagedBuildings("") == 1 && c.pageManagedBuildings("", {{7, false}}, 0, 10)[0].building_id == bid);
        CHECK(c.countFees("", "") == 1 && c.pageFees("", "", {}, 0, 10)[0].kind == "fee");

        // residents are rated on their own: charged 30 + 20 for the fund (20 open), 5 fee paid
        {   auto rq = c.residentQuality("", "");
            bool found = false;
            for (auto& q : rq) if (q.user_id == st1.user_id && q.building_id == bid) {
                found = true;
                CHECK(ToCents(q.fundCharged) == 5000 && ToCents(q.fundOpen) == 2000 && ToCents(q.feeCharged) == 500 && ToCents(q.feeOpen) == 0);
                CHECK(q.idxDen > 0 && q.idxNum < q.idxDen && q.apartment == "Α2");
                CHECK(ToCents(q.fundOverdue) == 2000 && ToCents(q.feeOverdue) == 0);          // open since 2025: late
            }
            CHECK(found);
        }
        // residents are not rated with the clients
        for (auto& q : c.clientQuality()) CHECK(q.user_id != st1.user_id);

        auto chk = c.verifyAuditLog();
        for (auto& pr : chk.problems) std::printf("audit: %s\n", pr.c_str());
        CHECK(chk.ok);
    }
    fs::remove_all(dir);
    if (fails) { std::printf("%d check(s) failed\n", fails); return 1; }
    std::printf("residents: all checks passed\n");
    return 0;
}
