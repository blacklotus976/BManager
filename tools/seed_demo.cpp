// Demo data for building management: months of fundings, expenses (shared for information), our fees, and residents
// paying - fully, partly, late (next month) or not at all. Everything goes through the same calls as the screens, so
// the fund, the audit chain and the receipts are exactly what using the program would leave behind.
//   seed_demo <database> [first month YYYY-MM] [last month YYYY-MM] [last day YYYY-MM-DD]
#include "SqliteConn.h"
#include "Money.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

static unsigned g_rnd = 20261023u;
static int Rnd(int n) { g_rnd = g_rnd * 1103515245u + 12345u; return (int)((g_rnd >> 8) % (unsigned)n); }
static std::string Day(const std::string& ym, int d) { char b[16]; std::snprintf(b, sizeof(b), "%s-%02d", ym.c_str(), d); return b; }
static std::string NextMonth(const std::string& ym) {
    int y = std::atoi(ym.substr(0, 4).c_str()), m = std::atoi(ym.substr(5, 2).c_str());
    if (++m > 12) { m = 1; y++; }
    char b[16]; std::snprintf(b, sizeof(b), "%04d-%02d", y, m); return b;
}
static const char* kMonth[] = { "Ιανουαρίου", "Φεβρουαρίου", "Μαρτίου", "Απριλίου", "Μαΐου", "Ιουνίου", "Ιουλίου",
                                "Αυγούστου", "Σεπτεμβρίου", "Οκτωβρίου", "Νοεμβρίου", "Δεκεμβρίου" };

static int ServiceFor(SqliteConn& c, const char* label, const char* notes) {
    for (auto& j : c.getJointCatalog(true)) if (j.service.label == label) return j.service.id;
    Service s; s.label = label; s.extra_notes = notes;
    int jid = c.addJointService(s, "equal");
    return jid > 0 ? c.getJointServiceById(jid).service.id : -1;
}

