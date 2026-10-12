#include "Seed.h"
#include "Money.h"
#include <vector>
#include <string>
#include <map>
#include <cstdlib>
#include <cstdio>

// Demo data set. Rules followed:
//  - Uses only DataConn methods (addBuilding/addApartment/addUser/addServiceGetId/
//    addServiceUser/addPayment/addMasterService/addMasterServiceComponent) so every
//    insert goes through the same audit-logging code path as normal app usage.
//  - Buildings exist on their own with a full list of apartments (one per building is
//    deliberately left VACANT so empty units are visible). A client carries no address;
//    they just point at the apartment they live in.
//  - 16 clients across 4 buildings, distributed 5 / 3 / 4 / 4.
//  - status is only ever "paid" or "future" for new rows (no legacy "pending"/"overdue").
//  - resolved_at is never set here (only the app's Resolve action sets it).
//  - resolution_date is only ever set on status=="future" rows.
//  - direction covers both "in" (collect from client) and "out" (manager paid on
//    client's behalf) across the data.
//  - No notes are seeded: the Notes tab starts empty on purpose.

// ---------------------------------------------------------------------------
// Service catalog modelled on the real offering of expresshomeservice.gr
// (pages: διαχείριση, κοινόχρηστα, θέρμανση, συντήρηση καυστήρα, απολύμανση –
// απεντόμωση, αποφράξεις, πυροσβεστήρες, συνεργεία καθαριότητας, πακέτα).
// Each category is a PACKAGE (master service); each concrete thing you can buy
// in it is a unique SERVICE. Amounts are the site's "από" prices where it gives
// one, otherwise a plausible figure. Fire extinguishers follow the structure the
// client asked for: Αγορά – Παραχώρηση – Συντήρηση – Αναγόμωση.
// ---------------------------------------------------------------------------
static const char* kPackages[] = {
    "ΠΥΡΟΣΒΕΣΤΗΡΑΣ",                 // 0
    "ΔΙΑΧΕΙΡΙΣΗ ΠΟΛΥΚΑΤΟΙΚΙΑΣ",       // 1
    "ΘΕΡΜΑΝΣΗ",                      // 2
    "ΚΑΥΣΤΗΡΑΣ – ΛΕΒΗΤΑΣ",            // 3
    "ΑΠΟΛΥΜΑΝΣΗ – ΑΠΕΝΤΟΜΩΣΗ",        // 4
    "ΑΠΟΦΡΑΞΕΙΣ",                    // 5
    "ΣΥΝΕΡΓΕΙΑ ΚΑΘΑΡΙΟΤΗΤΑΣ",         // 6
};
struct CatSvc { int pkg; const char* label; const char* notes; double amount; };
static const CatSvc kCatalog[] = {
    {0, "ΠΥΡΟΣΒΕΣΤΗΡΑΣ – ΑΓΟΡΑ",        "Πιστοποιημένοι πυροσβεστήρες σε χαμηλές τιμές.", 35.0},
    {0, "ΠΥΡΟΣΒΕΣΤΗΡΑΣ – ΠΑΡΑΧΩΡΗΣΗ",   "Παραχώρηση πυροσβεστήρα (δωρεάν ή με μειωμένο κόστος μέσα σε πακέτο).", 15.0},
    {0, "ΠΥΡΟΣΒΕΣΤΗΡΑΣ – ΣΥΝΤΗΡΗΣΗ",    "Υποχρεωτικός ετήσιος έλεγχος και τήρηση αρχείου για τον διαχειριστή.", 25.0},
    {0, "ΠΥΡΟΣΒΕΣΤΗΡΑΣ – ΑΝΑΓΟΜΩΣΗ",    "Αναγόμωση πυροσβεστήρα (π.χ. οροφής).", 30.0},

    {1, "ΔΙΑΧΕΙΡΙΣΗ – ΜΗΝΙΑΙΑ ΑΜΟΙΒΗ",  "Διαχείριση πολυκατοικίας και ταμείου. Από 40 €/μήνα.", 40.0},
    {1, "ΔΙΑΧΕΙΡΙΣΗ – ΠΛΗΡΩΜΗ ΛΟΓΑΡΙΑΣΜΩΝ", "Πληρωμή κοινόχρηστων λογαριασμών: ΔΕΗ, νερό, φυσικό αέριο.", 120.0},
    {1, "ΔΙΑΧΕΙΡΙΣΗ – ΠΛΗΡΩΜΗ ΣΥΝΕΡΓΕΙΩΝ", "Πληρωμή συνεργείων: καθαριότητα, ασανσέρ, κηπουρός.", 90.0},
    {1, "ΚΟΙΝΟΧΡΗΣΤΑ – ΕΚΔΟΣΗ & ΔΙΑΝΟΜΗ", "Έκδοση κοινοχρήστων και διανομή ειδοποιητηρίων, με ανάρτηση παραστατικών.", 15.0},
    {1, "ΚΟΙΝΟΧΡΗΣΤΑ – ΕΙΣΠΡΑΞΗ",       "Είσπραξη κοινοχρήστων με όλους τους τρόπους και ενημέρωση υπολοίπου.", 20.0},
    {1, "ΔΙΑΧΕΙΡΙΣΗ – ΝΟΜΙΚΗ ΚΑΛΥΨΗ",   "Νομική κάλυψη και συμβουλευτική σε θέματα διαχείρισης.", 30.0},

    {2, "ΘΕΡΜΑΝΣΗ – ΚΑΘΑΡΙΣΜΟΣ ΔΕΞΑΜΕΝΗΣ", "Καθαρισμός δεξαμενής πετρελαίου θέρμανσης (για καυστήρες πετρελαίου).", 45.0},
    {2, "ΘΕΡΜΑΝΣΗ – ΠΡΟΣΘΕΤΟ ΠΕΤΡΕΛΑΙΟΥ", "Πρόσθετο καυσίμου θέρμανσης (προσφορά 1 στις 4 παραγγελίες).", 20.0},

    {3, "ΚΑΥΣΤΗΡΑΣ – ΣΥΝΤΗΡΗΣΗ ΠΕΤΡΕΛΑΙΟΥ",  "Καθαρισμός λέβητα, έλεγχος και συντήρηση καυστήρα πετρελαίου. Από 50 €.", 50.0},
    {3, "ΚΑΥΣΤΗΡΑΣ – ΣΥΝΤΗΡΗΣΗ ΦΥΣΙΚΟΥ ΑΕΡΙΟΥ", "Συντήρηση καυστήρα φυσικού αερίου από αδειούχους τεχνικούς. Από 60 €.", 60.0},
    {3, "ΚΑΥΣΤΗΡΑΣ – ΜΕΤΡΗΣΕΙΣ ΚΑΥΣΑΕΡΙΩΝ", "Μετρήσεις καυσαερίων και βαθμού απόδοσης, ρύθμιση, φύλλο συντήρησης.", 25.0},
    {3, "ΚΑΥΣΤΗΡΑΣ – ΒΛΑΒΗ",               "Αποκατάσταση βλάβης καυστήρα.", 40.0},

    {4, "ΑΠΕΝΤΟΜΩΣΗ",                      "Κατσαρίδες, κουνούπια, μυρμήγκια, ψύλλοι, κοριοί. Από 30 € ανά εφαρμογή.", 30.0},
    {4, "ΜΥΟΚΤΟΝΙΑ",                       "Καταπολέμηση ποντικιών στους κοινόχρηστους χώρους.", 35.0},
    {4, "ΑΠΟΛΥΜΑΝΣΗ – ΕΤΗΣΙΟ ΠΡΟΓΡΑΜΜΑ",   "3 εφαρμογές (Μάρτιος, Ιούνιος, Σεπτέμβριος) στην τιμή των 2.", 60.0},

    {5, "ΑΠΟΦΡΑΞΗ – ΠΡΟΛΗΠΤΙΚΟΣ ΚΑΘΑΡΙΣΜΟΣ", "Καθαρισμός οριζόντιου δικτύου με πιεστικό νερού. Από 55 €.", 55.0},
    {5, "ΑΠΟΦΡΑΞΗ – ΕΚΤΑΚΤΗ",              "Άμεση εξυπηρέτηση σε έκτακτες αποφράξεις, ακόμα και αργίες.", 70.0},
    {5, "ΑΠΟΦΡΑΞΗ – ΕΛΕΓΧΟΣ ΜΕ ΚΑΜΕΡΑ",     "Κάμερα για εντοπισμό βλαβών στο αποχετευτικό δίκτυο.", 60.0},

    {6, "ΚΑΘΑΡΙΟΤΗΤΑ – ΤΑΚΤΙΚΗ",           "Τακτικός καθαρισμός κλιμακοστασίου και κοινόχρηστων. Από 50 €.", 50.0},
    {6, "ΚΑΘΑΡΙΟΤΗΤΑ – ΕΙΔΙΚΕΣ ΕΡΓΑΣΙΕΣ",  "Τοίχοι, ταράτσα, γκαράζ, ακάλυπτοι, λεβητοστάσιο.", 80.0},
    {6, "ΚΗΠΟΥΡΟΣ",                        "Φροντίδα κήπου και φύτευση/κλάδεμα.", 35.0},
};
// Express "bundle" packages: each lists catalog services by label.
struct Bundle { const char* name; std::vector<const char*> parts; };
static const std::vector<Bundle> kBundles = {
    {"ΠΑΚΕΤΟ BASIC", {"ΚΟΙΝΟΧΡΗΣΤΑ – ΕΚΔΟΣΗ & ΔΙΑΝΟΜΗ", "ΑΠΕΝΤΟΜΩΣΗ", "ΚΑΥΣΤΗΡΑΣ – ΣΥΝΤΗΡΗΣΗ ΠΕΤΡΕΛΑΙΟΥ",
        "ΚΑΥΣΤΗΡΑΣ – ΜΕΤΡΗΣΕΙΣ ΚΑΥΣΑΕΡΙΩΝ", "ΚΑΥΣΤΗΡΑΣ – ΒΛΑΒΗ", "ΘΕΡΜΑΝΣΗ – ΚΑΘΑΡΙΣΜΟΣ ΔΕΞΑΜΕΝΗΣ"}},
    {"ΠΑΚΕΤΟ ALL IN ONE", {"ΔΙΑΧΕΙΡΙΣΗ – ΜΗΝΙΑΙΑ ΑΜΟΙΒΗ", "ΔΙΑΧΕΙΡΙΣΗ – ΠΛΗΡΩΜΗ ΛΟΓΑΡΙΑΣΜΩΝ", "ΚΟΙΝΟΧΡΗΣΤΑ – ΕΚΔΟΣΗ & ΔΙΑΝΟΜΗ",
        "ΚΟΙΝΟΧΡΗΣΤΑ – ΕΙΣΠΡΑΞΗ", "ΔΙΑΧΕΙΡΙΣΗ – ΝΟΜΙΚΗ ΚΑΛΥΨΗ"}},
    {"ΠΑΚΕΤΟ FULL PACK", {"ΔΙΑΧΕΙΡΙΣΗ – ΜΗΝΙΑΙΑ ΑΜΟΙΒΗ", "ΚΑΘΑΡΙΟΤΗΤΑ – ΤΑΚΤΙΚΗ", "ΑΠΟΛΥΜΑΝΣΗ – ΕΤΗΣΙΟ ΠΡΟΓΡΑΜΜΑ",
        "ΚΑΥΣΤΗΡΑΣ – ΣΥΝΤΗΡΗΣΗ ΠΕΤΡΕΛΑΙΟΥ", "ΚΑΥΣΤΗΡΑΣ – ΜΕΤΡΗΣΕΙΣ ΚΑΥΣΑΕΡΙΩΝ", "ΚΑΥΣΤΗΡΑΣ – ΒΛΑΒΗ",
        "ΘΕΡΜΑΝΣΗ – ΚΑΘΑΡΙΣΜΟΣ ΔΕΞΑΜΕΝΗΣ", "ΑΠΟΦΡΑΞΗ – ΠΡΟΛΗΠΤΙΚΟΣ ΚΑΘΑΡΙΣΜΟΣ", "ΠΥΡΟΣΒΕΣΤΗΡΑΣ – ΑΝΑΓΟΜΩΣΗ"}},
};

