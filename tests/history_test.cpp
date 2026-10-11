// Payment <-> apartment history: who paid for which apartment survives moves and deletions.
#include "SqliteConn.h"
#include "Money.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } } while (0)

static int lastId(SqliteConn& db, const char* table) {
    std::vector<std::string> c; std::vector<std::vector<std::string>> rows; std::string err;
    db.executeRawQuery(std::string("SELECT MAX(id) FROM ") + table, c, rows, err);
    return std::atoi(rows[0][0].c_str());
}
static int pay(SqliteConn& db, int user, int svc, int y, int m, int d, int aptId = -1) {
    Payment p; p.user_id = user; p.service_id = svc; p.year = y; p.month = m; p.day = d;
    p.amount_due = 40; p.amount_paid = 40; p.status = "paid"; p.direction = "in"; p.apartment_id = aptId;
    db.addPayment(p);
    return lastId(db, "charges");
}

int main() {
    // own folder: the database, its WAL files and the audit checkpoints all go away together
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "bmanager_history_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::string path = (dir / "data.db").string();
    {
        SqliteConn db(path);
        Building b; b.address = "ΠΑΤΗΣΙΩΝ 205"; int bid = db.addBuilding(b);
        int a1 = db.addApartment(bid, "Α1"), a2 = db.addApartment(bid, "Β2");
        Service s; s.label = "ΚΟΙΝΟΧΡΗΣΤΑ"; s.suggested_price = 40; int svc = db.addServiceGetId(-1, s);

        User oldT; oldT.full_name = "ΠΑΛΙΟΣ ΕΝΟΙΚΟΣ"; db.addUser(oldT); int u1 = lastId(db, "users");
        User newT; newT.full_name = "ΝΕΟΣ ΕΝΟΙΚΟΣ";   db.addUser(newT); int u2 = lastId(db, "users");
        // u1 lived in Α1 Jan-Jun, then moved to Β2; u2 moved into Α1 in July
        db.setResidencyHistory(u1, {{-1, u1, a1, "2026-01-01", "2026-06-30"}, {-1, u1, a2, "2026-07-01", ""}});
        db.setResidencyHistory(u2, {{-1, u2, a1, "2026-07-01", ""}});

        int pOld  = pay(db, u1, svc, 2026, 3, 5);            // while in Α1
        int pNew  = pay(db, u2, svc, 2026, 8, 5);            // new resident of Α1
        int pU1B2 = pay(db, u1, svc, 2026, 8, 6);            // u1, now in Β2
        int pArr  = pay(db, u1, svc, 2026, 9, 1, a1);        // u1 settles Α1 arrears after moving: chosen by hand

        CHECK(db.getPaymentById(pOld).apartment_id == a1);
        CHECK(db.getPaymentById(pNew).apartment_id == a1);
        CHECK(db.getPaymentById(pU1B2).apartment_id == a2);
        CHECK(db.getPaymentById(pArr).apartment_id == a1);
        CHECK(db.getApartmentOnDate(u1, 2026, 3, 1) == a1 && db.getApartmentOnDate(u1, 2026, 9, 1) == a2);

        // Α1's ledger tells residents apart: 2 payments by the previous resident, 1 by the current
        int byOld = 0, byNew = 0;
        for (auto& p : db.getPaymentsForApartment(a1)) (p.user_id == u1 ? byOld : byNew)++;
        CHECK(byOld == 2 && byNew == 1);

        // list filter "client + type" (placeholders used to be bound out of order -> always 0 rows)
        CHECK(db.pagePayments("", "", u1, -1, "", "in", 0, 100).size() == 3);
        CHECK(db.countPayments("2026-08-01", "2026-12-31", u1, svc, "", "in") == 2);

        // history can't be erased by deleting things
        CHECK(!db.removeApartment(a1)); CHECK(!db.takeLastError().empty());
        CHECK(!db.removeBuilding(bid)); db.takeLastError();
        CHECK(!db.removeUser(u1));      db.takeLastError();
        CHECK(db.getPaymentById(pOld).apartment_id == a1);
        CHECK(db.getResidencies(a1).size() == 2);            // rollback kept the residencies too
        int a3 = db.addApartment(bid, "Γ3");
        CHECK(db.removeApartment(a3));                       // an apartment with no payments can go

        // a receipted payment's apartment is frozen
        Receipt r; r.user_id = u1;
        int rid = db.issueReceipt(r, {pOld});
        CHECK(rid > 0);
        Payment e = db.getPaymentById(pOld); e.apartment_id = a2;
        CHECK(!db.editPayment(pOld, e)); db.takeLastError();
        // re-deriving u1's history leaves the receipted payment alone instead of failing
        CHECK(db.setResidencyHistory(u1, {{-1, u1, a2, "2026-01-01", ""}}));
        CHECK(db.getPaymentById(pOld).apartment_id == a1);
    }
    std::filesystem::remove_all(dir);
    std::printf(fails ? "%d FAILED\n" : "all history tests passed\n", fails);
    return fails ? 1 : 0;
}
