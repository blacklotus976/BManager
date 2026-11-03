// Builds the databases that ship with the program, from scratch, through the same calls the screens use:
//   make_release_dbs <folder>      ->  <folder>/data.db (clean demo)  and  <folder>/empty.db (same tables, nothing in them)
// The demo keeps the three kinds of people apart:
//   * CLIENTS   people and businesses we serve (own services), plus one client per building. They never live in a managed building as residents.
//   * RESIDENTS people living in the apartments of the buildings we MANAGE (own table, own accounting record, never listed as clients).
//   * PARTNERS  the people / firms that do the work for us and that we pay.
// and shows every case: clients who pay at once / late / never / partly / at a loss, what we owe partners, scheduled bills,
// managed buildings with a healthy fund, a so-so fund and a bad one.
#include "SqliteConn.h"
#include "Money.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

static const std::string kToday = "2026-10-27";

static std::string Ago(int days) {
    int y = std::atoi(kToday.substr(0, 4).c_str()), m = std::atoi(kToday.substr(5, 2).c_str()), d = std::atoi(kToday.substr(8, 2).c_str()) - days;
    static const int dm[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    while (d < 1) { if (--m < 1) { m = 12; y--; } d += dm[m - 1]; }
    char b[16]; std::snprintf(b, sizeof(b), "%04d-%02d-%02d", y, m, d); return b;
}
static std::string Day(const std::string& ym, int d) { char b[16]; std::snprintf(b, sizeof(b), "%s-%02d", ym.c_str(), d); return b; }
static std::string NextMonth(const std::string& ym) {
    int y = std::atoi(ym.substr(0, 4).c_str()), m = std::atoi(ym.substr(5, 2).c_str());
    if (++m > 12) { m = 1; y++; }
    char b[16]; std::snprintf(b, sizeof(b), "%04d-%02d", y, m); return b;
}
static const char* kMonth[] = { "Ιανουαρίου", "Φεβρουαρίου", "Μαρτίου", "Απριλίου", "Μαΐου", "Ιουνίου", "Ιουλίου",
                                "Αυγούστου", "Σεπτεμβρίου", "Οκτωβρίου", "Νοεμβρίου", "Δεκεμβρίου" };

static unsigned g_rnd = 20261027u;
static int Rnd(int n) { g_rnd = g_rnd * 1103515245u + 12345u; return (int)((g_rnd >> 8) % (unsigned)n); }

static int g_fail = 0;
static void Need(bool ok, SqliteConn& c, const char* what) { if (!ok) { std::printf("FAILED %s: %s\n", what, c.takeLastError().c_str()); g_fail++; } }

static std::vector<JointShare> Equal(const std::vector<User>& rs, long long totalC) {
    std::vector<JointShare> out; if (rs.empty()) return out;
    long long each = totalC / (long long)rs.size(), rest = totalC - each * (long long)rs.size();
    for (size_t i = 0; i < rs.size(); i++) {
        JointShare s; s.apartment_id = rs[i].apartment_id; s.user_id = rs[i].id; s.amount = FromCents(each + ((long long)i < rest ? 1 : 0));
        out.push_back(s);
    }
    return out;
}

static int UserByName(SqliteConn& c, const std::string& name) {
    for (auto& u : c.searchUsers(name, 0, 20)) if (u.full_name == name && !u.resident) return u.id;
    return -1;
}

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    if (argc < 2) { std::printf("make_release_dbs <folder>\n"); return 2; }
    fs::path dir = argv[1]; fs::create_directories(dir);
    fs::remove(dir / "data.db"); fs::remove(dir / "data.db-wal"); fs::remove(dir / "data.db-shm");
    fs::remove(dir / "empty.db"); fs::remove(dir / "empty.db-wal"); fs::remove(dir / "empty.db-shm");
    { SqliteConn e((dir / "empty.db").string()); }                 // the same tables, nothing in them

    SqliteConn c((dir / "data.db").string());

    // ---------------- partners: who we pay for the work ----------------
    auto partner = [&](const char* name, const char* trade, const char* phone, bool active) {
        Partner p; p.name = name; p.trade = trade; p.phone = phone; p.active = active; int id = c.savePartner(p); Need(id > 0, c, name); return id; };
    const int pPlumber = partner("Νίκος Παπαγεωργίου", "Υδραυλικός", "6944 123 456", true);
    const int pElectric = partner("Ηλεκτρολογικά Μιχαηλίδης", "Ηλεκτρολόγος", "210 555 0188", true);
    const int pCleaner = partner("Μαρία Σταθοπούλου", "Καθαρίστρια κλιμακοστασίων", "6977 654 321", true);
    const int pBoiler = partner("Καυστήρες ΑΕ", "Τεχνικός θέρμανσης", "210 555 0142", true);

    // ---------------- services we sell (price we charge, cost to us) ----------------
    auto service = [&](const char* label, double price, double cost) {
        Service s; s.label = label; s.suggested_price = price; int id = c.addServiceGetId(-1, s); Need(id > 0, c, label);
        if (cost > 0) c.setServiceCost(id, cost); return id; };
    const int sPest = service("ΑΠΕΝΤΟΜΩΣΗ", 60, 25);
    const int sPipe = service("ΕΠΙΣΚΕΥΗ ΥΔΡΑΥΛΙΚΩΝ", 100, 60);
    const int sBoiler = service("ΣΥΝΤΗΡΗΣΗ ΚΑΥΣΤΗΡΑ", 90, 40);
    const int sClean = service("ΚΑΘΑΡΙΣΜΟΣ ΔΕΞΑΜΕΝΗΣ", 150, 70);
    const int sMonthly = service("ΜΗΝΙΑΙΑ ΥΠΗΡΕΣΙΑ ΣΥΝΤΗΡΗΣΗΣ", 50, 20);
    const int pkg = c.addMasterService("ΠΑΚΕΤΟ ΕΤΗΣΙΑΣ ΣΥΝΤΗΡΗΣΗΣ");
    if (pkg > 0) { c.addMasterServiceComponent(pkg, sBoiler); c.addMasterServiceComponent(pkg, sPest); c.addMasterServiceComponent(pkg, sClean); }

    // ---------------- buildings (4): three we manage, one that is just a client ----------------
    struct B { const char* addr; const char* area; const char* pc; const char* afm; std::vector<const char*> apts; bool managed; double open; const char* mood; };
    std::vector<B> bs = {
        { "ΠΑΤΗΣΙΩΝ 205", "ΑΘΗΝΑ", "11253", "099000101", { "Ισόγειο", "Α1", "Α2", "Β1", "Β2", "Γ1" }, true, 900, "good" },
        { "ΚΗΦΙΣΙΑΣ 118", "ΜΑΡΟΥΣΙ", "15125", "099000202", { "Α1", "Α2", "Β1", "Β2", "Γ1" }, true, 300, "mixed" },
        { "ΤΣΙΜΙΣΚΗ 81", "ΘΕΣΣΑΛΟΝΙΚΗ", "54622", "099000303", { "Ισόγειο", "1ος", "2ος", "3ος", "4ος" }, true, 100, "bad" },
        { "ΑΙΓΑΙΟΥ 9", "ΝΕΟ ΨΥΧΙΚΟ", "15451", "099000404", { "Α1", "Α2", "Β1" }, false, 0, "client" } };
    static const char* kRes[] = { "ΑΝΑΣΤΑΣΙΟΥ ΔΗΜΗΤΡΙΟΣ", "ΑΝΤΩΝΙΟΥ ΧΡΗΣΤΟΣ", "ΒΛΑΧΟΥ ΣΟΦΙΑ", "ΓΕΩΡΓΙΟΥ ΙΩΑΝΝΗΣ", "ΔΗΜΗΤΡΙΟΥ ΕΥΑΓΓΕΛΙΑ", "ΚΑΡΑΓΙΑΝΝΗΣ ΝΙΚΟΛΑΟΣ",
                                  "ΚΟΥΤΣΟΥΚΟΣ ΣΤΑΥΡΟΣ", "ΚΩΝΣΤΑΝΤΙΝΙΔΟΥ ΜΑΡΙΑ", "ΜΑΚΡΗΣ ΕΛΕΥΘΕΡΙΟΣ", "ΜΙΧΑΗΛΙΔΟΥ ΑΝΝΑ", "ΝΙΚΟΛΑΟΥ ΕΛΕΝΗ", "ΟΙΚΟΝΟΜΟΥ ΒΑΣΙΛΕΙΟΣ",
                                  "ΠΑΠΑΔΟΠΟΥΛΟΣ ΓΕΩΡΓΙΟΣ", "ΠΑΠΠΑΣ ΘΕΟΔΩΡΟΣ", "ΠΕΤΡΟΥ ΔΗΜΗΤΡΑ", "ΣΑΡΡΗΣ ΑΝΤΩΝΙΟΣ", "ΣΤΑΥΡΟΥ ΑΙΚΑΤΕΡΙΝΗ", "ΧΡΙΣΤΟΔΟΥΛΟΥ ΠΑΝΑΓΙΩΤΗΣ",
                                  "ΛΙΑΠΗΣ ΚΩΣΤΑΣ", "ΜΠΑΚΑΛΗΣ ΣΩΤΗΡΗΣ", "ΤΣΟΥΚΑΛΑΣ ΑΛΕΞΗΣ", "ΦΡΑΓΚΟΥ ΒΙΒΗ" };
    std::vector<int> bid(bs.size());
    int rn = 0;
    for (size_t i = 0; i < bs.size(); i++) {
        Building b; b.address = bs[i].addr; b.area = bs[i].area; b.postal_code = bs[i].pc; b.afm = bs[i].afm;
        bid[i] = c.addBuilding(b); Need(bid[i] > 0, c, bs[i].addr);
        std::vector<int> apt;
        for (auto* l : bs[i].apts) apt.push_back(c.addApartment(bid[i], l));
        if (!bs[i].managed) continue;                                           // the 4th building has no residents of ours
        Need(c.setBuildingManaged(bid[i], true), c, "managed");
        c.fundAdjust(bid[i], bs[i].open, "2026-06-01", "Αρχικό υπόλοιπο: παραλαβή ταμείου", true);
        for (size_t k = 0; k < apt.size(); k++) {
            if (bs[i].managed && k == apt.size() - 1 && i == 1) continue;       // one empty apartment
            DataConn::Resident r; r.apartment_id = apt[k]; r.full_name = kRes[rn % 20]; r.phone = "69" + std::to_string(10000000 + rn * 1111); r.start_date = "2026-01-01";
            rn++;
            if (k == 1 && i == 2) r.start_date = "2026-01-01";
            int rid = c.addResident(r); Need(rid > 0, c, "resident");
            if (k == 2 && i == 2) {                       // moved out in August; a new person moves in
                c.endResident(rid, "2026-08-31");
                DataConn::Resident n; n.apartment_id = apt[k]; n.full_name = "ΤΣΟΥΚΑΛΑΣ ΑΛΕΞΗΣ"; n.phone = "6970001234"; n.start_date = "2026-09-01";
                Need(c.addResident(n) > 0, c, "new resident");
            }
        }
    }

    // ---------------- clients: people and businesses (never residents) ----------------
    struct P { const char* name; const char* phone; };
    auto person = [&](const char* name, const char* phone, const char* kind = "person", const char* afm = "") {
        User u; u.full_name = name; u.phone = phone; u.kind = kind; u.afm = afm; u.email = ""; Need(c.addUser(u), c, name); return UserByName(c, name); };
    auto charge = [&](int uid, int svc, const std::string& date, double amount, const char* note, const char* status = "pending") {
        Payment p; p.user_id = uid; p.service_id = svc; p.direction = "in"; p.status = status; p.amount_due = amount; p.amount_paid = 0; p.apartment_id = 0;
        p.year = std::atoi(date.substr(0, 4).c_str()); p.month = std::atoi(date.substr(5, 2).c_str()); p.day = std::atoi(date.substr(8, 2).c_str()); p.notes = note;
        if (std::string(status) == "future") p.resolution_date = date;
        if (!c.addPayment(p)) { std::printf("charge: %s\n", c.takeLastError().c_str()); g_fail++; return -1; }
        int id = -1; for (auto& o : c.getOpenCharges(uid)) id = std::max(id, o.id);
        return id; };
    auto pay = [&](int uid, int cid, const std::string& date, double amount) {
        MoneyPayment mp; mp.user_id = uid; mp.pay_date = date; mp.amount = amount; mp.method = "cash";
        Receipt rc; rc.method = "cash";
        if (c.recordPayment(mp, {{-1, cid, amount}}, &rc) <= 0) { std::printf("pay: %s\n", c.takeLastError().c_str()); g_fail++; } };
    auto cost = [&](int uid, int svc, int pid, const std::string& date, double amount, double paidNow, const char* note) {
        Payment p; p.user_id = uid; p.service_id = svc; p.direction = "out"; p.partner_id = pid; p.notes = note;
        p.amount_due = amount; p.amount_paid = paidNow; p.status = paidNow + 0.004 >= amount ? "paid" : "pending";
        p.year = std::atoi(date.substr(0, 4).c_str()); p.month = std::atoi(date.substr(5, 2).c_str()); p.day = std::atoi(date.substr(8, 2).c_str());
        p.apartment_id = 0;
        if (!c.addPaymentsWithReceipt({p}, "", nullptr)) { std::printf("cost: %s\n", c.takeLastError().c_str()); g_fail++; } };

    // 1 excellent: six months, every bill paid the same day
    { int u = person("ΑΡΒΑΝΙΤΗΣ ΛΕΩΝΙΔΑΣ", "6911000001"); for (int k : {150, 120, 90, 60, 30, 5}) { int id = charge(u, sMonthly, Ago(k), 50, "μηνιαία συντήρηση"); pay(u, id, Ago(k), 50); cost(u, sMonthly, pElectric, Ago(k), 20, 20, "κόστος"); } }
    // 2 good: pays 3-5 days late; a fresh bill is open (owes now, but not late)
    { int u = person("ΒΑΣΙΛΟΠΟΥΛΟΥ ΧΡΙΣΤΙΝΑ", "6911000002"); for (int k : {140, 105, 70, 35}) { int id = charge(u, sBoiler, Ago(k), 90, "συντήρηση καυστήρα"); pay(u, id, Ago(k - 4), 90); cost(u, sBoiler, pBoiler, Ago(k), 40, 40, "κόστος"); } charge(u, sBoiler, Ago(8), 90, "συντήρηση καυστήρα"); }
    // 3 average: about 25 days late every time
    { int u = person("ΓΚΙΚΑΣ ΝΙΚΟΣ", "6911000003"); for (int k : {160, 120, 80}) { int id = charge(u, sPipe, Ago(k), 100, "επισκευή υδραυλικών"); pay(u, id, Ago(k - 25), 100); cost(u, sPipe, pPlumber, Ago(k), 60, 60, "κόστος"); } }
    // 4 slow: 50-60 days late, and one bill 45 days open
    { int u = person("ΔΡΑΚΟΣ ΜΑΝΩΛΗΣ", "6911000004"); for (int k : {170, 130, 95}) { int id = charge(u, sMonthly, Ago(k), 50, "μηνιαία συντήρηση"); pay(u, id, Ago(k - 55), 50); } charge(u, sMonthly, Ago(45), 50, "μηνιαία συντήρηση"); }
    // 5 never pays: three old bills, 60-120 days open
    { int u = person("ΕΥΣΤΑΘΙΟΥ ΡΕΝΑ", "6911000005"); charge(u, sPest, Ago(120), 60, "απεντόμωση"); charge(u, sPipe, Ago(90), 100, "επισκευή υδραυλικών"); charge(u, sClean, Ago(60), 150, "καθαρισμός δεξαμενής");
      cost(u, sPipe, pPlumber, Ago(90), 60, 60, "κόστος"); }
    // 6 pays at once, but the jobs cost us more than we charged: we LOSE money on this client
    { int u = person("ΖΟΡΜΠΑΣ ΠΑΝΟΣ", "6911000006"); int a = charge(u, sPipe, Ago(50), 100, "αντικατάσταση σωληνώσεων"); pay(u, a, Ago(50), 100); cost(u, sPipe, pPlumber, Ago(50), 165, 165, "υλικά και εργασία");
      int b = charge(u, sBoiler, Ago(20), 90, "έκτακτη βλάβη καυστήρα"); pay(u, b, Ago(20), 90); cost(u, sBoiler, pBoiler, Ago(20), 130, 130, "ανταλλακτικά"); }
    // 7 pays in parts: half at once, the rest 40 days later; the latest bill is half paid
    { int u = person("ΘΕΟΧΑΡΗΣ ΣΤΕΛΙΟΣ", "6911000007"); int a = charge(u, sClean, Ago(110), 150, "καθαρισμός δεξαμενής"); pay(u, a, Ago(108), 75); pay(u, a, Ago(68), 75);
      int b = charge(u, sClean, Ago(40), 150, "καθαρισμός δεξαμενής"); pay(u, b, Ago(38), 75); cost(u, sClean, pCleaner, Ago(40), 70, 70, "κόστος"); }
    // 8 big and profitable, with one bill scheduled for next month
    { int u = person("ΚΑΡΥΔΗΣ ΑΡΗΣ", "6911000008"); int a = charge(u, sBoiler, Ago(60), 300, "ετήσια συντήρηση εγκατάστασης"); pay(u, a, Ago(58), 300); cost(u, sBoiler, pBoiler, Ago(60), 80, 80, "κόστος");
      charge(u, sBoiler, "2026-11-15", 300, "προγραμματισμένη συντήρηση", "future"); }
    // 9 a business: pays by bank 15 days late; one cost we have not paid to the partner yet (we owe)
    { int u = person("ΤΕΧΝΙΚΗ ΚΑΡΥΔΗΣ ΕΠΕ", "2105550999", "business", "099777888"); int a = charge(u, sPipe, Ago(75), 400, "εργασίες συντήρησης καταστήματος");
      MoneyPayment mp; mp.user_id = u; mp.pay_date = Ago(60); mp.amount = 400; mp.method = "bank"; Receipt rc; rc.method = "bank"; c.recordPayment(mp, {{-1, a, 400.0}}, &rc);
      cost(u, sPipe, pPlumber, Ago(75), 220, 100, "υλικά (οφείλουμε τα υπόλοιπα)"); }
    // 10 we owe a partner for a job the client already paid
    { int u = person("ΜΥΛΩΝΑΣ ΓΙΑΝΝΗΣ", "6911000010"); int a = charge(u, sPest, Ago(30), 60, "απεντόμωση"); pay(u, a, Ago(29), 60); cost(u, sPest, pCleaner, Ago(30), 25, 0, "κόστος (απλήρωτο)"); }
    // a building as a client (the one we do NOT manage): it owes us part of a repair
    { int bc = -1; for (auto& u : c.pageUsers("", 0, {}, 0, 100, {}, 2)) if (u.client_building_id == bid[3]) bc = u.id;
      if (bc > 0) { int a = charge(bc, sPipe, Ago(45), 240, "επισκευή κοινόχρηστων σωληνώσεων"); pay(bc, a, Ago(30), 120); cost(bc, sPipe, pPlumber, Ago(45), 150, 150, "υλικά και εργασία"); } else { std::printf("no building client\n"); g_fail++; } }
    // a package charged to a client: one payment per service
    { int u = person("ΣΠΥΡΟΠΟΥΛΟΣ ΤΑΣΟΣ", "6911000011");
      if (pkg > 0) { std::vector<Payment> ps; for (int svc : {sBoiler, sPest, sClean}) { Payment p; p.user_id = u; p.service_id = svc; p.direction = "in"; p.status = "pending"; p.apartment_id = 0;
          p.amount_due = svc == sBoiler ? 90 : svc == sPest ? 60 : 150; p.amount_paid = 0; p.year = 2026; p.month = 10; p.day = 12; p.notes = "Πακέτο: ετήσια συντήρηση"; ps.push_back(p); }
        Need(c.addPaymentsWithReceipt(ps, "Πακέτο ετήσιας συντήρησης", nullptr) > 0, c, "package"); } }

    // ---------------- managed buildings: funds and residents' payments ----------------
    auto jointService = [&](const char* label, const char* notes) {
        for (auto& j : c.getJointCatalog(true)) if (j.service.label == label) return j.service.id;
        Service s; s.label = label; s.extra_notes = notes; int jid = c.addJointService(s, "equal"); return jid > 0 ? c.getJointServiceById(jid).service.id : -1; };
    const int sFund = jointService("Εισφορά ταμείου", "Χρήματα των ενοίκων στο ταμείο της πολυκατοικίας (όχι έσοδο μας).");
    const int sFee = jointService("Αμοιβή διαχείρισης", "Η αμοιβή μας για τη διαχείριση πολυκατοικίας: έσοδο μας, όχι χρήματα του ταμείου.");
    Need(sFund > 0 && sFee > 0, c, "joint services");
    const std::string first = "2026-06", last = "2026-10", lastDay = kToday;
    int nOps = 0, nExp = 0, nPay = 0;
    for (size_t bi = 0; bi < 3; bi++) {
        const std::string mood = bs[bi].mood;                         // good / mixed / bad
        auto rs = c.getBuildingResidents(bid[bi]);
        if (rs.empty()) continue;
        const long long n = (long long)rs.size();
        // how each resident behaves: some always on time, some late, some in parts, some never
        auto personality = [&](size_t idx) { int k = (int)((idx * 7 + bi * 3) % 6);
            if (mood == "good") return k == 5 ? 1 : 0;                 // nearly everybody on time
            if (mood == "mixed") return k % 4;                         // on time / late / partly / never
            return k < 2 ? 3 : (k < 4 ? 1 : 2); };                     // bad: many never pay, the rest late or partly
        auto payOp = [&](int opId, const std::string& ym, bool fee) {
            const std::string next = NextMonth(ym);
            size_t idx = 0;
            for (int cid : c.getJointOpCharges(opId)) {
                Payment ch = c.getPaymentById(cid); idx++;
                if (ch.direction != "in" || ToCents(ch.balance) <= 0) continue;
                // find the resident's index in the list to pick his personality
                size_t ri = 0; for (size_t q = 0; q < rs.size(); q++) if (rs[q].id == ch.user_id) ri = q;
                const int ps = personality(ri);
                std::vector<std::pair<std::string, long long>> pays; long long due = ToCents(ch.balance);
                if (ps == 0) pays.push_back({Day(ym, 3 + Rnd(10)), due});
                else if (ps == 1) pays.push_back({Day(next, 4 + Rnd(18)), due});
                else if (ps == 2) { pays.push_back({Day(ym, 8 + Rnd(10)), due / 3}); if (Rnd(100) < 40) pays.push_back({Day(next, 10 + Rnd(10)), due / 3}); }
                (void)fee;                                              // ps == 3: never pays
                for (auto& [d, a] : pays) {
                    if (d > lastDay || a <= 0) continue;
                    MoneyPayment m; m.user_id = ch.user_id; m.pay_date = d; m.amount = FromCents(a);
                    const char* methods[] = { "cash", "bank", "iris", "ebanking" }; m.method = methods[Rnd(4)];
                    Receipt rc; rc.method = m.method;
                    if (c.recordPayment(m, {{-1, cid, FromCents(a)}}, &rc) > 0) nPay++; else std::printf("payment: %s\n", c.takeLastError().c_str());
                }
            }
        };
        for (std::string ym = first; ym <= last; ym = NextMonth(ym)) {
            const std::string mname = kMonth[std::atoi(ym.substr(5, 2).c_str()) - 1];
            // monthly funding: healthy building asks enough, the bad one asks too little for what it spends
            const long long perRes = mood == "good" ? 3500 : (mood == "mixed" ? 3000 : 1500);
            JointOp f; f.building_id = bid[bi]; f.source_type = "service"; f.source_id = sFund; f.kind = "contribution";
            f.label = "Εισφορά " + mname; f.op_date = Day(ym, 1); f.total = FromCents(perRes * n);
            int fid = c.chargeJoint(f, {{sFund, f.total}}, Equal(rs, perRes * n));
            if (fid > 0) { nOps++; payOp(fid, ym, false); } else std::printf("funding: %s\n", c.takeLastError().c_str());
            struct Ex { const char* what; int day; long long lo, span; int chance; };
            const Ex exs[] = { {"ΔΕΗ κοινοχρήστων", 7, 4500, 6000, 100}, {"Νερό ΕΥΔΑΠ", 11, 1800, 3500, 100}, {"Καθαριότητα κλιμακοστασίου", 15, 6000, 1, 100},
                               {"Συντήρηση ανελκυστήρα", 19, 8500, 2000, 60}, {"Λαμπτήρες & ηλεκτρολογικά", 22, 1200, 2500, 35}, {"Επισκευή θυροτηλεφώνου", 26, 7000, 9000, 15} };
            for (auto& e : exs) {
                if (mood != "bad" && e.chance < 100 && Rnd(100) >= e.chance) continue;
                const std::string d = Day(ym, e.day); if (d > lastDay) continue;
                long long amt = e.lo + (e.span > 1 ? Rnd((int)e.span) : 0);
                if (mood == "bad") amt = amt * 13 / 10;                  // the bad building spends more
                if (c.fundExpenseShared(bid[bi], FromCents(amt), d, std::string(e.what) + " " + mname, "", Equal(rs, amt)) > 0) nExp++; else std::printf("expense: %s\n", c.takeLastError().c_str());
            }
            JointOp q; q.building_id = bid[bi]; q.source_type = "service"; q.source_id = sFee; q.kind = "fee"; q.label = "Αμοιβή διαχείρισης " + mname;
            q.op_date = Day(ym, 28); q.total = FromCents(500 * n);
            if (q.op_date <= lastDay) { int qid = c.chargeJoint(q, {{sFee, q.total}}, Equal(rs, 500 * n)); if (qid > 0) { nOps++; payOp(qid, ym, true); } }
        }
    }
    // a one-off fund correction in the good building (a refund received)
    c.fundAdjust(bid[0], 120.0, "2026-09-15", "Επιστροφή από ΔΕΗ: διόρθωση ταμείου", false);

    // the same money seen from every screen must agree to the cent
    {
        auto same = [&](const char* what, double x, double y) { if (ToCents(x) != ToCents(y)) { std::printf("MISMATCH %s: %.2f vs %.2f\n", what, x, y); g_fail++; } };
        auto cash = c.cashNow(); auto own = c.homeFigures(0, "", ""), jnt = c.homeFigures(1, "", "");
        same("collected", own.collected + jnt.collected, cash.moneyIn);
        same("owed to us", own.owed + jnt.owed, cash.owedIn);
        same("paid out", own.costPaid + jnt.costPaid, cash.moneyOut);
        same("we owe", own.cost + jnt.cost - own.costPaid - jnt.costPaid, cash.owedOut);
        same("charged = collected + owed (own)", own.charged, own.collected + own.owed);
        double pc = 0, pp = 0, po = 0; for (auto& s : c.partnerStats("", "")) { pc += s.cost; pp += s.paid; po += s.owedNow; }
        same("partners cost", pc, own.cost + jnt.cost); same("partners paid", pp, cash.moneyOut); same("partners owed", po, cash.owedOut);
        double owing = 0; c.countClientOwings("", &owing); same("clients owing (own)", owing, own.owed - own.scheduledIn);
        double owingJ = 0; c.countClientOwings("", &owingJ, true); same("residents owing (joint)", owingJ, jnt.owed - jnt.scheduledIn);
        // every managed building: start + the month's rows = end
        for (int b : c.getManagedBuildings()) for (std::string ym = "2026-06"; ym <= "2026-10"; ym = NextMonth(ym)) {
            auto mf = c.monthFund(b, ym); long long fund = ToCents(mf.fundStart), owed = ToCents(mf.owedStart);
            for (auto& x : mf.items) {
                if (x.kind == "expense") { fund += -ToCents(x.cost) + ToCents(x.paidIn); owed += ToCents(x.charged) - ToCents(x.paidIn); }
                else if (x.kind == "correction") fund += ToCents(x.cost);
                else if (x.kind == "funding") { fund += ToCents(x.paidIn); owed += ToCents(x.charged) - ToCents(x.paidIn); }
            }
            fund += ToCents(mf.priorPaidIn); owed -= ToCents(mf.priorPaidIn);
            if (fund != ToCents(mf.fundEnd) || owed != ToCents(mf.owedEnd)) { std::printf("MISMATCH fund of building %d in %s\n", b, ym.c_str()); g_fail++; }
        }
        std::printf("math check: cash in %.2f out %.2f | owed to us %.2f | we owe %.2f\n", cash.moneyIn, cash.moneyOut, cash.owedIn, cash.owedOut);
    }
    auto chk = c.verifyAuditLog();
    std::printf("data.db: %d fundings/fees, %d expenses, %d payments; failures %d; audit %s\n", nOps, nExp, nPay, g_fail, chk.ok ? "ok" : chk.problems.front().c_str());
    return g_fail ? 1 : 0;
}
