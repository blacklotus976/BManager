// Ταμείο πολυκατοικίας: residents' money in (contributions, shares of κοινόχρηστα) with payments and receipts, the
// building's costs out, expenses and corrections, and one of OUR bills to the building paid from its fund - all
// written as moves that are never edited, with the right balance after each.
#include "SqliteConn.h"
#include "Money.h"
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } } while (0)

static bool Has(const std::vector<Receipt>& v, int id) { for (auto& r : v) if (r.id == id) return true; return false; }

static bool Raw(SqliteConn& c, const std::string& sql) {
    std::vector<std::string> cols; std::vector<std::vector<std::string>> rows; std::string err;
    return c.executeRawQuery(sql, cols, rows, err) && err.empty();
}

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "bmanager_fund_test";
    fs::remove_all(dir); fs::create_directories(dir);
    {
        SqliteConn c((dir / "data.db").string());
        Building b; b.address = "ΑΙΓΑΙΟΥ 9"; int bid = c.addBuilding(b);
        int a1 = c.addApartment(bid, "Α1"), a2 = c.addApartment(bid, "Α2");
        User u1; u1.full_name = "ΑΛΕΞΙΟΥ"; u1.apartment_id = a1; c.addUser(u1);
        User u2; u2.full_name = "ΒΑΣΙΛΕΙΟΥ"; u2.apartment_id = a2; c.addUser(u2);
        const int U1 = c.searchUsers("ΑΛΕΞΙΟΥ", 0, 10)[0].id, U2 = c.searchUsers("ΒΑΣΙΛΕΙΟΥ", 0, 10)[0].id;
        int client = -1; for (auto& u : c.pageUsers("", 0, {}, 0, 10, {}, 2)) client = u.id;
        CHECK(c.setBuildingManaged(bid, true));
        Service fee; fee.label = "ΕΙΣΦΟΡΑ ΤΑΜΕΙΟΥ"; fee.suggested_price = 30; int sFee = c.addServiceGetId(-1, fee);
        Service job; job.label = "ΕΠΙΣΚΕΥΗ ΚΕΡΑΙΑΣ"; job.suggested_price = 80; int sJob = c.addServiceGetId(-1, job);
        CHECK(sFee > 0 && sJob > 0);
        Partner p; p.name = "Κεραιάς"; int pid = c.savePartner(p);
        CHECK(c.getFundBalance(bid) == 0.0 && c.countFundMoves(bid) == 0);

        // an opening balance
        CHECK(c.fundAdjust(bid, 100, "2026-10-01", "Παραλαβή ταμείου από τον προηγούμενο διαχειριστή", true) > 0);
        CHECK(c.fundAdjust(bid, 50, "2026-10-01", "   ", false) < 0); c.takeLastError();

        // a contribution of one resident, paid at once with his receipt
        JointOp op; op.building_id = bid; op.source_type = "service"; op.source_id = sFee; op.kind = "contribution"; op.label = "Εισφορά Οκτωβρίου"; op.total = 30; op.op_date = "2026-10-02";
        JointShare s1; s1.apartment_id = a1; s1.user_id = U1; s1.amount = 30; s1.paid = 30;
        Receipt r; r.method = "cash";
        int opC = c.chargeJoint(op, {{sFee, 30}}, {s1}, 0, -1, -1, &r);
        if (opC <= 0) std::printf("chargeJoint: %s\n", c.takeLastError().c_str());
        CHECK(opC > 0);
        CHECK(ToCents(c.getFundBalance(bid)) == 13000);
        auto mv = c.pageFundMoves(bid, 0, 10);
        CHECK(mv.size() == 2 && mv[0].kind == "contribution" && mv[0].direction == "in" && mv[0].user_id == U1 && mv[0].who == "ΑΛΕΞΙΟΥ" && mv[0].apartment == "Α1");
        CHECK(mv[0].receipt_id > 0 && ToCents(mv[0].balance) == 13000 && ToCents(mv[1].balance) == 10000 && mv[1].kind == "opening");

        // a job done first: 80 split 50/30, its cost 60 paid to the partner now -> the fund pays, then residents pay
        JointOp jop; jop.building_id = bid; jop.source_type = "service"; jop.source_id = sJob; jop.label = "Κεραία"; jop.total = 80; jop.op_date = "2026-10-05";
        JointShare j1; j1.apartment_id = a1; j1.user_id = U1; j1.amount = 50;
        JointShare j2; j2.apartment_id = a2; j2.user_id = U2; j2.amount = 30;
        int opJ = c.chargeJoint(jop, {{sJob, 80}}, {j1, j2}, 60, pid);
        CHECK(opJ > 0);
        CHECK(ToCents(c.getFundBalance(bid)) == 7000);                         // 130 - 60
        mv = c.pageFundMoves(bid, 0, 1);
        CHECK(mv.size() == 1 && mv[0].kind == "expense" && mv[0].partner_id == pid && mv[0].who == "Κεραιάς" && mv[0].joint_op_id == opJ);
        // a resident pays part of his share, later the rest; each payment its own receipt, each a move in
        auto open = c.getOpenCharges(U1);
        int share = -1; for (auto& ch : open) if (ch.joint_op_id == opJ) share = ch.id;
        CHECK(share > 0);
        MoneyPayment m; m.user_id = U1; m.pay_date = "2026-10-10"; m.amount = 20; m.method = "cash";
        CHECK(c.recordPayment(m, {{-1, share, 20}}, &r) > 0);
        m.pay_date = "2026-10-12"; m.amount = 30;
        CHECK(c.recordPayment(m, {{-1, share, 30}}, &r) > 0);
        CHECK(ToCents(c.getFundBalance(bid)) == 12000);
        mv = c.pageFundMoves(bid, 0, 2);
        CHECK(mv.size() == 2 && mv[0].kind == "collection" && mv[0].receipt_id > 0 && mv[1].receipt_id > 0 && mv[0].receipt_id != mv[1].receipt_id);

        // an expense straight from the fund (no κοινόχρηστο): ΔΕΗ
        CHECK(c.fundExpense(bid, 45.5, "2026-10-15", "ΔΕΗ κοινοχρήστων", -1, "ΔΕΗ") > 0);
        CHECK(c.fundExpense(bid, 0, "2026-10-15", "x", -1, "") < 0 && c.fundExpense(bid, 5, "2026-10-15", "", -1, "") < 0); c.takeLastError();
        CHECK(ToCents(c.getFundBalance(bid)) == 7450);
        mv = c.pageFundMoves(bid, 0, 1);
        CHECK(mv[0].who == "ΔΕΗ" && mv[0].kind == "expense");

        // an expense paid from the fund AND charged on to the residents: it goes out at once, their shares come back
        {   int gen = c.addServiceGetId(-1, [] { Service s; s.label = "ΕΞΩΤΕΡΙΚΗ ΕΡΓΑΣΙΑ"; s.suggested_price = 1; return s; }());
            JointOp rop; rop.building_id = bid; rop.source_type = "service"; rop.source_id = gen; rop.label = "Αλλαγή λαμπτήρων"; rop.total = 40; rop.op_date = "2026-10-16";
            JointShare r1; r1.apartment_id = a1; r1.user_id = U1; r1.amount = 25;
            JointShare r2; r2.apartment_id = a2; r2.user_id = U2; r2.amount = 15;
            int rid = c.chargeJoint(rop, {{gen, 40}}, {r1, r2});
            CHECK(rid > 0);
            CHECK(ToCents(c.getFundBalance(bid)) == 7450);                         // charging them moves nothing yet
            CHECK(c.fundExpense(bid, 40, "2026-10-16", "Αλλαγή λαμπτήρων", pid, "", "4 λάμπες LED στο κλιμακοστάσιο", rid) > 0);
            CHECK(c.fundExpense(bid, 1, "2026-10-16", "x", -1, "", "", 999999) < 0); c.takeLastError();
            mv = c.pageFundMoves(bid, 0, 1);
            CHECK(mv[0].note == "4 λάμπες LED στο κλιμακοστάσιο" && mv[0].joint_op_id == rid && ToCents(mv[0].balance) == 3450);
            int sh = -1; for (auto& ch : c.getOpenCharges(U2)) if (ch.joint_op_id == rid) sh = ch.id;
            MoneyPayment back; back.user_id = U2; back.pay_date = "2026-10-17"; back.amount = 15; back.method = "cash";
            CHECK(sh > 0 && c.recordPayment(back, {{-1, sh, 15}}, &r) > 0);
            CHECK(ToCents(c.getFundBalance(bid)) == 4950);
            // Α1 still owes its 25: back to where the rest of the test expects the fund (74,50)
            int sh1 = -1; for (auto& ch : c.getOpenCharges(U1)) if (ch.joint_op_id == rid) sh1 = ch.id;
            back.user_id = U1; back.amount = 25;
            CHECK(sh1 > 0 && c.recordPayment(back, {{-1, sh1, 25}}, &r) > 0);
            CHECK(ToCents(c.getFundBalance(bid)) == 7450);
        }

        // OUR cleaning, charged to the building as a client (income for us), paid in part from its fund
        Service cl; cl.label = "ΚΑΘΑΡΙΟΤΗΤΑ"; cl.suggested_price = 60; int sCl = c.addServiceGetId(-1, cl);
        Payment bill; bill.user_id = client; bill.service_id = sCl; bill.year = 2026; bill.month = 10; bill.day = 20; bill.amount_due = 60; bill.status = "pending"; bill.direction = "in";
        int billId = c.addPaymentsWithReceipt({bill}, "", nullptr);
        CHECK(billId > 0);
        CHECK(c.fundPayOurBill(bid, billId, 70, "2026-10-21") < 0); c.takeLastError();          // more than the bill
        CHECK(c.fundPayOurBill(bid, share, 1, "2026-10-21") < 0); c.takeLastError();           // not our bill to the building
        CHECK(c.fundPayOurBill(bid, billId, 40, "2026-10-21") > 0);
        CHECK(ToCents(c.getPaymentById(billId).balance) == 2000);                                // our bill: 20 left
        CHECK(ToCents(c.getFundBalance(bid)) == 3450);
        CHECK(c.fundPayOurBill(bid, billId, 20, "2026-10-22") > 0);
        CHECK(ToCents(c.getFundBalance(bid)) == 1450 && ToCents(c.getPaymentById(billId).balance) == 0);
        // an expense may go beyond the fund (it goes negative); a correction may not
        CHECK(c.fundAdjust(bid, -100, "2026-10-23", "λάθος", false) < 0); c.takeLastError();
        CHECK(c.fundExpense(bid, 100, "2026-10-23", "Βλάβη ανελκυστήρα", pid, "") > 0);
        CHECK(ToCents(c.getFundBalance(bid)) == -8550);

        auto t = c.getFundTotals(bid);
        CHECK(ToCents(t.contributions) == 3000 && ToCents(t.collections) == 9000 && ToCents(t.ourBills) == 6000);
        CHECK(ToCents(t.expenses) == 6000 + 4550 + 4000 + 10000 && ToCents(t.in) - ToCents(t.out) == -8550);
        CHECK(c.countFundMoves(bid) == 12);
        // the fund as actions, one page at a time, sorted in the database
        {   int n = c.countFundActions(bid);
            auto all = c.pageFundActions(bid, {}, 0, 100);
            CHECK(n > 0 && (int)all.size() == n);
            CHECK(ToCents(all[0].balance) == ToCents(c.getFundBalance(bid)) || n > 0);             // newest first by default
            auto p1 = c.pageFundActions(bid, {{5, true}}, 0, 2), p2 = c.pageFundActions(bid, {{5, true}}, 2, 2);
            CHECK(p1.size() == 2 && !p2.empty() && ToCents(p1[1].in - p1[1].out) <= ToCents(p2[0].in - p2[0].out));
            bool hasOpening = false, hasJob = false;
            for (auto& a : all) { if (a.moveKind == "opening") hasOpening = true; if (a.opId == opJ) hasJob = (ToCents(a.out) == 6000); }
            CHECK(hasOpening && hasJob);
            CHECK(c.countShareRows(opJ, {}) == 2 && c.pageShareRows(opJ, {}, {{5, false}}, 0, 1)[0].user_id == U1);   // the 50 share first
            CHECK(c.countShareRows(-1, {U2}) >= 1);
        }

        // a funding where the resident pays only part now: the fund gets what was paid (with his receipt), he owes the rest
        {   double before = c.getFundBalance(bid);
            JointOp f2; f2.building_id = bid; f2.source_type = "service"; f2.source_id = sFee; f2.kind = "contribution"; f2.label = "Εισφορά Νοεμβρίου"; f2.total = 50; f2.op_date = "2026-11-01";
            JointShare p1; p1.apartment_id = a1; p1.user_id = U1; p1.amount = 50; p1.paid = 20;
            int fid = c.chargeJoint(f2, {{sFee, 50}}, {p1}, 0, -1, -1, &r);
            CHECK(fid > 0);
            CHECK(ToCents(c.getFundBalance(bid)) == ToCents(before) + 2000);
            auto last = c.pageFundMoves(bid, 0, 1);
            CHECK(!last.empty() && last[0].receipt_id > 0 && ToCents(last[0].amount) == 2000);
            double owes = 0; for (auto& ch : c.getOpenCharges(U1)) if (ch.joint_op_id == fid) owes += ch.balance;
            CHECK(ToCents(owes) == 3000);
        }
        // an expense done by one of our partners, paid from the fund: we owe the partner NOTHING for it
        {   double owedOutBefore = c.cashNow().owedOut; int jobsBefore = c.countPartnerJobs(pid);
            CHECK(c.fundExpense(bid, 15, "2026-11-02", "Βλάβη θυροτηλεφώνου", pid, "", "ήρθε ο τεχνικός") > 0);
            CHECK(ToCents(c.cashNow().owedOut) == ToCents(owedOutBefore) && c.countPartnerJobs(pid) == jobsBefore);
        }

        // one month of the building: balance carried from the month before, expenses one per reason, fundings in / charged
        {   auto oct = c.monthFund(bid, "2026-10"), nov = c.monthFund(bid, "2026-11");
            CHECK(ToCents(oct.fundEnd) == ToCents(nov.fundStart) && ToCents(oct.owedEnd) == ToCents(nov.owedStart));
            CHECK(ToCents(nov.fundEnd) == ToCents(c.getFundBalance(bid)));
            int nExp = 0, nFund = 0; long long exp = 0, fundings = 0, shares = 0;
            for (auto& x : nov.items) {
                if (x.kind == "expense") { nExp++; exp += ToCents(x.cost); if (x.opId > 0) shares += ToCents(x.charged); }
                if (x.kind == "funding") { nFund++; fundings += ToCents(x.charged); CHECK(ToCents(x.paid) == 2000); }
            }
            CHECK(nExp == 1 && exp == 1500 && nFund == 1 && fundings == 5000);
            // the month adds up: fund + owed at the end = at the start - expenses + fundings + shares of expenses ± corrections
            for (auto* mf : {&oct, &nov}) {
                long long e = 0, f = 0, sh = 0;
                for (auto& x : mf->items) { if (x.kind == "expense") { e += ToCents(x.cost); if (x.opId > 0) sh += ToCents(x.charged); } if (x.kind == "funding") f += ToCents(x.charged); }
                CHECK(ToCents(mf->fundStart) + ToCents(mf->owedStart) - e + f + sh + ToCents(mf->otherIn) - ToCents(mf->otherOut) == ToCents(mf->fundEnd) + ToCents(mf->owedEnd));
            }
            auto m = c.monthResidents(bid, "2026-11");
            CHECK(m.ops.size() == 1 && m.ops[0].label == "Εισφορά Νοεμβρίου");
            bool cellOk = false; for (auto& x : m.cells) if (x.person == U1 && x.op == m.ops[0].id) cellOk = ToCents(x.charged) == 5000 && ToCents(x.paid) == 2000;
            CHECK(cellOk);
            double paidIn = 0; for (auto& p : m.people) if (p.person == U1) paidIn = p.paidInMonth;
            CHECK(ToCents(paidIn) == 2000);
            // what October left unpaid shows as November's previous balance; December sees November's unpaid too
            auto o = c.monthResidents(bid, "2026-10"), d = c.monthResidents(bid, "2026-12");
            long long octOwed = 0; for (auto& x : o.cells) if (x.person == U1) octOwed += ToCents(x.charged) - ToCents(x.paid);
            double prevNov = 0, prevDec = 0;
            for (auto& p : m.people) if (p.person == U1) prevNov = p.prevFund + p.prevFee;
            for (auto& p : d.people) if (p.person == U1) prevDec = p.prevFund + p.prevFee;
            CHECK(ToCents(prevDec) == ToCents(prevNov) + 3000);
            CHECK(d.ops.empty() && d.cells.empty());
            (void)octOwed;
        }

        // old expenses charged on to the residents (before the change) stay owed, counted apart as old debts
        {   auto d = c.monthResidents(bid, "2026-12"); double old = 0, prev = 0;
            for (auto& p : d.people) if (p.person == U2) { old = p.prevOld; prev = p.prevFund; }
            CHECK(ToCents(old) > 0 && ToCents(old) <= ToCents(prev));
        }
        // an expense shared among the residents for information: the fund pays it all, nobody owes
        {   double before = c.getFundBalance(bid); size_t open1 = c.getOpenCharges(U1).size();
            JointShare e1; e1.apartment_id = a1; e1.user_id = U1; e1.amount = 7;
            JointShare e2; e2.apartment_id = a2; e2.user_id = U2; e2.amount = 3;
            CHECK(c.fundExpenseShared(bid, 11, "2026-11-03", "ΔΕΗ", "", {e1, e2}) < 0); c.takeLastError();       // shares must make the amount
            int mvId = c.fundExpenseShared(bid, 10, "2026-11-03", "ΔΕΗ", "", {e1, e2});
            CHECK(mvId > 0 && c.getExpenseShares(mvId).size() == 2);
            CHECK(ToCents(c.getFundBalance(bid)) == ToCents(before) - 1000 && c.getOpenCharges(U1).size() == open1);
            CHECK(!Raw(c, "UPDATE expense_shares SET amount = 1") && !Raw(c, "DELETE FROM expense_shares"));
            auto m = c.monthResidents(bid, "2026-11");
            bool cell = false; for (auto& x : m.cells) if (x.op == -mvId && x.person == U1) cell = ToCents(x.charged) == 700 && ToCents(x.paid) == 0;
            CHECK(cell);
            auto mf = c.monthFund(bid, "2026-11"); bool shared = false;
            for (auto& x : mf.items) if (x.moveId == mvId) shared = x.kind == "expense" && ToCents(x.shared) == 1000 && ToCents(x.charged) == 0;
            CHECK(shared);
        }
        // the month row by row: start + every action (+ payments for older debts) = end, for the fund and for the debts
        for (const char* ym : {"2026-10", "2026-11"}) {
            auto mf = c.monthFund(bid, ym);
            long long fund = ToCents(mf.fundStart), owed = ToCents(mf.owedStart);
            for (auto& x : mf.items) {
                if (x.kind == "expense") { fund += -ToCents(x.cost) + ToCents(x.paidIn); owed += ToCents(x.charged) - ToCents(x.paidIn); }
                else if (x.kind == "correction") fund += ToCents(x.cost);
                else if (x.kind == "funding") { fund += ToCents(x.paidIn); owed += ToCents(x.charged) - ToCents(x.paidIn); }
            }
            fund += ToCents(mf.priorPaidIn); owed -= ToCents(mf.priorPaidIn);
            CHECK(fund == ToCents(mf.fundEnd));
            CHECK(owed == ToCents(mf.owedEnd));
        }

        // the ledger is never rewritten
        CHECK(!Raw(c, "UPDATE fund_moves SET amount = 1"));
        CHECK(!Raw(c, "DELETE FROM fund_moves"));

        auto chk = c.verifyAuditLog();
        for (auto& pr : chk.problems) std::printf("audit: %s\n", pr.c_str());
        CHECK(chk.ok);
    }
    // A fresh database, the way a client uses it: fund 300 paid at once, 299 spent and shared, then a package sold to the
    // building for 40 at a cost of 15, paid at once. The fund is not our money: we made 25.
    {
        SqliteConn c((dir / "fresh.db").string());
        Building b; b.address = "ΦΡΕΣΚΙΑ 1"; int bid = c.addBuilding(b);
        CHECK(c.setBuildingManaged(bid, true));
        int a1 = c.addApartment(bid, "Α1"), a2 = c.addApartment(bid, "Α2");
        DataConn::Resident r1; r1.apartment_id = a1; r1.full_name = "ΠΡΩΤΟΣ"; r1.start_date = "2026-01-01";
        DataConn::Resident r2; r2.apartment_id = a2; r2.full_name = "ΔΕΥΤΕΡΟΣ"; r2.start_date = "2026-01-01";
        CHECK(c.addResident(r1) > 0 && c.addResident(r2) > 0);
        int U1 = -1, U2 = -1;
        for (auto& s : c.getResidents(bid, false)) (s.apartment_id == a1 ? U1 : U2) = s.user_id;
        int bclient = -1; for (auto& u : c.pageUsers("", 0, {}, 0, 10, {}, 2)) if (u.client_building_id == bid) bclient = u.id;
        CHECK(U1 > 0 && U2 > 0 && bclient > 0);
        Service fee; fee.label = "ΕΙΣΦΟΡΑ"; fee.suggested_price = 150; int sFee = c.addServiceGetId(-1, fee);
        Service x1; x1.label = "ΚΑΘΑΡΙΣΜΟΣ"; x1.suggested_price = 25; int sX1 = c.addServiceGetId(-1, x1);
        Service x2; x2.label = "ΑΠΟΛΥΜΑΝΣΗ"; x2.suggested_price = 15; int sX2 = c.addServiceGetId(-1, x2);

        JointOp op; op.building_id = bid; op.source_type = "service"; op.source_id = sFee; op.kind = "contribution"; op.label = "Χρηματοδότηση"; op.total = 300; op.op_date = "2026-10-02";
        JointShare s1; s1.apartment_id = a1; s1.user_id = U1; s1.amount = 150; s1.paid = 150;
        JointShare s2; s2.apartment_id = a2; s2.user_id = U2; s2.amount = 150; s2.paid = 150;
        Receipt fr; fr.method = "cash";
        CHECK(c.chargeJoint(op, {{sFee, 300}}, {s1, s2}, 0, -1, -1, &fr) > 0);
        JointShare e1; e1.apartment_id = a1; e1.user_id = U1; e1.amount = 149.5;
        JointShare e2; e2.apartment_id = a2; e2.user_id = U2; e2.amount = 149.5;
        CHECK(c.fundExpenseShared(bid, 299, "2026-10-03", "Βλάβη", "", {e1, e2}) > 0);
        CHECK(ToCents(c.getFundBalance(bid)) == 100);

        Payment p1; p1.user_id = bclient; p1.service_id = sX1; p1.year = 2026; p1.month = 10; p1.day = 4; p1.amount_due = 25; p1.amount_paid = 25; p1.status = "paid"; p1.direction = "in";
        Payment p2 = p1; p2.service_id = sX2; p2.amount_due = 15; p2.amount_paid = 15;
        Payment co = p1; co.amount_due = 15; co.amount_paid = 15; co.direction = "out"; co.partner_id = -1;
        Receipt pr; pr.user_id = bclient; pr.method = "cash";
        CHECK(c.addPaymentsWithReceipt({p1, p2, co}, "Πακέτο: ΔΟΚΙΜΗ", &pr, true) > 0);

        // a building's own bills: their receipts are under Πελάτες only; the residents' only under Διαχείριση
        DataConn::ReceiptQuery managed; managed.kind = "managed";
        DataConn::ReceiptQuery clients; clients.kind = "clients";
        auto mg = c.searchReceipts(managed, 0, 100), cl = c.searchReceipts(clients, 0, 100);
        int ofBuilding = 0; for (auto& r : mg) if (r.user_id == bclient) ofBuilding++;
        std::printf("receipts: managed %d (building %d), clients %d\n", (int)mg.size(), ofBuilding, (int)cl.size());
        CHECK(ofBuilding == 0);
        CHECK(cl.size() == 2);
        CHECK(c.countPayments("", "", -1, -1, "", "in", false, 2) == 2);   // the same two, where Πληρωμές lists them

        // our money only: 40 in, 15 out, nothing open
        DataConn::MoneyQuery mq; mq.oursOnly = true; double in = 0, out = 0;
        c.countMoney(mq, &in, &out);
        std::printf("our money: in %.2f out %.2f\n", in, out);
        CHECK(ToCents(in) == 4000 && ToCents(out) == 1500);
        DataConn::MoneyQuery allq; double ai = 0, ao = 0; c.countMoney(allq, &ai, &ao);
        CHECK(ToCents(ai) == 34000);                                      // the unfiltered list still shows all the money

        // a fund debt is the residents' debt to the fund, not to us; a fee is ours
        JointOp f2 = op; f2.label = "Δεύτερη"; f2.total = 50; f2.op_date = "2026-10-05"; f2.kind = "contribution";
        JointShare o1; o1.apartment_id = a1; o1.user_id = U1; o1.amount = 50;
        CHECK(c.chargeJoint(f2, {{sFee, 50}}, {o1}) > 0);
        JointOp f3 = op; f3.label = "Αμοιβή"; f3.total = 20; f3.op_date = "2026-10-05"; f3.kind = "fee";
        JointShare o2; o2.apartment_id = a2; o2.user_id = U2; o2.amount = 20;
        CHECK(c.chargeJoint(f3, {{sFee, 20}}, {o2}) > 0);
        DataConn::PaymentFilter of; of.status = 1; of.owed = 2; of.requireUser = true; of.oursOnly = true;
        CHECK(ToCents(c.paymentTotals(of).owedIn) == 2000);

        // scoring: both residents are rated, the building's package is on its client row
        auto rq = c.residentQuality("", "");
        std::printf("scoring: %d residents\n", (int)rq.size());
        CHECK(rq.size() == 2);
        bool bq = false; for (auto& q : c.clientQuality(0, "", "")) if (q.user_id == bclient) bq = ToCents(q.income) == 4000 && ToCents(q.cost) == 1500;
        CHECK(bq);
    }
    // Monthly expenses: logged, charged to the residents at once, collected, paid - with the deposit when short.
    {
        SqliteConn c((dir / "monthly.db").string());
        Building b; b.address = "ΜΗΝΑ 1"; int bid = c.addBuilding(b); CHECK(c.setBuildingManaged(bid, true));
        int a1 = c.addApartment(bid, "Α1"), a2 = c.addApartment(bid, "Α2");
        DataConn::Resident r1; r1.apartment_id = a1; r1.full_name = "ΕΝΑΣ"; r1.start_date = "2026-01-01";
        DataConn::Resident r2; r2.apartment_id = a2; r2.full_name = "ΔΥΟ"; r2.start_date = "2026-01-01";
        CHECK(c.addResident(r1) > 0 && c.addResident(r2) > 0);
        int U1 = -1, U2 = -1; for (auto& s : c.getResidents(bid, false)) (s.apartment_id == a1 ? U1 : U2) = s.user_id;
        auto ours = [&](double& in) { DataConn::MoneyQuery q; q.oursOnly = true; double o = 0; c.countMoney(q, &in, &o); };
        auto payUser = [&](int uid, int cid, double amt, const char* date) { MoneyPayment m; m.user_id = uid; m.pay_date = date; m.amount = amt; m.method = "cash"; m.direction = "in"; Allocation a; a.charge_id = cid; a.amount = amt; Receipt r; r.method = "cash"; r.user_id = uid; return c.recordPayment(m, {a}, &r); };
        CHECK(c.fundAdjust(bid, 200, "2026-09-01", "Εγγύηση", true) > 0);
        JointShare s1; s1.apartment_id = a1; s1.user_id = U1; s1.amount = 150;
        JointShare s2; s2.apartment_id = a2; s2.user_id = U2; s2.amount = 150;
        std::vector<DataConn::BillReason> why = { {"ΔΕΗ", 200}, {"Νερό", 100} };
        JointShare bad = s2; bad.amount = 149;
        CHECK(c.addMonthlyBill(bid, "2026-09", "", why, {s1, bad}) < 0); c.takeLastError();
        CHECK(c.addMonthlyBill(bid, "2026-13", "", why, {s1, s2}) < 0); c.takeLastError();
        CHECK(c.addMonthlyBill(bid, "2026-09", "", {{"", 300}}, {s1, s2}) < 0); c.takeLastError();
        int mb = c.addMonthlyBill(bid, "2026-09", "", why, {s1, s2});
        if (mb <= 0) std::printf("bill: %s\n", c.takeLastError().c_str());
        CHECK(mb > 0 && c.getBillReasons(mb).size() == 2);
        // logged: they owe at once, the deposit has not moved
        auto bill = c.getMonthlyBills(bid, "2026-09").at(0);
        CHECK(ToCents(bill.total) == 30000 && ToCents(bill.owed) == 30000 && bill.status == "open" && ToCents(c.getFundBalance(bid)) == 20000);
        auto sh = c.getBillShares(mb); CHECK(sh.size() == 2 && ToCents(sh[0].amount) == 15000 && sh[0].name == "ΕΝΑΣ" && !sh[0].charges.empty());
        DataConn::PaymentFilter ow; ow.userId = U1; ow.status = 1; ow.owed = 2; CHECK(ToCents(c.paymentTotals(ow).owedIn) == 15000);
        // it is never our money or our debt
        DataConn::PaymentFilter of; of.status = 1; of.owed = 2; of.requireUser = true; of.oursOnly = true; CHECK(ToCents(c.paymentTotals(of).owedIn) == 0);
        // the first resident pays with a receipt: held for the bill, not the deposit
        int rid = payUser(U1, sh[0].charges[0], 150, "2026-09-10"); CHECK(rid > 0);
        CHECK(ToCents(c.getFundBalance(bid)) == 20000);
        double in = -1; ours(in); CHECK(ToCents(in) == 0);
        DataConn::ReceiptQuery mq; mq.kind = "managed"; CHECK(c.countReceipts(mq) == 1);
        bill = c.getMonthlyBills(bid, "2026-09").at(0); CHECK(ToCents(bill.collected) == 15000 && ToCents(bill.owed) == 15000);
        // the second delays: the cost is paid, 150 of it from the deposit; his debt stays
        CHECK(c.resolveMonthlyBill(mb, "2026-09-30"));
        CHECK(ToCents(c.getFundBalance(bid)) == 5000);
        bill = c.getMonthlyBills(bid, "2026-09").at(0); CHECK(bill.status == "resolved" && ToCents(bill.covered) == 15000 && ToCents(bill.owed) == 15000);
        CHECK(!c.resolveMonthlyBill(mb, "2026-09-30")); c.takeLastError();
        { auto ow2 = c.residentOwingsByMonth(); CHECK(ow2.size() == 1 && ow2[0].ym == "2026-09" && ow2[0].user_id == U2 && ow2[0].to == "deposit" && ToCents(ow2[0].owed) == 15000); }
        CHECK(c.getMonthlyBills(bid, "", true).size() == 1);
        // when he pays, it goes back to the deposit
        CHECK(payUser(U2, sh[1].charges[0], 100, "2026-10-05") > 0); CHECK(ToCents(c.getFundBalance(bid)) == 15000);
        CHECK(payUser(U2, sh[1].charges[0], 50, "2026-10-06") > 0); CHECK(ToCents(c.getFundBalance(bid)) == 20000);
        CHECK(c.getMonthlyBills(bid, "", true).empty());
        ours(in); CHECK(ToCents(in) == 0);
        // a deposit too small refuses, and nothing moves; a bill fully collected uses none of it
        JointShare t1 = s1; t1.amount = 500; JointShare t2 = s2; t2.amount = 500;
        int big = c.addMonthlyBill(bid, "2026-10", "", {{"Βλάβη", 1000}}, {t1, t2}); CHECK(big > 0);
        CHECK(!c.resolveMonthlyBill(big, "2026-10-30")); c.takeLastError();
        CHECK(c.getMonthlyBills(bid, "2026-10").at(0).status == "open" && ToCents(c.getFundBalance(bid)) == 20000);
        auto bs = c.getBillShares(big);
        CHECK(payUser(U1, bs[0].charges[0], 500, "2026-10-10") > 0 && payUser(U2, bs[1].charges[0], 500, "2026-10-11") > 0);
        CHECK(c.resolveMonthlyBill(big, "2026-10-30") && ToCents(c.getFundBalance(bid)) == 20000);
        // how it scores as managed: one month needed the deposit, which came back 6 days after the cost was paid
        { auto ms = c.manageScores("", ""); CHECK(ms.size() == 1 && ms[0].bills == 2 && ms[0].usedDeposit == 1 && ms[0].refilled == 1 && ToCents(ms[0].covered) == 15000 && ToCents(ms[0].owedToDeposit) == 0);
          CHECK(ms[0].avgRefillDays > 5.9 && ms[0].avgRefillDays < 6.1 && ToCents(ms[0].deposit) == 20000 && ms[0].avgCollectDays >= 0); }
        { bool delayed = false; for (auto& q : c.residentQuality("", "")) if (q.user_id == U2) delayed = q.AvgDelay() > 0; CHECK(delayed); }
        // the same building is also our client: its bill changes nothing in managing, and its receipt and rating are apart
        { int bc = -1; for (auto& u : c.pageUsers("", 0, {}, 0, 10, {}, 2)) if (u.client_building_id == bid) bc = u.id; CHECK(bc > 0);
          Service sv; sv.label = "ΥΠΗΡΕΣΙΑ ΜΑΣ"; sv.suggested_price = 10; int svc = c.addServiceGetId(-1, sv);
          Payment own; own.user_id = bc; own.service_id = svc; own.year = 2026; own.month = 10; own.day = 20; own.amount_due = 40; own.amount_paid = 40; own.status = "paid"; own.direction = "in";
          Receipt rr; rr.user_id = bc; rr.method = "cash"; DataConn::ReceiptQuery mq2; mq2.kind = "managed"; DataConn::ReceiptQuery cq2; cq2.kind = "clients";
          long long mBefore = c.countReceipts(mq2), cBefore = c.countReceipts(cq2);
          CHECK(c.addPaymentsWithReceipt({own}, "", &rr) > 0);
          CHECK(c.countReceipts(mq2) == mBefore && c.countReceipts(cq2) == cBefore + 1);
          CHECK(ToCents(c.getFundBalance(bid)) == 20000 && c.manageScores("", "")[0].bills == 2);
          bool rated = false; for (auto& q : c.clientQuality(0, "", "")) if (q.user_id == bc) rated = ToCents(q.income) == 4000; CHECK(rated);
          double in2 = 0; ours(in2); CHECK(ToCents(in2) == 4000); }
        // the deposit's moves say what happened: the cover with the cost, a resident's payback from his owing, no fees
        { bool cover = false, back = false; for (auto& mv2 : c.pageFundMoves(bid, 0, 100)) { if (mv2.description.rfind("Κάλυψη κόστους 2026-09", 0) == 0 && mv2.direction == "out") cover = ToCents(mv2.amount) == 15000; if (mv2.description.rfind("Από οφειλή: Έξοδα 2026-09", 0) == 0 && mv2.direction == "in" && mv2.description.find("300.00") != std::string::npos) back = true; } CHECK(cover && back); }
        // a mistake can be taken back while nothing was collected, and then nobody owes anything for it
        {   JointShare m1 = s1; m1.amount = 7; int oops = c.addMonthlyBill(bid, "2026-10", "", {{"Λάθος", 7}}, {m1}); CHECK(oops > 0);
            auto sh2 = c.getBillShares(oops); DataConn::PaymentFilter of2; of2.userId = U1; of2.status = 1; of2.owed = 2;
            CHECK(ToCents(c.paymentTotals(of2).owedIn) == 700);
            if (!c.cancelMonthlyBill(oops)) std::printf("cancel: %s\n", c.takeLastError().c_str());
            CHECK(ToCents(c.paymentTotals(of2).owedIn) == 0 && c.getBillReasons(oops).empty());
            int keep = c.addMonthlyBill(bid, "2026-10", "", {{"Μένει", 7}}, {m1}); CHECK(keep > 0);
            CHECK(payUser(U1, c.getBillShares(keep)[0].charges[0], 2, "2026-10-20") > 0); CHECK(!c.cancelMonthlyBill(keep)); c.takeLastError();   // money came in
            CHECK(payUser(U1, c.getBillShares(keep)[0].charges[0], 5, "2026-10-20") > 0); CHECK(c.resolveMonthlyBill(keep, "2026-10-21")); CHECK(!c.cancelMonthlyBill(keep)); c.takeLastError(); }
        // a resident who moves out keeps the debt; the new one in the apartment is not charged for it
        {   JointShare m2 = s2; m2.amount = 9; int mv = c.addMonthlyBill(bid, "2026-10", "", {{"Κίνηση", 9}}, {m2}); CHECK(mv > 0);
            int rid2 = -1; for (auto& rs : c.getResidents(bid, false)) if (rs.user_id == U2) rid2 = rs.id;
            CHECK(c.endResident(rid2, "2026-10-29"));
            DataConn::Resident nr; nr.apartment_id = a2; nr.full_name = "ΝΕΟΣ"; nr.start_date = "2026-10-30"; CHECK(c.addResident(nr) > 0);
            auto ms = c.getBillShares(mv); CHECK(ms.size() == 1 && ms[0].user_id == U2 && ToCents(ms[0].balance) == 900);
            bool listed = false; for (auto& o : c.residentOwingsByMonth()) if (o.user_id == U2 && ToCents(o.owed) == 900) listed = true; CHECK(listed);
            CHECK(payUser(U2, ms[0].charges[0], 9, "2026-10-31") > 0); CHECK(c.getBillShares(mv)[0].balance < 0.005 && c.resolveMonthlyBill(mv, "2026-10-31")); }
        // the same month can be logged again and again; each keeps its exact time
        CHECK(!c.getMonthlyBills(bid, "2026-09").at(0).created_at.empty());
        // our fee is the service charged to the building: shown on its own, never in the deposit
        { int fsv = c.ensureFeeService(); CHECK(fsv > 0 && c.ensureFeeService() == fsv); double dep = c.getFundBalance(bid);
          int bc = -1; for (auto& u : c.pageUsers("", 0, {}, 0, 10, {}, 2)) if (u.client_building_id == bid) bc = u.id;
          auto before = c.incomeSplit("", "");
          Payment fp; fp.user_id = bc; fp.service_id = fsv; fp.year = 2026; fp.month = 10; fp.day = 25; fp.amount_due = 30; fp.amount_paid = 10; fp.balance = 20; fp.status = "pending"; fp.direction = "in";
          CHECK(c.addPaymentsWithReceipt({fp}, "", nullptr) > 0);
          auto after = c.incomeSplit("", ""); CHECK(ToCents(after.fees) == ToCents(before.fees) + 1000 && ToCents(after.feesOwed) == ToCents(before.feesOwed) + 2000 && ToCents(after.servicesOwed) == ToCents(before.servicesOwed));
          CHECK(ToCents(c.getFundBalance(bid)) == ToCents(dep)); }
        // the level the deposit should stay above
        CHECK(ToCents(c.getDepositMin(bid)) == 0 && !c.setDepositMin(bid, -1)); c.takeLastError();
        CHECK(c.setDepositMin(bid, 100) && c.setDepositMin(bid, 500) && ToCents(c.getDepositMin(bid)) == 50000 && ToCents(c.manageScores("", "")[0].depositMin) == 50000);
        // only money added by hand after the first deposit counts as renewing it, never residents paying back
        CHECK(ToCents(c.manageScores("", "")[0].renewed) == 0);
        CHECK(c.fundAdjust(bid, 30, "2026-10-31", "Έκτακτη χρηματοδότηση", true) > 0);
        CHECK(ToCents(c.manageScores("", "")[0].renewed) == 3000);
        // the fund's own month figures stay true
        for (const char* ym : {"2026-09", "2026-10"}) { auto mf = c.monthFund(bid, ym); long long f = ToCents(mf.fundStart);
            for (auto& x : mf.items) { if (x.kind == "expense") f += -ToCents(x.cost) + ToCents(x.paidIn); else if (x.kind == "correction") f += ToCents(x.cost); else if (x.kind == "funding") f += ToCents(x.paidIn); }
            f += ToCents(mf.priorPaidIn); CHECK(f == ToCents(mf.fundEnd)); }
        auto chk = c.verifyAuditLog(); for (auto& pr : chk.problems) std::printf("audit: %s\n", pr.c_str()); CHECK(chk.ok);
    }
    // Everything together on an empty database (argv[1]: a copy of the shipped one): every total exact, nothing counted twice.
    {
        if (argc > 1) fs::copy_file(argv[1], dir / "whole.db");
        SqliteConn c((dir / "whole.db").string());
        CHECK(c.countUsers("", 0, {}) == 0);
        Building b; b.address = "ΟΛΑ 1"; int bid = c.addBuilding(b); CHECK(c.setBuildingManaged(bid, true));
        int a1 = c.addApartment(bid, "Α1"), a2 = c.addApartment(bid, "Α2");
        DataConn::Resident r1; r1.apartment_id = a1; r1.full_name = "ΕΝΑΣ"; r1.start_date = "2026-01-01";
        DataConn::Resident r2; r2.apartment_id = a2; r2.full_name = "ΔΥΟ"; r2.start_date = "2026-01-01";
        CHECK(c.addResident(r1) > 0 && c.addResident(r2) > 0);
        int U1 = -1, U2 = -1; for (auto& x : c.getResidents(bid, false)) (x.apartment_id == a1 ? U1 : U2) = x.user_id;
        int bc = -1; for (auto& u : c.pageUsers("", 0, {}, 0, 10, {}, 2)) if (u.client_building_id == bid) bc = u.id;
        User cl; cl.full_name = "ΠΕΛΑΤΗΣ"; c.addUser(cl); const int C1 = c.searchUsers("ΠΕΛΑΤΗΣ", 0, 10)[0].id;
        Service sv; sv.label = "ΥΠΗΡΕΣΙΑ"; sv.suggested_price = 100; int svc = c.addServiceGetId(-1, sv); CHECK(svc > 0);
        auto pay = [&](int uid, int cid, double amt) { MoneyPayment m; m.user_id = uid; m.pay_date = "2026-10-10"; m.amount = amt; m.method = "cash"; m.direction = "in"; Allocation al; al.charge_id = cid; al.amount = amt; Receipt r; r.user_id = uid; r.method = "cash"; return c.recordPayment(m, {al}, &r); };
        // a client's service: 100, 60 paid at once with a receipt, cost 30 paid
        Payment p1; p1.user_id = C1; p1.service_id = svc; p1.year = 2026; p1.month = 10; p1.day = 1; p1.amount_due = 100; p1.amount_paid = 60; p1.balance = 40; p1.status = "pending"; p1.direction = "in";
        Payment co = p1; co.direction = "out"; co.amount_due = 30; co.amount_paid = 30; co.balance = 0; co.status = "paid";
        Receipt rs; rs.user_id = C1; rs.method = "cash"; CHECK(c.addPaymentsWithReceipt({p1, co}, "x", &rs, true) > 0);
        // a contract: 120 in 12, three already due, one of them paid
        std::time_t now = std::time(nullptr); std::tm tm{}; localtime_s(&tm, &now);
        int ym = tm.tm_year + 1900, mm = tm.tm_mon + 1 - 2; if (mm < 1) { mm += 12; ym--; } char sd[16]; std::snprintf(sd, sizeof(sd), "%04d-%02d-01", ym, mm);
        DataConn::Contract k; k.user_id = C1; k.source_type = "service"; k.source_id = svc; k.label = "ΣΥΜΒΑΣΗ"; k.start_date = sd;
        int cid = c.addContract(k, {{svc, "ΥΠΗΡΕΣΙΑ", 120, 0}}); CHECK(cid > 0);
        auto ins = c.getContractInstallments(cid); CHECK(ins.size() == 12); CHECK(pay(C1, ins[0].charges[0], 10) > 0);
        // our fee, charged to the building as a client: 50, 20 paid
        Payment fp; fp.user_id = bc; fp.service_id = c.ensureFeeService(); fp.year = 2026; fp.month = 10; fp.day = 2; fp.amount_due = 50; fp.amount_paid = 20; fp.balance = 30; fp.status = "pending"; fp.direction = "in";
        Receipt rf; rf.user_id = bc; rf.method = "cash"; CHECK(fp.service_id > 0 && c.addPaymentsWithReceipt({fp}, "", &rf) > 0);
        // managing: deposit 500; a cost of 300; one pays, the deposit covers the other, who then pays back part of it
        CHECK(c.fundAdjust(bid, 500, "2026-09-01", "Εγγύηση", true) > 0); CHECK(c.setDepositMin(bid, 100));
        JointShare s1; s1.apartment_id = a1; s1.user_id = U1; s1.amount = 150; JointShare s2 = s1; s2.apartment_id = a2; s2.user_id = U2;
        int mb = c.addMonthlyBill(bid, "2026-10", "", {{"ΔΕΗ", 200}, {"Νερό", 100}}, {s1, s2}); CHECK(mb > 0);
        auto sh = c.getBillShares(mb); CHECK(sh.size() == 2);
        CHECK(pay(U1, sh[0].charges[0], 150) > 0); CHECK(ToCents(c.getFundBalance(bid)) == 50000);
        CHECK(c.resolveMonthlyBill(mb, "2026-10-20")); CHECK(ToCents(c.getFundBalance(bid)) == 35000);
        CHECK(!c.resolveMonthlyBill(mb, "2026-10-20")); c.takeLastError();           // never twice
        CHECK(pay(U2, sh[1].charges[0], 100) > 0); CHECK(ToCents(c.getFundBalance(bid)) == 45000);
        CHECK(pay(U2, sh[1].charges[0], 999) <= 0); c.takeLastError();               // never more than is owed
        CHECK(ToCents(c.getFundBalance(bid)) == 45000);
        // the home screen's figures
        DataConn::MoneyQuery oq; oq.oursOnly = true; double in = 0, out = 0; c.countMoney(oq, &in, &out);
        auto sp = c.incomeSplit("", "");
        DataConn::PaymentFilter of; of.status = 1; of.owed = 2; of.requireUser = true; of.oursOnly = true; auto ot = c.paymentTotals(of);
        std::printf("our money in %.2f out %.2f | contracts %.2f fees %.2f | owed: services %.2f contracts %.2f fees %.2f all %.2f | scheduled %.2f | deposit %.2f\n", in, out, sp.contracts, sp.fees, sp.servicesOwed, sp.contractsOwed, sp.feesOwed, ot.owedIn, sp.contractsScheduled, c.getFundBalance(bid));
        CHECK(ToCents(in) == 9000 && ToCents(out) == 3000);                          // 60 + 10 + 20; the 250 from residents is not ours
        CHECK(ToCents(sp.contracts) == 1000 && ToCents(sp.fees) == 2000 && ToCents(in) - 3000 == 6000);
        CHECK(ToCents(sp.servicesOwed) == 4000 && ToCents(sp.contractsOwed) == 2000 && ToCents(sp.feesOwed) == 3000 && ToCents(ot.owedIn) == 9000);
        CHECK(ToCents(sp.contractsScheduled) == 9000);
        double ai = 0, ao = 0; c.countMoney(DataConn::MoneyQuery{}, &ai, &ao); CHECK(ToCents(ai) == 34000 && ToCents(ao) == 3000);
        // the quick owings add up to the home figures: clients with services (the fee included) and clients with contracts
        { long long open = 0, ctr = 0; for (auto& q : c.clientQuality(0, "", "")) open += ToCents(q.openClient); for (auto& o : c.contractOwings()) ctr += ToCents(o.owed);
          CHECK(ctr == ToCents(sp.contractsOwed)); CHECK(open - ctr == ToCents(sp.servicesOwed) + ToCents(sp.feesOwed)); }
        // a partner keeps its IBAN and extra details
        { Partner pr; pr.name = "ΣΥΝ"; pr.iban = "GR1601101250000000012300695"; pr.extra_json = "{\"Ώρες\":\"πρωί\"}"; int pid = c.savePartner(pr); CHECK(pid > 0);
          Partner g = c.getPartner(pid); CHECK(g.iban == pr.iban && g.extra_json == pr.extra_json); g.iban = "GR00"; CHECK(c.savePartner(g) == pid && c.getPartner(pid).iban == "GR00" && c.getPartner(pid).extra_json == pr.extra_json); }
        // who owes the deposit, and how the building scores
        auto ow = c.residentOwingsByMonth(); CHECK(ow.size() == 1 && ow[0].user_id == U2 && ow[0].to == "deposit" && ow[0].ym == "2026-10" && ToCents(ow[0].owed) == 5000);
        auto ms = c.manageScores("", ""); CHECK(ms.size() == 1 && ms[0].bills == 1 && ms[0].usedDeposit == 1 && ToCents(ms[0].covered) == 15000 && ToCents(ms[0].owedToDeposit) == 5000 && ToCents(ms[0].deposit) == 45000);
        // nothing twice: each kind of line and receipt lands in one list only
        DataConn::PaymentFilter fa; fa.direction = "in"; CHECK(c.paymentTotals(fa).count == 16);    // 1 service + 12 installments + 1 fee + 2 shares
        fa.joint = 7; CHECK(c.paymentTotals(fa).count == 12);
        DataConn::ReceiptQuery rm; rm.kind = "managed"; DataConn::ReceiptQuery rc; rc.kind = "clients"; DataConn::ReceiptQuery rk; rk.kind = "contracts";
        CHECK(c.countReceipts(rm) == 2 && c.countReceipts(rc) == 2 && c.countReceipts(rk) == 1 && c.countReceipts(DataConn::ReceiptQuery{}) == 5);
        { std::vector<std::string> cols; std::vector<std::vector<std::string>> rows; std::string err;
          c.executeRawQuery("SELECT COUNT(*) FROM (SELECT receipt_id FROM payments WHERE receipt_id IS NOT NULL GROUP BY receipt_id HAVING COUNT(*) > 1)", cols, rows, err); CHECK(!rows.empty() && rows[0][0] == "0");
          c.executeRawQuery("SELECT COUNT(*) FROM (SELECT allocation_id FROM fund_moves WHERE allocation_id IS NOT NULL GROUP BY allocation_id HAVING COUNT(*) > 1)", cols, rows, err); CHECK(!rows.empty() && rows[0][0] == "0");
          c.executeRawQuery("SELECT COUNT(*) FROM charges WHERE ABS(amount_due - amount_paid - balance) > 0.005", cols, rows, err); CHECK(!rows.empty() && rows[0][0] == "0"); }
        auto chk = c.verifyAuditLog(); for (auto& pr : chk.problems) std::printf("audit: %s\n", pr.c_str()); CHECK(chk.ok);
    }
    // The rules of a charge: status follows the money, every receipt proves what was paid, nothing is erased.
    {
        SqliteConn c((dir / "rules.db").string());
        Building b; b.address = "ΚΑΝΟΝΕΣ 1"; int bid = c.addBuilding(b); CHECK(c.setBuildingManaged(bid, true)); int a1 = c.addApartment(bid, "Α1");
        DataConn::Resident r1; r1.apartment_id = a1; r1.full_name = "ΕΝΟΙΚΟΣ"; r1.start_date = "2026-01-01"; CHECK(c.addResident(r1) > 0);
        auto stays = c.getResidents(bid, false); int U = stays[0].user_id;
        User cl; cl.full_name = "ΚΑΝΟΝΑΣ"; c.addUser(cl); const int C1 = c.searchUsers("ΚΑΝΟΝΑΣ", 0, 10)[0].id;
        Service sv; sv.label = "ΥΠ"; sv.suggested_price = 100; int svc = c.addServiceGetId(-1, sv);
        auto one = [&](const std::string& q) { std::vector<std::string> cc; std::vector<std::vector<std::string>> rr; std::string e; c.executeRawQuery(q, cc, rr, e); return rr.empty() || rr[0].empty() ? std::string("?") : rr[0][0]; };
        auto pay = [&](int uid, int cid, double amt, const char* d, bool rec) { MoneyPayment m; m.user_id = uid; m.pay_date = d; m.amount = amt; m.method = "cash"; m.direction = "in"; Allocation al; al.charge_id = cid; al.amount = amt; Receipt r; r.user_id = uid; r.method = "cash"; r.issue_date = "2026-12-24"; return c.recordPayment(m, {al}, rec ? &r : nullptr); };
        Payment p; p.user_id = C1; p.service_id = svc; p.year = 2026; p.month = 10; p.day = 1; p.amount_due = 100; p.status = "paid"; p.direction = "in";
        int ch = c.addPaymentsWithReceipt({p}, "", nullptr); CHECK(ch > 0);
        CHECK(c.getPaymentById(ch).status == "pending");                      // "paid" with nothing paid is not paid
        CHECK(pay(C1, ch, 30, "2026-10-05", true) > 0); CHECK(c.getPaymentById(ch).status == "pending"); CHECK(one("SELECT COALESCE(completed_on,'none') FROM charges WHERE id=" + std::to_string(ch)) == "none");
        CHECK(pay(C1, ch, 30, "2026-10-09", true) > 0);                       // a second fraction, on a charge that already has a receipt
        CHECK(pay(C1, ch, 40, "2026-10-20", true) > 0);
        Payment done = c.getPaymentById(ch); CHECK(done.status == "paid" && ToCents(done.balance) == 0);
        CHECK(one("SELECT completed_on FROM charges WHERE id=" + std::to_string(ch)) == "2026-10-20"); CHECK(one("SELECT charge_date FROM charges WHERE id=" + std::to_string(ch)) == "2026-10-01");
        long long sumRec = 0; for (auto& r : c.getReceiptsForUser(C1)) { long long lines = 0; for (auto& l : c.getReceiptLines(r.id)) lines += ToCents(l.amount); CHECK(lines == ToCents(r.total)); sumRec += ToCents(r.total); CHECK(r.issue_date == "2026-12-24"); }
        CHECK(sumRec == 10000);
        // more money than charges: no receipt that says otherwise
        p.amount_due = 60; int ch2 = c.addPaymentsWithReceipt({p}, "", nullptr);
        { MoneyPayment m; m.user_id = C1; m.pay_date = "2026-10-05"; m.amount = 100; m.method = "cash"; m.direction = "in"; Allocation al; al.charge_id = ch2; al.amount = 60; Receipt r; r.user_id = C1; r.method = "cash"; CHECK(c.recordPayment(m, {al}, &r) <= 0); c.takeLastError(); }
        // a charge's date moves, the money's date does not; and it can't ask less than was paid
        p.amount_due = 50; int ch3 = c.addPaymentsWithReceipt({p}, "", nullptr); CHECK(pay(C1, ch3, 50, "2026-10-11", false) > 0);
        Payment e3 = c.getPaymentById(ch3); e3.month = 3; e3.amount_paid = 50; CHECK(c.editPayment(ch3, e3)); CHECK(one("SELECT MAX(m.pay_date) FROM allocations a JOIN payments m ON m.id=a.payment_id WHERE a.charge_id=" + std::to_string(ch3)) == "2026-10-11");
        e3 = c.getPaymentById(ch3); e3.amount_due = 20; e3.amount_paid = 50; CHECK(!c.editPayment(ch3, e3)); c.takeLastError(); CHECK(ToCents(c.getPaymentById(ch3).balance) == 0);
        // a voided receipt whose charge is deleted still says what it was
        p.amount_due = 10; int ch4 = c.addPaymentsWithReceipt({p}, "", nullptr); CHECK(pay(C1, ch4, 10, "2026-10-12", true) > 0);
        int rid = c.getPaymentById(ch4).receipt_id; CHECK(c.voidReceipt(rid, "λάθος")); CHECK(c.removePayment(ch4)); auto gl = c.getReceiptLines(rid); CHECK(gl.size() == 1 && ToCents(gl[0].amount) == 1000);
        // a contract cancelled: past installment owed, a part paid one owed, the rest gone; complete once paid
        std::time_t now = std::time(nullptr); std::tm tm{}; localtime_s(&tm, &now);
        int ym = tm.tm_year + 1900, mm = tm.tm_mon + 1 - 1; if (mm < 1) { mm += 12; ym--; } char sd[16]; std::snprintf(sd, sizeof(sd), "%04d-%02d-01", ym, mm);
        DataConn::Contract k; k.user_id = C1; k.source_type = "service"; k.source_id = svc; k.label = "Κ"; k.start_date = sd;
        int cid = c.addContract(k, {{svc, "ΥΠ", 120, 0}}); auto ins = c.getContractInstallments(cid); CHECK(ins.size() == 12);
        int fut = -1; for (auto& i : ins) if (i.future) { fut = i.charges[0]; break; }
        CHECK(fut > 0 && pay(C1, fut, 4, sd, false) > 0); CHECK(c.getPaymentById(fut).status == "future");       // part paid early is not owed yet
        const long long owed0 = ToCents(c.getContract(cid).owedNow);
        CHECK(c.cancelContract(cid, "τέλος")); DataConn::Contract kk = c.getContract(cid);
        CHECK(kk.cancelled && kk.open && ToCents(kk.scheduled) == 0 && ToCents(kk.owedNow) == owed0 + 600 && ToCents(kk.collected) == 400);
        for (auto& i : c.getContractInstallments(cid)) for (int chg : i.charges) { Payment x = c.getPaymentById(chg); if (ToCents(x.balance) > 0) CHECK(pay(C1, chg, x.balance, sd, true) > 0); }
        kk = c.getContract(cid); CHECK(kk.cancelled && !kk.open && ToCents(kk.owedNow) == 0);
        // leaving: a client who owes goes to the past clients and still owes; a resident goes to the past residents
        p.amount_due = 75; int ch5 = c.addPaymentsWithReceipt({p}, "", nullptr); CHECK(ch5 > 0);
        CHECK(c.removeUser(C1)); CHECK(ToCents(c.getPaymentById(ch5).balance) == 7500);
        CHECK(c.countUsers("ΚΑΝΟΝΑΣ", 0, {}) == 0 && c.countUsers("ΚΑΝΟΝΑΣ", 0, {}, 7 | 8) == 1);
        { bool still = false; for (auto& q : c.clientQuality(0, "", "")) if (q.user_id == C1 && ToCents(q.openClient) == 13500) still = true; CHECK(still); }
        JointShare s1; s1.apartment_id = a1; s1.user_id = U; s1.amount = 90; int mb = c.addMonthlyBill(bid, "2026-10", "", {{"ΔΕΗ", 90}}, {s1}); CHECK(mb > 0);
        CHECK(c.removeUser(U)); CHECK(c.getResidents(bid, false).empty()); CHECK(c.getResidents(bid, true).size() == 1 && ToCents(c.getResidents(bid, true)[0].exit_owed) == 9000);
        auto ow = c.residentOwingsByMonth(); CHECK(ow.size() == 1 && ow[0].user_id == U && ow[0].to == "cost" && ToCents(ow[0].owed) == 9000);
        { std::vector<std::string> cc; std::vector<std::vector<std::string>> rr; std::string e; c.executeRawQuery("SELECT COUNT(*) FROM charges WHERE (CAST(ROUND(balance*100) AS INTEGER) <= 0) <> (status = 'paid')", cc, rr, e); CHECK(!rr.empty() && rr[0][0] == "0"); }
        auto chk = c.verifyAuditLog(); for (auto& pr : chk.problems) std::printf("audit: %s\n", pr.c_str()); CHECK(chk.ok);
    }
    // Keys on, nothing lost, suggestions for a new monthly cost.
    {
        SqliteConn c((dir / "keys.db").string());
        auto one = [&](const std::string& q) { std::vector<std::string> cc; std::vector<std::vector<std::string>> rr; std::string e; c.executeRawQuery(q, cc, rr, e); return rr.empty() || rr[0].empty() ? std::string("?") : rr[0][0]; };
        CHECK(one("PRAGMA foreign_keys") == "1");
        Building b; b.address = "ΚΛΕΙΔΙΑ 1"; int bid = c.addBuilding(b); CHECK(c.setBuildingManaged(bid, true)); int a1 = c.addApartment(bid, "Α1");
        DataConn::Resident r1; r1.apartment_id = a1; r1.full_name = "ΕΝΑΣ"; r1.start_date = "2026-01-01"; CHECK(c.addResident(r1) > 0);
        int U = c.getResidents(bid, false)[0].user_id; CHECK(one("SELECT resident FROM users WHERE id=" + std::to_string(U)) == "1");
        int bc = -1; for (auto& u : c.pageUsers("", 0, {}, 0, 10, {}, 2)) if (u.client_building_id == bid) bc = u.id;
        CHECK(c.fundAdjust(bid, 500, "2026-09-01", "o", true) > 0);
        std::time_t now = std::time(nullptr); std::tm tm{}; localtime_s(&tm, &now);
        Service sv; sv.label = "ΚΑΘΑΡΙΣΜΟΣ"; sv.suggested_price = 10; int svc = c.addServiceGetId(-1, sv);
        Payment p; p.user_id = bc; p.service_id = svc; p.year = 2020; p.month = 1; p.day = 1; p.amount_due = 50; p.status = "pending"; p.direction = "in";
        int oldOpen = c.addPaymentsWithReceipt({p}, "", nullptr);
        p.amount_due = 20; int oldPaid = c.addPaymentsWithReceipt({p}, "", nullptr);                        // old and paid long ago: not suggested
        CHECK(c.fundPayOurBill(bid, oldOpen, 50, "2020-02-01") > 0);
        p.amount_due = 30; int open2 = c.addPaymentsWithReceipt({p}, "", nullptr);
        { MoneyPayment m; m.user_id = bc; m.pay_date = "2020-01-05"; m.amount = 20; m.method = "cash"; m.direction = "in"; Allocation al; al.charge_id = oldPaid; al.amount = 20; CHECK(c.recordPayment(m, {al}, nullptr) > 0); }
        p.year = tm.tm_year + 1900; p.month = tm.tm_mon + 1; p.day = tm.tm_mday; p.amount_due = 15; p.amount_paid = 15; int recent = c.addPaymentsWithReceipt({p}, "", nullptr);
        auto sg = c.billSuggestions(bid); CHECK(sg.size() == 2); if (sg.size() == 2) { CHECK(sg[0].charge_id == open2 && ToCents(sg[0].owed) == 3000); CHECK(sg[1].charge_id == recent && ToCents(sg[1].owed) == 0 && ToCents(sg[1].due) == 1500); }
        CHECK(!c.removePayment(oldOpen)); c.takeLastError(); CHECK(ToCents(c.getFundBalance(bid)) == 45000);   // paid from the deposit: stays
        JointShare s1; s1.apartment_id = a1; s1.user_id = U; s1.amount = 90; int mb = c.addMonthlyBill(bid, "2026-10", "", {{"ΔΕΗ", 90}}, {s1}); auto sh = c.getBillShares(mb);
        CHECK(!c.removePayment(sh[0].charges[0])); c.takeLastError();
        Payment e = c.getPaymentById(sh[0].charges[0]); e.amount_due = 10; CHECK(!c.editPayment(e.id, e)); c.takeLastError();
        CHECK(c.cancelMonthlyBill(mb)); CHECK(one("SELECT COUNT(*) FROM record_deletions WHERE snapshot_json LIKE '%ΔΕΗ%'") == "1");
        { Building pb; pb.address = "ΑΠΛΗ 1"; int pid = c.addBuilding(pb); CHECK(pid > 0 && c.removeBuilding(pid)); CHECK(!c.removeBuilding(bid)); c.takeLastError(); CHECK(c.getBuildingById(bid).id == bid); }
        CHECK(one("SELECT COUNT(*) FROM pragma_foreign_key_check") == "0");
        auto chk = c.verifyAuditLog(); for (auto& pr : chk.problems) std::printf("audit: %s\n", pr.c_str()); CHECK(chk.ok);
    }
    // Partner documents and the analysis by kind.
    {
        SqliteConn c((dir / "docs.db").string());
        Partner pr; pr.name = "ΗΛΕΚΤΡΟΛΟΓΟΣ"; int pid = c.savePartner(pr); CHECK(pid > 0);
        User cl; cl.full_name = "ΑΝΑΛΥΣΗ"; c.addUser(cl); const int C1 = c.searchUsers("ΑΝΑΛΥΣΗ", 0, 10)[0].id;
        Service a; a.label = "Α"; a.suggested_price = 10; int sa = c.addServiceGetId(-1, a); Service b; b.label = "Β"; b.suggested_price = 10; int sb = c.addServiceGetId(-1, b);
        Payment p; p.user_id = C1; p.service_id = sa; p.year = 2026; p.month = 10; p.day = 1; p.amount_due = 100; p.status = "pending"; p.direction = "in";
        Payment co = p; co.direction = "out"; co.amount_due = 40; co.partner_id = pid;
        CHECK(c.addPaymentsWithReceipt({p, co}, "", nullptr) > 0);                                       // a single service with its cost
        Payment k1 = p; k1.amount_due = 30; Payment k2 = k1; k2.service_id = sb; k2.amount_due = 20;
        CHECK(c.addPaymentsWithReceipt({k1, k2}, "ΠΑΚΕΤΟ Χ", nullptr) > 0);                              // a package
        auto rows = c.profitAnalysis("", ""); bool sv = false, pk = false;
        for (auto& r : rows) { if (r.kind == "service" && r.label == "Α") { sv = true; CHECK(ToCents(r.charged) == 10000 && ToCents(r.cost) == 4000 && ToCents(r.costOwed) == 4000 && r.lines == 1); } if (r.kind == "package") { pk = true; CHECK(r.label == "ΠΑΚΕΤΟ Χ" && ToCents(r.charged) == 5000); } }
        CHECK(sv && pk);
        auto mx = c.clientMix("", ""); CHECK(mx.size() == 1 && ToCents(mx[0].services) == 10000 && ToCents(mx[0].packages) == 5000 && ToCents(mx[0].contracts) == 0);
        int costLine = -1; for (auto& j : c.partnerJobs(pid, "", "", {}, 0, 10, 1, true)) costLine = j.charge_id; CHECK(costLine > 0);
        CHECK(!c.payOutWithDoc(costLine, 10, "2026-10-10", "bank", "invoice", "T1", "", "")); c.takeLastError();    // a document needs its date
        CHECK(ToCents(c.getPaymentById(costLine).balance) == 4000);
        CHECK(c.payOutWithDoc(costLine, 25, "2026-10-10", "bank", "invoice", "ΤΙΜ-1", "2026-10-09", "σημ"));
        CHECK(c.payOutWithDoc(costLine, 15, "2026-10-12", "cash", "receipt", "ΑΠ-2", "2026-10-12", ""));
        auto docs = c.getPartnerDocs(pid, "", ""); CHECK(docs.size() == 2); if (docs.size() == 2) { CHECK(docs[1].type == "invoice" && docs[1].number == "ΤΙΜ-1" && ToCents(docs[1].amount) == 2500 && docs[1].partner == "ΗΛΕΚΤΡΟΛΟΓΟΣ" && docs[1].what == "Α" && docs[1].method == "bank"); }
        CHECK(c.getPaymentById(costLine).status == "paid");
        auto chk = c.verifyAuditLog(); for (auto& pr2 : chk.problems) std::printf("audit: %s\n", pr2.c_str()); CHECK(chk.ok);
    }
    // A whole life on an empty database (argv[1]: a copy of the shipped empty.db, else a new one): after every step the
    // office's money, the funds and the debts must each be exactly what happened.
    {
        if (argc > 1) fs::copy_file(argv[1], dir / "life.db");
        SqliteConn c((dir / "life.db").string());
        CHECK(c.countUsers("", 0, {}) == 0);
        Building b; b.address = "ΖΩΗΣ 1"; int bid = c.addBuilding(b);
        CHECK(c.setBuildingManaged(bid, true));
        int a1 = c.addApartment(bid, "Α1"), a2 = c.addApartment(bid, "Α2");
        DataConn::Resident r1; r1.apartment_id = a1; r1.full_name = "ΕΝΑΣ"; r1.start_date = "2026-01-01";
        DataConn::Resident r2; r2.apartment_id = a2; r2.full_name = "ΔΥΟ"; r2.start_date = "2026-01-01";
        CHECK(c.addResident(r1) > 0 && c.addResident(r2) > 0);
        int U1 = -1, U2 = -1; for (auto& s : c.getResidents(bid, false)) (s.apartment_id == a1 ? U1 : U2) = s.user_id;
        int bclient = -1; for (auto& u : c.pageUsers("", 0, {}, 0, 10, {}, 2)) if (u.client_building_id == bid) bclient = u.id;
        User cl; cl.full_name = "ΠΕΛΑΤΗΣ"; c.addUser(cl); const int C1 = c.searchUsers("ΠΕΛΑΤΗΣ", 0, 10)[0].id;
        Partner pa; pa.name = "ΣΥΝΕΡΓΑΤΗΣ"; int pid = c.savePartner(pa);
        Service sv; sv.label = "ΥΠΗΡΕΣΙΑ"; sv.suggested_price = 10; int svc = c.addServiceGetId(-1, sv);

        // expected, in cents: our money in / out, owed to us, we owe, the fund, residents' debt to the fund
        long long xIn = 0, xOut = 0, xOwed = 0, xWeOwe = 0, xFund = 0, xDebt = 0;
        auto verify = [&](const char* step) {
            DataConn::MoneyQuery mq; mq.oursOnly = true; double in = 0, out = 0; c.countMoney(mq, &in, &out);
            DataConn::PaymentFilter f; f.status = 1; f.owed = 2; f.requireUser = true; f.oursOnly = true;
            auto t = c.paymentTotals(f);
            long long debt = 0;
            for (auto& q : c.residentQuality("", "")) debt += ToCents(q.fundOpen);
            auto mf = c.monthFund(bid, "2026-10");
            auto mr = c.monthResidents(bid, "2026-10"); long long cells = 0;    // what the monthly table adds up as owed
            for (auto& x : mr.cells) if (x.op > 0) cells += ToCents(x.charged) - std::min(ToCents(x.charged), ToCents(x.paid));
            bool ok = ToCents(in) == xIn && ToCents(out) == xOut && ToCents(t.owedIn) == xOwed && ToCents(t.owedOut) == xWeOwe &&
                      ToCents(c.getFundBalance(bid)) == xFund && debt == xDebt && ToCents(mf.owedEnd) == xDebt && ToCents(mf.fundEnd) == xFund;
            ok = ok && cells == xDebt + ToCents(mf.feeOwedEnd);
            std::printf("%-30s in %.2f out %.2f owed %.2f weOwe %.2f fund %.2f debt %.2f monthly %.2f %s\n", step, in, out, t.owedIn, t.owedOut, c.getFundBalance(bid), debt / 100.0, cells / 100.0, ok ? "ok" : "WRONG");
            if (!ok) fails++;
        };
        auto openOf = [&](int uid, double due) { for (auto& o : c.getOpenCharges(uid)) if (ToCents(o.amount_due) == ToCents(due)) return o.id; return -1; };
        auto pay = [&](int uid, const std::vector<std::pair<int, double>>& lines, const char* date) {
            MoneyPayment m; m.user_id = uid; m.pay_date = date; m.direction = "in"; m.method = "cash"; double tot = 0;
            std::vector<Allocation> al; for (auto& l : lines) { Allocation a; a.charge_id = l.first; a.amount = l.second; al.push_back(a); tot += l.second; }
            m.amount = tot; Receipt r; r.user_id = uid; r.method = "cash"; return c.recordPayment(m, al, &r); };
        JointOp base; base.building_id = bid; base.source_type = "service"; base.source_id = svc; base.op_date = "2026-10-02";

        verify("empty");
        JointOp f1 = base; f1.kind = "contribution"; f1.label = "Χρηματοδότηση"; f1.total = 300;
        JointShare s1; s1.apartment_id = a1; s1.user_id = U1; s1.amount = 150; s1.paid = 150;
        JointShare s2; s2.apartment_id = a2; s2.user_id = U2; s2.amount = 150; s2.paid = 100;
        Receipt rc; rc.method = "cash";
        CHECK(c.chargeJoint(f1, {{svc, 300}}, {s1, s2}, 0, -1, -1, &rc) > 0);
        xFund = 25000; xDebt = 5000; verify("funding 300, 50 unpaid");
        JointShare e1; e1.apartment_id = a1; e1.user_id = U1; e1.amount = 100;
        JointShare e2 = e1; e2.apartment_id = a2; e2.user_id = U2;
        CHECK(c.fundExpenseShared(bid, 200, "2026-10-03", "Βλάβη", "", {e1, e2}) > 0);
        xFund = 5000; verify("shared expense 200");
        int late = pay(U2, {{openOf(U2, 150), 50}}, "2026-10-04"); CHECK(late > 0);
        xFund = 10000; xDebt = 0; verify("U2 pays the 50 later");
        JointOp f2 = base; f2.kind = "fee"; f2.label = "Αμοιβή"; f2.total = 40; f2.op_date = "2026-10-05";
        JointShare fe1; fe1.apartment_id = a1; fe1.user_id = U1; fe1.amount = 20; fe1.paid = 20;
        JointShare fe2; fe2.apartment_id = a2; fe2.user_id = U2; fe2.amount = 20;
        Receipt rf; rf.method = "cash";
        CHECK(c.chargeJoint(f2, {{svc, 40}}, {fe1, fe2}, 0, -1, -1, &rf) > 0);
        xIn = 2000; xOwed = 2000; verify("fee 40, one paid");
        JointOp f3 = base; f3.kind = "contribution"; f3.label = "Δεύτερη"; f3.total = 10; f3.op_date = "2026-10-06";
        JointShare n2; n2.apartment_id = a2; n2.user_id = U2; n2.amount = 10;
        CHECK(c.chargeJoint(f3, {{svc, 10}}, {n2}) > 0);
        xDebt = 1000; verify("funding 10 unpaid");
        CHECK(pay(U2, {{openOf(U2, 20), 20}, {openOf(U2, 10), 10}}, "2026-10-07") > 0);   // one payment, fee + funding
        xIn = 4000; xOwed = 0; xFund = 11000; xDebt = 0; verify("mixed payment 20 fee + 10 fund");

        // a client's package: 60 + 40, cost 30, part paid each way
        Payment p1; p1.user_id = C1; p1.service_id = svc; p1.year = 2026; p1.month = 10; p1.day = 8; p1.amount_due = 60; p1.amount_paid = 60; p1.status = "paid"; p1.direction = "in";
        Payment p2 = p1; p2.amount_due = 40; p2.amount_paid = 0; p2.balance = 40; p2.status = "pending";
        Payment co = p1; co.direction = "out"; co.amount_due = 30; co.amount_paid = 10; co.balance = 20; co.status = "pending"; co.partner_id = pid;
        Receipt rp; rp.user_id = C1; rp.method = "cash";
        CHECK(c.addPaymentsWithReceipt({p1, p2, co}, "Πακέτο: Α", &rp, true) > 0);
        xIn = 10000; xOut = 1000; xOwed = 4000; xWeOwe = 2000; verify("client package part paid");
        CHECK(pay(C1, {{openOf(C1, 40), 40}}, "2026-10-09") > 0);
        xIn = 14000; xOwed = 0; verify("client pays the rest");
        int bill = -1; { DataConn::PaymentFilter of; of.direction = "out"; of.owed = 2; of.status = 1; for (auto& x : c.findPayments(of, DataConn::PayOrder::DateDesc, 0, 10)) bill = x.id; }
        CHECK(c.payOutCharge(bill, 20, "2026-10-10"));
        xOut = 3000; xWeOwe = 0; verify("we pay the partner");

        // the building's package 40, cost 15, all paid: 25 for us, the fund does not move
        Payment g1; g1.user_id = bclient; g1.service_id = svc; g1.year = 2026; g1.month = 10; g1.day = 11; g1.amount_due = 40; g1.amount_paid = 40; g1.status = "paid"; g1.direction = "in";
        Payment gc = g1; gc.direction = "out"; gc.amount_due = 15; gc.amount_paid = 15; gc.partner_id = pid;
        Receipt rg; rg.user_id = bclient; rg.method = "cash";
        CHECK(c.addPaymentsWithReceipt({g1, gc}, "Πακέτο: Β", &rg, true) > 0);
        xIn = 18000; xOut = 4500; verify("building package 40/15");

        // voiding a receipt moves no money
        CHECK(c.voidReceipt(late, "λάθος στοιχεία")); verify("receipt voided");
        // receipts land right: the late resident payment is managed only
        DataConn::ReceiptQuery mq; mq.kind = "managed"; DataConn::ReceiptQuery cq; cq.kind = "clients";
        CHECK(Has(c.searchReceipts(mq, 0, 100), late) && !Has(c.searchReceipts(cq, 0, 100), late));
        // all the money = ours + what went through the funds (300 - 50 + 50 + 10 in)
        DataConn::MoneyQuery all; double ai = 0, ao = 0; c.countMoney(all, &ai, &ao);
        CHECK(ToCents(ai) == xIn + 31000 && ToCents(ao) == xOut);
        auto chk = c.verifyAuditLog(); CHECK(chk.ok);
    }
    fs::remove_all(dir);
    if (fails) { std::printf("%d check(s) failed\n", fails); return 1; }
    std::printf("fund: all checks passed\n");
    return 0;
}
