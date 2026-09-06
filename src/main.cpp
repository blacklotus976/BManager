#include <memory>
#include <string>
#include <fstream>
#include "SqliteConn.h"
#include "Seed.h"
#include "ui/App.h"
#include "Config.h"

#ifdef _WIN32
#include <windows.h>

// A bare relative "data.db" only works if the process's current working
// directory happens to be the exe's own folder -- true when launched from
// build_imgui.bat, but NOT guaranteed for a plain double-click (Explorer's
// CWD can be inherited from wherever it was last pointed, and some shortcut/
// launch paths set it elsewhere), which is what produced "unable to open
// database file". Resolve paths from the exe's actual location instead.
static std::string DirNextToExe() {
    char exePath[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    if (len == 0 || len == MAX_PATH) return "";
    std::string path(exePath, len);
    size_t slash = path.find_last_of("\\/");
    if (slash == std::string::npos) return "";
    return path.substr(0, slash + 1);
}

// Config file lives next to the exe (not AppData) so copying the whole app
// folder to a USB stick / another PC / a shared release .zip carries the DB
// location setting with it. Format: one line, "db_path=<path>". The stored
// path is deliberately RELATIVE (default "data.db") rather than absolute --
// an absolute path baked in on one machine (e.g. "C:\Users\james\...") would
// silently break the moment the release is unzipped somewhere else on a
// different PC/username. A relative path is always resolved against the
// exe's OWN current folder at startup, wherever that folder happens to be,
// which is exactly what makes "unzip and run" / "share this release" work.
// An absolute path is still honored as-is if someone deliberately sets one
// by hand (e.g. to point at a shared network location) -- resolution just
// doesn't force one.
static std::string ConfigPath() { return DirNextToExe() + "config.ini"; }
static bool IsAbsolutePath(const std::string& p) {
    return p.size() >= 2 && ((p[1] == ':') || (p[0] == '\\' && p[1] == '\\'));
}

static std::string ReadConfiguredDbPath() {
    std::ifstream f(ConfigPath());
    if (!f.is_open()) return "";
    std::string line;
    while (std::getline(f, line)) {
        const std::string prefix = "db_path=";
        if (line.compare(0, prefix.size(), prefix) == 0) {
            std::string val = line.substr(prefix.size());
            // trim trailing CR (files hand-edited on Windows/Unix either way) and whitespace
            while (!val.empty() && (val.back() == '\r' || val.back() == '\n' || val.back() == ' ')) val.pop_back();
            if (val.empty()) return "";
            return IsAbsolutePath(val) ? val : (DirNextToExe() + val);
        }
    }
    return "";
}

// Stores whatever is passed as-is if it's already absolute (an explicit choice
// to point somewhere fixed); otherwise strips this exe's own folder prefix so
// what lands in config.ini stays relative and portable across machines.
void WriteConfiguredDbPath(const std::string& path) {
    std::string toStore = path;
    std::string here = DirNextToExe();
    if (!here.empty() && path.compare(0, here.size(), here) == 0) {
        toStore = path.substr(here.size());
    }
    std::ofstream f(ConfigPath(), std::ios::trunc);
    if (f.is_open()) f << "db_path=" << toStore << "\n";
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    std::string dbPath = ReadConfiguredDbPath();
    if (dbPath.empty()) {
        // No config yet (first run, or a fresh unzip of a shared release with no
        // config.ini carried over) -- default to "data.db" next to wherever this
        // exe currently lives, and write that out as a relative entry so
        // config.ini exists from here on, stays portable, and can be hand-edited
        // or changed via Settings.
        WriteConfiguredDbPath("data.db");
        dbPath = DirNextToExe() + "data.db";
    }
#else
int main() {
    std::string dbPath = "data.db";
#endif
    auto conn = std::make_unique<SqliteConn>(dbPath);
    seedDemoDataIfEmpty(conn.get());

    return RunApp(conn.get(), dbPath);
}
