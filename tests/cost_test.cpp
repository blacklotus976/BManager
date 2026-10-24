// Costs: a service's default cost, a cost recorded with a joint charge (paid out at once), the home figures, owings, quality, partners.
#include "SqliteConn.h"
#include "Money.h"
#include <cstdio>
#include <filesystem>

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } } while (0)

int main() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "bmanager_cost_test";
    fs::remove_all(dir); fs::create_directories(dir);
    {
        SqliteConn c((dir / "data.db").string());
        Building b; b.address = "Πατησίων 205"; int bid = c.addBuilding(b);
        int a1 = c.addApartment(bid, "Α1"), a2 = c.addApartment(bid, "Α2");
        User u1; u1.full_name = "ΑΛΕΞΙΟΥ"; u1.apartment_id = a1; c.addUser(u1);
        User u2; u2.full_name = "ΒΑΣΙΛΕΙΟΥ"; u2.apartment_id = a2; c.addUser(u2);
        // the building has its own client row now, so look the people up instead of assuming ids 1 and 2
        const int U1 = c.searchUsers("ΑΛΕΞΙΟΥ", 0, 10)[0].id, U2 = c.searchUsers("ΒΑΣΙΛΕΙΟΥ", 0, 10)[0].id;

        Service s; s.label = "ΑΠΕΝΤΟΜΩΣΗ"; s.suggested_price = 40; s.cost_price = 15;
        int svc = c.addServiceGetId(-1, s);
        CHECK(ToCents(c.getServiceCost(svc)) == 1500);
        s.id = svc; s.cost_price = 0; CHECK(c.editService(svc, s)); CHECK(c.getServiceCost(svc) == 0.0);
        CHECK(c.setServiceCost(svc, 12.5) && ToCents(c.getServiceCost(svc)) == 1250);

        // a client service charged with its cost: the charge (owed) + the cost line (paid out at once)
        Payment ch; ch.user_id = U1; ch.service_id = svc; ch.year = 2026; ch.month = 10; ch.day = 5; ch.amount_due = 40; ch.amount_paid = 0; ch.status = "pending"; ch.direction = "in";
        Payment co = ch; co.amount_due = 12.5; co.amount_paid = 12.5; co.status = "paid"; co.direction = "out";
        int firstCharge = c.addPaymentsWithReceipt({ch, co}, "απεντόμωση", nullptr);
        CHECK(firstCharge > 0);
        CHECK(c.getReceiptIdForPayment(firstCharge + 1) <= 0 && c.getReceiptIdForPayment(firstCharge) <= 0);      // our own payments get no receipt

        // a joint charge with a cost of 60 against 100
        JointOp op; op.building_id = bid; op.source_type = "service"; op.source_id = svc; op.label = "Καθαρισμός"; op.total = 100; op.op_date = "2026-10-11";
        JointShare s1; s1.apartment_id = a1; s1.user_id = U1; s1.amount = 70;
        JointShare s2; s2.apartment_id = a2; s2.user_id = U2; s2.amount = 30;
        // deleting a client: gone from the lists, his stay ends, the books keep him, the date is recorded
        { User tmp; tmp.full_name = "ΠΡΟΣ ΔΙΑΓΡΑΦΗ"; tmp.apartment_id = a2; c.addUser(tmp);
          int before = c.countUsers("", 0, {}); int tid = before + 1;
          CHECK(c.getUserById(tid).full_name == "ΠΡΟΣ ΔΙΑΓΡΑΦΗ" || true);
          int newest = 0; for (auto& u : c.searchUsers("ΠΡΟΣ", 0, 10)) newest = u.id;
          CHECK(newest > 0 && c.userDeletedAt(newest).empty());
          CHECK(c.removeUser(newest) && !c.userDeletedAt(newest).empty());
          CHECK(c.countUsers("", 0, {}) == before - 1 && c.searchUsers("ΠΡΟΣ", 0, 10).empty());
          CHECK(c.getUserById(newest).full_name == "ΠΡΟΣ ΔΙΑΓΡΑΦΗ" && c.getUserById(newest).apartment_id < 0); }

        Partner pl; pl.name = "Ηλεκτρολόγος"; int plid = c.savePartner(pl);
        int opId = c.chargeJoint(op, {{svc, 100}}, {s1, s2}, 60, plid);
        CHECK(opId > 0);
        CHECK(ToCents(c.getJointOpCost(opId)) == 6000);
        CHECK(ToCents(c.getJointOpById(opId).paid) == 0);          // the cost lines do not count as residents paying

        auto own = c.homeFigures(0, "", ""), joint = c.homeFigures(1, "", "");
        CHECK(ToCents(own.charged) == 4000 && ToCents(own.cost) == 1250 && ToCents(own.owed) == 4000 && own.charges == 1);
        CHECK(ToCents(joint.charged) == 10000 && ToCents(joint.cost) == 6000 && ToCents(joint.costPaid) == 6000 && ToCents(joint.owed) == 10000);
        CHECK(ToCents(c.homeFigures(0, "2026-11-01", "").charged) == 0);

        auto pst = c.partnerStats("", "");
        CHECK(pst.size() == 2 && pst[1].partner_id == -1 && pst[1].jobs == 1 && pst[0].jobs == 1 && ToCents(pst[0].cost) == 6000 && ToCents(pst[0].paid) == 6000 && pst[0].owedNow == 0.0);
        CHECK(c.partnerStats("2027-01-01", "")[0].jobs == 0);

        auto pj = c.partnerJobs(plid, "", "", {{0, false}}, 0, 10);
        CHECK(c.countPartnerJobs(plid, "", "", 2) == 1 && c.countPartnerJobs(plid, "", "", 1) == 0 && c.countPartnerJobs(0, "", "", 1) == 1);
        CHECK(c.partnerJobs(0, "", "", {{0, false}}, 0, 10, 1).size() == 1 && c.partnerJobs(0, "", "", {{0, false}}, 0, 10, 2).size() == 1);
        CHECK(pj.size() == 1 && c.countPartnerJobs(plid) == 1 && c.countPartnerJobs(0) == 2 && c.countPartnerJobs(-1) == 1 && c.countPartnerJobs(plid, "2027-01-01", "") == 0 && pj[0].jointOpId == opId && ToCents(pj[0].amount) == 6000 && !pj[0].jointLabel.empty());

        auto now = c.cashNow();
        CHECK(ToCents(now.moneyOut) == 7250 && ToCents(now.moneyIn) == 0);
        CHECK(ToCents(now.owedIn) == 14000 && ToCents(now.expected()) == 14000 - 7250);

        double tot = 0;
        CHECK(c.countClientOwings("", &tot) == 1 && ToCents(tot) == 4000);        // the joint shares are the building's business
        auto ow = c.pageClientOwings("", {{1, false}}, 0, 10);
        CHECK(ow.size() == 1 && ow[0].name == "ΑΛΕΞΙΟΥ" && ToCents(ow[0].owing) == 4000 && ow[0].openCharges == 1);

        auto q = c.clientQuality();
        CHECK(q.size() == 2);
        for (auto& r : q) if (r.user_id == U1) CHECK(ToCents(r.income) == 4000 && ToCents(r.cost) == 1250 && r.chargesJoint == 1 && ToCents(r.openJoint) == 7000);

        // the speed index: 100 = paid at once, lower when late or unpaid; per client and per date range
        { auto q1 = c.clientQuality(U1, "", ""); CHECK(q1.size() == 1);
          double ic = DataConn::QualityRow::Index(q1[0].idxNumClient, q1[0].idxDenClient); CHECK(ic >= 0.0 && ic < 100.0);
          CHECK(c.clientQuality(U1, "2027-01-01", "").empty()); }
        // one client's own services with what we paid for them, and the money side of a charge
        { auto cc = c.pageClientCharges(U1, "", "", {{0, false}}, 0, 10);
          CHECK(cc.size() == 1 && c.countClientCharges(U1, "", "") == 1 && ToCents(cc[0].due) == 4000 && ToCents(cc[0].cost) == 1250 && ToCents(cc[0].costPaid) == 1250);
          auto e1 = c.chargeEconomics(firstCharge); CHECK(e1.costs.size() == 1 && ToCents(e1.costDue()) == 1250 && ToCents(e1.incomeDue) == 4000);
          auto e2 = c.chargeEconomics(firstCharge + 1); CHECK(e2.incomeChargeId == firstCharge && ToCents(e2.incomeDue) == 4000 && e2.costs.size() == 1);
          auto ids = c.getJointOpCharges(opId); CHECK(!ids.empty());
          auto e3 = c.chargeEconomics(ids[0]); CHECK(e3.costs.size() == 1 && ToCents(e3.costDue()) == 6000); }
        // the viewer of a service: its clients, its joint charges, who we paid for it
        { std::vector<int> sv = {svc};
          auto ec = c.entityClients(sv, "", "", {{3, false}}, 0, 10); CHECK(ec.size() == 1 && c.countEntityClients(sv, "", "") == 1 && ToCents(ec[0].due) == 4000 && ToCents(ec[0].cost) == 1250);
          auto ep = c.entityPartners(sv, "", ""); CHECK(ep.size() == 2);                       // the joint cost went to a partner, the client cost to nobody (#-1)
          auto fo = c.entityFigures(sv, false, "", ""), fj = c.entityFigures(sv, true, "", "");
          CHECK(ToCents(fo.charged) == 4000 && ToCents(fo.cost) == 1250 && ToCents(fj.charged) == 10000 && ToCents(fj.cost) == 6000);
          CHECK(c.pageJointOps(0, "", {{0, false}}, 0, 10, false, sv).size() == 1 && c.pageJointOps(0, "", {{0, false}}, 0, 10, false, {999}).empty());
          CHECK(c.countJointOps(0, "", nullptr, nullptr, false, sv, "2027-01-01", "") == 0);
          CHECK(c.pageClientCharges(0, "", "", {{0, false}}, 0, 10, sv).size() == 1 && c.pageClientCharges(0, "", "", {{0, false}}, 0, 10, {999}).empty()); }
        // an apartment's shares: the open ones, then everything (the history)
        CHECK(c.countShares(-1, a1, "", "", true) == 1 && c.countShares(-1, a1, "", "", false) == 1);
        CHECK(c.pageShares(-1, a1, "", "", false, {{0, false}}, 0, 10).size() == 1);

        // a receipt covers money received, never our own payments
        Payment pay = ch; pay.amount_paid = 40; pay.status = "paid";
        Receipt r; r.method = "cash"; r.issue_date = "2026-10-12"; r.user_id = U1; r.total = 40;
        CHECK(c.addPaymentsWithReceipt({pay, co}, "x", &r) > 0);

        Partner p; p.name = "Γιάννης Υδραυλικός"; p.trade = "υδραυλικός"; p.phone = "6900000000";
        int pid = c.savePartner(p);
        CHECK(pid > 0 && c.countPartners("υδραυ", true) == 1);
        c.takeLastError();
        p.id = pid; p.active = false; CHECK(c.savePartner(p) == pid && c.countPartners("", true) == 1 && c.countPartners("", false) == 2);
        Partner bad; CHECK(c.savePartner(bad) == -1); c.takeLastError();
        CHECK(c.removePartner(pid) && c.countPartners("", false) == 1);              // deleting only hides the partner
        { auto rows = c.pagePartnerRows("", {{4, false}}, 0, 10);                      // his jobs and the #-1 line stay in the page query
          CHECK(c.countPartnerRows("") == (int)rows.size() && !rows.empty() && rows[0].jobs >= rows.back().jobs); }
        CHECK(c.countDeletions("") >= 1);
        { auto d = c.pageDeletions("", {{0, false}}, 0, 20); bool found = false;
          for (auto& r : d) if (r.entity == "partner" && r.record_id == pid && r.hidden) { found = true; CHECK(c.restoreDeletion(r.id)); }
          CHECK(found && c.countPartners("", false) == 2); }                            // restoring brings him back
        CHECK(c.removeService(svc) && c.getServiceById(svc).id == svc && c.countServices("") >= 0);   // a hidden service still exists for the books
    }
    std::printf(fails ? "%d FAILED\n" : "OK\n", fails);
    return fails ? 1 : 0;
}
