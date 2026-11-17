// The fourth database that ships with the program: a small, clear one for the client to explore.
//   make_small_db <folder>   ->   <folder>/small.db
// Three ordinary clients, one building we manage (its client record, apartments, residents, a starting fund and its limit,
// two partners of the building), one partner of ours, three services charged to every client (some paid, some owed, each
// with its cost to our partner) and one contract with one client.
#include "SqliteConn.h"
#include "Money.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>

static const std::string kToday = "2026-11-14";
static std::string Ago(int days) {
    int y = std::atoi(kToday.substr(0, 4).c_str()), m = std::atoi(kToday.substr(5, 2).c_str()), d = std::atoi(kToday.substr(8, 2).c_str()) - days;
    static const int dm[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    while (d < 1) { if (--m < 1) { m = 12; y--; } d += dm[m - 1]; }
    char b[16]; std::snprintf(b, sizeof(b), "%04d-%02d-%02d", y, m, d); return b;
}
static int g_fail = 0;
static void Need(bool ok, SqliteConn& c, const char* what) { if (!ok) { std::printf("FAILED %s: %s\n", what, c.takeLastError().c_str()); g_fail++; } }

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    if (argc < 2) { std::printf("make_small_db <folder>\n"); return 2; }
    fs::path dir = argv[1]; fs::create_directories(dir);
    for (const char* x : { "small.db", "small.db-wal", "small.db-shm" }) fs::remove(dir / x);
    SqliteConn c((dir / "small.db").string());

    // our partner: who does the work and whom we pay
    Partner pp; pp.name = "Τεχνική Υποστήριξη Παπαδάκη"; pp.trade = "Τεχνικός"; pp.phone = "6944 111 222"; pp.active = true;
    const int pTech = c.savePartner(pp); Need(pTech > 0, c, "partner");

    // the services: price we charge, cost to us
    auto service = [&](const char* label, const char* code, double price, double cost) {
        Service s; s.label = label; s.code = code; s.suggested_price = price; s.cost_price = cost;
        const int id = c.addServiceGetId(-1, s); Need(id > 0, c, label); return id; };
    const int sPest = service("ΑΠΕΝΤΟΜΩΣΗ", "Υ-01", 60, 25);
    const int sBoiler = service("ΣΥΝΤΗΡΗΣΗ ΚΑΥΣΤΗΡΑ", "Υ-02", 90, 40);
    const int sTank = service("ΚΑΘΑΡΙΣΜΟΣ ΔΕΞΑΜΕΝΗΣ", "Υ-03", 150, 70);

    // three ordinary clients
    auto client = [&](const char* name, const char* kind, const char* phone, const char* addr, const char* area, const char* tk, const char* afm) {
        User u; u.full_name = name; u.kind = kind; u.phone = phone; u.address = addr; u.area = area; u.postal_code = tk; u.afm = afm;
        Need(c.addUser(u), c, name);
        for (auto& f : c.searchUsers(name, 0, 10)) if (f.full_name == name && !f.resident) return f.id;
        return -1; };
    const int cA = client("ΠΑΠΑΔΟΠΟΥΛΟΥ ΜΑΡΙΑ", "person", "6971 234 567", "Πατησίων 120", "Κυψέλη", "11257", "");
    const int cB = client("ΓΕΩΡΓΙΟΥ ΝΙΚΟΣ", "person", "6982 345 678", "Θησέως 18", "Καλλιθέα", "17671", "");
    const int cC = client("ΚΑΦΕ ΑΘΗΝΑ Ο.Ε.", "business", "210 555 0300", "Ερμού 40", "Σύνταγμα", "10563", "099888777");

    // the building we manage: its client record, apartments, residents, starting fund and limit, its own partners
    User bu; bu.kind = "building"; bu.full_name = "ΚΗΦΙΣΙΑΣ 118"; bu.address = "ΚΗΦΙΣΙΑΣ 118"; bu.area = "Μαρούσι"; bu.postal_code = "15125"; bu.afm = "099000202";
    bu.manager_name = "Κώστας Λιάπης"; bu.phone = "210 555 0118";
    Need(c.addUser(bu), c, "building client");
    int bid = -1, cBld = -1;
    for (auto& b : c.getBuildings()) if (b.address == "ΚΗΦΙΣΙΑΣ 118") bid = b.id;
    for (auto& u : c.pageUsers("", 0, {}, 0, 100, {}, 2)) if (u.client_building_id == bid) cBld = u.id;
    Need(bid > 0 && cBld > 0, c, "building");
    Need(c.setBuildingManaged(bid, true), c, "managed");
    Need(c.fundAdjust(bid, 400, Ago(120), "Αρχικό αποθεματικό", true) > 0, c, "starting fund");
    Need(c.setDepositMin(bid, 200), c, "fund limit");
    const char* apts[] = { "Α1", "Α2", "Β1", "Β2" };
    const char* residents[] = { "ΑΝΤΩΝΙΟΥ ΧΡΗΣΤΟΣ", "ΒΛΑΧΟΥ ΣΟΦΙΑ", "ΔΗΜΗΤΡΙΟΥ ΕΛΕΝΗ", "ΜΑΚΡΗΣ ΛΕΥΤΕΡΗΣ" };
    for (int k = 0; k < 4; k++) {
        const int a = c.addApartment(bid, apts[k]); Need(a > 0, c, "apartment");
        DataConn::Resident r; r.apartment_id = a; r.full_name = residents[k]; r.phone = "69" + std::to_string(50000000 + k * 1111); r.start_date = Ago(300);
        Need(c.addResident(r) > 0, c, "resident");
    }
    for (auto x : { std::pair<const char*, const char*>{ "ΔΕΗ", "Ρεύμα κοινοχρήστων" }, { "Καθαρίστρια Ελένη Κ.", "Καθαριότητα" } }) {
        DataConn::BuildingPartner q; q.building_id = bid; q.name = x.first; q.trade = x.second;
        Need(c.saveBuildingPartner(q) > 0, c, "building partner");
    }

    // the services charged to every client (the three and the building): some paid, some still owed; each with its cost to our partner
    auto charge = [&](int uid, int svc, const std::string& date, double amount) {
        Payment p; p.user_id = uid; p.service_id = svc; p.direction = "in"; p.status = "pending"; p.amount_due = amount; p.amount_paid = 0; p.apartment_id = 0;
        p.year = std::atoi(date.substr(0, 4).c_str()); p.month = std::atoi(date.substr(5, 2).c_str()); p.day = std::atoi(date.substr(8, 2).c_str());
        if (!c.addPayment(p)) { std::printf("charge: %s\n", c.takeLastError().c_str()); g_fail++; return -1; }
        int id = -1; for (auto& o : c.getOpenCharges(uid)) id = std::max(id, o.id);
        return id; };
    auto pay = [&](int uid, int cid, const std::string& date, double amount) {
        MoneyPayment mp; mp.user_id = uid; mp.pay_date = date; mp.amount = amount; mp.method = "cash";
        Receipt rc; rc.method = "cash";
        if (c.recordPayment(mp, {{-1, cid, amount}}, &rc) <= 0) { std::printf("pay: %s\n", c.takeLastError().c_str()); g_fail++; } };
    auto cost = [&](int uid, int svc, const std::string& date, double amount, double paidNow) {
        Payment p; p.user_id = uid; p.service_id = svc; p.direction = "out"; p.partner_id = pTech; p.notes = "κόστος";
        p.amount_due = amount; p.amount_paid = paidNow; p.status = paidNow + 0.004 >= amount ? "paid" : "pending";
        p.year = std::atoi(date.substr(0, 4).c_str()); p.month = std::atoi(date.substr(5, 2).c_str()); p.day = std::atoi(date.substr(8, 2).c_str()); p.apartment_id = 0;
        if (!c.addPaymentsWithReceipt({p}, "", nullptr)) { std::printf("cost: %s\n", c.takeLastError().c_str()); g_fail++; } };
    struct S { int id; double price, cost; };
    const S svcs[3] = { { sPest, 60, 25 }, { sBoiler, 90, 40 }, { sTank, 150, 70 } };
    const int who[4] = { cA, cB, cC, cBld };
    for (int w = 0; w < 4; w++) for (int k = 0; k < 3; k++) {
        const std::string d = Ago(90 - k * 25 - w * 4);
        const int id = charge(who[w], svcs[k].id, d, svcs[k].price);
        const int state = (w + k) % 3;                       // 0 paid, 1 paid in part, 2 still owed
        if (id > 0 && state == 0) pay(who[w], id, d, svcs[k].price);
        if (id > 0 && state == 1) pay(who[w], id, d, svcs[k].price / 2);
        cost(who[w], svcs[k].id, d, svcs[k].cost, (w + k) % 2 == 0 ? svcs[k].cost : 0);      // half of the costs paid to our partner
    }

    // one contract: a yearly boiler service for the first client, in four installments (the first one paid)
    DataConn::Contract ct; ct.user_id = cA; ct.months = 12; ct.installments = 4; ct.start_date = Ago(100);
    ct.source_type = "service"; ct.source_id = sBoiler; ct.label = "ΣΥΝΤΗΡΗΣΗ ΚΑΥΣΤΗΡΑ";
    DataConn::ContractItem it; it.service_id = sBoiler; it.label = ct.label; it.amount = 320;
    const int kid = c.addContract(ct, { it }); Need(kid > 0, c, "contract");
    if (kid > 0) {
        auto ins = c.getContractInstallments(kid);
        if (!ins.empty()) for (int ch : ins[0].charges) { Payment pc = c.getPaymentById(ch); if (pc.direction == "in" && ToCents(pc.balance) > 0) pay(cA, ch, ins[0].date, pc.balance); }
    }

    auto chk = c.verifyAuditLog();
    for (auto& p : chk.problems) std::printf("audit: %s\n", p.c_str());
    std::printf("small.db: %s\n", g_fail == 0 && chk.ok ? "ok" : "PROBLEMS");
    return g_fail == 0 && chk.ok ? 0 : 1;
}
