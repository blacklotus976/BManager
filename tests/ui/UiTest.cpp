// UI test driver (see UiTest.h). A script, one step per line:
//   click / dclick / rclick / hover <target>          field <target> = <text>      type <text>      key <Enter|Tab|Escape|...>
//   clear (select all + delete in the active field)    drag <target> => dx,dy       dragto <target> => <target>
//   scrollto <target>   waitfor <target>   expect <target>   expectno <target>   wait <frames>   shot <name>
//   check <sql> => <expected first value>              log <text>                   quit
// <target> = a widget's label (the part before ##, or the whole label), "~text" = contains; options after " | ":
//   win=<part of the window name>   n=<which match, 1 = first>
// Every frame waits for the previous step's input to land. A target that does not show up within a few seconds fails the step
// (logged with a screenshot) and the script goes on. The log and the screenshots go to BM_UI_OUT.
#include "UiTest.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_glfw.h"
#include <GLFW/glfw3.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <GL/gl.h>
#include "sqlite3.h"
#include <vector>
#include <deque>
#include <string>
#include <fstream>
#include <sstream>
#include <functional>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <unordered_map>
#include <ctime>

namespace uitest {
namespace {
struct Item { ImGuiID id = 0; ImRect bb, clip, win; std::string label, window; ImGuiItemStatusFlags status = 0; bool disabled = false, isWindow = false; ImGuiWindow* wnd = nullptr; };
std::vector<Item> g_cur, g_prev;
std::unordered_map<ImGuiID, std::string> g_labels;      // id -> label (some widgets, e.g. tabs, give their label before they are added)
bool g_active = false, g_ready = false, g_done = false;
GLFWwindow* g_win = nullptr;
std::string g_db, g_out;
std::vector<std::string> g_lines; size_t g_pc = 0;
std::ofstream g_log;
int g_frame = 0, g_pass = 0, g_fail = 0, g_tries = 0, g_wait = 0, g_shotNo = 0;
std::string g_shot;                                   // a screenshot asked for: taken after the next frame is drawn
std::deque<std::function<void(ImGuiIO&)>> g_queue;    // input to give, one entry per frame
ImVec2 g_mouse(-1, -1);
sqlite3* g_rdb = nullptr;

std::string Trim(const std::string& s) { size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n"); return a == std::string::npos ? "" : s.substr(a, b - a + 1); }
void Log(const char* fmt, ...) {
    char b[4096]; va_list ap; va_start(ap, fmt); std::vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    g_log << "[" << g_frame << "] " << b << "\n"; g_log.flush();
}
void Pass(const std::string& what) { g_pass++; Log("OK    %s", what.c_str()); }
void Fail(const std::string& what) { g_fail++; Log("FAIL  %s", what.c_str()); char n[32]; std::snprintf(n, sizeof n, "fail_%02d", g_fail); g_shot = n; }

struct Target { std::string label, win; int n = 1; bool contains = false; };
Target ParseTarget(const std::string& spec) {
    Target t; std::vector<std::string> parts; size_t a = 0;
    while (true) { size_t b = spec.find(" | ", a); parts.push_back(Trim(spec.substr(a, b == std::string::npos ? std::string::npos : b - a))); if (b == std::string::npos) break; a = b + 3; }
    t.label = parts[0];
    if (!t.label.empty() && t.label[0] == '~') { t.contains = true; t.label = t.label.substr(1); }
    for (size_t i = 1; i < parts.size(); i++) {
        if (parts[i].rfind("win=", 0) == 0) t.win = parts[i].substr(4);
        else if (parts[i].rfind("n=", 0) == 0) t.n = std::max(1, std::atoi(parts[i].c_str() + 2));
    }
    return t;
}
bool LabelMatch(const Target& t, const std::string& full) {
    const std::string vis = full.substr(0, full.find("##"));
    if (t.contains) return vis.find(t.label) != std::string::npos;
    return vis == t.label || full == t.label;
}
bool Visible(const Item& it) {
    const ImVec2 c((it.bb.Min.x + it.bb.Max.x) * 0.5f, (it.bb.Min.y + it.bb.Max.y) * 0.5f);
    if (!(it.clip.Contains(c) && it.win.Contains(c) && it.bb.GetWidth() > 0 && it.bb.GetHeight() > 0)) return false;
    if (!it.wnd) return true;
    ImGuiWindow* root = it.wnd->RootWindow;
    // an open modal blocks everything that is not in it (or in a plain popup opened from it, such as a combo's list)
    if (ImGuiWindow* modal = ImGui::GetTopMostPopupModal())
        if (root != modal && !((root->Flags & ImGuiWindowFlags_Popup) && !(root->Flags & ImGuiWindowFlags_Modal))) return false;
    // another window on top of that point (a card, a bill window...) covers it
    ImGuiWindow* top = nullptr; ImGui::FindHoveredWindowEx(c, false, &top, nullptr);
    return !top || top->RootWindow == root;
}
std::string WhyHidden(const Item& it) {
    const ImVec2 c((it.bb.Min.x + it.bb.Max.x) * 0.5f, (it.bb.Min.y + it.bb.Max.y) * 0.5f);
    char b[256];
    if (!it.clip.Contains(c)) { std::snprintf(b, sizeof b, "clipped: centre %.0f,%.0f outside %.0f,%.0f-%.0f,%.0f", c.x, c.y, it.clip.Min.x, it.clip.Min.y, it.clip.Max.x, it.clip.Max.y); return b; }
    if (!it.win.Contains(c)) return "outside its window";
    if (it.wnd) {
        ImGuiWindow* root = it.wnd->RootWindow;
        if (ImGuiWindow* modal = ImGui::GetTopMostPopupModal()) if (root != modal && !((root->Flags & ImGuiWindowFlags_Popup) && !(root->Flags & ImGuiWindowFlags_Modal))) return std::string("behind the modal ") + modal->Name;
        ImGuiWindow* top = nullptr; ImGui::FindHoveredWindowEx(c, false, &top, nullptr);
        if (top && top->RootWindow != root) return std::string("covered by ") + top->Name;
    }
    return "?";
}
// the n-th matching item of the last frame; visibleOnly: only what can be clicked now
// n counts every match on the page in the order drawn (scrolled away or not), so "n=2" always means the same widget
const Item* Find(const Target& t, bool visibleOnly, int* count = nullptr) {
    int k = 0; const Item* hit = nullptr;
    for (auto& it : g_prev) {
        if (it.isWindow || it.label.empty() || !LabelMatch(t, it.label)) continue;      // a window's title is not a button
        if (!t.win.empty() && it.window.find(t.win) == std::string::npos) continue;
        if (++k == t.n && !hit) hit = &it;
    }
    if (count) *count = k;
    if (hit && visibleOnly && !Visible(*hit)) return nullptr;
    return hit;
}
ImVec2 Centre(const Item& it) { return ImVec2(std::floor((it.bb.Min.x + it.bb.Max.x) * 0.5f), std::floor((it.bb.Min.y + it.bb.Max.y) * 0.5f)); }

void MoveTo(ImVec2 p) { g_queue.push_back([p](ImGuiIO& io) { io.AddMousePosEvent(p.x, p.y); g_mouse = p; }); }
void Button(int b, bool down) { g_queue.push_back([b, down](ImGuiIO& io) { io.AddMouseButtonEvent(b, down); }); }
void Idle(int n) { for (int i = 0; i < n; i++) g_queue.push_back([](ImGuiIO&) {}); }
void Key(ImGuiKey k, bool down) { g_queue.push_back([k, down](ImGuiIO& io) { io.AddKeyEvent(k, down); }); }
void Tap(ImGuiKey k) { Key(k, true); Key(k, false); }
void Chord(ImGuiKey mod, ImGuiKey k) { Key(mod, true); Key(k, true); Key(k, false); Key(mod, false); }
void Text(const std::string& s) { g_queue.push_back([s](ImGuiIO& io) { io.AddInputCharactersUTF8(s.c_str()); }); }
void Click(ImVec2 p, int b, int times) { MoveTo(p); Idle(1); for (int i = 0; i < times; i++) { Button(b, true); Button(b, false); } Idle(2); }
ImGuiKey KeyByName(const std::string& n) {
    static const std::pair<const char*, ImGuiKey> k[] = { {"Enter", ImGuiKey_Enter}, {"Tab", ImGuiKey_Tab}, {"Escape", ImGuiKey_Escape}, {"Backspace", ImGuiKey_Backspace},
        {"Delete", ImGuiKey_Delete}, {"Up", ImGuiKey_UpArrow}, {"Down", ImGuiKey_DownArrow}, {"Left", ImGuiKey_LeftArrow}, {"Right", ImGuiKey_RightArrow},
        {"Home", ImGuiKey_Home}, {"End", ImGuiKey_End}, {"PageUp", ImGuiKey_PageUp}, {"PageDown", ImGuiKey_PageDown}, {"Space", ImGuiKey_Space} };
    for (auto& [a, b] : k) if (n == a) return b;
    return ImGuiKey_None;
}
std::string Query(const std::string& sql, bool& ok) {
    ok = false;
    if (!g_rdb && sqlite3_open_v2(g_db.c_str(), &g_rdb, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) { std::string e = sqlite3_errmsg(g_rdb); sqlite3_close(g_rdb); g_rdb = nullptr; return "open: " + e; }
    sqlite3_busy_timeout(g_rdb, 3000);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(g_rdb, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return std::string("sql: ") + sqlite3_errmsg(g_rdb);
    std::string v = "(no row)";
    if (sqlite3_step(st) == SQLITE_ROW) { const unsigned char* t = sqlite3_column_text(st, 0); v = t ? (const char*)t : "NULL"; }
    sqlite3_finalize(st); ok = true;
    return v;
}
// --- PNG (stored deflate: big files, no dependencies)
unsigned Crc(const unsigned char* p, size_t n, unsigned c = 0xffffffffu) {
    static unsigned t[256]; static bool init = false;
    if (!init) { for (unsigned i = 0; i < 256; i++) { unsigned x = i; for (int k = 0; k < 8; k++) x = x & 1 ? 0xedb88320u ^ (x >> 1) : x >> 1; t[i] = x; } init = true; }
    for (size_t i = 0; i < n; i++) c = t[(c ^ p[i]) & 255] ^ (c >> 8);
    return c;
}
void WritePng(const std::string& path, int w, int h, const std::vector<unsigned char>& rgb) {
    std::vector<unsigned char> raw; raw.reserve((size_t)(w * 3 + 1) * h);
    for (int y = 0; y < h; y++) { raw.push_back(0); raw.insert(raw.end(), rgb.begin() + (size_t)y * w * 3, rgb.begin() + (size_t)(y + 1) * w * 3); }
    std::vector<unsigned char> z = { 0x78, 0x01 }; unsigned a = 1, b = 0;
    for (unsigned char c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    for (size_t pos = 0; pos < raw.size(); ) {
        const size_t n = std::min<size_t>(65535, raw.size() - pos); const bool last = pos + n == raw.size();
        z.push_back(last ? 1 : 0); z.push_back(n & 255); z.push_back((n >> 8) & 255); z.push_back(~n & 255); z.push_back((~n >> 8) & 255);
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n); pos += n;
    }
    const unsigned ad = (b << 16) | a; for (int i = 3; i >= 0; i--) z.push_back((ad >> (i * 8)) & 255);
    std::ofstream f(path, std::ios::binary);
    auto be = [&](unsigned v) { unsigned char q[4] = { (unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v }; f.write((const char*)q, 4); };
    auto chunk = [&](const char* type, const std::vector<unsigned char>& d) {
        be((unsigned)d.size()); std::vector<unsigned char> td(type, type + 4); td.insert(td.end(), d.begin(), d.end());
        f.write((const char*)td.data(), td.size()); be(Crc(td.data(), td.size()) ^ 0xffffffffu); };
    const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 }; f.write((const char*)sig, 8);
    std::vector<unsigned char> ih = { (unsigned char)(w >> 24), (unsigned char)(w >> 16), (unsigned char)(w >> 8), (unsigned char)w,
                                      (unsigned char)(h >> 24), (unsigned char)(h >> 16), (unsigned char)(h >> 8), (unsigned char)h, 8, 2, 0, 0, 0 };
    chunk("IHDR", ih); chunk("IDAT", z); chunk("IEND", {});
}

// {d} today's day (no leading zero), {ym} YYYY-MM, {ymd} YYYY-MM-DD
std::string Expand(std::string s) {
    std::time_t t = std::time(nullptr); std::tm tm{}; localtime_s(&tm, &t);
    char d[8], ym[16], ymd[16]; std::snprintf(d, sizeof d, "%d", tm.tm_mday); std::strftime(ym, sizeof ym, "%Y-%m", &tm); std::strftime(ymd, sizeof ymd, "%Y-%m-%d", &tm);
    for (auto [k, v] : { std::pair<const char*, const char*>{"{d}", d}, {"{ym}", ym}, {"{ymd}", ymd} })
        for (size_t at; (at = s.find(k)) != std::string::npos; ) s.replace(at, std::strlen(k), v);
    return s;
}
// one step: true = done (passed or failed), false = try again next frame (its target is not there yet)
bool Step(const std::string& rawLine) {
    const std::string line = Expand(rawLine);
    std::string cmd = line, rest; size_t sp = line.find(' ');
    if (sp != std::string::npos) { cmd = line.substr(0, sp); rest = Trim(line.substr(sp + 1)); }
    const int kMaxTries = 300;
    auto need = [&](const Target& t, const Item*& it) -> int {           // 1 found, 0 keep waiting, 2 failed
        it = Find(t, true);
        if (it) return 1;
        if (++g_tries < kMaxTries) return 0;
        int cnt = 0; const Item* any = Find(t, false, &cnt);
        Fail(cmd + " '" + t.label + "': " + (cnt ? "exists but not clickable: " + (any ? WhyHidden(*any) : std::string("the n-th is missing")) : std::string("not found")));
        return 2;
    };
    if (cmd == "wait") { g_wait = std::max(1, std::atoi(rest.c_str())); return true; }
    if (cmd == "log") { Log("----  %s", rest.c_str()); return true; }
    if (cmd == "shot") { g_shot = rest; return true; }
    if (cmd == "quit") { g_done = true; return true; }
    if (cmd == "type") { Text(rest); Idle(1); Pass("type '" + rest + "'"); return true; }
    if (cmd == "key") { const ImGuiKey k = KeyByName(rest); if (k == ImGuiKey_None) { Fail("key: unknown " + rest); return true; } Tap(k); Idle(1); Pass("key " + rest); return true; }
    if (cmd == "clear") { Chord(ImGuiMod_Ctrl, ImGuiKey_A); Tap(ImGuiKey_Backspace); Idle(1); Pass("clear"); return true; }
    if (cmd == "check") {
        const size_t arrow = rest.rfind(" => ");
        if (arrow == std::string::npos) { Fail("check: no ' => '"); return true; }
        const std::string sql = Trim(rest.substr(0, arrow)), want = Trim(rest.substr(arrow + 4));
        bool ok = false; const std::string got = Query(sql, ok);
        if (ok && got == want) Pass("check " + sql + " => " + got); else Fail("check " + sql + " => got '" + got + "', want '" + want + "'");
        return true;
    }
    if (cmd == "expect" || cmd == "waitfor") { const Target t = ParseTarget(rest); const Item* it = nullptr; { const int r = need(t, it); if (r != 1) return r == 2; } Pass(cmd + " '" + t.label + "'"); return true; }
    if (cmd == "expectno") {
        const Target t = ParseTarget(rest);
        if (!Find(t, true)) { Pass("expectno '" + t.label + "'"); return true; }
        if (++g_tries < 30) return false;
        Fail("expectno '" + t.label + "': it is there"); return true;
    }
    if (cmd == "click" || cmd == "dclick" || cmd == "rclick" || cmd == "hover") {
        const Target t = ParseTarget(rest); const Item* it = nullptr;
        { const int r = need(t, it); if (r != 1) return r == 2; }
        if (it->disabled) { if (++g_tries < 60) return false; Fail(cmd + " '" + t.label + "': disabled"); return true; }
        const ImVec2 c = Centre(*it);
        if (cmd == "hover") { MoveTo(c); Idle(3); }
        else Click(c, cmd == "rclick" ? 1 : 0, cmd == "dclick" ? 2 : 1);
        Pass(cmd + " '" + t.label + "' at " + std::to_string((int)c.x) + "," + std::to_string((int)c.y) + " in " + it->window);
        return true;
    }
    if (cmd == "field") {
        const size_t eq = rest.find(" = ");
        const Target t = ParseTarget(eq == std::string::npos ? rest : rest.substr(0, eq));
        const std::string text = eq == std::string::npos ? std::string() : rest.substr(eq + 3);
        const Item* it = nullptr;
        { const int r = need(t, it); if (r != 1) return r == 2; }
        Click(Centre(*it), 0, 1); Chord(ImGuiMod_Ctrl, ImGuiKey_A); Tap(ImGuiKey_Backspace);
        if (!text.empty()) Text(text);
        Idle(1); Tap(ImGuiKey_Enter); Idle(2);
        Pass("field '" + t.label + "' = '" + text + "'");
        return true;
    }
    if (cmd == "drag" || cmd == "dragto") {
        const size_t arrow = rest.find(" => ");
        if (arrow == std::string::npos) { Fail(cmd + ": no ' => '"); return true; }
        const Target t = ParseTarget(rest.substr(0, arrow)); const Item* it = nullptr;
        { const int r = need(t, it); if (r != 1) return r == 2; }
        const ImVec2 a = Centre(*it); ImVec2 b = a;
        if (cmd == "drag") { float dx = 0, dy = 0; std::sscanf(rest.substr(arrow + 4).c_str(), "%f,%f", &dx, &dy); b = ImVec2(a.x + dx, a.y + dy); }
        else { const Item* to = Find(ParseTarget(rest.substr(arrow + 4)), true); if (!to) { Fail("dragto: target not found"); return true; } b = Centre(*to); }
        MoveTo(a); Idle(1); Button(0, true); Idle(1);
        const int steps = 24;
        for (int i = 1; i <= steps; i++) MoveTo(ImVec2(a.x + (b.x - a.x) * i / steps, a.y + (b.y - a.y) * i / steps));
        Idle(2); Button(0, false); Idle(3);
        Pass(cmd + " '" + t.label + "'");
        return true;
    }
    if (cmd == "vtx") {             // the biggest draw lists of the last frame (an OpenGL 3.0 context draws at most 65536 vertices per list)
        ImDrawData* dd = ImGui::GetDrawData();
        if (!dd) { Fail("vtx: no draw data"); return true; }
        std::vector<std::pair<int, std::string>> v;
        for (ImDrawList* l : dd->CmdLists) v.push_back({ l->VtxBuffer.Size, l->_OwnerName ? l->_OwnerName : "?" });
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
        for (size_t i = 0; i < v.size() && i < 4; i++) Log("      %d vertices: %s", v[i].first, v[i].second.c_str());
        const int limit = rest.empty() ? 65536 : std::atoi(rest.c_str());
        if (!v.empty() && v[0].first >= limit) Fail("vtx: " + std::to_string(v[0].first) + " vertices in " + v[0].second); else Pass("vtx under " + std::to_string(limit));
        return true;
    }
    if (cmd == "list") {            // every match of a target, in order: for writing scripts
        const Target t = ParseTarget(rest); int k = 0;
        for (auto& it : g_prev) {
            if (it.isWindow || it.label.empty() || !LabelMatch(t, it.label)) continue;
            if (!t.win.empty() && it.window.find(t.win) == std::string::npos) continue;
            Log("      #%d '%s' id %08X at %.0f,%.0f-%.0f,%.0f %s in %s", ++k, it.label.c_str(), it.id, it.bb.Min.x, it.bb.Min.y, it.bb.Max.x, it.bb.Max.y, Visible(it) ? "visible" : "hidden", it.window.c_str());
        }
        Pass("list '" + t.label + "': " + std::to_string(k)); return true;
    }
    if (cmd == "press") {           // mouse down on a widget and keep it down (then moveby / release)
        const Target t = ParseTarget(rest); const Item* it = nullptr;
        { const int r = need(t, it); if (r != 1) return r == 2; }
        MoveTo(Centre(*it)); Idle(1); Button(0, true); Idle(1);
        Pass("press '" + t.label + "'"); return true;
    }
    if (cmd == "moveby") {          // moveby dx,dy[,frames]: the mouse moves that far in small steps
        float dx = 0, dy = 0; int frames = 12; std::sscanf(rest.c_str(), "%f,%f,%d", &dx, &dy, &frames);
        const ImVec2 a = g_mouse;
        for (int i = 1; i <= std::max(1, frames); i++) MoveTo(ImVec2(a.x + dx * i / frames, a.y + dy * i / frames));
        Idle(1); Pass("moveby " + rest); return true;
    }
    if (cmd == "release") { Button(0, false); Idle(2); Pass("release"); return true; }
    if (cmd == "scrollto") {
        const Target t = ParseTarget(rest);
        if (Find(t, true)) { Pass("scrollto '" + t.label + "'"); return true; }
        const Item* it = Find(t, false);
        const Item* inWin = it;                               // not drawn at all (scrolled far away): scroll the window named by win=
        if (!inWin && !t.win.empty()) for (auto& x : g_prev) if (x.window.find(t.win) != std::string::npos) { inWin = &x; break; }
        if (!inWin) { if (++g_tries < kMaxTries) return false; Fail("scrollto '" + t.label + "': not found (give win= to scroll a window)"); return true; }
        if (++g_tries > 120) { Fail("scrollto '" + t.label + "': cannot bring it into view"); return true; }
        // the wheel goes over the visible part of the nearest window that can scroll up and down
        ImGuiWindow* sw = inWin->wnd;
        while (sw && sw->ScrollMax.y <= 0.0f && sw->ParentWindow) sw = sw->ParentWindow;
        const ImRect vis = sw ? sw->InnerClipRect : inWin->win;
        const ImVec2 p((vis.Min.x + vis.Max.x) * 0.5f, (vis.Min.y + vis.Max.y) * 0.5f);
        const float dir = it && it->bb.Max.y < vis.Min.y ? 1.0f : -1.0f;
        if (g_tries % 20 == 1) Log("      scroll %s (%.0f,%.0f)-(%.0f,%.0f) scrollY %.0f/%.0f dir %.0f item %s y %.0f", sw ? sw->Name : "?", vis.Min.x, vis.Min.y, vis.Max.x, vis.Max.y,
                                   sw ? sw->Scroll.y : 0.0f, sw ? sw->ScrollMax.y : 0.0f, dir, it ? "found" : "not drawn", it ? it->bb.Min.y : 0.0f);
        g_queue.push_back([p, dir](ImGuiIO& io) { io.AddMousePosEvent(p.x, p.y); io.AddMouseWheelEvent(0.0f, dir); });
        Idle(1);
        return false;
    }
    Fail("unknown step: " + line);
    return true;
}
} // namespace

bool Active() { static int a = -1; if (a < 0) a = std::getenv("BM_UI_SCRIPT") ? 1 : 0; return a == 1; }
void BeforeWindow() {
    if (!Active()) return;
    glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);        // never takes the focus from whatever the user is doing
    glfwWindowHint(GLFW_FOCUSED, GLFW_FALSE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_FALSE);
}
void Init(GLFWwindow* w, const std::string& dbPath) {
    if (!Active()) return;
    g_active = true; g_win = w; g_db = dbPath;
    const char* out = std::getenv("BM_UI_OUT"); g_out = out ? out : ".";
    g_log.open(g_out + "/uitest.log", std::ios::trunc);
    std::ifstream f(std::getenv("BM_UI_SCRIPT"));
    for (std::string l; std::getline(f, l); ) { l = Trim(l); if (!l.empty() && l[0] != '#') g_lines.push_back(l); }
    Log("script %s: %d steps, db %s", std::getenv("BM_UI_SCRIPT"), (int)g_lines.size(), dbPath.c_str());
    ImGui_ImplGlfw_RestoreCallbacks(w);                      // the real mouse and keyboard no longer reach the app
    glfwSwapInterval(0);
    int sw = 1600, sh = 960; if (const char* s = std::getenv("BM_UI_SIZE")) std::sscanf(s, "%dx%d", &sw, &sh);
    glfwSetWindowSize(w, sw, sh);
    g_ready = true;
}
void NewFrame(GLFWwindow* w) {
    ImGuiIO& io = ImGui::GetIO();
    int ww = 0, wh = 0, fw = 0, fh = 0; glfwGetWindowSize(w, &ww, &wh); glfwGetFramebufferSize(w, &fw, &fh);
    io.DisplaySize = ImVec2((float)ww, (float)wh);
    if (ww > 0 && wh > 0) io.DisplayFramebufferScale = ImVec2((float)fw / ww, (float)fh / wh);
    io.DeltaTime = 1.0f / 60.0f;                              // the same speed however fast the machine draws
    GImGui->TestEngineHookItems = true;
    g_prev.swap(g_cur); g_cur.clear();
    g_frame++;
    if (!g_ready) return;
    if (!g_queue.empty()) { auto f = g_queue.front(); g_queue.pop_front(); f(io); return; }
    if (!g_shot.empty()) return;                              // drawn and saved first
    if (g_wait > 0) { g_wait--; return; }
    if (g_done || g_pc >= g_lines.size()) {
        if (!g_done) g_done = true;
        static bool closing = false;
        if (!closing) { closing = true; Log("DONE  passed %d, failed %d", g_pass, g_fail); glfwSetWindowShouldClose(w, 1); }
        return;
    }
    const std::string& line = g_lines[g_pc];
    if (g_tries == 0) Log("      %s", line.c_str());
    if (Step(line)) { g_pc++; g_tries = 0; }                 // done (passed or failed): the next step
}
void AfterRender(GLFWwindow* w) {
    if (!g_active || g_shot.empty() || !g_queue.empty()) return;
    int fw = 0, fh = 0; glfwGetFramebufferSize(w, &fw, &fh);
    if (fw <= 0 || fh <= 0) return;
    std::vector<unsigned char> px((size_t)fw * fh * 3), flip((size_t)fw * fh * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1); glReadBuffer(GL_BACK);
    glReadPixels(0, 0, fw, fh, GL_RGB, GL_UNSIGNED_BYTE, px.data());
    for (int y = 0; y < fh; y++) std::memcpy(&flip[(size_t)y * fw * 3], &px[(size_t)(fh - 1 - y) * fw * 3], (size_t)fw * 3);
    char n[16]; std::snprintf(n, sizeof n, "%03d_", ++g_shotNo);
    const std::string path = g_out + "/" + n + g_shot + ".png";
    WritePng(path, fw, fh, flip);
    Log("SHOT  %s", path.c_str());
    g_shot.clear();
}
void Note(const char* label, const ImVec2& mn, const ImVec2& mx) {
    if (!g_active || !label || !*label) return;
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    Item it; it.bb = ImRect(mn, mx); it.clip = win ? win->ClipRect : it.bb; it.win = win ? win->Rect() : it.bb; it.label = label; it.window = win ? win->Name : ""; it.wnd = win;
    g_cur.push_back(it);
}
} // namespace uitest

// ---- Dear ImGui test-engine hooks: every item with an id, and the label of those that have one
void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& bb, const ImGuiLastItemData* item) {
    ImGuiWindow* w = ctx->CurrentWindow;
    uitest::Item it; it.id = id; it.bb = bb; it.clip = w ? w->ClipRect : bb; it.win = w ? w->Rect() : bb; it.window = w ? w->Name : ""; it.wnd = w;
    if (item) { it.status = item->StatusFlags; it.disabled = (item->ItemFlags & ImGuiItemFlags_Disabled) != 0; }
    else it.isWindow = true;                                  // windows register themselves without item data
    if (auto l = uitest::g_labels.find(id); l != uitest::g_labels.end()) it.label = l->second;
    uitest::g_cur.push_back(it);
}
void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags flags) {
    if (label) uitest::g_labels[id] = label;
    for (auto it = uitest::g_cur.rbegin(); it != uitest::g_cur.rend(); ++it) if (it->id == id) { if (label) it->label = label; it->status |= flags; return; }
}
void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}
const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) { return nullptr; }
