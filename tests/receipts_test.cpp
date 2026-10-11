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
    db.executeRawQuery("SELECT MAX(id) FROM payments", c, rows, err);
    return std::atoi(rows[0][0].c_str());
}

static int addPay(SqliteConn& db, int user, int svc, double due, double paid, const char* status = "paid", const char* dir = "in") {
    Payment p; p.user_id = user; p.service_id = svc; p.year = 2026; p.month = 10; p.day = 1;
    p.amount_due = due; p.amount_paid = paid; p.status = status; p.direction = dir;
    db.addPayment(p);
    std::vector<std::string> c; std::vector<std::vector<std::string>> rows; std::string err;
    db.executeRawQuery("SELECT MAX(id) FROM payments", c, rows, err);
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

    std::string path = (std::filesystem::temp_directory_path() / "bmanager_receipts_test.db").string();
    std::filesystem::remove(path);
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

        // void -> payments unlock -> re-receipt gets the next number
        CHECK(!db.voidReceipt(rid, "  ")); db.takeLastError();
        CHECK(db.voidReceipt(rid, "Λάθος ποσό στην p3"));
        CHECK(db.getReceiptById(rid).status == "void");
        CHECK(!db.voidReceipt(rid, "ξανά")); db.takeLastError();
        ed = db.getPaymentById(p3); ed.amount_paid = 15;
        CHECK(db.editPayment(p3, ed));
        int rid2 = db.issueReceipt(r, {p1, p2, p3});
        CHECK(rid2 > 0 && db.getReceiptById(rid2).number == 4 && ToCents(db.getReceiptById(rid2).total) == 4500);
        CHECK(db.getReceiptPaymentIds(rid).empty());
        CHECK(!db.executeRawQuery("UPDATE receipts SET notes='x' WHERE id = " + std::to_string(rid), c, rows, err)); // void is final
    }
    std::filesystem::remove(path);
    std::printf(fails ? "%d FAILED\n" : "all receipt tests passed\n", fails);
    return fails ? 1 : 0;
}
