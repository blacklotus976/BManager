// Receipts + RF: runs SqliteConn against a throw-away database file.
#include "SqliteConn.h"
#include "Rf.h"
#include "Money.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } } while (0)

static int lastPaymentId(SqliteConn& db) {
    std::vector<std::string> c; std::vector<std::vector<std::string>> rows; std::string err;
    db.executeRawQuery("SELECT MAX(id) FROM charges", c, rows, err);
    return std::atoi(rows[0][0].c_str());
}

static int addPay(SqliteConn& db, int user, int svc, double due, double paid, const char* status = "paid", const char* dir = "in") {
    Payment p; p.user_id = user; p.service_id = svc; p.year = 2026; p.month = 10; p.day = 1;
    p.amount_due = due; p.amount_paid = paid; p.status = status; p.direction = dir;
    db.addPayment(p);
    std::vector<std::string> c; std::vector<std::vector<std::string>> rows; std::string err;
    db.executeRawQuery("SELECT MAX(id) FROM charges", c, rows, err);
    return std::atoi(rows[0][0].c_str());
}

int main() {
    // RF codes (ISO 11649 example RF18 5390 0754 7034)
    CHECK(IsValidRfCode("RF18539007547034"));
    CHECK(IsValidRfCode("rf18 5390 0754 7034"));
    CHECK(!IsValidRfCode("RF19539007547034"));
    CHECK(MakeRfCode("539007547034") == "RF18539007547034");
    CHECK(IsValidRfCode(MakeRfCode("A0000001")));
    CHECK(MakeRfCode("") == "" && MakeRfCode("ABCDEFGHIJKLMNOPQRSTUV") == "");

    // own folder: the database, its WAL files and the audit checkpoints all go away together
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "bmanager_receipts_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::string path = (dir / "data.db").string();
    {
        SqliteConn db(path);
        User u; u.full_name = "ΤΕΣΤ ΠΕΛΑΤΗΣ"; db.addUser(u);
        Service s; s.label = "ΠΑΚΕΤΟ BASIC"; s.suggested_price = 15; int svc = db.addServiceGetId(1, s);

        // a package charge: three payments created together (one batch)
        std::vector<Payment> pkg(3);
        for (auto& p : pkg) { p.user_id = 1; p.service_id = svc; p.year = 2026; p.month = 10; p.day = 1;
                              p.amount_due = 15; p.amount_paid = 15; p.status = "paid"; p.direction = "in"; }
        pkg[2].amount_paid = 10.10;
        int batch = db.addPaymentBatch(pkg, "ΠΑΚΕΤΟ BASIC");
        CHECK(batch > 0);
        int p3 = lastPaymentId(db), p2 = p3 - 1, p1 = p3 - 2;
        CHECK(db.getPaymentById(p1).batch_id == batch && db.getPaymentById(p3).batch_id == batch);
        int fut = addPay(db, 1, svc, 15, 0, "future"), out = addPay(db, 1, svc, 50, 50, "paid", "out");
        int lone1 = addPay(db, 1, svc, 20, 20), lone2 = addPay(db, 1, svc, 20, 20);

        Receipt r; r.user_id = 1; r.method = "ebanking";
        int rid = db.issueReceipt(r, {p1, p2, p3});
        CHECK(rid > 0);
        Receipt got = db.getReceiptById(rid);
        CHECK(got.number == 1 && got.series == "A");
        CHECK(ToCents(got.total) == 4010);                      // 15 + 15 + 10.10
        CHECK(IsValidRfCode(got.rf_code));
        CHECK(db.getReceiptPaymentIds(rid).size() == 3);
        CHECK(db.getReceiptIdForPayment(p2) == rid);

        // a payment can't be on two issued receipts; future / outgoing payments can't be receipted
        CHECK(db.issueReceipt(r, {p1}) == -1); db.takeLastError();
        CHECK(db.issueReceipt(r, {fut}) == -1); db.takeLastError();
        CHECK(db.issueReceipt(r, {out}) == -1); db.takeLastError();
        CHECK(db.issueReceipt(r, {}) == -1); db.takeLastError();
        // payments created separately can't share a receipt (one each), nor mix with a batch
        CHECK(db.issueReceipt(r, {lone1, lone2}) == -1); db.takeLastError();
        CHECK(db.issueReceipt(r, {lone1}) > 0);
        CHECK(db.issueReceipt(r, {lone2}) > 0);
        Receipt badRf = r; badRf.rf_code = "RF00123";
        CHECK(db.issueReceipt(badRf, {fut}) == -1); db.takeLastError();

        // receipted money is locked, from the API and from raw SQL
        Payment ed = db.getPaymentById(p1); ed.amount_paid = 1;
        CHECK(!db.editPayment(p1, ed));
        CHECK(!db.takeLastError().empty());
        CHECK(!db.removePayment(p2)); db.takeLastError();
        std::vector<std::string> c; std::vector<std::vector<std::string>> rows; std::string err;
        CHECK(!db.executeRawQuery("UPDATE payments SET amount_paid = 0 WHERE id = " + std::to_string(p1), c, rows, err));
        CHECK(!db.executeRawQuery("DELETE FROM audit_log", c, rows, err));
        CHECK(!db.executeRawQuery("DROP TRIGGER audit_payments_upd", c, rows, err));
        CHECK(!db.executeRawQuery("DELETE FROM receipts", c, rows, err));
        CHECK(!db.executeRawQuery("UPDATE receipts SET total = 1", c, rows, err));
        CHECK(!db.executeRawQuery("UPDATE receipts SET status = 'void'", c, rows, err));   // no reason
        // notes on a payment stay editable
        ed = db.getPaymentById(p1); ed.notes = "σημείωση";
        CHECK(db.editPayment(p1, ed));

        // payment + receipt recorded together; a bad receipt leaves NOTHING behind
        {
            int before = lastPaymentId(db);
            Payment np; np.user_id = 1; np.service_id = svc; np.year = 2026; np.month = 10; np.day = 2;
            np.amount_due = 25; np.amount_paid = 25; np.status = "paid"; np.direction = "in";
            Receipt bad; bad.user_id = 1; bad.method = "bank"; bad.rf_code = "RF00BAD";
            CHECK(db.addPaymentsWithReceipt({np}, "", &bad) == -1); db.takeLastError();
            CHECK(lastPaymentId(db) == before);
            int next = db.nextReceiptNumber("A");
            Receipt good; good.user_id = 1; good.method = "ebanking"; good.bank_ref = "TRX-77"; good.paper_ref = "4521";
            int pid = db.addPaymentsWithReceipt({np}, "", &good);
            CHECK(pid > 0);
            int rcpt = db.getReceiptIdForPayment(pid);
            CHECK(rcpt > 0 && db.getReceiptById(rcpt).number == next && db.getReceiptById(rcpt).bank_ref == "TRX-77");
            // resolving a future payment with its receipt
            Payment f = db.getPaymentById(fut); f.amount_paid = 15; f.status = "paid";
            CHECK(db.editPaymentWithReceipt(fut, f, good));
            CHECK(db.getReceiptIdForPayment(fut) > 0 && db.getPaymentById(fut).status == "paid");
            Payment o = db.getPaymentById(out); o.notes = "x";
            CHECK(!db.editPaymentWithReceipt(out, o, good)); db.takeLastError();   // outgoing: refused...
            CHECK(db.getPaymentById(out).notes.empty());                            // ...and the edit rolled back too
        }

        // void -> payments unlock -> re-receipt gets the next number
        CHECK(!db.voidReceipt(rid, "  ")); db.takeLastError();
        CHECK(db.voidReceipt(rid, "Λάθος ποσό στην p3"));
        CHECK(db.getReceiptById(rid).status == "void");
        CHECK(!db.voidReceipt(rid, "ξανά")); db.takeLastError();
        ed = db.getPaymentById(p3); ed.amount_paid = 15;
        CHECK(db.editPayment(p3, ed));
        int rid2 = db.issueReceipt(r, {p1, p2, p3});
        CHECK(rid2 > 0 && db.getReceiptById(rid2).number == 6 && ToCents(db.getReceiptById(rid2).total) == 4500);
        CHECK(db.getReceiptPaymentIds(rid).empty());
        CHECK(!db.executeRawQuery("UPDATE receipts SET notes='x' WHERE id = " + std::to_string(rid), c, rows, err)); // void is final

        // ---- bills vs money: one transfer for two bills, partially; the rest stays as credit ----
        {
            User u2; u2.full_name = "ΔΕΥΤΕΡΟΣ"; db.addUser(u2);
            int c2 = 2;
            Service s2; s2.label = "ΚΑΘΑΡΙΟΤΗΤΑ"; s2.suggested_price = 50; int svc2 = db.addServiceGetId(c2, s2);
            int b1 = addPay(db, c2, svc2, 50, 0), b2 = addPay(db, c2, svc2, 30, 0);     // two unpaid bills
            CHECK(db.getOpenCharges(c2).size() == 2);
            MoneyPayment m; m.user_id = c2; m.pay_date = "2026-10-05"; m.amount = 100;
            Receipt rr; rr.user_id = c2; rr.method = "bank"; rr.bank_ref = "TRX-9";
            int mid = db.recordPayment(m, {{-1, b1, 50}, {-1, b2, 20}}, &rr);
            CHECK(mid > 0);
            CHECK(ToCents(db.getPaymentById(b1).balance) == 0 && ToCents(db.getPaymentById(b2).balance) == 1000);
            CHECK(ToCents(db.getClientCredit(c2)) == 3000);                                  // 100 - 50 - 20
            CHECK(db.getOpenCharges(c2).size() == 1);
            CHECK(db.getReceiptIdForPayment(b1) > 0 && db.getReceiptIdForPayment(b1) == db.getReceiptIdForPayment(b2));
            // a second instalment on b2, no receipt yet -> shows up in "missing receipt"
            MoneyPayment m2; m2.user_id = c2; m2.pay_date = "2026-10-20"; m2.amount = 10;
            CHECK(db.recordPayment(m2, {{-1, b2, 10}}, nullptr) > 0);
            CHECK(ToCents(db.getPaymentById(b2).balance) == 0);
            CHECK(db.getMoneyForCharge(b2).size() == 2);
            CHECK(db.countPayments("", "", c2, -1, "", "", true) == 1);                    // b2 has un-receipted money
            // over-allocation is refused, nothing saved
            MoneyPayment m3; m3.user_id = c2; m3.pay_date = "2026-10-21"; m3.amount = 5;
            CHECK(db.recordPayment(m3, {{-1, b1, 5}}, nullptr) == -1); db.takeLastError();   // b1 already settled
            CHECK(db.getMoneyForUser(c2).size() == 2);

            // ---- receipt search: AND / OR / NOT ----
            DataConn::ReceiptQuery q;
            DataConn::ReceiptCond byClient; byClient.field = DataConn::ReceiptCond::Client; byClient.id = c2;
            DataConn::ReceiptCond bySvc; bySvc.field = DataConn::ReceiptCond::Service; bySvc.id = svc2;
            DataConn::ReceiptCond byBank; byBank.field = DataConn::ReceiptCond::Method; byBank.text = "bank";
            q.conds = {byClient, bySvc};
            CHECK(db.countReceipts(q) == 1);
            q.conds = {byClient}; q.conds[0].negate = true;                                  // NOT client 2
            long long others = db.countReceipts(q);
            DataConn::ReceiptQuery all;
            CHECK(others == db.countReceipts(all) - 1);
            q.matchAll = false; q.conds = {bySvc, byBank};                                   // svc2 OR bank
            CHECK(db.countReceipts(q) >= 1);
            q.matchAll = true; q.conds = {bySvc}; q.conds[0].id = svc;                       // first service's receipts
            CHECK(db.countReceipts(q) >= 1);
            auto lines = db.getReceiptLines(db.getReceiptIdForPayment(b1));
            CHECK(lines.size() == 2);
        }
    }
    std::filesystem::remove_all(dir);
    std::printf(fails ? "%d FAILED\n" : "all receipt tests passed\n", fails);
    return fails ? 1 : 0;
}
