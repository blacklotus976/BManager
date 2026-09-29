#include "Persistence.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <windows.h>

using json = nlohmann::json;

std::string DefaultStatePath() {
    char exePath[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return "stockviewer_state.json";
    std::string p(exePath, len);
    size_t slash = p.find_last_of("\\/");
    if (slash == std::string::npos) return "stockviewer_state.json";
    return p.substr(0, slash + 1) + "stockviewer_state.json";
}

bool LoadState(const std::string& path, SavedState& out) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    try {
        json j; f >> j;
        out.version = j.value("version", 1);
        out.activeTab = j.value("activeTab", 0);

        out.stocks.clear();
        if (j.contains("stocks") && j["stocks"].is_array()) {
            for (auto& s : j["stocks"]) {
                SavedStock ss;
                ss.providerId   = s.value("provider", std::string());
                ss.ticker       = s.value("ticker", std::string());
                ss.entryPrice   = s.value("entryPrice", 0.0);
                ss.feePercent   = s.value("feePercent", 0.25);
                ss.targetRoiPct = s.value("targetRoiPct", 10.0);
                ss.timeframeId  = s.value("timeframe", std::string());
                ss.visible      = s.value("visible", true);
                ss.addedAtMs    = s.value("addedAtMs", (int64_t)0);
                if (!ss.providerId.empty() && !ss.ticker.empty())
                    out.stocks.push_back(std::move(ss));
            }
        }

        out.apiKeys.clear();
        if (j.contains("apiKeys") && j["apiKeys"].is_object()) {
            for (auto it = j["apiKeys"].begin(); it != j["apiKeys"].end(); ++it) {
                out.apiKeys.emplace_back(it.key(), it.value().get<std::string>());
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool SaveState(const std::string& path, const SavedState& s) {
    json j;
    j["version"] = s.version;
    j["activeTab"] = s.activeTab;
    j["stocks"] = json::array();
    for (auto& st : s.stocks) {
        j["stocks"].push_back({
            {"provider",    st.providerId},
            {"ticker",      st.ticker},
            {"entryPrice",  st.entryPrice},
            {"feePercent",  st.feePercent},
            {"targetRoiPct",st.targetRoiPct},
            {"timeframe",   st.timeframeId},
            {"visible",     st.visible},
            {"addedAtMs",   st.addedAtMs}
        });
    }
    j["apiKeys"] = json::object();
    for (auto& [k, v] : s.apiKeys) j["apiKeys"][k] = v;

    std::ofstream f(path, std::ios::trunc);
    if (!f.is_open()) return false;
    f << j.dump(2);
    return true;
}