// one share per resident, the total split equally (cents by order)
static std::vector<JointShare> Equal(const std::vector<User>& rs, long long totalC) {
    std::vector<JointShare> out; if (rs.empty()) return out;
    long long each = totalC / (long long)rs.size(), rest = totalC - each * (long long)rs.size();
    for (size_t i = 0; i < rs.size(); i++) {
        JointShare s; s.apartment_id = rs[i].apartment_id; s.user_id = rs[i].id; s.amount = FromCents(each + ((long long)i < rest ? 1 : 0));
        out.push_back(s);
    }
    return out;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("seed_demo <database> [first YYYY-MM] [last YYYY-MM] [last day]\n"); return 2; }
    const std::string first = argc > 2 ? argv[2] : "2026-06", last = argc > 3 ? argv[3] : "2026-10", lastDay = argc > 4 ? argv[4] : "2026-10-23";
    SqliteConn c(argv[1]);
    if (first == "badpayers") {    // clients who show every case of the scoring (dates relative to argv[3] = today)
        const std::string today = argc > 3 ? argv[3] : "2026-10-24";
        int y = std::atoi(today.substr(0, 4).c_str()), m = std::atoi(today.substr(5, 2).c_str()), d = std::atoi(today.substr(8, 2).c_str());
        auto ago = [&](int days) {           // the date `days` before today
            int yy = y, mm = m, dd = d - days;
            static const int dm[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
            while (dd < 1) { if (--mm < 1) { mm = 12; yy--; } dd += dm[mm - 1]; }
            char b[16]; std::snprintf(b, sizeof(b), "%04d-%02d-%02d", yy, mm, dd); return std::string(b); };
        std::vector<std::string> cols; std::vector<std::vector<std::string>> rows; std::string err;
        c.executeRawQuery("SELECT id FROM services ORDER BY id LIMIT 1", cols, rows, err);
        if (rows.empty()) { std::printf("no service\n"); return 1; }
        const int svc = std::atoi(rows[0][0].c_str());
        auto client = [&](const char* name, const char* phone) {
            for (auto& u : c.searchUsers(name, 0, 5)) if (u.full_name == name) return u.id;
            User u; u.full_name = name; u.phone = phone; u.kind = "person"; c.addUser(u);
            for (auto& x : c.searchUsers(name, 0, 5)) if (x.full_name == name) return x.id;
            return -1; };
        auto charge = [&](int uid, const std::string& date, double amount, const char* note) {
            Payment p; p.user_id = uid; p.service_id = svc; p.direction = "in"; p.status = "pending"; p.amount_due = amount; p.amount_paid = 0;
            p.year = std::atoi(date.substr(0, 4).c_str()); p.month = std::atoi(date.substr(5, 2).c_str()); p.day = std::atoi(date.substr(8, 2).c_str()); p.notes = note;
            if (!c.addPayment(p)) { std::printf("charge: %s\n", c.takeLastError().c_str()); return -1; }
            int id = -1; for (auto& o : c.getOpenCharges(uid)) id = std::max(id, o.id);
            return id; };
        auto pay = [&](int uid, int cid, const std::string& date, double amount) {
            MoneyPayment mp; mp.user_id = uid; mp.pay_date = date; mp.amount = amount; mp.method = "cash";
            Receipt rc; rc.method = "cash";
            if (c.recordPayment(mp, {{-1, cid, amount}}, &rc) <= 0) std::printf("pay: %s\n", c.takeLastError().c_str()); };
        auto cost = [&](int uid, const std::string& date, double amount) {        // what the job cost us, paid to the partner
            Payment p; p.user_id = uid; p.service_id = svc; p.direction = "out"; p.status = "paid"; p.amount_due = amount; p.amount_paid = amount;
            p.year = std::atoi(date.substr(0, 4).c_str()); p.month = std::atoi(date.substr(5, 2).c_str()); p.day = std::atoi(date.substr(8, 2).c_str());
            if (!c.addPayment(p)) std::printf("cost: %s\n", c.takeLastError().c_str()); };
        // 1. never pays: two old bills, open for 2-3 months -> late, payment score 0 -> problem
        int a = client("ΚΑΡΑΓΙΑΝΝΗΣ ΠΕΤΡΟΣ", "6911111111");
        charge(a, ago(95), 120, "επισκευή"); charge(a, ago(65), 80, "συντήρηση");
        // 2. pays, but always 6-8 weeks late -> payment score 0 though nothing is open now -> problem
        int b = client("ΛΑΜΠΡΟΥ ΔΕΣΠΟΙΝΑ", "6922222222");
        for (int k : {150, 110, 80}) { int id = charge(b, ago(k), 60, "μηνιαία υπηρεσία"); pay(b, id, ago(k - 50), 60); }
        // 3. mostly on time, one bill 45 days open -> late
        int e = client("ΣΑΒΒΙΔΗΣ ΜΑΝΟΣ", "6933333333");
        for (int k : {120, 90, 60}) { int id = charge(e, ago(k), 50, "μηνιαία υπηρεσία"); pay(e, id, ago(k - 3), 50); }
        charge(e, ago(45), 50, "μηνιαία υπηρεσία");
        // 4. pays at once, but the job cost us more than we charged -> loss -> problem
        int f = client("ΦΩΤΙΟΥ ΑΝΔΡΕΑΣ", "6944444444");
        { int id = charge(f, ago(40), 100, "αντικατάσταση σωλήνωσης"); pay(f, id, ago(40), 100); cost(f, ago(40), 165); }
        // 5. a good client with one fresh bill (10 days) -> fine, but "owes now"
        int g = client("ΔΗΜΑΣ ΚΩΣΤΗΣ", "6955555555");
        for (int k : {100, 70, 40}) { int id = charge(g, ago(k), 90, "μηνιαία υπηρεσία"); pay(g, id, ago(k - 1), 90); }
        charge(g, ago(10), 90, "μηνιαία υπηρεσία");
        auto chk = c.verifyAuditLog();
        std::printf("bad payers added; audit %s\n", chk.ok ? "ok" : chk.problems.front().c_str());
        return 0;
    }
    if (first == "check") {        // every managed building, every month: start + the month's rows = end
        int bad = 0;
        for (int bid : c.getManagedBuildings())
            for (std::string ym = "2026-01"; ym <= "2026-12"; ym = NextMonth(ym)) {
                auto mf = c.monthFund(bid, ym);
                long long fund = ToCents(mf.fundStart), owed = ToCents(mf.owedStart);
                for (auto& x : mf.items) {
                    if (x.kind == "expense") { fund += -ToCents(x.cost) + ToCents(x.paidIn); owed += ToCents(x.charged) - ToCents(x.paidIn); }
                    else if (x.kind == "correction") fund += ToCents(x.cost);
                    else if (x.kind == "funding") { fund += ToCents(x.paidIn); owed += ToCents(x.charged) - ToCents(x.paidIn); }
                }
                fund += ToCents(mf.priorPaidIn); owed -= ToCents(mf.priorPaidIn);
                if (fund != ToCents(mf.fundEnd) || owed != ToCents(mf.owedEnd)) {
                    bad++; std::printf("building %d %s: fund %lld vs %lld, owed %lld vs %lld\n", bid, ym.c_str(), fund, ToCents(mf.fundEnd), owed, ToCents(mf.owedEnd));
                }
            }
        std::printf("check: %d month(s) off\n", bad);
        return bad ? 1 : 0;
    }
    // a demo without buildings under management: every building becomes managed, with an opening balance, and
    // every apartment gets a resident (the client living there, or a made-up name for an empty one)
    if (c.getManagedBuildings().empty()) {
        static const char* kNames[] = { "ΠΑΠΑΔΟΠΟΥΛΟΣ ΓΙΩΡΓΟΣ", "ΝΙΚΟΛΑΟΥ ΕΛΕΝΗ", "ΓΕΩΡΓΙΟΥ ΔΗΜΗΤΡΗΣ", "ΚΩΝΣΤΑΝΤΙΝΟΥ ΜΑΡΙΑ",
                                        "ΙΩΑΝΝΟΥ ΚΩΣΤΑΣ", "ΑΘΑΝΑΣΙΟΥ ΣΟΦΙΑ", "ΧΡΙΣΤΟΔΟΥΛΟΥ ΝΙΚΟΣ", "ΜΙΧΑΗΛΙΔΟΥ ΑΝΝΑ" };
        int k = 0;
        for (auto& b : c.getBuildings()) {
            if (!c.setBuildingManaged(b.id, true)) continue;
            c.fundAdjust(b.id, 150 + 50 * Rnd(6), Day(first, 1), "Αρχικό υπόλοιπο: παραλαβή ταμείου", true);
            for (auto& a : c.getApartmentsForBuilding(b.id)) {
                std::vector<std::string> cols; std::vector<std::vector<std::string>> rows; std::string err;
                c.executeRawQuery("SELECT full_name, COALESCE(phone,'') FROM users WHERE apartment_id = " + std::to_string(a.id) +
                                  " AND COALESCE(resident,0) = 0 LIMIT 1", cols, rows, err);
                DataConn::Resident r; r.apartment_id = a.id; r.start_date = "2026-01-01";
                if (!rows.empty()) { r.full_name = rows[0][0]; r.phone = rows[0][1]; }
                else r.full_name = kNames[k++ % 8];
                if (c.addResident(r) <= 0) std::printf("resident: %s\n", c.takeLastError().c_str());
            }
        }
    }
    const int sFund = ServiceFor(c, "Εισφορά ταμείου", "Χρήματα των ενοίκων στο ταμείο της πολυκατοικίας (όχι έσοδο μας).");
    const int sFee = ServiceFor(c, "Αμοιβή διαχείρισης", "Η αμοιβή μας για τη διαχείριση πολυκατοικίας: έσοδο μας, όχι χρήματα του ταμείου.");
    if (sFund < 0 || sFee < 0) { std::printf("services: %s\n", c.takeLastError().c_str()); return 1; }
    int nOps = 0, nExp = 0, nPay = 0;
    // what a resident pays and when: most at once, some in parts, some next month, a few not at all
    auto payOp = [&](int opId, const std::string& ym, bool fee) {
        const std::string next = NextMonth(ym);
        for (int cid : c.getJointOpCharges(opId)) {
            Payment ch = c.getPaymentById(cid);
            if (ch.direction != "in" || ToCents(ch.balance) <= 0) continue;
            const int r = Rnd(100);
            std::vector<std::pair<std::string, long long>> pays;
            long long due = ToCents(ch.balance);
            if (r < 58) pays.push_back({Day(ym, 3 + Rnd(12)), due});                                   // pays it all, early
            else if (r < 72) { long long half = due / 2; pays.push_back({Day(ym, 5 + Rnd(10)), half});   // half now, the rest next month
                               pays.push_back({Day(next, 4 + Rnd(15)), due - half}); }
            else if (r < 84) pays.push_back({Day(next, 2 + Rnd(20)), due});                           // all of it next month
            else if (r < 92 && !fee) pays.push_back({Day(ym, 10 + Rnd(15)), due / 3});                 // a part, and still owes
            for (auto& [d, a] : pays) {
                if (d > lastDay || a <= 0) continue;
                MoneyPayment m; m.user_id = ch.user_id; m.pay_date = d; m.amount = FromCents(a);
                const char* methods[] = { "cash", "bank", "iris", "ebanking" }; m.method = methods[Rnd(4)];
                Receipt rc; rc.method = m.method;
                if (c.recordPayment(m, {{-1, cid, FromCents(a)}}, &rc) > 0) nPay++;
                else std::printf("payment %s: %s\n", d.c_str(), c.takeLastError().c_str());
            }
        }
    };
    for (int bid : c.getManagedBuildings()) {
        auto rs = c.getBuildingResidents(bid);
        if (rs.empty()) continue;
        const long long n = (long long)rs.size();
        for (std::string ym = first; ym <= last; ym = NextMonth(ym)) {
            const std::string mname = kMonth[std::atoi(ym.substr(5, 2).c_str()) - 1];
            // the month's funding: enough for the month's usual expenses
            const long long perRes = 1500 + 500 * (long long)Rnd(5);                                 // 15 - 35 € each
            JointOp f; f.building_id = bid; f.source_type = "service"; f.source_id = sFund; f.kind = "contribution";
            f.label = "Εισφορά " + mname; f.op_date = Day(ym, 1); f.total = FromCents(perRes * n);
            int fid = c.chargeJoint(f, {{sFund, f.total}}, Equal(rs, perRes * n));
            if (fid > 0) { nOps++; payOp(fid, ym, false); } else std::printf("funding: %s\n", c.takeLastError().c_str());
            // the month's expenses, each shared among the residents for information
            struct Ex { const char* what; int day; long long lo, span; int chance; };
            const Ex exs[] = { {"ΔΕΗ κοινοχρήστων", 7, 4500, 6000, 100}, {"Νερό ΕΥΔΑΠ", 11, 1800, 3500, 100},
                               {"Καθαριότητα κλιμακοστασίου", 15, 6000, 1, 100}, {"Συντήρηση ανελκυστήρα", 19, 8500, 2000, 60},
                               {"Λαμπτήρες & ηλεκτρολογικά", 22, 1200, 2500, 35}, {"Απολύμανση - μυοκτονία", 24, 4000, 1, 25},
                               {"Επισκευή θυροτηλεφώνου", 26, 7000, 9000, 15} };
            for (auto& e : exs) {
                if (Rnd(100) >= e.chance) continue;
                const std::string d = Day(ym, e.day);
                if (d > lastDay) continue;
                long long amt = e.lo + (e.span > 1 ? Rnd((int)e.span) : 0);
                std::string label = std::string(e.what) + " " + mname;
                if (c.fundExpenseShared(bid, FromCents(amt), d, label, "", Equal(rs, amt)) > 0) nExp++;
                else std::printf("expense: %s\n", c.takeLastError().c_str());
            }
            // a second funding now and then, for something bigger
            if (Rnd(100) < 25 && Day(ym, 20) <= lastDay) {
                JointOp g = f; g.label = "Έκτακτη εισφορά: αποθεματικό " + mname; g.op_date = Day(ym, 20); g.total = FromCents(2000 * n);
                int gid = c.chargeJoint(g, {{sFund, g.total}}, Equal(rs, 2000 * n));
                if (gid > 0) { nOps++; payOp(gid, ym, false); }
            }
            // our fee for the month
            JointOp q; q.building_id = bid; q.source_type = "service"; q.source_id = sFee; q.kind = "fee";
            q.label = "Αμοιβή διαχείρισης " + mname; q.op_date = Day(ym, 28); q.total = FromCents(500 * n);
            if (q.op_date <= lastDay) {
                int qid = c.chargeJoint(q, {{sFee, q.total}}, Equal(rs, 500 * n));
                if (qid > 0) { nOps++; payOp(qid, ym, true); } else std::printf("fee: %s\n", c.takeLastError().c_str());
            }
        }
    }
    auto chk = c.verifyAuditLog();
    std::printf("seeded: %d fundings/fees, %d expenses, %d payments; audit %s\n", nOps, nExp, nPay, chk.ok ? "ok" : chk.problems.front().c_str());
    return 0;
}
