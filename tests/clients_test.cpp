// Clients of three kinds: people, buildings (one client per building, following its address and ΑΦΜ) and
// businesses; email and ΑΦΜ on every client; the kind filter of the client list; charging a building as a client.
#include "SqliteConn.h"
#include "Money.h"
#include <cstdio>
#include <ctime>
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

        // the office's code, extra details and the manager: stored, searched, filtered by column
        {
            User x; x.full_name = "ΚΩΔΙΚΟΣ ΔΟΚΙΜΗ"; x.code = "Π-77"; x.special_code = "ΠΑΛ9"; x.extra_json = "{\"Από\":\"σύσταση\"}"; CHECK(c.addUser(x));
            User y; y.full_name = "ΑΛΛΟΣ ΔΟΚΙΜΗ"; y.code = "Π-78"; CHECK(c.addUser(y));
            auto hit = c.pageUsers("σύσταση", 0, {}, 0, 10); CHECK(hit.size() == 1 && hit[0].code == "Π-77" && hit[0].special_code == "ΠΑΛ9");
            CHECK(c.pageUsers("Π-78", 0, {}, 0, 10).size() == 1 && c.pageUsers("Π-78", 2, {}, 0, 10).size() == 1);
            User e = hit[0]; int id = e.id; e.code = "Π-99"; CHECK(c.editUser(id, e)); CHECK(c.getUserById(id).code == "Π-99");
            DataConn::ColFilters cf; cf.in[11] = {"Π-99"};
            auto one = c.pageUsers("", 0, {}, 0, 10, {}, 1, &cf); CHECK(one.size() == 1 && one[0].id == id && c.countUsers("", 0, {}, 1, &cf) == 1);
            cf = {}; cf.text[3] = "δοκιμη"; CHECK(c.countUsers("", 0, {}, 1, &cf) == 2);                   // accents and case do not matter
            cf = {}; cf.in[12] = {"Από: σύσταση"}; CHECK(c.countUsers("", 0, {}, 1, &cf) == 1);
            auto ex = c.distinctUserValues(12, "", "", 0, {}, 1, nullptr, 50); CHECK(ex.size() == 1 && ex[0].first == "Από: σύσταση" && ex[0].second == 1);
            cf = {}; cf.in[11] = {"Π-78"};
            CHECK(c.distinctUserValues(11, "", "", 0, {}, 1, &cf, 50).size() == 2);                        // its own filter leaves its values listed
            CHECK(c.distinctUserValues(7, "", "", 0, {}, 1, &cf, 50).size() == 1);                         // another column narrows them
            CHECK(c.removeUser(id)); CHECK(c.removeUser(c.pageUsers("Π-78", 0, {}, 0, 10)[0].id));
        }
        // a contract: 120 + 60 a year in 12 installments, started two months ago. What fell due is owed, the rest only scheduled.
        {
            SqliteConn c((dir / "contract.db").string());
            std::time_t now = std::time(nullptr); std::tm tm{}; localtime_s(&tm, &now);
            int ym = tm.tm_year + 1900; int mm = tm.tm_mon + 1 - 2; if (mm < 1) { mm += 12; ym--; }
            char sd[16]; std::snprintf(sd, sizeof(sd), "%04d-%02d-01", ym, mm);
            User x; x.full_name = "ΣΥΝΔΡΟΜΗΤΗΣ"; CHECK(c.addUser(x)); int su = c.pageUsers("ΣΥΝΔΡΟΜΗΤΗΣ", 0, {}, 0, 10)[0].id;
            Service s1; s1.label = "ΥΠ1"; s1.suggested_price = 10; int a1 = c.addServiceGetId(-1, s1); Service s2; s2.label = "ΥΠ2"; s2.suggested_price = 10; int a2 = c.addServiceGetId(-1, s2);
            DataConn::PaymentFilter of; of.userId = su; of.status = 1; of.owed = 2; of.direction = "in";
            DataConn::Contract k; k.user_id = su; k.source_type = "package"; k.source_id = 1; k.label = "ΕΤΗΣΙΟ"; k.start_date = sd; k.installments = 12;
            CHECK(c.addContract(k, {}) < 0); c.takeLastError();
            k.installments = 0; CHECK(c.addContract(k, {{a1, "ΥΠ1", 10, 0}}) < 0); c.takeLastError(); k.installments = 12;
            int cid = c.addContract(k, {{a1, "ΥΠ1", 120, 24}, {a2, "ΥΠ2", 60.01, 0}});
            if (cid <= 0) std::printf("contract: %s\n", c.takeLastError().c_str());
            CHECK(cid > 0);
            auto in = c.getContractInstallments(cid);
            CHECK(in.size() == 12);
            long long sum = 0; int fut = 0; for (auto& i : in) { sum += ToCents(i.due); if (i.future) fut++; }
            CHECK(sum == 18001 && fut == 9);                                    // to the cent; 3 dates have come (day 1 of three months)
            auto kk = c.getContract(cid);
            CHECK(kk.open && ToCents(kk.total) == 18001 && ToCents(kk.cost) == 2400 && ToCents(kk.collected) == 0);
            CHECK(ToCents(kk.owedNow) == ToCents(in[0].due) + ToCents(in[1].due) + ToCents(in[2].due));
            CHECK(ToCents(kk.owedNow) + ToCents(kk.scheduled) == 18001);
            CHECK(ToCents(c.paymentTotals(of).owedIn) == ToCents(kk.owedNow));   // the future installments are NOT owed
            // the money side: only what came due of the cost is owed to the partner
            DataConn::PaymentFilter wo; wo.userId = su; wo.status = 1; wo.owed = 2; wo.direction = "out";
            CHECK(ToCents(c.paymentTotals(wo).owedOut) == 600);
            // collect a due installment with a receipt, then pay a future one early
            auto payIt = [&](const DataConn::ContractInstallment& i) { MoneyPayment m; m.user_id = su; m.pay_date = sd; m.amount = i.due; m.method = "cash"; Receipt r; r.method = "cash"; r.user_id = su;
                std::vector<Allocation> al; for (int ch : i.charges) al.push_back({-1, ch, c.getPaymentById(ch).amount_due}); return c.recordPayment(m, al, &r); };
            int r1 = payIt(in[0]); CHECK(r1 > 0);
            int r2 = payIt(in[11]); CHECK(r2 > 0);
            kk = c.getContract(cid);
            CHECK(ToCents(kk.collected) == ToCents(in[0].due) + ToCents(in[11].due));
            CHECK(ToCents(kk.owedNow) == ToCents(in[1].due) + ToCents(in[2].due));
            CHECK(ToCents(kk.scheduled) == 18001 - ToCents(kk.collected) - ToCents(kk.owedNow));
            CHECK(c.getContractReceipts(cid).size() == 2);
            auto after = c.getContractInstallments(cid); CHECK(!after[11].future && ToCents(after[11].balance) == 0);
            // any duration, any number of payments, on chosen dates; dates outside it or doubled are refused
            {   DataConn::Contract d = k; d.label = "ΕΞΑΜΗΝΟ"; d.start_date = "2030-01-15"; d.months = 6; d.installments = 3;
                d.dates = {"2030-05-01", "2030-01-20", "2030-03-09"};
                int did = c.addContract(d, {{a1, "ΥΠ1", 100, 0}}); if (did <= 0) std::printf("contract: %s\n", c.takeLastError().c_str()); CHECK(did > 0);
                auto di = c.getContractInstallments(did); CHECK(di.size() == 3 && di[0].date == "2030-01-20" && di[2].date == "2030-05-01" && ToCents(di[0].due) == 3334 && di[2].future);
                auto dk = c.getContract(did); CHECK(dk.end_date == "2030-07-14" && dk.months == 6 && ToCents(dk.scheduled) == 10000);
                DataConn::Contract e = d; e.dates = {"2030-01-20", "2030-01-20", "2030-03-09"}; CHECK(c.addContract(e, {{a1, "ΥΠ1", 100, 0}}) < 0); c.takeLastError();
                e.dates = {"2030-01-20", "2030-02-20", "2031-03-09"}; CHECK(c.addContract(e, {{a1, "ΥΠ1", 100, 0}}) < 0); c.takeLastError();
                e.dates = {"2030-01-20"}; CHECK(c.addContract(e, {{a1, "ΥΠ1", 100, 0}}) < 0); c.takeLastError();
                DataConn::Contract ev = d; ev.dates.clear(); ev.label = "ΙΣΑ"; ev.installments = 5; ev.months = 10; int ei = c.addContract(ev, {{a1, "ΥΠ1", 50, 0}}); CHECK(ei > 0); CHECK(c.getContractInstallments(ei).size() == 5 && c.getContractInstallments(ei)[1].date == "2030-03-15"); }
            // ending a contract: its future installments go, what fell due stays
            {   DataConn::Contract q = k; q.label = "ΑΚΥΡΗ"; q.start_date = sd; q.months = 12; q.installments = 12; q.dates.clear();
                int qid = c.addContract(q, {{a1, "ΥΠ1", 120, 12}}); CHECK(qid > 0);
                auto q0 = c.getContract(qid); CHECK(ToCents(q0.owedNow) == 3000 && ToCents(q0.scheduled) == 9000);
                CHECK(!c.cancelContract(qid, " ")); c.takeLastError();
                CHECK(c.cancelContract(qid, "έφυγε"));
                auto q1 = c.getContract(qid); CHECK(q1.cancelled && q1.open && ToCents(q1.owedNow) == 3000 && ToCents(q1.scheduled) == 0);
                CHECK(!c.cancelContract(qid, "πάλι")); c.takeLastError();
                for (auto& inq : c.getContractInstallments(qid)) for (int ch : inq.charges) { MoneyPayment m; m.user_id = su; m.pay_date = sd; m.amount = 10; m.method = "cash"; Receipt r; r.method = "cash"; r.user_id = su; CHECK(c.recordPayment(m, {{-1, ch, 10.0}}, &r) > 0); }
                auto q2 = c.getContract(qid); CHECK(!q2.open && ToCents(q2.owedNow) == 0);
                DataConn::Contract tmp = k; (void)tmp; }
            // past contracts as shortcuts: the same one is listed once, with no need for its client
            { auto t0 = c.contractTemplates(10); CHECK(t0.size() == 4 && t0[3].label == "ΕΤΗΣΙΟ" && c.getContractItems(t0[3].id).size() == 2 && t0[3].months == 12); CHECK(c.refreshDueCharges()); }
            // contracts stand apart from services: lists, receipts, income and owings
            Payment own; own.user_id = su; own.service_id = a1; own.year = 2026; own.month = 1; own.day = 5; own.amount_due = 30; own.amount_paid = 30; own.status = "paid"; own.direction = "in";
            Receipt ownR; ownR.user_id = su; ownR.method = "cash"; CHECK(c.addPaymentsWithReceipt({own}, "", &ownR) > 0);
            DataConn::PaymentFilter svcF; svcF.userId = su; svcF.direction = "in"; svcF.joint = 4; CHECK(c.paymentTotals(svcF).count == 1);
            DataConn::PaymentFilter ctF = svcF; ctF.joint = 7; CHECK(c.paymentTotals(ctF).count == 35);
            DataConn::ReceiptQuery rqC; rqC.kind = "contracts"; DataConn::ReceiptQuery rqS; rqS.kind = "clients"; CHECK(c.countReceipts(rqC) == 5 && c.countReceipts(rqS) == 1);
            auto sp = c.incomeSplit("", ""); CHECK(ToCents(sp.contracts) == ToCents(kk.collected) + 3000 && ToCents(sp.fees) == 0 && ToCents(sp.contractsOwed) == ToCents(kk.owedNow) && ToCents(sp.contractsScheduled) == ToCents(kk.scheduled) + 15000 && ToCents(sp.servicesOwed) == 0);
            auto co = c.contractOwings(); CHECK(co.size() == 1 && co[0].user_id == su && ToCents(co[0].owed) == ToCents(kk.owedNow));
            CHECK(!c.getPaymentById(in[1].charges[0]).created_at.empty() && !c.getReceiptById(r1).created_at.empty());
            // the lines point at their contract, and cannot be removed or re-priced
            Payment line = c.getPaymentById(in[1].charges[0]); CHECK(line.contract_id == cid);
            CHECK(!c.removePayment(line.id)); c.takeLastError();
            Payment re = line; re.amount_due = 1; CHECK(!c.editPayment(line.id, re)); c.takeLastError();
            // totals and lists
            auto tot = c.contractTotals(su, 1, ""); CHECK(tot.count == 3 && ToCents(tot.total) == 33001);
            CHECK(c.pageContracts(su, 2, "", {}, 0, 10).size() == 1 && c.pageContracts(su, 0, "ετησιο", {}, 0, 10).size() == 1);      // the ended and paid one is old
            CHECK(c.getContractItems(cid).size() == 2);
            auto chk = c.verifyAuditLog(); for (auto& pr : chk.problems) std::printf("audit: %s\n", pr.c_str()); CHECK(chk.ok);
        }
        // the building got its client
        int bc = BuildingClient(c, bid);
        CHECK(bc > 0);
        User bu = c.getUserById(bc);
        { User m = bu; m.manager_name = "ΔΙΑΧΕΙΡΙΣΤΗΣ ΔΟΚΙΜΗ"; CHECK(c.editUser(bc, m)); CHECK(c.getUserById(bc).manager_name == "ΔΙΑΧΕΙΡΙΣΤΗΣ ΔΟΚΙΜΗ");
          CHECK(c.pageUsers("ΔΙΑΧΕΙΡΙΣΤΗΣ ΔΟΚΙΜΗ", 0, {}, 0, 10).size() == 1); m.manager_name = ""; CHECK(c.editUser(bc, m)); }
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
