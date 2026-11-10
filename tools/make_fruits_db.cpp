// The third database that ships with the program: a greengrocer / supermarket.
//   make_fruits_db <folder>   ->   <folder>/fruits.db
// Only people as CLIENTS (no buildings, no managed buildings, no residents, no contracts, no packages), many services named after fruit, vegetables
// and supermarket goods (each with just a price and a cost), and ONE partner, the supplier, who is paid the cost of everything we sell.
// Every sale is one charge to the client plus its cost line to the supplier (the same two lines the charge dialog makes), charged higher than the cost.
#include "SqliteConn.h"
#include "Money.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

static const std::string kToday = "2026-11-08";
static std::string Ago(int days) {
    int y = std::atoi(kToday.substr(0, 4).c_str()), m = std::atoi(kToday.substr(5, 2).c_str()), d = std::atoi(kToday.substr(8, 2).c_str()) - days;
    static const int dm[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    while (d < 1) { if (--m < 1) { m = 12; y--; } d += dm[m - 1]; }
    char b[16]; std::snprintf(b, sizeof(b), "%04d-%02d-%02d", y, m, d); return b;
}
static unsigned g_rnd = 20261108u;
static int Rnd(int n) { g_rnd = g_rnd * 1103515245u + 12345u; return (int)((g_rnd >> 8) % (unsigned)n); }
static double Cents(double v) { return std::round(v * 100.0) / 100.0; }
static int g_fail = 0;
static void Need(bool ok, SqliteConn& c, const char* what) { if (!ok) { std::printf("FAILED %s: %s\n", what, c.takeLastError().c_str()); g_fail++; } }

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    if (argc < 2) { std::printf("make_fruits_db <folder>\n"); return 2; }
    fs::path dir = argv[1]; fs::create_directories(dir);
    fs::remove(dir / "fruits.db"); fs::remove(dir / "fruits.db-wal"); fs::remove(dir / "fruits.db-shm");
    SqliteConn c((dir / "fruits.db").string());

    // the only partner: the supplier we pay for everything we sell
    Partner sup; sup.name = "ΠΡΟΜΗΘΕΥΤΗΣ ΦΡΟΥΤΩΝ & ΛΑΧΑΝΙΚΩΝ Α.Ε."; sup.trade = "Προμηθευτής"; sup.phone = "210 555 0100"; sup.active = true;
    const int pSup = c.savePartner(sup); Need(pSup > 0, c, "supplier");

    // the goods: name, price per unit (kg / piece / pack), our cost
    struct G { const char* name; double price, cost; };
    static const G goods[] = {
        { "ΜΗΛΑ ΑΡΓΟΥΣ (κιλό)", 1.80, 1.10 }, { "ΜΠΑΝΑΝΕΣ (κιλό)", 1.90, 1.20 }, { "ΠΟΡΤΟΚΑΛΙΑ ΧΥΜΟΥ (κιλό)", 1.20, 0.70 }, { "ΛΕΜΟΝΙΑ (κιλό)", 1.60, 0.95 },
        { "ΦΡΑΟΥΛΕΣ (κιλό)", 5.50, 3.60 }, { "ΣΤΑΦΥΛΙΑ ΣΟΥΛΤΑΝΙΝΑ (κιλό)", 3.20, 2.00 }, { "ΡΟΔΑΚΙΝΑ (κιλό)", 2.80, 1.70 }, { "ΝΕΚΤΑΡΙΝΙΑ (κιλό)", 2.60, 1.60 },
        { "ΚΕΡΑΣΙΑ (κιλό)", 6.90, 4.50 }, { "ΒΕΡΙΚΟΚΑ (κιλό)", 3.10, 1.90 }, { "ΚΑΡΠΟΥΖΙ (κιλό)", 0.65, 0.35 }, { "ΠΕΠΟΝΙ (κιλό)", 1.10, 0.60 },
        { "ΑΧΛΑΔΙΑ ΚΟΝΤΟΥΛΑ (κιλό)", 2.20, 1.35 }, { "ΚΙΒΙ (κιλό)", 2.90, 1.80 }, { "ΡΟΔΙΑ (κιλό)", 2.40, 1.45 }, { "ΣΥΚΑ ΦΡΕΣΚΑ (κιλό)", 7.50, 5.00 },
        { "ΑΝΑΝΑΣ (τεμάχιο)", 2.50, 1.50 }, { "ΑΒΟΚΑΝΤΟ (κιλό)", 5.90, 3.80 }, { "ΤΟΜΑΤΕΣ (κιλό)", 1.70, 1.00 }, { "ΑΓΓΟΥΡΙΑ (κιλό)", 1.40, 0.80 },
        { "ΠΑΤΑΤΕΣ (κιλό)", 0.99, 0.55 }, { "ΚΡΕΜΜΥΔΙΑ ΞΕΡΑ (κιλό)", 0.90, 0.50 }, { "ΚΑΡΟΤΑ (κιλό)", 1.10, 0.65 }, { "ΜΑΡΟΥΛΙ (τεμάχιο)", 1.50, 0.90 },
        { "ΣΠΑΝΑΚΙ (κιλό)", 2.20, 1.30 }, { "ΠΙΠΕΡΙΕΣ ΦΛΩΡΙΝΗΣ (κιλό)", 3.40, 2.10 }, { "ΚΟΛΟΚΥΘΑΚΙΑ (κιλό)", 1.60, 0.95 }, { "ΜΕΛΙΤΖΑΝΕΣ (κιλό)", 1.90, 1.15 },
        { "ΣΚΟΡΔΟ (κιλό)", 6.00, 4.00 }, { "ΜΑΪΝΤΑΝΟΣ (ματσάκι)", 1.00, 0.60 }, { "ΓΑΛΑ ΦΡΕΣΚΟ 1ΛΤ", 1.65, 1.25 }, { "ΨΩΜΙ ΤΟΣΤ", 1.80, 1.30 },
        { "ΕΛΑΙΟΛΑΔΟ 1ΛΤ", 8.90, 7.10 }, { "ΑΥΓΑ 6ΑΔΑ", 2.30, 1.70 }, { "ΤΥΡΙ ΦΕΤΑ 400γρ", 5.60, 4.10 }, { "ΓΙΑΟΥΡΤΙ ΣΤΡΑΓΓΙΣΤΟ", 1.90, 1.35 },
        { "ΧΥΜΟΣ ΠΟΡΤΟΚΑΛΙ 1ΛΤ", 2.40, 1.60 }, { "ΝΕΡΟ 6Χ1,5ΛΤ", 1.95, 1.30 }, { "ΡΥΖΙ 1 ΚΙΛΟ", 2.30, 1.60 }, { "ΜΑΚΑΡΟΝΙΑ 500γρ", 1.50, 1.00 },
        { "ΜΕΛΙ ΕΛΛΗΝΙΚΟ 450γρ", 7.50, 5.20 }, { "ΞΗΡΟΙ ΚΑΡΠΟΙ ΜΙΞ 200γρ", 3.80, 2.60 } };
    const int nG = (int)(sizeof(goods) / sizeof(goods[0]));
    std::vector<int> gid;
    for (int i = 0; i < nG; i++) {
        Service s; s.label = goods[i].name; s.suggested_price = goods[i].price;
        int id = c.addServiceGetId(-1, s); Need(id > 0, c, goods[i].name);
        if (id > 0) c.setServiceCost(id, goods[i].cost);
        gid.push_back(id);
    }

    // people (clients): first name x surname, no addresses, no buildings
    static const char* sur[] = { "ΠΑΠΑΔΟΠΟΥΛΟΣ", "ΓΕΩΡΓΙΟΥ", "ΝΙΚΟΛΑΟΥ", "ΔΗΜΗΤΡΙΟΥ", "ΚΩΝΣΤΑΝΤΙΝΟΥ", "ΑΝΑΣΤΑΣΙΟΥ", "ΙΩΑΝΝΟΥ", "ΧΡΙΣΤΟΔΟΥΛΟΥ", "ΠΑΝΑΓΙΩΤΟΥ", "ΑΘΑΝΑΣΙΟΥ",
                                 "ΒΑΣΙΛΕΙΟΥ", "ΣΤΑΥΡΟΥ", "ΜΙΧΑΗΛΙΔΗΣ", "ΑΛΕΞΙΟΥ", "ΠΕΤΡΟΥ", "ΘΕΟΔΩΡΟΥ", "ΚΑΡΑΓΙΑΝΝΗΣ", "ΟΙΚΟΝΟΜΟΥ", "ΜΑΝΩΛΑΚΗΣ", "ΒΛΑΧΟΣ" };
    static const char* fn[] = { "ΓΙΩΡΓΟΣ", "ΜΑΡΙΑ", "ΝΙΚΟΣ", "ΕΛΕΝΗ", "ΔΗΜΗΤΡΗΣ", "ΚΑΤΕΡΙΝΑ", "ΚΩΣΤΑΣ", "ΣΟΦΙΑ", "ΓΙΑΝΝΗΣ", "ΑΝΝΑ", "ΧΡΗΣΤΟΣ", "ΕΥΑΓΓΕΛΙΑ", "ΘΑΝΑΣΗΣ",
                                "ΒΑΣΙΛΙΚΗ", "ΠΑΝΑΓΙΩΤΗΣ", "ΔΕΣΠΟΙΝΑ", "ΑΝΤΩΝΗΣ", "ΧΡΙΣΤΙΝΑ", "ΣΤΕΦΑΝΟΣ", "ΙΩΑΝΝΑ" };
    std::vector<int> clients;
    for (int i = 0; i < 70; i++) {
        std::string name = std::string(sur[i % 20]) + " " + fn[(i * 7 + i / 20 * 3) % 20];
        User u; u.full_name = name; u.phone = "69" + std::to_string(40000000 + i * 1379); u.kind = "person"; u.email = ""; u.afm = "";
        Need(c.addUser(u), c, name.c_str());
        int id = -1; for (auto& f : c.searchUsers(name, 0, 20)) if (f.full_name == name && !f.resident) id = f.id;
        if (id > 0) clients.push_back(id);
    }

    // a year of shopping: every client has favourites and a rhythm; each item bought = one charge (+ its cost to the supplier)
    int sales = 0;
    for (size_t ci = 0; ci < clients.size(); ci++) {
        const int uid = clients[ci];
        int fav[4]; for (int& f : fav) f = Rnd(nG);
        const int visits = 6 + Rnd(24), gap = 330 / visits;
        for (int v = 0; v < visits; v++) {
            const int ago = 330 - v * gap - Rnd(std::max(1, gap));
            if (ago < 0) continue;
            const std::string date = Ago(ago);
            const int items = 1 + Rnd(3);
            for (int it = 0; it < items; it++) {
                const int g = Rnd(100) < 70 ? fav[Rnd(4)] : Rnd(nG);
                const int qty = 1 + Rnd(4);
                const double due = Cents(goods[g].price * qty), cost = Cents(goods[g].cost * qty);
                const int roll = Rnd(100);
                double paid = due; if (roll >= 90 && roll < 96) paid = Cents(due / 2); else if (roll >= 96) paid = 0;      // most pay at once; some half; a few owe
                Payment in; in.user_id = uid; in.service_id = gid[g]; in.direction = "in"; in.apartment_id = 0;
                in.amount_due = due; in.amount_paid = paid; in.balance = Cents(due - paid); in.status = paid + 0.004 >= due ? "paid" : "pending";
                in.year = std::atoi(date.substr(0, 4).c_str()); in.month = std::atoi(date.substr(5, 2).c_str()); in.day = std::atoi(date.substr(8, 2).c_str());
                in.notes = std::string(goods[g].name) + " x" + std::to_string(qty);
                Payment out = in; out.direction = "out"; out.partner_id = pSup; out.amount_due = cost;
                const bool recent = ago < 20;                       // the supplier is paid later for the last weeks: that is what we owe him
                out.amount_paid = recent && Rnd(100) < 60 ? 0.0 : cost; out.balance = Cents(cost - out.amount_paid);
                out.status = out.amount_paid + 0.004 >= cost ? "paid" : "pending"; out.notes = "Κόστος: " + std::string(goods[g].name);
                Need(c.addPaymentsWithReceipt({ in, out }, goods[g].name, nullptr) > 0, c, "sale"); sales++;
            }
        }
    }
    std::printf("fruits.db: %d clients, %d goods, %d sales, %d failures\n", (int)clients.size(), nG, sales, g_fail);
    return g_fail ? 1 : 0;
}
