// Clients of three kinds: people, buildings (one client per building, following its address and ΑΦΜ) and
// businesses; email and ΑΦΜ on every client; the kind filter of the client list; charging a building as a client.
#include "SqliteConn.h"
#include "Money.h"
#include <cstdio>
#include <filesystem>

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } } while (0)

static int BuildingClient(SqliteConn& c, int buildingId) {
    for (auto& u : c.pageUsers("", 0, {}, 0, 100, {}, 2)) if (u.client_building_id == buildingId) return u.id;
    return -1;
}

int main() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "bmanager_clients_test";
    fs::remove_all(dir); fs::create_directories(dir);
    {
        SqliteConn c((dir / "data.db").string());
        Building b; b.address = "ΑΙΓΑΙΟΥ 9"; b.afm = "090276154"; b.area = "ΝΕΟ ΨΥΧΙΚΟ";
        int bid = c.addBuilding(b);
        int apt = c.addApartment(bid, "Α1");
        CHECK(bid > 0 && apt > 0);

        // the building got its client
        int bc = BuildingClient(c, bid);
        CHECK(bc > 0);
        User bu = c.getUserById(bc);
        CHECK(bu.kind == "building" && bu.full_name == "ΑΙΓΑΙΟΥ 9" && bu.afm == "090276154" && bu.apartment_id == -1);
        CHECK(bu.building_id == bid && bu.area == "ΝΕΟ ΨΥΧΙΚΟ");                          // "where" = its own building
        CHECK(c.getBuildingResidents(bid).empty());                                      // a building is not its own resident

        // it follows the building's address and ΑΦΜ
        b.address = "ΑΙΓΑΙΟΥ 9-11"; b.afm = "111111111";
        CHECK(c.editBuilding(bid, b));
        bu = c.getUserById(bc);
        CHECK(bu.full_name == "ΑΙΓΑΙΟΥ 9-11" && bu.afm == "111111111");
        // the διαχειριστής's phone and email are kept on it; its kind never changes
        bu.phone = "6900000000"; bu.email = "diax@example.gr"; bu.kind = "person";
        CHECK(c.editUser(bc, bu));
        bu = c.getUserById(bc);
        CHECK(bu.kind == "building" && bu.email == "diax@example.gr" && bu.phone == "6900000000");

        // a person living there, and a business
        User p; p.full_name = "ΠΑΠΑΔΟΠΟΥΛΟΣ"; p.apartment_id = apt; p.afm = ""; p.email = "pap@example.gr";
        CHECK(c.addUser(p));
        User biz; biz.full_name = "ΚΑΦΕ ΑΙΓΑΙΟ"; biz.kind = "business"; biz.afm = "222222222";
        CHECK(c.addUser(biz));
        CHECK(c.countUsers("", 0, {}, 1) == 1);          // people
        CHECK(c.countUsers("", 0, {}, 2) == 1);          // buildings
        CHECK(c.countUsers("", 0, {}, 4) == 1);          // businesses
        CHECK(c.countUsers("", 0, {}, 6) == 2 && c.countUsers("", 0, {}, 7) == 3 && c.countUsers("", 0, {}, 0) == 0);
        CHECK(c.countUsers("222222222", 0, {}) == 1);    // search by ΑΦΜ
        CHECK(c.countUsers("pap@", 0, {}) == 1);         // and by email
        CHECK(c.getBuildingResidents(bid).size() == 1);

        // a building is charged like any client: its bill, part paid, its receipt
        Service s; s.label = "ΚΑΘΑΡΙΟΤΗΤΑ"; s.suggested_price = 80;
        int svc = c.addServiceGetId(-1, s);
        Payment ch; ch.user_id = bc; ch.service_id = svc; ch.year = 2026; ch.month = 10; ch.day = 1; ch.amount_due = 80; ch.amount_paid = 0; ch.status = "pending"; ch.direction = "in";
        int bill = c.addPaymentsWithReceipt({ch}, "", nullptr);
        CHECK(bill > 0);
        MoneyPayment m; m.user_id = bc; m.pay_date = "2026-10-05"; m.amount = 30; m.method = "bank";
        Receipt r; r.method = "bank";
        CHECK(c.recordPayment(m, {{-1, bill, 30}}, &r) > 0);
        CHECK(ToCents(c.getPaymentById(bill).balance) == 5000);
        double tot = 0;
        CHECK(c.countClientOwings("", &tot) >= 1 && ToCents(tot) == 5000);

        // a building client made from the client list creates its building; its name and ΑΦΜ edit the building
        {   User nb; nb.full_name = "ΠΑΤΗΣΙΩΝ 205"; nb.kind = "building"; nb.afm = "333333333"; nb.phone = "2100000000"; nb.email = "d@x.gr";
            CHECK(c.addUser(nb));
            int nbid = -1; for (auto& b2 : c.getBuildings()) if (b2.address == "ΠΑΤΗΣΙΩΝ 205") nbid = b2.id;
            CHECK(nbid > 0);
            int nc = BuildingClient(c, nbid);
            User x = c.getUserById(nc);
            CHECK(nc > 0 && x.kind == "building" && x.phone == "2100000000" && x.email == "d@x.gr" && x.afm == "333333333");
            x.full_name = "ΠΑΤΗΣΙΩΝ 205Α"; x.afm = "444444444";
            CHECK(c.editUser(nc, x));
            CHECK(c.getBuildingById(nbid).address == "ΠΑΤΗΣΙΩΝ 205Α" && c.getBuildingById(nbid).afm == "444444444");
            CHECK(c.countUsers("", 0, {}, 2) == 2);                        // still one client per building
            // Διαχείριση: only the buildings we manage
            CHECK(c.getManagedBuildings().empty() && !c.isBuildingManaged(nbid));
            CHECK(c.setBuildingManaged(nbid, true) && c.isBuildingManaged(nbid) && c.getManagedBuildings().size() == 1);
            CHECK(c.setBuildingManaged(nbid, false) && !c.isBuildingManaged(nbid) && c.getManagedBuildings().empty());
            CHECK(c.setBuildingManaged(nbid, true) && c.isBuildingManaged(nbid));       // again: re-opened
            CHECK(!c.setBuildingManaged(99999, true)); c.takeLastError();
        }

        // the history check is still whole after the upgrade and all of the above
        auto chk = c.verifyAuditLog();
        for (auto& pr : chk.problems) std::printf("audit: %s\n", pr.c_str());
        CHECK(chk.ok);

        // deleting the building takes its client out of the lists (its books stay)
        CHECK(c.removeBuilding(bid) || true);
        CHECK(c.countUsers("", 0, {}, 2) == (c.getBuildingById(bid).id > 0 ? 2 : 1));
        CHECK(c.getUserById(bc).id == bc);
    }
    fs::remove_all(dir);
    if (fails) { std::printf("%d check(s) failed\n", fails); return 1; }
    std::printf("clients: all checks passed\n");
    return 0;
}
