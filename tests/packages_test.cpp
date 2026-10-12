// Packages keep their history: retiring hides a package, taking a service out keeps the row as removed,
// adding it back restores it. Also: the joint / own-charges payment filter.
#include "SqliteConn.h"
#include "Money.h"
#include <cstdio>
#include <filesystem>

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } } while (0)

int main() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "bmanager_packages_test";
    fs::remove_all(dir); fs::create_directories(dir);
    {
        SqliteConn c((dir / "data.db").string());
        Service s; s.label = "Α"; s.suggested_price = 10; int a = c.addServiceGetId(-1, s);
        s.label = "Β"; int b = c.addServiceGetId(-1, s);
        int pkg = c.addMasterService("ΠΑΚΕΤΟ");
        CHECK(c.addMasterServiceComponent(pkg, a) && c.addMasterServiceComponent(pkg, b));
        CHECK(c.getMasterServiceComponents(pkg).size() == 2);
        CHECK(c.removeMasterServiceComponent(pkg, b));
        CHECK(c.getMasterServiceComponents(pkg).size() == 1);           // hidden, not deleted
        CHECK(c.addMasterServiceComponent(pkg, b));
        CHECK(c.getMasterServiceComponents(pkg).size() == 2);           // back again
        CHECK(c.renameMasterService(pkg, "ΝΕΟ ΟΝΟΜΑ") && c.getPackageLabel(pkg) == "ΝΕΟ ΟΝΟΜΑ");
        CHECK(c.countPackages("") == 1);
        CHECK(c.removeMasterService(pkg));
        CHECK(c.countPackages("") == 0 && c.getMasterServices().empty());   // retired: gone from lists
        CHECK(c.getPackageLabel(pkg) == "ΝΕΟ ΟΝΟΜΑ");                       // still known for history
        CHECK(!c.renameMasterService(pkg, "X") || c.getPackageLabel(pkg) == "ΝΕΟ ΟΝΟΜΑ");

        // own charge vs joint filter: a plain charge is "own"
        User u; u.full_name = "Π"; c.addUser(u);
        Payment p; p.user_id = 1; p.service_id = a; p.year = 2026; p.month = 10; p.day = 1; p.amount_due = 10; p.amount_paid = 0;
        CHECK(c.addPayment(p));
        DataConn::PaymentFilter f; f.userId = 1;
        f.joint = 2; CHECK(c.countPaymentsWhere(f) == 1);
        f.joint = 1; CHECK(c.countPaymentsWhere(f) == 0);
        CHECK(c.getClientOffers(1, "2026-10-01").empty());
    }
    fs::remove_all(dir);
    if (fails) { std::printf("%d check(s) failed\n", fails); return 1; }
    std::printf("packages: all checks passed\n");
    return 0;
}
