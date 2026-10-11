// Attachments: content-addressed copies next to the DB, never deleted, removal only with a reason, never on receipts.
#include "SqliteConn.h"
#include <sqlite3.h>
#include <cstdio>
#include <filesystem>
#include <fstream>

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } } while (0)

int main() {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "bmanager_attachments_test";
    fs::remove_all(dir); fs::create_directories(dir / "incoming");
    { std::ofstream(dir / "incoming" / "απόδειξη.jpg", std::ios::binary) << std::string(5000, 'x'); }
    { std::ofstream(dir / "incoming" / "copy.jpg", std::ios::binary) << std::string(5000, 'x'); }       // same bytes
    { std::ofstream(dir / "incoming" / "video.mp4", std::ios::binary) << std::string(100, 'v'); }
    std::string db = (dir / "data.db").string();
    {
        SqliteConn c(db);
        User u; u.full_name = "Π"; c.addUser(u);
        Service s; s.label = "Σ"; s.suggested_price = 10; int svc = c.addServiceGetId(1, s);
        Payment p; p.user_id = 1; p.service_id = svc; p.year = 2026; p.month = 1; p.day = 1; p.amount_due = 10; p.amount_paid = 10; p.status = "paid";
        Receipt r; r.user_id = 1; r.method = "cash";
        int line = c.addPaymentsWithReceipt({p}, "", &r);
        int rid = c.getReceiptIdForPayment(line);
        CHECK(rid > 0);

        int a1 = c.addAttachment("receipt", rid, (dir / "incoming" / "απόδειξη.jpg").u8string(), "σκαναρισμένη");
        int a2 = c.addAttachment("client", 1, (dir / "incoming" / "copy.jpg").u8string(), "");
        int a3 = c.addAttachment("charge", line, (dir / "incoming" / "video.mp4").u8string(), "");
        CHECK(a1 > 0 && a2 > 0 && a3 > 0);
        CHECK(c.addAttachment("client", 1, (dir / "incoming" / "missing.jpg").u8string(), "") == -1); c.takeLastError();
        auto ra = c.getAttachments("receipt", rid);
        CHECK(ra.size() == 1 && ra[0].kind == "image" && ra[0].file_name == "απόδειξη.jpg" && ra[0].size_bytes == 5000);
        CHECK(c.getAttachments("charge", line)[0].kind == "video");
        // identical files share one stored copy
        CHECK(ra[0].stored_path == c.getAttachments("client", 1)[0].stored_path);
        CHECK(fs::exists(fs::u8path(c.attachmentFullPath(ra[0]))));
        // the original can disappear; the attachment stays
        fs::remove(dir / "incoming" / "απόδειξη.jpg");
        CHECK(fs::exists(fs::u8path(c.attachmentFullPath(ra[0]))));

        // receipt evidence can't be removed; others only with a reason; never deleted
        CHECK(!c.removeAttachment(a1, "λάθος")); c.takeLastError();
        CHECK(!c.removeAttachment(a2, "")); c.takeLastError();
        CHECK(c.removeAttachment(a2, "διπλή φωτογραφία"));
        CHECK(c.getAttachments("client", 1).empty());
        CHECK(c.getAttachments("client", 1, true).size() == 1);
        CHECK(!c.removeAttachment(a2, "ξανά")); c.takeLastError();               // already removed: final
        CHECK(c.verifyAuditLog().ok);
    }
    {   // a stored file that disappears is reported by the integrity check
        SqliteConn c(db);
        auto list = c.getAttachments("charge", 1);
        if (!list.empty()) std::filesystem::remove(std::filesystem::u8path(c.attachmentFullPath(list[0])));
        auto chk = c.verifyAuditLog();
        bool missing = false; for (auto& p : chk.problems) if (p.find("συνημμένο") != std::string::npos) missing = true;
        CHECK(!chk.ok && missing);
    }
    {   // outside tools can't delete or rewrite attachment rows
        sqlite3* d = nullptr; sqlite3_open(db.c_str(), &d);
        CHECK(sqlite3_exec(d, "DELETE FROM attachments", nullptr, nullptr, nullptr) != SQLITE_OK);
        CHECK(sqlite3_exec(d, "UPDATE attachments SET sha256 = 'x'", nullptr, nullptr, nullptr) != SQLITE_OK);
        sqlite3_close(d);
    }
    fs::remove_all(dir);
    std::printf(fails ? "%d FAILED\n" : "all attachment tests passed\n", fails);
    return fails ? 1 : 0;
}
