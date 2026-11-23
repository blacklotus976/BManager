// Store (Αποθήκη): items, purchases (with and without a supplier / his document), service items, use on a charge, stock.
#include "SqliteConn.h"
#include "Money.h"
#include <cstdio>
#include <filesystem>

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } } while (0)

int main() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "bmanager_store_test";
    fs::remove_all(dir); fs::create_directories(dir);
    {
        SqliteConn c((dir / "data.db").string());
        Partner pr; pr.name = "ΧΗΜΙΚΑ ΑΕ"; int pid = c.savePartner(pr);
        CHECK(pid > 0);

        DataConn::StoreItem it; it.name = "Φίλτρο νερού"; it.code = "FLT-1"; it.suggested_cost = 4.5; it.partner_ids = { pid };
        int iid = c.saveStoreItem(it);
        CHECK(iid > 0);
        CHECK(c.countStoreItems("") == 1 && c.countStoreItems("χημικα") == 1 && c.countStoreItems("flt") == 1 && c.countStoreItems("xyz") == 0);
        CHECK(c.getStoreItem(iid).partner_ids.size() == 1);

        // no supplier: only the stock moves, no money
        CHECK(c.addStorePurchase(iid, 10, 4.5, 45, -1, "2026-10-01", "", false, "cash", "", "", "") > 0);
        // a supplier, owed for now: a cost line of 40.00 owed to him
        CHECK(c.addStorePurchase(iid, 8, 5, 40, pid, "2026-11-02", "", false, "cash", "", "", "") > 0);
        // a supplier, paid now with his invoice
        CHECK(c.addStorePurchase(iid, 2, 5, 10, pid, "2026-11-05", "", true, "bank", "invoice", "ΤΙΜ-77", "2026-11-05") > 0);
        DataConn::StoreItem got = c.getStoreItem(iid);
        CHECK(got.bought == 20.0 && got.used == 0.0 && got.stock == 20.0);
        auto months = c.storeMonths(iid);
        CHECK(months.size() == 2 && months[0].month == "2026-11" && months[0].bought == 10.0 && ToCents(months[0].spent) == 5000);
        CHECK(c.getPartnerDocs(pid, "", "").size() == 1);

        // a service that uses 2 filters; charged once, 3 were really used
        Service s; s.label = "ΣΥΝΤΗΡΗΣΗ"; s.suggested_price = 60; int svc = c.addServiceGetId(-1, s);
        DataConn::ServiceItem si; si.item_id = iid; si.qty = 2;
        CHECK(c.setServiceItems(svc, { si }));
        CHECK(c.getServiceItems(svc).size() == 1 && c.getServiceItems(svc)[0].qty == 2.0);
        User u; u.full_name = "ΠΕΛΑΤΗΣ"; c.addUser(u); int uid = c.searchUsers("ΠΕΛΑΤΗΣ", 0, 5)[0].id;
        Payment p; p.user_id = uid; p.service_id = svc; p.year = 2026; p.month = 11; p.day = 10; p.amount_due = 60; p.direction = "in"; p.status = "paid";
        int ch = c.addPaymentsWithReceipt({ p }, "", nullptr);
        CHECK(ch > 0);
        si.qty = 3; CHECK(c.logStoreUse(ch, { si }, "2026-11-10"));
        got = c.getStoreItem(iid);
        CHECK(got.used == 3.0 && got.stock == 17.0);
        auto moves = c.getStoreMoves(iid, 10);
        CHECK(moves.size() == 4 && moves[0].kind == "use" && moves[0].service == "ΣΥΝΤΗΡΗΣΗ" && moves[0].client == "ΠΕΛΑΤΗΣ");
        // the service price is untouched by the items
        CHECK(ToCents(c.getPaymentById(ch).amount_due) == 6000);
    }
    fs::remove_all(dir);
    if (fails) { std::printf("store_test: %d FAILED\n", fails); return 1; }
    std::printf("store_test: all passed\n");
    return 0;
}
