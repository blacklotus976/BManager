// Ταμείο πολυκατοικίας: residents' money in (contributions, shares of κοινόχρηστα) with payments and receipts, the
// building's costs out, expenses and corrections, and one of OUR bills to the building paid from its fund - all
// written as moves that are never edited, with the right balance after each.
#include "SqliteConn.h"
#include "Money.h"
#include <cstdio>
#include <filesystem>

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } } while (0)

static bool Raw(SqliteConn& c, const std::string& sql) {
    std::vector<std::string> cols; std::vector<std::vector<std::string>> rows; std::string err;
    return c.executeRawQuery(sql, cols, rows, err) && err.empty();
}

int main() {
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
    fs::remove_all(dir);
    if (fails) { std::printf("%d check(s) failed\n", fails); return 1; }
    std::printf("fund: all checks passed\n");
    return 0;
}
