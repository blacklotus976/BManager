// Ταμείο: every movement of money in and out, with who and what for; partial payments keep a bill open until it
// is filled, each payment with its own receipt; the cost bills of one partner.
#include "SqliteConn.h"
#include "Money.h"
#include <cstdio>
#include <filesystem>

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } } while (0)

int main() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "bmanager_cash_test";
    fs::remove_all(dir); fs::create_directories(dir);
    {
        SqliteConn c((dir / "data.db").string());
        User u1; u1.full_name = "ΑΛΕΞΙΟΥ"; c.addUser(u1);
        Service s; s.label = "ΣΥΝΤΗΡΗΣΗ ΚΑΥΣΤΗΡΑ"; s.suggested_price = 100;
        int svc = c.addServiceGetId(-1, s);
        Partner pl; pl.name = "Τεχνικός"; int plid = c.savePartner(pl);

        // a bill of 100 to the client, its cost of 50 owed to the partner
        Payment ch; ch.user_id = 1; ch.service_id = svc; ch.year = 2026; ch.month = 10; ch.day = 5; ch.amount_due = 100; ch.amount_paid = 0; ch.status = "pending"; ch.direction = "in";
        Payment co = ch; co.amount_due = 50; co.direction = "out"; co.partner_id = plid;
        int bill = c.addPaymentsWithReceipt({ch, co}, "καυστήρας", nullptr);
        int cost = bill + 1;
        CHECK(bill > 0);

        // part of it: a receipt for the 40, the bill stays open with 60 owed
        MoneyPayment m; m.user_id = 1; m.pay_date = "2026-10-10"; m.amount = 40; m.method = "cash";
        Receipt r; r.method = "cash";
        int m1 = c.recordPayment(m, {{-1, bill, 40}}, &r);
        CHECK(m1 > 0);
        Payment b = c.getPaymentById(bill);
        CHECK(ToCents(b.amount_paid) == 4000 && ToCents(b.balance) == 6000);
        // more than is owed cannot go on the bill
        MoneyPayment big = m; big.amount = 70;
        CHECK(c.recordPayment(big, {{-1, bill, 70}}, nullptr) < 0);
        // the rest: a second receipt, the bill is paid
        m.pay_date = "2026-10-20"; m.amount = 60;
        int m2 = c.recordPayment(m, {{-1, bill, 60}}, &r);
        CHECK(m2 > 0);
        {   auto money = c.getMoneyForCharge(bill);            // two payments, each with its own receipt
            CHECK(money.size() == 2 && money[0].receipt_id > 0 && money[1].receipt_id > 0 && money[0].receipt_id != money[1].receipt_id);
            CHECK(money.size() == 2 && ToCents(c.getReceiptById(money[0].receipt_id).total) == 4000 && ToCents(c.getReceiptById(money[1].receipt_id).total) == 6000); }
        CHECK(ToCents(c.getPaymentById(bill).balance) == 0);

        // we pay the partner
        MoneyPayment out; out.pay_date = "2026-10-21"; out.amount = 50; out.direction = "out"; out.method = "bank"; out.user_id = 1;
        CHECK(c.recordPayment(out, {{-1, cost, 50}}, nullptr) > 0);

        DataConn::PaymentFilter f; f.partnerId = plid; CHECK(c.countPaymentsWhere(f) == 1);

        // Ταμείο
        DataConn::MoneyQuery q; double in = 0, ou = 0;
        CHECK(c.countMoney(q, &in, &ou) == 3 && ToCents(in) == 10000 && ToCents(ou) == 5000);
        auto rows = c.pageMoney(q, 0, 10);
        CHECK(rows.size() == 3 && rows[0].direction == "out" && rows[0].who == "Τεχνικός" && rows[1].who == "ΑΛΕΞΙΟΥ");
        CHECK(rows[1].what == "ΣΥΝΤΗΡΗΣΗ ΚΑΥΣΤΗΡΑ");
        q.direction = "out"; CHECK(c.countMoney(q) == 1);
        q = {}; q.text = "αλεξ"; CHECK(c.countMoney(q) == 2);
        q = {}; q.receipt = 2; CHECK(c.countMoney(q) == 0);
        q = {}; q.receipt = 1; CHECK(c.countMoney(q) == 2);
        q = {}; q.fromDate = "2026-10-20"; CHECK(c.countMoney(q) == 2);
        q = {}; q.method = "bank"; CHECK(c.countMoney(q) == 1);
    }
    fs::remove_all(dir);
    if (fails) { std::printf("%d check(s) failed\n", fails); return 1; }
    std::printf("cash: all checks passed\n");
    return 0;
}
