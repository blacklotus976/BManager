// Installment plans: one batch, future installments, ONE receipt when the last installment is paid ("end" mode).
#include "SqliteConn.h"
#include "Money.h"
#include <cstdio>
#include <filesystem>

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } } while (0)

static std::vector<Payment> Plan(int user, int svc, double total, int n, bool firstPaid) {
    std::vector<Payment> v;
    long long t = ToCents(total), each = t / n;
    for (int k = 0; k < n; k++) {
        Payment p; p.user_id = user; p.service_id = svc; p.year = 2026; p.month = 1 + k; p.day = 10;
        p.amount_due = FromCents(k == n - 1 ? t - each * (n - 1) : each);
        bool paid = firstPaid && k == 0;
        p.amount_paid = paid ? p.amount_due : 0.0; p.status = paid ? "paid" : "future"; p.direction = "in";
        char d[16]; std::snprintf(d, sizeof(d), "2026-%02d-10", 1 + k); if (!paid) p.resolution_date = d;
        p.notes = "Δόση " + std::to_string(k + 1) + "/" + std::to_string(n);
        v.push_back(p);
    }
    return v;
}

int main() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "bmanager_plan_test";
    fs::remove_all(dir); fs::create_directories(dir);
    {
        SqliteConn c((dir / "data.db").string());
        User u; u.full_name = "Π"; c.addUser(u);
        Service s; s.label = "ΣΥΝΤΗΡΗΣΗ"; s.suggested_price = 100; int svc = c.addServiceGetId(-1, s);

        // "end": 3 monthly installments of 100.00, first paid now in cash
        Receipt first; first.user_id = 1; first.method = "cash";
        int plan = c.addInstallmentPlan(Plan(1, svc, 100.00, 3, true), "ΣΥΝΤΗΡΗΣΗ", 3, "month", "end", &first);
        CHECK(plan > 0);
        auto lines = c.findPayments({}, DataConn::PayOrder::DateAsc, 0, 10);
        CHECK(lines.size() == 3);
        int batch = lines[0].batch_id;
        auto pl = c.getPlanForBatch(batch);
        CHECK(pl.id == plan && pl.installments == 3 && pl.receipt_mode == "end" && ToCents(pl.total) == 10000);
        CHECK(ToCents(pl.paid) == 3333 && pl.paidInstallments == 1 && pl.receipt_id < 0);
        CHECK(c.getMoneyForUser(1).size() == 1 && c.getMoneyForUser(1)[0].receipt_id < 0);   // no receipt yet
        CHECK(c.getMoneyForUser(1)[0].method == "cash");
        // installment 2 resolved through the normal edit
        Payment p2 = lines[1]; p2.status = "paid"; p2.amount_paid = p2.amount_due;
        CHECK(c.editPayment(p2.id, p2));
        CHECK(c.getPlanForBatch(batch).receipt_id < 0);
        // installment 3 paid with "Νέα είσπραξη" -> the plan is complete -> one receipt for all 100.00
        MoneyPayment m; m.user_id = 1; m.pay_date = "2026-03-11"; m.amount = lines[2].amount_due; m.method = "bank";
        { int rp = c.recordPayment(m, {{-1, lines[2].id, lines[2].amount_due}}, nullptr); if (rp <= 0) std::printf("err: %s\n", c.takeLastError().c_str()); CHECK(rp > 0); }
        pl = c.getPlanForBatch(batch);
        CHECK(pl.receipt_id > 0 && pl.paidInstallments == 3);
        Receipt r = c.getReceiptById(pl.receipt_id);
        CHECK(ToCents(r.total) == 10000 && r.status == "issued" && r.method == "bank");
        for (auto& mm : c.getMoneyForUser(1)) CHECK(mm.receipt_id == pl.receipt_id);

        // "each": the first installment's receipt now, later ones on their own
        int plan2 = c.addInstallmentPlan(Plan(1, svc, 50.00, 2, true), "Β", 2, "week", "each", &first);
        CHECK(plan2 > 0);
        int withReceipt = 0; for (auto& mm : c.getMoneyForUser(1)) if (mm.receipt_id > 0 && mm.receipt_id != pl.receipt_id) withReceipt++;
        CHECK(withReceipt == 1);
        CHECK(c.addInstallmentPlan(Plan(1, svc, 10.00, 1, false), "x", 1, "month", "end", nullptr) == -1); c.takeLastError();
        CHECK(c.verifyAuditLog().ok);
    }
    fs::remove_all(dir);
    std::printf(fails ? "plan_test: %d FAILED\n" : "plan_test: all passed\n", fails);
    return fails ? 1 : 0;
}