struct YM { int y, m; };

// Every demo payment gets a believable "entered at" time (minute precision) so the apartment
// history can be told per minute: on the payment's own date at a working-day hour; scheduled
// (future) payments were entered in September.
static int g_seedSeq = 0;
static void SeedAddPayment(DataConn* db, Payment p) {
    int s = g_seedSeq++;
    char buf[32];
    if (p.status == "future") std::snprintf(buf, sizeof(buf), "2026-09-%02d %02d:%02d:00", 1 + s % 28, 8 + (s * 7) % 10, (s * 13) % 60);
    else std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:00", p.year, p.month, p.day, 8 + (s * 7) % 10, (s * 13) % 60);
    p.created_at = buf;
    db->addPayment(p);
}

void seedDemoDataIfEmpty(DataConn* db, const std::string& mediaDir) {
    if (db->getRowCount("users") > 0) return; // already has data

    // ---- Buildings and their apartments -------------------------------------------
    struct SeedBuilding { const char* address; const char* area; const char* postal; const char* afm;
                          std::vector<std::string> apartments; };
    std::vector<SeedBuilding> seedBuildings = {
        {"ΠΑΤΗΣΙΩΝ 205",  "ΑΘΗΝΑ",        "11253", "094018732", {"Ισόγειο","Α1","Α2","Β1","Β2"}},          // 5 residents = 5 apartments
        {"ΚΗΦΙΣΙΑΣ 118",  "ΜΑΡΟΥΣΙ",      "15125", "998456321", {"Α1","Α2","Β1","Β2"}},                          // 3 residents = 3 apartments
        {"ΤΣΙΜΙΣΚΗ 81",   "ΘΕΣΣΑΛΟΝΙΚΗ",  "54622", "800129457", {"Ισόγειο","1ος","2ος","3ος","4ος"}},             // 4 residents = 4 apartments
        {"ΑΙΓΑΙΟΥ 9",     "ΝΕΟ ΨΥΧΙΚΟ",   "15451", "090276154", {"Α1","Α2","Β1","Β2","Γ1"}},                // 4 residents = 4 active apartments (Γ1 is empty now)
    };
    // apartmentId[b][label] -> real DB id, so each client can be pointed at their unit.
    std::vector<std::map<std::string,int>> apartmentId(seedBuildings.size());
    std::vector<int> buildingIds;
    for (size_t b = 0; b < seedBuildings.size(); ++b) {
        Building bld;
        bld.address = seedBuildings[b].address; bld.area = seedBuildings[b].area;
        bld.postal_code = seedBuildings[b].postal; bld.afm = seedBuildings[b].afm;
        int bid = db->addBuilding(bld);
        if (bid < 0) return; // sanity guard, shouldn't happen
        buildingIds.push_back(bid);
        for (auto& label : seedBuildings[b].apartments)
            apartmentId[b][label] = db->addApartment(bid, label);
    }

    // ---- 16 clients. Every field filled in (name, phone, special code, residence) ---
    struct SeedUser { const char* name; const char* phone; const char* special; int bldg; const char* apt; };
    std::vector<SeedUser> seedUsers = {
        // Building 0 -- ΠΑΤΗΣΙΩΝ 205 (5)
        {"ΠΑΠΑΔΟΠΟΥΛΟΣ ΓΕΩΡΓΙΟΣ",     "6944123456", "Π-104",  0, "Ισόγειο"},
        {"ΑΝΑΣΤΑΣΙΟΥ ΔΗΜΗΤΡΙΟΣ",      "2106547890", "ΚΩΔ-77", 0, "Α1"},
        {"ΣΤΑΥΡΟΥ ΑΙΚΑΤΕΡΙΝΗ",        "2109988776", "Π-118",  0, "Α2"},
        {"ΓΕΩΡΓΙΟΥ ΙΩΑΝΝΗΣ",          "6981122334", "Π-211",  0, "Β1"},
        {"ΜΑΚΡΗΣ ΕΛΕΥΘΕΡΙΟΣ",         "6970011223", "ΚΩΔ-31", 0, "Β2"},
        // Building 1 -- ΚΗΦΙΣΙΑΣ 118 (3)
        {"ΝΙΚΟΛΑΟΥ ΕΛΕΝΗ",            "6971234567", "Π-142",  1, "Α1"},
        {"ΠΑΠΠΑΣ ΘΕΟΔΩΡΟΣ",           "6912345098", "Π-305",  1, "Α2"},
        {"ΜΙΧΑΗΛΙΔΟΥ ΑΝΝΑ",           "6977889900", "ΚΩΔ-52", 1, "Β1"},
        // Building 2 -- ΤΣΙΜΙΣΚΗ 81 (4)
        {"ΚΩΝΣΤΑΝΤΙΝΙΔΟΥ ΜΑΡΙΑ",      "6932345678", "Π-167",  2, "Ισόγειο"},
        {"ΧΡΙΣΤΟΔΟΥΛΟΥ ΠΑΝΑΓΙΩΤΗΣ",   "6955667788", "ΚΩΔ-88", 2, "1ος"},
        {"ΒΛΑΧΟΥ ΣΟΦΙΑ",              "6900112233", "Π-290",  2, "2ος"},
        {"ΟΙΚΟΝΟΜΟΥ ΒΑΣΙΛΕΙΟΣ",       "6936655443", "ΚΩΔ-64", 2, "3ος"},
        // Building 3 -- ΑΙΓΑΙΟΥ 9 (4)
        {"ΚΑΡΑΓΙΑΝΝΗΣ ΝΙΚΟΛΑΟΣ",       "6944556677", "ΚΩΔ-19", 3, "Α1"},
        {"ΔΗΜΗΤΡΙΟΥ ΕΥΑΓΓΕΛΙΑ",        "6933221144", "Π-402",  3, "Α2"},
        {"ΑΝΤΩΝΙΟΥ ΧΡΗΣΤΟΣ",          "6948899001", "ΚΩΔ-95", 3, "Β1"},
        {"ΠΕΤΡΟΥ ΔΗΜΗΤΡΑ",            "6922334455", "Π-377",  3, "Β2"},
        // Moved out (no apartment now): lived at ΠΑΤΗΣΙΩΝ 205 Β2 until June; ΜΑΚΡΗΣ moved in there in July.
        {"ΣΑΡΡΗΣ ΑΝΤΩΝΙΟΣ",           "6987001122", "Π-099", -1, ""},
        // Third tenant of ΤΣΙΜΙΣΚΗ 81 4ος (after ΒΛΑΧΟΥ Jan-Feb and ΚΩΝΣΤΑΝΤΙΝΙΔΟΥ Mar-May; empty in June).
        {"ΚΟΥΤΣΟΥΚΟΣ ΣΤΑΥΡΟΣ",        "6945001234", "Π-450",  2, "4ος"},
    };

    for (auto& su : seedUsers) {
        User u;
        u.full_name = su.name; u.phone = su.phone; u.special_code = su.special;
        u.apartment_id = su.bldg >= 0 ? apartmentId[su.bldg][su.apt] : -1;
        db->addUser(u);
    }

    std::vector<int> uid; // parallel to seedUsers, filled in by name lookup
    {
        auto all = db->searchUsers("", 0, 1000);
        for (auto& su : seedUsers) {
            int id = -1;
            for (auto& u : all) if (u.full_name == su.name) { id = u.id; break; }
            uid.push_back(id);
        }
    }
    if ((int)uid.size() < 16 || uid[0] < 0) return; // sanity guard, shouldn't happen

    // Apartment history BEFORE any payment: each payment stores the apartment its client lived in on its date,
    // so old payments stay with the old apartment and a new resident starts with a clean history.
    // -----------------------------------------------------------------
    // Apartment history. Everybody has lived in their apartment since January; four clients
    // moved during the year, so some apartments have had SEVERAL tenants and a few are empty
    // now. The payments' apartment snapshots are re-derived from these dates.
    // -----------------------------------------------------------------
    {
        auto aptOf = [&](int b, const char* label) { return apartmentId[b][label]; };
        auto stay = [&](int user, int apt, const char* from, const char* to) {
            Residency r; r.user_id = user; r.apartment_id = apt; r.from_date = from; r.to_date = to; return r; };
        for (size_t i = 0; i < seedUsers.size(); ++i) {
            std::vector<Residency> h;
            const std::string name = seedUsers[i].name;
            if (name == "ΣΑΡΡΗΣ ΑΝΤΩΝΙΟΣ") {           // moved out of the area: history only, no current apartment
                db->setResidencyHistory(uid[i], {stay(uid[i], aptOf(0, "Β2"), "2026-01-01", "2026-06-30")});
                continue;
            }
            const int nowApt = aptOf(seedUsers[i].bldg, seedUsers[i].apt);
            const char* from = "2026-01-01";
            if (name == "ΒΛΑΧΟΥ ΣΟΦΙΑ")            { h.push_back(stay(uid[i], aptOf(2, "4ος"), "2026-01-01", "2026-02-28")); from = "2026-03-01"; }
            if (name == "ΚΩΝΣΤΑΝΤΙΝΙΔΟΥ ΜΑΡΙΑ")    { h.push_back(stay(uid[i], aptOf(2, "Ισόγειο"), "2026-01-01", "2026-02-28"));   // upstairs while
                                                     h.push_back(stay(uid[i], aptOf(2, "4ος"), "2026-03-01", "2026-05-31")); from = "2026-06-01"; }   // the ground floor was renovated
            if (name == "ΜΙΧΑΗΛΙΔΟΥ ΑΝΝΑ")         { h.push_back(stay(uid[i], aptOf(1, "Β2"),  "2026-01-01", "2026-06-30")); from = "2026-07-01"; }
            // ΚΗΦΙΣΙΑΣ Β1 until June (then ΜΙΧΑΗΛΙΔΟΥ moved in there), ΠΑΤΗΣΙΩΝ Β2 from July (after ΣΑΡΡΗΣ left)
            if (name == "ΜΑΚΡΗΣ ΕΛΕΥΘΕΡΙΟΣ")        { h.push_back(stay(uid[i], aptOf(1, "Β1"),  "2026-01-01", "2026-06-30")); from = "2026-07-01"; }
            if (name == "ΚΟΥΤΣΟΥΚΟΣ ΣΤΑΥΡΟΣ")       { from = "2026-07-01"; }   // moved into 4ος in July
            if (name == "ΔΗΜΗΤΡΙΟΥ ΕΥΑΓΓΕΛΙΑ")     { h.push_back(stay(uid[i], aptOf(3, "Γ1"),  "2026-01-01", "2026-06-30")); from = "2026-07-01"; }
            h.push_back(stay(uid[i], nowApt, from, ""));
            db->setResidencyHistory(uid[i], h);
        }
    }


    // "Current" reference date for spreading past/future payments: 2026-09-06.
    std::vector<YM> pastMonths  = {{2026,2},{2026,3},{2026,4},{2026,5},{2026,6},{2026,7},{2026,8}};
    std::vector<YM> futureMonths = {{2026,9},{2026,10},{2026,11},{2026,12},{2027,1}};

    int payTotal = 0;

    // -----------------------------------------------------------------
    // Simple single-owner services (one per several users)
    // -----------------------------------------------------------------
    struct SimpleSvc { int ownerIdx; int id; std::string label; double amount; };
    std::vector<SimpleSvc> simple;
    std::map<std::string, int> idByLabel;
    {
        int n = 0;
        for (auto& c : kCatalog) {
            int owner = (n * 5 + 2) % 16;   // spread over all 16 clients (5 is coprime with 16)
            n++;
            Service s; s.label = c.label; s.extra_notes = c.notes; s.suggested_price = c.amount;
            int sid = db->addServiceGetId(-1, s);   // independent catalog service; clients are tied through payments
            if (sid >= 0) { simple.push_back({owner, sid, c.label, c.amount}); idByLabel[c.label] = sid; }
        }
    }

    // -----------------------------------------------------------------
    // Packages: one per category (its unique services as components), plus the
    // site's BASIC / ALL IN ONE / FULL PACK bundles.
    // -----------------------------------------------------------------
    {
        std::vector<int> pkgId;
        for (auto name : kPackages) pkgId.push_back(db->addMasterService(name));
        for (auto& c : kCatalog) {
            auto it = idByLabel.find(c.label);
            if (it != idByLabel.end() && pkgId[c.pkg] >= 0) db->addMasterServiceComponent(pkgId[c.pkg], it->second);
        }
        for (auto& b : kBundles) {
            int mid = db->addMasterService(b.name);
            if (mid < 0) continue;
            for (auto part : b.parts) {
                auto it = idByLabel.find(part);
                if (it != idByLabel.end()) db->addMasterServiceComponent(mid, it->second);
            }
        }
    }

    // -----------------------------------------------------------------
    // Payments: simple services -- past 'paid' rows + a couple of 'future' rows,
    // both directions, varied amounts, spanning several months.
    // -----------------------------------------------------------------
    for (size_t si = 0; si < simple.size(); ++si) {
        auto& sv = simple[si];
        int nPast = 3 + (int)(si % 3);      // 3..5 paid rows
        for (int i = 0; i < nPast; ++i) {
            YM ym = pastMonths[(i + si) % pastMonths.size()];
            Payment p;
            p.service_id = sv.id; p.user_id = uid[sv.ownerIdx];
            p.year = ym.y; p.month = ym.m; p.day = 3 + ((int)si * 2 + i * 5) % 25;
            p.amount_due = sv.amount + (double)((i * 3 + si) % 4) * 2.5;
            p.amount_paid = p.amount_due;
            p.balance = 0.0;
            p.status = "paid";
            p.direction = (si % 5 == 0) ? "out" : "in"; // most are collections, a few manager-paid-forward
            p.notes = (i == 0) ? "Τακτική εξόφληση" : "";
            SeedAddPayment(db, p); payTotal++;
        }
        // one future scheduled payment for most services
        if (si % 4 != 3) {
            YM ym = futureMonths[si % futureMonths.size()];
            Payment p;
            p.service_id = sv.id; p.user_id = uid[sv.ownerIdx];
            p.year = ym.y; p.month = ym.m; p.day = 5 + (int)(si * 3) % 20;
            p.amount_due = sv.amount + 5.0;
            p.amount_paid = 0.0;
            p.balance = p.amount_due;
            p.status = "future";
            p.direction = (si % 5 == 0) ? "out" : "in";
            p.resolution_date = "2026-1" + std::to_string(1 + (int)(si % 2)) + "-1" + std::to_string(1 + (int)(si % 8));
            p.notes = "";
            SeedAddPayment(db, p); payTotal++;
        }
    }

    // -----------------------------------------------------------------
    // Payments: package-component services -- add a few extra rows directly
    // on components already used above, tagged so it's clear packages don't
    // introduce a new payment dimension (still attach to the real service+user).
    // -----------------------------------------------------------------
    if (simple.size() > 0) {
        Payment p;
        p.service_id = simple[0].id; p.user_id = uid[simple[0].ownerIdx];
        p.year = 2026; p.month = 1; p.day = 20;
        p.amount_due = 45.0; p.amount_paid = 45.0; p.balance = 0.0;
        p.status = "paid"; p.direction = "in";
        p.notes = "Εξόφληση μέσω πακέτου συντήρησης";
        SeedAddPayment(db, p); payTotal++;
    }
    // Extra scattered payments so we comfortably clear 40-60 total and vary
    // amounts/dates further, split across a handful of users/services not yet
    // touched by an extra row.
    for (int k = 0; k < 8 && !simple.empty(); ++k) {
        auto& sv = simple[k % simple.size()];
        YM ym = pastMonths[(k * 3 + 1) % pastMonths.size()];
        Payment p;
        p.service_id = sv.id; p.user_id = uid[sv.ownerIdx];
        p.year = ym.y; p.month = ym.m; p.day = 2 + (k * 4) % 27;
        p.amount_due = sv.amount + (double)(k % 5) * 3.75;
        p.amount_paid = p.amount_due;
        p.balance = 0.0;
        p.status = "paid";
        p.direction = (k % 3 == 0) ? "out" : "in";
        p.notes = "";
        SeedAddPayment(db, p); payTotal++;
    }

    // -----------------------------------------------------------------
    // Package purchases: some clients bought a WHOLE package, others only PARTS of
    // it (2, 3, 4 ... services, not just one), so the Πακέτα screen has every case.
    // Every purchase is a normal standalone payment (client + service); nothing links
    // it to the package. {package, client, how many of its services}; a count equal to
    // the package's size = whole package. Components are rotated per client so the same
    // services are not always the ones chosen.
    // -----------------------------------------------------------------
    {
        struct Buy { int pkg, client, count; };
        const std::vector<Buy> plan = {
            // ΠΥΡΟΣΒΕΣΤΗΡΑΣ (4)
            {0, 3, 4}, {0, 7, 4}, {0, 5, 3}, {0, 11, 2}, {0, 14, 2},
            // ΔΙΑΧΕΙΡΙΣΗ ΠΟΛΥΚΑΤΟΙΚΙΑΣ (6)
            {1, 0, 6}, {1, 9, 6}, {1, 4, 5}, {1, 12, 4}, {1, 6, 3}, {1, 13, 3}, {1, 15, 2},
            // ΘΕΡΜΑΝΣΗ (3, incl. the shared oil the ΠΑΤΗΣΙΩΝ 205 residents already pay)
            {2, 0, 2}, {2, 1, 2}, {2, 2, 1},
            // ΚΑΥΣΤΗΡΑΣ – ΛΕΒΗΤΑΣ (4)
            {3, 5, 4}, {3, 8, 3}, {3, 10, 2}, {3, 2, 2},
            // ΑΠΟΛΥΜΑΝΣΗ – ΑΠΕΝΤΟΜΩΣΗ (3)
            {4, 6, 3}, {4, 12, 3}, {4, 1, 2}, {4, 9, 2},
            // ΑΠΟΦΡΑΞΕΙΣ (3)
            {5, 8, 3}, {5, 3, 2}, {5, 14, 2},
            // ΣΥΝΕΡΓΕΙΑ ΚΑΘΑΡΙΟΤΗΤΑΣ (3)
            {6, 1, 3}, {6, 10, 3}, {6, 7, 2},
        };
        int seq = 0;
        for (auto& b : plan) {
            std::vector<const CatSvc*> comps;
            for (auto& c : kCatalog) if (c.pkg == b.pkg) comps.push_back(&c);
            int M = (int)comps.size();
            for (int k = 0; k < b.count && k < M; ++k) {
                const CatSvc* c = comps[(k + b.client) % M];
                auto it = idByLabel.find(c->label);
                if (it == idByLabel.end()) continue;
                bool fut = (seq % 5 == 4);
                YM ym = fut ? futureMonths[seq % futureMonths.size()] : pastMonths[(seq + b.client) % pastMonths.size()];
                Payment p;
                p.service_id = it->second; p.user_id = uid[b.client];
                p.year = ym.y; p.month = ym.m; p.day = 2 + (seq * 3 + b.client) % 26;
                p.amount_due = c->amount;
                p.amount_paid = fut ? 0.0 : c->amount;
                p.balance = p.amount_due - p.amount_paid;
                p.status = fut ? "future" : "paid";
                p.direction = "in";
                if (fut) p.resolution_date = "2026-12-1" + std::to_string(1 + seq % 8);
                p.notes = std::string("Πακέτο: ") + kPackages[b.pkg];
                SeedAddPayment(db, p); payTotal++;
                seq++;
            }
        }
    }

    // Whole BASIC / FULL PACK bundles bought by a few clients, each purchase made at once: its lines form one
    // batch and (mostly) share ONE receipt. One purchase is left without a receipt on purpose
    // (Πληρωμές → «Μόνο χωρίς απόδειξη»).
    {
        struct BundleBuy { size_t bundle; int client; const char* method; const char* paper; };
        const std::vector<BundleBuy> bplan = { {0, 4, "cash", "1187"}, {0, 13, "bank", ""}, {2, 2, "", ""}, {2, 9, "card", ""} };
        int bseq = 0;
        for (auto& bb : bplan) {
            YM ym = pastMonths[(bseq + bb.client) % pastMonths.size()];
            int day = 4 + (bseq * 2 + bb.client) % 24;
            std::vector<Payment> lines;
            for (auto part : kBundles[bb.bundle].parts) {
                auto it = idByLabel.find(part);
                if (it == idByLabel.end()) continue;
                double amount = 0.0;
                for (auto& c : kCatalog) if (std::string(c.label) == part) amount = c.amount;
                Payment p;
                p.service_id = it->second; p.user_id = uid[bb.client];
                p.year = ym.y; p.month = ym.m; p.day = day;
                p.amount_due = amount; p.amount_paid = amount; p.balance = 0.0;
                p.status = "paid"; p.direction = "in";
                p.notes = std::string("Πακέτο: ") + kBundles[bb.bundle].name;
                char ts[32]; std::snprintf(ts, sizeof(ts), "%04d-%02d-%02d 11:%02d:00", ym.y, ym.m, day, 10 + bseq);
                p.created_at = ts;
                lines.push_back(p); payTotal++;
            }
            char d[16]; std::snprintf(d, sizeof(d), "%04d-%02d-%02d", ym.y, ym.m, day);
            Receipt r; r.user_id = uid[bb.client]; r.issue_date = d; r.method = bb.method; r.paper_ref = bb.paper;
            if (std::string(bb.method) == "bank") r.bank_ref = "ALPHA-TRX-55120931";
            db->addPaymentsWithReceipt(lines, kBundles[bb.bundle].name, *bb.method ? &r : nullptr);
            bseq++;
        }
    }

    // =================================================================================================
    // Κοινόχρηστα. Each joint charge splits ONE cost over the apartments of ONE building, among the residents
    // living there ON THE CHARGE'S DATE (from the residency history), so a resident who left keeps their old
    // shares and the new resident only gets the later ones. Shares are then paid (or not) like any charge,
    // each resident with their own payment and receipt.
    // =================================================================================================
    std::map<std::string, int> receiptIdOf;      // tag -> receipt id, for the attachments below
    std::map<std::string, int> chargeIdOf;       // tag -> charge id
    {
        auto cat = [&](const char* label, const char* notes, const char* kind) {
            Service sv; sv.label = label; sv.extra_notes = notes;
            int jid = db->addJointService(sv, kind);
            int svcId = -1;
            for (auto& j : db->getJointCatalog(true)) if (j.id == jid) svcId = j.service.id;
            return std::make_pair(jid, svcId);
        };
        auto oil   = cat("ΘΕΡΜΑΝΣΗ – ΠΕΤΡΕΛΑΙΟ ΚΟΙΝΟΧΡΗΣΤΟ", "Προμήθεια πετρελαίου για την κεντρική θέρμανση, κατανομή με χιλιοστά θέρμανσης.", "heating_oil");
        auto clean = cat("ΚΑΘΑΡΙΟΤΗΤΑ ΚΛΙΜΑΚΟΣ", "Μηνιαίος καθαρισμός κλιμακοστασίου, ισόποσα σε όλα τα διαμερίσματα.", "equal");
        auto power = cat("ΡΕΥΜΑ ΚΟΙΝΟΧΡΗΣΤΩΝ ΧΩΡΩΝ", "Λογαριασμός ΔΕΗ για φωτισμό κλιμακοστασίου και ανελκυστήρα.", "equal");
        auto lift  = cat("ΣΥΝΤΗΡΗΣΗ ΑΝΕΛΚΥΣΤΗΡΑ", "Μηνιαία συντήρηση ανελκυστήρα, κατανομή με χιλιοστά.", "millesimal");
        auto insur = cat("ΑΣΦΑΛΕΙΑ ΚΤΙΡΙΟΥ", "Ετήσιο ασφαλιστήριο κτιρίου (πυρός & αστικής ευθύνης), με χιλιοστά.", "millesimal");
        (void)power;

        // (apartment, resident) pairs of a building on a date; apartments with nobody then get an excluded row
        struct Row { int apt; int user; std::string label; };
        // strictly who had a recorded stay covering the date (no "before the first stay" fallback here)
        std::map<int, std::vector<Residency>> stays;
        for (int u : uid) stays[u] = db->getUserResidencies(u);
        auto livedOn = [&](int user, int apt, const std::string& date) {
            for (auto& r : stays[user]) if (r.apartment_id == apt && r.from_date <= date && (r.to_date.empty() || date <= r.to_date)) return true;
            return false;
        };
        auto residentsOn = [&](int b, const std::string& date) {
            std::vector<Row> rows;
            for (auto& [label, apt] : apartmentId[b]) {
                bool any = false;
                for (size_t i = 0; i < uid.size(); i++)
                    if (livedOn(uid[i], apt, date)) { rows.push_back({apt, uid[i], label}); any = true; }
                if (!any) rows.push_back({apt, -1, label});
            }
            return rows;
        };
        // weights: thousandths per apartment label (only for the kinds that use them)
        struct OpSpec {
            int b; std::pair<int,int> svc; const char* source; int sourceId; const char* kind; const char* label;
            const char* date; double total; std::map<std::string, double> weights; std::map<std::string, std::string> exclude;
            std::string params;
        };
        auto split = [](long long total, const std::vector<double>& w) {
            std::vector<long long> out(w.size(), 0); double sum = 0; for (double x : w) sum += x;
            long long given = 0;
            for (size_t i = 0; i < w.size(); i++) { out[i] = (long long)((double)total * w[i] / sum); given += out[i]; }
            for (size_t i = 0; given < total; i = (i + 1) % w.size()) if (w[i] > 0) { out[i]++; given++; }
            return out;
        };
        auto charge = [&](const OpSpec& o, const std::vector<JointLine>& lines) {
            auto rows = residentsOn(o.b, o.date);
            std::vector<JointShare> shares; std::vector<double> w; std::vector<size_t> inc;
            std::string wjson;
            for (auto& r : rows) {
                JointShare sh; sh.apartment_id = r.apt; sh.user_id = r.user;
                auto ex = o.exclude.find(r.label);
                if (r.user <= 0) { sh.included = false; sh.exclude_reason = "Κενό διαμέρισμα"; }
                else if (ex != o.exclude.end()) { sh.included = false; sh.exclude_reason = ex->second; }
                else {
                    double wt = 1.0;
                    if (auto it = o.weights.find(r.label); it != o.weights.end()) {
                        wt = it->second;
                        wjson += (wjson.empty() ? "" : ",") + std::string("\"") + std::to_string(r.apt) + "\":" + std::to_string((int)wt);
                    }
                    inc.push_back(shares.size()); w.push_back(wt);
                }
                shares.push_back(sh);
            }
            auto parts = split(ToCents(o.total), w);
            for (size_t i = 0; i < inc.size(); i++) shares[inc[i]].amount = FromCents(parts[i]);
            JointOp op; op.building_id = buildingIds[o.b]; op.source_type = o.source; op.source_id = o.sourceId; op.kind = o.kind;
            op.label = o.label; op.total = o.total; op.op_date = o.date;
            op.params_json = "{" + o.params + (o.weights.empty() ? "" : std::string(o.params.empty() ? "" : ",") + "\"weights\":{" + wjson + "}") + "}";
            return db->chargeJoint(op, lines, shares);
        };
        auto one = [](int svc, double total) { return std::vector<JointLine>{{svc, total}}; };

        std::map<std::string, double> patWeights  = {{"Ισόγειο", 150}, {"Α1", 210}, {"Α2", 190}, {"Β1", 220}, {"Β2", 230}};
        std::map<std::string, double> aigWeights  = {{"Α1", 120}, {"Α2", 95}, {"Β1", 110}, {"Β2", 80}, {"Γ1", 100}};
        std::map<std::string, double> tsimWeights = {{"Ισόγειο", 160}, {"1ος", 200}, {"2ος", 200}, {"3ος", 210}, {"4ος", 230}};
        std::map<std::string, double> kifWeights  = {{"Α1", 240}, {"Α2", 260}, {"Β1", 250}, {"Β2", 250}};

        std::vector<std::pair<std::string, int>> ops;   // tag -> op id
        auto add = [&](const std::string& tag, int id) { if (id > 0) ops.push_back({tag, id}); };
        // ΠΑΤΗΣΙΩΝ 205: heating oil in January (ΣΑΡΡΗΣ lives in Β2) and October (ΜΑΚΡΗΣ lives in Β2);
        // the ground floor has its own boiler.
        add("pat_oil_jan", charge({0, oil, "joint", oil.first, "heating_oil", "ΠΕΤΡΕΛΑΙΟ ΘΕΡΜΑΝΣΗΣ ΙΑΝ. 2026", "2026-01-15", 1500.00,
                                   patWeights, {{"Ισόγειο", "Αυτόνομη θέρμανση"}}, "\"litres\":1200,\"price_per_litre\":1.25,\"extra\":0"},
                                  one(oil.second, 1500.00)));
        add("pat_oil_oct", charge({0, oil, "joint", oil.first, "heating_oil", "ΠΕΤΡΕΛΑΙΟ ΘΕΡΜΑΝΣΗΣ ΟΚΤ. 2026", "2026-10-05", 1031.20,
                                   patWeights, {{"Ισόγειο", "Αυτόνομη θέρμανση"}}, "\"litres\":800,\"price_per_litre\":1.289,\"extra\":0"},
                                  one(oil.second, 1031.20)));
        // ΑΙΓΑΙΟΥ 9: heating oil in October (Γ1 empty since ΔΗΜΗΤΡΙΟΥ moved to Α2; Α2 has its own heating)
        add("aig_oil_oct", charge({3, oil, "joint", oil.first, "heating_oil", "ΠΕΤΡΕΛΑΙΟ ΘΕΡΜΑΝΣΗΣ ΟΚΤ. 2026", "2026-10-05", 1250.00,
                                   aigWeights, {{"Α2", "Αυτόνομη θέρμανση"}}, "\"litres\":1000,\"price_per_litre\":1.25,\"extra\":0"},
                                  one(oil.second, 1250.00)));
        // monthly stair cleaning in every building (equal split) -- March, June, September
        const char* months[][2] = {{"2026-03-01", "ΜΑΡ."}, {"2026-06-01", "ΙΟΥΝ."}, {"2026-09-01", "ΣΕΠΤ."}};
        const double cleanTotal[] = {80.00, 60.00, 75.00, 70.00};
        const char* bTag[] = {"pat", "kif", "tsim", "aig"};
        for (int b = 0; b < 4; b++)
            for (auto& mo : months)
                add(std::string(bTag[b]) + "_clean_" + mo[0], charge({b, clean, "joint", clean.first, "equal",
                        (std::string("ΚΑΘΑΡΙΟΤΗΤΑ ΚΛΙΜΑΚΟΣ ") + mo[1] + " 2026").c_str(), mo[0], cleanTotal[b], {}, {}, ""},
                        one(clean.second, cleanTotal[b])));
        // ΚΗΦΙΣΙΑΣ 118: elevator maintenance by thousandths
        add("kif_lift", charge({1, lift, "joint", lift.first, "millesimal", "ΣΥΝΤΗΡΗΣΗ ΑΝΕΛΚΥΣΤΗΡΑ Β' ΤΡΙΜ. 2026", "2026-04-10", 240.00,
                                kifWeights, {}, ""}, one(lift.second, 240.00)));
        // ΤΣΙΜΙΣΚΗ 81: yearly insurance in February (ΒΛΑΧΟΥ then lived in 4ος)
        add("tsim_ins", charge({2, insur, "joint", insur.first, "millesimal", "ΑΣΦΑΛΕΙΑ ΚΤΙΡΙΟΥ 2026", "2026-02-10", 720.00,
                                tsimWeights, {}, ""}, one(insur.second, 720.00)));
        // a PACKAGE charged as joint: ΠΥΡΟΣΒΕΣΤΗΡΑΣ (its services) on ΚΗΦΙΣΙΑΣ 118, one apartment excluded
        {
            int pkg = -1;
            for (auto& m : db->getMasterServices()) if (m.label == "ΠΥΡΟΣΒΕΣΤΗΡΑΣ") pkg = m.id;
            if (pkg > 0) {
                auto comps = db->getMasterServiceComponents(pkg);
                std::vector<double> w; for (auto& c : comps) w.push_back(c.suggested_price);
                auto parts = split(10500, w);                     // 105.00 € over the components by catalog price
                std::vector<JointLine> lines;
                for (size_t i = 0; i < comps.size(); i++) lines.push_back({comps[i].id, FromCents(parts[i])});
                add("kif_fire", charge({1, {-1, -1}, "package", pkg, "equal", "ΠΥΡΟΣΒΕΣΤΗΡΕΣ ΚΤΙΡΙΟΥ 2026", "2026-05-20", 105.00,
                                        {}, {{"Α2", "Έχει δικό του πυροσβεστήρα"}}, ""}, lines));
            }
        }
        // a plain SERVICE charged as joint: emergency drain unblocking on ΤΣΙΜΙΣΚΗ 81
        {
            auto it = idByLabel.find("ΑΠΟΦΡΑΞΗ – ΕΚΤΑΚΤΗ");
            if (it != idByLabel.end())
                add("tsim_drain", charge({2, {-1, -1}, "service", it->second, "equal", "ΕΚΤΑΚΤΗ ΑΠΟΦΡΑΞΗ ΚΕΝΤΡΙΚΗΣ ΑΠΟΧΕΤΕΥΣΗΣ", "2026-08-14", 140.00,
                                          {}, {}, ""}, one(it->second, 140.00)));
        }

        // ---- paying the shares: every case once -----------------------------------------------------
        // charges of an op for one client
        auto chargesOf = [&](const std::string& tag, int user) {
            std::vector<Payment> out;
            for (auto& [t, id] : ops) if (t == tag) for (int cid : db->getJointOpCharges(id)) {
                Payment p = db->getPaymentById(cid);
                if (p.user_id == user) out.push_back(p);
            }
            return out;
        };
        auto addDays = [](const std::string& date, int days) {   // good enough for the demo (stays inside the month)
            int d = std::atoi(date.substr(8, 2).c_str()) + days; if (d > 28) d = 28;
            char b[16]; std::snprintf(b, sizeof(b), "%s-%02d", date.substr(0, 7).c_str(), d); return std::string(b);
        };
        // pay `fraction` of a client's share(s) of the given ops with ONE payment; receipt if method given
        auto pay = [&](int user, std::vector<std::string> tags, double fraction, const char* method, const char* paper,
                       const std::string& receiptTag = "") {
            std::vector<Allocation> al; long long total = 0; std::string date;
            for (auto& tag : tags) for (auto& c : chargesOf(tag, user)) {
                long long amt = (long long)(ToCents(c.balance) * fraction);
                if (amt <= 0) continue;
                al.push_back({-1, c.id, FromCents(amt)}); total += amt;
                char d[16]; std::snprintf(d, sizeof(d), "%04d-%02d-%02d", c.year, c.month, c.day);
                if (std::string(d) > date) date = d;
            }
            if (total <= 0) return -1;
            MoneyPayment m; m.user_id = user; m.amount = FromCents(total); m.direction = "in";
            m.pay_date = addDays(date, 6); m.method = *method ? method : "cash";
            Receipt r; r.user_id = user; r.issue_date = m.pay_date; r.method = m.method; r.paper_ref = paper;
            if (m.method == "bank" || m.method == "ebanking") { r.bank_ref = "PIRAEUS-" + std::to_string(700000 + total % 99991); m.reference = r.bank_ref; }
            int mid = db->recordPayment(m, al, *method ? &r : nullptr);
            if (mid > 0 && !receiptTag.empty()) {
                for (auto& mm : db->getMoneyForUser(user)) if (mm.id == mid && mm.receipt_id > 0) receiptIdOf[receiptTag] = mm.receipt_id;
            }
            return mid;
        };
        auto U = [&](const char* name) { for (size_t i = 0; i < seedUsers.size(); i++) if (name == std::string(seedUsers[i].name)) return uid[i]; return -1; };

        // ΠΑΤΗΣΙΩΝ 205 January oil: most paid in full with a receipt each; ΣΑΡΡΗΣ (moved out in June) paid only
        // part and still owes the rest -- it stays HIS debt from apartment Β2, not the new resident's.
        pay(U("ΑΝΑΣΤΑΣΙΟΥ ΔΗΜΗΤΡΙΟΣ"), {"pat_oil_jan"}, 1.0, "cash", "1201", "pat_oil_cash");
        pay(U("ΣΤΑΥΡΟΥ ΑΙΚΑΤΕΡΙΝΗ"),   {"pat_oil_jan"}, 1.0, "bank", "");
        pay(U("ΓΕΩΡΓΙΟΥ ΙΩΑΝΝΗΣ"),     {"pat_oil_jan"}, 1.0, "ebanking", "");
        pay(U("ΣΑΡΡΗΣ ΑΝΤΩΝΙΟΣ"),      {"pat_oil_jan"}, 0.5, "", "");
        // ONE payment + ONE receipt covering two joint shares (March cleaning + January oil already paid -> cleaning
        // of March and June together)
        pay(U("ΑΝΑΣΤΑΣΙΟΥ ΔΗΜΗΤΡΙΟΣ"), {"pat_clean_2026-03-01", "pat_clean_2026-06-01"}, 1.0, "cash", "1305", "pat_two_shares");
        pay(U("ΠΑΠΑΔΟΠΟΥΛΟΣ ΓΕΩΡΓΙΟΣ"), {"pat_clean_2026-03-01"}, 1.0, "cash", "1306");
        // the receipt that gets voided and reissued (wrong method written)
        int wrong = pay(U("ΣΤΑΥΡΟΥ ΑΙΚΑΤΕΡΙΝΗ"), {"pat_clean_2026-03-01"}, 1.0, "cash", "1307");
        if (wrong > 0) {
            for (auto& mm : db->getMoneyForUser(U("ΣΤΑΥΡΟΥ ΑΙΚΑΤΕΡΙΝΗ"))) if (mm.id == wrong && mm.receipt_id > 0) {
                db->voidReceipt(mm.receipt_id, "Λάθος τρόπος πληρωμής: πληρώθηκε με κάρτα, όχι μετρητά");
                Receipt r; r.user_id = mm.user_id; r.issue_date = mm.pay_date; r.method = "card"; r.notes = "Αντικαθιστά την ακυρωμένη απόδειξη";
                db->issueReceiptForMoney(r, {mm.id});
            }
        }
        // ΚΗΦΙΣΙΑΣ 118: elevator + fire extinguishers. ΜΑΚΡΗΣ paid the elevator share of Β1 before moving away;
        // ΜΙΧΑΗΛΙΔΟΥ (then in Β2) paid hers without asking for a receipt.
        pay(U("ΝΙΚΟΛΑΟΥ ΕΛΕΝΗ"),   {"kif_lift", "kif_fire"}, 1.0, "bank", "", "kif_fire_bank");
        pay(U("ΜΑΚΡΗΣ ΕΛΕΥΘΕΡΙΟΣ"), {"kif_lift"}, 1.0, "cash", "1410");
        pay(U("ΜΙΧΑΗΛΙΔΟΥ ΑΝΝΑ"),  {"kif_lift", "kif_fire"}, 1.0, "", "");
        // ΤΣΙΜΙΣΚΗ 81: insurance (ΒΛΑΧΟΥ was in 4ος then) and the emergency drain job
        pay(U("ΒΛΑΧΟΥ ΣΟΦΙΑ"),             {"tsim_ins"}, 1.0, "iris", "");
        pay(U("ΧΡΙΣΤΟΔΟΥΛΟΥ ΠΑΝΑΓΙΩΤΗΣ"),  {"tsim_ins", "tsim_drain"}, 1.0, "cash", "1502");
        pay(U("ΟΙΚΟΝΟΜΟΥ ΒΑΣΙΛΕΙΟΣ"),      {"tsim_ins"}, 0.4, "", "");
        pay(U("ΚΩΝΣΤΑΝΤΙΝΙΔΟΥ ΜΑΡΙΑ"),     {"tsim_drain"}, 1.0, "collector", "1503", "tsim_drain_collector");
        // ΑΙΓΑΙΟΥ 9: September cleaning by everyone, October oil by one resident so far
        for (const char* n : {"ΚΑΡΑΓΙΑΝΝΗΣ ΝΙΚΟΛΑΟΣ", "ΔΗΜΗΤΡΙΟΥ ΕΥΑΓΓΕΛΙΑ", "ΑΝΤΩΝΙΟΥ ΧΡΗΣΤΟΣ", "ΠΕΤΡΟΥ ΔΗΜΗΤΡΑ"})
            pay(U(n), {"aig_clean_2026-09-01"}, 1.0, "cash", "");
        pay(U("ΚΑΡΑΓΙΑΝΝΗΣ ΝΙΚΟΛΑΟΣ"), {"aig_oil_oct"}, 1.0, "ebanking", "");

        // a normal service bill with an attached video (boiler fault) -- for the attachments
        auto it = idByLabel.find("ΚΑΥΣΤΗΡΑΣ – ΒΛΑΒΗ");
        if (it != idByLabel.end()) {
            Payment p; p.service_id = it->second; p.user_id = U("ΟΙΚΟΝΟΜΟΥ ΒΑΣΙΛΕΙΟΣ");
            p.year = 2026; p.month = 8; p.day = 14; p.amount_due = 85.00; p.amount_paid = 0.0; p.status = "paid"; p.direction = "in";
            p.notes = "Βλάβη καυστήρα διαμερίσματος – βίντεο και φωτογραφία στα συνημμένα";
            int cid = db->addPaymentsWithReceipt({p}, "", nullptr);
            if (cid > 0) chargeIdOf["boiler"] = cid;
        }
    }


    // ---- ΤΣΙΜΙΣΚΗ 81 4ος: its third tenant's own charges, and installment plans ----------------------------------
    {
        auto U = [&](const char* name) { for (size_t i = 0; i < seedUsers.size(); i++) if (name == std::string(seedUsers[i].name)) return uid[i]; return -1; };
        auto svc = [&](const char* label) { auto it = idByLabel.find(label); return it == idByLabel.end() ? -1 : it->second; };
        int kou = U("ΚΟΥΤΣΟΥΚΟΣ ΣΤΑΥΡΟΣ");
        auto line = [&](int user, int service, const char* date, double due, double paid, const char* note) {
            Payment p; p.user_id = user; p.service_id = service; std::sscanf(date, "%d-%d-%d", &p.year, &p.month, &p.day);
            p.amount_due = due; p.amount_paid = paid; p.status = "paid"; p.direction = "in"; p.notes = note;
            return p;
        };
        if (kou > 0) {
            Receipt r; r.user_id = kou; r.issue_date = "2026-07-05"; r.method = "cash"; r.paper_ref = "1601";
            db->addPaymentsWithReceipt({line(kou, svc("ΔΙΑΧΕΙΡΙΣΗ – ΜΗΝΙΑΙΑ ΑΜΟΙΒΗ"), "2026-07-05", 40.00, 40.00, "Ιούλιος")}, "", &r);
            db->addPaymentsWithReceipt({line(kou, svc("ΑΠΕΝΤΟΜΩΣΗ"), "2026-08-10", 30.00, 10.00, "Προκαταβολή 10 €, τα υπόλοιπα με τα κοινόχρηστα")}, "", nullptr);
            db->addPaymentsWithReceipt({line(kou, svc("ΔΙΑΧΕΙΡΙΣΗ – ΜΗΝΙΑΙΑ ΑΜΟΙΒΗ"), "2026-09-05", 40.00, 0.00, "Σεπτέμβριος – ανεξόφλητο")}, "", nullptr);
        }
        // installment plan still running: boiler service in 3 monthly installments, 1st paid, one receipt at the end
        auto plan = [&](int user, int service, double total, int n, const char* start, bool firstPaid, const char* label) {
            std::vector<Payment> lines;
            long long t = ToCents(total), each = t / n;
            int y = 0, m = 0, d = 0; std::sscanf(start, "%d-%d-%d", &y, &m, &d);
            for (int k = 0; k < n; k++) {
                int mm = m - 1 + k, yy = y + mm / 12; mm = mm % 12 + 1;
                char date[16]; std::snprintf(date, sizeof(date), "%04d-%02d-%02d", yy, mm, d);
                Payment p = line(user, service, date, FromCents(k == 0 ? t - each * (n - 1) : each), 0.0, "");
                bool paid = firstPaid && k == 0;
                p.amount_paid = paid ? p.amount_due : 0.0; p.status = paid ? "paid" : "future"; p.resolution_date = paid ? "" : date;
                p.notes = "Δόση " + std::to_string(k + 1) + "/" + std::to_string(n) + " · " + label;
                lines.push_back(p);
            }
            Receipt r; r.user_id = user; r.method = "cash";
            db->addInstallmentPlan(lines, label, n, "month", "end", firstPaid ? &r : nullptr);
            return lines;
        };
        if (kou > 0) plan(kou, svc("ΚΑΥΣΤΗΡΑΣ – ΣΥΝΤΗΡΗΣΗ ΠΕΤΡΕΛΑΙΟΥ"), 50.00, 3, "2026-09-01", true, "ΚΑΥΣΤΗΡΑΣ – ΣΥΝΤΗΡΗΣΗ ΠΕΤΡΕΛΑΙΟΥ");
        // a finished plan: all 3 installments paid -> the app issued ONE receipt for the whole 120.00 €
        int pap = U("ΠΑΠΑΔΟΠΟΥΛΟΣ ΓΕΩΡΓΙΟΣ");
        if (pap > 0) {
            plan(pap, svc("ΔΙΑΧΕΙΡΙΣΗ – ΠΛΗΡΩΜΗ ΛΟΓΑΡΙΑΣΜΩΝ"), 120.00, 3, "2026-05-15", true, "ΔΙΑΧΕΙΡΙΣΗ – ΠΛΗΡΩΜΗ ΛΟΓΑΡΙΑΣΜΩΝ");
            for (auto& c : db->getOpenCharges(pap)) {
                if (c.notes.find("Δόση 2/3 · ΔΙΑΧΕΙΡΙΣΗ – ΠΛΗΡΩΜΗ") == std::string::npos &&
                    c.notes.find("Δόση 3/3 · ΔΙΑΧΕΙΡΙΣΗ – ΠΛΗΡΩΜΗ") == std::string::npos) continue;
                MoneyPayment m; m.user_id = pap; m.amount = c.amount_due; m.method = "ebanking";
                char d[16]; std::snprintf(d, sizeof(d), "%04d-%02d-%02d", c.year, c.month, c.day); m.pay_date = d;
                db->recordPayment(m, {{-1, c.id, c.amount_due}}, nullptr);
            }
        }
    }

    // ---- offers: one for everyone, one for a single client, one ended, one cancelled ----
    {
        auto svc = [&](const char* label) { auto it = idByLabel.find(label); return it == idByLabel.end() ? -1 : it->second; };
        Offer a; a.name = "Φθινοπωρινή απεντόμωση"; a.kind = "percent"; a.value = 15; a.target_type = "service"; a.target_id = svc("ΑΠΕΝΤΟΜΩΣΗ");
        a.valid_from = "2026-10-01"; a.valid_to = "2026-11-15"; a.note = "Για όλους τους πελάτες";
        db->saveOffer(a);
        Offer b; b.name = "Πιστός πελάτης"; b.kind = "fixed"; b.value = 10; b.user_id = uid[0]; b.target_type = "all";
        b.valid_from = "2026-10-01"; b.valid_to = "2026-10-20"; db->saveOffer(b);
        Offer c; c.name = "Καλοκαιρινή προσφορά καυστήρα"; c.kind = "percent"; c.value = 20; c.target_type = "service";
        c.target_id = svc("ΚΑΥΣΤΗΡΑΣ – ΣΥΝΤΗΡΗΣΗ ΠΕΤΡΕΛΑΙΟΥ"); c.valid_from = "2026-06-01"; c.valid_to = "2026-08-31"; db->saveOffer(c);
        Offer d; d.name = "Διπλή καταχώριση"; d.kind = "percent"; d.value = 5; d.valid_from = "2026-10-01"; d.valid_to = "2026-12-31";
        int did = db->saveOffer(d); if (did > 0) db->cancelOffer(did, "Καταχωρίστηκε κατά λάθος δύο φορές");
    }

    // ---- attachments: real files copied next to the database (see docs/JOINT_CHARGES.md → «Συνημμένα») ----
    if (!mediaDir.empty()) {
        auto f = [&](const char* name) { return mediaDir + "/" + name; };
        auto userNamed = [&](const char* name) { for (size_t i = 0; i < seedUsers.size(); i++) if (name == std::string(seedUsers[i].name)) return uid[i]; return -1; };
        if (receiptIdOf.count("pat_two_shares"))
            db->addAttachment("receipt", receiptIdOf["pat_two_shares"], f("receipt_scan_A000001.jpg"), "Σκαναρισμένη χειρόγραφη απόδειξη");
        if (receiptIdOf.count("kif_fire_bank"))
            db->addAttachment("receipt", receiptIdOf["kif_fire_bank"], f("receipt_scan_A000002.jpg"), "Αποδεικτικό κατάθεσης");
        if (receiptIdOf.count("pat_oil_cash"))
            db->addAttachment("receipt", receiptIdOf["pat_oil_cash"], f("oil_delivery_note.jpg"), "Δελτίο παράδοσης πετρελαίου");
        if (chargeIdOf.count("boiler")) {
            db->addAttachment("charge", chargeIdOf["boiler"], f("boiler_inspection.mp4"), "Βίντεο από τον τεχνικό");
            db->addAttachment("charge", chargeIdOf["boiler"], f("boiler_damage.jpg"), "Φωτογραφία βλάβης");
        }
        int pap = userNamed("ΠΑΠΑΔΟΠΟΥΛΟΣ ΓΕΩΡΓΙΟΣ");
        if (pap > 0) {
            db->addAttachment("client", pap, f("management_contract.pdf"), "Σύμβαση διαχείρισης");
            db->addAttachment("client", pap, f("client_id_card.jpg"), "Αντίγραφο ταυτότητας");
        }
        db->takeLastError();   // a missing demo file is not an error worth showing
    }

    // Notes: deliberately NOT seeded -- the client wants the Notes tab to start
    // empty so the person actually using the app populates it themselves.

    (void)payTotal; // total payment rows inserted, kept for readability while authoring
}
