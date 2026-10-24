// The same money seen from every screen must add up: home figures, cash, owings, the ranking, partners, a client's profile.
#include "SqliteConn.h"
#include "Seed.h"
#include "Money.h"
#include <cstdio>
#include <filesystem>

static int fails = 0;
#define SAME(a, b) do { long long x_ = ToCents(a), y_ = ToCents(b); if (x_ != y_) { std::printf("FAIL line %d: %s = %lld  vs  %s = %lld\n", __LINE__, #a, x_, #b, y_); fails++; } } while (0)

int main() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "bmanager_math_test";
    fs::remove_all(dir); fs::create_directories(dir);
    {
        SqliteConn c((dir / "data.db").string());
        seedDemoDataIfEmpty(&c, "");
        using F = DataConn::PaymentFilter;

        auto cash = c.cashNow();
        auto own = c.homeFigures(0, "", ""), jnt = c.homeFigures(1, "", "");
        F fin; fin.direction = "in"; auto tin = c.paymentTotals(fin);          // every line: scheduled (future) ones are owed too
        F fout; fout.direction = "out"; auto tout = c.paymentTotals(fout);
        // the cash picture = the payments table
        SAME(cash.moneyIn, tin.collected);   SAME(cash.owedIn, tin.owedIn);
        SAME(cash.moneyOut, tout.paidOut);   SAME(cash.owedOut, tout.owedOut);
        // the two halves of the home page make the whole
        SAME(own.collected + jnt.collected, cash.moneyIn);
        SAME(own.owed + jnt.owed, cash.owedIn);
        SAME(own.costPaid + jnt.costPaid, cash.moneyOut);
        SAME(own.cost + jnt.cost - (own.costPaid + jnt.costPaid), cash.owedOut);
        // charged = collected + owed (nothing lost between the columns)
        SAME(own.charged, own.collected + own.owed);
        SAME(jnt.charged, jnt.collected + jnt.owed);

        // the ranking page adds up to the home page
        double inc = 0, incJ = 0, openC = 0, openJ = 0, cost = 0; long long n = 0;
        for (auto& r : c.clientQuality()) { inc += r.income; incJ += r.incomeJoint; openC += r.openClient; openJ += r.openJoint; cost += r.cost; n++; }
        // the ranking only rates charges that are due (not the scheduled ones): add those back to compare
        F fo1; fo1.direction = "in"; fo1.status = 2; fo1.joint = 2; auto fut1 = c.paymentTotals(fo1);
        F fo2; fo2.direction = "in"; fo2.status = 2; fo2.joint = 1; auto fut2 = c.paymentTotals(fo2);
        F fo3; fo3.direction = "out"; fo3.status = 2; fo3.joint = 2; auto fut3 = c.paymentTotals(fo3);
        SAME(inc + fut1.dueAll, own.charged);   SAME(incJ + fut2.dueAll, jnt.charged);
        SAME(openC + fut1.dueAll, own.owed);    SAME(openJ + fut2.dueAll, jnt.owed);
        SAME(cost + fut3.dueAll, own.cost);

        // clients owing (list) = the same open own charges that are due (scheduled ones are not owed yet)
        double owingTotal = 0; c.countClientOwings("", &owingTotal);
        SAME(owingTotal, own.owed - own.scheduledIn);

        // partners: what the cost lines say
        double pc = 0, pp = 0, po = 0; for (auto& s : c.partnerStats("", "")) { pc += s.cost; pp += s.paid; po += s.owedNow; }
        SAME(pc, own.cost + jnt.cost);   SAME(pp, cash.moneyOut);   SAME(po, cash.owedOut);

        // every client: the profile numbers = the payments table, own services and joint parts apart
        int clients = c.countUsers("", 0, {});
        for (auto& r : c.clientQuality()) {
            F fo; fo.userId = r.user_id; fo.direction = "in"; fo.status = 1; fo.joint = 2; auto to = c.paymentTotals(fo);
            F fj = fo; fj.joint = 1; auto tj = c.paymentTotals(fj);
            SAME(to.owedIn, r.openClient);   SAME(tj.owedIn, r.openJoint);
            SAME(to.dueAll, r.income);       SAME(tj.dueAll, r.incomeJoint);
            // the services table of his profile: charged - paid = owed, row by row, and the total of the rows
            double due = 0, paid = 0, bal = 0;
            for (auto& ch : c.pageClientCharges(r.user_id, "", "", {}, 0, 100000)) {
                SAME(ch.due - ch.paid, ch.balance);
                if (ch.status != "future") { due += ch.due; paid += ch.paid; bal += ch.balance; }
            }
            SAME(due, to.dueAll);  SAME(bal, r.openClient);  SAME(paid, to.collected);
        }
        // a joint charge with part-paid shares and a part-paid cost: paid + owed must equal the charge, on both sides
        {
            auto before = c.cashNow();
            Building b; b.address = "Δοκιμή 1"; int bid = c.addBuilding(b);
            int a1 = c.addApartment(bid, "Α1"), a2 = c.addApartment(bid, "Α2");
            User u1; u1.full_name = "ΠΡΩΤΟΣ"; u1.apartment_id = a1; c.addUser(u1); int uid1 = c.countUsers("", 0, {});
            User u2; u2.full_name = "ΔΕΥΤΕΡΟΣ"; u2.apartment_id = a2; c.addUser(u2); int uid2 = uid1 + 1;
            Service s; s.label = "ΔΟΚΙΜΗ ΧΡΕΩΣΗΣ"; s.suggested_price = 10; int svc = c.addServiceGetId(-1, s);
            JointOp op; op.building_id = bid; op.source_type = "service"; op.source_id = svc; op.label = "Δοκιμή"; op.total = 100; op.op_date = "2026-10-01";
            JointShare s1; s1.apartment_id = a1; s1.user_id = uid1; s1.amount = 70; s1.paid = 30;      // paid some
            JointShare s2; s2.apartment_id = a2; s2.user_id = uid2; s2.amount = 30; s2.paid = 0;       // owes all
            int opId = c.chargeJoint(op, {{svc, 100}}, {s1, s2}, 60.0, 1, 20.0);                       // cost 60, paid 20, owe 40
            if (svc <= 0) std::printf("service not created: %s\n", c.takeLastError().c_str());
            if (opId <= 0) std::printf("joint charge refused: %s\n", c.takeLastError().c_str());
            SAME(opId > 0 ? 1.0 : 0.0, 1.0);
            auto after = c.cashNow();
            SAME(after.moneyIn - before.moneyIn, 30.0);   SAME(after.owedIn - before.owedIn, 70.0);
            SAME(after.moneyOut - before.moneyOut, 20.0); SAME(after.owedOut - before.owedOut, 40.0);
            // two services in one joint charge, each with its own cost: 10 + 25 = 35, we paid 20 (fills the first line, part of the second)
            {
                Service s2s; s2s.label = "ΔΟΚΙΜΗ Β"; s2s.suggested_price = 5; int svc2 = c.addServiceGetId(-1, s2s);
                auto b0 = c.cashNow();
                JointOp o2 = op; o2.label = "Δύο υπηρεσίες"; JointShare t1 = s1; t1.paid = 0; JointShare t2 = s2; t2.paid = 0;
                JointLine l1; l1.service_id = svc; l1.total = 60; l1.cost = 10; JointLine l2; l2.service_id = svc2; l2.total = 40; l2.cost = 25;
                int id2 = c.chargeJoint(o2, {l1, l2}, {t1, t2}, 0.0, 1, 20.0);
                SAME(id2 > 0 ? 1.0 : 0.0, 1.0);
                auto a0 = c.cashNow();
                SAME(a0.moneyOut - b0.moneyOut, 20.0); SAME(a0.owedOut - b0.owedOut, 15.0);
                bool noted = false; for (int cid : c.getJointOpCharges(id2)) if (c.getPaymentById(cid).notes.find("μερίδιο") != std::string::npos) noted = true;
                SAME(noted ? 1.0 : 0.0, 1.0);
            }            // receipts: one per paying resident of a joint charge; one per service of a package when asked
            {
                size_t r0 = c.getReceipts(0, 100000).size();
                Receipt tpl; tpl.method = "cash";
                JointShare p1 = s1; p1.paid = 20; JointShare p2 = s2; p2.paid = 30;
                JointOp o3 = op; o3.label = "Με αποδείξεις";
                int id3 = c.chargeJoint(o3, {{svc, 100}}, {p1, p2}, 0.0, -1, -1.0, &tpl);
                if (id3 <= 0) std::printf("refused: %s\n", c.takeLastError().c_str());
                SAME(id3 > 0 ? 1.0 : 0.0, 1.0);
                SAME((double)(c.getReceipts(0, 100000).size() - r0), 2.0);
                size_t r1 = c.getReceipts(0, 100000).size();
                Payment a; a.user_id = uid1; a.service_id = svc; a.year = 2026; a.month = 10; a.day = 2; a.amount_due = 10; a.amount_paid = 10; a.status = "paid"; a.direction = "in";
                Payment b2 = a; b2.amount_due = 20; b2.amount_paid = 20;
                SAME(c.addPaymentsWithReceipt({a, b2}, "πακέτο", &tpl, true) > 0 ? 1.0 : 0.0, 1.0);
                SAME((double)(c.getReceipts(0, 100000).size() - r1), 2.0);
            }            // a charge to a client (not joint) has no apartment at all; a joint share always has one
            {
                Payment q; q.user_id = uid1; q.service_id = svc; q.year = 2026; q.month = 10; q.day = 3; q.amount_due = 15; q.amount_paid = 0; q.status = "pending"; q.direction = "in"; q.apartment_id = 0;
                int qid = c.addPaymentsWithReceipt({q}, "", nullptr);
                std::vector<std::string> cols; std::vector<std::vector<std::string>> rows; std::string err;
                c.executeRawQuery("SELECT apartment_id IS NULL FROM charges WHERE id = " + std::to_string(qid), cols, rows, err);
                SAME(!rows.empty() && rows[0][0] == "1" ? 1.0 : 0.0, 1.0);
                c.executeRawQuery("SELECT COUNT(*) FROM charges WHERE batch_id IN (SELECT batch_id FROM joint_ops) AND direction = 'in' AND apartment_id IS NULL", cols, rows, err);
                SAME(!rows.empty() && rows[0][0] == "0" ? 1.0 : 0.0, 1.0);
            }            // a package split between two partners, each line with its own payment now / owed later
            {
                Service s3; s3.label = "ΔΟΚΙΜΗ Γ"; s3.suggested_price = 5; int svc3 = c.addServiceGetId(-1, s3);
                auto owedOf = [&](int pid) { for (auto& ps : c.partnerStats("", "")) if (ps.partner_id == pid) return ps.owedNow; return 0.0; };
                double o1 = owedOf(1), o2 = owedOf(2);
                JointOp o4 = op; o4.label = "Δύο συνεργάτες"; JointShare t1 = s1; t1.paid = 0; JointShare t2 = s2; t2.paid = 0;
                JointLine l1; l1.service_id = svc; l1.total = 60; l1.cost = 10; l1.partner = 1; l1.costPaid = 10;
                JointLine l2; l2.service_id = svc3; l2.total = 40; l2.cost = 25; l2.partner = 2; l2.costPaid = 5;
                SAME(c.chargeJoint(o4, {l1, l2}, {t1, t2}) > 0 ? 1.0 : 0.0, 1.0);
                SAME(owedOf(1) - o1, 0.0); SAME(owedOf(2) - o2, 20.0);
            }            // paying a partner what we owe: moves paid/owed by exactly the amount, never more than the balance
            {
                JointOp o5 = op; o5.label = "Οφειλή προς συνεργάτη"; JointShare u1s = s1; u1s.paid = 0; JointShare u2s = s2; u2s.paid = 0;
                JointLine lq; lq.service_id = svc; lq.total = 100; lq.cost = 40; lq.partner = 1; lq.costPaid = 10;
                int id5 = c.chargeJoint(o5, {lq}, {u1s, u2s});
                int outId = -1; for (auto& j : c.partnerJobs(1, "", "", {}, 0, 100000, 2)) if (j.jointOpId == id5) outId = j.charge_id;
                auto b5 = c.cashNow();
                SAME(c.payOutCharge(outId, 10.0, "2026-10-17") ? 1.0 : 0.0, 1.0);
                auto a5 = c.cashNow();
                SAME(a5.moneyOut - b5.moneyOut, 10.0); SAME(b5.owedOut - a5.owedOut, 10.0);
                SAME(c.payOutCharge(outId, 100.0, "2026-10-17") ? 1.0 : 0.0, 0.0);           // more than we owe: refused
                SAME(c.payOutCharge(outId, 20.0, "2026-10-17") ? 1.0 : 0.0, 1.0);            // the rest: settled
                SAME(c.getPaymentById(outId).balance, 0.0);
            }            JointShare bad = s1; bad.paid = 80;
            SAME(c.chargeJoint(op, {{svc, 100}}, {bad, s2}, 0.0, -1) > 0 ? 1.0 : 0.0, 0.0);         // paid more than the share: refused
            SAME(c.chargeJoint(op, {{svc, 100}}, {s1, s2}, 10.0, -1, 20.0) > 0 ? 1.0 : 0.0, 0.0);    // paid more than the cost: refused
        }
        // scheduled (future) lines count as owed everywhere, on both sides
        {
            Payment fo; fo.user_id = -1; fo.service_id = 1; fo.year = 2030; fo.month = 1; fo.day = 1; fo.amount_due = 55; fo.amount_paid = 0; fo.status = "future"; fo.direction = "out"; fo.partner_id = 2;
            Payment fi; fi.user_id = 1; fi.service_id = 1; fi.year = 2030; fi.month = 1; fi.day = 1; fi.amount_due = 33; fi.amount_paid = 0; fi.status = "future"; fi.direction = "in";
            auto bf = c.cashNow();
            SAME(c.addPaymentsWithReceipt({fo, fi}, "μελλοντικά", nullptr) > 0 ? 1.0 : 0.0, 1.0);
            auto af = c.cashNow();
            SAME(af.owedOut - bf.owedOut, 55.0); SAME(af.owedIn - bf.owedIn, 33.0);
        }        // after every scenario above (unpaid costs on client services and joint charges): all the "we owe" figures must agree
        {
            auto cash2 = c.cashNow();
            auto own2 = c.homeFigures(0, "", ""), jnt2 = c.homeFigures(1, "", "");
            double ps = 0, pr = 0; for (auto& s : c.partnerStats("", "")) ps += s.owedNow;
            for (auto& r : c.pagePartnerRows("", {}, 0, 100000)) pr += r.owed;
            std::printf("owe: cash %.2f | home panels %.2f | partnerStats %.2f | partner list %.2f\n", cash2.owedOut, own2.cost + jnt2.cost - own2.costPaid - jnt2.costPaid, ps, pr);
            SAME(cash2.owedOut, ps);  SAME(cash2.owedOut, pr);
            SAME(cash2.owedOut, MoneySub(own2.cost + jnt2.cost, own2.costPaid + jnt2.costPaid));
            SAME(cash2.moneyOut, own2.costPaid + jnt2.costPaid);
        }        std::printf("clients rated: %lld of %d\n", n, clients);
    }
    std::printf(fails ? "math_test: %d FAILED\n" : "math_test: all consistent\n", fails);
    return fails ? 1 : 0;
}
