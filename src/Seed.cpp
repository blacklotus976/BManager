#include "Seed.h"
#include <vector>
#include <string>
#include <map>

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

void seedDemoDataIfEmpty(DataConn* db) {
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
    for (size_t b = 0; b < seedBuildings.size(); ++b) {
        Building bld;
        bld.address = seedBuildings[b].address; bld.area = seedBuildings[b].area;
        bld.postal_code = seedBuildings[b].postal; bld.afm = seedBuildings[b].afm;
        int bid = db->addBuilding(bld);
        if (bid < 0) return; // sanity guard, shouldn't happen
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
    };

    for (auto& su : seedUsers) {
        User u;
        u.full_name = su.name; u.phone = su.phone; u.special_code = su.special;
        u.apartment_id = apartmentId[su.bldg][su.apt];
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
    // Joint/shared services -- at least 2, different (not identical) subsets.
    // -----------------------------------------------------------------
    struct JointSvc { std::vector<int> ownerIdxs; int id; std::string label; double shareBase; };
    std::vector<JointSvc> joints;
    {
        // ΠΑΤΗΣΙΩΝ 205: all five residents share the building's heating oil.
        Service heating; heating.label = "ΘΕΡΜΑΝΣΗ – ΠΡΟΜΗΘΕΙΑ ΠΕΤΡΕΛΑΙΟΥ - ΠΑΤΗΣΙΩΝ 205";
        heating.suggested_price = 300.0;
        heating.extra_notes = "Κοινή παραγγελία πετρελαίου θέρμανσης, μοιρασμένη ανά χιλιοστά.";
        std::vector<int> ownersA = {0, 1, 2, 3, 4};
        int hid = db->addServiceGetId(uid[ownersA[0]], heating);
        if (hid >= 0) {
            for (size_t i = 1; i < ownersA.size(); ++i) db->addServiceUser(hid, uid[ownersA[i]]);
            joints.push_back({ownersA, hid, heating.label, 300.0});
        }

        // ΤΣΙΜΙΣΚΗ 81: its four residents share the building insurance.
        Service insurance; insurance.label = "ΑΣΦΑΛΕΙΑ ΑΣΤΙΚΗΣ ΕΥΘΥΝΗΣ ΚΤΙΡΙΟΥ - ΤΣΙΜΙΣΚΗ 81";
        insurance.suggested_price = 180.0;
        insurance.extra_notes = "Ετήσιο ασφαλιστήριο συμβόλαιο κτιρίου, μοιρασμένο ανά διαμέρισμα.";
        std::vector<int> ownersB = {8, 9, 10, 11};
        int iid = db->addServiceGetId(uid[ownersB[0]], insurance);
        if (iid >= 0) {
            for (size_t i = 1; i < ownersB.size(); ++i) db->addServiceUser(iid, uid[ownersB[i]]);
            joints.push_back({ownersB, iid, insurance.label, 180.0});
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
        if (!joints.empty() && pkgId[2] >= 0) db->addMasterServiceComponent(pkgId[2], joints[0].id); // heating oil
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
    // Payments: joint services -- each owner pays their own share, mix of
    // paid/future and in/out, uneven amounts.
    // -----------------------------------------------------------------
    for (size_t ji = 0; ji < joints.size(); ++ji) {
        auto& j = joints[ji];
        double shares[] = {320.0, 275.5, 410.0, 190.25, 260.75};
        for (size_t oi = 0; oi < j.ownerIdxs.size(); ++oi) {
            int owner = uid[j.ownerIdxs[oi]];
            double share = shares[(oi + ji) % 5];

            // Past paid installment
            YM ym = pastMonths[(oi + ji * 2) % pastMonths.size()];
            Payment paid;
            paid.service_id = j.id; paid.user_id = owner;
            paid.year = ym.y; paid.month = ym.m; paid.day = 8 + (int)oi * 3;
            paid.amount_due = share;
            paid.amount_paid = share;
            paid.balance = 0.0;
            paid.status = "paid";
            paid.direction = (ji == 0) ? "out" : "in"; // heating oil: manager fronts it; insurance: collected directly
            paid.notes = "Μερίδιο " + j.label;
            SeedAddPayment(db, paid); payTotal++;

            // For the heating-oil (direction=out) service, add a matching "in" collection
            // row for about half the owners so the net trends toward zero for some.
            if (ji == 0 && (oi % 2 == 0)) {
                Payment collect;
                collect.service_id = j.id; collect.user_id = owner;
                YM ym2 = pastMonths[(oi + ji * 2 + 1) % pastMonths.size()];
                collect.year = ym2.y; collect.month = ym2.m; collect.day = 15 + (int)oi;
                collect.amount_due = share;
                collect.amount_paid = share * 0.7;
                collect.balance = share * 0.3;
                collect.status = "paid";
                collect.direction = "in";
                collect.notes = "Είσπραξη μεριδίου πετρελαίου";
                SeedAddPayment(db, collect); payTotal++;
            }

            // One future installment per owner for the first joint service
            if (ji == 0) {
                YM ymF = futureMonths[oi % futureMonths.size()];
                Payment fut;
                fut.service_id = j.id; fut.user_id = owner;
                fut.year = ymF.y; fut.month = ymF.m; fut.day = 10 + (int)oi * 2;
                fut.amount_due = 150.0 + (double)oi * 10.0;
                fut.amount_paid = 0.0;
                fut.balance = fut.amount_due;
                fut.status = "future";
                fut.direction = "out";
                fut.resolution_date = "2026-12-2" + std::to_string(1 + (oi % 8));
                fut.notes = "Επιπλέον δόση πετρελαίου";
                SeedAddPayment(db, fut); payTotal++;
            }
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
    if (joints.size() > 1) {
        Payment p;
        p.service_id = joints[1].id; p.user_id = uid[joints[1].ownerIdxs[0]];
        YM ym = futureMonths[2 % futureMonths.size()];
        p.year = ym.y; p.month = ym.m; p.day = 18;
        p.amount_due = 200.0; p.amount_paid = 0.0; p.balance = 200.0;
        p.status = "future"; p.direction = "in";
        p.resolution_date = "2027-01-05";
        p.notes = "Ανανέωση ασφαλιστηρίου";
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

    // Whole BASIC / FULL PACK bundles bought by a few clients (the category packages
    // above already give partial buyers of the bundles through their overlap).
    {
        struct BundleBuy { size_t bundle; int client; };
        const std::vector<BundleBuy> bplan = { {0, 4}, {0, 13}, {2, 2}, {2, 9} };
        int bseq = 0;
        for (auto& bb : bplan) {
            for (auto part : kBundles[bb.bundle].parts) {
                auto it = idByLabel.find(part);
                if (it == idByLabel.end()) continue;
                double amount = 0.0;
                for (auto& c : kCatalog) if (std::string(c.label) == part) amount = c.amount;
                YM ym = pastMonths[(bseq + bb.client) % pastMonths.size()];
                Payment p;
                p.service_id = it->second; p.user_id = uid[bb.client];
                p.year = ym.y; p.month = ym.m; p.day = 4 + (bseq * 2 + bb.client) % 24;
                p.amount_due = amount; p.amount_paid = amount; p.balance = 0.0;
                p.status = "paid"; p.direction = "in";
                p.notes = std::string("Πακέτο: ") + kBundles[bb.bundle].name;
                SeedAddPayment(db, p); payTotal++;
                bseq++;
            }
        }
    }

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
            const int nowApt = aptOf(seedUsers[i].bldg, seedUsers[i].apt);
            const char* from = "2026-01-01";
            if (name == "ΒΛΑΧΟΥ ΣΟΦΙΑ")            { h.push_back(stay(uid[i], aptOf(2, "4ος"), "2026-01-01", "2026-02-28")); from = "2026-03-01"; }
            if (name == "ΚΩΝΣΤΑΝΤΙΝΙΔΟΥ ΜΑΡΙΑ")    { h.push_back(stay(uid[i], aptOf(2, "4ος"), "2026-03-01", "2026-05-31")); from = "2026-06-01"; }
            if (name == "ΜΙΧΑΗΛΙΔΟΥ ΑΝΝΑ")         { h.push_back(stay(uid[i], aptOf(1, "Β2"),  "2026-01-01", "2026-04-30")); from = "2026-05-01"; }
            if (name == "ΔΗΜΗΤΡΙΟΥ ΕΥΑΓΓΕΛΙΑ")     { h.push_back(stay(uid[i], aptOf(3, "Γ1"),  "2026-01-01", "2026-06-30")); from = "2026-07-01"; }
            h.push_back(stay(uid[i], nowApt, from, ""));
            db->setResidencyHistory(uid[i], h);
        }
    }

    // Notes: deliberately NOT seeded -- the client wants the Notes tab to start
    // empty so the person actually using the app populates it themselves.

    (void)payTotal; // total payment rows inserted, kept for readability while authoring
}
