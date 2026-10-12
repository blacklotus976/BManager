// Offers: matching (client / service / package / dates), editing, cancelling with a reason, uses recorded with charges.
#include "SqliteConn.h"
#include "Money.h"
#include <cstdio>
#include <filesystem>

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } } while (0)

int main() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "bmanager_offers_test";
    fs::remove_all(dir); fs::create_directories(dir);
    {
        SqliteConn c((dir / "data.db").string());
        User a; a.full_name = "Α"; c.addUser(a); User b; b.full_name = "Β"; c.addUser(b);
        Service s; s.label = "ΑΠΕΝΤΟΜΩΣΗ"; s.suggested_price = 40; int svc = c.addServiceGetId(-1, s);
        s.label = "ΑΛΛΗ"; int other = c.addServiceGetId(-1, s);
        int pkg = c.addMasterService("ΠΑΚΕΤΟ"); c.addMasterServiceComponent(pkg, svc);

        Offer all; all.name = "Όλοι 10%"; all.kind = "percent"; all.value = 10; all.valid_from = "2026-10-01"; all.valid_to = "2026-10-31";
        int o1 = c.saveOffer(all);
        Offer mine; mine.name = "Α 5€ απεντόμωση"; mine.kind = "fixed"; mine.value = 5; mine.user_id = 1; mine.target_type = "service";
        mine.target_id = svc; mine.valid_from = "2026-10-01"; mine.valid_to = "2026-10-15";
        int o2 = c.saveOffer(mine);
        Offer pk; pk.name = "Πακέτο 20%"; pk.value = 20; pk.target_type = "package"; pk.target_id = pkg; pk.valid_from = "2026-09-01"; pk.valid_to = "2026-12-31";
        int o3 = c.saveOffer(pk);
        CHECK(o1 > 0 && o2 > 0 && o3 > 0);
        Offer bad = all; bad.valid_to = "2026-09-01"; CHECK(c.saveOffer(bad) == -1); c.takeLastError();
        Offer bad2 = all; bad2.value = 120; CHECK(c.saveOffer(bad2) == -1); c.takeLastError();

        CHECK(c.findOffers(1, svc, -1, "2026-10-10").size() == 2);      // everyone + Α's own
        CHECK(c.findOffers(2, svc, -1, "2026-10-10").size() == 1);      // only everyone's
        CHECK(c.findOffers(1, other, -1, "2026-10-10").size() == 1);    // Α's is for another service
        CHECK(c.findOffers(1, svc, -1, "2026-10-20").size() == 1);      // Α's ended on the 15th
        CHECK(c.findOffers(2, -1, pkg, "2026-11-05").size() == 1);      // package offer only (October one ended)
        CHECK(ToCents(OfferDiscount(all, 33.33)) == 333 && ToCents(OfferDiscount(mine, 3.00)) == 300);

        // claimed on a charge: saved discounted, with list price and discount next to it
        Payment p; p.user_id = 1; p.service_id = svc; p.year = 2026; p.month = 10; p.day = 10; p.amount_due = 35.00; p.amount_paid = 35.00;
        p.status = "paid"; p.offer_id = o2; p.list_price = 40.00;
        CHECK(c.addPaymentsWithReceipt({p}, "", nullptr) > 0);
        auto offers = c.getOffers();
        for (auto& o : offers) if (o.id == o2) CHECK(o.uses == 1 && ToCents(o.given) == 500);

        // edit, then cancel (reason required); a cancelled offer is no longer suggested and can't change
        all.id = o1; all.value = 12; CHECK(c.saveOffer(all) == o1);
        CHECK(!c.cancelOffer(o1, " ")); c.takeLastError();
        CHECK(c.cancelOffer(o1, "Λήξη καμπάνιας"));
        CHECK(c.findOffers(2, svc, -1, "2026-10-10").empty());
        all.value = 15; CHECK(c.saveOffer(all) == -1); c.takeLastError();
        CHECK(c.verifyAuditLog().ok);
    }
    fs::remove_all(dir);
    std::printf(fails ? "offers_test: %d FAILED\n" : "offers_test: all passed\n", fails);
    return fails ? 1 : 0;
}
