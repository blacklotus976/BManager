#pragma once
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>
#include "Platform.h"

// Errors.csv: one line per error the app met (time, source, message), next to the database in logs/.
// Plain CSV (UTF-8 with BOM so Excel shows Greek), append-only, readable from Settings -> "Αρχείο σφαλμάτων".
namespace errlog {

inline std::string& Path() { static std::string p; return p; }
inline std::mutex& Mutex() { static std::mutex m; return m; }

inline void SetFolder(const std::string& dbFolder) {
    namespace fs = std::filesystem;
    fs::path dir = fs::u8path(dbFolder.empty() ? "." : dbFolder) / "logs";
    std::error_code ec; fs::create_directories(dir, ec);
    Path() = (dir / "errors.csv").u8string();
}

inline std::string CsvField(const std::string& s) {
    std::string out = "\"";
    for (char c : s) { if (c == '"') out += "\"\""; else if (c == '\r') continue; else out += c; }
    return out + "\"";
}

inline void Write(const std::string& source, const std::string& message) {
    if (Path().empty() || message.empty()) return;
    std::lock_guard<std::mutex> lk(Mutex());
    namespace fs = std::filesystem;
    std::error_code ec;
    bool fresh = !fs::exists(fs::u8path(Path()), ec) || fs::file_size(fs::u8path(Path()), ec) == 0;
    std::ofstream f(fs::u8path(Path()), std::ios::app | std::ios::binary);
    if (!f) return;
    if (fresh) f << "\xEF\xBB\xBF" "time,source,message\n";
    std::time_t t = std::time(nullptr); std::tm tm{}; localtime_s(&tm, &t);
    char ts[32]; std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
    f << ts << ',' << CsvField(source) << ',' << CsvField(message) << '\n';
}

struct Row { std::string time, source, message; };

// Newest first, at most `limit` rows.
inline std::vector<Row> ReadAll(size_t limit = 5000) {
    std::vector<Row> rows;
    std::ifstream f(std::filesystem::u8path(Path()), std::ios::binary);
    if (!f) return rows;
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.rfind("\xEF\xBB\xBF", 0) == 0) data.erase(0, 3);
    std::vector<std::string> fields; std::string cur; bool q = false, header = true;
    auto endRow = [&]() {
        fields.push_back(cur); cur.clear();
        if (header) header = false;
        else if (fields.size() >= 3) rows.push_back({fields[0], fields[1], fields[2]});
        fields.clear();
    };
    for (size_t i = 0; i < data.size(); i++) {
        char c = data[i];
        if (q) {
            if (c == '"') { if (i + 1 < data.size() && data[i + 1] == '"') { cur += '"'; i++; } else q = false; }
            else cur += c;
        } else if (c == '"') q = true;
        else if (c == ',') { fields.push_back(cur); cur.clear(); }
        else if (c == '\n') endRow();
        else if (c != '\r') cur += c;
    }
    if (!cur.empty() || !fields.empty()) endRow();
    std::vector<Row> out(rows.rbegin(), rows.rend());
    if (out.size() > limit) out.resize(limit);
    return out;
}

} // namespace errlog
