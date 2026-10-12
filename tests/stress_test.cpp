// Randomized stress test: thousands of mixed operations (charges, payments, receipts, voids, edits, deletions, joint
// charges, installment plans, offers), then every money invariant is checked straight in SQL, plus the audit chain.
#include "SqliteConn.h"
#include "Money.h"
#include <sqlite3.h>
#include <cstdio>
#include <filesystem>
#include <random>

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } } while (0)

static long long Q1(sqlite3* db, const char* sql) {
    sqlite3_stmt* st = nullptr; long long v = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
    else std::printf("SQL: %s\n", sqlite3_errmsg(db));
    sqlite3_finalize(st);
    return v;
}

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    int ops = argc > 1 ? std::atoi(argv[1]) : 3000;
    fs::path dir = fs::temp_directory_path() / "bmanager_stress_test";
    fs::remove_all(dir); fs::create_directories(dir);
    std::string dbp = (dir / "data.db").string();
    std::mt19937 rng(12345);
    auto R = [&](int n) { return (int)(rng() % (unsigned)n); };
    int counts[12] = {0}, refused = 0;
    {
        SqliteConn c(dbp);
        // 3 buildings x 4 apartments, 24 clients; some move once
        std::vector<int> blds, apts, users;
        for (int b = 0; b < 3; b++) {
            Building B; B.address = "ΟΔΟΣ " + std::to_string(b + 1); int bid = c.addBuilding(B); blds.push_back(bid);
            for (int a = 0; a < 4; a++) apts.push_back(c.addApartment(bid, "Δ" + std::to_string(a + 1)));
        }
        for (int i = 0; i < 24; i++) {
            User u; u.full_name = "ΠΕΛΑΤΗΣ " + std::to_string(i + 1); u.apartment_id = apts[i % apts.size()];
            c.addUser(u);
        }
        for (auto& u : c.searchUsers("", 0, 100)) users.push_back(u.id);
        for (size_t i = 0; i < users.size(); i += 5) {         // every 5th client moved in July
            int a1 = apts[i % apts.size()], a2 = apts[(i + 7) % apts.size()];
            c.setResidencyHistory(users[i], {{-1, users[i], a1, "2026-01-01", "2026-06-30"}, {-1, users[i], a2, "2026-07-01", ""}});
        }
        std::vector<int> svcs;
        for (int i = 0; i < 8; i++) { Service s; s.label = "ΥΠΗΡΕΣΙΑ " + std::to_string(i); s.suggested_price = 10 + 7.35 * i; svcs.push_back(c.addServiceGetId(-1, s)); }
        Service js; js.label = "ΚΑΘΑΡΙΟΤΗΤΑ"; int jid = c.addJointService(js, "equal");
        int jsvc = -1; for (auto& j : c.getJointCatalog(true)) if (j.id == jid) jsvc = j.service.id;
        Offer of; of.name = "ΕΚΠΤΩΣΗ"; of.kind = "percent"; of.value = 15; of.valid_from = "2026-01-01"; of.valid_to = "2026-12-31";
        int offerId = c.saveOffer(of);
        CHECK(offerId > 0);

        auto date = [&]() { char d[16]; std::snprintf(d, sizeof(d), "2026-%02d-%02d", 1 + R(12), 1 + R(28)); return std::string(d); };
        auto money = [&]() { return FromCents(100 + R(20000)); };
        for (int k = 0; k < ops; k++) {
            int op = R(11);
            int u = users[R(users.size())];
            switch (op) {
            case 0: case 1: {            // charge(s), paid / partly / not / future, optional receipt, optional offer
                int n = 1 + R(3);
                std::vector<Payment> lines;
                std::string d = date();
                int kind = R(4);
                for (int i = 0; i < n; i++) {
                    Payment p; p.user_id = u; p.service_id = svcs[R(svcs.size())];
                    std::sscanf(d.c_str(), "%d-%d-%d", &p.year, &p.month, &p.day);
                    double list = money(); p.amount_due = list;
                    if (R(4) == 0) { p.offer_id = offerId; p.list_price = list; p.amount_due = MoneySub(list, OfferDiscount(of, list)); }
                    p.status = kind == 3 ? "future" : "paid"; if (kind == 3) p.resolution_date = d;
                    p.amount_paid = kind == 0 ? p.amount_due : kind == 1 ? FromCents(ToCents(p.amount_due) / 2) : 0.0;
                    p.direction = R(10) == 0 ? "out" : "in";
                    lines.push_back(p);
                }
                Receipt r; r.user_id = u; r.method = "cash";
                bool wantReceipt = kind <= 1 && R(2) == 0;
                for (auto& l : lines) if (l.direction == "out") wantReceipt = false;
                if (c.addPaymentsWithReceipt(lines, "batch", wantReceipt ? &r : nullptr) < 0) refused++; else counts[0]++;
                c.takeLastError(); break; }
            case 2: {                    // money against open bills (several), maybe a receipt, maybe extra credit
                auto open = c.getOpenCharges(u);
                if (open.empty()) break;
                std::vector<Allocation> al; long long tot = 0;
                for (auto& o : open) { if (R(2)) continue; long long a = std::max(1LL, ToCents(o.balance) - R(3) * 100); a = std::min(a, ToCents(o.balance));
                                       if (a <= 0) continue; al.push_back({-1, o.id, FromCents(a)}); tot += a; }
                if (al.empty()) break;
                if (R(3) == 0) tot += 500;   // overpaid -> credit
                MoneyPayment m; m.user_id = u; m.pay_date = date(); m.amount = FromCents(tot); m.method = "bank";
                Receipt r; r.user_id = u; r.method = "bank";
                if (c.recordPayment(m, al, R(2) ? &r : nullptr) < 0) refused++; else counts[1]++;
                c.takeLastError(); break; }
            case 3: {                    // void a receipt, sometimes reissue
                auto rs = c.getReceipts(0, 50);
                if (rs.empty()) break;
                auto rr = rs[R(rs.size())];
                if (rr.status != "issued") break;
                if (c.voidReceipt(rr.id, "λάθος")) {
                    counts[2]++;
                    if (R(2)) { auto mon = c.getReceiptMoney(rr.id); std::vector<int> ids; for (auto& m : mon) ids.push_back(m.id);
                                Receipt r; r.user_id = rr.user_id; r.method = "card"; if (!ids.empty() && c.issueReceiptForMoney(r, ids) > 0) counts[3]++; }
                } else refused++;
                c.takeLastError(); break; }
            case 4: {                    // edit a charge: change amount or resolve a future one
                auto ps = c.getPaymentsForUser(u);
                if (ps.empty()) break;
                Payment p = ps[R(ps.size())];
                if (p.status == "future") { p.status = "paid"; p.amount_paid = p.amount_due; }
                else { p.amount_paid = FromCents(R((int)ToCents(p.amount_due) + 1)); }
                if (c.editPayment(p.id, p)) counts[4]++; else refused++;
                c.takeLastError(); break; }
            case 5: {                    // delete a charge (refused when it carries a receipt)
                auto ps = c.getPaymentsForUser(u);
                if (ps.empty()) break;
                if (c.removePayment(ps[R(ps.size())].id)) counts[5]++; else refused++;
                c.takeLastError(); break; }
            case 6: {                    // a joint charge over one building's residents on a date
                int b = R(3); std::string d = date();
                std::vector<JointShare> shares; std::vector<int> inc;
                for (int a = 0; a < 4; a++) {
                    int apt = apts[b * 4 + a];
                    int who = -1;
                    for (int uu : users) for (auto& rsd : c.getUserResidencies(uu))
                        if (rsd.apartment_id == apt && rsd.from_date <= d && (rsd.to_date.empty() || d <= rsd.to_date)) who = uu;
                    JointShare s; s.apartment_id = apt; s.user_id = who;
                    if (who <= 0 || R(5) == 0) { s.included = false; s.exclude_reason = who <= 0 ? "κενό" : "εξαίρεση"; }
                    else inc.push_back((int)shares.size());
                    shares.push_back(s);
                }
                if (inc.empty()) break;
                long long total = 1000 + R(50000), each = total / (long long)inc.size();
                for (size_t i = 0; i < inc.size(); i++) shares[inc[i]].amount = FromCents(i == 0 ? total - each * (long long)(inc.size() - 1) : each);
                JointOp op; op.building_id = blds[b]; op.source_type = "joint"; op.source_id = jid; op.label = "ΚΟΙΝΟΧΡΗΣΤΑ"; op.op_date = d; op.total = FromCents(total);
                if (c.chargeJoint(op, {{jsvc, op.total}}, shares) > 0) counts[6]++; else refused++;
                c.takeLastError(); break; }
            case 7: {                    // installment plan, one receipt at the end or one per installment
                int n = 2 + R(4); long long t = 1000 + R(30000), each = t / n;
                std::vector<Payment> lines;
                for (int i = 0; i < n; i++) {
                    Payment p; p.user_id = u; p.service_id = svcs[R(svcs.size())]; p.year = 2026; p.month = 1 + (i % 12); p.day = 10;
                    p.amount_due = FromCents(i == 0 ? t - each * (n - 1) : each);
                    bool paid = i == 0; p.status = paid ? "paid" : "future"; p.amount_paid = paid ? p.amount_due : 0.0;
                    char d[16]; std::snprintf(d, sizeof(d), "2026-%02d-10", p.month); if (!paid) p.resolution_date = d;
                    lines.push_back(p);
                }
                Receipt r; r.user_id = u; r.method = "cash";
                if (c.addInstallmentPlan(lines, "ΣΧΕΔΙΟ", n, "month", R(2) ? "end" : "each", &r) > 0) counts[7]++; else refused++;
                c.takeLastError(); break; }
            case 8: {                    // edit / cancel the offer
                if (R(10) == 0) { Offer o2 = of; o2.id = -1; o2.name = "ΝΕΑ"; o2.value = 5 + R(20); if (c.saveOffer(o2) > 0) counts[8]++; }
                c.takeLastError(); break; }
            default: {                   // receipt for money that has none
                auto mon = c.getMoneyForUser(u);
                for (auto& m : mon) if (m.receipt_id <= 0 && m.direction == "in") {
                    Receipt r; r.user_id = u; r.method = "cash";
                    if (c.issueReceiptForMoney(r, {m.id}) > 0) counts[9]++; else refused++;
                    break;
                }
                c.takeLastError(); break; }
            }
        }
        auto v = c.verifyAuditLog();
        CHECK(v.ok);
        for (auto& p : v.problems) std::printf("audit: %s\n", p.c_str());
    }
    sqlite3* db = nullptr; sqlite3_open(dbp.c_str(), &db);
    const char* C = "CAST(ROUND(%s*100) AS INTEGER)";
    (void)C;
    // 1. a charge's paid amount is exactly what its allocations say; balance = due - paid
    CHECK(Q1(db, "SELECT COUNT(*) FROM charges c WHERE CAST(ROUND(c.amount_paid*100) AS INTEGER) != "
                 "COALESCE((SELECT SUM(CAST(ROUND(a.amount*100) AS INTEGER)) FROM allocations a WHERE a.charge_id = c.id), 0)") == 0);
    CHECK(Q1(db, "SELECT COUNT(*) FROM charges WHERE CAST(ROUND(balance*100) AS INTEGER) != CAST(ROUND(amount_due*100) AS INTEGER) - CAST(ROUND(amount_paid*100) AS INTEGER)") == 0);
    // 2. no money is allocated beyond its amount; every amount is whole cents
    CHECK(Q1(db, "SELECT COUNT(*) FROM payments m WHERE COALESCE((SELECT SUM(CAST(ROUND(a.amount*100) AS INTEGER)) FROM allocations a WHERE a.payment_id = m.id),0) > CAST(ROUND(m.amount*100) AS INTEGER)") == 0);
    CHECK(Q1(db, "SELECT COUNT(*) FROM charges WHERE ABS(amount_due*100 - ROUND(amount_due*100)) > 1e-6 OR ABS(amount_paid*100 - ROUND(amount_paid*100)) > 1e-6") == 0);
    // 3. an issued receipt totals exactly the money on it; money is on at most one issued receipt by construction
    CHECK(Q1(db, "SELECT COUNT(*) FROM receipts r WHERE r.status = 'issued' AND CAST(ROUND(r.total*100) AS INTEGER) != "
                 "(SELECT SUM(CAST(ROUND(m.amount*100) AS INTEGER)) FROM payments m WHERE m.receipt_id = r.id)") == 0);
    // 4. receipt numbers never repeat
    CHECK(Q1(db, "SELECT COUNT(*) FROM (SELECT series, number FROM receipts GROUP BY series, number HAVING COUNT(*) > 1)") == 0);
    // 5. joint charges: shares add up to the total; no apartment twice in one charge
    CHECK(Q1(db, "SELECT COUNT(*) FROM joint_ops o WHERE CAST(ROUND(o.total*100) AS INTEGER) != (SELECT SUM(CAST(ROUND(s.amount*100) AS INTEGER)) FROM joint_shares s WHERE s.op_id = o.id AND s.included = 1)") == 0);
    CHECK(Q1(db, "SELECT COUNT(*) FROM (SELECT op_id, apartment_id, user_id FROM joint_shares GROUP BY op_id, apartment_id, user_id HAVING COUNT(*) > 1)") == 0);
    // 6. every charge's apartment is one its client lived in on that date
    CHECK(Q1(db, "SELECT COUNT(*) FROM charges c WHERE c.apartment_id IS NOT NULL AND NOT EXISTS (SELECT 1 FROM residencies r WHERE r.user_id = c.user_id "
                 "AND r.apartment_id = c.apartment_id AND r.from_date <= c.pay_date AND (r.to_date IS NULL OR r.to_date = '' OR r.to_date >= c.pay_date)) "
                 "AND EXISTS (SELECT 1 FROM residencies r2 WHERE r2.user_id = c.user_id AND r2.from_date <= c.pay_date)") == 0);
    // 7. a fully paid "one receipt at the end" plan has its receipt
    CHECK(Q1(db, "SELECT COUNT(*) FROM payment_plans pl WHERE pl.receipt_mode = 'end' "
                 "AND NOT EXISTS (SELECT 1 FROM charges c WHERE c.batch_id = pl.batch_id AND (c.status = 'future' OR CAST(ROUND(c.balance*100) AS INTEGER) > 0)) "
                 "AND EXISTS (SELECT 1 FROM charges c WHERE c.batch_id = pl.batch_id) "
                 "AND EXISTS (SELECT 1 FROM payments m JOIN allocations a ON a.payment_id = m.id JOIN charges c ON c.id = a.charge_id "
                 "            WHERE c.batch_id = pl.batch_id AND m.direction = 'in' AND m.receipt_id IS NULL)") == 0);   // (a voided receipt is re-issued by hand)
    // 8. offer uses are real discounts
    CHECK(Q1(db, "SELECT COUNT(*) FROM offer_uses WHERE discount <= 0 OR discount > list_price") == 0);
    CHECK(Q1(db, "SELECT COUNT(*) FROM offer_uses") > 0);
    long long nCharges = Q1(db, "SELECT COUNT(*) FROM charges"), nMoney = Q1(db, "SELECT COUNT(*) FROM payments"), nRec = Q1(db, "SELECT COUNT(*) FROM receipts");
    sqlite3_close(db);
    std::printf("ops %d: charges+ %d, money %d, voids %d, reissues %d, edits %d, deletes %d, joint %d, plans %d, offers %d, receipts later %d, refused %d\n",
                ops, counts[0], counts[1], counts[2], counts[3], counts[4], counts[5], counts[6], counts[7], counts[8], counts[9], refused);
    std::printf("database: %lld charges, %lld payments, %lld receipts\n", nCharges, nMoney, nRec);
    fs::remove_all(dir);
    std::printf(fails ? "stress_test: %d FAILED\n" : "stress_test: all invariants hold\n", fails);
    return fails ? 1 : 0;
}
