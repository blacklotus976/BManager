#include "Seed.h"
#include <vector>
#include <string>

// Fresh seed data set (rewritten). Rules followed:
//  - Uses only DataConn methods (addUser/addServiceGetId/addService/addServiceUser/
//    addPayment/addNote/addMasterService/addMasterServiceComponent) so every insert
//    goes through the same audit-logging code path as normal app usage.
//  - status is only ever "paid" or "future" for new rows (no legacy "pending"/"overdue").
//  - resolved_at is never set here (only the app's Resolve action sets it).
//  - resolution_date is only ever set on status=="future" rows.
//  - direction covers both "in" (collect from client) and "out" (manager paid on
//    client's behalf) across the data.
struct SeedUser {
    std::string name, phone, address, area, postal, contract, special;
};

struct YM { int y, m; };

void seedDemoDataIfEmpty(DataConn* db) {
    if (db->getRowCount("users") > 0) return; // already has data

    // 12 users, realistic Greek names/addresses. 6 of them get a non-empty
    // special_code (a plausible legacy external reference scheme); the rest blank.
    std::vector<SeedUser> seedUsers = {
        {"ΠΑΠΑΔΟΠΟΥΛΟΣ ΓΕΩΡΓΙΟΣ", "6944123456", "28 ΟΚΤΩΒΡΙΟΥ 49Β", "ΑΓ.ΠΑΡΑΣΚΕΥΗ", "15341", "ΣΥΜΒ-2021-014", "Π-104"},
        {"ΝΙΚΟΛΑΟΥ ΕΛΕΝΗ", "6971234567", "ΚΗΦΙΣΙΑΣ 112", "ΜΑΡΟΥΣΙ", "15125", "ΣΥΜΒ-2019-087", ""},
        {"ΑΝΑΣΤΑΣΙΟΥ ΔΗΜΗΤΡΙΟΣ", "2106547890", "ΠΑΤΗΣΙΩΝ 203", "ΑΘΗΝΑ", "11253", "ΣΥΜΒ-2022-041", "ΚΩΔ-77"},
        {"ΚΩΝΣΤΑΝΤΙΝΙΔΟΥ ΜΑΡΙΑ", "6932345678", "ΤΣΙΜΙΣΚΗ 77", "ΘΕΣΣΑΛΟΝΙΚΗ", "54622", "ΣΥΜΒ-2020-103", ""},
        {"ΓΕΩΡΓΙΟΥ ΙΩΑΝΝΗΣ", "6981122334", "ΒΑΣ. ΣΟΦΙΑΣ 15", "ΓΛΥΦΑΔΑ", "16674", "ΣΥΜΒ-2023-009", "Π-211"},
        {"ΣΤΑΥΡΟΥ ΑΙΚΑΤΕΡΙΝΗ", "2109988776", "ΕΡΜΟΥ 34", "ΠΕΡΙΣΤΕΡΙ", "12131", "ΣΥΜΒ-2018-055", ""},
        {"ΧΡΙΣΤΟΔΟΥΛΟΥ ΠΑΝΑΓΙΩΤΗΣ", "6955667788", "ΜΕΣΟΓΕΙΩΝ 260", "ΧΟΛΑΡΓΟΣ", "15561", "ΣΥΜΒ-2021-072", "ΚΩΔ-88"},
        {"ΒΛΑΧΟΥ ΣΟΦΙΑ", "6900112233", "ΑΓ.ΚΩΝΣΤΑΝΤΙΝΟΥ 8", "ΝΕΑ ΣΜΥΡΝΗ", "17121", "ΣΥΜΒ-2024-003", ""},
        {"ΠΑΠΠΑΣ ΘΕΟΔΩΡΟΣ", "6912345098", "ΛΕΩΦ. ΣΥΓΓΡΟΥ 145", "ΚΑΛΛΙΘΕΑ", "17671", "ΣΥΜΒ-2022-118", "Π-305"},
        {"ΜΙΧΑΗΛΙΔΟΥ ΑΝΝΑ", "6977889900", "ΑΓΙΟΥ ΔΗΜΗΤΡΙΟΥ 22", "ΑΓ.ΔΗΜΗΤΡΙΟΣ", "17342", "ΣΥΜΒ-2020-066", ""},
        {"ΚΑΡΑΓΙΑΝΝΗΣ ΝΙΚΟΛΑΟΣ", "6944556677", "ΑΙΓΑΙΟΥ ΠΕΛΑΓΟΥΣ 9", "ΝΕΟ ΨΥΧΙΚΟ", "15451", "ΣΥΜΒ-2023-051", "ΚΩΔ-19"},
        {"ΔΗΜΗΤΡΙΟΥ ΕΥΑΓΓΕΛΙΑ", "6933221144", "ΠΛΑΤΕΙΑΣ ΝΙΚΗΣ 5", "ΠΕΙΡΑΙΑΣ", "18531", "ΣΥΜΒ-2019-029", "Π-142"},
    };

    std::vector<int> uid; // parallel to seedUsers, filled in as we insert
    for (auto& su : seedUsers) {
        User u;
        u.full_name = su.name; u.phone = su.phone; u.address = su.address;
        u.area = su.area; u.postal_code = su.postal; u.contract_code = su.contract;
        u.special_code = su.special;
        db->addUser(u);
    }
    {
        // Re-fetch everyone once, in insertion order (ids ascending since the table
        // was empty), so we have real ids to work with for services/payments below.
        auto all = db->searchUsers("", 0, 100);
        for (auto& su : seedUsers) {
            int id = -1;
            for (auto& u : all) if (u.full_name == su.name) { id = u.id; break; }
            uid.push_back(id);
        }
    }
    if ((int)uid.size() < 12 || uid[0] < 0) return; // sanity guard, shouldn't happen

    // "Current" reference date for spreading past/future payments: 2026-09-06.
    std::vector<YM> pastMonths  = {{2026,2},{2026,3},{2026,4},{2026,5},{2026,6},{2026,7},{2026,8}};
    std::vector<YM> futureMonths = {{2026,9},{2026,10},{2026,11},{2026,12},{2027,1}};

    int payTotal = 0;

    // -----------------------------------------------------------------
    // Simple single-owner services (one per several users)
    // -----------------------------------------------------------------
    struct SimpleSvc { int ownerIdx; int id; std::string label; double amount; };
    std::vector<SimpleSvc> simple;
    {
        struct Def { int ownerIdx; const char* label; const char* notes; double amount; };
        std::vector<Def> defs = {
            {0, "ΔΙΑΧΕΙΡΙΣΗ ΠΟΛΥΚΑΤΟΙΚΙΑΣ", "Μηνιαία αμοιβή διαχειριστή", 45.0},
            {1, "ΚΑΘΑΡΙΟΤΗΤΑ ΚΟΙΝΟΧΡΗΣΤΩΝ", "Καθαρισμός κλιμακοστασίου", 60.0},
            {3, "ΣΥΝΤΗΡΗΣΗ ΑΝΕΛΚΥΣΤΗΡΑ", "Τακτικό service ανελκυστήρα", 75.0},
            {6, "ΚΗΠΟΥΡΙΚΕΣ ΕΡΓΑΣΙΕΣ", "Φροντίδα κοινόχρηστου κήπου", 35.0},
            {7, "ΑΠΕΝΤΟΜΩΣΗ", "Ετήσια απεντόμωση κτιρίου", 90.0},
            {8, "ΕΛΕΓΧΟΣ ΠΥΡΟΣΒΕΣΤΗΡΩΝ", "Ετήσιος έλεγχος/αναγόμωση", 55.0},
            {9, "ΚΑΘΑΡΙΣΜΟΣ ΤΖΑΜΙΩΝ", "Καθαρισμός εξωτερικών τζαμιών", 40.0},
            {10, "ΣΥΝΤΗΡΗΣΗ ΚΗΠΟΥ ΠΡΟΣΟΨΗΣ", "Φύτευση/κλάδεμα", 50.0},
        };
        for (auto& d : defs) {
            Service s; s.label = d.label; s.extra_notes = d.notes;
            int sid = db->addServiceGetId(uid[d.ownerIdx], s);
            if (sid >= 0) simple.push_back({d.ownerIdx, sid, d.label, d.amount});
        }
    }

    // -----------------------------------------------------------------
    // Joint/shared services -- at least 2, different (not identical) subsets.
    // -----------------------------------------------------------------
    struct JointSvc { std::vector<int> ownerIdxs; int id; std::string label; double shareBase; };
    std::vector<JointSvc> joints;
    {
        // Building A: owners 0,2,4,5 share heating oil.
        Service heating; heating.label = "ΠΕΤΡΕΛΑΙΟ (ΚΟΙΝΟΧΡΗΣΤΟ) - ΚΤΙΡΙΟ Α";
        heating.extra_notes = "Κοινή παραγγελία πετρελαίου θέρμανσης, μοιρασμένη ανά χιλιοστά.";
        std::vector<int> ownersA = {0, 2, 4, 5};
        int hid = db->addServiceGetId(uid[ownersA[0]], heating);
        if (hid >= 0) {
            for (size_t i = 1; i < ownersA.size(); ++i) db->addServiceUser(hid, uid[ownersA[i]]);
            joints.push_back({ownersA, hid, heating.label, 300.0});
        }

        // Building B: owners 1,3,6,8,9 share building insurance (overlapping subset,
        // not identical to the heating-oil group).
        Service insurance; insurance.label = "ΑΣΦΑΛΙΣΗ ΚΤΙΡΙΟΥ - ΚΤΙΡΙΟ Β";
        insurance.extra_notes = "Ετήσιο ασφαλιστήριο συμβόλαιο κτιρίου, μοιρασμένο ανά διαμέρισμα.";
        std::vector<int> ownersB = {1, 3, 6, 8, 9};
        int iid = db->addServiceGetId(uid[ownersB[0]], insurance);
        if (iid >= 0) {
            for (size_t i = 1; i < ownersB.size(); ++i) db->addServiceUser(iid, uid[ownersB[i]]);
            joints.push_back({ownersB, iid, insurance.label, 180.0});
        }
    }

    // -----------------------------------------------------------------
    // Master/package services -- at least 2, grouping 2-3 real components each.
    // -----------------------------------------------------------------
    {
        int pkg1 = db->addMasterService("Πακέτο Συντήρησης Πολυκατοικίας");
        if (pkg1 >= 0) {
            // ΔΙΑΧΕΙΡΙΣΗ (simple[0]), ΣΥΝΤΗΡΗΣΗ ΑΝΕΛΚΥΣΤΗΡΑ (simple[2]), ΚΑΘΑΡΙΟΤΗΤΑ (simple[1])
            if (simple.size() > 0) db->addMasterServiceComponent(pkg1, simple[0].id);
            if (simple.size() > 2) db->addMasterServiceComponent(pkg1, simple[2].id);
            if (simple.size() > 1) db->addMasterServiceComponent(pkg1, simple[1].id);
        }
        int pkg2 = db->addMasterService("Πακέτο Ασφάλισης & Πυροπροστασίας");
        if (pkg2 >= 0) {
            // ΑΣΦΑΛΙΣΗ ΚΤΙΡΙΟΥ (joint[1]), ΕΛΕΓΧΟΣ ΠΥΡΟΣΒΕΣΤΗΡΩΝ (simple[5])
            if (joints.size() > 1) db->addMasterServiceComponent(pkg2, joints[1].id);
            if (simple.size() > 5) db->addMasterServiceComponent(pkg2, simple[5].id);
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
            db->addPayment(p); payTotal++;
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
            db->addPayment(p); payTotal++;
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
            db->addPayment(paid); payTotal++;

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
                db->addPayment(collect); payTotal++;
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
                db->addPayment(fut); payTotal++;
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
        db->addPayment(p); payTotal++;
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
        db->addPayment(p); payTotal++;
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
        db->addPayment(p); payTotal++;
    }

    // Notes: deliberately NOT seeded -- the client wants the Notes tab to start
    // empty so the person actually using the app populates it themselves.

    (void)payTotal; // total payment rows inserted, kept for readability while authoring
}
