#include "PluginLoader.h"
#include <windows.h>
#include <filesystem>

using CreateFn  = IMarketDataProvider*(*)();
using VersionFn = const char*(*)();
using NameFn    = const char*(*)();

static std::vector<HMODULE> g_loadedModules;

std::vector<std::unique_ptr<IMarketDataProvider>>
LoadPluginsFrom(const std::string& dir, std::vector<std::string>& outLog) {
    std::vector<std::unique_ptr<IMarketDataProvider>> out;
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(dir, ec)) { outLog.push_back("Δεν υπάρχει φάκελος plugins: " + dir); return out; }

    for (auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".dll") continue;

        std::wstring wpath = entry.path().wstring();
        HMODULE h = LoadLibraryW(wpath.c_str());
        if (!h) {
            outLog.push_back("Αποτυχία φόρτωσης: " + entry.path().filename().string());
            continue;
        }

        auto verFn = (VersionFn)GetProcAddress(h, "StockViewer_PluginApiVersion");
        auto createFn = (CreateFn)GetProcAddress(h, "StockViewer_CreateProvider");
        auto nameFn = (NameFn)GetProcAddress(h, "StockViewer_PluginName");

        if (!verFn || !createFn) {
            outLog.push_back("Μη έγκυρο plugin (λείπουν exports): " + entry.path().filename().string());
            FreeLibrary(h);
            continue;
        }
        std::string ver = verFn();
        if (ver != STOCKVIEWER_PLUGIN_API_VERSION) {
            outLog.push_back("Λάθος έκδοση API (" + ver + " ≠ " +
                             STOCKVIEWER_PLUGIN_API_VERSION + "): " +
                             entry.path().filename().string());
            FreeLibrary(h);
            continue;
        }
        IMarketDataProvider* p = createFn();
        if (!p) {
            outLog.push_back("CreateProvider επέστρεψε null: " + entry.path().filename().string());
            FreeLibrary(h);
            continue;
        }
        std::string name = nameFn ? nameFn() : entry.path().filename().string();
        outLog.push_back("Φορτώθηκε: " + name + " (" + p->Id() + ")");
        g_loadedModules.push_back(h);
        out.emplace_back(p);
    }
    return out;
}

void UnloadAllPlugins() {
    for (auto h : g_loadedModules) FreeLibrary(h);
    g_loadedModules.clear();
}