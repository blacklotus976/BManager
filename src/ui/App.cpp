// ImGui/GLFW/OpenGL3 UI shell for PropertyManager, replacing the old FLTK UI.
// Single translation unit by design (small app, keeps the FLTK->ImGui swap easy to review).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "Platform.h"
#include "App.h"
#include "DataConn.h"
#include "SqliteConn.h"
#include "models/User.h"
#include "models/Service.h"
#include "models/MasterService.h"
#include "models/Payment.h"
#include "models/Note.h"
#include "Config.h"
#include "Money.h"
#include "TextFold.h"
#include "Rf.h"
#include "Seed.h"
#include "imgui.h"
#if IMGUI_VERSION_NUM >= 19100 && !defined(ImGuiChildFlags_Border)
#define ImGuiChildFlags_Border ImGuiChildFlags_Borders   // renamed in Dear ImGui 1.91
#endif
#include "imgui_impl_glfw.h"
#include "imgui_internal.h"
#include "imgui_impl_opengl3.h"
#ifdef _WIN32
#include <windows.h>
#include <GL/gl.h>
#include <shobjidl.h>
#endif
#include <GLFW/glfw3.h>

#include <vector>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <string>
#include <set>
#include <map>
#include <unordered_map>
#include <cstdio>
#include <cstring>
#include <functional>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <ctime>


#include "DatePicker.h"
#include "JointKinds.h"
#include "ErrorLog.h"  
#include "MediaViewer.h"


// ---------------------------------------------------------------------------
// Small shared helpers
// ---------------------------------------------------------------------------
static void GlfwErrorCallback(int error, const char* description) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
    errlog::Write("graphics", "GLFW " + std::to_string(error) + ": " + (description ? description : ""));
}

static std::string CurrentTimestampString() {
    auto t = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

static std::string CurrentDateString() {
    auto t = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tmv);
    return buf;
}

static std::string CurrentTimestampFolder() {
    auto t = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    char buf[64];
    std::strftime(buf, sizeof(buf), "backup_%Y%m%d_%H%M%S", &tmv);
    return buf;
}

#ifdef _WIN32
// Native "Select Folder" dialog via the modern COM-based IFileDialog (with
// FOS_PICKFOLDERS), not the legacy/dated SHBrowseForFolder. No COM init was
// found anywhere else in this app (checked main.cpp / App.cpp), so this
// function owns its own CoInitializeEx/CoUninitialize pair, scoped tightly
// around the dialog call. Returns empty string if the user cancels or the
// dialog fails for any reason.
static std::string PickFolderDialogWin32() {
    std::string result;
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool weInitialized = SUCCEEDED(hrInit);
    // RPC_E_CHANGED_MODE means COM was already initialized on this thread with a
    // different concurrency model -- that's fine, we just don't own it and must
    // not CoUninitialize what we didn't initialize.
    if (FAILED(hrInit) && hrInit != RPC_E_CHANGED_MODE) return result;

    IFileDialog* pFileDialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFileDialog));
    if (SUCCEEDED(hr)) {
        DWORD opts = 0;
        pFileDialog->GetOptions(&opts);
        pFileDialog->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        pFileDialog->SetTitle(L"Επιλογή φακέλου για αντίγραφο ασφαλείας (CSV)");
        hr = pFileDialog->Show(nullptr);
        if (SUCCEEDED(hr)) {
            IShellItem* pItem = nullptr;
            if (SUCCEEDED(pFileDialog->GetResult(&pItem))) {
                PWSTR wpath = nullptr;
                if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &wpath))) {
                    int len = WideCharToMultiByte(CP_UTF8, 0, wpath, -1, nullptr, 0, nullptr, nullptr);
                    if (len > 0) {
                        std::string buf(len, '\0');
                        WideCharToMultiByte(CP_UTF8, 0, wpath, -1, buf.data(), len, nullptr, nullptr);
                        result = buf.c_str(); // trim the trailing null WideCharToMultiByte counted
                    }
                    CoTaskMemFree(wpath);
                }
                pItem->Release();
            }
        }
        pFileDialog->Release();
    }
    if (weInitialized) CoUninitialize();
    return result;
}
#endif

// ---------------------------------------------------------------------------
// Confirm-before-write modal: shows old -> new field diffs and requires an
// explicit confirm before the write actually happens. Used by every
// add/edit/delete flow, per the existing client requirement (carried
// forward from the FLTK version).
// ---------------------------------------------------------------------------
struct FieldDiff { std::string field, oldVal, newVal; };
struct PendingConfirm {
    bool requested = false;
    bool open = false;
    std::string title;
    std::vector<FieldDiff> diffs;
    std::function<void()> onConfirm;
};
static PendingConfirm g_confirm;

static void DiffAdd(std::vector<FieldDiff>& d, const char* field, const std::string& a, const std::string& b) {
    if (a != b) d.push_back({field, a.empty() ? "-" : a, b.empty() ? "-" : b});
}

// ---------------------------------------------------------------------------
// Modal helper: every dialog gets the X (top right) AND Escape. For EDIT dialogs
// `changes` returns the difference between what is stored and what is typed; when it
// is non-empty, closing first asks "discard changes?" with a stored-vs-typed
// comparison (same idea as the save confirmation) instead of silently losing edits.
// ---------------------------------------------------------------------------
static std::map<std::string, bool> g_discardAsk;
static bool BeginGuardedModal(const char* name, bool& show, ImGuiWindowFlags flags,
                              const std::function<std::vector<FieldDiff>()>& changes = nullptr) {
    bool xOpen = true;
    // never taller than the window: long dialogs scroll instead of hiding their buttons below the screen
    ImGuiViewport* gvp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(gvp->WorkSize.x * 0.95f, gvp->WorkSize.y * 0.92f));
    ImGui::SetNextWindowPos(gvp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    bool began = ImGui::BeginPopupModal(name, &xOpen, flags);
    bool& asking = g_discardAsk[name];
    if (!began) {
        if (show && !xOpen) {                                  // X was pressed
            if (changes && !changes().empty()) { ImGui::OpenPopup(name); asking = true; }
            else { show = false; asking = false; }
        }
        return false;
    }
    if (!asking && ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        if (changes && !changes().empty()) asking = true;
        else { show = false; ImGui::CloseCurrentPopup(); }
    }
    if (asking) {
        const char* dn = "Απόρριψη αλλαγών;###discard";
        if (!ImGui::IsPopupOpen(dn)) ImGui::OpenPopup(dn);
        bool dOpen = true;
        ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal(dn, &dOpen, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("Έχετε κάνει αλλαγές που δεν έχουν αποθηκευτεί. Να απορριφθούν;");
            auto diffs = changes ? changes() : std::vector<FieldDiff>();
            if (ImGui::BeginTable("discarddiff", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Πεδίο"); ImGui::TableSetupColumn("Αποθηκευμένο"); ImGui::TableSetupColumn("Τώρα");
                ImGui::TableHeadersRow();
                for (auto& d : diffs) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(d.field.c_str());
                    ImGui::TableSetColumnIndex(1); ImGui::TextColored(ImVec4(0.8f,0.4f,0.4f,1), "%s", d.oldVal.c_str());
                    ImGui::TableSetColumnIndex(2); ImGui::TextColored(ImVec4(0.4f,0.8f,0.4f,1), "%s", d.newVal.c_str());
                }
                ImGui::EndTable();
            }
            ImGui::Spacing();
            if (ImGui::Button("Απόρριψη αλλαγών", ImVec2(170, 0))) { show = false; asking = false; ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if (ImGui::Button("Συνέχεια επεξεργασίας", ImVec2(190, 0))) { asking = false; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        } else if (!dOpen) {
            asking = false;                                     // X on the question = keep editing
        }
        if (!show) ImGui::CloseCurrentPopup();
    }
    return true;
}


static void RequestConfirm(const std::string& title, std::vector<FieldDiff> diffs, std::function<void()> onConfirm) {
    g_confirm.requested = true;
    g_confirm.title = title;
    g_confirm.diffs = std::move(diffs);
    g_confirm.onConfirm = std::move(onConfirm);
}

static void DrawConfirmModal() {
    if (g_confirm.requested) {
        ImGui::OpenPopup("Επιβεβαίωση");
        g_confirm.requested = false;
        g_confirm.open = true;
    }
    if (!g_confirm.open) return;
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Appearing);
    if (BeginGuardedModal("Επιβεβαίωση", g_confirm.open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", g_confirm.title.c_str());
        ImGui::Separator();
        if (ImGui::BeginTable("diff", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Πεδίο");
            ImGui::TableSetupColumn("Πριν");
            ImGui::TableSetupColumn("Μετά");
            ImGui::TableHeadersRow();
            for (auto& d : g_confirm.diffs) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(d.field.c_str());
                ImGui::TableSetColumnIndex(1); ImGui::TextColored(ImVec4(0.8f,0.4f,0.4f,1), "%s", d.oldVal.c_str());
                ImGui::TableSetColumnIndex(2); ImGui::TextColored(ImVec4(0.4f,0.8f,0.4f,1), "%s", d.newVal.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
        if (ImGui::Button("Επιβεβαίωση", ImVec2(150, 0))) {
            if (g_confirm.onConfirm) g_confirm.onConfirm();
            g_confirm.open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Ακύρωση", ImVec2(150, 0))) {
            g_confirm.open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------
// Small text-buffer helper for ImGui inputs
// ---------------------------------------------------------------------------
struct TextBuf {
    char data[256];
    TextBuf() { data[0] = 0; }
    void set(const std::string& s) { std::snprintf(data, sizeof(data), "%s", s.c_str()); }
    std::string str() const { return std::string(data); }
};

// ---------------------------------------------------------------------------
// Money inputs. Amounts are euros with exactly two decimals; people type "12,50" as often as
// "12.50", so plain InputDouble/atof (which stop at the comma -> 12) are never used for money.
// ---------------------------------------------------------------------------
static const ImVec4 kInvalidMoneyCol(0.95f, 0.40f, 0.35f, 1.0f);

// ---------------------------------------------------------------------------
// One look everywhere: money "1.234,56 €", dates "12/10/2026", and colours with a fixed meaning.
// ---------------------------------------------------------------------------
static std::string Eur(double v) {
    long long c = ToCents(v);
    bool neg = c < 0; if (neg) c = -c;
    std::string whole = std::to_string(c / 100), g;
    for (size_t i = 0; i < whole.size(); i++) { if (i && (whole.size() - i) % 3 == 0) g += '.'; g += whole[i]; }
    char b[48]; std::snprintf(b, sizeof(b), "%s%s,%02lld €", neg ? "-" : "", g.c_str(), c % 100);
    return b;
}
// "2026-10-12" -> "12/10/2026"; "2026-10-12 14:05:00" -> "12/10/2026 14:05"; anything else unchanged
static std::string DateGr(const std::string& iso) {
    if (iso.size() < 10 || iso[4] != '-' || iso[7] != '-') return iso;
    std::string d = iso.substr(8, 2) + "/" + iso.substr(5, 2) + "/" + iso.substr(0, 4);
    if (iso.size() >= 16 && (iso[10] == ' ' || iso[10] == 'T')) d += " " + iso.substr(11, 5);
    return d;
}
static std::string DateGr(int y, int m, int d) { char b[16]; std::snprintf(b, sizeof(b), "%02d/%02d/%04d", d, m, y); return b; }
// colours: green = money in, red = money out, orange = owed, blue-grey = scheduled, blue = clickable, grey = muted
static const ImVec4 kColIn(0.35f, 0.80f, 0.50f, 1.0f);
static const ImVec4 kColOut(0.90f, 0.40f, 0.40f, 1.0f);
static const ImVec4 kColOwed(0.95f, 0.65f, 0.25f, 1.0f);
static const ImVec4 kColSched(0.60f, 0.70f, 0.85f, 1.0f);
static const ImVec4 kColLink(0.55f, 0.78f, 1.0f, 1.0f);
static const ImVec4 kColMuted(0.55f, 0.58f, 0.65f, 1.0f);
static GLFWwindow* g_glfwWindow = nullptr;
static const ImVec4 kColWeOwe(0.78f, 0.55f, 0.95f, 1.0f);      // what WE still owe (to partners)
// A section title that is easy to find with the eye: tinted bar, accent stripe, larger accent text. Returns where the text ends.
static ImVec2 SectionTitle(const char* label) {
    ImGui::Spacing();
    ImVec2 p = ImGui::GetCursorScreenPos(); float w = ImGui::GetContentRegionAvail().x;
    ImVec2 ts = ImGui::CalcTextSize(label);
    float h = ts.y + 10.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(40, 100, 150, 85), 3.0f);
    dl->AddRectFilled(p, ImVec2(p.x + 5.0f, p.y + h), IM_COL32(90, 205, 245, 255), 3.0f);
    dl->AddText(ImVec2(p.x + 14.0f, p.y + 5.0f), IM_COL32(205, 238, 255, 255), label);
    ImGui::Dummy(ImVec2(w, h + 3.0f));
    return ImVec2(p.x + 14.0f + ts.x + 10.0f, p.y + 2.0f);
}
// Red hint after a money text field that does not parse. Returns whether it is valid.
static bool MoneyFieldHint(const TextBuf& b) {
    double v = 0.0;
    bool ok = ParseMoney(b.str(), v);
    if (!ok) { ImGui::SameLine(); ImGui::TextColored(kInvalidMoneyCol, "μη έγκυρο ποσό (π.χ. 12,50)"); }
    return ok;
}

// Drop-in for ImGui::InputDouble on money: while focused it keeps the typed text, otherwise it
// shows the value as 0.00. *v only changes to a valid, cent-exact amount. Returns true on change.
static bool InputMoney(const char* label, double* v) {
    static std::map<ImGuiID, std::string> editing;   // text of the field(s) currently being typed in
    ImGuiID id = ImGui::GetID(label);
    auto it = editing.find(id);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s", it != editing.end() ? it->second.c_str() : FormatMoney(*v).c_str());
    ImGui::InputText(label, buf, sizeof(buf));
    bool changed = false;
    if (ImGui::IsItemActive()) {
        editing[id] = buf;
        double parsed = 0.0;
        if (ParseMoney(buf, parsed)) {
            if (ToCents(parsed) != ToCents(*v)) { *v = parsed; changed = true; }
        } else if (buf[0]) {
            ImGui::SameLine(); ImGui::TextColored(kInvalidMoneyCol, "μη έγκυρο ποσό");
        }
    } else {
        editing.erase(id);
    }
    return changed;
}

// ---------------------------------------------------------------------------
// Global app state
// ---------------------------------------------------------------------------
struct AppState {
    DataConn* conn = nullptr;
    std::string dbPath;

    // cached lists (reloaded on demand). Payments are NOT held in memory: screens ask the database
    // for exactly the rows/totals they show (findPayments / paymentTotals / paymentTotalsBy).
    std::vector<User> users;
    std::vector<Service> services;
    std::vector<DataConn::JointServiceSummary> jointServices;
    std::vector<Note> notes;
    std::unordered_map<int, size_t> userIdx, serviceIdx;   // id -> position, for O(1) name lookups
    long long reloadGen = 0;                               // bumped by reloadAll(): forces cached views to recompute

    void reloadUsers() {
        users = conn->searchUsers("", 0, 1 << 30);          // all clients (was capped at 5000)
        userIdx.clear(); for (size_t i = 0; i < users.size(); i++) userIdx[users[i].id] = i;
    }
    void reloadServices() {
        services = conn->searchServices("", 0, 1 << 30);
        serviceIdx.clear(); for (size_t i = 0; i < services.size(); i++) serviceIdx[services[i].id] = i;
    }
    void reloadJoint() { jointServices = conn->getJointServices(); }
    void reloadNotes() { notes = conn->getAllNotes(); }
    void reloadAll() { reloadUsers(); reloadServices(); reloadJoint(); reloadNotes(); reloadGen++; }

    const User* user(int id) const { auto it = userIdx.find(id); return it == userIdx.end() ? nullptr : &users[it->second]; }
    mutable std::map<int, std::string> goneNames;       // deleted clients: still named in the history
    std::string userName(int id) const {
        const User* u = user(id);
        if (u) return u->full_name;
        if (id > 0) {
            auto it = goneNames.find(id);
            if (it == goneNames.end()) { User g = conn->getUserById(id); it = goneNames.emplace(id, g.id > 0 ? g.full_name + " (διαγραμμένος)" : "#" + std::to_string(id)).first; }
            return it->second;
        }
        return "#" + std::to_string(id);
    }
    std::string serviceLabel(int id) const {
        auto it = serviceIdx.find(id);
        return it == serviceIdx.end() ? "#" + std::to_string(id) : services[it->second].label;
    }
};
static AppState g_app;

static std::string SeriesLabel(const std::string& s) { return s == "A" ? "Α" : s == "P" ? "Π" : s; }

// A charge line's label: the service, marked when the line is one resident's share of a joint charge.
static std::string ChargeLabel(const Payment& p) {
    std::string s = g_app.serviceLabel(p.service_id);
    if (p.joint_op_id > 0) s += "  [Κοινόχρηστο]";
    if (p.used_offer_id > 0) s += "  [Προσφορά: " + p.used_offer_name + "]";
    return s;
}

// A value computed from the database that is recomputed only when the data changed (any write on the
// connection), the caller's key changed (filters, selection), or the user pressed a refresh button.
// Wrap every screen's heavy computation in one of these: an unchanged screen then costs nothing per frame.
template <class T> struct Cached {
    long long ver = -1; std::string key; T val{};
    template <class F> const T& get(const std::string& k, F&& compute) {
        long long v = g_app.conn->dataVersion() * 1000003 + g_app.reloadGen;
        if (v != ver || k != key) { val = compute(); ver = v; key = k; }
        return val;
    }
};


// =====================================================================================================
// SEARCH PICKERS -- a button showing the current choice; clicking opens a popup with a search box and pages of
// results queried from the database (never a dropdown of every client/service). Same look everywhere.
// =====================================================================================================
struct PickerState { char q[128] = ""; int offset = 0; };
static const int kPickPage = 10;

// The popup frame shared by the pickers. body(query, offset) draws the rows and returns the total count.
template <class Body>
static bool PickerPopup(const char* popupId, PickerState& ps, const char* hint, Body&& body) {
    bool picked = false;
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopup(popupId)) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##q", hint, ps.q, sizeof(ps.q))) ps.offset = 0;
        ImGui::Separator();
        int total = body(std::string(ps.q), ps.offset, picked);
        ImGui::Separator();
        int pages = std::max(1, (total + kPickPage - 1) / kPickPage), page = ps.offset / kPickPage + 1;
        ImGui::BeginDisabled(ps.offset == 0);
        if (ImGui::SmallButton("◄ Προηγ.")) ps.offset = std::max(0, ps.offset - kPickPage);
        ImGui::EndDisabled();
        ImGui::SameLine(); ImGui::TextDisabled("Σελίδα %d / %d  ·  %d αποτελέσματα", page, pages, total);
        ImGui::SameLine();
        ImGui::BeginDisabled(ps.offset + kPickPage >= total);
        if (ImGui::SmallButton("Επόμ. ►")) ps.offset += kPickPage;
        ImGui::EndDisabled();
        if (picked) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    return picked;
}

// Business partner (Συνεργάτης) a cost was paid to. A short list, so a plain combo. -1 = nobody in particular.
static bool PickPartner(const char* id, int& partnerId, float width = 300.0f) {
    static std::map<std::string, PickerState> states;
    PickerState& ps = states[id];
    static Cached<Partner> curC;
    ImGui::PushID(id);
    std::string cur = "#-1 (δεν καταγράφεται συνεργάτης)";
    if (partnerId > 0) {
        const Partner& cp = curC.get(std::to_string(partnerId), [&] { return g_app.conn->getPartner(partnerId); });
        if (cp.id == partnerId) cur = cp.name + (cp.trade.empty() ? "" : "  ·  " + cp.trade);
    }
    if (ImGui::Button((cur + "  ▼").c_str(), ImVec2(width, 0))) { ps = PickerState{}; ImGui::OpenPopup("pickpartner"); }
    bool picked = PickerPopup("pickpartner", ps, "Αναζήτηση συνεργάτη (όνομα, ειδικότητα, τηλέφωνο)…", [&](const std::string& q, int off, bool& done) {
        static Cached<std::pair<std::vector<Partner>, int>> cache;
        const auto& res = cache.get(q + "#" + std::to_string(off), [&] {
            return std::make_pair(g_app.conn->pagePartners(q, true, {}, off, kPickPage), g_app.conn->countPartners(q, true)); });
        if (off == 0 && q.empty() && ImGui::Selectable("#-1 (δεν καταγράφεται συνεργάτης)", partnerId <= 0)) { partnerId = -1; done = true; }
        for (auto& pr : res.first) {
            ImGui::PushID(pr.id);
            std::string lb = pr.name + (pr.trade.empty() ? "" : "  ·  " + pr.trade);
            if (ImGui::Selectable(lb.c_str(), pr.id == partnerId)) { partnerId = pr.id; done = true; }
            ImGui::PopID();
        }
        if (res.first.empty()) ImGui::TextDisabled("Κανένας συνεργάτης.");
        return res.second;
    });
    ImGui::PopID();
    return picked;
}
// Client: searches name, phone, code and address. Returns true when a client was chosen.
static bool PickClient(const char* id, int& userId, float width = 320.0f) {
    static std::map<std::string, PickerState> states;
    PickerState& ps = states[id];
    ImGui::PushID(id);
    std::string cur = userId > 0 ? g_app.userName(userId) : "Επιλέξτε πελάτη…";
    if (ImGui::Button((cur + "  ▼").c_str(), ImVec2(width, 0))) { ps = PickerState{}; ImGui::OpenPopup("pickcli"); }
    bool picked = PickerPopup("pickcli", ps, "Αναζήτηση: όνομα, τηλέφωνο, κωδικός, διεύθυνση…", [&](const std::string& q, int off, bool& done) {
        static Cached<std::pair<std::vector<User>, int>> cache;
        const auto& res = cache.get(q + "#" + std::to_string(off), [&] {
            return std::make_pair(g_app.conn->pageUsers(q, 0, {}, off, kPickPage), g_app.conn->countUsers(q, 0, {})); });
        if (ImGui::BeginTable("cli", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("Πελάτης", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("Τηλέφωνο", ImGuiTableColumnFlags_WidthFixed, 110);
            ImGui::TableSetupColumn("Διεύθυνση", ImGuiTableColumnFlags_WidthStretch, 1.8f);
            for (auto& u : res.first) {
                ImGui::PushID(u.id); ImGui::TableNextRow(); ImGui::TableNextColumn();
                if (ImGui::Selectable(u.full_name.c_str(), u.id == userId, ImGuiSelectableFlags_SpanAllColumns)) { userId = u.id; done = true; }
                ImGui::TableNextColumn(); ImGui::TextDisabled("%s", u.phone.c_str());
                ImGui::TableNextColumn(); ImGui::TextDisabled("%s", u.address.c_str());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (res.first.empty()) ImGui::TextDisabled("Κανένας πελάτης.");
        return res.second;
    });
    ImGui::PopID();
    return picked;
}

// Service (catalog services only; joint ones are charged to buildings from Κοινόχρηστα).
static bool PickService(const char* id, int& serviceId, float width = 320.0f) {
    static std::map<std::string, PickerState> states;
    PickerState& ps = states[id];
    ImGui::PushID(id);
    std::string cur = serviceId > 0 ? g_app.serviceLabel(serviceId) : "Επιλέξτε υπηρεσία…";
    if (ImGui::Button((cur + "  ▼").c_str(), ImVec2(width, 0))) { ps = PickerState{}; ImGui::OpenPopup("picksvc"); }
    bool picked = PickerPopup("picksvc", ps, "Αναζήτηση υπηρεσίας…", [&](const std::string& q, int off, bool& done) {
        static Cached<std::pair<std::vector<Service>, int>> cache;
        const auto& res = cache.get(q + "#" + std::to_string(off), [&] {
            return std::make_pair(g_app.conn->pageServices(q, off, kPickPage), g_app.conn->countServices(q)); });
        if (ImGui::BeginTable("svc", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("Υπηρεσία", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Τιμή", ImGuiTableColumnFlags_WidthFixed, 80);
            for (auto& s : res.first) {
                ImGui::PushID(s.id); ImGui::TableNextRow(); ImGui::TableNextColumn();
                if (ImGui::Selectable(s.label.c_str(), s.id == serviceId, ImGuiSelectableFlags_SpanAllColumns)) { serviceId = s.id; done = true; }
                if (!s.extra_notes.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", s.extra_notes.c_str());
                ImGui::TableNextColumn(); ImGui::TextDisabled("%s", Eur(s.suggested_price).c_str());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (res.first.empty()) ImGui::TextDisabled("Καμία υπηρεσία.");
        return res.second;
    });
    ImGui::PopID();
    return picked;
}

// Package: name or any of its services; shows how many services and the total price. allLabel: an extra first row
// ("Όλα") that sets -1, for filters.
static bool PickPackage(const char* id, int& pkgId, float width = 320.0f, const char* allLabel = nullptr) {
    static std::map<std::string, PickerState> states;
    PickerState& ps = states[id];
    ImGui::PushID(id);
    static std::map<int, std::string> names;
    if (pkgId > 0 && !names.count(pkgId)) names[pkgId] = g_app.conn->getPackageLabel(pkgId);
    std::string cur = pkgId > 0 ? names[pkgId] : (allLabel ? allLabel : "Επιλέξτε πακέτο…");
    if (ImGui::Button((cur + "  ▼").c_str(), ImVec2(width, 0))) { ps = PickerState{}; ImGui::OpenPopup("pickpkg"); }
    bool picked = PickerPopup("pickpkg", ps, "Αναζήτηση πακέτου ή υπηρεσίας του…", [&](const std::string& q, int off, bool& done) {
        if (allLabel && off == 0 && ImGui::Selectable(allLabel, pkgId <= 0)) { pkgId = -1; done = true; }
        static Cached<std::pair<std::vector<DataConn::PackageRow>, int>> cache;
        const auto& res = cache.get(q + "#" + std::to_string(off), [&] {
            return std::make_pair(g_app.conn->pagePackages(q, {{0, true}}, off, kPickPage), g_app.conn->countPackages(q)); });
        if (ImGui::BeginTable("pkg", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("Πακέτο", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Υπηρεσίες", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Σύνολο", ImGuiTableColumnFlags_WidthFixed, 90);
            for (auto& r : res.first) {
                ImGui::PushID(r.pkg.id); ImGui::TableNextRow(); ImGui::TableNextColumn();
                if (ImGui::Selectable(r.pkg.label.c_str(), r.pkg.id == pkgId, ImGuiSelectableFlags_SpanAllColumns)) {
                    pkgId = r.pkg.id; names[pkgId] = r.pkg.label; done = true; }
                if (ImGui::IsItemHovered() && !r.parts.empty()) ImGui::SetTooltip("%s", r.parts.c_str());
                ImGui::TableNextColumn(); ImGui::TextDisabled("%d", r.components);
                ImGui::TableNextColumn(); ImGui::TextDisabled("%s", Eur(r.total).c_str());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (res.first.empty()) ImGui::TextDisabled("Κανένα πακέτο.");
        return res.second;
    });
    ImGui::PopID();
    return picked;
}

// Building: address, area, ΤΚ, ΑΦΜ.
static bool PickBuilding(const char* id, int& buildingId, float width = 320.0f, const char* allLabel = nullptr) {
    static std::map<std::string, PickerState> states;
    PickerState& ps = states[id];
    ImGui::PushID(id);
    static std::map<int, std::string> names;
    if (buildingId > 0 && !names.count(buildingId)) names[buildingId] = g_app.conn->getBuildingById(buildingId).address;
    std::string cur = buildingId > 0 ? names[buildingId] : (allLabel ? allLabel : "Επιλέξτε πολυκατοικία…");
    if (ImGui::Button((cur + "  ▼").c_str(), ImVec2(width, 0))) { ps = PickerState{}; ImGui::OpenPopup("pickbld"); }
    bool picked = PickerPopup("pickbld", ps, "Αναζήτηση: διεύθυνση, περιοχή, Τ.Κ., ΑΦΜ…", [&](const std::string& q, int off, bool& done) {
        if (allLabel && off == 0 && ImGui::Selectable(allLabel, buildingId <= 0)) { buildingId = -1; done = true; }
        static Cached<std::pair<std::vector<Building>, int>> cache;
        const auto& res = cache.get(q + "#" + std::to_string(off), [&] {
            return std::make_pair(g_app.conn->pageBuildings(q, off, kPickPage), g_app.conn->countBuildings(q)); });
        if (ImGui::BeginTable("bld", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("Διεύθυνση", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("Περιοχή", ImGuiTableColumnFlags_WidthStretch, 1.2f);
            ImGui::TableSetupColumn("Τ.Κ.", ImGuiTableColumnFlags_WidthFixed, 70);
            for (auto& b : res.first) {
                ImGui::PushID(b.id); ImGui::TableNextRow(); ImGui::TableNextColumn();
                if (ImGui::Selectable(b.address.c_str(), b.id == buildingId, ImGuiSelectableFlags_SpanAllColumns)) {
                    buildingId = b.id; names[b.id] = b.address; done = true; }
                ImGui::TableNextColumn(); ImGui::TextDisabled("%s", b.area.c_str());
                ImGui::TableNextColumn(); ImGui::TextDisabled("%s", b.postal_code.c_str());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (res.first.empty()) ImGui::TextDisabled("Καμία πολυκατοικία.");
        return res.second;
    });
    ImGui::PopID();
    return picked;
}

// =====================================================================================================
// ATTACHMENTS -- photos / videos / PDFs on receipts, payments, clients, buildings.
// Files arrive by drag & drop onto a «Συνημμένα» panel or through «Προσθήκη αρχείου…». They are copied into the
// database folder (content-addressed); the original can be moved or deleted afterwards.
// =====================================================================================================
static std::vector<std::string> g_droppedFiles;          // filled by the GLFW drop callback
static bool g_dropConsumed = false;
static std::string g_dropHint;
static double g_dropHintUntil = 0;
static int g_dropFrame = -1;                              // frame the files arrived in

static void OnFilesDropped(GLFWwindow*, int count, const char** paths) {
    for (int i = 0; i < count; i++) g_droppedFiles.push_back(paths[i]);
    g_dropConsumed = false;
    g_dropFrame = -1;
}

#ifdef _WIN32
static std::vector<std::string> PickFilesDialogWin32() {
    std::vector<std::string> out;
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool weInitialized = SUCCEEDED(hrInit);
    if (FAILED(hrInit) && hrInit != RPC_E_CHANGED_MODE) return out;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0; dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
        dlg->SetTitle(L"Επιλογή αρχείων για επισύναψη");
        if (SUCCEEDED(dlg->Show(nullptr))) {
            IShellItemArray* items = nullptr;
            if (SUCCEEDED(dlg->GetResults(&items))) {
                DWORD n = 0; items->GetCount(&n);
                for (DWORD i = 0; i < n; i++) {
                    IShellItem* it = nullptr;
                    if (FAILED(items->GetItemAt(i, &it))) continue;
                    PWSTR w = nullptr;
                    if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &w))) {
                        int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
                        std::string b(len, '\0'); WideCharToMultiByte(CP_UTF8, 0, w, -1, b.data(), len, nullptr, nullptr);
                        out.push_back(b.c_str());
                        CoTaskMemFree(w);
                    }
                    it->Release();
                }
                items->Release();
            }
        }
        dlg->Release();
    }
    if (weInitialized) CoUninitialize();
    return out;
}
#endif

// Native file picker. Linux: zenity if installed (ok=false when it isn't, so the caller asks for a path instead).
static std::vector<std::string> PickFiles(bool& ok) {
    ok = true;
#ifdef _WIN32
    return PickFilesDialogWin32();
#else
    std::vector<std::string> out;
    if (std::system("command -v zenity >/dev/null 2>&1") != 0) { ok = false; return out; }
    FILE* f = popen("zenity --file-selection --multiple --separator='\n' --title='Επιλογή αρχείων' 2>/dev/null", "r");
    if (!f) { ok = false; return out; }
    char line[4096];
    while (fgets(line, sizeof(line), f)) { std::string l = line; while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back(); if (!l.empty()) out.push_back(l); }
    pclose(f);
    return out;
#endif
}

static Attachment g_viewAttachment;
static bool g_showAttachmentViewer = false;
static ImageView g_imageView;
static VideoView g_videoView;
static std::string g_viewerError;

static void OpenAttachmentViewer(const Attachment& a) {
    g_viewAttachment = a; g_showAttachmentViewer = true; g_viewerError.clear();
    g_imageView.Close(); g_videoView.Close();
    std::string path = g_app.conn->attachmentFullPath(a);
    if (a.kind == "image") g_imageView.Open(path, g_viewerError);
    else if (a.kind == "video" && VideoView::Available()) g_videoView.Open(path, g_viewerError);
}

static std::string HumanSize(long long b) {
    char buf[32];
    if (b < 1024) std::snprintf(buf, sizeof(buf), "%lld B", b);
    else if (b < 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.0f KB", b / 1024.0);
    else std::snprintf(buf, sizeof(buf), "%.1f MB", b / (1024.0 * 1024.0));
    return buf;
}

// «Συνημμένα» section for one record. Takes dropped files while the mouse is over it.
static void DrawAttachmentsPanel(const char* entity, int entityId) {
    if (entityId <= 0) return;
    ImGui::PushID(entity); ImGui::PushID(entityId);
    std::vector<Attachment> list = g_app.conn->getAttachments(entity, entityId);   // one indexed query
    SectionTitle(("Συνημμένα (" + std::to_string(list.size()) + ")").c_str());
    ImGui::BeginChild("attpanel", ImVec2(0, 0), ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY);
    bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem |
                                          ImGuiHoveredFlags_AllowWhenBlockedByPopup);
    auto addFiles = [&](const std::vector<std::string>& files) {
        int ok = 0;
        for (auto& f : files) if (g_app.conn->addAttachment(entity, entityId, f, "") > 0) ok++;
        g_dropHint = std::to_string(ok) + " από " + std::to_string(files.size()) + " αρχεία επισυνάφθηκαν";
        g_dropHintUntil = ImGui::GetTime() + 4;
    };
    if (hovered && !g_droppedFiles.empty() && !g_dropConsumed) {
        std::vector<std::string> files; files.swap(g_droppedFiles);
        g_dropConsumed = true;
        addFiles(files);
    }
    static TextBuf manualPath; static bool askPath = false;
    if (ImGui::SmallButton("+ Προσθήκη αρχείου…")) {
        bool ok = true; auto files = PickFiles(ok);
        if (!ok) askPath = true; else if (!files.empty()) addFiles(files);
    }
    ImGui::SameLine(); ImGui::TextDisabled("ή σύρετε φωτογραφίες / βίντεο / PDF εδώ");
    if (askPath) {
        ImGui::SetNextItemWidth(360);
        ImGui::InputTextWithHint("##path", "διαδρομή αρχείου", manualPath.data, sizeof(manualPath.data));
        ImGui::SameLine();
        if (ImGui::SmallButton("Επισύναψη") && !manualPath.str().empty()) { addFiles({manualPath.str()}); manualPath.set(""); askPath = false; }
    }
    for (auto& a : list) {
        ImGui::PushID(a.id);
        const char* tag = a.kind == "image" ? "[εικόνα]" : a.kind == "video" ? "[βίντεο]" : a.kind == "pdf" ? "[PDF]" : "[αρχείο]";
        ImGui::TextDisabled("%s", tag); ImGui::SameLine();
        if (ImGui::Selectable(a.file_name.c_str(), false, 0, ImVec2(260, 0))) OpenAttachmentViewer(a);
        ImGui::SameLine(); ImGui::TextDisabled("%s · %s", HumanSize(a.size_bytes).c_str(), a.created_at.c_str());
        if (std::string(entity) != "receipt") {                // receipt evidence can't be removed
            ImGui::SameLine();
            static TextBuf reason;
            if (ImGui::SmallButton("Αφαίρεση…")) { reason.set(""); ImGui::OpenPopup("rmatt"); }
            if (ImGui::BeginPopup("rmatt")) {
                ImGui::Text("Αφαίρεση «%s»", a.file_name.c_str());
                ImGui::TextDisabled("Μένει στο ιστορικό. Χρειάζεται αιτιολογία.");
                ImGui::SetNextItemWidth(300); ImGui::InputText("##why", reason.data, sizeof(reason.data));
                ImGui::BeginDisabled(reason.str().size() < 3);
                if (ImGui::Button("Αφαίρεση")) { g_app.conn->removeAttachment(a.id, reason.str()); ImGui::CloseCurrentPopup(); }
                ImGui::EndDisabled();
                ImGui::EndPopup();
            }
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::PopID(); ImGui::PopID();
}

static void DrawAttachmentViewer() {
    // files dropped outside any «Συνημμένα» panel: after one full frame unclaimed, explain where to drop them
    if (!g_droppedFiles.empty()) {
        if (g_dropFrame < 0) g_dropFrame = ImGui::GetFrameCount();
        else if (ImGui::GetFrameCount() > g_dropFrame + 1) {
            g_droppedFiles.clear();
            g_dropHint = "Σύρετε τα αρχεία πάνω σε μια ενότητα «Συνημμένα» (απόδειξη, πληρωμή ή πελάτη).";
            g_dropHintUntil = ImGui::GetTime() + 5;
        }
    }
    if (!g_dropHint.empty() && ImGui::GetTime() < g_dropHintUntil) {
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 50), ImGuiCond_Always, ImVec2(0.5f, 0));
        ImGui::Begin("##drophint", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextUnformatted(g_dropHint.c_str());
        ImGui::End();
    }
    if (!g_showAttachmentViewer) { if (g_imageView.IsOpen()) g_imageView.Close(); if (g_videoView.IsOpen()) g_videoView.Close(); return; }
    const Attachment& a = g_viewAttachment;
    ImGui::SetNextWindowSize(ImVec2(900, 640), ImGuiCond_FirstUseEver);
    std::string title = a.file_name + "###attviewer";
    if (ImGui::Begin(title.c_str(), &g_showAttachmentViewer, ImGuiWindowFlags_NoCollapse)) {
        std::string path = g_app.conn->attachmentFullPath(a);
        ImGui::TextDisabled("%s · προστέθηκε %s", HumanSize(a.size_bytes).c_str(), a.created_at.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Άνοιγμα με εφαρμογή συστήματος")) OpenWithSystem(path);
        ImGui::SameLine();
        if (ImGui::SmallButton("Εμφάνιση στον φάκελο")) RevealInFolder(path);
        if (!g_viewerError.empty()) ImGui::TextColored(kInvalidMoneyCol, "%s", g_viewerError.c_str());
        if (a.kind == "image" && g_imageView.IsOpen()) g_imageView.Draw();
        else if (a.kind == "video" && g_videoView.IsOpen()) g_videoView.Draw(ImGui::GetIO().DeltaTime);
        else if (a.kind == "video") ImGui::TextWrapped("Η αναπαραγωγή μέσα στην εφαρμογή δεν είναι διαθέσιμη σε αυτή την έκδοση. Χρησιμοποιήστε «Άνοιγμα με εφαρμογή συστήματος».");
        else ImGui::TextWrapped("Αυτός ο τύπος αρχείου ανοίγει με την εφαρμογή του συστήματος.");
    }
    ImGui::End();
}

// Forward declaration -- StatTile is defined further down but DrawBuildingsScreen
// (defined above it) needs it.
static void StatTile(const char* label, const std::string& value, ImVec4 accent, const char* sub, ImVec2 size);

// ---------------------------------------------------------------------------
// Card helpers
// ---------------------------------------------------------------------------

static bool BeginCard(const char* id, ImVec2 size, bool noScroll = false) {
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGuiChildFlags  childFlags = ImGuiChildFlags_Border;
    // NoScrollbar hides the bar; NoScrollWithMouse is what actually stops
    // ImGui from scrolling the child on wheel events. Both are needed when
    // the caller wants a truly frozen card (day banners, timeline rows).
    ImGuiWindowFlags winFlags = noScroll
        ? (ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)
        : 0;
    bool open = ImGui::BeginChild(id, size, childFlags, winFlags);
    return open;
}

static void EndCard() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
}

static void CardWrapNext(float cardWidth) {
    float avail = ImGui::GetContentRegionAvail().x;
    ImGuiStyle& style = ImGui::GetStyle();
    float lastX2 = ImGui::GetItemRectMax().x;
    float nextX2 = lastX2 + style.ItemSpacing.x + cardWidth;
    if (nextX2 < ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x)
        ImGui::SameLine();
}

// ---------------------------------------------------------------------------
// Pager (offset/limit list-view pagination). One instance per paged tab.
// "total" is filled by a companion countX call; nothing else in the struct
// knows about the underlying query.
// ---------------------------------------------------------------------------
struct Pager {
    int offset   = 0;
    int pageSize = 50;
    int total    = 0;

    int  CurrentPage() const { return pageSize > 0 ? (offset / pageSize) + 1 : 1; }
    int  TotalPages()  const { return pageSize > 0 ? ((total + pageSize - 1) / pageSize) : 1; }
    bool HasPrev()     const { return offset > 0; }
    bool HasNext()     const { return offset + pageSize < total; }
    int  ShowingFrom() const { return total == 0 ? 0 : offset + 1; }
    int  ShowingTo()   const { return std::min(offset + pageSize, total); }
    void Prev()        { if (HasPrev()) { offset -= pageSize; if (offset < 0) offset = 0; } }
    void Next()        { if (HasNext()) offset += pageSize; }
    void Reset()       { offset = 0; }
};

// Renders a small pager row:  [<]  Σελίδα X / Y  (a–b από N)  [>]   |  N ανά σελίδα
// Returns true if the pager state changed this frame, so the caller can
// requery. Offsets are the only thing that changes.
static bool DrawPager(Pager& p, const char* id) {
    bool changed = false;
    ImGui::PushID(id);
    ImGui::Separator();

    if (!p.HasPrev()) ImGui::BeginDisabled();
    if (ImGui::ArrowButton("##prev", ImGuiDir_Left)) { p.Prev(); changed = true; }
    if (!p.HasPrev()) ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::Text("Σελίδα %d / %d   (%d-%d από %d)",
                p.CurrentPage(), p.TotalPages(),
                p.ShowingFrom(), p.ShowingTo(), p.total);

    ImGui::SameLine();
    if (!p.HasNext()) ImGui::BeginDisabled();
    if (ImGui::ArrowButton("##next", ImGuiDir_Right)) { p.Next(); changed = true; }
    if (!p.HasNext()) ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::TextUnformatted("Ανά σελίδα:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    static const char* kSizes[] = { "10", "20", "50", "100", "200" };
    static const int   kSizeVals[] = { 10, 20, 50, 100, 200 };
    int curIdx = 1;
    for (int i = 0; i < 5; i++) if (kSizeVals[i] == p.pageSize) curIdx = i;
    if (ImGui::Combo("##pagesize", &curIdx, kSizes, 5)) {
        p.pageSize = kSizeVals[curIdx];
        p.Reset();
        changed = true;
    }

    ImGui::PopID();
    return changed;
}

// Global pager instances (one per paged list tab). Reset when a filter
// changes so a stale offset never leaves the user paginated into a filter
// window that no longer exists.
static Pager g_usersPager;
static Pager g_servicesPager;
static Pager g_paymentsPager;

// ---------------------------------------------------------------------------
// Excel-style list helpers.
//  * Header sort: click a column header -> ascending, again -> descending; the arrow
//    is drawn by ImGui. The choice goes to the SQL ORDER BY (the lists are paged, so
//    sorting only the visible page would be wrong).
//  * Row double-click: the first cell is a full-width selectable; a double click
//    anywhere on the row (not on one of its buttons) opens the item.
// ---------------------------------------------------------------------------
// Several keys can be active at once (first = most important), like a spreadsheet's multi-sort.
struct ColSort { SortSpec keys; };
static ColSort g_usersSort    = { { { 3, true } } };    // UCOL_NAME
static ColSort g_servicesSort = { { { 1, true } } };    // label
static ColSort g_paymentsSort = { { { 1, false } } };   // date, newest first

// Click on a column's ▲/▼: adds the column to the sort, switches its direction, or removes it
// when the same arrow is clicked again. Other columns' sorts are kept.
// A plain click sorts by THAT column alone (so the view always changes, whatever was sorted before); the same arrow
// again clears it. With Shift held the column is added as a further key, like a spreadsheet's "then by".
static void SortToggle(ColSort& st, int col, bool asc, bool add = false) {
    if (!add) {
        bool same = st.keys.size() == 1 && st.keys[0].first == col && st.keys[0].second == asc;
        st.keys.clear();
        if (!same) st.keys.push_back({ col, asc });
        return;
    }
    for (size_t i = 0; i < st.keys.size(); i++) {
        if (st.keys[i].first != col) continue;
        if (st.keys[i].second == asc) st.keys.erase(st.keys.begin() + i);
        else st.keys[i].second = asc;
        return;
    }
    st.keys.push_back({ col, asc });
}

static std::string TruncToWidth(const std::string& s, float maxW);
static ImGuiID RowSelectable(const char* label) {
    ImGui::Selectable(label, false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);
    return ImGui::GetItemID();
}
// Evaluate AFTER the row's buttons were submitted, so a double click on a button
// does not also open the row.
static bool RowDoubleClicked(ImGuiID rowId) {
    return rowId != 0 && ImGui::IsMouseDoubleClicked(0) && ImGui::GetCurrentContext()->HoveredId == rowId;
}

// ---------------------------------------------------------------------------
// Spreadsheet-style list headers (PyCharm CSV viewer style). Every column header is
//        <  NAME  ▲ ▼  >
// "<" / ">" swap the column with its left / right neighbour (so there is no separate
// "rearrange columns" mode), "▲" / "▼" sort ascending / descending. The sort goes to the
// SQL ORDER BY (the lists are paged), the order is kept in memory per list.
// The last column (the actions) is not sortable and stays put.
// ---------------------------------------------------------------------------
struct ListColDef { const char* name; ImGuiTableColumnFlags flags; float w; bool arrows; };

// Compact headers (no move buttons): for tables that must fit the page width without sideways scrolling.
static bool g_listCompact = false;
// Width a list column needs so its name and its four header buttons are never squashed.
static float ListColNeed(const ListColDef& d) {
    float name = ImGui::CalcTextSize(d.name).x + 4.0f;
    float arrows = d.arrows ? (g_listCompact ? 2.0f : 4.0f) * (ImGui::CalcTextSize("▲").x + 8.0f) + 6.0f : 0.0f;
    return std::max(name, arrows) + ImGui::GetStyle().CellPadding.x * 2.0f + 6.0f;
}
// Every column gets a real width (stretch weights become ~80 px per unit) of at least what its header needs;
// the tables scroll sideways when the window is narrower than the columns.
static void ListSetupColumns(const std::vector<int>& order, const ListColDef* defs) {
    for (int l : order) {
        const ListColDef& d = defs[l];
        bool stretch = (d.flags & ImGuiTableColumnFlags_WidthStretch) != 0;
        float w = std::max(stretch ? d.w * 80.0f : d.w, ListColNeed(d));
        ImGuiTableColumnFlags f = (d.flags & ~ImGuiTableColumnFlags_WidthStretch) | ImGuiTableColumnFlags_WidthFixed;
        ImGui::TableSetupColumn(d.name, f | ImGuiTableColumnFlags_NoReorder | ImGuiTableColumnFlags_NoSort, w);
    }
}
static int ListPos(const std::vector<int>& order, int logical) {
    for (int d = 0; d < (int)order.size(); d++) if (order[d] == logical) return d;
    return 0;
}
static void ListHeaderRow(std::vector<int>& order, const ListColDef* defs, ColSort& sort, Pager& pager) {
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    const int n = (int)order.size();
    const ImVec4 accent(0.35f, 0.80f, 1.0f, 1.0f);
    int swapA = -1, swapB = -1;
    for (int d = 0; d < n; d++) {
        if (!ImGui::TableSetColumnIndex(d)) continue;           // hidden column
        const int l = order[d];
        const ListColDef& def = defs[l];
        ImGui::PushID(l);
        if (!def.arrows) { ImGui::TextUnformatted(def.name); ImGui::PopID(); continue; }

        // two lines: the full column name, then the buttons (move left, sort up, sort down, move right)
        int ki = -1;
        for (size_t q = 0; q < sort.keys.size(); q++) if (sort.keys[q].first == l) ki = (int)q;
        const bool sorted = ki >= 0;
        const bool sAsc = sorted && sort.keys[ki].second;
        const bool sDesc = sorted && !sort.keys[ki].second;
        std::string title = std::string(def.name) + ((sorted && sort.keys.size() > 1) ? " " + std::to_string(ki + 1) : std::string());
        ImGui::PushStyleColor(ImGuiCol_Text, sorted ? accent : ImGui::GetStyleColorVec4(ImGuiCol_Text));
        ImGui::TextUnformatted(TruncToWidth(title, ImGui::GetContentRegionAvail().x).c_str());
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n▲ / ▼ ταξινόμηση κατά αυτή τη στήλη (κλικ ξανά: αφαίρεση)\nShift+κλικ: προσθήκη ως δεύτερο κριτήριο\n◄ / ► μετακίνηση στήλης", def.name);

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 0));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1, 1, 1, 0.06f));
        const bool canL = d > 0 && defs[order[d - 1]].arrows;
        const bool canR = d + 1 < n && defs[order[d + 1]].arrows;
        auto btn = [&](const char* label, bool enabled, bool active) {
            if (active) ImGui::PushStyleColor(ImGuiCol_Text, accent);
            if (!enabled) ImGui::BeginDisabled();
            bool c = ImGui::SmallButton(label);
            if (!enabled) ImGui::EndDisabled();
            if (active) ImGui::PopStyleColor();
            return c;
        };
        if (!g_listCompact) { if (btn("◄##l", canL, false)) { swapA = d; swapB = d - 1; } ImGui::SameLine(); }
        if (btn("▲##a", true, sAsc))  { SortToggle(sort, l, true, ImGui::GetIO().KeyShift);  pager.Reset(); }
        ImGui::SameLine();
        if (btn("▼##d", true, sDesc)) { SortToggle(sort, l, false, ImGui::GetIO().KeyShift); pager.Reset(); }
        if (!g_listCompact) { ImGui::SameLine(); if (btn("►##r", canR, false)) { swapA = d; swapB = d + 1; } }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        ImGui::PopID();
    }
    if (swapA >= 0) std::swap(order[swapA], order[swapB]);
}


// In-memory version of the list tables (same header: name, ◄ ▲ ▼ ►; same pager), for lists that are already
// loaded. cmp(col, a, b) returns <0 / 0 / >0 for one column; several sort keys apply in order.
template <class Row, class Cmp>
static void GridSort(std::vector<const Row*>& rows, const ColSort& s, Cmp cmp) {
    if (s.keys.empty()) return;
    std::stable_sort(rows.begin(), rows.end(), [&](const Row* a, const Row* b) {
        for (auto& k : s.keys) { int c = cmp(k.first, *a, *b); if (c != 0) return k.second ? c < 0 : c > 0; }
        return false;
    });
}
template <class T> static int Cmp3(const T& a, const T& b) { return a < b ? -1 : (b < a ? 1 : 0); }
static int CmpText(const std::string& a, const std::string& b) { return Cmp3(FoldGreek(a), FoldGreek(b)); }
struct GridState { std::vector<int> order; ColSort sort; Pager pager; };

// A list's sort keys expressed in the column numbers its SQL query understands (columns without a mapping are dropped),
// and a short string of them for cache keys. Every key the person clicked is kept: the query sorts by all of them.
static SortSpec MapSort(const SortSpec& s, std::initializer_list<std::pair<int, int>> map) {
    SortSpec out;
    for (auto& k : s) for (auto& m : map) if (m.first == k.first) out.push_back({ m.second, k.second });
    return out;
}
// In-memory tables (everything already loaded): ImGui's own header sorting. Click a header = that column,
// Ctrl+click = add it as a further key. cmp(col, a, b) returns <0 / 0 / >0 for one column.
template <class V, class Cmp>
static void SortByTableSpecs(V& v, Cmp cmp) {
    ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs();
    if (!sp || sp->SpecsCount == 0) return;
    std::stable_sort(v.begin(), v.end(), [&](const auto& a, const auto& b) {
        for (int i = 0; i < sp->SpecsCount; i++) {
            const ImGuiTableColumnSortSpecs& s = sp->Specs[i];
            int c = cmp((int)s.ColumnIndex, a, b);
            if (c != 0) return s.SortDirection == ImGuiSortDirection_Ascending ? c < 0 : c > 0;
        }
        return false;
    });
}
static const ImGuiTableFlags kSortableTable = ImGuiTableFlags_Sortable | ImGuiTableFlags_SortMulti;

static std::string SortKeyStr(const SortSpec& s) {
    std::string r; for (auto& k : s) r += std::to_string(k.first) + (k.second ? "a" : "d") + ","; return r;
}

// A list view as a model: its column order, ALL its sort keys and its page position live together, and the rows are
// fetched again from the database whenever any of them changes (a sort click, a page change, a filter). Draw it with
// ListSetupColumns / ListHeaderRow(m.g.order, cols, m.g.sort, m.g.pager) so it is always sortable and rearrangeable.
template <class Row> struct ListModel {
    GridState g;
    struct Pg { std::vector<Row> rows; int count = 0; };
    Cached<Pg> cache;
    void Init(int ncols, SortSpec defaultSort, int pageSize) {
        if (!g.order.empty()) return;
        for (int i = 0; i < ncols; i++) g.order.push_back(i);
        g.sort.keys = defaultSort; g.pager.pageSize = pageSize;
    }
    // fetch(sort, offset, limit) -> rows of that page; count() -> all rows; key = whatever else the query depends on
    template <class Fetch, class Count> const Pg& Load(const std::string& key, Fetch fetch, Count count) {
        const Pg& p = cache.get(key + "|" + SortKeyStr(g.sort.keys) + "|" + std::to_string(g.pager.offset) + "|" + std::to_string(g.pager.pageSize), [&] {
            Pg r; r.count = count(); r.rows = fetch(g.sort.keys, g.pager.offset, g.pager.pageSize); return r; });
        g.pager.total = p.count;
        if (g.pager.offset > 0 && g.pager.offset >= g.pager.total) g.pager.Reset();
        return p;
    }
};

// The payment ranking of clients (Προσφορές, and each client's profile): state of one such table.
struct QualityView { GridState g; char q[128] = ""; std::string from, to; bool blkS = true, blkJ = true, blkT = true, more = false; };
static void DrawQuality(const char* id, QualityView& v, int onlyUser, float height);
static void OpenPartnerProfile(int id);

// Sort menu for the CARD view only (the list view sorts through its headers).
static void DrawSortMenu(const char* id, const char* const* names, const int* cols, int n, ColSort& st, Pager& pager) {
    std::string label = "Σειρά: ";
    bool any = false;
    for (auto& k : st.keys)
        for (int i = 0; i < n; i++)
            if (cols[i] == k.first) { label += std::string(any ? ", " : "") + names[i] + (k.second ? " ↑" : " ↓"); any = true; }
    if (!any) label += "—";
    ImGui::PushID(id);
    if (ImGui::Button(label.c_str())) ImGui::OpenPopup("##sortmenu");
    if (ImGui::BeginPopup("##sortmenu")) {
        for (int i = 0; i < n; i++) {
            ImGui::PushID(i);
            if (ImGui::BeginMenu(names[i])) {
                if (ImGui::MenuItem("↑ Αύξουσα")) { st.keys = { { cols[i], true } };  pager.Reset(); }
                if (ImGui::MenuItem("↓ Φθίνουσα")) { st.keys = { { cols[i], false } }; pager.Reset(); }
                ImGui::EndMenu();
            }
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
}






// ---------------------------------------------------------------------------
// BUILDINGS SCREEN ("Πολυκατοικίες") -- buildings are REAL stored entities
// (address, area, ΤΚ, building ΑΦΜ) with their own list of apartments. A client
// only points at the apartment they live in (users.apartment_id), so a building
// exists whether or not anyone lives in it, and vacant apartments are visible.
// Left pane = building index; right pane = building info + per-apartment table.
// ---------------------------------------------------------------------------
struct Money4 { double collected = 0.0, paidOut = 0.0, owedIn = 0.0, owedOut = 0.0; };
static void AddMoney4(Money4& m, const DataConn::MoneyTotals& t) {   // in cents: totals stay exact
    m.collected = FromCents(ToCents(m.collected) + ToCents(t.collected)); m.paidOut = FromCents(ToCents(m.paidOut) + ToCents(t.paidOut));
    m.owedIn = FromCents(ToCents(m.owedIn) + ToCents(t.owedIn));           m.owedOut = FromCents(ToCents(m.owedOut) + ToCents(t.owedOut));
}
static void AddMoney4(Money4& m, const Money4& t) {
    m.collected = FromCents(ToCents(m.collected) + ToCents(t.collected)); m.paidOut = FromCents(ToCents(m.paidOut) + ToCents(t.paidOut));
    m.owedIn = FromCents(ToCents(m.owedIn) + ToCents(t.owedIn));           m.owedOut = FromCents(ToCents(m.owedOut) + ToCents(t.owedOut));
}
// Money is counted where it happened: by the apartment stored on each charge. A resident's row shows only what
// they did IN THIS apartment; what earlier residents did there is a separate row. So every charge is counted
// exactly once, in one apartment of one building (a client who moved is never counted in two buildings).
struct AptAgg {
    Apartment apt;
    std::vector<int> userIndices;    // residents, indices into g_app.users
    std::vector<Money4> residentMoney;   // parallel to userIndices: their charges at this apartment
    std::vector<int> formerUserIds;      // earlier residents who have charges here
    Money4 former;                       // their charges here
    double collected = 0.0, paidOut = 0.0, owedIn = 0.0, owedOut = 0.0;   // whole apartment
};
struct BuildingAgg {
    Building b;
    std::string displayName;
    std::vector<AptAgg> apts;
    int residents = 0;
    int active = 0;                  // apartments with at least one resident
    int totalApts = 0;               // ALL apartments, occupied or vacant
    double collected = 0.0;          // Εισπράχθηκαν (in, resolved, amount_paid)
    double paidOut   = 0.0;          // Πληρώθηκαν (out, resolved, amount_paid)
    double owedIn    = 0.0;          // Οφείλονται να εισπραχθούν (in, resolved, balance>0)
    double owedOut   = 0.0;          // Οφείλονται να πληρωθούν (out, resolved, balance>0)
};

static bool g_showVacantApts = true;    // every apartment, vacant ones say so

// Resolved money per (apartment, client) -- one GROUP BY, cached until the data changes. Key "apt|user".
static const std::unordered_map<std::string, DataConn::MoneyTotals>& TotalsByAptUser() {
    static Cached<std::unordered_map<std::string, DataConn::MoneyTotals>> cache;
    return cache.get("", [] {
        std::unordered_map<std::string, DataConn::MoneyTotals> m;
        DataConn::PaymentFilter f; f.status = 1; f.joint = 1;   // a building is about its joint charges only (κοινόχρηστα), never a client's own services
        for (auto& g : g_app.conn->paymentTotalsBy(DataConn::PayGroup::ApartmentUser, f)) m[g.key] = g.t;
        return m;
    });
}

static std::vector<BuildingAgg> ComputeBuildings();
// Recomputed only when the data or the vacant-apartments toggle changes (was: every frame, with a DB
// query per building and a scan of every client and every payment per apartment).
static const std::vector<BuildingAgg>& BuildBuildings() {
    static Cached<std::vector<BuildingAgg>> cache;
    return cache.get(g_showVacantApts ? "v" : "", ComputeBuildings);
}

static std::vector<BuildingAgg> ComputeBuildings() {
    std::unordered_map<int, std::vector<int>> residentsOf;              // apartment -> indices into g_app.users
    for (size_t i = 0; i < g_app.users.size(); i++)
        if (g_app.users[i].apartment_id > 0) residentsOf[g_app.users[i].apartment_id].push_back((int)i);
    const auto& byAptUser = TotalsByAptUser();
    std::unordered_map<int, std::vector<std::pair<int, const DataConn::MoneyTotals*>>> byApt;   // apt -> (user, totals)
    for (auto& [k, v] : byAptUser) {
        size_t bar = k.find('|');
        byApt[std::atoi(k.c_str())].push_back({std::atoi(k.c_str() + bar + 1), &v});
    }
    std::vector<BuildingAgg> out;
    for (auto& b : g_app.conn->getBuildings()) {
        BuildingAgg agg;
        agg.b = b;
        agg.displayName = b.address.empty() ? "(χωρίς διεύθυνση)" : b.address;
        if (!b.area.empty())        agg.displayName += " · " + b.area;
        if (!b.postal_code.empty()) agg.displayName += " · Τ.Κ. " + b.postal_code;
        for (auto& a : g_app.conn->getApartmentsForBuilding(b.id)) {
            agg.totalApts++;   // count EVERY apartment -- vacant AND hidden ones too
            AptAgg aa; aa.apt = a;
            if (auto it = residentsOf.find(a.id); it != residentsOf.end()) aa.userIndices = it->second;
            aa.residentMoney.assign(aa.userIndices.size(), Money4{});
            Money4 whole;
            if (auto it = byApt.find(a.id); it != byApt.end()) {
                for (auto& [uid, tot] : it->second) {
                    AddMoney4(whole, *tot);
                    bool current = false;
                    for (size_t r = 0; r < aa.userIndices.size(); r++)
                        if (g_app.users[aa.userIndices[r]].id == uid) { AddMoney4(aa.residentMoney[r], *tot); current = true; }
                    if (!current) { AddMoney4(aa.former, *tot); if (uid > 0) aa.formerUserIds.push_back(uid); }
                }
            }
            aa.collected = whole.collected; aa.paidOut = whole.paidOut; aa.owedIn = whole.owedIn; aa.owedOut = whole.owedOut;
            bool hasMoney = !aa.formerUserIds.empty() || ToCents(whole.collected) || ToCents(whole.paidOut) || ToCents(whole.owedIn) || ToCents(whole.owedOut);
            // the building totals always include every apartment; the list hides empty vacant ones unless asked
            agg.collected = FromCents(ToCents(agg.collected) + ToCents(aa.collected)); agg.paidOut = FromCents(ToCents(agg.paidOut) + ToCents(aa.paidOut));
            agg.owedIn = FromCents(ToCents(agg.owedIn) + ToCents(aa.owedIn));          agg.owedOut = FromCents(ToCents(agg.owedOut) + ToCents(aa.owedOut));
            if (!aa.userIndices.empty()) agg.active++;
            agg.residents += (int)aa.userIndices.size();
            if (aa.userIndices.empty() && !g_showVacantApts && !hasMoney) continue;
            agg.apts.push_back(std::move(aa));
        }
        out.push_back(std::move(agg));
    }
    std::sort(out.begin(), out.end(), [](const BuildingAgg& a, const BuildingAgg& b) {
        return a.displayName < b.displayName;
    });
    return out;
}

// Building / apartment dialogs (drawn once per frame from DrawBuildingsScreen).
static bool g_showBldgDialog = false, g_bldgDialogWasOpen = false, g_bldgDialogIsEdit = false;
static int  g_bldgDialogId = -1;
static TextBuf bd_addr, bd_area, bd_postal, bd_afm;
static bool g_showAptDialog = false, g_aptDialogWasOpen = false, g_aptDialogIsEdit = false;
static int  g_aptDialogId = -1, g_aptDialogBuilding = -1;
static TextBuf ad_label;

// Forward declarations -- these are defined further down in the file
// (SubstrCI in the USERS section, OpenUserProfile just below the user dialog,
// StatTile in the dashboard helpers). Signatures must match exactly.
static bool SubstrCI(const std::string& hay, const std::string& needle);
static void OpenUserProfile(int id);
static void StatTile(const char* label, const std::string& value, ImVec4 accent, const char* sub, ImVec2 size);
static void OpenRecordPayment(int userId);
static void OpenReceiptView(int id);

// Jump to the Διαμερίσματα tab with a given apartment / building open (set from other tabs).
static int g_aptGotoId = -1;
static int g_aptGotoBuilding = -1;
static bool g_tabToApartments = false;

static std::vector<int> g_ledgerUserIds;
static void OpenResidentLedger(int uid) {
    if (uid < 0) return;
    if (std::find(g_ledgerUserIds.begin(), g_ledgerUserIds.end(), uid) == g_ledgerUserIds.end()) g_ledgerUserIds.push_back(uid);
}

static void DrawBuildingDialogs() {
    if (g_showBldgDialog && !g_bldgDialogWasOpen) ImGui::OpenPopup("Στοιχεία Πολυκατοικίας");
    g_bldgDialogWasOpen = g_showBldgDialog;
    if (g_showBldgDialog) {
        ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Appearing);
        if (BeginGuardedModal("Στοιχεία Πολυκατοικίας", g_showBldgDialog, ImGuiWindowFlags_AlwaysAutoResize, []() {
            std::vector<FieldDiff> d; if (!g_bldgDialogIsEdit) return d;
            Building o = g_app.conn->getBuildingById(g_bldgDialogId);
            DiffAdd(d, "Διεύθυνση", o.address, bd_addr.str()); DiffAdd(d, "Περιοχή", o.area, bd_area.str());
            DiffAdd(d, "Τ.Κ.", o.postal_code, bd_postal.str()); DiffAdd(d, "ΑΦΜ", o.afm, bd_afm.str());
            return d; })) {
            ImGui::InputText("Διεύθυνση", bd_addr.data, sizeof(bd_addr.data));
            ImGui::InputText("Περιοχή", bd_area.data, sizeof(bd_area.data));
            ImGui::InputText("Τ.Κ.", bd_postal.data, sizeof(bd_postal.data));
            ImGui::InputText("ΑΦΜ πολυκατοικίας", bd_afm.data, sizeof(bd_afm.data));
            ImGui::Spacing();
            if (ImGui::Button("Αποθήκευση", ImVec2(150, 0)) && !bd_addr.str().empty()) {
                Building nb; nb.address = bd_addr.str(); nb.area = bd_area.str();
                nb.postal_code = bd_postal.str(); nb.afm = bd_afm.str();
                if (g_bldgDialogIsEdit) {
                    Building old = g_app.conn->getBuildingById(g_bldgDialogId);
                    std::vector<FieldDiff> diffs = {
                        {"Διεύθυνση", old.address, nb.address}, {"Περιοχή", old.area, nb.area},
                        {"Τ.Κ.", old.postal_code, nb.postal_code}, {"ΑΦΜ", old.afm, nb.afm} };
                    int id = g_bldgDialogId;
                    RequestConfirm("Επεξεργασία πολυκατοικίας", diffs, [id, nb]() {
                        g_app.conn->editBuilding(id, nb); g_app.reloadUsers(); });
                } else {
                    RequestConfirm("Νέα πολυκατοικία", { {"Διεύθυνση", "-", nb.address} }, [nb]() {
                        g_app.conn->addBuilding(nb); g_app.reloadUsers(); });
                }
                g_showBldgDialog = false; ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Ακύρωση", ImVec2(150, 0))) { g_showBldgDialog = false; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
    }

    if (g_showAptDialog && !g_aptDialogWasOpen) ImGui::OpenPopup("Διαμέρισμα");
    g_aptDialogWasOpen = g_showAptDialog;
    if (g_showAptDialog) {
        ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_Appearing);
        if (BeginGuardedModal("Διαμέρισμα", g_showAptDialog, ImGuiWindowFlags_AlwaysAutoResize, []() {
            std::vector<FieldDiff> d; if (!g_aptDialogIsEdit) return d;
            std::string old;
            for (auto& b : g_app.conn->getBuildings())
                for (auto& a : g_app.conn->getApartmentsForBuilding(b.id)) if (a.id == g_aptDialogId) old = a.label;
            DiffAdd(d, "Ετικέτα", old, ad_label.str());
            return d; })) {
            ImGui::InputText("Ετικέτα (π.χ. Α1, 2ος)", ad_label.data, sizeof(ad_label.data));
            ImGui::Spacing();
            if (ImGui::Button("Αποθήκευση", ImVec2(150, 0)) && !ad_label.str().empty()) {
                std::string label = ad_label.str();
                int id = g_aptDialogId, bid = g_aptDialogBuilding;
                if (g_aptDialogIsEdit)
                    RequestConfirm("Επεξεργασία διαμερίσματος", { {"Ετικέτα", "-", label} }, [id, label]() {
                        g_app.conn->editApartment(id, label); g_app.reloadUsers(); });
                else
                    RequestConfirm("Νέο διαμέρισμα", { {"Ετικέτα", "-", label} }, [bid, label]() {
                        g_app.conn->addApartment(bid, label); g_app.reloadUsers(); });
                g_showAptDialog = false; ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Ακύρωση", ImVec2(150, 0))) { g_showAptDialog = false; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
    }
}

static void OpenJointCharge(int buildingId, int sourceType = 0, int sourceId = -1);
static void OpenGenericJointCharge(int buildingId);
static void OpenJointChargeFor(int sourceType, int sourceId, const std::string& label);   // package (1) or service (2) split over a building
static void DrawJointOpsTable(int buildingId, const char* id);

static std::string AptDateLabel(const std::string& ymd);

// One apartment's residency timeline, oldest first: every stay plus the empty periods between them,
// ending with today (a current resident, or "κενό" since the last one left).
struct StaySeg { int user_id = -1; std::string from, to; bool current = false; };
static std::vector<StaySeg> AptTimeline(int aptId) {
    auto rs = g_app.conn->getResidencies(aptId);
    std::sort(rs.begin(), rs.end(), [](const Residency& x, const Residency& y) { return x.from_date < y.from_date; });
    std::vector<StaySeg> out; std::string reach;            // latest end reached so far ("~" = still open)
    for (auto& r : rs) {
        if (!reach.empty() && reach != "~" && r.from_date > reach) out.push_back({ -1, reach, r.from_date, false });
        out.push_back({ r.user_id, r.from_date, r.to_date, r.to_date.empty() });
        std::string end = r.to_date.empty() ? "~" : r.to_date;
        if (reach.empty() || end == "~" || (reach != "~" && end > reach)) reach = end;
    }
    if (!reach.empty() && reach != "~") out.push_back({ -1, reach, "", true });
    if (out.empty()) out.push_back({ -1, "", "", true });
    return out;
}

static void DrawAptTimeline(int aptId, const char* id, float maxH = 0.0f) {
    struct TS { Cached<std::vector<StaySeg>> tl; Pager tp; int lastApt = -1; };
    static std::map<std::string, TS> st; TS& T = st[id];
    auto& tl = T.tl; Pager& tp = T.tp; int& lastApt = T.lastApt;
    if (lastApt != aptId) { lastApt = aptId; tp.Reset(); tp.pageSize = 10; }
    const auto& segs = tl.get(std::to_string(aptId), [&] { return AptTimeline(aptId); });
    tp.total = (int)segs.size();
    float h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.6f;
    if (ImGui::BeginTable((std::string(id) + "_t").c_str(), 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, maxH > 0 ? std::min(maxH, 40.0f + 26.0f * (float)std::min(tp.total, tp.pageSize)) : std::max(100.0f, h)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Κάτοικος", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Από", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Έως", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Διάρκεια", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableHeadersRow();
        for (int i = tp.offset; i < std::min(tp.total, tp.offset + tp.pageSize); i++) {
            const StaySeg& s = segs[i];
            ImGui::PushID(i); ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (s.user_id > 0) {
                ImGui::PushStyleColor(ImGuiCol_Text, kColLink);
                if (ImGui::Selectable(g_app.userName(s.user_id).c_str())) OpenResidentLedger(s.user_id);
                ImGui::PopStyleColor();
            } else ImGui::TextDisabled(s.current ? "Κανένας κάτοικος αυτή τη στιγμή" : "— κενό —");
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(s.from.empty() ? "—" : AptDateLabel(s.from).c_str());
            ImGui::TableSetColumnIndex(2);
            if (s.current) ImGui::TextColored(kColIn, "σήμερα"); else ImGui::TextUnformatted(AptDateLabel(s.to).c_str());
            ImGui::TableSetColumnIndex(3);
            if (!s.from.empty()) {
                auto days = [](const std::string& d) { int y = 0, m = 0, dd = 0; std::sscanf(d.c_str(), "%d-%d-%d", &y, &m, &dd);
                                                       return y * 365 + m * 30 + dd; };
                std::string to = s.to;
                if (to.empty()) { std::time_t t = std::time(nullptr); char tb[16]; std::strftime(tb, sizeof(tb), "%Y-%m-%d", std::localtime(&t)); to = tb; }
                int n = std::max(0, days(to) - days(s.from));
                if (n >= 365) ImGui::TextDisabled("%d έτ. %d μήν.", n / 365, (n % 365) / 30);
                else if (n >= 30) ImGui::TextDisabled("%d μήνες", n / 30);
                else ImGui::TextDisabled("%d ημέρες", n);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    DrawPager(tp, (std::string(id) + "_p").c_str());
}

static int g_bldSelApt = -1;

// Buildings tab, «Διαμερίσματα & Κάτοικοι»: who lives where now (no numbers). With the checkbox every stay
// becomes its own row with dates. Clicking an apartment opens its residency history below, oldest first.
static void OpenJointView(int id);
// What is still owed in a building: its joint charges that are not fully paid (the history is on the Κοινόχρηστα page).
static const std::vector<JointOp>& BuildingOpenJoints(int buildingId) {
    static std::map<int, Cached<std::vector<JointOp>>> caches;
    return caches[buildingId].get("", [&] { return g_app.conn->pageJointOps(buildingId, "", {{0, false}}, 0, 500, /*onlyOpen=*/true); });
}
static void DrawBuildingOpenJoints(int buildingId) {
    std::vector<JointOp> ops = BuildingOpenJoints(buildingId);
    if (ops.empty()) { ImGui::TextColored(kColIn, "Δεν εκκρεμεί κανένα κοινόχρηστο σε αυτή την πολυκατοικία."); return; }
    double owed = 0; for (auto& o : ops) owed = FromCents(ToCents(owed) + ToCents(o.total) - ToCents(std::min(o.paid, o.total)));
    ImGui::TextColored(kColOwed, "Εκκρεμούν %s σε %d κοινόχρηστα", Eur(owed).c_str(), (int)ops.size());
    ImGui::SameLine(); ImGui::TextDisabled("· το ιστορικό όλων των χρεώσεων είναι στη σελίδα «Κοινόχρηστα»");
    if (ImGui::BeginTable("bldopenjoint", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp | kSortableTable,
                          ImVec2(0, std::max(120.0f, ImGui::GetContentRegionAvail().y - 8)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Ημ/νία", ImGuiTableColumnFlags_WidthFixed, 90); ImGui::TableSetupColumn("Κοινόχρηστο", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Σύνολο", ImGuiTableColumnFlags_WidthFixed, 90); ImGui::TableSetupColumn("Εισπράχθηκαν", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("Υπόλοιπο", ImGuiTableColumnFlags_WidthFixed, 90); ImGui::TableSetupColumn("Συμπληρώθηκε", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 80);
        ImGui::TableHeadersRow();
        SortByTableSpecs(ops, [](int c, const JointOp& a, const JointOp& b) {
            auto left = [](const JointOp& o) { return ToCents(o.total) - ToCents(std::min(o.paid, o.total)); };
            switch (c) { case 0: return Cmp3(a.op_date, b.op_date); case 1: return CmpText(a.label, b.label); case 2: return Cmp3(ToCents(a.total), ToCents(b.total));
                         case 3: return Cmp3(ToCents(a.paid), ToCents(b.paid)); case 4: return Cmp3(left(a), left(b));
                         default: return Cmp3(a.total > 0 ? a.paid / a.total : 1.0, b.total > 0 ? b.paid / b.total : 1.0); } });
        for (auto& o : ops) {
            double paid = std::min(o.paid, o.total), pct = o.total > 0.005 ? paid / o.total * 100.0 : 100.0;
            ImGui::PushID(o.id); ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(DateGr(o.op_date).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(o.label.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(Eur(o.total).c_str());
            ImGui::TableNextColumn(); ImGui::TextColored(kColIn, "%s", Eur(paid).c_str());
            ImGui::TableNextColumn(); ImGui::TextColored(kColOwed, "%s", Eur(MoneySub(o.total, paid)).c_str());
            ImGui::TableNextColumn(); { char bf[24]; std::snprintf(bf, sizeof(bf), "%.0f%%", pct);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.85f, 0.60f, 0.25f, 1)); ImGui::ProgressBar((float)(pct / 100.0), ImVec2(-1, 0), bf); ImGui::PopStyleColor(); }
            ImGui::TableNextColumn(); if (ImGui::SmallButton("Άνοιγμα")) OpenJointView(o.id);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

static void DrawBuildingApartments(const BuildingAgg& b) {
    static bool history = false; static char q[96] = ""; static Pager pager; static int lastB = -1;
    if (lastB != b.b.id) { lastB = b.b.id; pager.Reset(); g_bldSelApt = -1; }
    if (ImGui::Checkbox("Εμφάνιση προηγούμενων ενοίκων", &history)) pager.Reset();
    ImGui::SameLine(); ImGui::SetNextItemWidth(260);
    if (ImGui::InputTextWithHint("##bapq", "Αναζήτηση διαμερίσματος ή ενοίκου...", q, sizeof(q))) pager.Reset();

    struct Row { const AptAgg* a; StaySeg s; bool first; };
    static Cached<std::vector<Row>> cache;
    const std::vector<Row>& rows = cache.get(std::to_string(b.b.id) + (history ? "h" : "") + "|" + q, [&] {
        std::vector<Row> out;
        for (auto& a : b.apts) {
            std::vector<StaySeg> segs;
            if (history) segs = AptTimeline(a.apt.id);
            else {
                for (int ui : a.userIndices) segs.push_back({ g_app.users[ui].id, "", "", true });
                if (segs.empty()) segs.push_back({ -1, "", "", true });
            }
            if (q[0]) {
                bool hit = SubstrCI(a.apt.label, q);
                for (auto& sg : segs) if (sg.user_id > 0 && SubstrCI(g_app.userName(sg.user_id), q)) hit = true;
                if (!hit) continue;
            }
            for (size_t i = 0; i < segs.size(); i++) out.push_back({ &a, segs[i], i == 0 });
        }
        return out;
    });
    pager.total = (int)rows.size();
    if (pager.offset >= pager.total) pager.Reset();

    const float avail = ImGui::GetContentRegionAvail().y;
    const bool showDetail = g_bldSelApt > 0;
    float tableH = (showDetail ? avail * 0.50f : avail) - ImGui::GetFrameHeightWithSpacing() * 1.6f;
    int ncol = history ? 5 : 3;
    if (ImGui::BeginTable("bldgapts", ncol, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                          ImVec2(0, std::max(140.0f, tableH)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Διαμέρισμα", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Κάτοικος", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        if (history) {
            ImGui::TableSetupColumn("Από", ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableSetupColumn("Έως", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        }
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableHeadersRow();
        for (int i = pager.offset; i < std::min(pager.total, pager.offset + pager.pageSize); i++) {
            const Row& r = rows[i];
            ImGui::PushID(i); ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            bool firstOnPage = r.first || i == pager.offset;
            if (firstOnPage) {
                ImGui::PushStyleColor(ImGuiCol_Text, kColLink);
                if (ImGui::Selectable(r.a->apt.label.c_str(), g_bldSelApt == r.a->apt.id))
                    g_bldSelApt = g_bldSelApt == r.a->apt.id ? -1 : r.a->apt.id;
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ιστορικό διαμονής του διαμερίσματος");
            }
            ImGui::TableSetColumnIndex(1);
            if (r.s.user_id > 0) {
                ImGui::PushStyleColor(ImGuiCol_Text, r.s.current ? kColLink : kColMuted);
                if (ImGui::Selectable(g_app.userName(r.s.user_id).c_str())) OpenResidentLedger(r.s.user_id);
                ImGui::PopStyleColor();
            } else if (r.s.current) ImGui::TextDisabled("Κανένας κάτοικος αυτή τη στιγμή");
            else ImGui::TextDisabled("— κενό —");
            if (history) {
                ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(r.s.from.empty() ? "—" : AptDateLabel(r.s.from).c_str());
                ImGui::TableSetColumnIndex(3);
                if (r.s.current) ImGui::TextColored(kColIn, "σήμερα"); else ImGui::TextUnformatted(AptDateLabel(r.s.to).c_str());
            }
            ImGui::TableSetColumnIndex(ncol - 1);
            if (r.first) {
                if (ImGui::SmallButton("Μετονομασία")) {
                    g_showAptDialog = true; g_aptDialogIsEdit = true; g_aptDialogId = r.a->apt.id;
                    g_aptDialogBuilding = b.b.id; ad_label.set(r.a->apt.label);
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("X")) {
                    int id = r.a->apt.id; std::string lbl = r.a->apt.label;
                    RequestConfirm("Διαγραφή διαμερίσματος (οι κάτοικοι μένουν χωρίς διαμέρισμα)",
                                   { {"Διαμέρισμα", lbl, "(διαγραφή)"} }, [id]() {
                        g_app.conn->removeApartment(id); g_app.reloadUsers(); });
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    DrawPager(pager, "bldaptpg");

    // ---- residency history of the clicked apartment ----
    if (!showDetail) return;
    const AptAgg* sel = nullptr;
    for (auto& a : b.apts) if (a.apt.id == g_bldSelApt) sel = &a;
    if (!sel) { g_bldSelApt = -1; return; }
    SectionTitle(("Ιστορικό διαμονής — " + sel->apt.label).c_str());
    ImGui::TextDisabled("Εισπράχθηκαν %s  ·  Οφείλονται %s", Eur(sel->collected).c_str(), Eur(sel->owedIn).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Κλείσιμο")) { g_bldSelApt = -1; return; }
    DrawAptTimeline(sel->apt.id, "bldtl");
}

// The selected building: its money, its apartments (with current residents) and its joint charges.
// The building as a client: every joint charge made to it (whole amounts, not per apartment), one page at a time.
static void DrawBuildingCharges(int buildingId) {
    static Pager pager; static int lastB = -1;
    if (lastB != buildingId) { lastB = buildingId; pager.Reset(); pager.pageSize = 15; }
    struct Tot { double charged = 0, collected = 0, cost = 0; int n = 0; };
    static Cached<Tot> tc;
    const Tot& t = tc.get(std::to_string(buildingId), [&] {
        Tot r;
        for (auto& o : g_app.conn->getJointOps(buildingId, 0, 1000000)) {
            r.n++; r.charged = FromCents(ToCents(r.charged) + ToCents(o.total));
            r.collected = FromCents(ToCents(r.collected) + ToCents(std::min(o.paid, o.total)));
            r.cost = FromCents(ToCents(r.cost) + ToCents(g_app.conn->getJointOpCost(o.id)));
        }
        return r; });
    pager.total = t.n;
    if (ImGui::BeginTable("bldsum", 4, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        double left = MoneySub(t.charged, t.collected), profit = MoneySub(t.charged, t.cost);
        ImGui::TableNextColumn(); StatTile("Χρεώθηκε η πολυκατοικία", Eur(t.charged), ImVec4(0.95f, 0.75f, 0.45f, 1), (std::to_string(t.n) + " χρεώσεις").c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 84));
        ImGui::TableNextColumn(); StatTile("Εισπράχθηκαν από ενοίκους", Eur(t.collected), kColIn, nullptr, ImVec2(ImGui::GetContentRegionAvail().x, 84));
        ImGui::TableNextColumn(); StatTile("Μένουν να εισπραχθούν", Eur(left), left > 0.005 ? kColOwed : kColMuted, nullptr, ImVec2(ImGui::GetContentRegionAvail().x, 84));
        ImGui::TableNextColumn(); StatTile("Κόστος μας · διαφορά", Eur(t.cost), kColOut, ("διαφορά " + Eur(profit)).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 84));
        ImGui::EndTable();
    }
    if (t.n == 0) { ImGui::TextDisabled("Καμία χρέωση σε αυτή την πολυκατοικία ακόμα."); return; }
    struct Row { JointOp op; double cost = 0; };
    static Cached<std::vector<Row>> pc;
    const auto& rows = pc.get(std::to_string(buildingId) + "|" + std::to_string(pager.offset) + "|" + std::to_string(pager.pageSize), [&] {
        std::vector<Row> v;
        for (auto& o : g_app.conn->getJointOps(buildingId, pager.offset, pager.pageSize)) v.push_back({o, g_app.conn->getJointOpCost(o.id)});
        return v; });
    float h = std::max(160.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.6f);
    if (ImGui::BeginTable("bldcharges", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp, ImVec2(0, h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Ημ/νία", ImGuiTableColumnFlags_WidthFixed, 90); ImGui::TableSetupColumn("Τι χρεώθηκε", 0, 2.2f);
        ImGui::TableSetupColumn("Σύνολο", ImGuiTableColumnFlags_WidthFixed, 85); ImGui::TableSetupColumn("Εισπράχθηκαν", ImGuiTableColumnFlags_WidthFixed, 95);
        ImGui::TableSetupColumn("Μένουν", ImGuiTableColumnFlags_WidthFixed, 85); ImGui::TableSetupColumn("Κόστος μας", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("Κατάσταση", ImGuiTableColumnFlags_WidthFixed, 100); ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableHeadersRow();
        for (auto& r : rows) {
            const JointOp& o = r.op;
            double paid = std::min(o.paid, o.total), left = MoneySub(o.total, paid);
            ImGui::PushID(o.id); ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(DateGr(o.op_date).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(o.label.c_str());
            if (!o.note.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", o.note.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(Eur(o.total).c_str());
            ImGui::TableNextColumn(); if (paid > 0.005) ImGui::TextColored(kColIn, "%s", Eur(paid).c_str()); else ImGui::TextDisabled("—");
            ImGui::TableNextColumn(); if (left > 0.005) ImGui::TextColored(kColOwed, "%s", Eur(left).c_str()); else ImGui::TextDisabled("—");
            ImGui::TableNextColumn(); if (r.cost > 0.005) ImGui::TextColored(kColOut, "%s", Eur(r.cost).c_str()); else ImGui::TextDisabled("—");
            ImGui::TableNextColumn();
            if (ToCents(paid) >= ToCents(o.total)) ImGui::TextColored(kColIn, "Εξοφλήθηκε"); else if (paid > 0.005) ImGui::TextColored(kColOwed, "Μερικώς"); else ImGui::TextColored(kColOut, "Απλήρωτη");
            ImGui::TableNextColumn(); if (ImGui::SmallButton("Άνοιγμα")) OpenJointView(o.id);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Τι χρεώθηκε κάθε διαμέρισμα και τι πλήρωσε");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    DrawPager(pager, "bldchpg");
}

static void DrawBuildingDetail(const BuildingAgg& b) {

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f,0.95f,1.0f,1.0f));
    ImGui::SetWindowFontScale(1.15f);
    ImGui::TextUnformatted(b.displayName.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();

    ImGui::TextDisabled("ΑΦΜ: %s  ·  %d ενεργά διαμερίσματα  ·  %d κάτοικοι",
                        b.b.afm.empty() ? "—" : b.b.afm.c_str(), b.active, b.residents);
    if (ImGui::Button("Επεξεργασία πολυκατοικίας")) {
        g_showBldgDialog = true; g_bldgDialogIsEdit = true; g_bldgDialogId = b.b.id;
        bd_addr.set(b.b.address); bd_area.set(b.b.area); bd_postal.set(b.b.postal_code); bd_afm.set(b.b.afm);
    }
    ImGui::SameLine();
    if (ImGui::Button("Διαγραφή πολυκατοικίας")) {
        int id = b.b.id; std::string name = b.displayName;
        RequestConfirm("Διαγραφή πολυκατοικίας (οι κάτοικοι μένουν χωρίς διαμέρισμα)",
                       { {"Πολυκατοικία", name, "(διαγραφή)"} }, [id]() {
            g_app.conn->removeBuilding(id); g_app.reloadUsers(); });
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Διαμέρισμα")) {
        g_showAptDialog = true; g_aptDialogIsEdit = false; g_aptDialogId = -1;
        g_aptDialogBuilding = b.b.id; ad_label.set("");
    }
    ImGui::SameLine();
    if (ImGui::Button("Χρέωση κοινοχρήστου")) OpenJointCharge(b.b.id);
    ImGui::SameLine();
    if (ImGui::Button("Διαχείριση Κοινοχρήστων")) OpenGenericJointCharge(b.b.id);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Γενική χρέωση της πολυκατοικίας για ό,τι δεν είναι υπηρεσία");
    ImGui::Spacing();

    // ---- Logistics: 4 stat tiles (same visual language as profile/dashboard) ----
    char buf[64];
    std::string finHead = "Οικονομικά κοινοχρήστων  ·  οφείλονται " + Eur(b.owedIn) + "###bldgfin";
    if (ImGui::CollapsingHeader(finHead.c_str()) && ImGui::BeginTable("bldgstats", 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextRow();
        struct Tile { const char* label; double v; ImVec4 col; };
        Tile tiles[4] = {
            {"Εισπράχθηκαν", b.collected, kColIn},
            {"Πληρώθηκαν", b.paidOut, kColOut},
            {"Οφείλονται να εισπραχθούν", b.owedIn, kColOwed},
            {"Οφείλονται να πληρωθούν", b.owedOut, kColWeOwe} };
        for (int i = 0; i < 4; i++) {
            ImGui::TableNextColumn(); ImGui::PushID(i);
            float w = ImGui::GetContentRegionAvail().x;
            std::snprintf(buf, sizeof(buf), "%s", Eur(tiles[i].v).c_str());
            StatTile(tiles[i].label, buf, tiles[i].col, nullptr, ImVec2(w, 88));
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    int nJoint = (int)BuildingOpenJoints(b.b.id).size();
    if (!ImGui::BeginTabBar("bldgtabs")) return;
    if (ImGui::BeginTabItem("Χρεώσεις πολυκατοικίας")) {
        DrawBuildingCharges(b.b.id);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Διαμερίσματα & Κάτοικοι")) {

        DrawBuildingApartments(b);
    ImGui::EndTabItem();
    }
    std::string jointTab = "Εκκρεμή κοινόχρηστα (" + std::to_string(nJoint) + ")###bldgjointtab";
    if (ImGui::BeginTabItem(jointTab.c_str())) {
        DrawBuildingOpenJoints(b.b.id);
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();

}

// ---------------------------------------------------------------------------
// USERS SCREEN
// ---------------------------------------------------------------------------
static bool g_showUserDialog = false;
static bool g_userDialogWasOpen = false;   // ← new
static bool g_userDialogIsEdit = false;
static int g_userDialogId = -1;
static TextBuf ub_name, ub_phone, ub_special;
static int g_userDialogApt = -1;           // chosen apartment id (-1 = none)

// --- Πελάτες page: search-field scope, name-letter filter, view toggle, sort ---
static int g_userSearchField = 0;   // index into kUserSearchFields
static const char* kUserSearchFields[] = { "Όλα", "Ονοματεπώνυμο", "Ειδικός Κωδικός", "Τηλέφωνο", "Διεύθυνση" };
static std::set<std::string> g_usersNameLetters;   // active letter filter (empty = all)
static bool g_usersShowResidence = true;           // Στοιχεία κατοικίας: address / ΤΚ / area / apartment
static bool g_usersTableView = true;               // false = cards, true = table (default: list)

static const char* kUserSortFields[] = { "ΚΩΔ", "ΔΙΕΥΘΥΝΣΗ", "ΟΝΟΜΑ", "ΤΗΛ", "ΠΕΡΙΟΧΗ", "ΤΚ", "ΕΙΔ.ΚΩΔ" };
static const int kUserSortToCol[] = { 0, 1, 3, 4, 5, 6, 7 };   // menu entry -> table column

static void OpenAddUser() {
    g_showUserDialog = true; g_userDialogIsEdit = false; g_userDialogId = -1;
    ub_name.set(""); ub_phone.set(""); ub_special.set(""); g_userDialogApt = -1;
}
static void OpenEditUser(const User& u) {
    g_showUserDialog = true; g_userDialogIsEdit = true; g_userDialogId = u.id;
    ub_name.set(u.full_name); ub_phone.set(u.phone); ub_special.set(u.special_code);
    g_userDialogApt = u.apartment_id;
}

static std::string AptDisplay(int aptId) {
    for (auto& b : g_app.conn->getBuildings())
        for (auto& a : g_app.conn->getApartmentsForBuilding(b.id))
            if (a.id == aptId) return b.address + " — " + a.label;
    return "";
}

// Every apartment id that is currently linked to some client. Used to keep
// the residence picker honest: a client can only be pointed at an EMPTY unit
// (or their own current one while editing) -- never at someone else's.
static std::set<int> OccupiedApartmentIds() {
    std::set<int> occ;
    for (auto& u : g_app.users) if (u.apartment_id >= 0) occ.insert(u.apartment_id);
    return occ;
}

static void DrawUserDialog() {
    if (g_showUserDialog && !g_userDialogWasOpen) {
        ImGui::OpenPopup("Στοιχεία Πελάτη");
    }
    g_userDialogWasOpen = g_showUserDialog;
    if (!g_showUserDialog) return;
    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Appearing);
    if (BeginGuardedModal("Στοιχεία Πελάτη", g_showUserDialog, ImGuiWindowFlags_AlwaysAutoResize, []() {
        std::vector<FieldDiff> d; if (!g_userDialogIsEdit) return d;
        User o = g_app.conn->getUserById(g_userDialogId);
        DiffAdd(d, "Όνομα", o.full_name, ub_name.str()); DiffAdd(d, "Τηλέφωνο", o.phone, ub_phone.str());
        DiffAdd(d, "Ειδ. Κωδικός", o.special_code, ub_special.str());
        if (o.apartment_id != g_userDialogApt)
            d.push_back({"Κατοικία", o.apartment_id >= 0 ? AptDisplay(o.apartment_id) : "-", g_userDialogApt >= 0 ? AptDisplay(g_userDialogApt) : "-"});
        return d; })) {
        ImGui::InputText("Ονοματεπώνυμο", ub_name.data, sizeof(ub_name.data));
        ImGui::InputText("Τηλέφωνο", ub_phone.data, sizeof(ub_phone.data));
        ImGui::InputText("Ειδικός Κωδικός", ub_special.data, sizeof(ub_special.data));

        // Residence picker. Only EMPTY apartments are offered. When editing,
        // the client's OWN current apartment is also shown (so saving without
        // touching the field doesn't silently unassign them). No other occupied
        // unit ever appears, which is what guarantees a new client can never
        // corrupt data by overwriting somebody else's apartment.
        std::set<int> occupied = OccupiedApartmentIds();
        int currentApt = g_userDialogApt;
        std::string preview = currentApt >= 0 ? AptDisplay(currentApt) : "(καμία)";
        if (preview.empty() && currentApt >= 0) preview = "(άγνωστο διαμέρισμα)";

        ImGui::TextDisabled("Κατοικία  —  διαθέσιμα: μόνο κενά διαμερίσματα");
        if (ImGui::BeginCombo("##aptcombo", preview.c_str())) {
            if (ImGui::Selectable("(καμία)", g_userDialogApt < 0)) g_userDialogApt = -1;

            int shown = 0;
            for (auto& b : g_app.conn->getBuildings()) {
                std::vector<Apartment> avail;
                for (auto& a : g_app.conn->getApartmentsForBuilding(b.id)) {
                    bool isOccupied = occupied.count(a.id) > 0;
                    bool isCurrent  = (a.id == currentApt);
                    if (!isOccupied || isCurrent) avail.push_back(a);   // hide everyone else's
                }
                if (avail.empty()) continue;
                ImGui::Separator();
                std::string head = b.address + (b.area.empty() ? "" : "  ·  " + b.area);
                ImGui::TextDisabled("%s", head.c_str());
                for (auto& a : avail) {
                    ImGui::PushID(a.id);
                    bool isCurrent = (a.id == currentApt);
                    std::string label = "    " + a.label + (isCurrent ? "   (τρέχουσα)" : "   (κενό)");
                    if (ImGui::Selectable(label.c_str(), isCurrent)) g_userDialogApt = a.id;
                    ImGui::PopID();
                    shown++;
                }
            }
            if (shown == 0) {
                ImGui::Separator();
                ImGui::TextDisabled("Δεν υπάρχουν διαθέσιμα κενά διαμερίσματα.");
                ImGui::TextDisabled("Προσθέστε νέα από την καρτέλα «Πολυκατοικίες».");
            }
            ImGui::EndCombo();
        }
        ImGui::Spacing();
        if (ImGui::Button("Αποθήκευση", ImVec2(150, 0))) {
            User nu;
            nu.full_name = ub_name.str(); nu.phone = ub_phone.str();
            nu.special_code = ub_special.str();
            nu.apartment_id = g_userDialogApt;
            if (g_userDialogIsEdit) {
                User old = g_app.conn->getUserById(g_userDialogId);
                std::vector<FieldDiff> diffs = {
                    {"Όνομα", old.full_name, nu.full_name}, {"Τηλέφωνο", old.phone, nu.phone},
                    {"Ειδ. Κωδικός", old.special_code, nu.special_code},
                    {"Κατοικία", old.apartment_id >= 0 ? AptDisplay(old.apartment_id) : "-",
                                 nu.apartment_id >= 0 ? AptDisplay(nu.apartment_id) : "-"},
                };
                int id = g_userDialogId;
                RequestConfirm("Επεξεργασία πελάτη", diffs, [id, nu]() {
                    g_app.conn->editUser(id, nu);
                    g_app.reloadUsers();
                });
            } else {
                std::vector<FieldDiff> diffs = { {"Όνομα", "-", nu.full_name}, {"Τηλέφωνο", "-", nu.phone} };
                RequestConfirm("Νέος πελάτης", diffs, [nu]() {
                    g_app.conn->addUser(nu);
                    g_app.reloadUsers();
                });
            }
            g_showUserDialog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Ακύρωση", ImVec2(150, 0))) { g_showUserDialog = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

// Drill-down navigation state (bare minimum: User profile -> Service detail ->
// Payment viewer, a dead end with nowhere further to go). Defined here, ahead
// of the screens that trigger them, so those screens can just set the ids
// directly; the actual window-drawing functions are defined further down
// (after the payment dialog they reuse) and invoked once per frame from the
// main loop regardless of which tab is active.
// Multi-window state: any number of each type can be open at once.
// Opening the same ID twice is a no-op (push-if-absent), so clicking
// the same row twice doesn't spawn a duplicate window. Closing a window
// (X button) removes its entry, leaving the others untouched.
static std::vector<int> g_profileUserIds;
static std::vector<int> g_detailServiceIds;
static std::vector<int> g_pkgViewIds;                // package viewers (windows) that are open
static void OpenPackageView(int id) { if (std::find(g_pkgViewIds.begin(), g_pkgViewIds.end(), id) == g_pkgViewIds.end()) g_pkgViewIds.push_back(id); }
static std::vector<int> g_viewPaymentIds;

static void OpenUserProfile(int id) {
    if (id < 0) return;
    if (std::find(g_profileUserIds.begin(), g_profileUserIds.end(), id) == g_profileUserIds.end())
        g_profileUserIds.push_back(id);
}
static void OpenServiceDetail(int id) {
    if (id < 0) return;
    if (std::find(g_detailServiceIds.begin(), g_detailServiceIds.end(), id) == g_detailServiceIds.end())
        g_detailServiceIds.push_back(id);
}
static void OpenPaymentView(int id) {
    if (id < 0) return;
    if (std::find(g_viewPaymentIds.begin(), g_viewPaymentIds.end(), id) == g_viewPaymentIds.end())
        g_viewPaymentIds.push_back(id);
}
static char g_userSearch[128] = "";

// ASCII-case-insensitive substring test (matches the app-wide search behaviour;
// Greek is stored uppercase, so byte-exact match on Greek is what we want).
// Fold text for searching: ASCII lower-cased; Greek lower/upper case, accents and the
// final sigma are all mapped to one unaccented capital, so "παπαδοπουλος" matches
// "ΠΑΠΑΔΟΠΟΥΛΟΣ" and "ελενη" matches "Ελένη".

static bool SubstrCI(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    return FoldGreek(hay).find(FoldGreek(needle)) != std::string::npos;
}

// Finds the next case-insensitive occurrence of `needle` in `hay`, starting at
// `start`. Returns npos if none. Greek is stored uppercase so byte-compare is
// exact for it; ASCII-lowering handles the common Latin case.
static size_t FindCI(const std::string& hay, const std::string& needle, size_t start) {
    if (needle.empty() || hay.size() < needle.size()) return std::string::npos;
    for (size_t i = start; i + needle.size() <= hay.size(); i++) {
        bool ok = true;
        for (size_t j = 0; j < needle.size(); j++) {
            if (std::tolower((unsigned char)hay[i + j]) != std::tolower((unsigned char)needle[j])) { ok = false; break; }
        }
        if (ok) return i;
    }
    return std::string::npos;
}

// Renders `text`, painting a yellow background behind every case-insensitive
// occurrence of `needle`. Empty needle = plain text, no highlight. Used in
// table cells and card fields so search hits visually pop.
// Text that looks clickable (link colour, underline on hover); returns true on a click. Search hits are highlighted as in TextHighlighted.
static void TextHighlighted(const std::string& text, const std::string& needle);
static bool LinkCell(const std::string& text, const std::string& needle = "") {
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float w = std::max(8.0f, ImGui::GetContentRegionAvail().x);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.78f, 1.0f, 1.0f));
    TextHighlighted(text, needle);
    ImGui::PopStyleColor();
    ImVec2 p1 = ImVec2(p0.x + w, ImGui::GetCursorScreenPos().y);
    bool hov = !text.empty() && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && ImGui::IsMouseHoveringRect(p0, p1);
    if (hov) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p0.x, p1.y - 1), ImVec2(p0.x + std::min(w, ImGui::CalcTextSize(text.c_str()).x), p1.y - 1), IM_COL32(140, 200, 255, 200));
        if (ImGui::IsMouseClicked(0)) return true;
    }
    return false;
}

static void TextHighlighted(const std::string& text, const std::string& needle) {
    if (needle.empty() || text.empty()) { ImGui::TextUnformatted(text.c_str()); return; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 hlCol = IM_COL32(255, 220, 40, 170);
    const float lineH = ImGui::GetTextLineHeight();
    size_t pos = 0;
    bool onLine = false;
    while (pos < text.size()) {
        size_t hit = FindCI(text, needle, pos);
        if (hit == std::string::npos) hit = text.size();
        if (hit > pos) {
            if (onLine) ImGui::SameLine(0, 0);
            ImGui::TextUnformatted(text.substr(pos, hit - pos).c_str());
            onLine = true;
        }
        if (hit >= text.size()) return;
        if (onLine) ImGui::SameLine(0, 0);
        std::string seg = text.substr(hit, needle.size());
        ImVec2 p  = ImGui::GetCursorScreenPos();
        ImVec2 sz = ImGui::CalcTextSize(seg.c_str());
        dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + lineH), hlCol);
        ImGui::TextUnformatted(seg.c_str());
        onLine = true;
        pos = hit + needle.size();
    }
}

// Shortens to fit maxW pixels; appends "..." when truncated. Guards the card
// and table layouts from being stretched/pushed apart by an over-long value.
static std::string TruncToWidth(const std::string& s, float maxW) {
    if (maxW <= 0.0f || ImGui::CalcTextSize(s.c_str()).x <= maxW) return s;
    std::string cur = s;
    while (!cur.empty() && ImGui::CalcTextSize((cur + "...").c_str()).x > maxW) {
        cur.pop_back();
        while (!cur.empty() && ((unsigned char)cur.back() & 0xC0) == 0x80) cur.pop_back();   // never cut a UTF-8 character in half
        if (!cur.empty() && ((unsigned char)cur.back() & 0xC0) == 0xC0) cur.pop_back();       // dangling lead byte
    }
    return cur + "...";
}


// ---------------------------------------------------------------------------
// Πελάτες table columns. Logical IDs are stable across frames so the user's
// display order survives the Αναδιάταξη Κολονών toggle (which only controls
// whether dragging is allowed -- the order itself is our own in-memory state,
// never written to the DB). Στοιχεία κατοικίας truly hides/readds the four
// residence columns by omitting them from TableSetupColumn, and the row body
// writes every cell through a logical->display lookup, so nothing shifts
// when columns come and go.
// ---------------------------------------------------------------------------
enum UserColId {
    UCOL_ID = 0, UCOL_ADDR, UCOL_APT, UCOL_NAME, UCOL_PHONE,
    UCOL_AREA, UCOL_TK, UCOL_SPECIAL,
    UCOL_COUNT
};

struct UserColDef {
    const char*           name;
    ImGuiTableColumnFlags width_flag;   // WidthFixed or WidthStretch
    float                 value;        // pixels if fixed, weight if stretch
    bool                  pinned;       // NoResize | NoReorder (ID, ACTIONS)
    bool                  residence;    // hidden when g_usersShowResidence is off
};

static const UserColDef kUserColDefs[UCOL_COUNT] = {
    { "ΚΩΔ",       ImGuiTableColumnFlags_WidthFixed,   110.0f, true,  false },
    { "ΔΙΕΥΘΥΝΣΗ", ImGuiTableColumnFlags_WidthStretch, 2.0f,   false, true  },
    { "ΔΙΑΜΕΡ.",   ImGuiTableColumnFlags_WidthStretch, 1.5f,   false, true  },
    { "ΟΝΟΜΑ",     ImGuiTableColumnFlags_WidthStretch, 2.0f,   false, false },
    { "ΤΗΛ",       ImGuiTableColumnFlags_WidthStretch, 1.3f,   false, false },
    { "ΠΕΡΙΟΧΗ",   ImGuiTableColumnFlags_WidthStretch, 1.6f,   false, true  },
    { "ΤΚ",        ImGuiTableColumnFlags_WidthStretch, 1.0f,   false, true  },
    { "ΕΙΔ.ΚΩΔ",   ImGuiTableColumnFlags_WidthStretch, 1.3f,   false, false },
};
// Header definitions for the arrow headers (name, width, has <>^v) and the user's column order.
static const ListColDef kUserListCols[UCOL_COUNT] = {
    { "ΚΩΔ",       ImGuiTableColumnFlags_WidthFixed,   110.0f, true  },
    { "ΔΙΕΥΘΥΝΣΗ", ImGuiTableColumnFlags_WidthStretch, 2.0f,   true  },
    { "ΔΙΑΜΕΡ.",   ImGuiTableColumnFlags_WidthStretch, 1.5f,   true  },
    { "ΟΝΟΜΑ",     ImGuiTableColumnFlags_WidthStretch, 2.0f,   true  },
    { "ΤΗΛ",       ImGuiTableColumnFlags_WidthStretch, 1.3f,   true  },
    { "ΠΕΡΙΟΧΗ",   ImGuiTableColumnFlags_WidthStretch, 1.6f,   true  },
    { "ΤΚ",        ImGuiTableColumnFlags_WidthStretch, 1.0f,   true  },
    { "ΕΙΔ.ΚΩΔ",   ImGuiTableColumnFlags_WidthStretch, 1.3f,   true  },
};
static std::vector<int> g_usersColOrder = { 0, 1, 2, 3, 4, 5, 6, 7 };

static void DrawUsersTable(const std::vector<User>& rows) {
    // Fixed canonical setup order (never changed by user drag state). ImGui's
    // own per-column DisplayOrder tracking handles the visual reordering; because
    // the setup order never changes, that display order is preserved across
    // frames AND across Reorderable on/off toggles. Toggling Reorderable only
    // affects whether ImGui accepts drag input -- it never touches stored order,
    // which is why "turn reorder off" keeps whatever arrangement you made.
    // Reorderable is ALWAYS on so ImGui never throws away the user's current
    // column arrangement. Whether the user can actually drag headers is
    // controlled per-column via NoReorder (set on every column while the
    // Αναδιάταξη Κολονών toggle is OFF): NoReorder blocks the drag but leaves
    // each column's stored DisplayOrder intact, so the layout the user built
    // stays exactly where they left it when they lock the columns again.
    if (!ImGui::BeginTable("userstable", UCOL_COUNT,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable |
            ImGuiTableFlags_Hideable)) return;

    ImGui::TableSetupScrollFreeze(1, 1);
    ListSetupColumns(g_usersColOrder, kUserListCols);
    // Στοιχεία κατοικίας: hide/show the residence columns (display index = position).
    for (int d = 0; d < UCOL_COUNT; ++d)
        if (kUserColDefs[g_usersColOrder[d]].residence) ImGui::TableSetColumnEnabled(d, g_usersShowResidence);
    ListHeaderRow(g_usersColOrder, kUserListCols, g_usersSort, g_usersPager);
    auto P = [&](int logical) { return ListPos(g_usersColOrder, logical); };

    // Highlight scope: "Όλα" = every column; specific scope = only that one column.
    auto hl = [&](int scope) -> std::string {
        if (g_userSearchField == 0) return std::string(g_userSearch);
        if (g_userSearchField == scope) return std::string(g_userSearch);
        return std::string();
    };

    for (auto& u : rows) {
        ImGui::PushID(u.id);
        ImGui::TableNextRow();
        float colW = 0.0f;

        // TableSetColumnIndex() returns false for a disabled (hidden) column and
        // closes the previous cell -- so writing a cell is just: if it returns
        // true, draw; if false, skip. No separate visibility lookup needed.
        ImGuiID rowId = 0;
        if (ImGui::TableSetColumnIndex(P(UCOL_ID))) { char idb[16]; std::snprintf(idb, sizeof(idb), "%d", u.id); rowId = RowSelectable(idb); }
        // the address / apartment lead to that building with the apartment opened; the name opens the profile
        if (ImGui::TableSetColumnIndex(P(UCOL_ADDR)))    { colW = ImGui::GetContentRegionAvail().x; if (LinkCell(TruncToWidth(u.address, colW), hl(4)) && u.apartment_id > 0) { g_aptGotoId = u.apartment_id; g_tabToApartments = true; } }
        if (ImGui::TableSetColumnIndex(P(UCOL_APT)))     { colW = ImGui::GetContentRegionAvail().x; if (LinkCell(TruncToWidth(u.apartment, colW)) && u.apartment_id > 0) { g_aptGotoId = u.apartment_id; g_tabToApartments = true; } }
        if (ImGui::TableSetColumnIndex(P(UCOL_NAME)))    { colW = ImGui::GetContentRegionAvail().x; if (LinkCell(TruncToWidth(u.full_name, colW), hl(1))) OpenUserProfile(u.id); }
        if (ImGui::TableSetColumnIndex(P(UCOL_PHONE)))   { colW = ImGui::GetContentRegionAvail().x; TextHighlighted(TruncToWidth(u.phone,        colW), hl(3)); }
        if (ImGui::TableSetColumnIndex(P(UCOL_AREA)))    { colW = ImGui::GetContentRegionAvail().x; TextHighlighted(TruncToWidth(u.area,         colW), hl(0)); }
        if (ImGui::TableSetColumnIndex(P(UCOL_TK)))      { colW = ImGui::GetContentRegionAvail().x; TextHighlighted(TruncToWidth(u.postal_code,  colW), hl(0)); }
        if (ImGui::TableSetColumnIndex(P(UCOL_SPECIAL))) { colW = ImGui::GetContentRegionAvail().x; TextHighlighted(TruncToWidth(u.special_code, colW), hl(2)); }

        if (RowDoubleClicked(rowId)) OpenUserProfile(u.id);
        ImGui::PopID();
    }
    ImGui::EndTable();
}


static void DrawUsersScreen() {
    ImGuiStyle& style = ImGui::GetStyle();

    // Fit-aware SameLine: only stay on the current row if the next widget
    // actually fits. When the window narrows, controls wrap naturally to the
    // next line instead of pushing off-screen (this is the fix for the
    // toolbar pushing the view-toggle / actions column out of view).
    auto Fits = [&](float nextW) {
        return ImGui::GetContentRegionAvail().x >= nextW + style.ItemSpacing.x;
    };
    auto SoftSameLine = [&](float nextW) { if (Fits(nextW)) ImGui::SameLine(); };

    // --- Row 1: search + scope combo ---------------------------------------
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##usersearch", "Αναζήτηση ιδιοκτήτη...", g_userSearch, sizeof(g_userSearch));
    SoftSameLine(40 + 160);

    ImGui::TextUnformatted("Ανα:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    ImGui::Combo("##usersearchfield", &g_userSearchField, kUserSearchFields, IM_ARRAYSIZE(kUserSearchFields));
    SoftSameLine(140);

    // --- Φίλτρο Ονόματος -----------------------------------------------
    if (ImGui::Button("Φίλτρο Ονόματος")) ImGui::OpenPopup("##namefilterpopup");
    if (ImGui::BeginPopup("##namefilterpopup")) {
        static const char* kLetters[] = { "Α","Β","Γ","Δ","Ε","Ζ","Η","Θ","Ι","Κ","Λ","Μ",
                                           "Ν","Ξ","Ο","Π","Ρ","Σ","Τ","Υ","Φ","Χ","Ψ","Ω" };
        const int kTotal = 1 + IM_ARRAYSIZE(kLetters);
        const int kPerRow = 13;
        for (int i = 0; i < kTotal; i++) {
            if (i > 0 && (i % kPerRow) != 0) ImGui::SameLine();
            if (i == 0) {
                bool active = g_usersNameLetters.empty();
                if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f,0.55f,0.75f,1.0f));
                if (ImGui::SmallButton("Όλοι")) g_usersNameLetters.clear();
                if (active) ImGui::PopStyleColor();
            } else {
                const char* L = kLetters[i - 1];
                bool active = g_usersNameLetters.count(L) > 0;
                if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f,0.55f,0.75f,1.0f));
                ImGui::PushID(i);
                if (ImGui::SmallButton(L)) {
                    if (active) g_usersNameLetters.erase(L);
                    else        g_usersNameLetters.insert(L);
                }
                ImGui::PopID();
                if (active) ImGui::PopStyleColor();
            }
        }
        ImGui::EndPopup();
    }
    SoftSameLine(180);

    // --- Σειρά: button shows "ΟΝΟΜΑ ↑" etc.; click -> attribute list;
    //     hovering an attribute opens a submenu with ↑ Αύξουσα / ↓ Φθίνουσα.
    if (!g_usersTableView) DrawSortMenu("usersort", kUserSortFields, kUserSortToCol, IM_ARRAYSIZE(kUserSortFields), g_usersSort, g_usersPager);
    SoftSameLine(160);

    // --- + Νέος / Ανανέωση ---------------------------------------------
    if (ImGui::Button("+ Νέος Πελάτης")) OpenAddUser();
    SoftSameLine(110);
    if (ImGui::Button("Ανανέωση")) g_app.reloadUsers();

    // Right edge of Ανανέωση in WINDOW-LOCAL coordinates, taken from
    // GetItemRectMax() (the actual pixel rect of the widget we just drew).
    // This is what lets us re-enter the SAME line right-aligned without the
    // bug from before, where GetCursorPosX() had already been reset to the
    // new line's start by the Button() call and produced a bogus X.
    float lastBtnRight = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;

    // --- Action group: Αναδιάταξη Κολονών | Στοιχεία κατοικίας | Κάρτες/Λίστα.
    //     Right-aligned on the SAME line as + Νέος Πελάτης / Ανανέωση. If the
    //     line doesn't have room for the whole group at the right edge, we
    //     skip the SameLine entirely and let the group flow to the next line
    //     at the left -- so it never overlaps and never clips.
    {
        const ImVec4 onCol(0.24f, 0.55f, 0.75f, 1.0f);
        const float sp = ImGui::GetStyle().ItemSpacing.x;
        const float fpad = ImGui::GetStyle().FramePadding.x * 2.0f;
        auto NaturalW = [&](const char* s) { return ImGui::CalcTextSize(s).x + fpad; };
        float groupW = NaturalW("Στοιχεία κατοικίας")
                     + NaturalW(g_usersTableView ? "Κάρτες" : "Λίστα")
                     + sp;

        float contentRight = ImGui::GetContentRegionMax().x;
        float targetX = contentRight - groupW;
        if (targetX > lastBtnRight + sp) {
            ImGui::SameLine(targetX);   // right-aligned, SAME line as Ανανέωση
        }
        // else: no SameLine -> buttons wrap to the next line naturally.

        if (g_usersShowResidence) ImGui::PushStyleColor(ImGuiCol_Button, onCol);
        if (ImGui::Button("Στοιχεία κατοικίας")) g_usersShowResidence = !g_usersShowResidence;
        if (g_usersShowResidence) ImGui::PopStyleColor();

        ImGui::SameLine();
        if (ImGui::Button(g_usersTableView ? "Κάρτες" : "Λίστα")) g_usersTableView = !g_usersTableView;
    }
    ImGui::Separator();

    // --- Detect filter changes -> reset pager to page 1 ---
    // Any of: text search, scope, letter set. Comparing against the previous
    // frame's snapshot is cheap and catches every transition.
    static int lastScope = -1;
    static std::string lastSearch;
    static std::set<std::string> lastLetters;
    if (lastScope != g_userSearchField || lastSearch != g_userSearch ||
        lastLetters != g_usersNameLetters) {
        g_usersPager.Reset();
        lastScope = g_userSearchField;
        lastSearch = g_userSearch;
        lastLetters = g_usersNameLetters;
    }

    // --- Query ONE page from SQL. No full-table load. ---
    std::vector<std::string> lettersVec(g_usersNameLetters.begin(), g_usersNameLetters.end());
    std::vector<User> page = g_app.conn->pageUsers(
        g_userSearch, g_userSearchField, lettersVec,
        g_usersPager.offset, g_usersPager.pageSize, g_usersSort.keys);
    g_usersPager.total = g_app.conn->countUsers(g_userSearch, g_userSearchField, lettersVec);

    // --- Render: table or cards (only the current page), pager below. ---
    // Reserve a row at the bottom for the pager; the list fills the rest.
    float pagerH = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y * 3.0f;
    if (g_usersTableView) {
        ImGui::BeginChild("usertablewrap", ImVec2(0, -pagerH), false);
        DrawUsersTable(page);
        ImGui::EndChild();
    } else {
        ImGui::BeginChild("usercards", ImVec2(0, -pagerH), false);

        // React-like responsive grid: measure available width, pack as many
        // columns as will fit at >= kMinCardW pixels each, then stretch every
        // card to fill its slot exactly. On a wide window this means more,
        // wider cards; on a narrow one, fewer cards but still filling the row.
        // No hardcoded pixel width anywhere -- resizing the window only changes
        // how many slots there are, never whether cards are cut off.
        const float availW   = ImGui::GetContentRegionAvail().x;
        const float spacing  = ImGui::GetStyle().ItemSpacing.x;
        const float kMinCardW = 240.0f;
        int cols = (int)std::floor((availW + spacing) / (kMinCardW + spacing));
        if (cols < 1) cols = 1;
        const float cardW = (availW - spacing * (cols - 1)) / (float)cols;

        const float cardPad = 10.0f;
        const float innerW  = cardW - cardPad * 2.0f;
        const float lineH   = ImGui::GetTextLineHeightWithSpacing();
        const float btnRowH = ImGui::GetFrameHeight();
        const int   kContentLines = g_usersShowResidence ? 6 : 3;
        const float cardH = kContentLines * lineH + 8.0f + btnRowH + cardPad * 2.0f + 4.0f;

        auto hl = [&](int scope) -> std::string {
            if (g_userSearchField == 0) return std::string(g_userSearch);
            if (g_userSearchField == scope) return std::string(g_userSearch);
            return std::string();
        };

        for (size_t i = 0; i < page.size(); i++) {
            auto& u = page[i];
            ImGui::PushID(u.id);
            BeginCard("card", ImVec2(cardW, cardH), /*noScroll=*/true);

            TextHighlighted(TruncToWidth(u.full_name, innerW), hl(1));
            TextHighlighted(TruncToWidth("Τηλ: " + u.phone, innerW), hl(3));
            if (g_usersShowResidence) {
                TextHighlighted(TruncToWidth(u.address, innerW), hl(4));
                TextHighlighted(TruncToWidth("Διαμ.: " + u.apartment + "   Τ.Κ.: " + u.postal_code, innerW), "");
                TextHighlighted(TruncToWidth("Περιοχή: " + u.area, innerW), hl(0));
            }
            TextHighlighted(TruncToWidth("Κωδ: " + u.special_code, innerW), hl(2));

            // Buttons pinned to the bottom of the card -- resize-safe, they
            // never drift into the fields above no matter how tall the row is.
            ImGui::SetCursorPosY(ImGui::GetWindowHeight() - btnRowH - cardPad);

            const float btnSpacing = ImGui::GetStyle().ItemSpacing.x;
            const float btnW = (ImGui::GetContentRegionAvail().x - btnSpacing) / 2.0f;
            if (ImGui::Button("Προφίλ", ImVec2(btnW, 0))) OpenUserProfile(u.id);
            ImGui::SameLine();
            if (ImGui::Button("+Υπ.",   ImVec2(btnW, 0))) {
                extern int g_prefillOwnerUserId; extern bool g_showServiceDialog; extern bool g_serviceDialogIsEdit;
                extern void ResetServiceDialogFields();
                g_prefillOwnerUserId = u.id;
                ResetServiceDialogFields();
                g_serviceDialogIsEdit = false;
                g_showServiceDialog = true;
            }
            EndCard();
            ImGui::PopID();
            CardWrapNext(cardW);
        }
        ImGui::EndChild();
    }

    // --- Pager (fixed at the bottom of the tab, outside the scrolling list) ---
    DrawPager(g_usersPager, "userspager");

    DrawUserDialog();
}


// ---------------------------------------------------------------------------
// SERVICES SCREEN (single-owner services; joint services live on their own tab)
// ---------------------------------------------------------------------------
bool g_showServiceDialog = false;
static bool g_serviceDialogWasOpen = false;
bool g_serviceDialogIsEdit = false;
int g_prefillOwnerUserId = -1;
static int g_serviceDialogId = -1;
static TextBuf sb_label, sb_notes;
static double sb_price = 0.0, jb_price = 0.0;   // suggested price (€)
static double sb_cost = 0.0;                    // what the service costs us by default (€), 0 = not set
// "Χρήση υπάρχουσας υπηρεσίας" -- per the client ("we won't have a million oil
// services for a million users, but one oil, and opening it will say to which
// users is applied distinctly"), creating a brand-new services row should not
// be the only/default path: joining an existing one (just adding a
// service_users row for this owner) is offered at least as prominently.
static bool sb_joinExisting = false;
static int sb_existingServiceId = -1;
// Package (master-service) creation mode -- see sb_pkgSelected below.
static bool sb_isPackage = false;
// When true (set by a profile drill-in), the owner radio list is hidden -- the
// owner is already fixed by the context that opened the dialog.
static bool sb_lockOwner = false;
static TextBuf sb_pkgLabel;
static std::vector<char> sb_pkgSelected; // parallel to g_app.services
static TextBuf sb_pkgNewServiceLabel;
static double sb_pkgNewServicePrice = 0.0;

void ResetServiceDialogFields() {
    sb_label.set(""); sb_notes.set(""); sb_price = 0.0; sb_cost = 0.0;
    sb_joinExisting = false; sb_existingServiceId = g_app.services.empty() ? -1 : g_app.services[0].id;
    sb_isPackage = false; sb_lockOwner = false;
    sb_pkgLabel.set(""); sb_pkgNewServiceLabel.set("");
    sb_pkgSelected.assign(g_app.services.size(), 0);
}

// Package (master-service) content of the Add Service dialog: a label input,
// a checkbox-list multi-select of EXISTING services (same UX shape as
// DrawJointDialog's roster picker), plus an inline "+ Νέα Υπηρεσία" to create
// a brand-new component service on the spot if the one they want doesn't
// exist yet (mirrors DrawJointDialog's inline "+ Νέος" user creation).
static void DrawPackageServiceForm() {
    ImGui::InputText("Ετικέτα πακέτου", sb_pkgLabel.data, sizeof(sb_pkgLabel.data));
    ImGui::Separator();
    ImGui::Text("Επιλογή υπηρεσιών που περιλαμβάνει το πακέτο:");
    if (sb_pkgSelected.size() != g_app.services.size()) sb_pkgSelected.assign(g_app.services.size(), 0);
    ImGui::BeginChild("pkgsvcpick", ImVec2(0, 140), true);
    for (size_t i = 0; i < g_app.services.size(); i++) {
        bool v = sb_pkgSelected[i] != 0;
        if (ImGui::Checkbox(g_app.services[i].label.c_str(), &v)) sb_pkgSelected[i] = v ? 1 : 0;
    }
    ImGui::EndChild();

    ImGui::TextDisabled("Δεν βρίσκετε την υπηρεσία; Δημιουργήστε μία νέα:");
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##pkgnewsvc", "Νέα υπηρεσία (π.χ. Πετρέλαιο)", sb_pkgNewServiceLabel.data, sizeof(sb_pkgNewServiceLabel.data));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    InputMoney("€##pkgnewprice", &sb_pkgNewServicePrice);
    ImGui::SameLine();
    if (ImGui::Button("+ Νέα Υπηρεσία") && sb_pkgNewServicePrice > 0.0) {
        std::string lbl = sb_pkgNewServiceLabel.str();
        if (!lbl.empty()) {
            Service ns; ns.label = lbl; ns.suggested_price = sb_pkgNewServicePrice;
            // A brand-new catalog service still needs at least one owner to exist
            // (service_users has no "ownerless" concept) -- default to the first
            // user; owners can be adjusted afterward like any other service.
            int newId = g_app.conn->addServiceGetId(-1, ns);   // shown in the package's list right away; the package itself is confirmed on save
            g_app.reloadServices();
            sb_pkgSelected.assign(g_app.services.size(), 0);
            if (newId >= 0) {
                for (size_t i = 0; i < g_app.services.size(); i++)
                    if (g_app.services[i].id == newId) sb_pkgSelected[i] = 1;
            }
            sb_pkgNewServiceLabel.set(""); sb_pkgNewServicePrice = 0.0;
        }
    }
}

static void DrawServiceDialog() {
    if (g_showServiceDialog && !g_serviceDialogWasOpen) {
        ImGui::OpenPopup("Στοιχεία Υπηρεσίας");
    }
    g_serviceDialogWasOpen = g_showServiceDialog;
    if (!g_showServiceDialog) return;
    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Appearing);
    if (BeginGuardedModal("Στοιχεία Υπηρεσίας", g_showServiceDialog, ImGuiWindowFlags_AlwaysAutoResize, []() {
        std::vector<FieldDiff> d; if (!g_serviceDialogIsEdit) return d;
        Service o = g_app.conn->getServiceById(g_serviceDialogId);
        DiffAdd(d, "Ετικέτα", o.label, sb_label.str()); DiffAdd(d, "Σημειώσεις", o.extra_notes, sb_notes.str());
        if (std::fabs(o.suggested_price - sb_price) > 0.004) {
            char a[32], b[32]; std::snprintf(a, sizeof(a), "%s", Eur(o.suggested_price).c_str()); std::snprintf(b, sizeof(b), "%s", Eur(sb_price).c_str());
            d.push_back({"Τιμή", a, b});
        }
        if (std::fabs(o.cost_price - sb_cost) > 0.004) d.push_back({"Κόστος", Eur(o.cost_price), Eur(sb_cost)});
        return d; })) {
        if (!g_serviceDialogIsEdit) {
            ImGui::Checkbox("Πακέτο (πολλαπλές υπηρεσίες)", &sb_isPackage);
            ImGui::Separator();
        }
        if (sb_isPackage && !g_serviceDialogIsEdit) {
            DrawPackageServiceForm();
            ImGui::Spacing();
            int selCount = 0; for (char c : sb_pkgSelected) if (c) selCount++;
            if (selCount == 0) ImGui::TextColored(ImVec4(1,0.5f,0.3f,1), "Επιλέξτε τουλάχιστον 1 υπηρεσία.");
            ImGui::BeginDisabled(selCount == 0 || sb_pkgLabel.str().empty());
            if (ImGui::Button("Δημιουργία Πακέτου", ImVec2(180, 0))) {
                std::string label = sb_pkgLabel.str();
                std::vector<int> serviceIds;
                for (size_t i = 0; i < sb_pkgSelected.size(); i++) if (sb_pkgSelected[i]) serviceIds.push_back(g_app.services[i].id);
                RequestConfirm("Νέο πακέτο υπηρεσιών", { {"Ετικέτα", "-", label}, {"Υπηρεσίες", "-", std::to_string(serviceIds.size())} },
                    [label, serviceIds]() {
                        int mid = g_app.conn->addMasterService(label);
                        if (mid >= 0) for (int sid : serviceIds) g_app.conn->addMasterServiceComponent(mid, sid);
                        g_app.reloadServices();
                        g_app.reloadJoint();
                    });
                g_showServiceDialog = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Ακύρωση", ImVec2(150, 0))) { g_showServiceDialog = false; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
            return;
        }

        // A service is an independent catalog entry: only its own attributes here.
        ImGui::InputText("Ετικέτα", sb_label.data, sizeof(sb_label.data));
        ImGui::InputTextMultiline("Σημειώσεις", sb_notes.data, sizeof(sb_notes.data), ImVec2(0, 60));
        InputMoney("Προτεινόμενη τιμή (€)", &sb_price);
        InputMoney("Κόστος για εμάς (€)", &sb_cost);
        ImGui::TextDisabled("Το κόστος είναι προεπιλογή: κάθε χρέωση ξεκινά με αυτό και μπορείτε να το αλλάξετε.");
        ImGui::Spacing();
        bool needPrice = sb_price <= 0.0;
        if (needPrice) ImGui::TextColored(ImVec4(1,0.5f,0.3f,1), "Η τιμή είναι υποχρεωτική (> 0).");
        ImGui::BeginDisabled(needPrice || sb_label.str().empty());
        if (ImGui::Button("Αποθήκευση", ImVec2(150, 0))) {
            if (g_serviceDialogIsEdit) {
                Service ns; ns.label = sb_label.str(); ns.extra_notes = sb_notes.str(); ns.suggested_price = sb_price; ns.cost_price = sb_cost;
                Service old = g_app.conn->getServiceById(g_serviceDialogId);
                std::vector<FieldDiff> diffs = { {"Ετικέτα", old.label, ns.label}, {"Τιμή", Eur(old.suggested_price), Eur(ns.suggested_price)}, {"Κόστος", Eur(old.cost_price), Eur(ns.cost_price)} };
                int id = g_serviceDialogId;
                RequestConfirm("Επεξεργασία υπηρεσίας", diffs, [id, ns]() {
                    g_app.conn->editService(id, ns);
                    g_app.reloadServices();
                });
            } else {
                Service ns; ns.label = sb_label.str(); ns.extra_notes = sb_notes.str(); ns.suggested_price = sb_price; ns.cost_price = sb_cost;
                std::vector<FieldDiff> diffs = { {"Ετικέτα", "-", ns.label}, {"Τιμή", "-", Eur(ns.suggested_price)}, {"Κόστος", "-", Eur(ns.cost_price)} };
                RequestConfirm("Νέα υπηρεσία", diffs, [ns]() {
                    g_app.conn->addService(-1, ns);   // services are independent: no owner
                    g_app.reloadServices();
                    g_app.reloadJoint();
                });
            }
            g_showServiceDialog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Ακύρωση", ImVec2(150, 0))) { g_showServiceDialog = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------
// Table ("list") views for Υπηρεσίες and Πληρωμές. Columns are user-reorderable
// (drag a header) and resizable; ImGui remembers the order in imgui.ini.
// ---------------------------------------------------------------------------
static bool g_servicesTableView = true;    // false = cards, true = list (default)
static bool g_paymentsTableView = true;

static void OpenChargeService(int serviceId);

static const ListColDef kServiceListCols[6] = {
    { "ΚΩΔ",        ImGuiTableColumnFlags_WidthFixed,   110.0f, true  },
    { "ΥΠΗΡΕΣΙΑ",   ImGuiTableColumnFlags_WidthStretch, 2.4f,   true  },
    { "ΤΙΜΗ",       ImGuiTableColumnFlags_WidthFixed,   130.0f, true  },
    { "ΣΗΜΕΙΩΣΕΙΣ", ImGuiTableColumnFlags_WidthStretch, 3.0f,   true  },
    { "ΚΟΣΤΟΣ",     ImGuiTableColumnFlags_WidthFixed,   130.0f, true  },
    { "ΕΝΕΡΓΕΙΕΣ",  ImGuiTableColumnFlags_WidthFixed,   420.0f, false },
};
static std::vector<int> g_servicesColOrder = { 0, 1, 2, 4, 3, 5 };
static const char* const kServiceSortNames[] = { "ΚΩΔ", "ΥΠΗΡΕΣΙΑ", "ΤΙΜΗ", "ΣΗΜΕΙΩΣΕΙΣ" };
static const int kServiceSortCols[] = { 0, 1, 2, 3 };

static void DrawServicesScreen() {
    // Toolbar: + Νέο / Ανανέωση on the left; Κάρτες-Λίστα right-aligned on the
    // same row. Uses GetItemRectMax to find the actual right edge of Ανανέωση,
    // then SameLine(targetX) to re-enter the row with the toggle's left edge
    // at contentRight - toggleW. Falls back to a new line if there's no room.
    if (ImGui::Button("+ Νέα Υπηρεσία")) {
        g_serviceDialogIsEdit = false;
        ResetServiceDialogFields();
        g_showServiceDialog = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Διαχείριση Κοινοχρήστων")) OpenGenericJointCharge(-1);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Γενική χρέωση σε πολυκατοικία για ό,τι δεν είναι υπηρεσία:\nπεριγραφή, σύνολο, κόστος, και τα ποσά κάθε διαμερίσματος.");
    ImGui::SameLine();
    if (ImGui::Button("Ανανέωση")) { g_app.reloadServices(); g_app.reloadJoint(); }
    if (!g_servicesTableView) { ImGui::SameLine(); DrawSortMenu("svcsort", kServiceSortNames, kServiceSortCols, 4, g_servicesSort, g_servicesPager); }

    {
        float lastRight = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
        float contentRight = ImGui::GetContentRegionMax().x;
        const float toggleW = 90.0f;
        float targetX = contentRight - toggleW;
        if (targetX > lastRight + ImGui::GetStyle().ItemSpacing.x) {
            ImGui::SameLine(targetX);
        }
        if (ImGui::Button(g_servicesTableView ? "Κάρτες" : "Λίστα", ImVec2(toggleW, 0)))
            g_servicesTableView = !g_servicesTableView;
    }
    // smart search: name or notes, accents/capitals ignored; queried when typing pauses (debounce)
    static char svcQ[128] = ""; static std::string svcQuery; static double svcTypedAt = 0;
    ImGui::SetNextItemWidth(360);
    if (ImGui::InputTextWithHint("##svcq", "Αναζήτηση υπηρεσίας (όνομα ή σημειώσεις)...", svcQ, sizeof(svcQ))) svcTypedAt = ImGui::GetTime();
    if (svcQuery != svcQ && ImGui::GetTime() - svcTypedAt > 0.25) { svcQuery = svcQ; g_servicesPager.Reset(); }
    ImGui::SameLine();
    if (svcQ[0] && ImGui::SmallButton("×")) { svcQ[0] = 0; svcQuery.clear(); g_servicesPager.Reset(); }
    ImGui::SameLine();
    ImGui::TextDisabled("Οι κοινόχρηστες υπηρεσίες είναι στα «Κοινόχρηστα».");
    ImGui::Separator();

    // --- Query one page from SQL. Joint services are excluded at SQL level. ---
    static Cached<std::pair<std::vector<Service>, int>> svcCache;
    std::string skey = svcQuery + "|" + std::to_string(g_servicesPager.offset) + "|" + std::to_string(g_servicesPager.pageSize) + "|";
    for (auto& k : g_servicesSort.keys) skey += std::to_string(k.first) + (k.second ? "a" : "d");
    const auto& svcPage = svcCache.get(skey, [&] {
        return std::make_pair(g_app.conn->pageServices(svcQuery, g_servicesPager.offset, g_servicesPager.pageSize, g_servicesSort.keys),
                              g_app.conn->countServices(svcQuery)); });
    std::vector<Service> page = svcPage.first;
    g_servicesPager.total = svcPage.second;
    if (page.empty()) ImGui::TextDisabled(svcQuery.empty() ? "Καμία υπηρεσία ακόμα." : "Καμία υπηρεσία με αυτή την αναζήτηση.");

    // Joint-service id set is still needed so the TABLE view (which draws its
    // own headers) can be told which services to skip -- but with paging, the
    // SQL already filtered them out; the set is passed empty.
    std::set<int> jointIds;

    float pagerH = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y * 3.0f;
    ImGui::BeginChild("svccards", ImVec2(0, -pagerH), false);
    if (g_servicesTableView) {
        // The table-drawing function iterates g_app.services internally. It has
        // been left as-is for compatibility; with the pager in play, we instead
        // render the page here directly via a tiny inline table.
        if (ImGui::BeginTable("servicestable", 6,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                ImGuiTableFlags_Resizable | ImGuiTableFlags_Hideable)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ListSetupColumns(g_servicesColOrder, kServiceListCols);
            ListHeaderRow(g_servicesColOrder, kServiceListCols, g_servicesSort, g_servicesPager);
            auto P = [&](int logical) { return ListPos(g_servicesColOrder, logical); };
            for (auto& s : page) {
                ImGui::PushID(s.id);
                ImGui::TableNextRow();
                ImGuiID rowId = 0;
                { char idb[16]; std::snprintf(idb, sizeof(idb), "%d", s.id); ImGui::TableSetColumnIndex(P(0)); rowId = RowSelectable(idb); }
                ImGui::TableSetColumnIndex(P(1)); if (LinkCell(s.label)) OpenServiceDetail(s.id);
                ImGui::TableSetColumnIndex(P(2)); ImGui::Text("%s", Eur(s.suggested_price).c_str());
                ImGui::TableSetColumnIndex(P(4)); if (s.cost_price > 0.005) ImGui::TextColored(kColOut, "%s", Eur(s.cost_price).c_str()); else ImGui::TextDisabled("—");
                ImGui::TableSetColumnIndex(P(3)); ImGui::TextUnformatted(TruncToWidth(s.extra_notes, ImGui::GetContentRegionAvail().x).c_str());
                ImGui::TableSetColumnIndex(P(5));
                if (ImGui::SmallButton("Χρέωση")) OpenChargeService(s.id);
                ImGui::SameLine();
                if (ImGui::SmallButton("Ως κοινόχρηστο")) OpenJointChargeFor(2, s.id, s.label);
                if (RowDoubleClicked(rowId)) OpenServiceDetail(s.id);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
    } else {
        const float cardW = 260, cardH = 150;
        for (auto& s : page) {
            ImGui::PushID(s.id);
            BeginCard("card", ImVec2(cardW, cardH));
            if (LinkCell(s.label)) OpenServiceDetail(s.id);
            if (s.suggested_price > 0.0) ImGui::TextColored(kColIn, "Προτεινόμενη τιμή: %s", Eur(s.suggested_price).c_str());
            if (s.cost_price > 0.005) ImGui::TextDisabled("Κόστος για εμάς: %s", Eur(s.cost_price).c_str());
            if (!s.extra_notes.empty()) ImGui::TextWrapped("%s", s.extra_notes.c_str());
            ImGui::Spacing();
            if (ImGui::SmallButton("Χρέωση")) OpenChargeService(s.id);
            ImGui::SameLine();
            if (ImGui::SmallButton("Κοινόχρ.")) OpenJointChargeFor(2, s.id, s.label);
            EndCard();
            ImGui::PopID();
            CardWrapNext(cardW);
        }
        ImGui::EndChild();
    }

    DrawPager(g_servicesPager, "servicespager");
}

// ---------------------------------------------------------------------------
// ΚΟΙΝΟΧΡΗΣΤΑ ("Κοινόχρηστες Υπηρεσίες")
// A joint charge splits ONE cost over a collection of apartments (normally a building's): every current
// resident gets their own charge (then their own payment and receipt as usual); excluded apartments are
// recorded with the reason.
// ---------------------------------------------------------------------------
static void OpenRecordPayment(int userId);
static void OpenJointView(int id);

// ---- catalog dialog (new / edit joint service) ----
static bool g_showJointSvc = false, g_jointSvcWasOpen = false;
static int g_jointSvcId = -1;                 // -1 = new
static TextBuf js_label, js_notes;
static bool js_active = true;

static void OpenJointServiceDialog(const JointService* j) {
    g_showJointSvc = true;
    g_jointSvcId = j ? j->id : -1;
    js_label.set(j ? j->service.label : ""); js_notes.set(j ? j->service.extra_notes : "");
    js_active = j ? j->active : true;
}

static void DrawJointServiceDialog() {
    const char* title = "Κοινόχρηστη υπηρεσία###jointsvc";
    if (g_showJointSvc && !g_jointSvcWasOpen) ImGui::OpenPopup(title);
    g_jointSvcWasOpen = g_showJointSvc;
    if (!g_showJointSvc) return;
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Appearing);
    if (BeginGuardedModal(title, g_showJointSvc, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Όνομα", js_label.data, sizeof(js_label.data));
        ImGui::InputTextMultiline("Σημειώσεις", js_notes.data, sizeof(js_notes.data), ImVec2(0, 50));
        ImGui::PushTextWrapPos(480); ImGui::TextDisabled("Μια συντόμευση για κάτι που χρεώνετε συχνά στις πολυκατοικίες. Τα ποσά και τα μερίδια τα ορίζετε κάθε φορά που χρεώνετε."); ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::BeginDisabled(js_label.str().empty());
        if (ImGui::Button("Αποθήκευση", ImVec2(150, 0))) {
            Service s; s.label = js_label.str(); s.extra_notes = js_notes.str();
            std::string kind = "equal";
            int id = g_jointSvcId; bool active = true;   // joint services stay in the catalog for good
            RequestConfirm(id < 0 ? "Νέα κοινόχρηστη υπηρεσία" : "Αλλαγή κοινόχρηστης υπηρεσίας",
                           { {"Όνομα", "-", s.label} }, [id, s, kind, active]() {
                if (id < 0) g_app.conn->addJointService(s, kind);
                else g_app.conn->editJointService(id, s, kind, active);
                g_app.reloadServices(); g_app.reloadGen++;
            });
            g_showJointSvc = false; ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Ακύρωση", ImVec2(150, 0))) { g_showJointSvc = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

// ---- receipts (shared by every charge dialog) ----
struct ReceiptForm { bool issue = true; int method = 0; TextBuf paper, bank, rf; };
static const char* kReceiptMethodCodes[] = { "cash", "collector", "bank", "ebanking", "card", "iris", "other" };
static const char* kReceiptMethodNames[] = { "Μετρητά", "Εισπράκτορας", "Κατάθεση σε τράπεζα", "e-banking", "Κάρτα", "IRIS", "Άλλο" };
static std::string ReceiptMethodName(const std::string& code) {
    for (int i = 0; i < IM_ARRAYSIZE(kReceiptMethodCodes); i++) if (code == kReceiptMethodCodes[i]) return kReceiptMethodNames[i];
    return code;
}

// Draws the section; returns false while something typed is invalid (Save stays blocked).
static bool DrawReceiptForm(const char* id, ReceiptForm& f, double total, int payments, bool perPayment = false) {
    ImGui::PushID(id);
    ImGui::Spacing();
    SectionTitle("Απόδειξη είσπραξης");
    ImGui::Checkbox("Έκδοση απόδειξης", &f.issue);
    bool ok = true;
    if (f.issue) {
        static Cached<int> next;
        int n = next.get("A", [] { return g_app.conn->nextReceiptNumber("A"); });
        ImGui::SameLine();
        ImGui::TextDisabled("θα εκδοθεί Α-%06d · %s%s", n, Eur(total).c_str(),
                            payments > 1 ? (perPayment ? ("  (" + std::to_string(payments) + " πληρωμές, κάθε μία με δική της απόδειξη)").c_str() : ("  (" + std::to_string(payments) + " πληρωμές, μία απόδειξη)").c_str()) : "");
        if (ToCents(total) <= 0) { ImGui::TextColored(kInvalidMoneyCol, "Η απόδειξη χρειάζεται πληρωμένο ποσό μεγαλύτερο από 0."); ok = false; }
        ImGui::SetNextItemWidth(220);
        ImGui::Combo("Τρόπος πληρωμής", &f.method, kReceiptMethodNames, IM_ARRAYSIZE(kReceiptMethodNames));
        bool electronic = f.method >= 2 && f.method <= 5;
        ImGui::SetNextItemWidth(220);
        ImGui::InputTextWithHint("Αρ. χειρόγραφης απόδειξης", "αν κόψατε από μπλοκ", f.paper.data, sizeof(f.paper.data));
        if (electronic) {
            ImGui::SetNextItemWidth(220);
            ImGui::InputTextWithHint("Αναφορά συναλλαγής", "αρ. συναλλαγής", f.bank.data, sizeof(f.bank.data));
        }
        ImGui::SetNextItemWidth(220);
        ImGui::InputTextWithHint("Κωδικός RF", "αυτόματο αν μείνει κενό", f.rf.data, sizeof(f.rf.data));
        if (!f.rf.str().empty() && !IsValidRfCode(f.rf.str())) {
            ImGui::SameLine(); ImGui::TextColored(kInvalidMoneyCol, "μη έγκυρος κωδικός RF");
            ok = false;
        }
    } else {
        ImGui::TextDisabled("Χωρίς απόδειξη τώρα. Μπορεί να εκδοθεί αργότερα από την πληρωμή.");
    }
    ImGui::PopID();
    return ok;
}

static Receipt ReceiptFromForm(const ReceiptForm& f, int userId) {
    Receipt r;
    r.user_id = userId;
    r.method = kReceiptMethodCodes[f.method];
    r.paper_ref = f.paper.str();
    r.bank_ref = (f.method >= 2 && f.method <= 5) ? f.bank.str() : "";
    r.rf_code = f.rf.str();
    return r;
}

// One line for confirm dialogs.
static std::string ReceiptSummary(const ReceiptForm& f) {
    if (!f.issue) return "όχι";
    char num[24]; std::snprintf(num, sizeof(num), "Α-%06d", g_app.conn->nextReceiptNumber("A"));
    std::string s = std::string(num) + " · " + kReceiptMethodNames[f.method];
    if (!f.paper.str().empty()) s += " · χειρόγρ. " + f.paper.str();
    if (!f.bank.str().empty() && f.method >= 2 && f.method <= 5) s += " · αναφ. " + f.bank.str();
    return s;
}

// ---- charge dialog ----
struct JointChargeState {
    bool show = false, wasOpen = false;
    int sourceType = 0;                       // 0 joint service, 1 package, 2 service
    int sourceId = -1;
    int buildingId = -1;
    int kind = 0;
    std::string date;
    TextBuf label, note, svcFilter;
    JointDraft d;
    bool manual = false;                      // the person typed a share: stop re-proposing
    long long proposedFor = -1;               // hash of the inputs the current proposal was made for
    int loadedBuilding = -1;
    bool confirming = false;                  // second step: summary + final button
    double cost = 0.0;                        // what it costs us
    double costPaid = 0.0; bool costPaidManual = false;   // how much of it we already paid (the rest we owe)
    int partner = -1;                         // the business partner the cost was paid to
    long long costFor = -1;                   // the source the cost was last defaulted for
    std::vector<Service> comps; std::vector<double> compCost, compPaid; std::vector<int> compPartner; std::vector<char> compPaidManual;    // a package: what each of its services costs us (the sum is the cost)
    ReceiptForm rec;                          // the receipt(s) for what the residents pay now: one per paying resident
    bool sourceFixed = false;                 // opened from a service / package / joint shortcut: what is charged is already known
};
static JointChargeState g_jc;
static void OpenJointCharge(int buildingId, int sourceType, int sourceId) {
    g_jc = JointChargeState{};
    g_jc.show = true; g_jc.buildingId = buildingId; g_jc.sourceType = sourceType; g_jc.sourceId = sourceId;
    g_jc.date = CurrentDateString();
}

static void OpenJointChargeFor(int sourceType, int sourceId, const std::string& label) {
    OpenJointCharge(-1, sourceType, sourceId); g_jc.label.set(label); g_jc.sourceFixed = sourceId > 0;
}

// «Διαχείριση Κοινοχρήστων»: one generic entry for any building need we have no service for. It lives in the joint
// catalog, so it never shows among the services; the charge gets its own description, total, cost and shares.
static const char* kGenericJointLabel = "Διαχείριση Κοινοχρήστων";
static int GenericJointServiceId() {
    static int sid = -1;
    if (sid > 0) return sid;
    for (auto& j : g_app.conn->getJointCatalog(true)) if (j.service.label == kGenericJointLabel) { sid = j.service.id; return sid; }
    Service s; s.label = kGenericJointLabel;
    s.extra_notes = "Γενική χρέωση πολυκατοικίας: ό,τι δεν καλύπτεται από υπηρεσία. Γράψτε τι είναι στην περιγραφή.";
    int jid = g_app.conn->addJointService(s, "equal");
    if (jid > 0) sid = g_app.conn->getJointServiceById(jid).service.id;
    return sid;
}
static void OpenGenericJointCharge(int buildingId) {
    int sid = GenericJointServiceId();
    if (sid <= 0) return;
    OpenJointChargeFor(2, sid, kGenericJointLabel);
    g_jc.buildingId = buildingId;
}

// Every apartment of the building, one row per current resident; vacant apartments are excluded rows.
static void JointLoadRows() {
    g_jc.d.rows.clear();
    g_jc.loadedBuilding = g_jc.buildingId;
    g_jc.manual = false; g_jc.proposedFor = -1;
    if (g_jc.buildingId <= 0) return;
    auto residents = g_app.conn->getBuildingResidents(g_jc.buildingId);   // only this building's people, never every client
    for (auto& a : g_app.conn->getApartmentsForBuilding(g_jc.buildingId)) {
        bool any = false;
        for (auto& u : residents) {
            if (u.apartment_id != a.id) continue;
            JointRow r; r.apartment_id = a.id; r.user_id = u.id; r.apt_label = a.label; r.user_name = u.full_name;
            g_jc.d.rows.push_back(r);
            any = true;
        }
        if (!any) {
            JointRow r; r.apartment_id = a.id; r.apt_label = a.label; r.included = false;
            std::snprintf(r.reason, sizeof(r.reason), "%s", "Κενό διαμέρισμα");
            g_jc.d.rows.push_back(r);
        }
    }
}

// What the charge splits: one line, or a package's components (the total spread by their catalog prices).
static std::vector<JointLine> JointLines(std::string* sourceLabel) {
    std::vector<JointLine> lines;
    long long total = ToCents(g_jc.d.total);
    if (g_jc.sourceType == 1) {
        auto comps = g_app.conn->getMasterServiceComponents(g_jc.sourceId);
        std::vector<double> w; for (auto& c : comps) w.push_back(c.suggested_price);
        auto parts = SplitCents(total, w);
        for (size_t i = 0; i < comps.size(); i++) {
            JointLine ln; ln.service_id = comps[i].id; ln.total = FromCents(parts[i]);
            if (g_jc.compCost.size() == comps.size()) { ln.cost = std::max(0.0, g_jc.compCost[i]); ln.partner = g_jc.compPartner[i]; ln.costPaid = std::min(std::max(0.0, g_jc.compPaid[i]), ln.cost); }      // costs per service
            lines.push_back(ln);
        }
        if (sourceLabel) *sourceLabel = g_app.conn->getPackageLabel(g_jc.sourceId);
    } else if (g_jc.sourceType == 0) {
        JointService j = g_app.conn->getJointServiceById(g_jc.sourceId);
        if (j.id > 0) {
            lines.push_back({j.service.id, g_jc.d.total});
            if (sourceLabel) *sourceLabel = j.service.label;
        }
    } else {
        lines.push_back({g_jc.sourceId, g_jc.d.total});
        if (sourceLabel) *sourceLabel = g_app.serviceLabel(g_jc.sourceId);
    }
    return lines;
}

// What it costs us by default: the service's own cost, or the sum over a package's services.
static double JointDefaultCost() {
    if (g_jc.sourceId <= 0) return 0.0;
    if (g_jc.sourceType == 1) {
        double c = 0.0;
        for (auto& s : g_app.conn->getMasterServiceComponents(g_jc.sourceId)) c = FromCents(ToCents(c) + ToCents(g_app.conn->getServiceCost(s.id)));
        return c;
    }
    return g_app.conn->getServiceCost(g_jc.sourceId);
}

static void DrawJointChargeDialog() {
    const char* title = "Χρέωση πολυκατοικίας###jointcharge";
    if (g_jc.show && !g_jc.wasOpen) ImGui::OpenPopup(title);
    g_jc.wasOpen = g_jc.show;
    if (!g_jc.show) return;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowSize(ImVec2(std::min(1000.0f, vp->WorkSize.x * 0.95f), vp->WorkSize.y * 0.92f), ImGuiCond_Appearing);
    if (!BeginGuardedModal(title, g_jc.show, 0)) return;
    if (g_jc.sourceType == 0) g_jc.sourceType = 2;          // the old joint-service catalog is gone: plain services and packages

    // 1. what
    if (g_jc.sourceFixed) {       // we know what is charged: no question
        ImGui::TextColored(ImVec4(0.70f,0.85f,1.0f,1.0f), "Χρεώνεται: %s", g_jc.sourceType == 1 ? ("πακέτο «" + g_app.conn->getPackageLabel(g_jc.sourceId) + "»").c_str() : ("υπηρεσία «" + g_app.serviceLabel(g_jc.sourceId) + "»").c_str());
    } else {
    SectionTitle("Τι χρεώνεται");
    for (int k = 0; k < 2; k++) {
        int i = k == 0 ? 2 : 1;
        if (k) ImGui::SameLine();
        if (ImGui::RadioButton(i == 2 ? "Υπηρεσία" : "Πακέτο υπηρεσιών", g_jc.sourceType == i)) { g_jc.sourceType = i; g_jc.sourceId = -1; }
    }
    if (g_jc.sourceType == 1) {
        if (PickPackage("jcpkg", g_jc.sourceId, 420)) g_jc.label.set(g_app.conn->getPackageLabel(g_jc.sourceId));
    } else {
        if (PickService("jcsvc", g_jc.sourceId, 420)) g_jc.label.set(g_app.serviceLabel(g_jc.sourceId));
    }
    }

    // 2. where
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.70f,0.85f,1.0f,1.0f), "%s", g_jc.sourceFixed ? "Σε ποια πολυκατοικία" : "2. Σε ποια πολυκατοικία");
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Πολυκατοικία:"); ImGui::SameLine();
    PickBuilding("jcbld", g_jc.buildingId, 420);

    // 3. amounts
    ImGui::Spacing();
    SectionTitle("Τι χρεώνουμε");
    long long srcKey = (long long)g_jc.sourceType * 1000003LL + g_jc.sourceId;
    if (g_jc.costFor != srcKey) { g_jc.costFor = srcKey; g_jc.cost = JointDefaultCost(); g_jc.costPaidManual = false;
        g_jc.comps.clear(); g_jc.compCost.clear(); g_jc.compPaid.clear(); g_jc.compPartner.clear(); g_jc.compPaidManual.clear();
        if (g_jc.sourceType == 1 && g_jc.sourceId > 0) { g_jc.comps = g_app.conn->getMasterServiceComponents(g_jc.sourceId); for (auto& s : g_jc.comps) { g_jc.compCost.push_back(g_app.conn->getServiceCost(s.id)); g_jc.compPaid.push_back(g_jc.compCost.back()); g_jc.compPartner.push_back(-1); g_jc.compPaidManual.push_back(0); } } }
    ImGui::SetNextItemWidth(160); InputMoney("Σύνολο που χρεώνουμε (€)", &g_jc.d.total);
    SectionTitle("Τι μας κοστίζει και πώς πληρώνουμε τον συνεργάτη");
    if (g_jc.comps.empty()) { ImGui::SetNextItemWidth(160); InputMoney("Κόστος για εμάς (€)", &g_jc.cost); }
    else {      // a package: the cost of each of its services, summed
        ImGui::TextDisabled("Κόστος για εμάς ανά υπηρεσία του πακέτου (προτεινόμενο· αλλάζει):");
        double sumc = 0.0, sump = 0.0;
        for (size_t i = 0; i < g_jc.comps.size(); i++) {
            ImGui::PushID((int)i); ImGui::SetNextItemWidth(110); InputMoney("##ccost", &g_jc.compCost[i]); ImGui::SameLine(); ImGui::TextUnformatted(g_jc.comps[i].label.c_str());
            double cc = std::max(0.0, g_jc.compCost[i]);
            if (!g_jc.compPaidManual[i]) g_jc.compPaid[i] = cc;
            ImGui::Indent(24);
            ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("κόστος προς"); ImGui::SameLine(); PickPartner("jcrowpart", g_jc.compPartner[i], 260);
            ImGui::SameLine(0, 10); ImGui::SetNextItemWidth(110); if (InputMoney("##cpaid", &g_jc.compPaid[i])) g_jc.compPaidManual[i] = 1;
            g_jc.compPaid[i] = std::min(std::max(0.0, g_jc.compPaid[i]), cc);
            ImGui::SameLine(); ImGui::TextDisabled("πληρωμή τώρα"); if (ToCents(cc) > ToCents(g_jc.compPaid[i])) { ImGui::SameLine(); ImGui::TextColored(kColWeOwe, "οφειλή %s", Eur(MoneySub(cc, g_jc.compPaid[i])).c_str()); }
            ImGui::Unindent(24);
            sumc = FromCents(ToCents(sumc) + ToCents(cc)); sump = FromCents(ToCents(sump) + ToCents(g_jc.compPaid[i]));
            ImGui::PopID();
        }
        g_jc.costPaid = sump; g_jc.costPaidManual = true;        g_jc.cost = sumc; ImGui::Text("Σύνολο κόστους: %s", Eur(sumc).c_str());
    }
    ImGui::SameLine(); ImGui::TextDisabled("(προτεινόμενο· αλλάζει)");
    if (g_jc.comps.empty()) {
    if (!g_jc.costPaidManual) g_jc.costPaid = g_jc.cost;                         // by default we paid it all
    g_jc.costPaid = std::min(std::max(0.0, g_jc.costPaid), std::max(0.0, g_jc.cost));
    ImGui::SetNextItemWidth(160); if (InputMoney("Πληρωμές από το κόστος (€)", &g_jc.costPaid)) g_jc.costPaidManual = true;
    }
    { double we = MoneySub(std::max(0.0, g_jc.cost), g_jc.costPaid); ImGui::SameLine(); if (we > 0.005) ImGui::TextColored(kColWeOwe, "Οφειλόμενα: %s", Eur(we).c_str()); else ImGui::TextDisabled("καμία οφειλή"); }
    if (g_jc.comps.empty()) { ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Το κόστος πληρώθηκε σε:"); ImGui::SameLine(); PickPartner("jcpart", g_jc.partner, 300); }
    if (g_jc.cost > 0.005 && g_jc.d.total > 0.005)
        ImGui::TextColored(g_jc.d.total - g_jc.cost >= 0 ? kColIn : kColOut, "Κέρδος: %s", Eur(MoneySub(g_jc.d.total, g_jc.cost)).c_str());
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Ημερομηνία:"); ImGui::SameLine(); DatePicker("jcdate", g_jc.date, ImVec2(0, 1));
    ImGui::SameLine(); ImGui::SetNextItemWidth(320); ImGui::InputText("Περιγραφή", g_jc.label.data, sizeof(g_jc.label.data));
    ImGui::SetNextItemWidth(560); ImGui::InputText("Σημείωση", g_jc.note.data, sizeof(g_jc.note.data));
    if (g_jc.loadedBuilding != g_jc.buildingId) JointLoadRows();

    // no automatic split: the shares are typed by hand; an excluded apartment keeps 0
    for (auto& r : g_jc.d.rows) if (!r.included) r.amount = 0;

    // 4. shares: each apartment's amount, free; they must add up to the total
    int nInc = 0; long long sum = 0;
    for (auto& r : g_jc.d.rows) if (r.included) { nInc++; sum += ToCents(r.amount); }
    ImGui::Spacing();
    SectionTitle(("Πώς πληρώνουν οι ένοικοι · μερίδια (" + std::to_string(nInc) + " χρεώνονται)").c_str());
    ImGui::TextDisabled("Βάλτε το ποσό κάθε διαμερίσματος (ξεκλειδώστε το «Χρέωση» για να εξαιρέσετε κάποιο)· το σύνολο των μεριδίων πρέπει να ισούται με το σύνολο της χρέωσης.");
    { long long left = ToCents(g_jc.d.total) - sum; ImGui::TextColored(left == 0 ? kColIn : kColOwed, left == 0 ? "Συμπληρώθηκε" : "Υπόλοιπο: %s", Eur(FromCents(left)).c_str()); }
    float tableH = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 2.6f;
    if (ImGui::BeginTable("jcrows", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, std::max(120.0f, tableH)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Χρέωση", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Διαμέρισμα", ImGuiTableColumnFlags_WidthStretch, 0.7f);
        ImGui::TableSetupColumn("Ένοικος", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Μερίδιο (€) / αιτία εξαίρεσης", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Εισπράξεις ήδη (€)", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < g_jc.d.rows.size(); i++) {
            auto& r = g_jc.d.rows[i];
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(r.user_id <= 0);
            ImGui::Checkbox("##inc", &r.included);
            ImGui::EndDisabled();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.apt_label.c_str());
            ImGui::TableNextColumn();
            if (r.user_id > 0) ImGui::TextUnformatted(r.user_name.c_str()); else ImGui::TextDisabled("— κενό —");
            ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1);
            if (r.included) {
                if (InputMoney("##amt", &r.amount)) g_jc.manual = true;
            } else {
                ImGui::InputTextWithHint("##why", "Γιατί εξαιρείται (υποχρεωτικό)", r.reason, sizeof(r.reason));
            }
            ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1);
            if (r.included) {
                if (InputMoney("##paid", &r.paid)) r.paid = std::min(std::max(0.0, r.paid), r.amount);
                if (ToCents(r.paid) > ToCents(r.amount)) r.paid = r.amount;
            } else { r.paid = 0; ImGui::TextDisabled("—"); }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    long long paidSum = 0; int nPayers = 0; for (auto& r : g_jc.d.rows) if (r.included) { paidSum += ToCents(r.paid); if (ToCents(r.paid) > 0) nPayers++; }
    bool recOk = true;
    if (paidSum > 0) recOk = DrawReceiptForm("jcrec", g_jc.rec, FromCents(paidSum), nPayers, true);      // each paying resident gets his own receipt, tied to his payment
    // checks + save
    long long diff = ToCents(g_jc.d.total) - sum;
    bool reasonsOk = true;
    for (auto& r : g_jc.d.rows) if (!r.included && std::string(r.reason).find_first_not_of(" \t") == std::string::npos) reasonsOk = false;
    std::string problem;
    if (g_jc.sourceId <= 0) problem = "Επιλέξτε τι χρεώνεται.";
    else if (g_jc.buildingId <= 0) problem = "Επιλέξτε πολυκατοικία.";
    else if (ToCents(g_jc.d.total) <= 0) problem = "Δώστε το ποσό.";
    else if (nInc == 0) problem = "Κανένας ένοικος δεν χρεώνεται.";
    else if (diff != 0) problem = "Τα μερίδια αθροίζουν " + Eur(FromCents(sum)) + " — " +
                                  (diff > 0 ? "λείπουν " : "περισσεύουν ") + Eur(FromCents(std::llabs(diff))) + ".";
    else if (!reasonsOk) problem = "Κάθε εξαίρεση χρειάζεται αιτία.";
    else if (g_jc.cost < 0) problem = "Το κόστος δεν μπορεί να είναι αρνητικό.";
    else if (!recOk) problem = "Συμπληρώστε σωστά την απόδειξη.";
    else for (auto& r : g_jc.d.rows)
        if (r.included && ToCents(r.amount) <= 0) {
            problem = "Μερίδιο 0 € για " + r.apt_label + " (" + r.user_name + "): δώστε ποσό, ή εξαιρέστε το.";
            break;
        }
    if (problem.empty() && g_jc.label.str().empty()) problem = "Δώστε περιγραφή.";
    ImGui::TextUnformatted("Χρεώνουμε"); ImGui::SameLine(); ImGui::Text("%s", Eur(g_jc.d.total).c_str());
    ImGui::SameLine(0, 18); ImGui::TextUnformatted("εισπράξεις"); ImGui::SameLine(); ImGui::TextColored(kColIn, "%s", Eur(FromCents(paidSum)).c_str());
    ImGui::SameLine(0, 18); ImGui::TextUnformatted("οφειλές"); ImGui::SameLine(); ImGui::TextColored(kColOwed, "%s", Eur(FromCents(ToCents(g_jc.d.total) - paidSum)).c_str());
    ImGui::SameLine(0, 18); ImGui::TextUnformatted("κόστος"); ImGui::SameLine(); ImGui::TextColored(kColOut, "%s", Eur(g_jc.cost).c_str());
    ImGui::SameLine(0, 18); ImGui::TextUnformatted("πληρωμές"); ImGui::SameLine(); ImGui::TextColored(kColOut, "%s", Eur(g_jc.costPaid).c_str());
    ImGui::SameLine(0, 18); ImGui::TextUnformatted("οφειλόμενα"); ImGui::SameLine(); ImGui::TextColored(kColWeOwe, "%s", Eur(MoneySub(std::max(0.0, g_jc.cost), g_jc.costPaid)).c_str());
    if (problem.empty()) ImGui::TextColored(kColIn, "Σύνολο %s σε %d ενοίκους — κάθε ένοικος παίρνει τη δική του χρέωση.", Eur(g_jc.d.total).c_str(), nInc);
    else ImGui::TextColored(kInvalidMoneyCol, "%s", problem.c_str());

    if (!problem.empty()) g_jc.confirming = false;
    if (!g_jc.confirming) {
        ImGui::BeginDisabled(!problem.empty());
        if (ImGui::Button("Χρέωση...", ImVec2(150, 0))) g_jc.confirming = true;
        ImGui::EndDisabled();
    } else {
        int nExc = 0; for (auto& r : g_jc.d.rows) if (!r.included) nExc++;
        ImGui::TextColored(ImVec4(1.0f,0.8f,0.3f,1), "Επιβεβαίωση: %d χρεώσεις, %d εξαιρέσεις%s. Η κατανομή δεν αλλάζει μετά την αποθήκευση.", nInc, nExc,
                           g_jc.cost > 0.005 ? (", κόστος " + Eur(g_jc.cost) + " (πληρωμές " + Eur(g_jc.costPaid) + ")").c_str() : "");
        if (ImGui::Button("Επιβεβαίωση χρέωσης", ImVec2(190, 0))) {
            std::string srcLabel;
            auto lines = JointLines(&srcLabel);
            JointOp op;
            op.building_id = g_jc.buildingId;
            op.source_type = g_jc.sourceType == 1 ? "package" : "service";
            op.source_id = g_jc.sourceId; op.kind = "equal"; op.label = g_jc.label.str();
            op.total = g_jc.d.total; op.op_date = g_jc.date; op.params_json = "{}"; op.note = g_jc.note.str();
            std::vector<JointShare> shares;
            for (auto& r : g_jc.d.rows) {
                JointShare sh; sh.apartment_id = r.apartment_id; sh.user_id = r.user_id; sh.amount = r.amount;
                sh.included = r.included; sh.exclude_reason = r.reason; sh.paid = r.included ? r.paid : 0.0;
                shares.push_back(sh);
            }
            int opId = [&] { Receipt rcp = ReceiptFromForm(g_jc.rec, 0); bool wr = paidSum > 0 && g_jc.rec.issue; return g_app.conn->chargeJoint(op, lines, shares, std::max(0.0, g_jc.cost), g_jc.partner, g_jc.costPaid, wr ? &rcp : nullptr); }();
            g_jc.confirming = false;
            if (opId > 0) { g_jc.show = false; g_app.reloadGen++; OpenJointView(opId); }
        }
        ImGui::SameLine();
        if (ImGui::Button("Πίσω", ImVec2(120, 0))) g_jc.confirming = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Κλείσιμο", ImVec2(150, 0))) { g_jc.show = false; ImGui::CloseCurrentPopup(); }
    if (!g_jc.show) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ---- one operation: who was charged, who was excluded and why ----
static int g_jointViewId = -1;
static void OpenJointView(int id) { g_jointViewId = id; }

static void DrawJointViewWindow() {
    if (g_jointViewId < 0) return;
    static Cached<std::pair<JointOp, std::vector<JointShare>>> cache;
    const auto& data = cache.get(std::to_string(g_jointViewId), [] {
        return std::make_pair(g_app.conn->getJointOpById(g_jointViewId), g_app.conn->getJointShares(g_jointViewId)); });
    const JointOp& o = data.first;
    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(1000, 560), ImGuiCond_Appearing);
    std::string title = "Κοινόχρηστο: " + o.label + "###jointview";
    if (ImGui::Begin(title.c_str(), &open)) {
        ImGui::TextColored(ImVec4(1.0f,0.8f,0.3f,1), "%s", o.label.c_str());
        ImGui::Text("%s  ·  %s", o.building_address.c_str(), o.op_date.c_str());
        ImGui::Text("Σύνολο %s  ·  εισπράχθηκαν %s  ·  υπόλοιπο %s", Eur(o.total).c_str(), Eur(o.paid).c_str(),
                    Eur(MoneySub(o.total, o.paid)).c_str());
        ImGui::TextDisabled("Καταχωρήθηκε %s  ·  %d χρεώθηκαν, %d εξαιρέθηκαν", o.created_at.c_str(), o.included, o.excluded);
        { double cost = g_app.conn->getJointOpCost(o.id);
          if (cost > 0.005) ImGui::TextColored(MoneySub(o.total, cost) >= 0 ? kColIn : kColOut, "Κόστος μας %s (πληρωμένο)  ·  κέρδος %s", Eur(cost).c_str(), Eur(MoneySub(o.total, cost)).c_str()); }
        if (!o.note.empty()) ImGui::TextWrapped("Σημείωση: %s", o.note.c_str());
        ImGui::Separator();
        if (ImGui::BeginTable("jshares", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, -1))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Διαμέρισμα", ImGuiTableColumnFlags_WidthStretch, 0.7f);
            ImGui::TableSetupColumn("Ένοικος", ImGuiTableColumnFlags_WidthStretch, 1.6f);
            ImGui::TableSetupColumn("Μερίδιο", ImGuiTableColumnFlags_WidthStretch, 0.8f);
            ImGui::TableSetupColumn("Πληρωμένο", ImGuiTableColumnFlags_WidthStretch, 0.8f);
            ImGui::TableSetupColumn("Κατάσταση", ImGuiTableColumnFlags_WidthStretch, 1.6f);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 190);
            ImGui::TableHeadersRow();
            for (auto& s : data.second) {
                ImGui::PushID(s.id);
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(s.apartment_label.c_str());
                ImGui::TableNextColumn();
                if (s.user_id > 0) ImGui::TextUnformatted(s.user_name.c_str()); else ImGui::TextDisabled("— κενό —");
                if (s.included) {
                    ImGui::TableNextColumn(); ImGui::Text("%s", Eur(s.amount).c_str());
                    ImGui::TableNextColumn(); ImGui::Text("%s", Eur(s.paid).c_str());
                    ImGui::TableNextColumn();
                    bool done = ToCents(s.paid) >= ToCents(s.amount);
                    ImGui::TextColored(done ? kColIn : kColOwed, done ? "Εξοφλήθηκε" : "Χρεώθηκε");
                    ImGui::TableNextColumn();
                    if (ImGui::SmallButton("Καρτέλα")) OpenResidentLedger(s.user_id);
                    if (!done) { ImGui::SameLine(); if (ImGui::SmallButton("Είσπραξη")) OpenRecordPayment(s.user_id); }
                } else {
                    ImGui::TableNextColumn(); ImGui::TextDisabled("—");
                    ImGui::TableNextColumn(); ImGui::TextDisabled("—");
                    ImGui::TableNextColumn(); ImGui::TextColored(ImVec4(0.75f,0.60f,0.95f,1), "Εξαιρέθηκε: %s", s.exclude_reason.c_str());
                    ImGui::TableNextColumn();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
    if (!open) g_jointViewId = -1;
}

// ---- list of operations (Joint Services tab: all buildings; Buildings tab: one building) ----
// Spreadsheet view: every column sortable (◄ ▲ ▼ ►), paged. «Ποσοστό ολοκλήρωσης» = collected / total, so sorting it
// ascending shows the joint charges with the most still owed first.
enum { JC_DATE, JC_BLD, JC_LABEL, JC_KIND, JC_TOTAL, JC_PAID, JC_LEFT, JC_PCT, JC_INC, JC_EXC, JC_ACT, JC_COUNT };
static const ListColDef kJointCols[JC_COUNT] = {
    { "Ημ/νία",              ImGuiTableColumnFlags_WidthFixed, 100.0f, true },
    { "Πολυκατοικία",        ImGuiTableColumnFlags_WidthFixed, 170.0f, true },
    { "Περιγραφή",           ImGuiTableColumnFlags_WidthFixed, 280.0f, true },
    { "Κόστος",          ImGuiTableColumnFlags_WidthFixed, 110.0f, true },
    { "Σύνολο",              ImGuiTableColumnFlags_WidthFixed, 100.0f, true },
    { "Εισπράχθηκαν",        ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 110.0f, true },
    { "Υπόλοιπο",            ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 100.0f, true },
    { "Ποσοστό Ολοκλήρωσης", ImGuiTableColumnFlags_WidthFixed, 170.0f, true },
    { "Χρεώθηκαν",           ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 100.0f, true },
    { "Εξαιρέθηκαν",         ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultHide, 110.0f, true },
    { "",                    ImGuiTableColumnFlags_WidthFixed,  80.0f, false },
};
static double JointPct(const JointOp& o) { return o.total > 0.005 ? std::min(100.0, o.paid / o.total * 100.0) : 100.0; }

static void DrawJointOpsTable(int buildingId, const char* id) {
    struct St { GridState g; char q[128] = ""; };
    static std::map<std::string, St> states;
    St& S = states[id];
    GridState& g = S.g;
    if (g.order.empty()) { for (int i = 0; i < JC_COUNT; i++) g.order.push_back(i); g.sort.keys = { { JC_DATE, false } }; }
    ImGui::SetNextItemWidth(320);
    if (ImGui::InputTextWithHint("##jq", "Αναζήτηση: περιγραφή ή πολυκατοικία...", S.q, sizeof(S.q))) g.pager.Reset();
    // every sort key goes to SQL (paging needs the ORDER BY in the query)
    std::string key = std::to_string(buildingId) + "|" + S.q + "|" + SortKeyStr(g.sort.keys) + "|" +
                      std::to_string(g.pager.offset) + "|" + std::to_string(g.pager.pageSize);
    struct Pg { std::vector<JointOp> rows; int count = 0; double total = 0, paid = 0; std::map<int, double> cost; };
    static std::map<std::string, Cached<Pg>> caches;
    const Pg& pg = caches[id].get(key, [&] {
        Pg p; std::string q = S.q;
        p.count = g_app.conn->countJointOps(buildingId, q, &p.total, &p.paid);
        p.rows = g_app.conn->pageJointOps(buildingId, q, g.sort.keys, g.pager.offset, g.pager.pageSize);
        for (auto& o : p.rows) p.cost[o.id] = g_app.conn->getJointOpCost(o.id);
        return p;
    });
    ImGui::SameLine();
    ImGui::TextDisabled("%d χρεώσεις · μένουν να εισπραχθούν %s", pg.count, Eur(MoneySub(pg.total, pg.paid)).c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Σύνολο %s · εισπράχθηκαν %s · ολοκλήρωση %.1f%%\nΔεξί κλικ στις επικεφαλίδες: εμφάνιση περισσότερων στηλών.",
        Eur(pg.total).c_str(), Eur(pg.paid).c_str(), pg.total > 0.005 ? pg.paid / pg.total * 100.0 : 100.0);
    g.pager.total = pg.count;
    if (pg.count == 0) { ImGui::TextDisabled(S.q[0] ? "Καμία χρέωση με αυτή την αναζήτηση." : "Δεν έχουν γίνει κοινόχρηστες χρεώσεις ακόμα."); return; }
    const bool all = buildingId <= 0;
    float h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.6f;
    if (ImGui::BeginTable(id, JC_COUNT, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                                        ImGuiTableFlags_Resizable | ImGuiTableFlags_Hideable, ImVec2(0, std::max(160.0f, h)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ListSetupColumns(g.order, kJointCols);
        if (!all) ImGui::TableSetColumnEnabled(ListPos(g.order, JC_BLD), false);
        ListHeaderRow(g.order, kJointCols, g.sort, g.pager);
        auto P = [&](int c) { return ListPos(g.order, c); };
        for (const JointOp& o : pg.rows) {
            ImGui::PushID(o.id); ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(P(JC_DATE)); ImGui::TextUnformatted(DateGr(o.op_date).c_str());
            ImGui::TableSetColumnIndex(P(JC_BLD)); ImGui::TextUnformatted(o.building_address.c_str());
            ImGui::TableSetColumnIndex(P(JC_LABEL)); ImGui::TextUnformatted(o.label.c_str());
            ImGui::TableSetColumnIndex(P(JC_KIND)); { double cs = pg.cost.count(o.id) ? pg.cost.at(o.id) : 0.0; if (cs > 0.005) ImGui::TextColored(kColOut, "%s", Eur(cs).c_str()); else ImGui::TextDisabled("—"); }
            ImGui::TableSetColumnIndex(P(JC_TOTAL)); ImGui::TextUnformatted(Eur(o.total).c_str());
            ImGui::TableSetColumnIndex(P(JC_PAID)); ImGui::TextColored(kColIn, "%s", Eur(std::min(o.paid, o.total)).c_str());
            double left = MoneySub(o.total, std::min(o.paid, o.total));
            ImGui::TableSetColumnIndex(P(JC_LEFT)); ImGui::TextColored(left > 0.005 ? kColOwed : kColMuted, "%s", Eur(left).c_str());
            ImGui::TableSetColumnIndex(P(JC_PCT));
            { double pct = JointPct(o); char b[32]; std::snprintf(b, sizeof(b), "%.1f%%", pct);
              ImGui::PushStyleColor(ImGuiCol_PlotHistogram, pct >= 99.995 ? ImVec4(0.30f,0.70f,0.45f,1) : ImVec4(0.85f,0.60f,0.25f,1));
              ImGui::ProgressBar((float)(pct / 100.0), ImVec2(-1, 0), b); ImGui::PopStyleColor(); }
            ImGui::TableSetColumnIndex(P(JC_INC)); ImGui::Text("%d", o.included);
            ImGui::TableSetColumnIndex(P(JC_EXC));
            if (o.excluded) ImGui::TextColored(ImVec4(0.75f,0.60f,0.95f,1), "%d", o.excluded); else ImGui::TextDisabled("0");
            ImGui::TableSetColumnIndex(P(JC_ACT)); if (ImGui::SmallButton("Άνοιγμα")) OpenJointView(o.id);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    DrawPager(g.pager, (std::string(id) + "_pg").c_str());
}

static void DrawJointServicesScreen() {
    if (ImGui::Button("+ Νέα συνηθισμένη χρέωση")) OpenJointServiceDialog(nullptr);
    ImGui::SameLine(); if (ImGui::Button("Χρέωση πολυκατοικίας")) OpenJointCharge(-1);
    ImGui::TextDisabled("Πάνω: οι συνηθισμένες χρεώσεις σας, για γρήγορη χρέωση. Κάτω: το ιστορικό όλων των κοινοχρήστων με την πορεία εξόφλησης.");
    enum { JK_NAME, JK_NOTES, JK_ACT, JK_COUNT };
    static const ListColDef kCatCols[JK_COUNT] = {
        { "Συνηθισμένη χρέωση", ImGuiTableColumnFlags_WidthFixed, 360.0f, true }, { "Σημειώσεις", ImGuiTableColumnFlags_WidthFixed, 460.0f, true },
        { "", ImGuiTableColumnFlags_WidthFixed, 280.0f, false } };
    static GridState g; static char q[128] = "";
    Pager& pager = g.pager;
    if (g.order.empty()) { for (int i = 0; i < JK_COUNT; i++) g.order.push_back(i); g.sort.keys = { { JK_NAME, true } }; }
    ImGui::SetNextItemWidth(320);
    if (ImGui::InputTextWithHint("##jcq", "Αναζήτηση...", q, sizeof(q))) pager.Reset();
    struct Pg { std::vector<JointService> rows; int count = 0; };
    static Cached<Pg> cache;
    const Pg& pg = cache.get(std::string(q) + "|" + SortKeyStr(g.sort.keys) + "|" + std::to_string(pager.offset) + "|" + std::to_string(pager.pageSize), [&] {
        Pg r; r.count = g_app.conn->countJointCatalog(q); r.rows = g_app.conn->pageJointCatalog(q, pager.offset, pager.pageSize, g.sort.keys); return r; });
    pager.total = pg.count;
    ImGui::SameLine(); ImGui::TextDisabled("%d συνηθισμένες χρεώσεις", pg.count);
    if (pg.count == 0) ImGui::TextDisabled("Δεν έχετε ορίσει συντομεύσεις ακόμα. Πατήστε «+ Νέα συνηθισμένη χρέωση».");
    float h = std::min(ImGui::GetContentRegionAvail().y * 0.34f, 60.0f + 28.0f * (float)pg.rows.size());
    if (pg.count > 0 && ImGui::BeginTable("jointcat", JK_COUNT, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                                          ImGuiTableFlags_Resizable, ImVec2(0, std::max(160.0f, h)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ListSetupColumns(g.order, kCatCols);
        ListHeaderRow(g.order, kCatCols, g.sort, g.pager);
        auto P = [&](int c) { return ListPos(g.order, c); };
        for (auto& j : pg.rows) {
            ImGui::PushID(j.id); ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(P(JK_NAME)); ImGui::TextUnformatted(j.service.label.c_str());
            ImGui::TableSetColumnIndex(P(JK_NOTES)); ImGui::TextDisabled("%s", TruncToWidth(j.service.extra_notes, ImGui::GetContentRegionAvail().x).c_str());
            ImGui::TableSetColumnIndex(P(JK_ACT));
            if (ImGui::SmallButton("Χρέωση")) OpenJointChargeFor(2, j.service.id, j.service.label);
            ImGui::SameLine(); if (ImGui::SmallButton("Επεξεργασία")) OpenJointServiceDialog(&j);
            ImGui::SameLine(); if (ImGui::SmallButton("Ιστορικό")) OpenServiceDetail(j.service.id);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (pg.count > 0) DrawPager(pager, "jointcatpg");
    SectionTitle("Ιστορικό κοινοχρήστων (όλες οι πολυκατοικίες)");
    DrawJointOpsTable(-1, "jointops_all");
    DrawJointServiceDialog();
}

// ---------------------------------------------------------------------------
// MASTER SERVICES SCREEN ("Πακέτα Υπηρεσιών") -- a purely organizational/viewing
// grouping over real, independent, already-reusable `services` rows. Payments
// are untouched by this feature: this screen only navigates to each component
// service's own (unchanged) Service Detail window and reuses the existing
// getUsersForService/payment-sum logic for each component's roster, mirroring
// the visual style of the Joint Services screen above.
// ---------------------------------------------------------------------------
static void OpenPackageCharge(int pkgId);

static int g_selectedPackageId = -1;

// Two panes. LEFT: every package with its components and prices (click to select).
// RIGHT: for the selected package, which clients bought the WHOLE package, then the
// clients that bought only PARTS of it, most parts first. "Bought" = the client has
// at least one payment on that component service.
// Package editor: rename, add or take out services. Nothing is deleted: a removed service is kept as
// "removed" in the package's history, and charges already made keep their own lines and batch label.
static int g_pkgEditId = -1;
static void DrawPackageEditor() {
    if (g_pkgEditId <= 0) return;
    static int loaded = -1; static TextBuf name; static int addSvc = -1;
    if (loaded != g_pkgEditId) { loaded = g_pkgEditId; name.set(g_app.conn->getPackageLabel(g_pkgEditId)); addSvc = -1; }
    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(560, 460), ImGuiCond_FirstUseEver);
    ImGui::Begin("Επεξεργασία πακέτου###pkgedit", &open, ImGuiWindowFlags_NoCollapse);
    ImGui::TextDisabled("Οι αλλαγές ισχύουν για τις επόμενες χρεώσεις. Όσα χρεώθηκαν ήδη μένουν όπως ήταν.");
    ImGui::SetNextItemWidth(360); ImGui::InputText("Όνομα", name.data, sizeof(name.data));
    ImGui::SameLine();
    if (ImGui::Button("Αποθήκευση ονόματος") && !name.str().empty()) {
        int id = g_pkgEditId; std::string nm = name.str(), old = g_app.conn->getPackageLabel(id);
        RequestConfirm("Μετονομασία πακέτου", { {"Όνομα", old, nm} }, [id, nm]() { g_app.conn->renameMasterService(id, nm); g_app.reloadServices(); });
    }
    SectionTitle("Υπηρεσίες του πακέτου");
    auto comps = g_app.conn->getMasterServiceComponents(g_pkgEditId);
    if (ImGui::BeginTable("pkgcomps", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 240))) {
        ImGui::TableSetupColumn("Υπηρεσία", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Τιμή", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableHeadersRow();
        for (auto& c : comps) {
            ImGui::PushID(c.id); ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.label.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%s", Eur(c.suggested_price).c_str());
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Αφαίρεση")) {
                int pid = g_pkgEditId, sid = c.id; std::string lbl = c.label;
                RequestConfirm("Αφαίρεση υπηρεσίας από το πακέτο", { {"Υπηρεσία", lbl, "(αφαιρείται, μένει στο ιστορικό)"} },
                               [pid, sid]() { g_app.conn->removeMasterServiceComponent(pid, sid); g_app.reloadServices(); });
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Προσθήκη:"); ImGui::SameLine();
    PickService("pkgaddsvc", addSvc, 300.0f);
    ImGui::SameLine();
    if (ImGui::Button("+ Προσθήκη") && addSvc > 0) {
        g_app.conn->addMasterServiceComponent(g_pkgEditId, addSvc); g_app.reloadServices(); addSvc = -1;
    }
    ImGui::End();
    if (!open) { g_pkgEditId = -1; loaded = -1; }
}

static void DrawMasterServicesScreen() {
    if (ImGui::Button("+ Νέο Πακέτο")) {
        g_serviceDialogIsEdit = false;
        ResetServiceDialogFields();
        sb_isPackage = true;
        g_showServiceDialog = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Ανανέωση")) g_app.reloadServices();
    ImGui::Separator();

    // ---------------- LEFT: every package, searched / sorted / paged in SQL ----------------
    DrawPackageEditor();
    {
    enum { PC_NAME, PC_N, PC_PARTS, PC_TOTAL, PC_COST, PC_ACT, PC_COUNT };
    static const ListColDef kPkgCols[PC_COUNT] = {
        { "Πακέτο", ImGuiTableColumnFlags_WidthFixed, 220.0f, true }, { "Υπηρεσίες", ImGuiTableColumnFlags_WidthFixed, 100.0f, true },
        { "Τι περιλαμβάνει (τιμή ανά υπηρεσία)", ImGuiTableColumnFlags_WidthStretch, 1.0f, false },
        { "Σύνολο τιμής", ImGuiTableColumnFlags_WidthFixed, 120.0f, true }, { "Σύνολο κόστους", ImGuiTableColumnFlags_WidthFixed, 150.0f, true },
        { "", ImGuiTableColumnFlags_WidthFixed, 390.0f, false } };
    static GridState g; static char q[128] = "";
    if (g.order.empty()) { for (int i = 0; i < PC_COUNT; i++) g.order.push_back(i); g.sort.keys = { { PC_NAME, true } }; }
    ImGui::SetNextItemWidth(320);
    if (ImGui::InputTextWithHint("##pkq", "Αναζήτηση πακέτου ή υπηρεσίας του...", q, sizeof(q))) g.pager.Reset();
    SortSpec pkgSort = MapSort(g.sort.keys, {{PC_NAME, 0}, {PC_N, 1}, {PC_TOTAL, 2}, {PC_COST, 3}});
    struct Pg { std::vector<DataConn::PackageRow> rows; int count = 0; };
    static Cached<Pg> cache;
    const Pg& pg = cache.get(std::string(q) + "|" + SortKeyStr(pkgSort) + "|" + std::to_string(g.pager.offset) + "|" +
                             std::to_string(g.pager.pageSize), [&] {
        Pg p; p.count = g_app.conn->countPackages(q); p.rows = g_app.conn->pagePackages(q, pkgSort, g.pager.offset, g.pager.pageSize); return p; });
    g.pager.total = pg.count;
    ImGui::SameLine(); ImGui::TextDisabled("%d πακέτα", pg.count);
    if (g_selectedPackageId <= 0 && !pg.rows.empty()) g_selectedPackageId = pg.rows[0].pkg.id;
    float h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.6f;
    if (ImGui::BeginTable("pkgsgrid", PC_COUNT, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                          ImGuiTableFlags_Resizable, ImVec2(0, std::max(160.0f, h)))) {
        ImGui::TableSetupScrollFreeze(1, 1);
        ListSetupColumns(g.order, kPkgCols);
        ListHeaderRow(g.order, kPkgCols, g.sort, g.pager);
        auto P = [&](int c) { return ListPos(g.order, c); };
        for (auto& r : pg.rows) {
            ImGui::PushID(r.pkg.id); ImGui::TableNextRow();
            bool sel = r.pkg.id == g_selectedPackageId;
            ImGui::TableSetColumnIndex(P(PC_NAME));
            (void)sel;
            if (LinkCell(r.pkg.label)) OpenPackageView(r.pkg.id);
            ImGui::TableSetColumnIndex(P(PC_N)); ImGui::Text("%d", r.components);
            ImGui::TableSetColumnIndex(P(PC_PARTS));
            ImGui::TextUnformatted(TruncToWidth(r.parts, ImGui::GetContentRegionAvail().x).c_str());
            if (ImGui::IsItemHovered() && !r.parts.empty()) {
                ImGui::BeginTooltip(); ImGui::TextUnformatted(r.pkg.label.c_str()); ImGui::Separator();
                size_t p0 = 0; std::string s = r.parts;
                while (p0 < s.size()) { size_t p1 = s.find("), ", p0); std::string part = s.substr(p0, p1 == std::string::npos ? std::string::npos : p1 + 1 - p0);
                                        ImGui::BulletText("%s", part.c_str()); if (p1 == std::string::npos) break; p0 = p1 + 3; }
                ImGui::EndTooltip();
            }
            ImGui::TableSetColumnIndex(P(PC_TOTAL)); ImGui::TextColored(kColIn, "%s", Eur(r.total).c_str());
            ImGui::TableSetColumnIndex(P(PC_COST)); if (r.totalCost > 0.005) ImGui::TextColored(kColOut, "%s", Eur(r.totalCost).c_str()); else ImGui::TextDisabled("—");
            ImGui::TableSetColumnIndex(P(PC_ACT));
            if (ImGui::SmallButton("Χρέωση")) OpenPackageCharge(r.pkg.id);
            ImGui::SameLine(); if (ImGui::SmallButton("Ως κοινόχρηστο")) OpenJointChargeFor(1, r.pkg.id, r.pkg.label);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    DrawPager(g.pager, "pkgspg");
    }


}

// ---------------------------------------------------------------------------
// PAYMENTS SCREEN (heaviest filter set, per spec)
// ---------------------------------------------------------------------------
static std::string g_payStartDate, g_payEndDate;
static int g_payUserFilter = -1;
static int g_payServiceFilter = -1;
static int g_payStatusFilter = 0; // 0=all,1=future,2=resolved
static const char* kStatusNames[] = { "Όλα", "Αναμένονται", "Λήξιμα" };
static int g_payDirectionFilter = 0; // 0=all,1=in,2=out
static const char* kDirectionFilterNames[] = { "Όλες", "Εισπράξεις", "Πληρωμές" };

// Direction/type badge: green "down" arrow for money coming in from the client,
// red "up" arrow for money the manager fronted (client owes it back). "comment"
// is not a direction anymore -- a payment's optional comment lives in its `notes`
// field, an always-visible attribute alongside the amount, not a substitute type.
// Banking-app-style restraint: green = money in, red = money out.
static ImVec4 DirectionColor(const std::string& direction) {
    return direction == "out" ? kColOut : kColIn;
}
static const char* DirectionBadge(const std::string& direction) {
    return direction == "out" ? "\xE2\x86\x91 Πληρωμή" // "↑ Πληρωμή"
                               : "\xE2\x86\x93 Είσπραξη"; // "↓ Είσπραξη"
}

// Status color, restrained to the same 3-4 color banking-app system: green = resolved
// (settled/real), red = overdue (leftover pre-simplification data only), neutral
// gray-blue = future (just "scheduled", not itself an alert -- the amber "needs
// attention soon" tier lives only in the notifications popup, tied to proximity to
// resolution_date, not to this general status dot). Old "pending" rows fall back to
// the same neutral tone as future since they're not urgent by themselves either.
static ImVec4 StatusColor(const std::string& status) {
    if (status == "future") return ImVec4(0.60f, 0.65f, 0.72f, 1);
    if (status == "overdue") return kColOut;
    if (status == "pending") return ImVec4(0.60f, 0.65f, 0.72f, 1);
    return kColIn; // paid / resolved (default)
}


// Status in words with its colour: Προγραμματισμένη / Οφείλεται / Εξοφλήθηκε (or Πληρώθηκε for money paid out).
static void StatusCell(const Payment& p) {
    // four words: in = Εξοφλήθηκε (we got paid) / Οφειλή (owed to us); out = Πληρώθηκε (we paid) / Οφειλούμενη (we owe it)
    bool open = p.status == "future" || p.balance > 0.005;
    if (p.direction == "out") ImGui::TextColored(open ? kColWeOwe : kColOut, open ? "Οφειλούμενη" : "Πληρώθηκε");
    else ImGui::TextColored(open ? kColOwed : kColIn, open ? "Οφειλή" : "Εξοφλήθηκε");
}
static bool g_showPaymentDialog = false;
static bool g_paymentDialogWasOpen = false;
static bool g_paymentDialogIsEdit = false;
static int g_paymentDialogId = -1;
// ---------------------------------------------------------------------------
// Receipt details asked when money comes in. Until the client's real receipt book / invoicing provider is
// known, a receipt is: the app's own sequential number (series A), how the money came in, and the references
// that prove it (number of a handwritten receipt, bank transaction reference, RF code). All optional except
// the method; the number is assigned automatically and can never repeat.
// ---------------------------------------------------------------------------
static ReceiptForm pb_receipt, rb_receipt, pc_receipt;

// ---------------------------------------------------------------------------
// INSTALLMENTS (δόσεις): split a charge into N payments every month/week. Shared by the service and package dialogs.
// ---------------------------------------------------------------------------
struct PlanForm { bool on = false; int count = 3; int every = 0; bool firstNow = true; int mode = 0; };   // every 0 month, 1 week; mode 0 end, 1 each

static std::string AddPeriodIso(const std::string& date, int k, int every) {
    int y = 0, m = 0, d = 0;
    if (std::sscanf(date.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return date;
    if (every == 0) {                                     // months, clamped to the month's length (31/01 -> 28/02)
        int mm = m - 1 + k; y += mm / 12; m = mm % 12 + 1;
        static const int len[] = {31,28,31,30,31,30,31,31,30,31,30,31};
        int ml = len[m - 1] + (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 1 : 0);
        d = std::min(d, ml);
    } else {                                              // weeks
        std::tm tm{}; tm.tm_year = y - 1900; tm.tm_mon = m - 1; tm.tm_mday = d + 7 * k; tm.tm_hour = 12;
        std::mktime(&tm); y = tm.tm_year + 1900; m = tm.tm_mon + 1; d = tm.tm_mday;
    }
    char b[16]; std::snprintf(b, sizeof(b), "%04d-%02d-%02d", y, m, d); return b;
}

// Per installment amounts of a total (cents exact, the leftover cents go to the first installments).
static std::vector<long long> PlanAmounts(double total, int n) { return SplitCents(ToCents(total), std::vector<double>(n, 1.0)); }

// Draws the section. Returns false while invalid.
static bool DrawPlanForm(const char* id, PlanForm& f, double total, const std::string& start) {
    ImGui::PushID(id);
    ImGui::Spacing();
    ImGui::Checkbox("Πληρωμή σε δόσεις", &f.on);
    ImGui::SameLine(); ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Το ποσό μοιράζεται σε ισόποσες δόσεις, ανά μήνα ή εβδομάδα.\n"
                                                  "Οι επόμενες δόσεις γράφονται ως μελλοντικές πληρωμές και εμφανίζονται στις υπενθυμίσεις.\n"
                                                  "Όταν πληρώνεται μια δόση (Επίλυση ή Νέα είσπραξη) γίνεται κανονική πληρωμή.");
    bool ok = true;
    if (f.on) {
        ImGui::Indent();
        ImGui::SetNextItemWidth(110); ImGui::InputInt("δόσεις", &f.count);
        f.count = std::max(2, std::min(f.count, 120));
        ImGui::SameLine(); ImGui::RadioButton("ανά μήνα", &f.every, 0);
        ImGui::SameLine(); ImGui::RadioButton("ανά εβδομάδα", &f.every, 1);
        ImGui::Checkbox("Η 1η δόση πληρώνεται τώρα", &f.firstNow);
        ImGui::TextDisabled("Απόδειξη:"); ImGui::SameLine();
        ImGui::RadioButton("μία, όταν εξοφληθεί η τελευταία δόση", &f.mode, 0); ImGui::SameLine();
        ImGui::RadioButton("σε κάθε δόση", &f.mode, 1);
        auto parts = PlanAmounts(total, f.count);
        if (ToCents(total) < f.count) { ImGui::TextColored(kInvalidMoneyCol, "Το ποσό είναι πολύ μικρό για τόσες δόσεις."); ok = false; }
        float h = std::min(24.0f * (float)f.count + 30.0f, 170.0f);
        if (ImGui::BeginTable("plan", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, h))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Δόση", ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("Ημερομηνία", ImGuiTableColumnFlags_WidthFixed, 110);
            ImGui::TableSetupColumn("Ποσό", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (int k = 0; k < f.count; k++) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%d/%d", k + 1, f.count);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(AddPeriodIso(start, k, f.every).c_str());
                ImGui::TableNextColumn(); ImGui::Text("%s%s", Eur(FromCents(parts[k])).c_str(), k == 0 && f.firstNow ? "  (πληρώνεται τώρα)" : "");
            }
            ImGui::EndTable();
        }
        ImGui::Unindent();
    }
    ImGui::PopID();
    return ok;
}

// The charges of a plan: every item (service, total) split over the installments; installment k is due on its date.
static std::vector<Payment> BuildPlanLines(int user, const std::vector<std::pair<int, double>>& items, const PlanForm& f,
                                           const std::string& start, const std::string& note, int aptId) {
    std::vector<Payment> lines;
    std::vector<std::vector<long long>> parts;
    for (auto& it : items) parts.push_back(PlanAmounts(it.second, f.count));
    for (int k = 0; k < f.count; k++) {
        std::string date = AddPeriodIso(start, k, f.every);
        bool paidNow = k == 0 && f.firstNow;
        for (size_t i = 0; i < items.size(); i++) {
            if (parts[i][k] <= 0) continue;
            Payment p;
            p.user_id = user; p.service_id = items[i].first; p.apartment_id = aptId;
            std::sscanf(date.c_str(), "%d-%d-%d", &p.year, &p.month, &p.day);
            p.amount_due = FromCents(parts[i][k]);
            p.amount_paid = paidNow ? p.amount_due : 0.0;
            p.balance = MoneySub(p.amount_due, p.amount_paid);
            p.status = paidNow ? "paid" : "future";
            p.resolution_date = paidNow ? "" : date;
            p.direction = "in";
            p.notes = "Δόση " + std::to_string(k + 1) + "/" + std::to_string(f.count) + (note.empty() ? "" : " · " + note);
            lines.push_back(p);
        }
    }
    return lines;
}

// Receipt part of a plan: "each" + paid now -> the normal receipt form for the 1st installment; "end" -> only the method.
static bool DrawPlanReceipt(const char* id, const PlanForm& f, ReceiptForm& rf, double firstAmount) {
    if (!f.firstNow) { ImGui::TextDisabled("Καμία είσπραξη τώρα· η απόδειξη %s.", f.mode == 0 ? "εκδίδεται με την τελευταία δόση" : "εκδίδεται σε κάθε δόση"); return true; }
    if (f.mode == 1) return DrawReceiptForm(id, rf, firstAmount, 1);
    ImGui::PushID(id);
    SectionTitle("Είσπραξη 1ης δόσης");
    ImGui::SetNextItemWidth(220);
    ImGui::Combo("Τρόπος πληρωμής", &rf.method, kReceiptMethodNames, IM_ARRAYSIZE(kReceiptMethodNames));
    ImGui::TextDisabled("Η απόδειξη (μία για όλο το ποσό) εκδίδεται αυτόματα όταν πληρωθεί η τελευταία δόση.");
    ImGui::PopID();
    return true;
}


static int pb_userId = -1, pb_serviceId = -1;
// Apartment the payment is FOR. -1 = automatic: the apartment the client lived in on the payment date
// (residency history). Picked by hand e.g. for arrears of a previous apartment paid after moving.
static int pb_aptId = -1, pb_aptUser = -1;
static int pb_direction = 0; // 0 = in (collect from client), 1 = out (manager paid on client's behalf)
// Simplified status model: a payment is either "future" (scheduled, has a resolution_date,
// doesn't count in any total) or resolved (status="paid", counts fully). No more
// pending/overdue as user-selectable states -- old rows with those statuses still display
// fine (anything != "future" is treated as resolved for coloring/counting).
static bool pb_isFuture = false;
// When true (set by a profile drill-in), the "Χρήστης" combo is hidden -- the
// owner is already fixed by the context that opened the dialog.
static bool pb_lockUser = false;
// Same idea for the service: opening the payment dialog from a service's own
// "Χρέωση" button (or the Service Detail window) already fixes which service
// the payment is for, so the dialog shows it as text instead of a combo --
// the user cannot accidentally bill a different service from here.
static bool pb_lockService = false;
static TextBuf pb_due, pb_paid, pb_notes;
static std::string pb_dateStr;   // "YYYY-MM-DD" for the payment's own date
// Resolution date is picked via the calendar popup (DatePickerButton), never
// typed by hand -- a plain std::string, not a TextBuf, since there's no free
// text entry for it anymore.
static std::string pb_resolutionDate;
static PlanForm pb_plan;
static TextBuf pb_costPaid; static bool pb_costPaidManual = false;                             // how much of our cost we already paid (the rest we owe)
static TextBuf pb_cost; static int pb_costSvc = -2; static int pb_partner = -1;           // what this charge costs us (a paid-out line, settled at once)
static int pb_offerId = -1; static double pb_offerList = 0.0;     // claimed offer and the price before it
static bool DrawOfferClaim(const char* id, int userId, int serviceId, int packageId, const std::string& date, double listTotal,
                           int& offerId, const std::function<void(double)>& onApply, const std::function<void()>& onRemove);

static void OpenAddPayment() {
    g_showPaymentDialog = true; g_paymentDialogIsEdit = false;
    pb_userId = -1; pb_serviceId = -1;          // chosen with the search pickers
    pb_plan = PlanForm{}; pb_offerId = -1; pb_offerList = 0.0;
    pb_dateStr = CurrentDateString();
    pb_due.set("0"); pb_paid.set("0"); pb_notes.set(""); pb_resolutionDate.clear();
    pb_direction = 0; pb_isFuture = false; pb_lockUser = false; pb_lockService = false;
    pb_aptId = -1; pb_aptUser = -1;
    pb_receipt = ReceiptForm{};
    pb_cost.set(""); pb_costSvc = -2; pb_partner = -1; pb_costPaid.set(""); pb_costPaidManual = false;
}


// "Πατησίων 205 – Α1" for an apartment id ("" if unknown). Small query; used by dialogs only.
static std::string ApartmentName(int aptId) {
    if (aptId <= 0) return "";
    std::vector<std::string> c; std::vector<std::vector<std::string>> rows; std::string err;
    g_app.conn->executeRawQuery("SELECT b.address || ' – ' || a.label FROM apartments a JOIN buildings b ON b.id = a.building_id WHERE a.id = "
                                + std::to_string(aptId), c, rows, err);
    return rows.empty() || rows[0].empty() ? "#" + std::to_string(aptId) : rows[0][0];
}

// Apartment the client lived in on `date` (YYYY-MM-DD) -- same rule the DB uses for new payments.
static int ApartmentOnDate(int userId, const std::string& date) {
    int y = 0, m = 0, d = 0;
    if (userId <= 0 || std::sscanf(date.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return -1;
    return g_app.conn->getApartmentOnDate(userId, y, m, d);
}

// Picker for pb_aptId: "automatic" + every apartment the client has lived in (with dates) + their current one.
static void DrawPaymentApartmentPicker() {
    if (pb_aptUser != pb_userId) { pb_aptId = -1; pb_aptUser = pb_userId; }   // other client -> back to automatic
    int autoApt = ApartmentOnDate(pb_userId, pb_dateStr);
    std::string autoLabel = "Αυτόματα: " + (autoApt > 0 ? ApartmentName(autoApt) : std::string("χωρίς διαμέρισμα"));
    std::string preview = pb_aptId > 0 ? ApartmentName(pb_aptId) : autoLabel;
    if (ImGui::BeginCombo("Διαμέρισμα", preview.c_str())) {
        if (ImGui::Selectable(autoLabel.c_str(), pb_aptId <= 0)) pb_aptId = -1;
        std::set<int> shown;
        auto res = g_app.conn->getUserResidencies(pb_userId);
        for (auto it = res.rbegin(); it != res.rend(); ++it) {
            if (!shown.insert(it->apartment_id).second) continue;
            std::string l = ApartmentName(it->apartment_id) + "   (" + it->from_date + " → " + (it->to_date.empty() ? "σήμερα" : it->to_date) + ")";
            if (ImGui::Selectable(l.c_str(), pb_aptId == it->apartment_id)) pb_aptId = it->apartment_id;
        }
        int cur = g_app.conn->getUserById(pb_userId).apartment_id;
        if (cur > 0 && shown.insert(cur).second && ImGui::Selectable(ApartmentName(cur).c_str(), pb_aptId == cur)) pb_aptId = cur;
        if (pb_aptId > 0 && shown.insert(pb_aptId).second) ImGui::Selectable(ApartmentName(pb_aptId).c_str(), true);
        ImGui::EndCombo();
    }
    ImGui::SameLine(); ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Για ποιο διαμέρισμα είναι η πληρωμή. Αυτόματα = όπου έμενε ο πελάτης\n"
                                                  "την ημερομηνία πληρωμής. Επιλέξτε χειροκίνητα π.χ. για παλιές οφειλές\n"
                                                  "προηγούμενου διαμερίσματος που εξοφλούνται μετά τη μετακόμιση.");
}

static void DrawPaymentDialog() {
    if (g_showPaymentDialog && !g_paymentDialogWasOpen) {
        ImGui::OpenPopup("Στοιχεία Πληρωμής");
    }
    g_paymentDialogWasOpen = g_showPaymentDialog;
    if (!g_showPaymentDialog) return;
    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
    if (BeginGuardedModal("Στοιχεία Πληρωμής", g_showPaymentDialog, ImGuiWindowFlags_AlwaysAutoResize, []() {
        std::vector<FieldDiff> d; if (!g_paymentDialogIsEdit) return d;
        Payment o = g_app.conn->getPaymentById(g_paymentDialogId);
        char od[16]; std::snprintf(od, sizeof(od), "%04d-%02d-%02d", o.year, o.month, o.day);
        DiffAdd(d, "Ημερομηνία", od, pb_dateStr);
        if (o.user_id != pb_userId) d.push_back({"Πελάτης", g_app.userName(o.user_id), g_app.userName(pb_userId)});
        if (o.service_id != pb_serviceId) d.push_back({"Υπηρεσία", g_app.serviceLabel(o.service_id), g_app.serviceLabel(pb_serviceId)});
        { int na = pb_aptId > 0 ? pb_aptId : ApartmentOnDate(pb_userId, pb_dateStr);
          if (na != o.apartment_id) d.push_back({"Διαμέρισμα", ApartmentName(o.apartment_id), ApartmentName(na)}); }
        double nd = 0.0, np = 0.0; ParseMoney(pb_due.str(), nd); ParseMoney(pb_paid.str(), np);
        char a[32], b[32];
        if (std::fabs(o.amount_due - nd) > 0.004) { std::snprintf(a, sizeof(a), "%.2f", o.amount_due); std::snprintf(b, sizeof(b), "%.2f", nd); d.push_back({"Οφειλή", a, b}); }
        if (!pb_isFuture && std::fabs(o.amount_paid - np) > 0.004) { std::snprintf(a, sizeof(a), "%.2f", o.amount_paid); std::snprintf(b, sizeof(b), "%.2f", np); d.push_back({"Πληρωμένο", a, b}); }
        if ((o.status == "future") != pb_isFuture) d.push_back({"Κατάσταση", o.status == "future" ? "μελλοντική" : "εξοφλημένη", pb_isFuture ? "μελλοντική" : "εξοφλημένη"});
        DiffAdd(d, "Τύπος", o.direction == "out" ? "Πληρωμή" : "Είσπραξη", pb_direction == 1 ? "Πληρωμή" : "Είσπραξη");
        DiffAdd(d, "Σημειώσεις", o.notes, pb_notes.str());
        if (pb_isFuture) DiffAdd(d, "Ημ/νία επίλυσης", o.resolution_date, pb_resolutionDate);
        return d; })) {
        ImGui::TextUnformatted("Πελάτης:"); ImGui::SameLine(110);
        if (pb_lockUser) ImGui::TextUnformatted(g_app.userName(pb_userId).c_str());
        else PickClient("pbcli", pb_userId, 330);
        ImGui::TextUnformatted("Υπηρεσία:"); ImGui::SameLine(110);
        if (pb_lockService) ImGui::TextUnformatted(g_app.serviceLabel(pb_serviceId).c_str());
        else if (PickService("pbsvc", pb_serviceId, 330) && !g_paymentDialogIsEdit) {
            Service s = g_app.conn->getServiceById(pb_serviceId);          // a new charge starts at the catalog price
            pb_due.set(FormatMoney(s.suggested_price)); pb_paid.set(FormatMoney(s.suggested_price));
        }
        ImGui::TextUnformatted("Ημερομηνία πληρωμής:");
        DatePicker("paydate", pb_dateStr, ImVec2(0, 1));
        if (!g_paymentDialogIsEdit) pb_direction = 0;      // we do not pay on behalf of clients any more: our payments are costs to partners
        else if (pb_direction == 1) ImGui::TextColored(kColOut, "Πληρωμή προς συνεργάτη (κόστος)");
        ImGui::Spacing();
        SectionTitle("1. Τι χρεώνουμε");
        ImGui::InputText("Οφειλόμενο ποσό", pb_due.data, sizeof(pb_due.data));
        bool amountsOk = MoneyFieldHint(pb_due) && pb_userId > 0 && pb_serviceId > 0;
        if (!g_paymentDialogIsEdit && pb_direction == 0) {
            // matching offers: claim one (the amount becomes list price - discount) or ignore them
            double cur = 0.0; ParseMoney(pb_due.str(), cur);
            double list = pb_offerId > 0 ? pb_offerList : cur;
            DrawOfferClaim("pboffer", pb_userId, pb_serviceId, -1, pb_dateStr, list, pb_offerId,
                [&](double d) { pb_offerList = list; pb_due.set(FormatMoney(MoneySub(list, d))); if (!pb_isFuture) pb_paid.set(FormatMoney(MoneySub(list, d))); },
                [&]() { pb_due.set(FormatMoney(pb_offerList)); if (!pb_isFuture) pb_paid.set(FormatMoney(pb_offerList)); });
            if (pb_offerId > 0) ImGui::TextColored(kColIn, "Τιμή καταλόγου %s → με την προσφορά %s", Eur(pb_offerList).c_str(), Eur(cur).c_str());
        }
        if (!g_paymentDialogIsEdit && pb_direction == 0) SectionTitle("2. Πώς πληρώνει ο πελάτης");
        const bool planAllowed = !g_paymentDialogIsEdit && pb_direction == 0;
        if (!planAllowed) pb_plan.on = false;
        if (planAllowed) {
            double due = 0.0; ParseMoney(pb_due.str(), due);
            amountsOk = DrawPlanForm("pbplan", pb_plan, due, pb_dateStr) && amountsOk;
        }
        if (pb_plan.on) pb_isFuture = false;
        if (!pb_plan.on) ImGui::Checkbox("Μελλοντική πληρωμή", &pb_isFuture);
        if (pb_isFuture) {
            ImGui::TextDisabled("Δεν έχει πληρωθεί ακόμα -- δεν προσμετράται σε κανένα σύνολο μέχρι να επιλυθεί.");
            // Real calendar popup, not handwritten text -- and the picker itself
            // refuses to let you click today or any past day, so the "must be in
            // the future from now" rule is enforced at selection time, not after.
            ImGui::TextUnformatted("Ημ/νία επίλυσης:");
            DatePicker("resdate", pb_resolutionDate, ImVec2(0, 1), /*futureOnly=*/true);
            ImGui::TextDisabled("Πότε αναμένεται αυτή η μελλοντική πληρωμή να γίνει πραγματική.");
        } else if (!pb_plan.on) {
            ImGui::InputText("Πληρωμένο ποσό", pb_paid.data, sizeof(pb_paid.data));
            amountsOk = MoneyFieldHint(pb_paid) && amountsOk;
        }
        // Notes/comment is a normal, always-visible, optional attribute of every
        // payment -- never a substitute for the amount fields (per client correction).
        ImGui::InputTextMultiline("Σημειώσεις/Σχόλιο", pb_notes.data, sizeof(pb_notes.data), ImVec2(0, 60));
        // Receipt: asked for money coming in now; future payments get theirs when resolved, payments made on the
        // client's behalf are backed by the supplier's document instead.
        const bool askReceipt = !g_paymentDialogIsEdit && !pb_isFuture && pb_direction == 0 && !pb_plan.on;
        if (pb_plan.on) {
            double due = 0.0; ParseMoney(pb_due.str(), due);
            amountsOk = DrawPlanReceipt("pbplanrec", pb_plan, pb_receipt, FromCents(PlanAmounts(due, pb_plan.count)[0])) && amountsOk;
        } else if (askReceipt) {
            double paidNow = 0.0; ParseMoney(pb_paid.str(), paidNow);
            amountsOk = DrawReceiptForm("pbrec", pb_receipt, paidNow, 1) && amountsOk;
        } else if (g_paymentDialogIsEdit) {
            int rid = g_app.conn->getReceiptIdForPayment(g_paymentDialogId);
            Receipt rc = rid > 0 ? g_app.conn->getReceiptById(rid) : Receipt{};
            ImGui::Spacing();
            if (rc.id > 0) ImGui::TextColored(rc.status == "issued" ? ImVec4(0.55f, 0.80f, 0.55f, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1),
                                              "Απόδειξη %s-%06d · %s · %s%s", SeriesLabel(rc.series).c_str(), rc.number, ReceiptMethodName(rc.method).c_str(),
                                              rc.issue_date.c_str(), rc.status == "void" ? " · ΑΚΥΡΩΜΕΝΗ" : "  (τα ποσά είναι κλειδωμένα)");
            if (rc.id > 0) { ImGui::SameLine(); if (ImGui::SmallButton("Άνοιγμα")) OpenReceiptView(rc.id); }
            else ImGui::TextDisabled("Χωρίς απόδειξη.");
        }
        const bool costAllowed = !g_paymentDialogIsEdit && pb_direction == 0;
        if (costAllowed) {
            SectionTitle("3. Πώς πληρώνουμε τον συνεργάτη (κόστος)");
            if (pb_costSvc != pb_serviceId) {
                pb_costSvc = pb_serviceId;
                double c = pb_serviceId > 0 ? g_app.conn->getServiceCost(pb_serviceId) : 0.0;
                pb_cost.set(c > 0.005 ? FormatMoney(c) : "");
            }
            ImGui::InputText("Κόστος για εμάς", pb_cost.data, sizeof(pb_cost.data));
            double cst = 0.0, dueNow = 0.0;
            bool costBlank = pb_cost.str().find_first_not_of(" ") == std::string::npos;
            if (!costBlank) amountsOk = MoneyFieldHint(pb_cost) && amountsOk;
            if (!costBlank) ParseMoney(pb_cost.str(), cst);
            ParseMoney(pb_due.str(), dueNow);
            ImGui::SameLine(); ImGui::TextDisabled("(προτεινόμενο· αλλάζει)");
            if (cst > 0.005) {
                if (!pb_costPaidManual) pb_costPaid.set(FormatMoney(cst));
                if (ImGui::InputText("Πληρωμές από το κόστος", pb_costPaid.data, sizeof(pb_costPaid.data))) pb_costPaidManual = true;
                double cp = 0.0; bool cpOk = MoneyFieldHint(pb_costPaid) && ParseMoney(pb_costPaid.str(), cp) && ToCents(cp) <= ToCents(cst);
                if (!cpOk) { amountsOk = false; ImGui::TextColored(kColOut, "Το πληρωμένο κόστος δεν μπορεί να ξεπερνά το κόστος."); }
                else if (ToCents(cp) < ToCents(cst)) ImGui::TextColored(kColWeOwe, "Οφειλόμενα: %s", Eur(MoneySub(cst, cp)).c_str());
                ImGui::TextColored(dueNow - cst >= 0 ? kColIn : kColOut, "Κέρδος από τη χρέωση: %s", Eur(MoneySub(dueNow, cst)).c_str());
                ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Πληρώθηκε σε συνεργάτη:"); ImGui::SameLine(); PickPartner("pbpart", pb_partner, 300);
            }
        }
        ImGui::Spacing();
        if (ImGui::Button("Αποθήκευση", ImVec2(150, 0)) && amountsOk) {
            Payment np;
            np.user_id = pb_userId; np.service_id = pb_serviceId;
                        int iy = 0, im = 0, id = 0;
            std::sscanf(pb_dateStr.c_str(), "%d-%d-%d", &iy, &im, &id);
            np.year = iy; np.month = im; np.day = id;
            if (pb_isFuture) {
                ParseMoney(pb_due.str(), np.amount_due); np.amount_paid = 0.0;
                np.balance = np.amount_due;
                np.status = "future";
                np.direction = pb_direction == 1 ? "out" : "in";
            } else {
                ParseMoney(pb_due.str(), np.amount_due); ParseMoney(pb_paid.str(), np.amount_paid);
                np.balance = MoneySub(np.amount_due, np.amount_paid);
                np.status = "paid"; // resolved immediately -- same status value the Resolve flow uses
                np.direction = pb_direction == 1 ? "out" : "in";
            }
            np.notes = pb_notes.str();
            if (pb_offerId > 0 && !g_paymentDialogIsEdit) { np.offer_id = pb_offerId; np.list_price = pb_offerList; }
            np.resolution_date = np.status == "future" ? pb_resolutionDate : "";
            np.apartment_id = g_paymentDialogIsEdit ? -1 : 0;      // a charge to a client has nothing to do with an apartment (0 = none); only joint shares carry one
            double costAmt = 0.0;
            if (costAllowed && pb_cost.str().find_first_not_of(" ") != std::string::npos) ParseMoney(pb_cost.str(), costAmt);
            Payment co = np;     // our cost: a paid-out line of the same client and service
            double costPaidAmt = costAmt; if (costAmt > 0.005 && pb_costPaid.str().find_first_not_of(" ") != std::string::npos) ParseMoney(pb_costPaid.str(), costPaidAmt);
            co.direction = "out"; co.amount_due = costAmt; co.amount_paid = costPaidAmt; co.balance = MoneySub(costAmt, costPaidAmt); co.status = ToCents(costPaidAmt) >= ToCents(costAmt) ? "paid" : "pending";
            co.offer_id = -1; co.list_price = 0.0; co.resolution_date.clear(); co.notes = "Κόστος: " + g_app.serviceLabel(np.service_id);
            co.partner_id = pb_partner;
            if (g_paymentDialogIsEdit) {
                Payment old = g_app.conn->getPaymentById(g_paymentDialogId);
                np.resolved_at = old.resolved_at; // editing via the normal dialog never touches resolved_at
                std::vector<FieldDiff> diffs = {
                    {"Ποσό οφειλής", FormatMoney(old.amount_due), FormatMoney(np.amount_due)},
                    {"Πληρωμένο", FormatMoney(old.amount_paid), FormatMoney(np.amount_paid)},
                    {"Κατάσταση", old.status, np.status},
                };
                int id = g_paymentDialogId;
                RequestConfirm("Επεξεργασία πληρωμής", diffs, [id, np]() { g_app.conn->editPayment(id, np); });
            } else if (pb_plan.on) {
                PlanForm pf = pb_plan;
                std::string label = g_app.serviceLabel(np.service_id);
                auto lines = BuildPlanLines(np.user_id, {{np.service_id, np.amount_due}}, pf, pb_dateStr, np.notes, np.apartment_id);
                if (np.offer_id > 0 && !lines.empty()) {   // the discount is recorded once, on the 1st installment
                    lines[0].offer_id = np.offer_id; lines[0].list_price = lines[0].amount_due + MoneySub(np.list_price, np.amount_due);
                }
                Receipt rc = ReceiptFromForm(pb_receipt, np.user_id);
                bool withReceipt = pf.firstNow && (pf.mode == 0 || pb_receipt.issue);
                std::vector<FieldDiff> diffs = { {"Πελάτης", "-", g_app.userName(np.user_id)}, {"Υπηρεσία", "-", label},
                    {"Δόσεις", "-", std::to_string(pf.count) + (pf.every == 0 ? " μηνιαίες" : " εβδομαδιαίες") + " · " + Eur(np.amount_due)},
                    {"Απόδειξη", "-", pf.mode == 0 ? "μία, με την τελευταία δόση" : "σε κάθε δόση"} };
                if (costAmt > 0.005) diffs.push_back({"Κόστος για εμάς", "-", Eur(costAmt) + " (πληρωμές " + Eur(costPaidAmt) + ")"});
                RequestConfirm("Χρέωση σε δόσεις", diffs, [lines, label, pf, rc, withReceipt, co, costAmt]() {
                    g_app.conn->addInstallmentPlan(lines, label, pf.count, pf.every == 0 ? "month" : "week",
                                                   pf.mode == 0 ? "end" : "each", withReceipt ? &rc : nullptr);
                    if (costAmt > 0.005) g_app.conn->addPaymentsWithReceipt({co}, "", nullptr);
                });
            } else {
                std::vector<FieldDiff> diffs = { {"Χρήστης", "-", g_app.userName(np.user_id)}, {"Ποσό", "-", FormatMoney(np.amount_due)} };
                bool withReceipt = askReceipt && pb_receipt.issue;
                Receipt rc = ReceiptFromForm(pb_receipt, np.user_id);
                if (askReceipt) diffs.push_back({"Απόδειξη", "-", ReceiptSummary(pb_receipt)});
                if (costAmt > 0.005) diffs.push_back({"Κόστος για εμάς", "-", Eur(costAmt) + " (πληρωμές " + Eur(costPaidAmt) + ")"});
                diffs.push_back({"Εισπράξεις ήδη", "-", Eur(np.amount_paid) + " · οφειλές " + Eur(np.balance)});
                // charge + our cost + receipt in one transaction: all saved or none
                RequestConfirm("Νέα πληρωμή", diffs, [np, withReceipt, rc, co, costAmt]() {
                    std::vector<Payment> lines = { np };
                    if (costAmt > 0.005) lines.push_back(co);
                    g_app.conn->addPaymentsWithReceipt(lines, np.notes.empty() ? g_app.serviceLabel(np.service_id) : np.notes, withReceipt ? &rc : nullptr);
                });
            }
            g_showPaymentDialog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Ακύρωση", ImVec2(150, 0))) { g_showPaymentDialog = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------
// PACKAGE CHARGE: "charging a package" is only a shortcut that expands into
// ordinary, standalone payments -- ONE per component service, for ONE client,
// amount = that service's suggested price. Nothing links the resulting rows to the
// package afterwards (a note records the package name); each is a normal
// per-client, per-service payment. Components the client doesn't have yet are linked
// to the client's roster (buying a service never makes it a shared/joint service).
// The dialog has a checkbox per component so only the ticked ones are charged.
// ---------------------------------------------------------------------------
static bool g_showPkgChargeDialog = false, g_pkgChargeWasOpen = false;
static int pc_userId = -1, pc_pkgId = -1, pc_direction = 0, pc_checkedFor = -2;
static bool pc_isFuture = false;
static std::string pc_dateStr, pc_resolutionDate;
static std::vector<char> pc_checked;   // one checkbox per component of the selected package
static int pc_partner = -1;           // the business partner all this package's costs were paid to
static std::vector<TextBuf> pc_cost;   // what each component costs us (starts at the service's own cost)
static std::vector<TextBuf> pc_price;  // what each component is charged for (starts at the service's price)
static std::vector<int> pc_cpart; static std::vector<TextBuf> pc_cpaid; static std::vector<char> pc_cpaidManual;   // per service: the partner we pay its cost to, and how much of it is paid now
static TextBuf pc_paidNow, pc_costPaid; static bool pc_paidManual = false, pc_costPaidManual = false;   // what the client already paid / what we already paid of our cost
static bool pc_lockUser = false, pc_lockPkg = true;
static int pc_offerId = -1; static double pc_offerDisc = 0.0;
static PlanForm pc_plan;

static void OpenPackageCharge(int pkgId) {
    g_showPkgChargeDialog = true;
    pc_userId = -1; pc_lockUser = false; pc_lockPkg = pkgId > 0; pc_plan = PlanForm{}; pc_offerId = -1; pc_offerDisc = 0.0;
    pc_pkgId = pkgId; pc_checkedFor = -2; pc_partner = -1; pc_paidManual = pc_costPaidManual = false;
    pc_dateStr = CurrentDateString(); pc_resolutionDate.clear();
    pc_receipt = ReceiptForm{};
    pc_direction = 0; pc_isFuture = false;
}

// Charge a single catalog service to a client (opens the normal payment dialog with
// the service and its suggested price pre-filled). Used from the Υπηρεσίες tab.
static void OpenChargeService(int serviceId) {
    OpenAddPayment();
    pb_serviceId = serviceId;
    pb_lockService = true;   // opened from this specific service -- locked
    Service s = g_app.conn->getServiceById(serviceId);
    pb_due.set(FormatMoney(s.suggested_price));
    pb_paid.set(FormatMoney(s.suggested_price));
}

static void DrawPackageChargeDialog() {
    if (g_showPkgChargeDialog && !g_pkgChargeWasOpen) ImGui::OpenPopup("Χρέωση Πακέτου");
    g_pkgChargeWasOpen = g_showPkgChargeDialog;
    if (!g_showPkgChargeDialog) return;
    ImGui::SetNextWindowSize(ImVec2(540, 0), ImGuiCond_Appearing);
    if (BeginGuardedModal("Χρέωση Πακέτου", g_showPkgChargeDialog, ImGuiWindowFlags_AlwaysAutoResize)) {
        // The package is fixed by whichever "Χρέωση πακέτου" button opened this
        // dialog -- shown as locked text, not a combo, so only the component
        // services below are selectable. There is no way to bill a different
        // package from here without closing and clicking another package's
        // button, which keeps the action unambiguous.
        std::string pkgName = "(άγνωστο πακέτο)";
        if (pc_pkgId > 0) pkgName = g_app.conn->getPackageLabel(pc_pkgId);
        ImGui::TextUnformatted("Πακέτο:"); ImGui::SameLine(110);
        if (pc_lockPkg) ImGui::TextUnformatted(pkgName.c_str());
        else PickPackage("pcpkg", pc_pkgId, 360);
        ImGui::TextUnformatted("Πελάτης:"); ImGui::SameLine(110);
        if (pc_lockUser) ImGui::TextUnformatted(g_app.userName(pc_userId).c_str());
        else PickClient("pccli", pc_userId, 360);
        ImGui::TextUnformatted("Ημερομηνία:");
        DatePicker("pkgdate", pc_dateStr, ImVec2(0, 1));
        pc_direction = 0;       // we do not pay on behalf of clients any more
        if (!pc_plan.on) ImGui::Checkbox("Μελλοντικές πληρωμές (δεν έχουν εξοφληθεί ακόμα)", &pc_isFuture);
        if (pc_isFuture) {
            ImGui::TextUnformatted("Ημ/νία επίλυσης:");
            DatePicker("pkgres", pc_resolutionDate, ImVec2(0, 1), /*futureOnly=*/true);
        }

        // Components, each with a checkbox: tick exactly the services to charge.
        // Shared (joint) services start unticked: their price is a building total.
        auto comps = g_app.conn->getMasterServiceComponents(pc_pkgId);
        std::set<int> jointIds;
        for (auto& j : g_app.jointServices) jointIds.insert(j.service.id);
        if (pc_checkedFor != pc_pkgId || pc_checked.size() != comps.size()) {
            pc_checked.assign(comps.size(), 0);
            for (size_t i = 0; i < comps.size(); i++)
                pc_checked[i] = (!jointIds.count(comps[i].id) && comps[i].suggested_price > 0.0) ? 1 : 0;
            pc_cost.assign(comps.size(), TextBuf{}); pc_price.assign(comps.size(), TextBuf{});
            pc_cpart.assign(comps.size(), -1); pc_cpaid.assign(comps.size(), TextBuf{}); pc_cpaidManual.assign(comps.size(), 0);
            for (size_t i = 0; i < comps.size(); i++) pc_price[i].set(FormatMoney(comps[i].suggested_price));
            for (size_t i = 0; i < comps.size(); i++) {
                double c = g_app.conn->getServiceCost(comps[i].id);
                pc_cost[i].set(c > 0.005 ? FormatMoney(c) : "");
            }
            pc_checkedFor = pc_pkgId;
        }
        ImGui::Separator();
        SectionTitle("1. Τι χρεώνουμε (μία πληρωμή για κάθε υπηρεσία του πακέτου)");
        ImGui::TextDisabled("Επιλέξτε ποιες υπηρεσίες χρεώνονται, με τιμή και κόστος για εμάς (προτεινόμενα· αλλάζουν):");
        std::vector<Service> charge;
        std::vector<double> chargeCost, chargePrice, chargeCostPaid; std::vector<int> chargePartner;
        double total = 0.0, costTotal = 0.0;
        bool costsOk = true, pricesOk = true, costPaidOk = true; double costPaidTotal = 0.0;
        for (size_t i = 0; i < comps.size(); i++) {
            auto& s = comps[i];
            bool on = pc_checked[i] != 0;
            ImGui::PushID((int)i);
            std::string lbl = s.label + (jointIds.count(s.id) ? "  (κοινόχρηστη)" : "");
            if (ImGui::Checkbox(lbl.c_str(), &on)) pc_checked[i] = on ? 1 : 0;
            double cst = 0.0, prc = s.suggested_price;
            if (pc_checked[i]) {
                ImGui::SameLine(300); ImGui::SetNextItemWidth(90);
                ImGui::InputTextWithHint("##pcprice", "τιμή", pc_price[i].data, sizeof(pc_price[i].data));
                if (!ParseMoney(pc_price[i].str(), prc) || prc < 0) { pricesOk = false; prc = 0.0; }
            }
            if (pc_checked[i] && pc_direction == 0) {
                ImGui::SameLine(0, 10); ImGui::SetNextItemWidth(90);
                ImGui::InputTextWithHint("##pccost", "κόστος", pc_cost[i].data, sizeof(pc_cost[i].data));
                if (pc_cost[i].str().find_first_not_of(" ") != std::string::npos && !ParseMoney(pc_cost[i].str(), cst)) { costsOk = false; cst = 0.0; }
            }
            double cpd = 0.0;
            if (pc_checked[i] && cst > 0.005) {       // our cost of this service: which partner, how much paid now (the rest we owe)
                if (!pc_cpaidManual[i]) pc_cpaid[i].set(FormatMoney(cst));
                ImGui::Indent(24);
                ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("κόστος %s προς", Eur(cst).c_str()); ImGui::SameLine();
                PickPartner("pcrowpart", pc_cpart[i], 260);
                ImGui::SameLine(0, 10); ImGui::SetNextItemWidth(90);
                if (ImGui::InputTextWithHint("##pccpaid", "πληρωμή τώρα", pc_cpaid[i].data, sizeof(pc_cpaid[i].data))) pc_cpaidManual[i] = 1;
                if (!ParseMoney(pc_cpaid[i].str(), cpd) || ToCents(cpd) < 0 || ToCents(cpd) > ToCents(cst)) { costPaidOk = false; cpd = 0.0; }
                else if (ToCents(cpd) < ToCents(cst)) { ImGui::SameLine(); ImGui::TextColored(kColWeOwe, "οφειλή %s", Eur(MoneySub(cst, cpd)).c_str()); }
                ImGui::Unindent(24);
            }
            ImGui::PopID();
            if (pc_checked[i]) { charge.push_back(s); chargeCost.push_back(cst); chargePrice.push_back(prc); chargeCostPaid.push_back(cpd); chargePartner.push_back(pc_cpart[i]); total += prc; costTotal += cst; costPaidTotal += cpd; }
        }
        if (ImGui::SmallButton("Όλες")) std::fill(pc_checked.begin(), pc_checked.end(), 1);
        ImGui::SameLine();
        if (ImGui::SmallButton("Καμία")) std::fill(pc_checked.begin(), pc_checked.end(), 0);
        ImGui::Text("Σύνολο: %s σε %d πληρωμές", Eur(total).c_str(), (int)charge.size());
        if (costTotal > 0.005) { ImGui::SameLine(); ImGui::TextColored(total - costTotal >= 0 ? kColIn : kColOut, "· κόστος μας %s · κέρδος %s", Eur(costTotal).c_str(), Eur(MoneySub(total, costTotal)).c_str()); }
        if (!costsOk) ImGui::TextColored(kColOut, "Κάποιο κόστος δεν είναι έγκυρο ποσό.");
        if (!pricesOk) ImGui::TextColored(kColOut, "Κάποια τιμή δεν είναι έγκυρο ποσό.");
        if (!costPaidOk) ImGui::TextColored(kColOut, "Κάποια πληρωμή κόστους δεν είναι έγκυρο ποσό (0 έως το κόστος).");
        else if (costTotal > 0.005) ImGui::TextColored(kColWeOwe, "Κόστος %s · πληρωμές %s · οφειλόμενα %s", Eur(costTotal).c_str(), Eur(costPaidTotal).c_str(), Eur(MoneySub(costTotal, costPaidTotal)).c_str());
        ImGui::Spacing();
        bool ok = costPaidOk && costsOk && pricesOk && pc_userId > 0 && pc_pkgId > 0 && !charge.empty() && (!pc_isFuture || !pc_resolutionDate.empty());
        // offers for this package (or all services): the discount is spread over the ticked services by price
        if (pc_direction == 0) {
            double listTotal = total;
            DrawOfferClaim("pcoffer", pc_userId, -1, pc_pkgId, pc_dateStr, listTotal, pc_offerId,
                           [&](double d) { pc_offerDisc = d; }, [&]() { pc_offerDisc = 0.0; });
            if (pc_offerId > 0) {
                Offer tmp; for (auto& o : g_app.conn->findOffers(pc_userId, -1, pc_pkgId, pc_dateStr)) if (o.id == pc_offerId) tmp = o;
                pc_offerDisc = OfferDiscount(tmp, listTotal);       // follows the ticked services
                ImGui::TextColored(kColIn, "Σύνολο %s - έκπτωση %s = %s", Eur(listTotal).c_str(), Eur(pc_offerDisc).c_str(), Eur(MoneySub(listTotal, pc_offerDisc)).c_str());
                total = MoneySub(listTotal, pc_offerDisc);
            }
        } else pc_offerId = -1;
        double paidNow = total; bool paidOk = true;       // what the client already paid of the total (0 = he owes all of it)
        if (!pc_isFuture && !pc_plan.on) {
            SectionTitle("2. Πώς πληρώνει ο πελάτης");
            if (!pc_paidManual) pc_paidNow.set(FormatMoney(total));
            if (ImGui::InputText("Εισπράξεις ήδη (από το σύνολο)", pc_paidNow.data, sizeof(pc_paidNow.data))) pc_paidManual = true;
            paidOk = MoneyFieldHint(pc_paidNow) && ParseMoney(pc_paidNow.str(), paidNow) && ToCents(paidNow) >= 0 && ToCents(paidNow) <= ToCents(total);
            if (!paidOk) { ImGui::TextColored(kColOut, "Το ποσό που πλήρωσε δεν μπορεί να ξεπερνά το σύνολο."); paidNow = 0; ok = false; }
            else ImGui::TextColored(ToCents(paidNow) < ToCents(total) ? kColOwed : kColIn, ToCents(paidNow) < ToCents(total) ? "Οφειλές: %s" : "Εξοφλημένο", Eur(MoneySub(total, paidNow)).c_str());
        }
        if (pc_direction == 0) ok = DrawPlanForm("pcplan", pc_plan, total, pc_dateStr) && ok; else pc_plan.on = false;
        if (pc_plan.on) pc_isFuture = false;
        const bool askReceipt = !pc_isFuture && pc_direction == 0 && !pc_plan.on && ToCents(paidNow) > 0;   // money collected now -> one receipt for it
        if (pc_plan.on) ok = DrawPlanReceipt("pcplanrec", pc_plan, pc_receipt, FromCents(PlanAmounts(total, pc_plan.count)[0])) && ok;
        else if (askReceipt) ok = DrawReceiptForm("pcrec", pc_receipt, paidNow, (int)charge.size(), true) && ok;
        ImGui::BeginDisabled(!ok);
        if (ImGui::Button("Χρέωση", ImVec2(150, 0))) {
            int iy = 0, im = 0, id = 0;
            std::sscanf(pc_dateStr.c_str(), "%d-%d-%d", &iy, &im, &id);
            int uid = pc_userId; bool fut = pc_isFuture; std::string dir = pc_direction == 1 ? "out" : "in";
            std::string res = pc_resolutionDate, note = "Πακέτο: " + pkgName;
            bool withReceipt = askReceipt && pc_receipt.issue;
            Receipt rc = ReceiptFromForm(pc_receipt, uid);
            std::vector<FieldDiff> cdiffs = { {"Πελάτης", "-", g_app.userName(uid)}, {"Πακέτο", "-", pkgName},
                                              {"Πληρωμές", "-", std::to_string(charge.size()) + " / " + Eur(total)} };
            if (askReceipt) cdiffs.push_back({"Απόδειξη", "-", ReceiptSummary(pc_receipt)});
            std::vector<Payment> costLines;      // our cost, one paid-out line per charged service; what we paid fills the lines in order, the rest we owe
            for (size_t k = 0; k < charge.size(); k++) {
                if (chargeCost[k] <= 0.005) continue;
                Payment c; c.user_id = uid; c.service_id = charge[k].id; c.year = iy; c.month = im; c.day = id;
                long long cp = std::min(ToCents(chargeCostPaid[k]), ToCents(chargeCost[k]));
                c.amount_due = chargeCost[k]; c.amount_paid = FromCents(cp); c.balance = MoneySub(chargeCost[k], c.amount_paid); c.status = cp == ToCents(chargeCost[k]) ? "paid" : "pending"; c.direction = "out";
                c.notes = "Κόστος: " + pkgName; c.partner_id = chargePartner[k];
                costLines.push_back(c);
            }
            if (!costLines.empty()) cdiffs.push_back({"Κόστος για εμάς", "-", Eur(costTotal) + " (πληρωμές " + Eur(costPaidTotal) + ")"});
            if (!fut) cdiffs.push_back({"Εισπράξεις ήδη", "-", Eur(paidNow) + " · οφειλές " + Eur(MoneySub(total, paidNow))});
            // per-service discount (cents exact): the offer's discount spread by catalog price
            std::vector<double> discOf(charge.size(), 0.0);
            int offerId = pc_offerId;
            if (offerId > 0 && ToCents(pc_offerDisc) > 0) {
                std::vector<double> w = chargePrice;
                auto parts = SplitCents(ToCents(pc_offerDisc), w);
                for (size_t k = 0; k < charge.size(); k++) discOf[k] = FromCents(parts[k]);
                cdiffs.push_back({"Προσφορά", "-", "-" + Eur(pc_offerDisc)});
            }
            if (pc_plan.on) {
                PlanForm pf = pc_plan;
                std::vector<std::pair<int, double>> items;
                for (size_t k = 0; k < charge.size(); k++) items.push_back({charge[k].id, MoneySub(chargePrice[k], discOf[k])});
                auto lines = BuildPlanLines(uid, items, pf, pc_dateStr, note, -1);
                if (offerId > 0 && ToCents(pc_offerDisc) > 0 && !lines.empty()) {
                    lines[0].offer_id = offerId; lines[0].list_price = lines[0].amount_due + pc_offerDisc;
                }
                bool withPlanReceipt = pf.firstNow && (pf.mode == 0 || pc_receipt.issue);
                cdiffs.push_back({"Δόσεις", "-", std::to_string(pf.count) + (pf.every == 0 ? " μηνιαίες" : " εβδομαδιαίες")});
                cdiffs.push_back({"Απόδειξη", "-", pf.mode == 0 ? "μία, με την τελευταία δόση" : "σε κάθε δόση"});
                RequestConfirm("Χρέωση πακέτου σε δόσεις", cdiffs, [=]() {
                    g_app.conn->addInstallmentPlan(lines, pkgName, pf.count, pf.every == 0 ? "month" : "week",
                                                   pf.mode == 0 ? "end" : "each", withPlanReceipt ? &rc : nullptr);
                    if (!costLines.empty()) g_app.conn->addPaymentsWithReceipt(costLines, "Κόστος: " + pkgName, nullptr);
                    g_app.reloadAll();
                });
                g_showPkgChargeDialog = false;
                ImGui::CloseCurrentPopup();
            } else
            RequestConfirm("Χρέωση πακέτου", cdiffs,
                [=]() {
                    // one action -> one batch: all-or-nothing, and these payments may share one receipt
                    std::vector<Payment> batch;
                    long long payLeft = ToCents(paidNow);       // what the client paid fills the lines in order
                    for (size_t k = 0; k < charge.size(); k++) {
                        const Service& s = charge[k];
                        Payment p;
                        p.user_id = uid; p.service_id = s.id;
                        p.year = iy; p.month = im; p.day = id;
                        p.amount_due = MoneySub(chargePrice[k], discOf[k]);
                        if (offerId > 0 && ToCents(discOf[k]) > 0) { p.offer_id = offerId; p.list_price = chargePrice[k]; }
                        long long pk = fut ? 0 : std::min(payLeft, ToCents(p.amount_due)); payLeft -= pk;
                        p.amount_paid = FromCents(pk);
                        p.balance = MoneySub(p.amount_due, p.amount_paid);
                        p.status = fut ? "future" : (pk == ToCents(p.amount_due) ? "paid" : "pending");
                        p.direction = dir;
                        p.resolution_date = fut ? res : "";
                        p.notes = note;
                        batch.push_back(p);
                    }
                    // one batch, and (if asked) its single receipt, in one transaction -- our cost lines included
                    for (auto& c : costLines) batch.push_back(c);
                    g_app.conn->addPaymentsWithReceipt(batch, note, withReceipt ? &rc : nullptr, true);      // a receipt for each service's payment
                    g_app.reloadAll();
                });
            g_showPkgChargeDialog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Ακύρωση", ImVec2(150, 0))) { g_showPkgChargeDialog = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}


// ---------------------------------------------------------------------------
// CHARGE A CLIENT (from their profile): pick a service or a package with the search pickers; the matching dialog
// opens with the client fixed. Joint charges are not here -- they go to apartments (Κοινόχρηστα / Πολυκατοικίες).
// ---------------------------------------------------------------------------
static bool g_showClientCharge = false, g_clientChargeWasOpen = false;
static int g_ccUser = -1, g_ccKind = 0, g_ccService = -1, g_ccPkg = -1;
static int g_ccOpenNext = 0;                      // 1 service dialog, 2 package dialog (opened next frame)

static void OpenClientCharge(int userId) {
    g_showClientCharge = true; g_ccUser = userId; g_ccService = -1; g_ccPkg = -1;
}

static void DrawClientChargeDialog() {
    if (g_ccOpenNext == 1) {
        OpenAddPayment(); pb_userId = g_ccUser; pb_lockUser = true; pb_serviceId = g_ccService; pb_lockService = true;
        Service s = g_app.conn->getServiceById(g_ccService);
        pb_due.set(FormatMoney(s.suggested_price)); pb_paid.set(FormatMoney(s.suggested_price));
        g_ccOpenNext = 0;
    } else if (g_ccOpenNext == 2) {
        OpenPackageCharge(g_ccPkg); pc_userId = g_ccUser; pc_lockUser = true;
        g_ccOpenNext = 0;
    }
    const char* title = "Χρέωση πελάτη###clientcharge";
    if (g_showClientCharge && !g_clientChargeWasOpen) ImGui::OpenPopup(title);
    g_clientChargeWasOpen = g_showClientCharge;
    if (!g_showClientCharge) return;
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Appearing);
    if (BeginGuardedModal(title, g_showClientCharge, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Πελάτης: %s", g_app.userName(g_ccUser).c_str());
        ImGui::Spacing();
        ImGui::RadioButton("Υπηρεσία", &g_ccKind, 0); ImGui::SameLine(); ImGui::RadioButton("Πακέτο υπηρεσιών", &g_ccKind, 1);
        ImGui::Spacing();
        bool chosen = false;
        if (g_ccKind == 0) chosen = PickService("ccsvc", g_ccService, 460);
        else chosen = PickPackage("ccpkg", g_ccPkg, 460);
        ImGui::TextDisabled("Τα κοινόχρηστα χρεώνονται σε διαμερίσματα: Κοινόχρηστα ή Πολυκατοικίες → «Χρέωση κοινοχρήστου».");
        ImGui::Spacing();
        bool ready = g_ccKind == 0 ? g_ccService > 0 : g_ccPkg > 0;
        ImGui::BeginDisabled(!ready);
        if (ImGui::Button("Συνέχεια", ImVec2(150, 0)) || (chosen && ready)) {
            g_ccOpenNext = g_ccKind == 0 ? 1 : 2;
            g_showClientCharge = false; ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Ακύρωση", ImVec2(150, 0))) { g_showClientCharge = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------
// RESOLVE DIALOG: small, dedicated popup for turning a status='future' payment
// into a real one. Confirms/adjusts the final amount_paid; resolving always
// lands on status="paid" (the single "resolved" state in the simplified model --
// there's no more pending/overdue choice). Sets resolved_at to now and
// explicitly leaves year/month/day (the initiation date) untouched.
// ---------------------------------------------------------------------------
static bool g_showResolveDialog = false;
static bool g_resolveDialogWasOpen = false;
static int g_resolveDialogId = -1;
static TextBuf rb_paid;

static void OpenRecordPaymentFor(const Payment& p);
// «Επίλυση» of a scheduled bill is a payment like any other: the Είσπραξη / Πληρωμή dialog, where any part of what
// is left can be entered; the bill closes when it is filled.
static void OpenResolvePayment(const Payment& p) { OpenRecordPaymentFor(p); }

static void DrawResolveDialog() {
    if (g_showResolveDialog && !g_resolveDialogWasOpen) {
        ImGui::OpenPopup("Επίλυση Μελλοντικής Πληρωμής");
    }
    g_resolveDialogWasOpen = g_showResolveDialog;
    if (!g_showResolveDialog) return;
    ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_Appearing);
    if (BeginGuardedModal("Επίλυση Μελλοντικής Πληρωμής", g_showResolveDialog, ImGuiWindowFlags_AlwaysAutoResize)) {
        Payment p = g_app.conn->getPaymentById(g_resolveDialogId);
        ImGui::Text("%s / %s", g_app.userName(p.user_id).c_str(), ChargeLabel(p).c_str());
        ImGui::TextDisabled("Αρχική ημερομηνία: %04d-%02d-%02d (δεν αλλάζει)", p.year, p.month, p.day);
        if (!p.resolution_date.empty()) ImGui::TextDisabled("Ημ/νία επίλυσης: %s", p.resolution_date.c_str());
        ImGui::Separator();
        ImGui::InputText("Τελικό πληρωμένο ποσό", rb_paid.data, sizeof(rb_paid.data));
        bool paidOk = MoneyFieldHint(rb_paid);
        // an installment of a "one receipt at the end" plan: no receipt now, the plan issues it when complete
        static Cached<DataConn::PaymentPlan> planC;
        const DataConn::PaymentPlan& plan = planC.get(std::to_string(p.batch_id), [&] { return g_app.conn->getPlanForBatch(p.batch_id); });
        if (plan.id > 0) ImGui::TextColored(ImVec4(0.55f, 0.80f, 0.95f, 1), "Δόση προγράμματος «%s»: %d/%d εξοφλημένες", plan.label.c_str(),
                                            plan.paidInstallments, plan.installments);
        const bool askReceipt = p.direction == "in" && !(plan.id > 0 && plan.receipt_mode == "end");
        if (plan.id > 0 && plan.receipt_mode == "end") ImGui::TextDisabled("Η απόδειξη εκδίδεται αυτόματα μόλις εξοφληθεί η τελευταία δόση.");
        if (askReceipt) { double v = 0.0; ParseMoney(rb_paid.str(), v); paidOk = DrawReceiptForm("rbrec", rb_receipt, v, 1) && paidOk; }
        ImGui::Spacing();
        if (ImGui::Button("Επίλυση", ImVec2(150, 0)) && paidOk) {
            Payment np = p; // preserve year/month/day and everything else not explicitly changed
            ParseMoney(rb_paid.str(), np.amount_paid);
            np.balance = MoneySub(np.amount_due, np.amount_paid);
            np.status = "paid";
            np.resolved_at = CurrentTimestampString();
            int id = g_resolveDialogId;
            std::vector<FieldDiff> diffs = {
                {"Κατάσταση", p.status, np.status},
                {"Πληρωμένο", FormatMoney(p.amount_paid), FormatMoney(np.amount_paid)},
                {"Επιλύθηκε στις", "-", np.resolved_at},
            };
            bool withReceipt = askReceipt && rb_receipt.issue;
            Receipt rc = ReceiptFromForm(rb_receipt, np.user_id);
            if (askReceipt) diffs.push_back({"Απόδειξη", "-", ReceiptSummary(rb_receipt)});
            RequestConfirm("Επίλυση μελλοντικής πληρωμής", diffs, [id, np, withReceipt, rc]() {
                if (withReceipt) g_app.conn->editPaymentWithReceipt(id, np, rc);   // both or neither
                else g_app.conn->editPayment(id, np);
            });
            g_showResolveDialog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Ακύρωση", ImVec2(150, 0))) { g_showResolveDialog = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

// Opens the payment dialog to change an existing charge (from the payment's own window).
static void StartEditPayment(const Payment& p) {
    g_paymentDialogIsEdit = true; g_paymentDialogId = p.id;
    pb_userId = p.user_id; pb_serviceId = p.service_id; pb_aptId = p.apartment_id; pb_aptUser = p.user_id;
    char db[16];
    std::snprintf(db, sizeof(db), "%04d-%02d-%02d", p.year, p.month, p.day);
    pb_dateStr = db;
    pb_due.set(FormatMoney(p.amount_due)); pb_paid.set(FormatMoney(p.amount_paid));
    pb_isFuture = (p.status == "future"); pb_notes.set(p.notes); pb_resolutionDate = p.resolution_date;
    pb_direction = p.direction == "out" ? 1 : 0;
    pb_lockService = false;   // editing an existing payment -- service is editable
    g_showPaymentDialog = true;
}

static const ListColDef kPaymentListCols[12] = {
    { "ΚΩΔ",        ImGuiTableColumnFlags_WidthFixed, 100.0f, true  },
    { "ΗΜ/ΝΙΑ",     ImGuiTableColumnFlags_WidthFixed, 140.0f, true  },
    { "ΠΕΛΑΤΗΣ",    ImGuiTableColumnFlags_WidthFixed, 200.0f, true  },
    { "ΥΠΗΡΕΣΙΑ",   ImGuiTableColumnFlags_WidthFixed, 260.0f, true  },
    { "ΤΥΠΟΣ",      ImGuiTableColumnFlags_WidthFixed, 120.0f, true  },
    { "ΚΑΤΑΣΤΑΣΗ",  ImGuiTableColumnFlags_WidthFixed, 150.0f, true  },
    { "ΠΟΣΟ",       ImGuiTableColumnFlags_WidthFixed, 120.0f, true  },
    { "ΠΛΗΡΩΜΕΝΟ",  ImGuiTableColumnFlags_WidthFixed, 150.0f, true  },
    { "ΣΗΜΕΙΩΣΕΙΣ", ImGuiTableColumnFlags_WidthFixed, 190.0f, true  },
    { "ΑΠΟΔΕΙΞΗ",   ImGuiTableColumnFlags_WidthFixed, 130.0f, true  },
    { "ΕΝΕΡΓΕΙΕΣ",  ImGuiTableColumnFlags_WidthFixed, 310.0f, false },
    { "ΕΚΤΕΛΕΣΤΗΣ ΥΠΗΡΕΣΙΑΣ", ImGuiTableColumnFlags_WidthFixed, 200.0f, false },
};
static std::vector<int> g_paymentsColOrder = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 10 };
static const char* const kPaymentSortNames[] = { "ΚΩΔ", "ΗΜ/ΝΙΑ", "ΠΕΛΑΤΗΣ", "ΥΠΗΡΕΣΙΑ", "ΤΥΠΟΣ", "ΚΑΤΑΣΤΑΣΗ", "ΠΟΣΟ", "ΠΛΗΡΩΜΕΝΟ", "ΣΗΜΕΙΩΣΕΙΣ" };
static const int kPaymentSortCols[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };

static void DrawUserFilterPicker(const char* label, int& selectedUserId, float width);
static void DrawServiceFilterPicker(const char* label, int& selectedServiceId, float width);

static void DrawPaymentsTable(const std::vector<const Payment*>& rows) {
    if (!ImGui::BeginTable("paymentstable", 12,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_Hideable)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ListSetupColumns(g_paymentsColOrder, kPaymentListCols);
    ListHeaderRow(g_paymentsColOrder, kPaymentListCols, g_paymentsSort, g_paymentsPager);
    auto P = [&](int logical) { return ListPos(g_paymentsColOrder, logical); };
    // who carried out the service we paid for (partner name, or #-1): looked up for this page's paid-out lines only
    std::string outIds; for (const Payment* pp : rows) if (pp->direction == "out") outIds += (outIds.empty() ? "" : ",") + std::to_string(pp->id);
    static Cached<std::map<int, std::string>> execCache;
    const auto& execOf = execCache.get(outIds, [&] {
        std::map<int, std::string> m;
        if (outIds.empty()) return m;
        std::vector<std::string> cols; std::vector<std::vector<std::string>> rs; std::string err;
        g_app.conn->executeRawQuery("SELECT pc.charge_id, p.name FROM partner_costs pc JOIN partners p ON p.id = pc.partner_id WHERE pc.charge_id IN (" + outIds + ")", cols, rs, err);
        for (auto& r : rs) if (r.size() >= 2) m[std::atoi(r[0].c_str())] = r[1];
        return m; });
    for (const Payment* pp : rows) {
        const Payment& p = *pp;
        ImGui::PushID(p.id);
        ImGui::TableNextRow();
        ImGuiID rowId = 0;
        { char idb[16]; std::snprintf(idb, sizeof(idb), "%d", p.id); ImGui::TableSetColumnIndex(P(0)); rowId = RowSelectable(idb); }
        ImGui::TableSetColumnIndex(P(1)); ImGui::TextUnformatted(DateGr(p.year, p.month, p.day).c_str());
        ImGui::TableSetColumnIndex(P(2));
        if (p.direction == "out") ImGui::TextDisabled("—");        // what we pay has no client
        else ImGui::TextUnformatted(TruncToWidth(g_app.userName(p.user_id), ImGui::GetContentRegionAvail().x).c_str());
        ImGui::TableSetColumnIndex(P(11));
        if (p.direction == "out") { auto it = execOf.find(p.id); ImGui::TextUnformatted(it != execOf.end() ? it->second.c_str() : "#-1"); }
        else ImGui::TextDisabled("—");
        ImGui::TableSetColumnIndex(P(3)); ImGui::TextUnformatted(TruncToWidth(ChargeLabel(p), ImGui::GetContentRegionAvail().x).c_str());
        ImGui::TableSetColumnIndex(P(4)); ImGui::TextColored(DirectionColor(p.direction), "%s", DirectionBadge(p.direction));
        ImGui::TableSetColumnIndex(P(5)); StatusCell(p);
        ImGui::TableSetColumnIndex(P(6)); ImGui::TextUnformatted(Eur(p.amount_due).c_str());
        ImGui::TableSetColumnIndex(P(7)); ImGui::TextUnformatted(Eur(p.amount_paid).c_str());
        ImGui::TableSetColumnIndex(P(8)); ImGui::TextUnformatted(TruncToWidth(p.notes, ImGui::GetContentRegionAvail().x).c_str());
        ImGui::TableSetColumnIndex(P(9));
        if (p.receipt_id > 0) {
            static std::map<int, std::pair<std::string, bool>> rno;          // receipt id -> ("Α-000012", void?)
            static long long rnoVer = -1;
            if (rnoVer != g_app.conn->dataVersion()) { rno.clear(); rnoVer = g_app.conn->dataVersion(); }
            auto it = rno.find(p.receipt_id);
            if (it == rno.end()) {
                Receipt r = g_app.conn->getReceiptById(p.receipt_id);
                char b[32]; std::snprintf(b, sizeof(b), "%s-%06d", SeriesLabel(r.series).c_str(), r.number);
                it = rno.emplace(p.receipt_id, std::make_pair(std::string(b), r.status == "void")).first;
            }
            ImGui::PushStyleColor(ImGuiCol_Text, it->second.second ? ImVec4(0.60f,0.60f,0.62f,1) : kColLink);
            if (ImGui::SmallButton((it->second.first + (it->second.second ? " (ακυρ.)" : "")).c_str())) OpenReceiptView(p.receipt_id);
            ImGui::PopStyleColor();
        } else if (p.status == "future") ImGui::TextDisabled("—");
        else if (p.direction == "in" && p.amount_paid > 0.005) ImGui::TextColored(kColOwed, "χωρίς απόδειξη");
        else ImGui::TextDisabled("—");
        ImGui::TableSetColumnIndex(P(10));
        if (ImGui::SmallButton("Προβολή")) OpenPaymentView(p.id);
        if (p.status == "future") {
            ImGui::SameLine();
            if (ImGui::SmallButton("Επίλυση")) OpenResolvePayment(p);
        }
        if (RowDoubleClicked(rowId)) OpenPaymentView(p.id);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

static void DrawPaymentsScreen() {
    ImGui::TextDisabled("Προβολή πληρωμών. Για νέα χρέωση: Υπηρεσίες → Χρέωση, ή Πακέτα → Χρέωση πακέτου.");
    ImGui::SameLine();
    if (ImGui::Button("Ανανέωση")) { g_paymentsPager.Reset(); }
    if (!g_paymentsTableView) { ImGui::SameLine(); DrawSortMenu("paysort", kPaymentSortNames, kPaymentSortCols, 9, g_paymentsSort, g_paymentsPager); }

    // Κάρτες / Λίστα toggle right-aligned on the same row as the toolbar above.
    {
        float lastRight = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
        float contentRight = ImGui::GetContentRegionMax().x;
        const float toggleW = 90.0f;
        float targetX = contentRight - toggleW;
        if (targetX > lastRight + ImGui::GetStyle().ItemSpacing.x) {
            ImGui::SameLine(targetX);
        }
        if (ImGui::Button(g_paymentsTableView ? "Κάρτες" : "Λίστα", ImVec2(toggleW, 0)))
            g_paymentsTableView = !g_paymentsTableView;
    }
    // ---- filters: one framed box; each filter is a block that moves to the next line when it doesn't fit ----
    static bool missingReceiptOnly = false;
    ImGui::BeginChild("payfilters", ImVec2(0, 0), ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY);
    {
        const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        bool first = true;
        auto flow = [&](float width) {                      // SameLine only if the next block still fits
            if (!first) {
                float x = ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x * 3.0f;
                if (x + width <= right) ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x * 3.0f);
            }
            first = false;
        };
        auto label = [](const char* l) { ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%s", l); ImGui::SameLine(); };
        flow(260); ImGui::BeginGroup(); label("Από:");  DatePicker("payfilt_start", g_payStartDate, ImVec2(0, 1)); ImGui::EndGroup();
        flow(260); ImGui::BeginGroup(); label("Έως:");  DatePicker("payfilt_end", g_payEndDate, ImVec2(0, 1));   ImGui::EndGroup();
        int u = g_payUserFilter, sv = g_payServiceFilter;
        flow(290); ImGui::BeginGroup(); DrawUserFilterPicker("Πελάτης:", g_payUserFilter, 210.0f); ImGui::EndGroup();
        flow(300); ImGui::BeginGroup(); DrawServiceFilterPicker("Υπηρεσία:", g_payServiceFilter, 210.0f); ImGui::EndGroup();
        if (u != g_payUserFilter || sv != g_payServiceFilter) g_paymentsPager.Reset();
        flow(230); ImGui::BeginGroup(); label("Κατάσταση:"); ImGui::SetNextItemWidth(130);
        ImGui::Combo("##filt_status", &g_payStatusFilter, kStatusNames, 3); ImGui::EndGroup();
        flow(230); ImGui::BeginGroup(); label("Τύπος:"); ImGui::SetNextItemWidth(160);
        ImGui::Combo("##filt_direction", &g_payDirectionFilter, kDirectionFilterNames, 3); ImGui::EndGroup();
        flow(200); ImGui::Checkbox("Μόνο χωρίς απόδειξη", &missingReceiptOnly);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Εισπράξεις για τις οποίες δεν έχει εκδοθεί απόδειξη (ή η απόδειξή τους ακυρώθηκε).");
        bool any = !g_payStartDate.empty() || !g_payEndDate.empty() || g_payUserFilter > 0 || g_payServiceFilter > 0 ||
                   g_payStatusFilter != 0 || g_payDirectionFilter != 0 || missingReceiptOnly;
        flow(170);
        ImGui::BeginDisabled(!any);
        if (ImGui::Button("Καθαρισμός φίλτρων")) {
            g_payStartDate.clear(); g_payEndDate.clear(); g_payUserFilter = -1; g_payServiceFilter = -1;
            g_payStatusFilter = 0; g_payDirectionFilter = 0; missingReceiptOnly = false;
        }
        ImGui::EndDisabled();
    }
    ImGui::EndChild();

    // --- Detect filter changes -> reset pager ---
    static std::string lastStart, lastEnd;
    static int lastUser = -2, lastSvc = -2, lastStat = -1, lastDir = -1;
    static bool lastMissing = false;
    if (lastStart != g_payStartDate || lastEnd != g_payEndDate ||
        lastUser != g_payUserFilter || lastSvc != g_payServiceFilter ||
        lastStat != g_payStatusFilter || lastDir != g_payDirectionFilter || lastMissing != missingReceiptOnly) {
        lastMissing = missingReceiptOnly;
        g_paymentsPager.Reset();
        lastStart = g_payStartDate; lastEnd = g_payEndDate;
        lastUser = g_payUserFilter; lastSvc = g_payServiceFilter;
        lastStat = g_payStatusFilter; lastDir = g_payDirectionFilter;
    }

    // Translate the combo indexes into the string form the SQL layer expects.
    std::string statusStr = (g_payStatusFilter == 1) ? "future" :
                            (g_payStatusFilter == 2) ? "resolved" : "";
    std::string dirStr    = (g_payDirectionFilter == 1) ? "in" :
                            (g_payDirectionFilter == 2) ? "out" : "";

    // --- Query one page from SQL (cached until data, filters, page or sort change) ---
    std::string sortKey;
    for (auto& k : g_paymentsSort.keys) sortKey += std::to_string(k.first) + (k.second ? "a" : "d");
    char pk[256];
    std::snprintf(pk, sizeof(pk), "%s|%s|%d|%d|%s|%s|%d|%d|%d|%s", g_payStartDate.c_str(), g_payEndDate.c_str(), g_payUserFilter,
                  g_payServiceFilter, statusStr.c_str(), dirStr.c_str(), g_paymentsPager.offset, g_paymentsPager.pageSize,
                  (int)missingReceiptOnly, sortKey.c_str());
    static Cached<std::pair<std::vector<Payment>, int>> pageCache;
    const auto& pc = pageCache.get(pk, [&] {
        return std::make_pair(
            g_app.conn->pagePayments(g_payStartDate, g_payEndDate, g_payUserFilter, g_payServiceFilter, statusStr, dirStr,
                                     g_paymentsPager.offset, g_paymentsPager.pageSize, g_paymentsSort.keys, missingReceiptOnly),
            g_app.conn->countPayments(g_payStartDate, g_payEndDate, g_payUserFilter, g_payServiceFilter, statusStr, dirStr,
                                      missingReceiptOnly));
    });
    const std::vector<Payment>& page = pc.first;
    g_paymentsPager.total = pc.second;

    std::vector<const Payment*> payRows;
    payRows.reserve(page.size());
    for (auto& p : page) payRows.push_back(&p);

    float pagerH = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y * 3.0f;
    ImGui::BeginChild("paycards", ImVec2(0, -pagerH), false);
    if (g_paymentsTableView) { DrawPaymentsTable(payRows); ImGui::EndChild(); DrawPager(g_paymentsPager, "paypager"); return; }
    const float cardW = 260;
    for (const Payment* pp : payRows) {
        const Payment& p = *pp;
        ImVec4 col = StatusColor(p.status);
        ImGui::PushID(p.id);
        // Card height computed from what's actually drawn inside, not a hardcoded
        // magic number -- this is what broke last time (Επίλυση button added later
        // without bumping a fixed height). Lines: badge, user/service, date, status,
        // amount, optional notes, then the button row (always drawn, may include the
        // conditional "Επίλυση" button for future payments) plus card padding.
        bool showResDate = (p.status == "future" && !p.resolution_date.empty());
        bool showResolvedAt = !p.resolved_at.empty();
        float lineH = ImGui::GetTextLineHeightWithSpacing();
        int textLines = 5 + (p.notes.empty() ? 0 : 1) + (showResDate ? 1 : 0) + (showResolvedAt ? 1 : 0);
        float cardH = textLines * lineH + 30.0f /*button row*/ + 16.0f /*padding*/;
        BeginCard("card", ImVec2(cardW, cardH));
        ImGui::TextColored(DirectionColor(p.direction), "%s", DirectionBadge(p.direction));
        ImGui::Text("%s / %s", g_app.userName(p.user_id).c_str(), ChargeLabel(p).c_str());
        ImGui::TextUnformatted(DateGr(p.year, p.month, p.day).c_str());
        ImGui::TextColored(col, "%s", p.status.c_str());
        ImGui::Text("Οφειλή: %.2f  Πληρωμένο: %.2f", p.amount_due, p.amount_paid);
        if (showResDate) ImGui::TextDisabled("Ημ/νία επίλυσης: %s", p.resolution_date.c_str());
        if (showResolvedAt) ImGui::TextDisabled("Επιλύθηκε: %s", p.resolved_at.c_str());
        if (!p.notes.empty()) ImGui::TextWrapped("%s", p.notes.c_str());
        if (ImGui::SmallButton("Προβολή")) OpenPaymentView(p.id);
        if (p.status == "future") {
            ImGui::SameLine();
            if (ImGui::SmallButton("Επίλυση")) OpenResolvePayment(p);
        }
        EndCard();
        ImGui::PopID();
        CardWrapNext(cardW);
    }
    ImGui::EndChild();

    DrawPager(g_paymentsPager, "paypager");
}

// ---------------------------------------------------------------------------
// DRILL-DOWN WINDOWS: User Profile -> Service Detail -> Payment Viewer.
// Bare minimum per spec: the Payment Viewer is a dead end, nowhere further to go.
// ---------------------------------------------------------------------------

// Opens the Add-Payment dialog pre-bound to a specific service: if that service
// has exactly one owner, the owner is filled in silently (no extra prompt,
// matching how single-owner services already behave elsewhere); if it has more
// than one (a joint service), a small picker asks which owner first.
static bool g_showPayOwnerPick = false;
static bool g_payOwnerPickWasOpen = false;
static int g_payPickServiceId = -1;

static void DrawPayOwnerPickPopup() {
    if (g_showPayOwnerPick && !g_payOwnerPickWasOpen) {
        ImGui::OpenPopup("Για ποιον ιδιοκτήτη είναι η πληρωμή;");
    }
    g_payOwnerPickWasOpen = g_showPayOwnerPick;
    if (!g_showPayOwnerPick) return;
    ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_Appearing);
    if (BeginGuardedModal("Για ποιον ιδιοκτήτη είναι η πληρωμή;", g_showPayOwnerPick, ImGuiWindowFlags_AlwaysAutoResize)) {
        auto roster = g_app.conn->getUsersForService(g_payPickServiceId);
        for (auto& u : roster) {
            if (ImGui::Selectable(u.full_name.c_str())) {
                OpenAddPayment();
                pb_serviceId = g_payPickServiceId;
                pb_lockService = true;
                pb_userId = u.id;
                g_showPayOwnerPick = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::Spacing();
        if (ImGui::Button("Ακύρωση")) { g_showPayOwnerPick = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

// User Profile. Layout, top to bottom:
//   1. Top-right "Κλείσιμο" button (the window's own X has been removed --
//      the client explicitly disliked it -- and NoCollapse removes the
//      collapse triangle).
//   2. Four stat tiles: Εισπράχθηκαν / Πληρώθηκαν / Οφείλονται / Προς πληρωμή.
//   3. Attributes section (border-boxed), with an inline "Επεξεργασία" button
//      that reuses the existing edit-user dialog.
//   4. + Νέα Υπηρεσία / + Νέα Πληρωμή buttons.
//   5. Two columns: services list on the left; for whichever service is
//      currently hovered (or was last hovered), that service's payments on
//      the right. Click a service -> open Service Detail; click a payment ->
//      open Payment Viewer.
//
// Hovered service is persisted per-user in g_profileHoveredSvc so the right
// panel does not blank out the moment the mouse crosses the divider to click
// a payment row.
// Forward declaration: StatTile is defined further down (dashboard section),
// but DrawUserProfileWindow (below) uses it. Declaring the signature here lets
// the profile function compile without moving the whole tile helper up.
// Small stat tile used across the top of the dashboard and profile windows.
// Big bold value, small label on top, optional subtitle. Both the label and
// the value auto-shrink to fit the tile's width, so a long label like
// "Οφείλονται να εισπραχθούν" or a wide value like "12345.67 €" never
// overflows when the containing window gets narrow -- the tile stays legible
// at every size, React-flex-style.
static void StatTile(const char* label, const std::string& value, ImVec4 accent, const char* sub, ImVec2 size) {
    BeginCard(label, size);
    float availW = ImGui::GetContentRegionAvail().x;

    // Label: shrink font if it would overflow.
    {
        float w = ImGui::CalcTextSize(label).x;
        float scale = (w > availW && w > 0.0f) ? (availW / w) : 1.0f;
        if (scale < 1.0f) ImGui::SetWindowFontScale(scale);
        ImGui::TextDisabled("%s", label);
        if (scale < 1.0f) ImGui::SetWindowFontScale(1.0f);
    }

    // Value: preferred large scale, shrunk if needed.
    {
        ImGui::PushStyleColor(ImGuiCol_Text, accent);
        float w = ImGui::CalcTextSize(value.c_str()).x;
        float scale = 1.35f;
        if (w * scale > availW && w > 0.0f) scale = availW / w;
        ImGui::SetWindowFontScale(scale);
        ImGui::TextUnformatted(value.c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::PopStyleColor();
    }

    if (sub && sub[0]) ImGui::TextDisabled("%s", sub);
    EndCard();
}


static long long DaysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}
static std::string IsoFromDays(long long z) {
    z += 719468;
    long long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = (long long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    char b[16]; std::snprintf(b, sizeof(b), "%04d-%02u-%02u", (int)y, m, d);
    return b;
}
// A date range filter: from / to (both empty = all dates) and shortcuts for this week / month / year.
static bool DrawDateRange(const char* id, std::string& from, std::string& to) {
    bool ch = false;
    ImGui::PushID(id);
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Από:"); ImGui::SameLine();
    if (DatePicker("from", from, ImVec2(0, 1))) ch = true;
    ImGui::SameLine(); ImGui::TextUnformatted("Έως:"); ImGui::SameLine();
    if (DatePicker("to", to, ImVec2(0, 1))) ch = true;
    std::string today = CurrentDateString();
    int y = 0, m = 0, d = 0; std::sscanf(today.c_str(), "%d-%d-%d", &y, &m, &d);
    long long t0 = DaysFromCivil(y, m, d);
    ImGui::SameLine(0, 16); if (ImGui::SmallButton("Αυτή την εβδομάδα")) { long long mon = t0 - (((t0 % 7) + 7 + 3) % 7); from = IsoFromDays(mon); to = IsoFromDays(mon + 6); ch = true; }
    ImGui::SameLine(); if (ImGui::SmallButton("Αυτόν τον μήνα")) {
        char b[16]; std::snprintf(b, sizeof(b), "%04d-%02d-01", y, m); from = b;
        to = IsoFromDays(DaysFromCivil(m == 12 ? y + 1 : y, m == 12 ? 1 : m + 1, 1) - 1); ch = true; }
    ImGui::SameLine(); if (ImGui::SmallButton("Φέτος")) { from = std::to_string(y) + "-01-01"; to = std::to_string(y) + "-12-31"; ch = true; }
    if (!from.empty() || !to.empty()) { ImGui::SameLine(); if (ImGui::SmallButton("×")) { from.clear(); to.clear(); ch = true; } if (ImGui::IsItemHovered()) ImGui::SetTooltip("Όλες οι ημερομηνίες"); }
    else { ImGui::SameLine(); ImGui::TextDisabled("(όλες οι ημερομηνίες)"); }
    ImGui::PopID();
    return ch;
}

static std::map<int, int> g_profileHoveredSvc; // uid -> currently-shown service id

static std::string OfferValueText(const Offer& o);
static std::string OfferTargetText(const Offer& o);
static std::string OfferStateText(const Offer& o, ImVec4& col);

struct ProfileState {
    std::string sFrom, sTo;                         // the statistics' dates
    std::string from, to;                           // the services table's dates
    std::string jfrom, jto;                         // the joint shares table's own dates
    Pager owed, owedJ, offers;
    int selCharge = -1;                             // the service line whose payments are shown on the right
    ListModel<DataConn::ClientChargeRow> charges;
    ListModel<JointShare> shares;
    QualityView quality;
};
static std::map<int, ProfileState> g_profileState;

static std::string PayFilterKey(const DataConn::PaymentFilter& f) {
    return f.fromDate + "|" + f.toDate + "|" + std::to_string(f.userId) + "|" + std::to_string(f.serviceId) + "|" + std::to_string(f.owed) +
           "|" + std::to_string(f.joint) + "|" + std::to_string(f.jointOpId);
}

// One page of payments for a filter (SQL count + page), newest first.
static void ProfilePaymentsTable(const char* id, const DataConn::PaymentFilter& f, Pager& pg, bool showService) {
    if (pg.pageSize == 50) pg.pageSize = 8;
    struct Pg { std::vector<Payment> rows; long long count = 0; };
    static std::map<std::string, Cached<Pg>> caches;
    const Pg& p = caches[id].get(PayFilterKey(f) + "#" + std::to_string(pg.offset) + "/" + std::to_string(pg.pageSize), [&] {
        Pg r; r.count = g_app.conn->countPaymentsWhere(f); r.rows = g_app.conn->findPayments(f, DataConn::PayOrder::DateDesc, pg.offset, pg.pageSize); return r; });
    pg.total = (int)p.count;
    if (p.rows.empty()) { ImGui::TextDisabled("Καμία πληρωμή."); return; }
    if (ImGui::BeginTable(id, showService ? 5 : 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Ημ/νία", ImGuiTableColumnFlags_WidthFixed, 86.0f);
        if (showService) ImGui::TableSetupColumn("Για", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Ποσό", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(showService ? "Υπόλοιπο" : "Πληρώθηκε", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Κατάσταση", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableHeadersRow();
        for (auto& r : p.rows) {
            ImGui::PushID(r.id); ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(DateGr(r.year, r.month, r.day).c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) OpenPaymentView(r.id);
            if (showService) { ImGui::TableNextColumn(); ImGui::TextUnformatted(ChargeLabel(r).c_str()); }
            ImGui::TableNextColumn(); ImGui::TextColored(DirectionColor(r.direction), "%s", Eur(r.amount_due).c_str());
            ImGui::TableNextColumn();
            if (showService) ImGui::TextColored(kColOwed, "%s", Eur(r.balance).c_str()); else ImGui::Text("%s", Eur(r.amount_paid).c_str());
            ImGui::TableNextColumn(); StatusCell(r);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    DrawPager(pg, (std::string(id) + "_pg").c_str());
}

// Shares of joint charges as a model list: a client's (byApt = false) or an apartment's (byApt = true). onlyOpen = what is still
// owed by the resident or by us; completed ones are left out (they are in the history list).
enum { SH_DATE, SH_OP, SH_WHO, SH_TOTAL, SH_SHARE, SH_PAID, SH_OWED, SH_WEOWE, SH_ACT, SH_COUNT };
static const ListColDef kShareColsClient[SH_COUNT] = {
    { "Ημ/νία", ImGuiTableColumnFlags_WidthFixed, 100.0f, true },        { "Κοινόχρηστο", ImGuiTableColumnFlags_WidthFixed, 230.0f, true },
    { "Πολυκατοικία - διαμέρισμα", ImGuiTableColumnFlags_WidthFixed, 230.0f, false }, { "Σύνολο κτιρίου", ImGuiTableColumnFlags_WidthFixed, 120.0f, true },
    { "Μερίδιο", ImGuiTableColumnFlags_WidthFixed, 100.0f, true },       { "Πληρωμένο", ImGuiTableColumnFlags_WidthFixed, 150.0f, true },
    { "Οφειλές", ImGuiTableColumnFlags_WidthFixed, 160.0f, true },       { "Οφειλόμενα", ImGuiTableColumnFlags_WidthFixed, 110.0f, false },       { "", ImGuiTableColumnFlags_WidthFixed, 80.0f, false } };
static const ListColDef kShareColsApt[SH_COUNT] = {
    { "Ημ/νία", ImGuiTableColumnFlags_WidthFixed, 100.0f, true },        { "Κοινόχρηστο", ImGuiTableColumnFlags_WidthFixed, 230.0f, true },
    { "Ένοικος τότε", ImGuiTableColumnFlags_WidthFixed, 200.0f, true },  { "Σύνολο κτιρίου", ImGuiTableColumnFlags_WidthFixed, 120.0f, true },
    { "Μερίδιο", ImGuiTableColumnFlags_WidthFixed, 100.0f, true },       { "Πληρωμένο", ImGuiTableColumnFlags_WidthFixed, 150.0f, true },
    { "Οφειλές", ImGuiTableColumnFlags_WidthFixed, 160.0f, true },       { "Οφειλόμενα", ImGuiTableColumnFlags_WidthFixed, 110.0f, false },       { "", ImGuiTableColumnFlags_WidthFixed, 80.0f, false } };
static void DrawSharesTable(const char* id, ListModel<JointShare>& m, int userId, int aptId, const std::string& from, const std::string& to, bool onlyOpen, float height) {
    const bool byApt = aptId > 0;
    const ListColDef* cols = byApt ? kShareColsApt : kShareColsClient;
    m.Init(SH_COUNT, SortSpec{{SH_DATE, false}}, 10);
    const auto& pg = m.Load(std::string(onlyOpen ? "o" : "h") + "|" + std::to_string(userId) + "|" + std::to_string(aptId) + "|" + from + "|" + to,
        [&](const SortSpec& s, int off, int lim) {
            return g_app.conn->pageShares(userId, aptId, from, to, onlyOpen, MapSort(s, {{SH_DATE, 0}, {SH_OP, 1}, {SH_WHO, 2}, {SH_TOTAL, 3}, {SH_SHARE, 4}, {SH_PAID, 5}, {SH_OWED, 6}}), off, lim); },
        [&] { return g_app.conn->countShares(userId, aptId, from, to, onlyOpen); });
    if (pg.count == 0) { ImGui::TextDisabled(onlyOpen ? "Δεν εκκρεμεί τίποτα." : "Κανένα κοινόχρηστο."); return; }
    if (ImGui::BeginTable(id, SH_COUNT, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable,
                          ImVec2(0, height))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ListSetupColumns(m.g.order, cols);
        ListHeaderRow(m.g.order, cols, m.g.sort, m.g.pager);
        auto P = [&](int c) { return ListPos(m.g.order, c); };
        for (auto& s : pg.rows) {
            double owed = MoneySub(s.amount, std::min(s.paid, s.amount));
            ImGui::PushID(s.id); ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(P(SH_DATE)); ImGui::TextUnformatted(DateGr(s.op_date).c_str());
            ImGui::TableSetColumnIndex(P(SH_OP)); ImGui::TextUnformatted(s.op_label.c_str());
            ImGui::TableSetColumnIndex(P(SH_WHO));
            if (byApt) {
                if (s.user_id <= 0) ImGui::TextDisabled("— κενό —");
                else {
                    // the resident AT THE CHARGE: say so when he no longer lives here (the debt stays his)
                    static std::map<int, Cached<std::set<int>>> nowC;
                    const auto& now = nowC[aptId].get("", [&] { std::set<int> r; for (auto& x : g_app.conn->getResidencies(aptId)) if (x.to_date.empty()) r.insert(x.user_id); return r; });
                    bool past = now.count(s.user_id) == 0;
                    if (LinkCell(s.user_name + (past ? "  (πρώην ένοικος)" : ""))) OpenUserProfile(s.user_id);
                    if (past && ImGui::IsItemHovered()) ImGui::SetTooltip("Έμενε εδώ όταν χρεώθηκε το κοινόχρηστο και δεν μένει πια.\nΤο μερίδιο το οφείλει εκείνος/εκείνη.");
                }
            }
            else ImGui::Text("%s, %s", s.building_address.c_str(), s.apartment_label.c_str());
            ImGui::TableSetColumnIndex(P(SH_TOTAL)); ImGui::TextDisabled("%s", Eur(s.op_total).c_str());
            ImGui::TableSetColumnIndex(P(SH_SHARE)); ImGui::TextUnformatted(Eur(s.amount).c_str());
            ImGui::TableSetColumnIndex(P(SH_PAID));
            { float fr = s.amount > 0.005 ? (float)std::min(1.0, s.paid / s.amount) : 1.0f; char b[40]; std::snprintf(b, sizeof(b), "%s (%.0f%%)", Eur(std::min(s.paid, s.amount)).c_str(), fr * 100.0f);
              ImGui::PushStyleColor(ImGuiCol_PlotHistogram, fr >= 0.9999f ? ImVec4(0.30f, 0.70f, 0.45f, 1) : ImVec4(0.85f, 0.60f, 0.25f, 1));
              ImGui::ProgressBar(fr, ImVec2(-1, 0), b); ImGui::PopStyleColor(); }
            ImGui::TableSetColumnIndex(P(SH_OWED));
            if (owed > 0.005) ImGui::TextColored(kColOwed, "%s", Eur(owed).c_str()); else ImGui::TextDisabled("—");
            ImGui::TableSetColumnIndex(P(SH_WEOWE));
            if (s.op_cost_owed > 0.005) ImGui::TextColored(kColWeOwe, "%s", Eur(s.op_cost_owed).c_str()); else ImGui::TextDisabled("%s", Eur(0.0).c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Το κόστος αυτού του κοινοχρήστου που παραμένει οφειλή σε συνεργάτες");
            ImGui::TableSetColumnIndex(P(SH_ACT)); if (ImGui::SmallButton("Άνοιγμα")) OpenJointView(s.op_id);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    DrawPager(m.g.pager, (std::string(id) + "_pg").c_str());
}

// A client's own services, one charge per line: what they were charged and paid, what it cost us and what we paid for it.
enum { CC_DATE, CC_SVC, CC_DUE, CC_PAID, CC_OWED, CC_COST, CC_COSTPAID, CC_PROFIT, CC_COUNT };
static const ListColDef kClientChargeCols[CC_COUNT] = {
    { "Ημ/νία", ImGuiTableColumnFlags_WidthFixed, 100.0f, true },       { "Υπηρεσία", ImGuiTableColumnFlags_WidthFixed, 260.0f, true },
    { "Χρεώθηκε", ImGuiTableColumnFlags_WidthFixed, 100.0f, true },     { "Εισπράξεις", ImGuiTableColumnFlags_WidthFixed, 100.0f, true },
    { "Οφειλές", ImGuiTableColumnFlags_WidthFixed, 100.0f, true },      { "Κόστος", ImGuiTableColumnFlags_WidthFixed, 100.0f, true },
    { "Πληρωμές", ImGuiTableColumnFlags_WidthFixed, 100.0f, true },    { "Κέρδος", ImGuiTableColumnFlags_WidthFixed, 100.0f, true } };

static void DrawUserProfileWindow() {
    std::vector<int> ids = g_profileUserIds;
    for (size_t i = 0; i < ids.size(); ++i) {
        int uid = ids[i];
        User u = g_app.conn->getUserById(uid);
        bool closeThis = false;

        ImGuiIO& io = ImGui::GetIO();
        float cascade = (float)i * 26.0f;

        // React-style: default size is proportional to the display, capped at
        // a comfortable max so it doesn't dominate a 4K screen. Min size is
        // enforced so the user can't shrink it into an unusable sliver.
        float defW = std::min(940.0f, io.DisplaySize.x * 0.85f);
        float defH = std::min(660.0f, io.DisplaySize.y * 0.85f);
        defW = std::max(defW, 500.0f);
        defH = std::max(defH, 400.0f);

        std::string title = "Προφίλ: " + u.full_name + "###profile_" + std::to_string(uid);
        ImGui::SetNextWindowSize(ImVec2(defW, defH), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(500, 400), ImVec2(FLT_MAX, FLT_MAX));
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - defW - 60.0f - cascade, 60.0f + cascade),
                                ImGuiCond_FirstUseEver);
        bool winOpen = true;
        ImGui::Begin(title.c_str(), &winOpen, ImGuiWindowFlags_NoCollapse);      // closed with the x of the title bar
        if (!winOpen) closeThis = true;
        if (ImGui::Button("+ Χρέωση υπηρεσίας / πακέτου")) OpenClientCharge(u.id);
        ImGui::SameLine();
        if (ImGui::Button("+ Νέα είσπραξη")) OpenRecordPayment(u.id);
        { double credit = g_app.conn->getClientCredit(u.id);
          if (ToCents(credit) > 0) { ImGui::SameLine(); ImGui::TextColored(ImVec4(0.55f, 0.80f, 0.95f, 1), "Πίστωση: %s", Eur(credit).c_str()); } }
        // --- Attributes box: auto-height so every field is visible at once,
        //     no scrollbar ever. The whole block sizes itself to its content.
        ImGui::BeginChild("userattrs", ImVec2(0, 0),
            ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY);
        {
            ImGui::TextColored(ImVec4(0.70f,0.85f,1.0f,1.0f), "Στοιχεία Πελάτη");
            {
                const char* editLbl = "Επεξεργασία";
                float editW = ImGui::CalcTextSize(editLbl).x + ImGui::GetStyle().FramePadding.x * 2.0f;
                float tX = ImGui::GetWindowContentRegionMax().x - editW;
                if (tX > ImGui::GetCursorPosX()) ImGui::SameLine(tX);
                if (ImGui::Button(editLbl)) OpenEditUser(u);
            }
            ImGui::Separator();
            if (ImGui::BeginTable("attrsgrid", 2, ImGuiTableFlags_SizingStretchSame)) {
                auto Row = [](const char* k1, const std::string& v1,
                              const char* k2, const std::string& v2) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("%s:", k1); ImGui::SameLine();
                    ImGui::TextUnformatted(v1.empty() ? "—" : v1.c_str());
                    ImGui::TableNextColumn();
                    if (k2 && k2[0]) {
                        ImGui::TextDisabled("%s:", k2); ImGui::SameLine();
                        ImGui::TextUnformatted(v2.empty() ? "—" : v2.c_str());
                    }
                };
                Row("Τηλέφωνο",        u.phone,         "Περιοχή",         u.area);
                Row("Διεύθυνση",       u.address,       "Διαμέρισμα",      u.apartment);
                Row("Τ.Κ.",            u.postal_code,   "Ειδικός Κωδικός", u.special_code);
                Row("Δημιουργήθηκε",   u.created_at,    "",                "");
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();


        // 2. attached files
        if (ImGui::CollapsingHeader("Συνημμένα πελάτη (συμβόλαια, ταυτότητα, φωτογραφίες)")) DrawAttachmentsPanel("client", u.id);

        ProfileState& PS = g_profileState[uid];
        PS.charges.Init(CC_COUNT, {{CC_DATE, false}}, 10);
        PS.shares.Init(SH_COUNT, {{SH_DATE, false}}, 10);
        if (PS.quality.g.order.empty()) PS.quality.blkJ = u.apartment_id > 0;      // not a resident: no joint block to show
        const std::string suid = std::to_string(uid);

        // 3. statistics for a date range (all dates by default)
        SectionTitle("Στατιστικά");
        DrawDateRange(("pst" + suid).c_str(), PS.sFrom, PS.sTo);
        {
            DataConn::PaymentFilter fa; fa.userId = uid; fa.fromDate = PS.sFrom; fa.toDate = PS.sTo;
            static std::map<int, Cached<DataConn::MoneyTotals>> tc;
            const auto& tt = tc[uid].get(PS.sFrom + "|" + PS.sTo, [&] { return g_app.conn->paymentTotals(fa); });
            char buf[64];
            if (ImGui::BeginTable("userstats", 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableNextRow();
                struct Tl { const char* l; double v; ImVec4 c; };
                Tl tl[4] = { {"Εισπράξεις", tt.collected, kColIn}, {"Οφειλές", tt.owedIn, kColOwed},
                             {"Πληρωμές", tt.paidOut, kColOut}, {"Οφειλόμενα", tt.owedOut, kColWeOwe} };
                for (int k = 0; k < 4; k++) { ImGui::TableNextColumn(); ImGui::PushID(k);
                    std::snprintf(buf, sizeof(buf), "%s", Eur(tl[k].v).c_str());
                    StatTile(tl[k].l, buf, tl[k].c, nullptr, ImVec2(ImGui::GetContentRegionAvail().x, 76)); ImGui::PopID(); }
                ImGui::EndTable();
            }
        }

        // 4. what is owed right now, split: his own services / his parts of joint charges
        SectionTitle("Τρέχουσες οφειλές");
        auto owedBlock = [&](const char* title, int jointMode, Pager& pg, const char* tid) {
            DataConn::PaymentFilter fo; fo.userId = uid; fo.owed = 2; fo.joint = jointMode;
            static std::map<std::string, Cached<DataConn::MoneyTotals>> oc;
            const auto& ot = oc[std::string(tid) + suid].get("", [&] { return g_app.conn->paymentTotals(fo); });
            if (ot.count == 0) { ImGui::TextColored(kColIn, "%s: δεν οφείλει τίποτα.", title); return; }
            char h[200]; std::snprintf(h, sizeof(h), "%s · %lld χρεώσεις · %s", title, ot.count, Eur(FromCents(ToCents(ot.owedIn) + ToCents(ot.owedOut))).c_str());
            ImGui::TextColored(kColOwed, "%s", h);
            ProfilePaymentsTable(tid, fo, pg, true);
        };
        owedBlock("Υπηρεσίες του", 2, PS.owed, "profowed");
        owedBlock("Κοινόχρηστα (μερίδια)", 1, PS.owedJ, "profowedj");

        // 5. offers and discounts that apply to him right now
        {
            static std::map<int, Cached<std::vector<Offer>>> ofc;
            const auto& offers = ofc[uid].get(CurrentDateString(), [&] { return g_app.conn->getClientOffers(uid, CurrentDateString()); });
            SectionTitle(("Τρέχουσες προσφορές (" + std::to_string(offers.size()) + ")").c_str());
            if (offers.empty()) ImGui::TextDisabled("Καμία προσφορά αυτή τη στιγμή.");
            else {
                Pager& op = PS.offers; if (op.pageSize == 50) op.pageSize = 5;
                op.total = (int)offers.size(); if (op.offset >= op.total) op.Reset();
                if (ImGui::BeginTable("profoffers", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                    ImGui::TableSetupColumn("Προσφορά", ImGuiTableColumnFlags_WidthStretch, 1.6f);
                    ImGui::TableSetupColumn("Έκπτωση", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                    ImGui::TableSetupColumn("Για", ImGuiTableColumnFlags_WidthStretch, 1.4f);
                    ImGui::TableSetupColumn("Κατάσταση", ImGuiTableColumnFlags_WidthStretch, 1.4f);
                    ImGui::TableHeadersRow();
                    for (int i = op.offset; i < std::min(op.total, op.offset + op.pageSize); i++) {
                        const Offer& o = offers[i]; ImGui::TableNextRow();
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(o.name.c_str());
                        if (o.user_id < 0) { ImGui::SameLine(); ImGui::TextDisabled("(για όλους)"); }
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(OfferValueText(o).c_str());
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(OfferTargetText(o).c_str());
                        ImGui::TableNextColumn(); ImVec4 c; std::string st = OfferStateText(o, c); ImGui::TextColored(c, "%s", st.c_str());
                    }
                    ImGui::EndTable();
                }
                DrawPager(op, "profofpg");
            }
        }

        // 6. his rating, as in the Προσφορές page: only his row, for a date range
        SectionTitle("Βαθμολογία πληρωμών (όπως στις Προσφορές)");
        DrawQuality(("profq" + suid).c_str(), PS.quality, uid, 112.0f);

        // 7. his services, one charge per line, with what he paid and what we paid for it (dates: all by default)
        SectionTitle("Υπηρεσίες του (αναλυτικά)");
        if (DrawDateRange(("pts" + suid).c_str(), PS.from, PS.to)) { PS.charges.g.pager.Reset(); PS.selCharge = -1; }
        ImGui::TextDisabled("Αριστερό κλικ σε γραμμή: άνοιγμα της χρέωσης · δεξί κλικ: οι πληρωμές και το κόστος της εδώ δίπλα.");
        {
            const auto& pg = PS.charges.Load(suid + "|" + PS.from + "|" + PS.to,
                [&](const SortSpec& s, int off, int lim) {
                    return g_app.conn->pageClientCharges(uid, PS.from, PS.to, MapSort(s, {{CC_DATE, 0}, {CC_SVC, 1}, {CC_DUE, 2}, {CC_PAID, 3}, {CC_OWED, 4}, {CC_COST, 5}, {CC_COSTPAID, 6}, {CC_PROFIT, 7}}), off, lim); },
                [&] { return g_app.conn->countClientCharges(uid, PS.from, PS.to); });
            const float H = 330.0f;
            const bool selOn = PS.selCharge > 0;
            const float availW = ImGui::GetContentRegionAvail().x;
            const float pagerH = ImGui::GetFrameHeightWithSpacing() * 1.6f;
            const float rowH = ImGui::GetTextLineHeight() + ImGui::GetStyle().CellPadding.y * 2.0f;
            const float tableH = std::min(H - pagerH, ImGui::GetTextLineHeight() + ImGui::GetFrameHeight() + 14.0f + (float)pg.rows.size() * rowH + 16.0f);   // exactly its rows
            ImGui::BeginChild("pcL", ImVec2(selOn ? availW * 0.60f : 0.0f, selOn ? H : (pg.count == 0 ? ImGui::GetTextLineHeightWithSpacing() * 1.5f : tableH + pagerH + 6.0f)), false);
            if (pg.count == 0) ImGui::TextDisabled("Καμία υπηρεσία σε αυτό το διάστημα.");
            else {
                if (ImGui::BeginTable("profcharges", CC_COUNT, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable,
                                      ImVec2(0, selOn ? H - pagerH : tableH))) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ListSetupColumns(PS.charges.g.order, kClientChargeCols);
                    ImGui::TableSetColumnEnabled(ListPos(PS.charges.g.order, CC_PROFIT), false);      // no profit per service here: the figures are the columns
                    ListHeaderRow(PS.charges.g.order, kClientChargeCols, PS.charges.g.sort, PS.charges.g.pager);
                    auto P = [&](int c) { return ListPos(PS.charges.g.order, c); };
                    for (auto& r : pg.rows) {
                        ImGui::PushID(r.charge_id); ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(P(CC_DATE));
                        if (ImGui::Selectable(DateGr(r.date).c_str(), PS.selCharge == r.charge_id, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) OpenPaymentView(r.charge_id);
                        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) PS.selCharge = PS.selCharge == r.charge_id ? -1 : r.charge_id;
                        ImGui::TableSetColumnIndex(P(CC_SVC)); ImGui::TextUnformatted(r.service.c_str());
                        if (!r.offerName.empty()) { ImGui::SameLine(); ImGui::TextColored(kColOwed, "[%s]", r.offerName.c_str()); }
                        ImGui::TableSetColumnIndex(P(CC_DUE)); ImGui::TextUnformatted(Eur(r.due).c_str());
                        ImGui::TableSetColumnIndex(P(CC_PAID)); ImGui::TextColored(kColIn, "%s", Eur(r.paid).c_str());
                        ImGui::TableSetColumnIndex(P(CC_OWED)); if (r.balance > 0.005) { ImGui::TextColored(kColOwed, "%s", Eur(r.balance).c_str()); } else ImGui::TextDisabled("%s", Eur(0.0).c_str());
                        ImGui::TableSetColumnIndex(P(CC_COST)); if (r.cost > 0.005) ImGui::TextColored(kColOut, "%s", Eur(r.cost).c_str()); else ImGui::TextDisabled("%s", Eur(0.0).c_str());
                        ImGui::TableSetColumnIndex(P(CC_COSTPAID)); if (r.cost > 0.005) ImGui::TextColored(kColOut, "%s", Eur(r.costPaid).c_str()); else ImGui::TextDisabled("%s", Eur(0.0).c_str());
                        ImGui::TableSetColumnIndex(P(CC_PROFIT)); ImGui::TextColored(r.profit() >= 0 ? kColIn : kColOut, "%s", Eur(r.profit()).c_str());
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                DrawPager(PS.charges.g.pager, ("pcpg" + suid).c_str());
            }
            ImGui::EndChild();
            if (selOn) {
                ImGui::SameLine();
                ImGui::BeginChild("pcR", ImVec2(0, H), true);
                struct Side { Payment p; std::vector<MoneyPayment> money; DataConn::ChargeEconomics eco; };
                static Cached<Side> side;
                const Side& sd = side.get(std::to_string(PS.selCharge), [&] { Side s; s.p = g_app.conn->getPaymentById(PS.selCharge); s.money = g_app.conn->getMoneyForCharge(PS.selCharge); s.eco = g_app.conn->chargeEconomics(PS.selCharge); return s; });
                ImGui::TextColored(ImVec4(0.70f, 0.85f, 1.0f, 1.0f), "%s", ChargeLabel(sd.p).c_str());
                ImGui::SameLine(ImGui::GetContentRegionMax().x - 24); if (ImGui::SmallButton("x")) PS.selCharge = -1;
                ImGui::TextDisabled("%s · χρεώθηκε %s", DateGr(sd.p.year, sd.p.month, sd.p.day).c_str(), Eur(sd.p.amount_due).c_str());
                if (ImGui::SmallButton("Άνοιγμα χρέωσης")) OpenPaymentView(PS.selCharge);
                SectionTitle("Πληρωμές του πελάτη");
                if (sd.money.empty()) ImGui::TextDisabled("Καμία πληρωμή ακόμα.");
                for (auto& m : sd.money) ImGui::Text("%s · %s · %s", DateGr(m.pay_date).c_str(), ReceiptMethodName(m.method).c_str(), Eur(m.allocated > 0 ? m.allocated : m.amount).c_str());
                SectionTitle("Κόστος για εμάς");
                if (sd.eco.costs.empty()) ImGui::TextDisabled("Δεν καταχωρήθηκε κόστος.");
                for (auto& c : sd.eco.costs) {
                    ImGui::PushID(c.charge_id);
                    ImGui::Text("%s · %s", c.partner.c_str(), Eur(c.amount).c_str()); ImGui::SameLine();
                    ImGui::TextColored(c.balance > 0.005 ? kColWeOwe : kColIn, "%s", c.balance > 0.005 ? ("οφείλεται " + Eur(c.balance)).c_str() : "πληρώθηκε");
                    ImGui::PopID();
                }
            }
        }

        // 8. his parts of joint charges
        ImGui::Spacing();
        SectionTitle("Κοινόχρηστα (τα μερίδιά του)");
        if (DrawDateRange(("ptj" + suid).c_str(), PS.jfrom, PS.jto)) PS.shares.g.pager.Reset();
        DrawSharesTable(("profsh" + suid).c_str(), PS.shares, uid, -1, PS.jfrom, PS.jto, false, 280.0f);

        // 9. the very end: deleting the client (he leaves every list and his stay ends; the books keep everything he paid / owed)
        ImGui::Spacing(); ImGui::Separator();
        {
            std::string gone = g_app.conn->userDeletedAt(uid);
            if (!gone.empty()) ImGui::TextColored(kColMuted, "Ο πελάτης διαγράφηκε στις %s. Το ιστορικό του μένει όπως ήταν.", gone.c_str());
            else {
                ImGui::TextDisabled("Η διαγραφή βγάζει τον πελάτη από όλες τις λίστες και κλείνει τη διαμονή του στο διαμέρισμα. Οι χρεώσεις, οι πληρωμές και οι αποδείξεις του μένουν στο ιστορικό.");
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.18f, 1.0f));
                if (ImGui::Button("Διαγραφή πελάτη")) {
                    int id = uid; std::string name = u.full_name;
                    RequestConfirm("Διαγραφή πελάτη", { {"Όνομα", name, "(διαγραφή)"}, {"Διαμονή", u.apartment.empty() ? "-" : u.address + ", " + u.apartment, "τέλος σήμερα"} }, [id]() {
                        g_app.conn->removeUser(id);
                        g_app.reloadUsers();
                        g_profileUserIds.erase(std::remove(g_profileUserIds.begin(), g_profileUserIds.end(), id), g_profileUserIds.end());
                    });
                }
                ImGui::PopStyleColor();
            }
        }
        ImGui::End();

        if (closeThis) {
            g_profileUserIds.erase(
                std::remove(g_profileUserIds.begin(), g_profileUserIds.end(), uid),
                g_profileUserIds.end());
            g_profileHoveredSvc.erase(uid);
        }
    }
}



// Service Detail: a service is an independent catalog entry -- it is NOT tied to any
// client. The window shows the service's own logistics (money in/out, owed, scheduled),
// then an analysis built purely from its payments: which clients paid/owe for it and
// every payment on it.
// ---------------------------------------------------------------------------------------------------------
// The viewer shared by a service and a package. It keeps two worlds apart:
//   1. charges to single clients (per client, and under a client the charges with what he paid and what it cost us)
//   2. joint charges (per building; open one to see its residents)
// plus who we paid for it. `svc` = the service, or the services of the package.
// ---------------------------------------------------------------------------------------------------------
struct EntityState {
    std::string from, to; int selClient = -1, selOp = -1;
    ListModel<DataConn::EntityClientRow> clients; ListModel<DataConn::ClientChargeRow> charges; ListModel<JointOp> ops;
};
static std::map<std::string, EntityState> g_entityState;
enum { EV_NAME, EV_N, EV_SVCS, EV_DUE, EV_PAID, EV_OWED, EV_COST, EV_PROFIT, EV_COUNT };
static const ListColDef kEntityClientCols[EV_COUNT] = {
    { "Πελάτης", ImGuiTableColumnFlags_WidthFixed, 230.0f, true },    { "Χρεώσεις", ImGuiTableColumnFlags_WidthFixed, 90.0f, true },
    { "Υπηρεσίες", ImGuiTableColumnFlags_WidthFixed, 100.0f, true },  { "Χρεώθηκε", ImGuiTableColumnFlags_WidthFixed, 110.0f, true },
    { "Εισπράξεις", ImGuiTableColumnFlags_WidthFixed, 110.0f, true },    { "Οφειλές", ImGuiTableColumnFlags_WidthFixed, 110.0f, true },
    { "Κόστος", ImGuiTableColumnFlags_WidthFixed, 110.0f, true },     { "Κέρδος", ImGuiTableColumnFlags_WidthFixed, 110.0f, true } };
enum { EJ_BLD, EJ_LABEL, EJ_DATE, EJ_TOTAL, EJ_PCT, EJ_COST, EJ_COUNT };
static const ListColDef kEntityOpCols[EJ_COUNT] = {
    { "Πολυκατοικία", ImGuiTableColumnFlags_WidthFixed, 200.0f, true }, { "Κοινόχρηστο", ImGuiTableColumnFlags_WidthFixed, 240.0f, true },
    { "Ημ/νία", ImGuiTableColumnFlags_WidthFixed, 100.0f, true },       { "Σύνολο", ImGuiTableColumnFlags_WidthFixed, 100.0f, true },
    { "Συμπληρώθηκε", ImGuiTableColumnFlags_WidthFixed, 170.0f, true }, { "Κόστος", ImGuiTableColumnFlags_WidthFixed, 100.0f, false } };

static void DrawFigureLine(const char* title, const DataConn::HomeFigures& f) {
    ImGui::TextColored(ImVec4(0.70f, 0.85f, 1.0f, 1.0f), "%s", title);
    ImGui::SameLine(); ImGui::TextDisabled(" χρεώθηκαν"); ImGui::SameLine(); ImGui::Text("%s", Eur(f.charged).c_str());
    ImGui::SameLine(); ImGui::TextDisabled("· εισπράχθηκαν"); ImGui::SameLine(); ImGui::TextColored(kColIn, "%s", Eur(f.collected).c_str());
    ImGui::SameLine(); ImGui::TextDisabled("· οφείλονται"); ImGui::SameLine(); ImGui::TextColored(f.owed > 0.005 ? kColOwed : kColMuted, "%s", Eur(f.owed).c_str());
    ImGui::SameLine(); ImGui::TextDisabled("· κόστος"); ImGui::SameLine(); ImGui::TextColored(kColOut, "%s", Eur(f.cost).c_str());
    ImGui::SameLine(); ImGui::TextDisabled("· κέρδος"); ImGui::SameLine(); ImGui::TextColored(f.profit() >= 0 ? kColIn : kColOut, "%s", Eur(f.profit()).c_str());
}

static void DrawEntityViewer(const std::string& key, const std::vector<int>& svc) {
    EntityState& S = g_entityState[key];
    S.clients.Init(EV_COUNT, {{EV_DUE, false}}, 10); S.charges.Init(CC_COUNT, {{CC_DATE, false}}, 10); S.ops.Init(EJ_COUNT, {{EJ_DATE, false}}, 10);
    ImGui::PushID(key.c_str());
    if (DrawDateRange("er", S.from, S.to)) { S.clients.g.pager.Reset(); S.charges.g.pager.Reset(); S.ops.g.pager.Reset(); S.selClient = -1; S.selOp = -1; }
    const std::string dk = key + "|" + S.from + "|" + S.to;

    // ---- 1. single clients
    {
        static std::map<std::string, Cached<DataConn::HomeFigures>> fc;
        const auto& f = fc[key].get(dk, [&] { return g_app.conn->entityFigures(svc, false, S.from, S.to); });
        ImGui::Spacing(); SectionTitle("Χρεώσεις σε μεμονωμένους πελάτες");
        DrawFigureLine("Μεμονωμένες:", f);
        const auto& pg = S.clients.Load(dk, [&](const SortSpec& s, int off, int lim) {
                return g_app.conn->entityClients(svc, S.from, S.to, MapSort(s, {{EV_NAME, 0}, {EV_N, 1}, {EV_SVCS, 2}, {EV_DUE, 3}, {EV_PAID, 4}, {EV_OWED, 5}, {EV_COST, 6}, {EV_PROFIT, 7}}), off, lim); },
            [&] { return g_app.conn->countEntityClients(svc, S.from, S.to); });
        if (pg.count == 0) ImGui::TextDisabled("Καμία χρέωση σε μεμονωμένους πελάτες σε αυτό το διάστημα.");
        else {
            if (ImGui::BeginTable("entclients", EV_COUNT, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable,
                                  ImVec2(0, std::min(300.0f, 66.0f + 26.0f * (float)pg.rows.size())))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ListSetupColumns(S.clients.g.order, kEntityClientCols);
                ListHeaderRow(S.clients.g.order, kEntityClientCols, S.clients.g.sort, S.clients.g.pager);
                auto P = [&](int c) { return ListPos(S.clients.g.order, c); };
                for (auto& r : pg.rows) {
                    ImGui::PushID(r.user_id); ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(P(EV_NAME));
                    if (ImGui::Selectable(r.name.c_str(), S.selClient == r.user_id, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) { S.selClient = S.selClient == r.user_id ? -1 : r.user_id; S.charges.g.pager.Reset(); }
                    ImGui::TableSetColumnIndex(P(EV_N)); ImGui::Text("%d", r.charges);
                    ImGui::TableSetColumnIndex(P(EV_SVCS)); ImGui::Text("%d", r.services);
                    ImGui::TableSetColumnIndex(P(EV_DUE)); ImGui::TextUnformatted(Eur(r.due).c_str());
                    ImGui::TableSetColumnIndex(P(EV_PAID)); ImGui::TextColored(kColIn, "%s", Eur(r.paid).c_str());
                    ImGui::TableSetColumnIndex(P(EV_OWED)); if (r.balance > 0.005) ImGui::TextColored(kColOwed, "%s", Eur(r.balance).c_str()); else ImGui::TextDisabled("—");
                    ImGui::TableSetColumnIndex(P(EV_COST)); if (r.cost > 0.005) ImGui::TextColored(kColOut, "%s", Eur(r.cost).c_str()); else ImGui::TextDisabled("—");
                    ImGui::TableSetColumnIndex(P(EV_PROFIT)); ImGui::TextColored(r.profit() >= 0 ? kColIn : kColOut, "%s", Eur(r.profit()).c_str());
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            DrawPager(S.clients.g.pager, "entclpg");
            ImGui::TextDisabled("Κλικ σε πελάτη: οι χρεώσεις του με αυτό, και οι πληρωμές που έγιναν.");
        }
        if (S.selClient > 0) {
            ImGui::TextColored(ImVec4(0.70f, 0.85f, 1.0f, 1.0f), "Χρεώσεις του: %s", g_app.userName(S.selClient).c_str());
            ImGui::SameLine(); if (ImGui::SmallButton("Προφίλ πελάτη")) OpenUserProfile(S.selClient);
            const auto& cg = S.charges.Load(dk + "|" + std::to_string(S.selClient), [&](const SortSpec& s, int off, int lim) {
                    return g_app.conn->pageClientCharges(S.selClient, S.from, S.to, MapSort(s, {{CC_DATE, 0}, {CC_SVC, 1}, {CC_DUE, 2}, {CC_PAID, 3}, {CC_OWED, 4}, {CC_COST, 5}, {CC_COSTPAID, 6}, {CC_PROFIT, 7}}), off, lim, svc); },
                [&] { return g_app.conn->countClientCharges(S.selClient, S.from, S.to, svc); });
            if (ImGui::BeginTable("entcharges", CC_COUNT, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable,
                                  ImVec2(0, std::min(280.0f, 66.0f + 26.0f * (float)cg.rows.size())))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ListSetupColumns(S.charges.g.order, kClientChargeCols);
                ListHeaderRow(S.charges.g.order, kClientChargeCols, S.charges.g.sort, S.charges.g.pager);
                auto P = [&](int c) { return ListPos(S.charges.g.order, c); };
                for (auto& r : cg.rows) {
                    ImGui::PushID(r.charge_id); ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(P(CC_DATE));
                    if (ImGui::Selectable(DateGr(r.date).c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) OpenPaymentView(r.charge_id);
                    ImGui::TableSetColumnIndex(P(CC_SVC)); ImGui::TextUnformatted(r.service.c_str());
                    ImGui::TableSetColumnIndex(P(CC_DUE)); ImGui::TextUnformatted(Eur(r.due).c_str());
                    ImGui::TableSetColumnIndex(P(CC_PAID)); ImGui::TextColored(kColIn, "%s", Eur(r.paid).c_str());
                    ImGui::TableSetColumnIndex(P(CC_OWED)); if (r.balance > 0.005) { ImGui::TextColored(kColOwed, "%s", Eur(r.balance).c_str()); } else ImGui::TextDisabled("%s", Eur(0.0).c_str());
                    ImGui::TableSetColumnIndex(P(CC_COST)); if (r.cost > 0.005) ImGui::TextColored(kColOut, "%s", Eur(r.cost).c_str()); else ImGui::TextDisabled("%s", Eur(0.0).c_str());
                    ImGui::TableSetColumnIndex(P(CC_COSTPAID)); if (r.cost > 0.005) ImGui::TextColored(kColOut, "%s", Eur(r.costPaid).c_str()); else ImGui::TextDisabled("%s", Eur(0.0).c_str());
                    ImGui::TableSetColumnIndex(P(CC_PROFIT)); ImGui::TextColored(r.profit() >= 0 ? kColIn : kColOut, "%s", Eur(r.profit()).c_str());
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            DrawPager(S.charges.g.pager, "entchpg");
        }
    }

    // ---- 2. joint charges: per building, residents when one is opened
    {
        static std::map<std::string, Cached<DataConn::HomeFigures>> fc;
        const auto& f = fc[key].get(dk, [&] { return g_app.conn->entityFigures(svc, true, S.from, S.to); });
        ImGui::Spacing(); SectionTitle("Ως κοινόχρηστο (ανά πολυκατοικία)");
        DrawFigureLine("Κοινόχρηστα:", f);
        const auto& og = S.ops.Load(dk, [&](const SortSpec& s, int off, int lim) {
                return g_app.conn->pageJointOps(0, "", MapSort(s, {{EJ_BLD, 1}, {EJ_LABEL, 2}, {EJ_DATE, 0}, {EJ_TOTAL, 4}, {EJ_PCT, 7}}), off, lim, false, svc, S.from, S.to); },
            [&] { return g_app.conn->countJointOps(0, "", nullptr, nullptr, false, svc, S.from, S.to); });
        if (og.count == 0) ImGui::TextDisabled("Δεν έχει χρεωθεί ως κοινόχρηστο σε αυτό το διάστημα.");
        else {
            if (ImGui::BeginTable("entops", EJ_COUNT, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable,
                                  ImVec2(0, std::min(260.0f, 66.0f + 26.0f * (float)og.rows.size())))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ListSetupColumns(S.ops.g.order, kEntityOpCols);
                ListHeaderRow(S.ops.g.order, kEntityOpCols, S.ops.g.sort, S.ops.g.pager);
                auto P = [&](int c) { return ListPos(S.ops.g.order, c); };
                for (auto& o : og.rows) {
                    ImGui::PushID(o.id); ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(P(EJ_BLD));
                    if (ImGui::Selectable(o.building_address.empty() ? "—" : o.building_address.c_str(), S.selOp == o.id, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) S.selOp = S.selOp == o.id ? -1 : o.id;
                    ImGui::TableSetColumnIndex(P(EJ_LABEL)); ImGui::TextUnformatted(o.label.c_str());
                    ImGui::TableSetColumnIndex(P(EJ_DATE)); ImGui::TextUnformatted(DateGr(o.op_date).c_str());
                    ImGui::TableSetColumnIndex(P(EJ_TOTAL)); ImGui::TextUnformatted(Eur(o.total).c_str());
                    ImGui::TableSetColumnIndex(P(EJ_PCT));
                    { double pct = o.total > 0.005 ? std::min(100.0, o.paid / o.total * 100.0) : 100.0; char b[32]; std::snprintf(b, sizeof(b), "%.0f%%", pct);
                      ImGui::PushStyleColor(ImGuiCol_PlotHistogram, pct >= 99.995 ? ImVec4(0.30f, 0.70f, 0.45f, 1) : ImVec4(0.85f, 0.60f, 0.25f, 1));
                      ImGui::ProgressBar((float)(pct / 100.0), ImVec2(-1, 0), b); ImGui::PopStyleColor(); }
                    ImGui::TableSetColumnIndex(P(EJ_COST));
                    { static std::map<int, Cached<double>> cc; double cst = cc[o.id].get("", [&] { return g_app.conn->getJointOpCost(o.id); });
                      if (cst > 0.005) ImGui::TextColored(kColOut, "%s", Eur(cst).c_str()); else ImGui::TextDisabled("—"); }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            DrawPager(S.ops.g.pager, "entoppg");
            ImGui::TextDisabled("Κλικ σε κοινόχρηστο: οι ένοικοι και οι πληρωμές τους.");
            if (S.selOp > 0) {
                static std::map<int, Cached<std::pair<JointOp, std::vector<JointShare>>>> sc;
                const auto& d = sc[S.selOp].get("", [&] { return std::make_pair(g_app.conn->getJointOpById(S.selOp), g_app.conn->getJointShares(S.selOp)); });
                ImGui::TextColored(ImVec4(0.70f, 0.85f, 1.0f, 1.0f), "%s · %s", d.first.label.c_str(), d.first.building_address.c_str());
                ImGui::SameLine(); if (ImGui::SmallButton("Άνοιγμα κοινοχρήστου")) OpenJointView(S.selOp);
                if (ImGui::BeginTable("entshares", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                    ImGui::TableSetupColumn("Διαμέρισμα", ImGuiTableColumnFlags_WidthFixed, 100); ImGui::TableSetupColumn("Ένοικος", ImGuiTableColumnFlags_WidthStretch, 2.0f);
                    ImGui::TableSetupColumn("Μερίδιο", ImGuiTableColumnFlags_WidthFixed, 100); ImGui::TableSetupColumn("Εισπράξεις", ImGuiTableColumnFlags_WidthFixed, 100);
                    ImGui::TableSetupColumn("Οφειλές", ImGuiTableColumnFlags_WidthFixed, 100);
                    ImGui::TableHeadersRow();
                    for (auto& s : d.second) {
                        if (!s.included) continue;
                        double left = MoneySub(s.amount, std::min(s.paid, s.amount));
                        ImGui::PushID(s.id); ImGui::TableNextRow();
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(s.apartment_label.c_str());
                        ImGui::TableNextColumn(); if (s.user_id > 0 && LinkCell(s.user_name)) OpenUserProfile(s.user_id);
                        ImGui::TableNextColumn(); ImGui::TextUnformatted(Eur(s.amount).c_str());
                        ImGui::TableNextColumn(); ImGui::TextColored(kColIn, "%s", Eur(std::min(s.paid, s.amount)).c_str());
                        ImGui::TableNextColumn(); if (left > 0.005) ImGui::TextColored(kColOwed, "%s", Eur(left).c_str()); else ImGui::TextDisabled("—");
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
            }
        }
    }

    // ---- 3. who we paid for it
    {
        static std::map<std::string, Cached<std::vector<DataConn::PartnerStat>>> pc;
        const auto& ps = pc[key].get(dk, [&] { return g_app.conn->entityPartners(svc, S.from, S.to); });
        ImGui::Spacing(); SectionTitle("Κόστος: σε ποιους πληρώσαμε");
        if (ps.empty()) ImGui::TextDisabled("Δεν υπάρχει καταχωρημένο κόστος.");
        else if (ImGui::BeginTable("entpartners", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Συνεργάτης", ImGuiTableColumnFlags_WidthStretch, 2.0f); ImGui::TableSetupColumn("Δουλειές", ImGuiTableColumnFlags_WidthFixed, 80);
            ImGui::TableSetupColumn("Κόστος", ImGuiTableColumnFlags_WidthFixed, 110); ImGui::TableSetupColumn("Πληρώθηκε", ImGuiTableColumnFlags_WidthFixed, 110);
            ImGui::TableSetupColumn("Οφείλεται", ImGuiTableColumnFlags_WidthFixed, 110);
            ImGui::TableHeadersRow();
            for (auto& s : ps) {
                ImGui::PushID(s.partner_id); ImGui::TableNextRow();
                ImGui::TableNextColumn(); if (LinkCell(s.name)) OpenPartnerProfile(s.partner_id);
                ImGui::TableNextColumn(); ImGui::Text("%d", s.jobs);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(Eur(s.cost).c_str());
                ImGui::TableNextColumn(); ImGui::TextColored(kColOut, "%s", Eur(s.paid).c_str());
                ImGui::TableNextColumn(); if (s.owedNow > 0.005) ImGui::TextColored(kColWeOwe, "%s", Eur(s.owedNow).c_str()); else ImGui::TextDisabled("—");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::PopID();
}

// Service viewer: its data, the two distinct worlds (single clients / joint charges), edit at the top, delete at the very end.
static void DrawServiceDetailWindow() {
    std::vector<int> ids = g_detailServiceIds;
    for (size_t i = 0; i < ids.size(); ++i) {
        int sid = ids[i];
        Service s = g_app.conn->getServiceById(sid);
        bool open = true;
        ImGuiIO& io = ImGui::GetIO();
        float cascade = (float)i * 26.0f;
        std::string title = "Υπηρεσία: " + s.label + "###service_" + std::to_string(sid);
        ImGui::SetNextWindowSize(ImVec2(900, 700), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 940.0f - cascade, 70.0f + cascade), ImGuiCond_FirstUseEver);
        ImGui::Begin(title.c_str(), &open);
        ImGui::TextColored(kColIn, "Τιμή: %s", Eur(s.suggested_price).c_str());
        ImGui::SameLine(); ImGui::TextColored(kColOut, "· Κόστος: %s", Eur(s.cost_price).c_str());
        if (!s.extra_notes.empty()) ImGui::TextWrapped("%s", s.extra_notes.c_str());
        if (!s.created_at.empty()) ImGui::TextDisabled("Δημιουργήθηκε: %s", s.created_at.c_str());
        if (ImGui::SmallButton("Χρέωση")) OpenChargeService(s.id);
        ImGui::SameLine(); if (ImGui::SmallButton("Ως κοινόχρηστο")) OpenJointChargeFor(2, s.id, s.label);
        ImGui::SameLine();
        if (ImGui::SmallButton("Επεξεργασία")) {
            g_serviceDialogIsEdit = true; g_serviceDialogId = s.id;
            sb_label.set(s.label); sb_notes.set(s.extra_notes); sb_price = s.suggested_price; sb_cost = s.cost_price;
            g_showServiceDialog = true;
        }
        DrawEntityViewer("svc" + std::to_string(sid), {sid});

        // the very end: deleting the service
        ImGui::Spacing(); ImGui::Separator();
        ImGui::TextDisabled("Η διαγραφή κρύβει την υπηρεσία από τις λίστες· τίποτα δεν χάνεται από τη βάση. Βρίσκεται στις Ρυθμίσεις ► Αρχείο διαγραφών (με δυνατότητα επαναφοράς).");
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.18f, 1.0f));
        if (ImGui::Button("Διαγραφή υπηρεσίας")) {
            std::string label = s.label;
            RequestConfirm("Διαγραφή υπηρεσίας", { {"Ετικέτα", label, "(διαγραφή)"} }, [sid]() {
                if (g_app.conn->removeService(sid)) { g_detailServiceIds.erase(std::remove(g_detailServiceIds.begin(), g_detailServiceIds.end(), sid), g_detailServiceIds.end()); }
                g_app.reloadServices(); g_app.reloadJoint();
            });
        }
        ImGui::PopStyleColor();
        ImGui::End();
        if (!open) g_detailServiceIds.erase(std::remove(g_detailServiceIds.begin(), g_detailServiceIds.end(), sid), g_detailServiceIds.end());
    }
}

// Package viewer: like the service's, over all the services of the package; what it includes, edit on top, delete at the end.
static void DrawPackageViewWindow() {
    std::vector<int> ids = g_pkgViewIds;
    for (size_t i = 0; i < ids.size(); ++i) {
        int pid = ids[i];
        std::string label = g_app.conn->getPackageLabel(pid);
        auto comps = g_app.conn->getMasterServiceComponents(pid);
        std::vector<int> svc; double price = 0, cost = 0;
        for (auto& c : comps) { svc.push_back(c.id); price += c.suggested_price; cost += g_app.conn->getServiceCost(c.id); }
        bool open = true;
        ImGuiIO& io = ImGui::GetIO();
        float cascade = (float)i * 26.0f;
        ImGui::SetNextWindowSize(ImVec2(900, 720), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 940.0f - cascade, 60.0f + cascade), ImGuiCond_FirstUseEver);
        ImGui::Begin(("Πακέτο: " + label + "###package_" + std::to_string(pid)).c_str(), &open);
        ImGui::TextColored(kColIn, "Σύνολο τιμής: %s", Eur(price).c_str());
        ImGui::SameLine(); ImGui::TextColored(kColOut, "· Σύνολο κόστους: %s", Eur(cost).c_str());
        ImGui::SameLine(); ImGui::TextColored(price - cost >= 0 ? kColIn : kColOut, "· Κέρδος: %s", Eur(MoneySub(price, cost)).c_str());
        if (ImGui::SmallButton("Χρέωση")) OpenPackageCharge(pid);
        ImGui::SameLine(); if (ImGui::SmallButton("Ως κοινόχρηστο")) OpenJointChargeFor(1, pid, label);
        ImGui::SameLine(); if (ImGui::SmallButton("Επεξεργασία")) g_pkgEditId = pid;
        if (ImGui::CollapsingHeader("Τι περιλαμβάνει")) {
            if (ImGui::BeginTable("pkgcomp", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Υπηρεσία", ImGuiTableColumnFlags_WidthStretch, 2.0f); ImGui::TableSetupColumn("Τιμή", ImGuiTableColumnFlags_WidthFixed, 100); ImGui::TableSetupColumn("Κόστος", ImGuiTableColumnFlags_WidthFixed, 100);
                ImGui::TableHeadersRow();
                for (auto& c : comps) {
                    ImGui::TableNextRow(); ImGui::TableNextColumn();
                    if (LinkCell(c.label)) OpenServiceDetail(c.id);
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(Eur(c.suggested_price).c_str());
                    ImGui::TableNextColumn(); double cs = g_app.conn->getServiceCost(c.id); if (cs > 0.005) ImGui::TextUnformatted(Eur(cs).c_str()); else ImGui::TextDisabled("—");
                }
                ImGui::EndTable();
            }
        }
        DrawEntityViewer("pkg" + std::to_string(pid), svc);

        ImGui::Spacing(); ImGui::Separator();
        ImGui::TextDisabled("Η απόσυρση κρύβει το πακέτο από τις λίστες. Οι χρεώσεις του και το ιστορικό του μένουν.");
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.18f, 1.0f));
        if (ImGui::Button("Απόσυρση πακέτου")) {
            RequestConfirm("Απόσυρση πακέτου (κρύβεται· οι χρεώσεις του και το ιστορικό του μένουν)", { {"Πακέτο", label, "(απόσυρση)"} }, [pid]() {
                g_app.conn->removeMasterService(pid); g_app.reloadServices();
                g_pkgViewIds.erase(std::remove(g_pkgViewIds.begin(), g_pkgViewIds.end(), pid), g_pkgViewIds.end());
            });
        }
        ImGui::PopStyleColor();
        ImGui::End();
        if (!open) g_pkgViewIds.erase(std::remove(g_pkgViewIds.begin(), g_pkgViewIds.end(), pid), g_pkgViewIds.end());
    }
}

// ---------------------------------------------------------------------------
// RESIDENT LEDGER: opened from a building's resident list. A separate window with
// that resident's money organised for the building context: totals, a month-by-month
// breakdown (building dues are monthly), then their services grouped by package, each
// with its own payments. Payments always stay per client; the building only gives the
// context (where the client lives).
// ---------------------------------------------------------------------------
// Week start (Monday) of an ISO date, for grouping by week.
static std::string WeekStartIso(const std::string& iso) {
    std::tm t{}; t.tm_year = std::atoi(iso.substr(0, 4).c_str()) - 1900; t.tm_mon = std::atoi(iso.substr(5, 2).c_str()) - 1;
    t.tm_mday = std::atoi(iso.substr(8, 2).c_str()); t.tm_hour = 12; std::mktime(&t);
    int back = (t.tm_wday + 6) % 7; t.tm_mday -= back; std::mktime(&t);
    char b[16]; std::strftime(b, sizeof(b), "%Y-%m-%d", &t); return b;
}

// RESIDENT WINDOW (opened from a building's residents): only what this person owes as a resident, i.e. his shares of
// the building's joint charges. His own services belong to his profile, not here.
static void DrawResidentLedgerWindow() {
    static const char* kMonths[] = { "Ιαν","Φεβ","Μαρ","Απρ","Μαΐ","Ιουν","Ιουλ","Αυγ","Σεπ","Οκτ","Νοε","Δεκ" };
    std::vector<int> ids = g_ledgerUserIds;
    for (size_t i = 0; i < ids.size(); ++i) {
        int uid = ids[i];
        User u = g_app.conn->getUserById(uid);
        bool open = true;
        ImGuiIO& io = ImGui::GetIO();
        float cascade = (float)i * 26.0f;
        std::string title = "Κοινόχρηστα ενοίκου: " + u.full_name + "###ledger_" + std::to_string(uid);
        ImGui::SetNextWindowSize(ImVec2(860, 680), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 920.0f - cascade, 70.0f + cascade), ImGuiCond_FirstUseEver);
        ImGui::Begin(title.c_str(), &open);

        std::string loc = u.address.empty() ? "(χωρίς διαμέρισμα)" : u.address;
        if (!u.apartment.empty()) loc += "  ·  διαμ. " + u.apartment;
        ImGui::TextColored(ImVec4(0.70f, 0.85f, 1.0f, 1.0f), "%s", loc.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Προφίλ πελάτη (όλες οι υπηρεσίες)")) OpenUserProfile(uid);
        ImGui::TextDisabled("Εδώ μόνο τα μερίδιά του από κοινόχρηστα. Οι δικές του υπηρεσίες είναι στο προφίλ του.");

        ProfileState& PS = g_profileState[-uid];          // own state, separate from the profile window
        static std::map<int, int> periodOf; static std::map<int, bool> customOf;
        int& period = periodOf[uid]; bool& custom = customOf[uid];
        {
            ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Ανά"); ImGui::SameLine();
            const char* pn[] = { "Ημέρα", "Εβδομάδα", "Μήνα" };
            for (int k = 0; k < 3; k++) {
                if (k) ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, period == k + 0 ? ImVec4(0.24f, 0.55f, 0.75f, 1.0f) : ImVec4(0.16f, 0.18f, 0.24f, 1.0f));
                if (ImGui::Button(pn[k], ImVec2(90, 0))) period = k;
                ImGui::PopStyleColor();
            }
            ImGui::SameLine(0, 24);
            if (ImGui::Checkbox("Συγκεκριμένο διάστημα", &custom)) { if (!custom) { PS.from.clear(); PS.to.clear(); } PS.shares.g.pager.Reset(); PS.owed.Reset(); }
            if (custom) {
                ImGui::SameLine(); ImGui::TextDisabled("Από:"); ImGui::SameLine();
                bool ch = DatePicker(("lfrom" + std::to_string(uid)).c_str(), PS.from, ImVec2(0, 1));
                ImGui::SameLine(); ImGui::TextDisabled("Έως:"); ImGui::SameLine();
                ch |= DatePicker(("lto" + std::to_string(uid)).c_str(), PS.to, ImVec2(0, 1));
                if (ch) { PS.shares.g.pager.Reset(); PS.owed.Reset(); }
            }
        }
        DataConn::PaymentFilter f; f.userId = uid; f.joint = 1; f.fromDate = PS.from; f.toDate = PS.to;

        // totals of his joint shares in the range
        struct Head { DataConn::MoneyTotals t; std::vector<DataConn::GroupedTotals> days; };
        static std::map<int, Cached<Head>> heads;
        const Head& hd = heads[uid].get(PayFilterKey(f), [&] {
            Head h; h.t = g_app.conn->paymentTotals(f); h.days = g_app.conn->paymentTotalsBy(DataConn::PayGroup::Day, f); return h; });
        char buf[48];
        if (ImGui::BeginTable("ledgerstats", 3, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableNextRow();
            struct Tile { const char* label; double v; ImVec4 col; };
            Tile tiles[3] = { {"Χρεώθηκαν (μερίδια)", hd.t.dueAll, ImVec4(0.85f,0.88f,0.95f,1)}, {"Εισπράξεις", hd.t.collected, kColIn}, {"Οφειλές", hd.t.owedIn, kColOwed} };
            for (int k = 0; k < 3; k++) {
                ImGui::TableNextColumn(); ImGui::PushID(k);
                std::snprintf(buf, sizeof(buf), "%s", Eur(tiles[k].v).c_str());
                StatTile(tiles[k].label, buf, tiles[k].col, nullptr, ImVec2(ImGui::GetContentRegionAvail().x, 72));
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        // per day / week / month (newest first, paged)
        {
            struct Row { std::string key, label; double due = 0, paid = 0, owed = 0; long long n = 0; };
            std::map<std::string, Row, std::greater<std::string>> by;
            for (auto& d : hd.days) {
                std::string k = period == 0 ? d.key : period == 1 ? WeekStartIso(d.key) : d.key.substr(0, 7);
                Row& r = by[k]; r.key = k;
                r.due = FromCents(ToCents(r.due) + ToCents(d.t.dueAll)); r.paid = FromCents(ToCents(r.paid) + ToCents(d.t.collected));
                r.owed = FromCents(ToCents(r.owed) + ToCents(d.t.owedIn)); r.n += d.t.count;
            }
            std::vector<Row> rows; for (auto& [k, r] : by) rows.push_back(r);
            static std::map<int, Pager> pp; Pager& pg = pp[uid]; if (pg.pageSize == 50) pg.pageSize = 8;
            pg.total = (int)rows.size(); if (pg.offset >= pg.total) pg.Reset();
            SectionTitle(period == 0 ? "Ανά ημέρα" : period == 1 ? "Ανά εβδομάδα" : "Ανά μήνα");
            if (rows.empty()) ImGui::TextDisabled("Κανένα κοινόχρηστο σε αυτό το διάστημα.");
            else if (ImGui::BeginTable("ledgerper", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableSetupColumn("Περίοδος", ImGuiTableColumnFlags_WidthStretch, 1.6f);
                ImGui::TableSetupColumn("Χρεώθηκαν", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Εισπράξεις", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Οφειλές", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Χρεώσεις", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                ImGui::TableHeadersRow();
                for (int r = pg.offset; r < std::min(pg.total, pg.offset + pg.pageSize); r++) {
                    const Row& x = rows[r]; ImGui::TableNextRow(); ImGui::TableNextColumn();
                    if (period == 2) ImGui::Text("%s %s", kMonths[(std::atoi(x.key.substr(5, 2).c_str()) + 11) % 12], x.key.substr(0, 4).c_str());
                    else if (period == 1) ImGui::Text("εβδ. από %s", DateGr(x.key).c_str());
                    else ImGui::TextUnformatted(DateGr(x.key).c_str());
                    ImGui::TableNextColumn(); ImGui::Text("%s", Eur(x.due).c_str());
                    ImGui::TableNextColumn(); ImGui::TextColored(kColIn, "%s", Eur(x.paid).c_str());
                    ImGui::TableNextColumn(); ImGui::TextColored(x.owed > 0.005 ? kColOwed : kColMuted, "%s", Eur(x.owed).c_str());
                    ImGui::TableNextColumn(); ImGui::Text("%lld", x.n);
                }
                ImGui::EndTable();
            }
            DrawPager(pg, ("ledgerperpg" + std::to_string(uid)).c_str());
        }

        // his parts: what is still open (from him or from us), then the whole history
        SectionTitle("Τρέχοντα μερίδια (εκκρεμή)");
        static std::map<int, ListModel<JointShare>> ledOpen, ledHist;
        DrawSharesTable(("ledopen" + std::to_string(uid)).c_str(), ledOpen[uid], uid, -1, PS.from, PS.to, true, 200.0f);
        SectionTitle("Ιστορικό μεριδίων");
        DrawSharesTable(("ledhist" + std::to_string(uid)).c_str(), ledHist[uid], uid, -1, PS.from, PS.to, false, 260.0f);
        ImGui::End();

        if (!open) g_ledgerUserIds.erase(std::remove(g_ledgerUserIds.begin(), g_ledgerUserIds.end(), uid), g_ledgerUserIds.end());
    }
}

// Payment Viewer: the bare minimum, read-only, nowhere further to go.
static std::string ReceiptNo(const Receipt& r);
static void DrawPaymentViewWindow() {
    std::vector<int> ids = g_viewPaymentIds;
    for (size_t i = 0; i < ids.size(); ++i) {
        int pid = ids[i];
        Payment p = g_app.conn->getPaymentById(pid);
        bool open = true;

        ImGuiIO& io = ImGui::GetIO();
        float cascade = (float)i * 26.0f;
        std::string title = "Πληρωμή #" + std::to_string(pid) + "###payment_" + std::to_string(pid);
        ImGui::SetNextWindowSize(ImVec2(400, 0), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 440.0f - cascade, 80.0f + cascade),
                                ImGuiCond_FirstUseEver);
        ImGui::Begin(title.c_str(), &open);

        ImGui::TextColored(DirectionColor(p.direction), "%s", DirectionBadge(p.direction));
        ImGui::SameLine(0, 16); if (ImGui::SmallButton("Επεξεργασία")) StartEditPayment(p);
        if (p.status == "future") { ImGui::SameLine(); if (ImGui::SmallButton("Επίλυση")) OpenResolvePayment(p); }
        ImGui::Separator();

        // Clickable: opens a *new* profile / service window (or focuses the
        // existing one if this exact id already has a window open).
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.72f, 1.0f, 1.0f));
        if (ImGui::Selectable(("Πελάτης: " + g_app.userName(p.user_id)).c_str()))
            OpenUserProfile(p.user_id);
        if (ImGui::Selectable(("Υπηρεσία: " + g_app.serviceLabel(p.service_id)).c_str()))
            OpenServiceDetail(p.service_id);
        if (p.joint_op_id > 0) {
            static Cached<JointOp> jop;
            const JointOp& o = jop.get(std::to_string(p.joint_op_id), [&] { return g_app.conn->getJointOpById(p.joint_op_id); });
            std::string s = "Μερίδιο κοινοχρήστου: " + o.label + " (" + o.building_address + ", " + o.op_date +
                            ", σύνολο " + Eur(o.total) + ")";
            if (ImGui::Selectable(s.c_str())) OpenJointView(o.id);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Άνοιγμα της κοινόχρηστης χρέωσης: όλα τα μερίδια και οι εξαιρέσεις");
        }
        if (p.batch_id > 0) {
            static Cached<DataConn::PaymentPlan> plc;
            const auto& pl = plc.get(std::to_string(p.batch_id), [&] { return g_app.conn->getPlanForBatch(p.batch_id); });
            if (pl.id > 0) {
                std::string s = "Πρόγραμμα δόσεων: " + pl.label + " · " + std::to_string(pl.paidInstallments) + "/" + std::to_string(pl.installments) +
                                " εξοφλημένες · " + FormatMoney(pl.paid) + " / " + Eur(pl.total);
                if (pl.receipt_id > 0) { if (ImGui::Selectable((s + " · απόδειξη εκδόθηκε").c_str())) OpenReceiptView(pl.receipt_id); }
                else ImGui::TextUnformatted((s + (pl.receipt_mode == "end" ? " · απόδειξη με την τελευταία δόση" : " · απόδειξη ανά δόση")).c_str());
            }
        }
        ImGui::PopStyleColor();
        {
            static std::map<int, Cached<DataConn::ChargeEconomics>> ecoCaches;
            const DataConn::ChargeEconomics& eco = ecoCaches[pid].get("", [&] { return g_app.conn->chargeEconomics(pid); });
            if (!eco.costs.empty() || p.direction == "out") {
                SectionTitle("Κόστος και συνεργάτες");
                if (eco.incomeChargeId > 0)
                    ImGui::Text("Τι πήραμε: χρεώθηκε %s · εισπράχθηκε %s", Eur(eco.incomeDue).c_str(), Eur(eco.incomePaid).c_str());
                for (auto& c : eco.costs) {
                    ImGui::PushID(c.charge_id);
                    ImGui::PushStyleColor(ImGuiCol_Text, kColLink);
                    if (ImGui::Selectable(c.partner.c_str(), false, 0, ImVec2(ImGui::CalcTextSize(c.partner.c_str()).x, 0))) OpenPartnerProfile(c.partner_id);
                    ImGui::PopStyleColor();
                    ImGui::SameLine(); ImGui::Text("· κόστος %s · πληρώθηκε %s", Eur(c.amount).c_str(), Eur(c.paid).c_str());
                    if (c.balance > 0.005) { ImGui::SameLine(); ImGui::TextColored(kColOwed, "· οφείλεται %s", Eur(c.balance).c_str()); }
                    if (c.charge_id != pid) { ImGui::SameLine(); if (ImGui::SmallButton("Άνοιγμα")) OpenPaymentView(c.charge_id); }
                    ImGui::PopID();
                }
                if (!eco.costs.empty() && eco.incomeChargeId > 0)
                    ImGui::TextColored(eco.incomeDue - eco.costDue() >= 0 ? kColIn : kColOut, "Κέρδος: %s", Eur(MoneySub(eco.incomeDue, eco.costDue())).c_str());
            }
        }
        if (p.used_offer_id > 0)
            ImGui::TextColored(kColOwed, "%% Προσφορά: %s  ·  τιμή καταλόγου %s, έκπτωση %s", p.used_offer_name.c_str(), Eur(p.used_list_price).c_str(), Eur(p.used_discount).c_str());
        {
            // Where it happened = where the client currently lives (derived by JOIN).
            User owner = g_app.conn->getUserById(p.user_id);
            if (!owner.address.empty()) {
                std::string loc = owner.address;
                if (!owner.apartment.empty())   loc += ", διαμ. " + owner.apartment;
                if (!owner.area.empty())        loc += " · " + owner.area;
                if (!owner.postal_code.empty()) loc += " · Τ.Κ. " + owner.postal_code;
                ImGui::TextDisabled("Τοποθεσία: %s", loc.c_str());
            }
        }

        ImGui::Separator();
        ImGui::Text("Ημερομηνία: %04d-%02d-%02d", p.year, p.month, p.day);
        ImGui::Text("Οφειλόμενο: %s", Eur(p.amount_due).c_str());
        ImGui::Text("Πληρωμένο: %s", Eur(p.amount_paid).c_str());
        ImGui::Text("Κατάσταση: %s", p.status.c_str());
        if (p.status == "future" && !p.resolution_date.empty()) ImGui::Text("Ημ/νία επίλυσης: %s", p.resolution_date.c_str());
        if (!p.resolved_at.empty()) ImGui::TextDisabled("Επιλύθηκε στις: %s", p.resolved_at.c_str());
        if (!p.notes.empty()) ImGui::TextWrapped("Σημειώσεις: %s", p.notes.c_str());
        // the money behind this line (bills and payments are separate)
        auto money = g_app.conn->getMoneyForCharge(p.id);
        if (!money.empty()) {
            SectionTitle("Εισπράξεις / πληρωμές");
            for (auto& m : money) {
                ImGui::Text("%s · %s (εδώ %s) · %s", m.pay_date.c_str(), Eur(m.amount).c_str(), Eur(m.allocated).c_str(),
                            m.method == "unspecified" ? "τρόπος —" : ReceiptMethodName(m.method).c_str());
                if (m.receipt_id > 0) { ImGui::SameLine(); ImGui::PushID(m.id); if (ImGui::SmallButton("Απόδειξη")) OpenReceiptView(m.receipt_id); ImGui::PopID(); }
            }
        }
        // every receipt tied to this charge (through the money paid on it)
        {
            std::set<int> rids; for (auto& m : money) if (m.receipt_id > 0) rids.insert(m.receipt_id);
            if (p.receipt_id > 0) rids.insert(p.receipt_id);
            SectionTitle(("Αποδείξεις (" + std::to_string(rids.size()) + ")").c_str());
            if (rids.empty()) ImGui::TextDisabled(p.direction == "out" ? "Πληρωμή προς τρίτο: δεν εκδίδεται απόδειξη." : (p.status == "future" ? "Προγραμματισμένη: δεν έχει πληρωθεί ακόμα." : "Καμία απόδειξη για αυτή τη χρέωση."));
            for (int rid : rids) {
                Receipt r = g_app.conn->getReceiptById(rid);
                ImGui::PushID(rid);
                ImGui::Text("%s · %s · %s · %s", ReceiptNo(r).c_str(), DateGr(r.issue_date).c_str(), Eur(r.total).c_str(), r.kind == "joint" ? "κοινόχρηστα" : "προσωπική χρήση");
                if (r.status == "void") { ImGui::SameLine(); ImGui::TextColored(kColOut, "ΑΚΥΡΩΜΕΝΗ"); }
                ImGui::SameLine(); if (ImGui::SmallButton("Άνοιγμα απόδειξης")) OpenReceiptView(rid);
                ImGui::PopID();
            }
        }
        DrawAttachmentsPanel("charge", p.id);

        // the very end: deleting this charge
        ImGui::Spacing(); ImGui::Separator();
        ImGui::TextDisabled("Η διαγραφή αφαιρεί τη χρέωση. Αν έχει εκδοθεί απόδειξη, το πρόγραμμα δεν την επιτρέπει (ακυρώστε πρώτα την απόδειξη).");
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.18f, 1.0f));
        if (ImGui::Button("Διαγραφή πληρωμής")) {
            int id = pid;
            RequestConfirm("Διαγραφή πληρωμής", { {"Πληρωμή #", std::to_string(id), "(διαγραφή)"} }, [id]() {
                g_app.conn->removePayment(id);
                g_viewPaymentIds.erase(std::remove(g_viewPaymentIds.begin(), g_viewPaymentIds.end(), id), g_viewPaymentIds.end());
            });
        }
        ImGui::PopStyleColor();
        ImGui::End();

        if (!open) {
            g_viewPaymentIds.erase(std::remove(g_viewPaymentIds.begin(), g_viewPaymentIds.end(), pid),
                                   g_viewPaymentIds.end());
        }
    }
}

// ---------------------------------------------------------------------------
// DASHBOARD / TIMELINE SCREEN ("Αρχική")
// ---------------------------------------------------------------------------
static std::string g_dashStartDate, g_dashEndDate;
static int g_dashUserFilter = -1, g_dashServiceFilter = -1;
static bool g_showNotifPopup = false;

// --- Dashboard timeline pagination (offset-based "load older" paging) -----
// Manual button-driven paging (not scroll-triggered): simplest reliable option
// in immediate-mode UI. Resets to 0 whenever a filter changes or on refresh,
// so a stale offset never leaves the user paginated into an irrelevant window.
static int g_dashPageOffset = 0;
static const int kDashPageSize = 20;
// Snapshot of the filter values as of the last frame, used purely to detect a
// change and reset paging -- compared by value every frame, cheap at this scale.
static std::string g_dashStartDatePrev, g_dashEndDatePrev;
static int g_dashUserFilterPrev = -1, g_dashServiceFilterPrev = -1;
// Set true whenever the timeline is rebuilt (filter change / chip switch /
// refresh) so the scroll position snaps back to the top -- the newest entries
// are always what the user lands on. (Historical name kept; the boundary
// divider it used to snap to no longer exists.)
static bool g_dashScrollToBoundary = true;
// Quick type filter above the timeline: 0 = Όλα, 1 = μόνο εισπράξεις (in),
// 2 = μόνο πληρωμές (out), 3 = μόνο μελλοντικές. Does NOT affect the stat
// tiles above -- those always reflect the full filtered set.
static int g_dashTypeFilter = 0;

// --- Phone-book-style picker state ------------------------------------------
// Each picker owns its own search text, letter filter, sort direction, and page
// offset. Only one page of rows is fetched from the DB per frame while the
// popup is open (kXxxPickerPageSize), so the pickers stay cheap even when the
// users/services tables are huge -- the full tables are never loaded just to
// render the filter row.
static char g_userPickerSearch[128] = "";
// Multi-select letter filter: click a letter to add it to the active set,
// click it again to remove it. "Όλοι" clears the whole set (no letter filter).
// Empty set = no filtering. Non-empty set = names starting with ANY of them.
static std::set<std::string> g_userPickerLetters;
static bool g_userPickerReverse = false;    // false = Α→Ω, true = Ω→Α
static int g_userPickerOffset = 0;
static const int kUserPickerPageSize = 20;

static char g_servicePickerSearch[128] = "";
static int g_servicePickerOffset = 0;
static const int kServicePickerPageSize = 20;

// Phone-book user picker: label on left with colon, then a button showing the
// current selection, which opens a dictionary-style popup with a Greek-letter
// strip (jump to first letter), a name search, an A→Z / Z→A toggle, and paged
// rows. Selecting a row sets `selectedUserId` and closes the popup.
static void DrawUserFilterPicker(const char* label, int& selectedUserId, float width) {
    ImGui::TextUnformatted(label);
    ImGui::SameLine();

    std::string preview = (selectedUserId < 0) ? "Όλοι" : g_app.userName(selectedUserId);
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(width);
    if (ImGui::Button((preview + " ▼").c_str(), ImVec2(width, 0))) {
        g_userPickerOffset = 0;
        ImGui::OpenPopup("##userpicker");
    }

    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopup("##userpicker")) {
        ImGui::TextDisabled("Επιλογή ιδιοκτήτη");
        ImGui::Separator();

                // Greek-letter strip: Όλοι + Α..Ω, wraps every 13 buttons.
        // Letters are TOGGLES: click to activate (adds to the filter set),
        // click again to deactivate (removes it). Multiple letters can be
        // active at once -- names starting with ANY active letter match.
        // "Όλοι" is active (and clicking it) when the set is empty.
        static const char* kLetters[] = {
            "Α","Β","Γ","Δ","Ε","Ζ","Η","Θ","Ι","Κ","Λ","Μ",
            "Ν","Ξ","Ο","Π","Ρ","Σ","Τ","Υ","Φ","Χ","Ψ","Ω"
        };
        const int kTotal = 1 + IM_ARRAYSIZE(kLetters);
        const int kPerRow = 13;
        for (int i = 0; i < kTotal; i++) {
            if (i > 0 && (i % kPerRow) != 0) ImGui::SameLine();
            if (i == 0) {
                bool active = g_userPickerLetters.empty();
                if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f,0.55f,0.75f,1.0f));
                if (ImGui::SmallButton("Όλοι")) { g_userPickerLetters.clear(); g_userPickerOffset = 0; }
                if (active) ImGui::PopStyleColor();
            } else {
                const char* L = kLetters[i - 1];
                bool active = (g_userPickerLetters.count(L) > 0);
                if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f,0.55f,0.75f,1.0f));
                ImGui::PushID(i);
                if (ImGui::SmallButton(L)) {
                    // Toggle: if it's already active, remove it; otherwise add it.
                    if (active) g_userPickerLetters.erase(L);
                    else        g_userPickerLetters.insert(L);
                    g_userPickerOffset = 0;
                }
                ImGui::PopID();
                if (active) ImGui::PopStyleColor();
            }
        }

        ImGui::Separator();

        // Search + sort toggle
        ImGui::SetNextItemWidth(280);
        if (ImGui::InputTextWithHint("##userpickersearch", "Αναζήτηση ονόματος...",
                                     g_userPickerSearch, sizeof(g_userPickerSearch))) {
            g_userPickerOffset = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button(g_userPickerReverse ? "Ω → Α" : "Α → Ω")) {
            g_userPickerReverse = !g_userPickerReverse;
            g_userPickerOffset = 0;
        }

        ImGui::Separator();

        // Fetch this page + count in one shot per frame while popup is open.
                // Fetch this page + count in one shot per frame while popup is open.
        // The letter set is copied into a vector just before the call (that's
        // what the SQL builder expects); empty vector = no letter filtering.
        std::vector<std::string> lettersVec(g_userPickerLetters.begin(),
                                            g_userPickerLetters.end());
        std::vector<User> rows = g_app.conn->pickerUsers(
            lettersVec, g_userPickerSearch,
            g_userPickerOffset, kUserPickerPageSize, g_userPickerReverse);
        int total = g_app.conn->pickerUsersCount(lettersVec, g_userPickerSearch);

        ImGui::BeginChild("##userpickerlist", ImVec2(400, 320), true);
        if (rows.empty()) {
            ImGui::TextDisabled("Καμία εγγραφή.");
        }
        std::string lastInitial;
        for (auto& u : rows) {
            // Section header whenever the first byte(s) of the name change.
            std::string initial;
            if (!u.full_name.empty()) {
                unsigned char c = (unsigned char)u.full_name[0];
                if (c < 0x80) initial = std::string(1, (char)c);
                else if ((c & 0xE0) == 0xC0 && u.full_name.size() >= 2) initial = u.full_name.substr(0, 2);
                else if ((c & 0xF0) == 0xE0 && u.full_name.size() >= 3) initial = u.full_name.substr(0, 3);
                else initial = std::string(1, (char)c);
            }
            if (initial != lastInitial) {
                ImGui::Spacing();
                ImGui::TextDisabled("— %s —", initial.c_str());
                lastInitial = initial;
            }
            ImGui::PushID(u.id);
            if (ImGui::Selectable(u.full_name.c_str(), u.id == selectedUserId)) {
                selectedUserId = u.id;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndChild();

        ImGui::Separator();
        int showingEnd = std::min(g_userPickerOffset + (int)rows.size(), total);
        ImGui::TextDisabled("Εμφάνιση %d-%d από %d",
                            rows.empty() ? 0 : g_userPickerOffset + 1, showingEnd, total);
        ImGui::SameLine();
        if (g_userPickerOffset > 0) {
            if (ImGui::SmallButton("← Προηγ.")) g_userPickerOffset = std::max(0, g_userPickerOffset - kUserPickerPageSize);
        } else { ImGui::BeginDisabled(); ImGui::SmallButton("← Προηγ."); ImGui::EndDisabled(); }
        ImGui::SameLine();
        if (showingEnd < total) {
            if (ImGui::SmallButton("Επόμενα →")) g_userPickerOffset += kUserPickerPageSize;
        } else { ImGui::BeginDisabled(); ImGui::SmallButton("Επόμενα →"); ImGui::EndDisabled(); }
        ImGui::SameLine();
        if (ImGui::SmallButton("Καθαρισμός")) {
            selectedUserId = -1;
            g_userPickerSearch[0] = 0;
            g_userPickerLetters.clear();
            g_userPickerReverse = false;
            g_userPickerOffset = 0;
        }

        ImGui::EndPopup();
    }
    ImGui::PopID();
}

// Service picker: label on left, button showing current selection, popup with
// a name search and paged rows (no letter strip -- the user asked for a simple
// searchable dropdown here).
static void DrawServiceFilterPicker(const char* label, int& selectedServiceId, float width) {
    ImGui::TextUnformatted(label);
    ImGui::SameLine();

    std::string preview = (selectedServiceId < 0) ? "Όλες" : g_app.serviceLabel(selectedServiceId);
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(width);
    if (ImGui::Button((preview + " ▼").c_str(), ImVec2(width, 0))) {
        g_servicePickerOffset = 0;
        ImGui::OpenPopup("##servicepicker");
    }

    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopup("##servicepicker")) {
        ImGui::TextDisabled("Επιλογή υπηρεσίας");
        ImGui::Separator();

        ImGui::SetNextItemWidth(280);
        if (ImGui::InputTextWithHint("##servicepickersearch", "Αναζήτηση υπηρεσίας...",
                                     g_servicePickerSearch, sizeof(g_servicePickerSearch))) {
            g_servicePickerOffset = 0;
        }

        ImGui::Separator();

        std::vector<Service> rows = g_app.conn->pickerServices(
            g_servicePickerSearch, g_servicePickerOffset, kServicePickerPageSize);
        int total = g_app.conn->pickerServicesCount(g_servicePickerSearch);

        ImGui::BeginChild("##servicepickerlist", ImVec2(400, 320), true);
        if (rows.empty()) ImGui::TextDisabled("Καμία εγγραφή.");
        for (auto& s : rows) {
            ImGui::PushID(s.id);
            if (ImGui::Selectable(s.label.c_str(), s.id == selectedServiceId)) {
                selectedServiceId = s.id;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndChild();

        ImGui::Separator();
        int showingEnd = std::min(g_servicePickerOffset + (int)rows.size(), total);
        ImGui::TextDisabled("Εμφάνιση %d-%d από %d",
                            rows.empty() ? 0 : g_servicePickerOffset + 1, showingEnd, total);
        ImGui::SameLine();
        if (g_servicePickerOffset > 0) {
            if (ImGui::SmallButton("← Προηγ.")) g_servicePickerOffset = std::max(0, g_servicePickerOffset - kServicePickerPageSize);
        } else { ImGui::BeginDisabled(); ImGui::SmallButton("← Προηγ."); ImGui::EndDisabled(); }
        ImGui::SameLine();
        if (showingEnd < total) {
            if (ImGui::SmallButton("Επόμενα →")) g_servicePickerOffset += kServicePickerPageSize;
        } else { ImGui::BeginDisabled(); ImGui::SmallButton("Επόμενα →"); ImGui::EndDisabled(); }
        ImGui::SameLine();
        if (ImGui::SmallButton("Καθαρισμός")) {
            selectedServiceId = -1;
            g_servicePickerSearch[0] = 0;
            g_servicePickerOffset = 0;
        }

        ImGui::EndPopup();
    }
    ImGui::PopID();
}

// Parses "YYYY-MM-DD" and returns whole days between it and today (negative = past,
// 0 = today, positive = future). Returns a large positive number if dateStr is empty
// or unparsable, so callers treat it as "far away" / non-urgent rather than crashing.
static int DaysUntilDate(const std::string& dateStr) {
    int y = 0, m = 0, d = 0;
    if (dateStr.size() < 8 || std::sscanf(dateStr.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return 9999;
    std::tm target{}; target.tm_year = y - 1900; target.tm_mon = m - 1; target.tm_mday = d; target.tm_hour = 12;
    std::time_t targetT = std::mktime(&target);
    std::time_t nowT = std::time(nullptr);
    std::tm nowTm{}; localtime_s(&nowTm, &nowT);
    nowTm.tm_hour = 12; nowTm.tm_min = 0; nowTm.tm_sec = 0;
    std::time_t nowNoon = std::mktime(&nowTm);
    if (targetT == (std::time_t)-1 || nowNoon == (std::time_t)-1) return 9999;
    return (int)std::round(std::difftime(targetT, nowNoon) / 86400.0);
}


// Full-width day section banner: "Πέμπτη, 20 Αυγούστου 2026 · Σήμερα" on the
// left, that day's net total on the right. Each day's rows stack under their
// banner, giving the timeline a banking-app look. Replaces both the old
// muted two-line "Σήμερα/Χθες" header and the removed weekly ruler markers.
static void DrawDaySectionHeader(int year, int month, int day,
                                  int curYear, int curMonth, int curDay,
                                  double dayNet) {
    static const char* kWeekday[] = { "Κυριακή","Δευτέρα","Τρίτη","Τετάρτη","Πέμπτη","Παρασκευή","Σάββατο" };
    static const char* kMonthNames[] = { "Ιανουαρίου","Φεβρουαρίου","Μαρτίου","Απριλίου","Μαΐου","Ιουνίου",
                                          "Ιουλίου","Αυγούστου","Σεπτεμβρίου","Οκτωβρίου","Νοεμβρίου","Δεκεμβρίου" };
    auto dow = [](int y, int m, int d) {
        static const int t[] = { 0,3,2,5,0,3,5,1,4,6,2,4 };
        if (m < 3) y -= 1;
        return (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
    };

    // "Σήμερα" / "Χθες" suffix, same rules as before.
    std::tm t{}; t.tm_year = year - 1900; t.tm_mon = month - 1; t.tm_mday = day; t.tm_hour = 12;
    std::time_t dT = std::mktime(&t);
    std::tm c{}; c.tm_year = curYear - 1900; c.tm_mon = curMonth - 1; c.tm_mday = curDay; c.tm_hour = 12;
    std::time_t cT = std::mktime(&c);
    double diffDays = (dT != (std::time_t)-1 && cT != (std::time_t)-1) ? std::difftime(cT, dT) / 86400.0 : 999.0;
    const char* suffix = "";
    if (year == curYear && month == curMonth && day == curDay) suffix = "  ·  Σήμερα";
    else if (diffDays >= 0.5 && diffDays < 1.5) suffix = "  ·  Χθες";

    char dateBuf[96];
    std::snprintf(dateBuf, sizeof(dateBuf), "%s, %d %s %d%s",
                  kWeekday[dow(year, month, day)], day, kMonthNames[month - 1], year, suffix);

    float w = ImGui::GetContentRegionAvail().x;
    const float hdrH = 36.0f;
    std::string dayId = "day_" + std::to_string(year) + "_" + std::to_string(month) + "_" + std::to_string(day);
    ImGui::PushID(dayId.c_str());
    // Distinct background so the banner reads as a section tab, not a row.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.16f, 0.19f, 0.24f, 0.65f));
        BeginCard("dayhdr", ImVec2(w, hdrH), /*noScroll=*/true);

    ImGui::SetCursorPos(ImVec2(14, 8));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.88f, 0.91f, 0.98f, 1.0f));
    ImGui::TextUnformatted(dateBuf);
    ImGui::PopStyleColor();

    ImGui::SetCursorPos(ImVec2(std::max(200.0f, w - 170.0f), 8));
    ImVec4 netCol = dayNet >= 0 ? kColIn : kColOut;
    ImGui::PushStyleColor(ImGuiCol_Text, netCol);
    ImGui::Text("%s%s", dayNet >= 0 ? "+" : "", Eur(dayNet).c_str());
    ImGui::PopStyleColor();

    EndCard();
    ImGui::PopStyleColor();
    ImGui::PopID();
}

// One timeline row: circular direction arrow at the left, owner name on top
// with the service label under it, amount on the right. Two-line layout so
// the full-width card reads like a bank statement entry rather than the old
// cramped one-liner. Date is carried by the day-section banner above, not
// repeated per row.
static void DrawTimelineRow(const Payment& p, bool faded) {
    ImGui::PushID(p.id);
    float w = ImGui::GetContentRegionAvail().x;
    const float rowH = 54.0f;
    if (faded) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.60f);
        BeginCard("row", ImVec2(w, rowH), /*noScroll=*/true);

    bool isOut = p.direction == "out";
    ImVec4 dirCol = isOut ? kColOut : kColIn;

    // Left: direction arrow
    ImGui::SetCursorPos(ImVec2(16, 16));
    ImGui::PushStyleColor(ImGuiCol_Text, dirCol);
    ImGui::Text("%s", isOut ? u8"↑" : u8"↓");
    ImGui::PopStyleColor();

    // Middle, line 1: owner name
    ImGui::SetCursorPos(ImVec2(44, 8));
    ImGui::TextUnformatted(g_app.userName(p.user_id).c_str());

    // Middle, line 2: service · direction · optional future marker · optional note
    ImGui::SetCursorPos(ImVec2(44, 30));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f,0.63f,0.70f,1));
    std::string sub = ChargeLabel(p);
    sub += "  ·  ";
    sub += isOut ? "Πληρωμή" : "Είσπραξη";
    if (p.status == "future") sub += "  ·  ⏱ Μελλοντική";
    if (!p.notes.empty()) { sub += "  ·  "; sub += p.notes; }
    ImGui::TextUnformatted(sub.c_str());
    ImGui::PopStyleColor();

    // Right: amount (paid if resolved, due if future)
    ImGui::SetCursorPos(ImVec2(std::max(220.0f, w - 160.0f), 18));
    {
        bool isPaid = p.status != "future";
        ImGui::PushStyleColor(ImGuiCol_Text, dirCol);
        ImGui::Text("%s%s", isOut ? "-" : "+", Eur(isPaid ? p.amount_paid : p.amount_due).c_str());
        ImGui::PopStyleColor();
    }

    // Invisible full-row click zone -- opens the read-only Payment Viewer.
    ImGui::SetCursorPos(ImVec2(0, 0));
    if (ImGui::InvisibleButton("clickzone", ImVec2(w, rowH))) OpenPaymentView(p.id);
    EndCard();
    if (faded) ImGui::PopStyleVar();
    ImGui::PopID();
}
static void DrawDashboardScreen() {
    auto future = g_app.conn->getFuturePayments();

    // --- Header: reminders + refresh -----------------------------------
    if (ImGui::Button(("[!] Υπενθυμίσεις (" + std::to_string(future.size()) + ")").c_str())) g_showNotifPopup = true;
    ImGui::SameLine();
    if (ImGui::Button("Ανανέωση")) { g_dashPageOffset = 0; g_dashScrollToBoundary = true; }

    static bool notifOpen = false;
    if (g_showNotifPopup) {
        ImGui::OpenPopup("Επερχόμενες Πληρωμές");
        g_showNotifPopup = false;
        notifOpen = true;
    }
    if (BeginGuardedModal("Επερχόμενες Πληρωμές", notifOpen, ImGuiWindowFlags_AlwaysAutoResize)) {
        // Future payments live ONLY here (they're excluded from the main dashboard
        // timeline entirely, per the client). This is also where Resolve is reachable
        // for them now that they no longer appear as rows on the main screen.
        // Color gradient by proximity to resolution_date: green/neutral when comfortably
        // future (>7 days out), amber once within 7 days ("needs attention soon" -- the
        // one place amber is used in this app), red once past due (existing badge kept).
        for (auto& p : future) {
            int days = DaysUntilDate(p.resolution_date);
            ImVec4 dateCol = days < 0 ? kColOut : days <= 7 ? ImVec4(0.90f,0.65f,0.20f,1) : ImVec4(0.55f,0.65f,0.72f,1);
            ImGui::PushID(p.id);
            ImGui::TextColored(dateCol, "%s", p.resolution_date.empty() ? "(χωρίς ημ/νία)" : p.resolution_date.c_str());
            ImGui::SameLine();
            ImGui::Text("%s / %s  %s", g_app.userName(p.user_id).c_str(), ChargeLabel(p).c_str(), Eur(p.amount_due).c_str());
            if (days < 0) { ImGui::SameLine(); ImGui::TextColored(kColOut, "[ΕΛΗΞΕ]"); }
            else if (days <= 7) { ImGui::SameLine(); ImGui::TextColored(ImVec4(0.90f,0.65f,0.20f,1), "[σε %d ημ.]", days); }
            ImGui::SameLine();
            if (ImGui::SmallButton("Επίλυση")) { notifOpen = false; ImGui::CloseCurrentPopup(); OpenResolvePayment(p); }
            ImGui::PopID();
        }
        if (future.empty()) ImGui::TextDisabled("Καμία επερχόμενη πληρωμή.");
        ImGui::Spacing();
        if (ImGui::Button("Κλείσιμο")) { notifOpen = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    ImGui::Separator();

        // --- Filters, restyled as one integrated toolbar row ----------------
    // Filters now use the phone-book pickers: label on the LEFT with a colon,
    // and a button that opens a paged, searchable popup. Users list is NOT
    // loaded in full -- only the visible page is queried from the DB.
    BeginCard("filters", ImVec2(0, 60));
    ImGui::TextDisabled("Φίλτρα:");
    ImGui::SameLine();
    ImGui::TextDisabled("Από:"); ImGui::SameLine();
    DatePicker("dash_start", g_dashStartDate, ImVec2(0, 1));
    ImGui::SameLine();
    ImGui::TextDisabled("Έως:"); ImGui::SameLine();
    DatePicker("dash_end", g_dashEndDate, ImVec2(0, 1));
    ImGui::SameLine();
    DrawUserFilterPicker("Χρήστης:", g_dashUserFilter, 180.0f);
    ImGui::SameLine();
    DrawServiceFilterPicker("Υπηρεσία:", g_dashServiceFilter, 180.0f);
    EndCard();
    ImGui::Spacing();

    // --- Stats + timeline rows come straight from SQL, cached until the data or a filter changes ----------
    // (was: every frame, a copy + full sort of every payment, and a rescan of all of them per day banner)
    std::time_t nowT = std::time(nullptr);
    std::tm nowTm{}; localtime_s(&nowTm, &nowT);
    int curYear = nowTm.tm_year + 1900, curMonth = nowTm.tm_mon + 1, curDay = nowTm.tm_mday;

    DataConn::PaymentFilter base;
    base.fromDate = g_dashStartDate; base.toDate = g_dashEndDate;
    base.userId = g_dashUserFilter;  base.serviceId = g_dashServiceFilter;
    const std::string fkey = g_dashStartDate + "|" + g_dashEndDate + "|" + std::to_string(g_dashUserFilter) + "|" + std::to_string(g_dashServiceFilter);

    // --- 3x3 stat spec, verbatim per client: "total outcome, total input, the
    // sum, and then the same of future payments only, and then another time the
    // same with future and the past together as one."
    //   Group A (resolved only): outcome/input from amount_paid.
    //   Group B (future only): outcome/input from amount_due (nothing paid yet).
    //   Group C (combined): A + B for each of outcome/input/sum.
    static Cached<DataConn::MoneyTotals> totCache;
    const DataConn::MoneyTotals& tot = totCache.get(fkey, [&] { return g_app.conn->paymentTotals(base); });
    double outcomeA = tot.paidOut,   inputA = tot.collected;   // resolved
    double outcomeB = tot.futureOut, inputB = tot.futureIn;    // future
    double sumA = inputA - outcomeA;
    double sumB = inputB - outcomeB;
    double outcomeC = outcomeA + outcomeB, inputC = inputA + inputB, sumC = inputC - outcomeC;
    // Reset pagination whenever a filter actually changed since last frame (or the
    // caller pressed "Ανανέωση", handled separately below) -- a filter change makes
    // the previous offset window meaningless.
    bool filtersChanged = g_dashStartDate != g_dashStartDatePrev ||
                       g_dashEndDate   != g_dashEndDatePrev   ||
                       g_dashUserFilter    != g_dashUserFilterPrev ||
                       g_dashServiceFilter != g_dashServiceFilterPrev;
    if (filtersChanged) {
        g_dashPageOffset = 0;
        g_dashScrollToBoundary = true;
        g_dashStartDatePrev = g_dashStartDate;
        g_dashEndDatePrev   = g_dashEndDate;
        g_dashUserFilterPrev = g_dashUserFilter;
        g_dashServiceFilterPrev = g_dashServiceFilter;
    }

    // --- 3 clearly-separated groups of 3 tiles, per the client's exact spec:
    // "total outcome, total input, the sum, and then the same of future
    // payments only, and then another time the same with future and the past
    // together as one." Each group gets its own labeled section header and its
    // own 3-column ImGuiTableFlags_SizingStretchSame table -- kept as 3 separate
    // BeginTable calls (rather than one 3x3 table) so each section is visually
    // its own block with its own heading, not just three unlabeled rows.
    //
    // Column width is measured explicitly via GetContentRegionAvail() right
    // after TableNextColumn(), same fix as before -- passing -1 into a
    // BeginChild-based card size inside a table cell resolves against the
    // OUTER window, not the narrow column, and is exactly what produced the
    // old "one giant tile, rest missing" bug. Never do that again here.
    // One compact 3x3 grid: groups (Resolved / Future / Combined) as COLUMNS,
    // metrics (Outcome / Input / Sum) as ROWS -- horizontal, not three stacked
    // full-width blocks. Every cell needs a genuinely unique id: BeginTable
    // pushes one ID scope for the whole table, so identical tile labels reused
    // across columns (e.g. "Σύνολο Εξόδων" appears once per group) would still
    // collide with each other here -- PushID(col) around each cell scopes them
    // apart, same fix as StatTile's own id, applied at the call-site level too.
    char buf[64];
    {
        // the whole period in four plain numbers, then what that means for the cash
        const double owedToUs = tot.owedIn;
        const double weOwe    = tot.owedOut;
        SectionTitle("Η εικόνα της περιόδου");
        if (ImGui::BeginTable("dashstats4", 4, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextRow();
            struct T { const char* l; double v; ImVec4 c; const char* s; };
            T tl[4] = { { "Εισπράξεις", tot.collected, kColIn, "χρήματα που εισπράχθηκαν από πελάτες" },
                        { "Οφειλές", owedToUs, kColOwed, "χρεώσεις που οφείλουν οι πελάτες" },
                        { "Πληρωμές", tot.paidOut, kColOut, "κόστος που πληρώθηκε σε συνεργάτες" },
                        { "Οφειλόμενα", weOwe, kColWeOwe, "κόστος που οφείλουμε ακόμα" } };
            for (int k = 0; k < 4; k++) {
                ImGui::TableNextColumn(); ImGui::PushID(k);
                std::snprintf(buf, sizeof(buf), "%s", Eur(tl[k].v).c_str());
                StatTile(tl[k].l, buf, tl[k].c, tl[k].s, ImVec2(ImGui::GetContentRegionAvail().x, 96));
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        const double cashNow = MoneySub(tot.collected, tot.paidOut), cashAll = FromCents(ToCents(cashNow) + ToCents(owedToUs) - ToCents(weOwe));
        ImGui::Text("Ταμείο (εισπράξεις - πληρωμές):"); ImGui::SameLine(); ImGui::TextColored(cashNow >= 0 ? kColIn : kColOut, "%s", Eur(cashNow).c_str());
        ImGui::SameLine(0, 28); ImGui::Text("Όταν τακτοποιηθούν όλα (+ οφειλές - οφειλόμενα):"); ImGui::SameLine(); ImGui::TextColored(cashAll >= 0 ? kColIn : kColOut, "%s", Eur(cashAll).c_str());
        ImGui::TextDisabled("Με τα φίλτρα από πάνω (ημερομηνίες, πελάτης, υπηρεσία). Η λίστα από κάτω δείχνει τις πρόσφατες κινήσεις.");
    }
    ImGui::Spacing();

    // --- Informational type filter: inline text links, not tabby buttons.
    // Reads as a quiet strip you can click, not a chunky segmented control.
    // Does NOT affect the stat tiles above -- only the timeline below.
    ImGui::TextDisabled("Εμφάνιση:");
    ImGui::SameLine();
    auto QuietFilter = [](const char* label, bool active) -> bool {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f,0.25f,0.32f,0.55f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.25f,0.30f,0.40f,0.80f));
        ImGui::PushStyleColor(ImGuiCol_Text,
            active ? ImVec4(0.40f,0.90f,0.60f,1.0f) : ImVec4(0.58f,0.63f,0.70f,1.0f));
        bool clicked = ImGui::Button(label);
        ImGui::PopStyleColor(4);
        return clicked;
    };
    auto ApplyTypeFilter = [&](int v) {
        g_dashTypeFilter = v; g_dashPageOffset = 0; g_dashScrollToBoundary = true;
    };
    if (QuietFilter("Όλες",        g_dashTypeFilter == 0)) ApplyTypeFilter(0);
    ImGui::SameLine(); ImGui::TextDisabled("·"); ImGui::SameLine();
    if (QuietFilter("Εισπράξεις",  g_dashTypeFilter == 1)) ApplyTypeFilter(1);
    ImGui::SameLine(); ImGui::TextDisabled("·"); ImGui::SameLine();
    if (QuietFilter("Πληρωμές",    g_dashTypeFilter == 2)) ApplyTypeFilter(2);
    ImGui::SameLine(); ImGui::TextDisabled("·"); ImGui::SameLine();
    if (QuietFilter("Μελλοντικές", g_dashTypeFilter == 3)) ApplyTypeFilter(3);
    ImGui::Separator();

    // --- Timeline rows for the type chip: all future rows (top) + the resolved pages loaded so far ------
    struct DashRows { std::vector<Payment> futureList, resolvedList; long long resolvedTotal = 0; std::map<std::string, double> dayNet; };
    static Cached<DashRows> rowsCache;
    const DashRows& rows = rowsCache.get(fkey + "|" + std::to_string(g_dashTypeFilter) + "|" + std::to_string(g_dashPageOffset), [&] {
        DashRows r;
        DataConn::PaymentFilter tf = base;
        if (g_dashTypeFilter == 1) tf.direction = "in";
        if (g_dashTypeFilter == 2) tf.direction = "out";
        // Future: nearest-due at the BOTTOM (closest to the boundary); more distant ones above.
        DataConn::PaymentFilter ff = tf; ff.status = 2;
        r.futureList = g_app.conn->findPayments(ff, DataConn::PayOrder::ResolutionDesc, 0, 1000);
        if (g_dashTypeFilter != 3) {
            // Resolved: newest first, only as many as the pages loaded so far.
            DataConn::PaymentFilter rf = tf; rf.status = 1;
            r.resolvedList = g_app.conn->findPayments(rf, DataConn::PayOrder::DateDesc, 0, g_dashPageOffset + kDashPageSize);
            // the total doesn't depend on how many pages are loaded: cached separately, so "load older" stays instant
            static Cached<long long> totalCache;
            r.resolvedTotal = totalCache.get(fkey + "|" + std::to_string(g_dashTypeFilter), [&] { return g_app.conn->countPaymentsWhere(rf); });
            if (!r.resolvedList.empty()) {
                // Day banner totals cover the WHOLE day, also rows not loaded yet (a day can span pages).
                DataConn::PaymentFilter df = rf;
                char a[16], b[16];
                const Payment& oldest = r.resolvedList.back(); const Payment& newest = r.resolvedList.front();
                std::snprintf(a, sizeof(a), "%04d-%02d-%02d", oldest.year, oldest.month, oldest.day);
                std::snprintf(b, sizeof(b), "%04d-%02d-%02d", newest.year, newest.month, newest.day);
                df.fromDate = a; df.toDate = b;
                for (auto& g : g_app.conn->paymentTotalsBy(DataConn::PayGroup::Day, df)) r.dayNet[g.key] = g.t.collected - g.t.paidOut;
            }
        }
        return r;
    });
    const std::vector<Payment>& futureList = rows.futureList;
    const std::vector<Payment>& resolvedList = rows.resolvedList;
    if (futureList.empty() && resolvedList.empty()) {
        ImGui::TextDisabled("Καμία κίνηση στο τρέχον φίλτρο.");
        return;
    }
    int pageEnd = (int)resolvedList.size();

    ImGui::BeginChild("timelinelist", ImVec2(0, 0), false);

    // --- Vertical timeline spine setup -----------------------------------
    // Background line on channel 0, per-entry dots on channel 1 -- so the line
    // never paints over a dot regardless of draw order. The line itself is
    // emitted LAST (we don't know its bottom Y until all content is laid out).
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->ChannelsSplit(2);
    const float kSpineXOffset = 22.0f;
    const float kRowIndent    = 46.0f;
    const float kRowHeight    = 54.0f;
    float spineX    = ImGui::GetCursorScreenPos().x + kSpineXOffset;
    float spineTopY = ImGui::GetCursorScreenPos().y;
    ImU32 spineLineCol = ImGui::ColorConvertFloat4ToU32(ImVec4(0.30f,0.33f,0.40f,0.75f));

    auto DrawSpineDot = [&](ImVec4 col, float rowH) {
        dl->ChannelsSetCurrent(1);
        float cy = ImGui::GetCursorScreenPos().y + rowH * 0.5f;
        dl->AddCircleFilled(ImVec2(spineX, cy), 4.5f, ImGui::ColorConvertFloat4ToU32(col));
        dl->AddCircle(ImVec2(spineX, cy), 4.5f, IM_COL32(20,20,24,255), 0, 1.0f);
        dl->ChannelsSetCurrent(0);
    };
    auto DrawSpineBigDot = [&](ImU32 col, float cy) {
        dl->ChannelsSetCurrent(1);
        dl->AddCircleFilled(ImVec2(spineX, cy), 6.0f, col);
        dl->AddCircle(ImVec2(spineX, cy), 6.0f, IM_COL32(20,20,24,255), 0, 1.2f);
        dl->ChannelsSetCurrent(0);
    };

    bool any = false;

    // --- FUTURE section (top, dimmed) ------------------------------------
    // Fade future rows only when they sit alongside resolved rows; when the
    // "Μελλοντικές" filter is active they ARE the focus, so no fade.
    bool fadeFuture = (g_dashTypeFilter != 3);
    if (!futureList.empty()) {
        for (auto& p : futureList) {
            any = true;
            DrawSpineDot(StatusColor("future"), kRowHeight);
            ImGui::Indent(kRowIndent);
            DrawTimelineRow(p, /*faded=*/fadeFuture);
            ImGui::Unindent(kRowIndent);
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
        }
    }

    // --- BOUNDARY divider between future (above) and resolved (below) -----
    // Only shown when both sections are present this frame.
    if (!futureList.empty() && !resolvedList.empty()) {
        DrawSpineBigDot(IM_COL32(200,170,60,255), ImGui::GetCursorScreenPos().y + 12.0f);
        ImGui::Indent(kRowIndent);
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.85f,0.70f,0.30f,1.0f), "%s", u8"▲ Μελλοντικές Πληρωμές  ·  Πρόσφατες Κινήσεις ▼");
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        ImGui::Unindent(kRowIndent);

        // Snap scroll so the boundary sits ~40px from the top of the viewport
        // -- resolves are what you land on, and you scroll UP to reveal future.
        // Only done ONCE per rebuild (filter change / chip switch / refresh).
        float boundaryY = ImGui::GetCursorPosY();
        if (g_dashScrollToBoundary) {
            ImGui::SetScrollY(std::max(0.0f, boundaryY - 40.0f));
            g_dashScrollToBoundary = false;
        }
    } else if (g_dashScrollToBoundary) {
        // No boundary this frame (e.g. only future or only resolved) -- sit at
        // the top of the list.
        ImGui::SetScrollY(0.0f);
        g_dashScrollToBoundary = false;
    }

    // --- RESOLVED section (day banners + rows) ----------------------------
    int lastY = -1, lastM = -1, lastD = -1;
    for (int i = 0; i < pageEnd; i++) {
        const Payment& p = resolvedList[i];
        any = true;
        if (p.year != lastY || p.month != lastM || p.day != lastD) {
            char dk[16]; std::snprintf(dk, sizeof(dk), "%04d-%02d-%02d", p.year, p.month, p.day);
            auto dn = rows.dayNet.find(dk);
            double dayNet = dn == rows.dayNet.end() ? 0.0 : dn->second;
            DrawSpineBigDot(ImGui::ColorConvertFloat4ToU32(ImVec4(0.45f,0.65f,0.85f,1.0f)),
                            ImGui::GetCursorScreenPos().y + 18.0f);
            ImGui::Indent(kRowIndent);
            DrawDaySectionHeader(p.year, p.month, p.day, curYear, curMonth, curDay, dayNet);
            ImGui::Unindent(kRowIndent);
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            lastY = p.year; lastM = p.month; lastD = p.day;
        }
        DrawSpineDot(StatusColor(p.status), kRowHeight);
        ImGui::Indent(kRowIndent);
        DrawTimelineRow(p, false);
        ImGui::Unindent(kRowIndent);
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
    }

    if (!any) ImGui::TextDisabled("Καμία κίνηση στο τρέχον φίλτρο.");

    // --- LOAD OLDER -------------------------------------------------------
    if (pageEnd < rows.resolvedTotal) {
        ImGui::Spacing();
        ImGui::Indent(kRowIndent);
        int remaining = (int)(rows.resolvedTotal - pageEnd);
        // Large buffer on purpose: Greek chars are 2 UTF-8 bytes each, so the
        // old char[64] silently truncated the label mid-character and rendered
        // garbled text whenever the remaining count hit 3+ digits.
        char lbl[256];
        std::snprintf(lbl, sizeof(lbl), "Φόρτωση παλαιότερων  ·  %d ακόμα", remaining);
        float w = ImGui::GetContentRegionAvail().x;
        if (ImGui::Button(lbl, ImVec2(w, 34))) {
            g_dashPageOffset += kDashPageSize;
        }
        ImGui::Unindent(kRowIndent);
    }

    // --- Emit the spine line last, on the background channel --------------
    float spineBottomY = ImGui::GetCursorScreenPos().y;
    dl->ChannelsSetCurrent(0);
    if (spineBottomY > spineTopY) {
        dl->AddLine(ImVec2(spineX, spineTopY), ImVec2(spineX, spineBottomY), spineLineCol, 2.0f);
    }
    dl->ChannelsMerge();

    ImGui::EndChild();
}


// ---------------------------------------------------------------------------
// NOTES SCREEN ("Σημειώσεις") - standalone sticky-notes scratchpad, not tied
// to users/services/payments. Skips the RequestConfirm step (used elsewhere
// for business data) since this is a lightweight personal scratchpad -- a
// confirm-diff popup would be overkill for jotting down a note.
// ---------------------------------------------------------------------------
static bool g_showNoteDialog = false;
static bool g_noteDialogWasOpen = false;
static bool g_noteDialogIsEdit = false;
static int g_noteDialogId = -1;
static char nb_title[128] = "";
static char nb_content[2048] = "";

static void OpenAddNote() {
    g_showNoteDialog = true; g_noteDialogIsEdit = false; g_noteDialogId = -1;
    nb_title[0] = 0; nb_content[0] = 0;
}
static void OpenEditNote(const Note& n) {
    g_showNoteDialog = true; g_noteDialogIsEdit = true; g_noteDialogId = n.id;
    std::snprintf(nb_title, sizeof(nb_title), "%s", n.title.c_str());
    std::snprintf(nb_content, sizeof(nb_content), "%s", n.content.c_str());
}

static void DrawNoteDialog() {
    if (g_showNoteDialog && !g_noteDialogWasOpen) {
        ImGui::OpenPopup("Σημείωση");
    }
    g_noteDialogWasOpen = g_showNoteDialog;
    if (!g_showNoteDialog) return;
    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Appearing);
    if (BeginGuardedModal("Σημείωση", g_showNoteDialog, ImGuiWindowFlags_AlwaysAutoResize, []() {
        std::vector<FieldDiff> d; if (!g_noteDialogIsEdit) return d;
        for (auto& n : g_app.notes) if (n.id == g_noteDialogId) {
            DiffAdd(d, "Τίτλος", n.title, nb_title); DiffAdd(d, "Περιεχόμενο", n.content, nb_content);
        }
        return d; })) {
        ImGui::InputText("Τίτλος", nb_title, sizeof(nb_title));
        ImGui::InputTextMultiline("Περιεχόμενο", nb_content, sizeof(nb_content), ImVec2(0, 150));
        ImGui::Spacing();
        if (ImGui::Button("Αποθήκευση", ImVec2(150, 0))) {
            Note n; n.title = nb_title; n.content = nb_content;
            bool isEdit = g_noteDialogIsEdit; int nid = g_noteDialogId;
            RequestConfirm(isEdit ? "Αλλαγή σημείωσης" : "Νέα σημείωση", { {"Τίτλος", "-", n.title} }, [isEdit, nid, n]() {
                if (isEdit) g_app.conn->editNote(nid, n); else g_app.conn->addNote(n);
                g_app.reloadNotes();
            });
            g_showNoteDialog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Ακύρωση", ImVec2(150, 0))) { g_showNoteDialog = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

// Rotating pastel/warm "sticky note" background colors, cycled by note id so
// the board doesn't look uniform. Text stays dark since these are light fills.
static void NoteCardColor(int id, ImU32& bg, ImVec4& textCol) {
    static const ImU32 kBg[] = {
        IM_COL32(255, 235, 140, 255), // pastel yellow
        IM_COL32(255, 190, 170, 255), // pastel coral
        IM_COL32(170, 220, 255, 255), // pastel blue
        IM_COL32(190, 235, 170, 255), // pastel green
    };
    bg = kBg[((id % 4) + 4) % 4];
    textCol = ImVec4(0.12f, 0.12f, 0.14f, 1.0f);
}

// ---------------------------------------------------------------------------
// SEARCH SCREEN ("Αναζήτηση")
// ---------------------------------------------------------------------------
// Opens like a search engine: one big bar in the middle of an otherwise calm page. As soon as
// something is typed (or a category is chosen) the bar glides to the top and the results appear
// under it -- clients, services, apartments and buildings side by side in four equal columns
// (each with its own pager); payments and packages have their own category chips. One box
// searches EVERYTHING, Greek-aware (case, accents and the final sigma are ignored, so
// "παπαδοπουλος" finds "ΠΑΠΑΔΟΠΟΥΛΟΣ"), and every part of a result that matched is lit in yellow.
// Choosing a category adds a filter form whose fields follow the current data model.
// ---------------------------------------------------------------------------
static int g_sqCat = 0;   // 0 Όλα, 1 Πελάτες, 2 Πολυκατοικίες, 3 Διαμερίσματα, 4 Υπηρεσίες, 5 Πακέτα, 6 Πληρωμές, 7 Αποδείξεις
static const char* kSqCats[] = { "Όλα", "Πελάτες", "Πολυκατοικίες", "Διαμερίσματα", "Υπηρεσίες", "Πακέτα", "Πληρωμές", "Αποδείξεις", "Συνεργάτες", "Προσφορές", "Σημειώσεις" };
static void OpenPartnerProfile(int id);
static void OpenOffersPage();
static void OpenNoteFromSearch(const Note& n);
static TextBuf sq_all;
static TextBuf sq_u_name, sq_u_phone, sq_u_code, sq_u_addr, sq_u_area, sq_u_postal, sq_u_apt, sq_u_service;
static int sq_u_hasApt = 0;                       // 0 any, 1 has apartment, 2 none
static TextBuf sq_b_addr, sq_b_area, sq_b_postal, sq_b_afm;
static TextBuf sq_a_building, sq_a_label, sq_a_resident;
static int sq_a_vacant = 0;                       // 0 all, 1 occupied, 2 empty
static TextBuf sq_s_label, sq_s_notes, sq_s_pkg;
static double sq_s_min = 0.0, sq_s_max = 0.0;
static TextBuf sq_k_name, sq_k_service;
static TextBuf sq_p_client, sq_p_service, sq_p_notes, sq_p_building;
static std::string sq_p_from, sq_p_to;
static int sq_p_dir = 0, sq_p_status = 0;         // dir: 0 all/1 in/2 out; status: 0 all/1 paid/2 future/3 owed
static double sq_p_min = 0.0, sq_p_max = 0.0;
static std::vector<std::string> g_sqRecent;       // last searches (this session)
static float g_sqAnim = 0.0f;                     // 0 = bar centred (landing), 1 = bar on top (results)
static int g_sqLastFrame = -10;
static int g_sqPage[11] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };   // pager position per category
static std::string g_sqSig;                       // search signature: a change resets the pagers

static void ResetSearchForm() {
    sq_all.set("");
    for (TextBuf* b : { &sq_u_name, &sq_u_phone, &sq_u_code, &sq_u_addr, &sq_u_area, &sq_u_postal, &sq_u_apt, &sq_u_service,
                        &sq_b_addr, &sq_b_area, &sq_b_postal, &sq_b_afm, &sq_a_building, &sq_a_label, &sq_a_resident,
                        &sq_s_label, &sq_s_notes, &sq_s_pkg, &sq_k_name, &sq_k_service,
                        &sq_p_client, &sq_p_service, &sq_p_notes, &sq_p_building }) b->set("");
    sq_u_hasApt = 0; sq_a_vacant = 0; sq_s_min = sq_s_max = 0.0; sq_p_min = sq_p_max = 0.0;
    sq_p_from.clear(); sq_p_to.clear(); sq_p_dir = 0; sq_p_status = 0;
}

static bool SqHit(const std::string& hay, const TextBuf& needle) {
    std::string n = needle.str();
    return n.empty() || SubstrCI(hay, n);
}
static bool SqHit(const std::string& hay, const std::string& needle) { return needle.empty() || SubstrCI(hay, needle); }

// Draws `text` with every part that matches one of `needles` lit in yellow. Matching is
// done on the Greek-folded text; the fold keeps byte lengths, so the positions carry over.
static void SqText(const std::string& text, const std::vector<std::string>& needles) {
    if (text.empty()) { ImGui::TextUnformatted(""); return; }
    std::string f = FoldGreek(text);
    std::vector<std::pair<size_t, size_t>> rng;
    for (auto& n0 : needles) {
        if (n0.empty()) continue;
        std::string n = FoldGreek(n0);
        if (n.empty()) continue;
        for (size_t pos = 0; (pos = f.find(n, pos)) != std::string::npos; pos += n.size()) rng.push_back({ pos, pos + n.size() });
    }
    if (rng.empty() || f.size() != text.size()) { ImGui::TextUnformatted(text.c_str()); return; }
    std::sort(rng.begin(), rng.end());
    std::vector<std::pair<size_t, size_t>> merged;
    for (auto& r : rng) { if (!merged.empty() && r.first <= merged.back().second) merged.back().second = std::max(merged.back().second, r.second); else merged.push_back(r); }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float lineH = ImGui::GetTextLineHeight();
    size_t pos = 0; bool onLine = false;
    auto plain = [&](size_t a, size_t b) { if (b <= a) return; if (onLine) ImGui::SameLine(0, 0); ImGui::TextUnformatted(text.substr(a, b - a).c_str()); onLine = true; };
    for (auto& m : merged) {
        plain(pos, m.first);
        if (onLine) ImGui::SameLine(0, 0);
        std::string seg = text.substr(m.first, m.second - m.first);
        ImVec2 p = ImGui::GetCursorScreenPos(), sz = ImGui::CalcTextSize(seg.c_str());
        dl->AddRectFilled(ImVec2(p.x - 1, p.y), ImVec2(p.x + sz.x + 1, p.y + lineH), IM_COL32(255, 214, 40, 235), 3.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.08f, 0.10f, 1.0f));
        ImGui::TextUnformatted(seg.c_str());
        ImGui::PopStyleColor();
        onLine = true; pos = m.second;
    }
    plain(pos, text.size());
}
// truncate to the room left in the cell, then highlight
static void SqCell(const std::string& text, const std::vector<std::string>& needles) {
    SqText(TruncToWidth(text, ImGui::GetContentRegionAvail().x), needles);
}

struct SqBRow { Building b; int apts = 0, residents = 0; };
struct SqARow { Apartment a; Building b; std::string current; int moves = 0; };
struct SqResults {
    std::vector<const User*> users;
    std::vector<SqBRow> buildings;
    std::vector<SqARow> apartments;
    std::vector<const Service*> services;
    std::vector<const DataConn::MasterServiceSummary*> packages;
    std::vector<Payment> payments;           // the rows to list (only fetched for the open category, capped)
    long long payCount = 0;                  // all matches, for the chip / header
    double payDue = 0.0, payPaid = 0.0;
    std::vector<Receipt> receipts;           // only fetched for the open category (capped)
    long long rcCount = 0; double rcTotal = 0.0;
    std::vector<Partner> partners;           // business partners (Συνεργάτες)
    std::vector<Offer> offers;               // offers (Προσφορές)
    std::vector<Note> notes;                 // notes (Σημειώσεις)
    int Count(int cat) const {
        switch (cat) { case 1: return (int)users.size(); case 2: return (int)buildings.size(); case 3: return (int)apartments.size();
                       case 4: return (int)services.size(); case 5: return (int)packages.size(); case 6: return (int)payCount;
                       case 7: return (int)rcCount; case 8: return (int)partners.size(); case 9: return (int)offers.size(); case 10: return (int)notes.size(); default: return 0; }
    }
    int Total() const { int t = 0; for (int c = 1; c <= 10; c++) t += Count(c); return t; }
};

struct SqContext {
    std::map<int, const User*> userById;
    std::map<int, const Service*> svcById;
    std::vector<DataConn::MasterServiceSummary> summaries;
    std::map<int, std::string> pkgsOfService;
    std::vector<Building> buildings;
    std::map<int, std::vector<Apartment>> aptsOf;            // building -> apartments
    std::map<int, std::string> pastResidents;                // apartment -> names of everyone who ever lived there
    std::map<int, int> aptMoves;                             // apartment -> movements
};

// Fills ONE category of `out`. `form` = also apply that category's filter form.
static void SqCompute(int cat, bool form, const std::string& quick, SqResults& out, const SqContext& cx) {
    switch (cat) {
    case 10:    // notes: title and text
        for (auto& n : g_app.notes) if (!quick.empty() && SqHit(n.title + " " + n.content, quick)) out.notes.push_back(n);
        if (g_sqCat == 10 && quick.empty()) out.notes = g_app.notes;
        break;
    case 9:     // offers: name, note, who and what they are for
        for (auto& o : g_app.conn->getOffers()) {
            std::string blob = o.name + " " + o.note + " " + (o.user_id > 0 ? g_app.userName(o.user_id) : std::string()) + " " + o.target_type + " " + o.valid_from + " " + o.valid_to;
            if (quick.empty() ? g_sqCat == 9 : SqHit(blob, quick)) out.offers.push_back(o);
        }
        break;
    case 8:     // business partners: name / trade / phone / email / notes
        if (!quick.empty() || g_sqCat == 8) out.partners = g_app.conn->pagePartners(quick, false, {}, 0, 500);
        break;
    case 7: {   // receipts: number / handwritten / bank reference / RF / client name
        DataConn::ReceiptQuery q; q.text = quick;
        out.rcCount = g_app.conn->countReceipts(q, &out.rcTotal);
        if (form) out.receipts = g_app.conn->searchReceipts(q, 0, 500);
        else if (g_sqCat == 0 && !quick.empty()) out.receipts = g_app.conn->searchReceipts(q, 0, 200);
        break;
    }
    case 1: {
        std::set<int> bought; const bool useBought = form && !sq_u_service.str().empty();
        if (useBought) {
            DataConn::PaymentFilter f; f.serviceText = sq_u_service.str();
            for (auto& g : g_app.conn->paymentTotalsBy(DataConn::PayGroup::User, f)) bought.insert(std::atoi(g.key.c_str()));
        }
        for (auto& u : g_app.users) {
            std::string blob = u.full_name + " " + u.phone + " " + u.special_code + " " + u.address + " " + u.area + " " + u.postal_code + " " + u.apartment;
            if (!SqHit(blob, quick)) continue;
            if (form) {
                if (!SqHit(u.full_name, sq_u_name) || !SqHit(u.phone, sq_u_phone) || !SqHit(u.special_code, sq_u_code) ||
                    !SqHit(u.address, sq_u_addr) || !SqHit(u.area, sq_u_area) || !SqHit(u.postal_code, sq_u_postal) ||
                    !SqHit(u.apartment, sq_u_apt)) continue;
                if (sq_u_hasApt == 1 && u.apartment_id < 0) continue;
                if (sq_u_hasApt == 2 && u.apartment_id >= 0) continue;
                if (useBought && !bought.count(u.id)) continue;
            }
            out.users.push_back(&u);
        }
        break; }
    case 2:
        for (auto& b : cx.buildings) {
            std::string blob = b.address + " " + b.area + " " + b.postal_code + " " + b.afm;
            if (!SqHit(blob, quick)) continue;
            if (form && (!SqHit(b.address, sq_b_addr) || !SqHit(b.area, sq_b_area) || !SqHit(b.postal_code, sq_b_postal) || !SqHit(b.afm, sq_b_afm))) continue;
            SqBRow r; r.b = b;
            std::set<int> aptIds, active;
            auto it = cx.aptsOf.find(b.id);
            if (it != cx.aptsOf.end()) for (auto& a : it->second) aptIds.insert(a.id);
            for (auto& u : g_app.users) if (aptIds.count(u.apartment_id)) { r.residents++; active.insert(u.apartment_id); }
            r.apts = (int)active.size();
            out.buildings.push_back(std::move(r));
        }
        break;
    case 3:
        for (auto& b : cx.buildings) {
            auto it = cx.aptsOf.find(b.id); if (it == cx.aptsOf.end()) continue;
            for (auto& a : it->second) {
                SqARow r; r.a = a; r.b = b;
                for (auto& u : g_app.users) if (u.apartment_id == a.id) r.current += (r.current.empty() ? "" : ", ") + u.full_name;
                auto pr = cx.pastResidents.find(a.id);
                std::string blob = b.address + " " + b.area + " " + b.afm + " " + a.label + " " + r.current + " " + (pr != cx.pastResidents.end() ? pr->second : "");
                if (!SqHit(blob, quick)) continue;
                if (form) {
                    if (!SqHit(b.address, sq_a_building) || !SqHit(a.label, sq_a_label) ||
                        !SqHit(r.current + " " + (pr != cx.pastResidents.end() ? pr->second : ""), sq_a_resident)) continue;
                    if (sq_a_vacant == 1 && r.current.empty()) continue;
                    if (sq_a_vacant == 2 && !r.current.empty()) continue;
                }
                auto mv = cx.aptMoves.find(a.id); r.moves = mv != cx.aptMoves.end() ? mv->second : 0;
                out.apartments.push_back(std::move(r));
            }
        }
        break;
    case 4:
        for (auto& s : g_app.services) {
            auto pit = cx.pkgsOfService.find(s.id);
            std::string pk = pit != cx.pkgsOfService.end() ? pit->second : "";
            if (!SqHit(s.label + " " + s.extra_notes + " " + pk, quick)) continue;
            if (form) {
                if (!SqHit(s.label, sq_s_label) || !SqHit(s.extra_notes, sq_s_notes) || !SqHit(pk, sq_s_pkg)) continue;
                if (sq_s_min > 0.0 && s.suggested_price < sq_s_min) continue;
                if (sq_s_max > 0.0 && s.suggested_price > sq_s_max) continue;
            }
            out.services.push_back(&s);
        }
        break;
    case 5:
        for (auto& ms : cx.summaries) {
            std::string comps;
            for (auto& c : ms.components) comps += c.label + ", ";
            if (!SqHit(ms.masterService.label + " " + comps, quick)) continue;
            if (form && (!SqHit(ms.masterService.label, sq_k_name) || !SqHit(comps, sq_k_service))) continue;
            out.packages.push_back(&ms);
        }
        break;
    case 6: {
        // In SQL, with the same accent/case-insensitive matching (fold()) as the rest of the search screen.
        DataConn::PaymentFilter f; f.text = quick;
        if (form) {
            f.clientText = sq_p_client.str(); f.serviceText = sq_p_service.str();
            f.notesText = sq_p_notes.str();   f.buildingText = sq_p_building.str();
            f.fromDate = sq_p_from; f.toDate = sq_p_to;
            if (sq_p_dir == 1) f.direction = "in";
            if (sq_p_dir == 2) f.direction = "out";
            if (sq_p_status == 1) f.owed = 1;          // settled
            if (sq_p_status == 2) f.status = 2;        // future
            if (sq_p_status == 3) f.owed = 2;          // still owed
            f.minDue = sq_p_min; f.maxDue = sq_p_max;
        }
        auto t = g_app.conn->paymentTotals(f);
        out.payCount = t.count; out.payDue = t.dueAll; out.payPaid = t.paidAll;
        if (form) out.payments = g_app.conn->findPayments(f, DataConn::PayOrder::DateAsc, 0, 5000);   // oldest first, as before
        else if (g_sqCat == 0 && !quick.empty()) out.payments = g_app.conn->findPayments(f, DataConn::PayOrder::DateDesc, 0, 200);
        break; }
    default: break;
    }
}

// The text the user typed for this category (quick box + its filter fields) -> what to light up.
static std::vector<std::string> SqNeedles(int cat, const std::string& quick) {
    std::vector<std::string> v; if (!quick.empty()) v.push_back(quick);
    auto add = [&](const TextBuf& b) { if (!b.str().empty()) v.push_back(b.str()); };
    if (cat == 1) { add(sq_u_name); add(sq_u_phone); add(sq_u_code); add(sq_u_addr); add(sq_u_area); add(sq_u_postal); add(sq_u_apt); }
    if (cat == 2) { add(sq_b_addr); add(sq_b_area); add(sq_b_postal); add(sq_b_afm); }
    if (cat == 3) { add(sq_a_building); add(sq_a_label); add(sq_a_resident); }
    if (cat == 4) { add(sq_s_label); add(sq_s_notes); add(sq_s_pkg); }
    if (cat == 5) { add(sq_k_name); add(sq_k_service); }
    if (cat == 6) { add(sq_p_client); add(sq_p_service); add(sq_p_notes); add(sq_p_building); }
    return v;
}

// Section title with a coloured accent bar and the result count.
static void SqSection(const char* title, int count, ImVec4 accent) {
    ImGui::Spacing();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + 2), ImVec2(p.x + 4, p.y + ImGui::GetTextLineHeight() + 2),
                                              ImGui::ColorConvertFloat4ToU32(accent), 2.0f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12);
    ImGui::TextColored(ImVec4(0.92f, 0.95f, 1.0f, 1.0f), "%s", title);
    ImGui::SameLine();
    ImGui::TextDisabled("%d αποτελέσματα", count);
    ImGui::Spacing();
}

// Pager helpers for a result list: slice [from,to) of `total` rows at pageSize per page.
static int SqPageRange(int cat, int total, int pageSize, int& from, int& to) {
    int pages = std::max(1, (total + pageSize - 1) / pageSize);
    int& pg = g_sqPage[cat];
    pg = std::max(0, std::min(pg, pages - 1));
    from = pg * pageSize; to = std::min(total, from + pageSize);
    return pages;
}
static void SqPagerDraw(int cat, int total, int pages, const char* id) {
    int& pg = g_sqPage[cat];
    ImGui::PushID(id);
    if (pg == 0) ImGui::BeginDisabled();
    if (ImGui::SmallButton("◄##p")) pg--;
    if (pg == 0) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%d / %d  ·  %d", pg + 1, pages, total);
    ImGui::SameLine();
    if (pg >= pages - 1) ImGui::BeginDisabled();
    if (ImGui::SmallButton("►##n")) pg++;
    if (pg >= pages - 1) ImGui::EndDisabled();
    ImGui::PopID();
}

// A pill-shaped toggle button; returns true on click.
static bool SqChip(const char* label, bool active) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 16.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14, 6));
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f, 0.55f, 0.75f, 1.0f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.18f, 0.24f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.30f, 0.40f, 1.0f));
    }
    bool c = ImGui::Button(label);
    ImGui::PopStyleColor(active ? 1 : 2);
    ImGui::PopStyleVar(2);
    return c;
}
static float SqChipWidth(const char* label) { return ImGui::CalcTextSize(label).x + 28.0f; }

// First cell of a result row: a link-coloured, highlighted text that is also the click target.
static bool SqLinkCell(const std::string& text, const std::vector<std::string>& needles) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::Selectable("##lc", false, 0, ImVec2(0, 0));
    ImVec2 endPos = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(p);
    ImGui::PushStyleColor(ImGuiCol_Text, kColLink);
    SqText(TruncToWidth(text, ImGui::GetContentRegionAvail().x), needles);
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos(endPos);
    return clicked;
}

// One result "card" row in a column: two lines over a clickable area. Returns true on click.
static bool SqRow(const std::string& l1, const std::string& l2, const std::vector<std::string>& hl, float width, bool l1Link) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::Selectable("##row", false, 0, ImVec2(0, 46));
    ImVec2 endPos = ImGui::GetCursorScreenPos();
    const float lh = ImGui::GetTextLineHeight();
    ImGui::SetCursorScreenPos(ImVec2(p.x + 8, p.y + 3));
    if (l1Link) ImGui::PushStyleColor(ImGuiCol_Text, kColLink);
    SqText(TruncToWidth(l1, width - 20), hl);
    if (l1Link) ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos(ImVec2(p.x + 8, p.y + 5 + lh));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.67f, 0.76f, 1.0f));
    SqText(TruncToWidth(l2, width - 20), hl);
    ImGui::PopStyleColor();
    ImGui::SetCursorScreenPos(endPos);
    return clicked;
}

static void DrawSearchScreen() {
    ImGuiIO& io = ImGui::GetIO();
    const float availW = ImGui::GetContentRegionAvail().x, availH = ImGui::GetContentRegionAvail().y;
    const float sp = ImGui::GetStyle().ItemSpacing.x;

    // ---- data used by every category (cached: was rebuilt every frame with a query per apartment) ----
    static Cached<SqContext> cxCache;
    const SqContext& cx = cxCache.get("", [] {
    SqContext cx;
    for (auto& u : g_app.users) cx.userById[u.id] = &u;
    for (auto& s : g_app.services) cx.svcById[s.id] = &s;
    for (auto& ms : g_app.conn->getMasterServices()) {      // packages and their services (no client rosters: unused here)
        DataConn::MasterServiceSummary sm; sm.masterService = ms; sm.components = g_app.conn->getMasterServiceComponents(ms.id);
        cx.summaries.push_back(std::move(sm));
    }
    for (auto& ms : cx.summaries)
        for (auto& c : ms.components) cx.pkgsOfService[c.id] += (cx.pkgsOfService[c.id].empty() ? "" : ", ") + ms.masterService.label;
    cx.buildings = g_app.conn->getBuildings();
    for (auto& b : cx.buildings) {
        cx.aptsOf[b.id] = g_app.conn->getApartmentsForBuilding(b.id);
        for (auto& a : cx.aptsOf[b.id]) {
            std::set<int> seen;
            for (auto& r : g_app.conn->getResidencies(a.id)) if (seen.insert(r.user_id).second) cx.pastResidents[a.id] += (cx.pastResidents[a.id].empty() ? "" : ", ") + g_app.userName(r.user_id);
        }
    }
    {
        DataConn::PaymentFilter f; f.status = 1;
        for (auto& g : g_app.conn->paymentTotalsBy(DataConn::PayGroup::Apartment, f))
            if (int apt = std::atoi(g.key.c_str()); apt > 0) cx.aptMoves[apt] = (int)g.t.count;
    }
    return cx;
    });
    const int nApts = [&] { int n = 0; for (auto& kv : cx.aptsOf) n += (int)kv.second.size(); return n; }();

    // ---- landing <-> results animation -------------------------------------------
    const bool landing = (g_sqCat == 0 && sq_all.str().empty());
    g_sqAnim += ((landing ? 0.0f : 1.0f) - g_sqAnim) * std::min(1.0f, io.DeltaTime * 9.0f);
    if (std::fabs((landing ? 0.0f : 1.0f) - g_sqAnim) < 0.003f) g_sqAnim = landing ? 0.0f : 1.0f;
    const float k = g_sqAnim;                              // 0 landing .. 1 results
    const bool firstFrame = (ImGui::GetFrameCount() - g_sqLastFrame) > 2;
    g_sqLastFrame = ImGui::GetFrameCount();

    // ---- title (fades out while the bar moves up) --------------------------------
    ImGui::Dummy(ImVec2(0, 6.0f + (1.0f - k) * std::max(0.0f, availH * 0.20f - 40.0f)));
    if (k < 0.97f) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 1.0f - k);
        const char* title = "Αναζήτηση";
        ImGui::SetWindowFontScale(2.0f);
        float tw = ImGui::CalcTextSize(title).x;
        ImGui::SetCursorPosX((availW - tw) * 0.5f);
        ImGui::TextColored(ImVec4(0.90f, 0.94f, 1.0f, 1.0f), "%s", title);
        ImGui::SetWindowFontScale(1.0f);
        const char* sub = "Πελάτες, πολυκατοικίες, διαμερίσματα, υπηρεσίες, πακέτα, πληρωμές και αποδείξεις — σε ένα σημείο";
        ImGui::SetCursorPosX((availW - ImGui::CalcTextSize(sub).x) * 0.5f);
        ImGui::TextDisabled("%s", sub);
        ImGui::PopStyleVar();
        ImGui::Dummy(ImVec2(0, (1.0f - k) * 14.0f));
    }

    // ---- the big bar -------------------------------------------------------------
    const float barW = std::min(availW - 80.0f, 520.0f + (1.0f - k) * 260.0f);
    const float scale = 1.0f + 0.30f * (1.0f - k);
    ImGui::SetCursorPosX((availW - barW) * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 18.0f + 10.0f * (1.0f - k));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(22.0f, 9.0f + 6.0f * (1.0f - k)));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.30f, 0.62f, 0.85f, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.13f, 0.15f, 0.20f, 1.0f));
    ImGui::SetWindowFontScale(scale);
    if (firstFrame) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(barW);
    ImGui::InputTextWithHint("##sqall", "Αναζητήστε πελάτη, πολυκατοικία, διαμέρισμα, υπηρεσία, πληρωμή...", sq_all.data, sizeof(sq_all.data));
    const bool commit = ImGui::IsItemDeactivatedAfterEdit();
    if (ImGui::IsItemActive() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { ResetSearchForm(); g_sqCat = 0; }
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
    ImGui::SameLine(0, 6);
    if (!sq_all.str().empty() || g_sqCat != 0) {
        if (ImGui::Button("×", ImVec2(24 * scale, 0))) ResetSearchForm(), g_sqCat = 0;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Καθαρισμός αναζήτησης");
    }
    const std::string quick = sq_all.str();
    if (commit && !quick.empty()) {
        g_sqRecent.erase(std::remove(g_sqRecent.begin(), g_sqRecent.end(), quick), g_sqRecent.end());
        g_sqRecent.insert(g_sqRecent.begin(), quick);
        if (g_sqRecent.size() > 6) g_sqRecent.resize(6);
    }

    // ---- results for every category (counts on the chips) -------------------------
    // recomputed only when the text, a form field, the category or the data changes (was: every frame)
    auto computeResults = [&] {
        static Cached<SqResults> resCache;
        std::string key = quick + '\x1f' + std::to_string(g_sqCat);
        for (TextBuf* b : { &sq_u_name, &sq_u_phone, &sq_u_code, &sq_u_addr, &sq_u_area, &sq_u_postal, &sq_u_apt, &sq_u_service,
                            &sq_b_addr, &sq_b_area, &sq_b_postal, &sq_b_afm, &sq_a_building, &sq_a_label, &sq_a_resident,
                            &sq_s_label, &sq_s_notes, &sq_s_pkg, &sq_k_name, &sq_k_service,
                            &sq_p_client, &sq_p_service, &sq_p_notes, &sq_p_building }) key += '\x1f' + b->str();
        char nums[160];
        std::snprintf(nums, sizeof(nums), "|%d|%d|%d|%d|%.2f|%.2f|%.2f|%.2f|", sq_u_hasApt, sq_a_vacant, sq_p_dir, sq_p_status, sq_s_min, sq_s_max, sq_p_min, sq_p_max);
        key += nums + sq_p_from + "|" + sq_p_to;
        // Debounce: while the text/form is still changing (typing), keep showing the previous results; search
        // once input has been still for 250 ms. On a large database one search can take a second.
        // Only typing is debounced; a category switch or a quick-search click is answered at once (the old
        // results would belong to another category).
        static std::string pendingKey; static double changedAt = 0.0; static int lastCat = -1;
        if (key != pendingKey) { pendingKey = key; changedAt = ImGui::GetTime(); }
        bool sameCat = lastCat == g_sqCat; lastCat = g_sqCat;
        if (sameCat && resCache.ver >= 0 && key != resCache.key && ImGui::GetTime() - changedAt < 0.25) return resCache.val;
        return resCache.get(key, [&] {
            SqResults r;
            for (int c = 1; c <= 10; c++) SqCompute(c, c == g_sqCat, quick, r, cx);
            return r;
        });
    };
    SqResults res = computeResults();

    // ---- category chips (centred) ------------------------------------------------
    {
        std::vector<std::string> labels;
        float total = 0.0f;
        for (int i = 0; i < IM_ARRAYSIZE(kSqCats); i++) {
            std::string l = kSqCats[i];
            if (i > 0 && (!quick.empty() || g_sqCat == i)) l += "  " + std::to_string(res.Count(i));
            labels.push_back(l);
            total += SqChipWidth(l.c_str()) + (i ? sp : 0.0f);
        }
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::SetCursorPosX(std::max(0.0f, (availW - total) * 0.5f));
        for (int i = 0; i < IM_ARRAYSIZE(kSqCats); i++) {
            if (i) ImGui::SameLine();
            ImGui::PushID(i);
            if (SqChip(labels[i].c_str(), g_sqCat == i)) g_sqCat = i;
            ImGui::PopID();
        }
    }

    // ---------------------------------------------------------------- landing extras
    if (landing && k < 0.15f) {
        ImGui::Dummy(ImVec2(0, 22));
        auto centeredText = [&](const char* t, ImVec4 col) {
            ImGui::SetCursorPosX((availW - ImGui::CalcTextSize(t).x) * 0.5f); ImGui::TextColored(col, "%s", t);
        };
        centeredText("Γρήγορες αναζητήσεις", ImVec4(0.62f, 0.68f, 0.78f, 1.0f));
        struct Quick { const char* label; int cat; };
        const Quick qs[] = { {"Ανεξόφλητες πληρωμές", 6}, {"Μελλοντικές πληρωμές", 6}, {"Πληρωμές εκ μέρους πελάτη", 6},
                             {"Πελάτες χωρίς διαμέρισμα", 1}, {"Κενά διαμερίσματα", 3}, {"Υπηρεσίες άνω των 50 €", 4} };
        for (int row = 0; row < 2; row++) {
            float w = 0; for (int j = 0; j < 3; j++) w += SqChipWidth(qs[row * 3 + j].label) + (j ? sp : 0);
            ImGui::SetCursorPosX(std::max(0.0f, (availW - w) * 0.5f));
            for (int j = 0; j < 3; j++) {
                const Quick& q = qs[row * 3 + j];
                if (j) ImGui::SameLine();
                ImGui::PushID(row * 3 + j);
                if (SqChip(q.label, false)) {
                    ResetSearchForm(); g_sqCat = q.cat;
                    int idx = row * 3 + j;
                    if (idx == 0) sq_p_status = 3;
                    if (idx == 1) sq_p_status = 2;
                    if (idx == 2) sq_p_dir = 2;
                    if (idx == 3) sq_u_hasApt = 2;
                    if (idx == 4) sq_a_vacant = 2;
                    if (idx == 5) sq_s_min = 50.0;
                }
                ImGui::PopID();
            }
        }
        if (!g_sqRecent.empty()) {
            ImGui::Dummy(ImVec2(0, 16));
            centeredText("Πρόσφατες αναζητήσεις", ImVec4(0.62f, 0.68f, 0.78f, 1.0f));
            float w = 0; for (size_t i = 0; i < g_sqRecent.size(); i++) w += SqChipWidth(g_sqRecent[i].c_str()) + (i ? sp : 0);
            ImGui::SetCursorPosX(std::max(0.0f, (availW - w) * 0.5f));
            for (size_t i = 0; i < g_sqRecent.size(); i++) {
                if (i) ImGui::SameLine();
                ImGui::PushID((int)i + 100);
                if (SqChip(g_sqRecent[i].c_str(), false)) sq_all.set(g_sqRecent[i]);
                ImGui::PopID();
            }
        }
        ImGui::Dummy(ImVec2(0, 22));
        char stats[200];
        std::snprintf(stats, sizeof(stats), "%d πελάτες  ·  %d πολυκατοικίες  ·  %d διαμερίσματα  ·  %d υπηρεσίες  ·  %d πακέτα  ·  %d πληρωμές",
                      (int)g_app.users.size(), (int)cx.buildings.size(), nApts, (int)g_app.services.size(), (int)cx.summaries.size(), (int)[] {
                          static Cached<long long> n; return n.get("", [] { return g_app.conn->countPaymentsWhere({}); }); }());
        centeredText(stats, ImVec4(0.45f, 0.50f, 0.58f, 1.0f));
        return;
    }
    if (landing) return;                       // still gliding up: nothing else yet

    ImGui::Separator();

    // ---------------------------------------------------------------- filter form
    if (g_sqCat != 0) {
        ImGui::BeginChild("sqform", ImVec2(0, 0), ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY);
        ImGui::TextColored(ImVec4(0.70f, 0.85f, 1.0f, 1.0f), "Φίλτρα — %s", kSqCats[g_sqCat]);
        ImGui::SameLine();
        if (ImGui::SmallButton("Καθαρισμός φίλτρων")) { std::string keep = sq_all.str(); ResetSearchForm(); sq_all.set(keep); }
        auto field = [](const char* hint, TextBuf& b, float w) {
            ImGui::SetNextItemWidth(w);
            ImGui::InputTextWithHint((std::string("##sq") + hint).c_str(), hint, b.data, sizeof(b.data));
        };
        auto money = [](const char* hint, double& v, float w) {
            ImGui::SetNextItemWidth(w);
            InputMoney((std::string("##sqm") + hint).c_str(), &v);
            ImGui::SameLine(); ImGui::TextDisabled("%s", hint);
        };
        switch (g_sqCat) {
        case 1:
            field("Ονοματεπώνυμο", sq_u_name, 190); ImGui::SameLine(); field("Τηλέφωνο", sq_u_phone, 130); ImGui::SameLine();
            field("Ειδικός κωδικός", sq_u_code, 130); ImGui::SameLine(); field("Υπηρεσία που έχει αγοράσει", sq_u_service, 210);
            field("Διεύθυνση", sq_u_addr, 190); ImGui::SameLine(); field("Περιοχή", sq_u_area, 130); ImGui::SameLine();
            field("Τ.Κ.", sq_u_postal, 80); ImGui::SameLine(); field("Διαμέρισμα", sq_u_apt, 110); ImGui::SameLine();
            ImGui::SetNextItemWidth(150);
            { const char* names[] = { "Κατοικία: όλοι", "Με διαμέρισμα", "Χωρίς διαμέρισμα" }; ImGui::Combo("##sqhasapt", &sq_u_hasApt, names, 3); }
            break;
        case 2:
            field("Διεύθυνση", sq_b_addr, 200); ImGui::SameLine(); field("Περιοχή", sq_b_area, 140); ImGui::SameLine();
            field("Τ.Κ.", sq_b_postal, 80); ImGui::SameLine(); field("ΑΦΜ", sq_b_afm, 130);
            break;
        case 3:
            field("Πολυκατοικία", sq_a_building, 200); ImGui::SameLine(); field("Διαμέρισμα", sq_a_label, 120); ImGui::SameLine();
            field("Κάτοικος (τωρινός ή παλιός)", sq_a_resident, 230); ImGui::SameLine();
            ImGui::SetNextItemWidth(150);
            { const char* names[] = { "Όλα", "Κατοικημένα", "Κενά" }; ImGui::Combo("##sqvacant", &sq_a_vacant, names, 3); }
            break;
        case 4:
            field("Ετικέτα", sq_s_label, 220); ImGui::SameLine(); field("Σημειώσεις", sq_s_notes, 220); ImGui::SameLine();
            field("Σε πακέτο", sq_s_pkg, 180);
            money("τιμή από", sq_s_min, 80); ImGui::SameLine(); money("έως", sq_s_max, 80);
            break;
        case 5:
            field("Όνομα πακέτου", sq_k_name, 220); ImGui::SameLine(); field("Περιέχει υπηρεσία", sq_k_service, 220);
            break;
        case 6: {
            ImGui::TextDisabled("Από:"); ImGui::SameLine(); DatePicker("sqfrom", sq_p_from, ImVec2(0, 1));
            ImGui::SameLine(); ImGui::TextDisabled("Έως:"); ImGui::SameLine(); DatePicker("sqto", sq_p_to, ImVec2(0, 1));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(130);
            { const char* d[] = { "Τύπος: όλοι", "Εισπράξεις", "Πληρωμές" }; ImGui::Combo("##sqdir", &sq_p_dir, d, 3); }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(150);
            { const char* st[] = { "Κατάσταση: όλες", "Εξοφλημένες", "Μελλοντικές", "Με υπόλοιπο" }; ImGui::Combo("##sqstatus", &sq_p_status, st, 4); }
            field("Πελάτης", sq_p_client, 190); ImGui::SameLine(); field("Υπηρεσία", sq_p_service, 200); ImGui::SameLine();
            field("Πολυκατοικία", sq_p_building, 170); ImGui::SameLine(); field("Σημειώσεις", sq_p_notes, 170);
            money("ποσό από", sq_p_min, 80); ImGui::SameLine(); money("έως", sq_p_max, 80);
            break; }
        }
        ImGui::EndChild();
        // the form may have changed: recompute the selected category with the new values
        const SqResults& again = computeResults();
        switch (g_sqCat) {
            case 1: res.users = again.users; break;           case 2: res.buildings = again.buildings; break;
            case 3: res.apartments = again.apartments; break; case 4: res.services = again.services; break;
            case 5: res.packages = again.packages; break;
            case 6: res.payments = again.payments; res.payCount = again.payCount; res.payDue = again.payDue; res.payPaid = again.payPaid; break;
        }
    }

    // any change of what is searched puts every pager back on page 1
    {
        std::string sig = quick + "|" + std::to_string(g_sqCat) + "|" + std::to_string(res.Total());
        for (int c = 1; c <= 6; c++) for (auto& n : SqNeedles(c, "")) sig += "|" + n;
        sig += "|" + std::to_string(sq_u_hasApt) + std::to_string(sq_a_vacant) + std::to_string(sq_p_dir) + std::to_string(sq_p_status) + sq_p_from + sq_p_to;
        if (sig != g_sqSig) { g_sqSig = sig; for (int& p : g_sqPage) p = 0; }
    }

    const bool all = (g_sqCat == 0);
    const int total = all ? res.Total() : res.Count(g_sqCat);
    if (total == 0) {
        ImGui::Dummy(ImVec2(0, 40));
        std::string msg = quick.empty() ? std::string("Κανένα αποτέλεσμα με αυτά τα φίλτρα.") : "Δεν βρέθηκαν αποτελέσματα για «" + quick + "».";
        ImGui::SetCursorPosX(std::max(0.0f, (availW - ImGui::CalcTextSize(msg.c_str()).x) * 0.5f));
        ImGui::TextColored(ImVec4(0.85f, 0.88f, 0.95f, 1.0f), "%s", msg.c_str());
        const char* tip = "Δοκιμάστε λιγότερες λέξεις, χωρίς τόνους, ή αφαιρέστε φίλτρα.";
        ImGui::SetCursorPosX(std::max(0.0f, (availW - ImGui::CalcTextSize(tip).x) * 0.5f));
        ImGui::TextDisabled("%s", tip);
        return;
    }

    // ================================================================ ALL: one column per kind; an empty one takes half the width
    if (all) {
        const float resH = ImGui::GetContentRegionAvail().y;
        const int pageSize = std::max(4, (int)((resH - 130.0f) / 50.0f));
        struct Col { int cat; const char* title; ImVec4 accent; };
        const Col cols[9] = { {1, "Πελάτες", ImVec4(0.35f, 0.65f, 1.0f, 1)}, {4, "Υπηρεσίες", ImVec4(0.95f, 0.75f, 0.30f, 1)},
                              {3, "Διαμερίσματα", ImVec4(0.55f, 0.85f, 0.45f, 1)}, {2, "Πολυκατοικίες", ImVec4(0.30f, 0.80f, 0.70f, 1)},
                              {6, "Πληρωμές", ImVec4(0.95f, 0.45f, 0.55f, 1)}, {7, "Αποδείξεις", ImVec4(0.55f, 0.85f, 0.75f, 1)}, {8, "Συνεργάτες", ImVec4(0.80f, 0.60f, 0.95f, 1)},
                              {9, "Προσφορές", ImVec4(0.95f, 0.65f, 0.35f, 1)}, {10, "Σημειώσεις", ImVec4(0.90f, 0.85f, 0.45f, 1)} };
        float weights[9], wsum = 0;
        for (int i = 0; i < 9; i++) { weights[i] = res.Count(cols[i].cat) > 0 ? 1.0f : 0.5f; wsum += weights[i]; }
        const float usable = ImGui::GetContentRegionAvail().x - 8.0f * sp;
        for (int ci = 0; ci < 9; ci++) {
            const Col& c = cols[ci];
            const float colW = usable * weights[ci] / wsum;
            if (ci) ImGui::SameLine();
            ImGui::BeginChild((std::string("sqcol") + std::to_string(c.cat)).c_str(), ImVec2(colW, resH), true);
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + 2), ImVec2(p.x + 4, p.y + ImGui::GetTextLineHeight() + 2), ImGui::ColorConvertFloat4ToU32(c.accent), 2.0f);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12);
            ImGui::TextColored(ImVec4(0.92f, 0.95f, 1.0f, 1.0f), "%s", c.title);
            ImGui::SameLine(); ImGui::TextDisabled("%d", res.Count(c.cat));
            ImGui::Separator();
            // payments / receipts: only the first rows are loaded here; the category chip opens the full list
            const int cnt = c.cat == 6 ? (int)res.payments.size() : c.cat == 7 ? (int)res.receipts.size() : res.Count(c.cat);   // (partners: Count)
            int from, to; int pages = SqPageRange(c.cat, cnt, pageSize, from, to);
            const auto hl = SqNeedles(c.cat, quick);
            if (res.Count(c.cat) == 0) ImGui::TextDisabled("Κανένα αποτέλεσμα.");
            for (int i = from; i < to; i++) {
                ImGui::PushID(i);
                if (c.cat == 1) {
                    const User& u = *res.users[i];
                    std::string l2 = u.phone + (u.address.empty() ? "" : "  ·  " + u.address + (u.apartment.empty() ? "" : ", " + u.apartment));
                    if (SqRow(u.full_name, l2, hl, colW, true)) OpenUserProfile(u.id);
                } else if (c.cat == 4) {
                    const Service& s = *res.services[i];
                    char pr[32]; std::snprintf(pr, sizeof(pr), "%s", Eur(s.suggested_price).c_str());
                    auto pit = cx.pkgsOfService.find(s.id);
                    if (SqRow(s.label, std::string(pr) + (pit != cx.pkgsOfService.end() ? "  ·  " + pit->second : ""), hl, colW, true)) OpenServiceDetail(s.id);
                } else if (c.cat == 3) {
                    const SqARow& a = res.apartments[i];
                    if (SqRow(a.b.address + " — " + a.a.label, (a.current.empty() ? "κενό" : a.current) + "  ·  " + std::to_string(a.moves) + " κινήσεις", hl, colW, true)) { g_aptGotoId = a.a.id; g_tabToApartments = true; }
                } else if (c.cat == 2) {
                    const SqBRow& b = res.buildings[i];
                    std::string l2 = b.b.area + " · Τ.Κ. " + b.b.postal_code + " · ΑΦΜ " + (b.b.afm.empty() ? "—" : b.b.afm) + " · " + std::to_string(b.apts) + " διαμ.";
                    if (SqRow(b.b.address, l2, hl, colW, true)) { g_aptGotoBuilding = b.b.id; g_tabToApartments = true; }
                } else if (c.cat == 6) {
                    const Payment& py = res.payments[i];
                    char d[64]; std::snprintf(d, sizeof(d), "%04d-%02d-%02d · %s / %s", py.year, py.month, py.day,
                                              FormatMoney(py.amount_paid).c_str(), Eur(py.amount_due).c_str());
                    if (SqRow(g_app.userName(py.user_id), ChargeLabel(py) + "  ·  " + d, hl, colW, true)) OpenPaymentView(py.id);
                } else if (c.cat == 9) {
                    const Offer& o = res.offers[i]; ImVec4 oc; std::string st = OfferStateText(o, oc);
                    if (SqRow(o.name, OfferValueText(o) + "  ·  " + st, hl, colW, true)) OpenOffersPage();
                } else if (c.cat == 10) {
                    const Note& n = res.notes[i];
                    if (SqRow(n.title.empty() ? std::string("(χωρίς τίτλο)") : n.title, n.content.substr(0, 90), hl, colW, true)) OpenNoteFromSearch(n);
                } else if (c.cat == 8) {
                    const Partner& pr = res.partners[i];
                    if (SqRow(pr.name, pr.trade + (pr.phone.empty() ? "" : "  ·  " + pr.phone), hl, colW, true)) OpenPartnerProfile(pr.id);
                } else {
                    const Receipt& r = res.receipts[i];
                    char no[32]; std::snprintf(no, sizeof(no), "%s-%06d", SeriesLabel(r.series).c_str(), r.number);
                    std::string refs = r.paper_ref.empty() ? "" : "  ·  χειρ. " + r.paper_ref;
                    if (!r.bank_ref.empty()) refs += "  ·  " + r.bank_ref;
                    std::string l2 = r.issue_date + "  ·  " + Eur(r.total) + (r.status == "void" ? "  ·  ΑΚΥΡΩΜΕΝΗ" : "") + refs;
                    if (SqRow(std::string(no) + "  " + (r.user_id > 0 ? g_app.userName(r.user_id) : ""), l2, hl, colW, true)) OpenReceiptView(r.id);
                }
                ImGui::PopID();
            }
            if ((c.cat == 6 || c.cat == 7) && res.Count(c.cat) > cnt && ImGui::SmallButton("Όλα τα αποτελέσματα ►")) g_sqCat = c.cat;
            ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ImGui::GetWindowHeight() - ImGui::GetFrameHeightWithSpacing() - 8.0f));
            SqPagerDraw(c.cat, cnt, pages, "pg");
            ImGui::EndChild();
        }
        return;
    }

    // ================================================================ ONE CATEGORY: wide, paged table
    ImGui::BeginChild("searchresults", ImVec2(0, 0), false);
    const ImGuiTableFlags tf = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp | kSortableTable;
    const ImVec4 link(0.55f, 0.78f, 1.0f, 1.0f);
    const auto hl = SqNeedles(g_sqCat, quick);
    const int pageSize = 25;
    int from = 0, to = 0, pages = 1;
    auto beginSec = [&](const char* title, int count, ImVec4 accent) { SqSection(title, count, accent); pages = SqPageRange(g_sqCat, count, pageSize, from, to); };

    if (g_sqCat == 1) {
        beginSec("Πελάτες", (int)res.users.size(), ImVec4(0.35f, 0.65f, 1.0f, 1));
        if (ImGui::BeginTable("sqtu", 7, tf)) {
            ImGui::TableSetupColumn("Πελάτης", ImGuiTableColumnFlags_WidthStretch, 2.2f);
            ImGui::TableSetupColumn("Τηλέφωνο", ImGuiTableColumnFlags_WidthStretch, 1.1f);
            ImGui::TableSetupColumn("Κωδ.", ImGuiTableColumnFlags_WidthStretch, 0.8f);
            ImGui::TableSetupColumn("Διεύθυνση", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("Διαμ.", ImGuiTableColumnFlags_WidthStretch, 0.7f);
            ImGui::TableSetupColumn("Περιοχή", ImGuiTableColumnFlags_WidthStretch, 1.2f);
            ImGui::TableSetupColumn("Τ.Κ.", ImGuiTableColumnFlags_WidthStretch, 0.6f);
            ImGui::TableHeadersRow();
            SortByTableSpecs(res.users, [](int c, const User* a, const User* b) {
                switch (c) { case 0: return CmpText(a->full_name, b->full_name); case 1: return Cmp3(a->phone, b->phone); case 2: return Cmp3(a->special_code, b->special_code);
                             case 3: return CmpText(a->address, b->address); case 4: return CmpText(a->apartment, b->apartment); case 5: return CmpText(a->area, b->area);
                             default: return Cmp3(a->postal_code, b->postal_code); } });
            for (int i = from; i < to; i++) {
                const User& u = *res.users[i];
                ImGui::PushID(u.id); ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (SqLinkCell(u.full_name, hl)) OpenUserProfile(u.id);
                ImGui::TableSetColumnIndex(1); SqCell(u.phone, hl);
                ImGui::TableSetColumnIndex(2); SqCell(u.special_code, hl);
                ImGui::TableSetColumnIndex(3); SqCell(u.address.empty() ? "—" : u.address, hl);
                ImGui::TableSetColumnIndex(4); SqCell(u.apartment.empty() ? "—" : u.apartment, hl);
                ImGui::TableSetColumnIndex(5); SqCell(u.area, hl);
                ImGui::TableSetColumnIndex(6); SqCell(u.postal_code, hl);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        SqPagerDraw(g_sqCat, (int)res.users.size(), pages, "pgu");
    } else if (g_sqCat == 2) {
        beginSec("Πολυκατοικίες", (int)res.buildings.size(), ImVec4(0.30f, 0.80f, 0.70f, 1));
        if (ImGui::BeginTable("sqtb", 6, tf)) {
            ImGui::TableSetupColumn("Διεύθυνση", ImGuiTableColumnFlags_WidthStretch, 2.4f);
            ImGui::TableSetupColumn("Περιοχή", ImGuiTableColumnFlags_WidthStretch, 1.3f);
            ImGui::TableSetupColumn("Τ.Κ.", ImGuiTableColumnFlags_WidthStretch, 0.7f);
            ImGui::TableSetupColumn("ΑΦΜ", ImGuiTableColumnFlags_WidthStretch, 1.1f);
            ImGui::TableSetupColumn("Ενεργά διαμ.", ImGuiTableColumnFlags_WidthStretch, 0.9f);
            ImGui::TableSetupColumn("Κάτοικοι", ImGuiTableColumnFlags_WidthStretch, 0.8f);
            ImGui::TableHeadersRow();
            SortByTableSpecs(res.buildings, [](int c, const SqBRow& a, const SqBRow& b) {
                switch (c) { case 0: return CmpText(a.b.address, b.b.address); case 1: return CmpText(a.b.area, b.b.area); case 2: return Cmp3(a.b.postal_code, b.b.postal_code);
                             case 3: return Cmp3(a.b.afm, b.b.afm); case 4: return Cmp3(a.apts, b.apts); default: return Cmp3(a.residents, b.residents); } });
            for (int i = from; i < to; i++) {
                const SqBRow& r = res.buildings[i];
                ImGui::PushID(r.b.id); ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (SqLinkCell(r.b.address, hl)) { g_aptGotoBuilding = r.b.id; g_tabToApartments = true; }
                ImGui::TableSetColumnIndex(1); SqCell(r.b.area, hl);
                ImGui::TableSetColumnIndex(2); SqCell(r.b.postal_code, hl);
                ImGui::TableSetColumnIndex(3); SqCell(r.b.afm.empty() ? "—" : r.b.afm, hl);
                ImGui::TableSetColumnIndex(4); ImGui::Text("%d", r.apts);
                ImGui::TableSetColumnIndex(5); ImGui::Text("%d", r.residents);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        SqPagerDraw(g_sqCat, (int)res.buildings.size(), pages, "pgb");
    } else if (g_sqCat == 3) {
        beginSec("Διαμερίσματα", (int)res.apartments.size(), ImVec4(0.55f, 0.85f, 0.45f, 1));
        if (ImGui::BeginTable("sqta", 5, tf)) {
            ImGui::TableSetupColumn("Πολυκατοικία", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("Διαμέρισμα", ImGuiTableColumnFlags_WidthStretch, 0.9f);
            ImGui::TableSetupColumn("ΑΦΜ", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Κάτοικος σήμερα", ImGuiTableColumnFlags_WidthStretch, 2.2f);
            ImGui::TableSetupColumn("Κινήσεις", ImGuiTableColumnFlags_WidthStretch, 0.7f);
            ImGui::TableHeadersRow();
            SortByTableSpecs(res.apartments, [](int c, const SqARow& a, const SqARow& b) {
                switch (c) { case 0: return CmpText(a.b.address, b.b.address); case 1: return CmpText(a.a.label, b.a.label); case 2: return Cmp3(a.b.afm, b.b.afm);
                             case 3: return CmpText(a.current, b.current); default: return Cmp3(a.moves, b.moves); } });
            for (int i = from; i < to; i++) {
                const SqARow& r = res.apartments[i];
                ImGui::PushID(r.a.id); ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (SqLinkCell(r.b.address, hl)) { g_aptGotoId = r.a.id; g_tabToApartments = true; }
                ImGui::TableSetColumnIndex(1); SqCell(r.a.label, hl);
                ImGui::TableSetColumnIndex(2); SqCell(r.b.afm.empty() ? "—" : r.b.afm, hl);
                ImGui::TableSetColumnIndex(3); SqCell(r.current.empty() ? "κενό" : r.current, hl);
                ImGui::TableSetColumnIndex(4); ImGui::Text("%d", r.moves);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        SqPagerDraw(g_sqCat, (int)res.apartments.size(), pages, "pga");
    } else if (g_sqCat == 4) {
        beginSec("Υπηρεσίες", (int)res.services.size(), ImVec4(0.95f, 0.75f, 0.30f, 1));
        if (ImGui::BeginTable("sqts", 5, tf)) {
            ImGui::TableSetupColumn("Υπηρεσία", ImGuiTableColumnFlags_WidthStretch, 2.4f);
            ImGui::TableSetupColumn("Τιμή", ImGuiTableColumnFlags_WidthStretch, 0.7f);
            ImGui::TableSetupColumn("Πακέτα", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("Πελάτες", ImGuiTableColumnFlags_WidthStretch, 0.7f);
            ImGui::TableSetupColumn("Σημειώσεις", ImGuiTableColumnFlags_WidthStretch, 3.0f);
            ImGui::TableHeadersRow();
            SortByTableSpecs(res.services, [&](int c, const Service* a, const Service* b) {
                auto pk = [&](const Service* s) { auto it = cx.pkgsOfService.find(s->id); return it == cx.pkgsOfService.end() ? std::string() : it->second; };
                switch (c) { case 0: return CmpText(a->label, b->label); case 1: return Cmp3(ToCents(a->suggested_price), ToCents(b->suggested_price));
                             case 2: return CmpText(pk(a), pk(b)); case 3: return 0; default: return CmpText(a->extra_notes, b->extra_notes); } });
            for (int i = from; i < to; i++) {
                const Service& s = *res.services[i];
                // distinct clients per service: one GROUP BY (client, service), cached
                static Cached<std::unordered_map<int, int>> clientsPerService;
                const auto& cps = clientsPerService.get("", [] {
                    std::unordered_map<int, int> m;
                    for (auto& g : g_app.conn->paymentTotalsBy(DataConn::PayGroup::UserService, {}))
                        m[std::atoi(g.key.substr(g.key.find('|') + 1).c_str())]++;
                    return m;
                });
                auto cpsIt = cps.find(s.id);
                const int nClients = cpsIt == cps.end() ? 0 : cpsIt->second;
                auto pit = cx.pkgsOfService.find(s.id);
                ImGui::PushID(s.id); ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (SqLinkCell(s.label, hl)) OpenServiceDetail(s.id);
                ImGui::TableSetColumnIndex(1); ImGui::Text("%s", Eur(s.suggested_price).c_str());
                ImGui::TableSetColumnIndex(2); SqCell(pit != cx.pkgsOfService.end() ? pit->second : "—", hl);
                ImGui::TableSetColumnIndex(3); ImGui::Text("%d", nClients);
                ImGui::TableSetColumnIndex(4); SqCell(s.extra_notes, hl);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        SqPagerDraw(g_sqCat, (int)res.services.size(), pages, "pgs");
    } else if (g_sqCat == 5) {
        beginSec("Πακέτα", (int)res.packages.size(), ImVec4(0.75f, 0.55f, 0.95f, 1));
        if (ImGui::BeginTable("sqtk", 4, tf)) {
            ImGui::TableSetupColumn("Πακέτο", ImGuiTableColumnFlags_WidthStretch, 1.8f);
            ImGui::TableSetupColumn("Υπηρεσίες", ImGuiTableColumnFlags_WidthStretch, 0.7f);
            ImGui::TableSetupColumn("Σύνολο", ImGuiTableColumnFlags_WidthStretch, 0.8f);
            ImGui::TableSetupColumn("Περιέχει", ImGuiTableColumnFlags_WidthStretch, 4.0f);
            ImGui::TableHeadersRow();
            SortByTableSpecs(res.packages, [](int c, const DataConn::MasterServiceSummary* a, const DataConn::MasterServiceSummary* b) {
                auto tot = [](const DataConn::MasterServiceSummary* s) { long long x = 0; for (auto& k : s->components) x += ToCents(k.suggested_price); return x; };
                auto names = [](const DataConn::MasterServiceSummary* s) { std::string r; for (auto& k : s->components) r += k.label + ", "; return r; };
                switch (c) { case 0: return CmpText(a->masterService.label, b->masterService.label); case 1: return Cmp3(a->components.size(), b->components.size());
                             case 2: return Cmp3(tot(a), tot(b)); default: return CmpText(names(a), names(b)); } });
            for (int i = from; i < to; i++) {
                const auto* ms = res.packages[i];
                double tot = 0; std::string comps;
                for (auto& c : ms->components) { tot += c.suggested_price; comps += (comps.empty() ? "" : ", ") + c.label; }
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); SqCell(ms->masterService.label, hl);
                ImGui::TableSetColumnIndex(1); ImGui::Text("%d", (int)ms->components.size());
                ImGui::TableSetColumnIndex(2); ImGui::Text("%s", Eur(tot).c_str());
                ImGui::TableSetColumnIndex(3); SqCell(comps, hl);
            }
            ImGui::EndTable();
        }
        SqPagerDraw(g_sqCat, (int)res.packages.size(), pages, "pgk");
    } else if (g_sqCat == 6) {
        // header: every match; paging: only the rows actually loaded (capped), never past the end of the list
        SqSection("Πληρωμές", (int)res.payCount, ImVec4(0.95f, 0.45f, 0.55f, 1));
        pages = SqPageRange(g_sqCat, (int)res.payments.size(), pageSize, from, to);
        ImGui::TextDisabled("Σύνολο οφειλής: %s   ·   Σύνολο πληρωμένου: %s   (όλων των αποτελεσμάτων)", Eur(res.payDue).c_str(), Eur(res.payPaid).c_str());
        if (ImGui::BeginTable("sqtp", 8, tf)) {
            ImGui::TableSetupColumn("Ημ/νία", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Πελάτης", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("Υπηρεσία", ImGuiTableColumnFlags_WidthStretch, 2.2f);
            ImGui::TableSetupColumn("Πολυκατοικία", ImGuiTableColumnFlags_WidthStretch, 1.6f);
            ImGui::TableSetupColumn("Τύπος", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Κατάσταση", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Οφειλή", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableSetupColumn("Πληρωμένο", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableHeadersRow();
            SortByTableSpecs(res.payments, [&](int c, const Payment& a, const Payment& b) {
                auto addr = [&](const Payment& p) { auto u = cx.userById.find(p.user_id); return u == cx.userById.end() ? std::string() : u->second->address; };
                switch (c) { case 0: return Cmp3(a.year * 10000 + a.month * 100 + a.day, b.year * 10000 + b.month * 100 + b.day);
                             case 1: return CmpText(g_app.userName(a.user_id), g_app.userName(b.user_id)); case 2: return CmpText(ChargeLabel(a), ChargeLabel(b));
                             case 3: return CmpText(addr(a), addr(b)); case 4: return Cmp3(a.direction, b.direction); case 5: return Cmp3(a.status, b.status);
                             case 6: return Cmp3(ToCents(a.amount_due), ToCents(b.amount_due)); default: return Cmp3(ToCents(a.amount_paid), ToCents(b.amount_paid)); } });
            for (int i = from; i < to; i++) {
                const Payment& p = res.payments[i];
                auto uit = cx.userById.find(p.user_id);
                ImGui::PushID(p.id); ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                char date[16]; std::snprintf(date, sizeof(date), "%04d-%02d-%02d", p.year, p.month, p.day);
                ImGui::PushStyleColor(ImGuiCol_Text, link);
                if (ImGui::Selectable(date, false, ImGuiSelectableFlags_SpanAllColumns)) OpenPaymentView(p.id);
                ImGui::PopStyleColor();
                ImGui::TableSetColumnIndex(1); SqCell(g_app.userName(p.user_id), hl);
                ImGui::TableSetColumnIndex(2); SqCell(ChargeLabel(p), hl);
                ImGui::TableSetColumnIndex(3); SqCell(uit != cx.userById.end() && !uit->second->address.empty() ? uit->second->address : "—", hl);
                ImGui::TableSetColumnIndex(4); ImGui::TextColored(DirectionColor(p.direction), "%s", DirectionBadge(p.direction));
                ImGui::TableSetColumnIndex(5); StatusCell(p);
                ImGui::TableSetColumnIndex(6); ImGui::TextUnformatted(Eur(p.amount_due).c_str());
                ImGui::TableSetColumnIndex(7); ImGui::TextUnformatted(Eur(p.amount_paid).c_str());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        SqPagerDraw(g_sqCat, (int)res.payments.size(), pages, "pgp");
    } else if (g_sqCat == 7) {
        SqSection("Αποδείξεις", (int)res.rcCount, ImVec4(0.55f, 0.85f, 0.75f, 1));
        pages = SqPageRange(g_sqCat, (int)res.receipts.size(), pageSize, from, to);
        ImGui::TextDisabled("Σύνολο σε ισχύ: %s   ·   αναζήτηση σε αριθμό, χειρόγραφη, αναφορά τράπεζας, RF και όνομα πελάτη", Eur(res.rcTotal).c_str());
        if (ImGui::BeginTable("sqtr", 6, tf)) {
            ImGui::TableSetupColumn("Αριθμός", ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableSetupColumn("Ημ/νία", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Πελάτης", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("Αναφορές", ImGuiTableColumnFlags_WidthStretch, 1.6f);
            ImGui::TableSetupColumn("Σύνολο", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Κατάσταση", ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableHeadersRow();
            SortByTableSpecs(res.receipts, [](int c, const Receipt& a, const Receipt& b) {
                auto refs = [](const Receipt& r) { return r.paper_ref + " " + r.bank_ref; };
                switch (c) { case 0: return Cmp3(a.series + std::to_string(1000000 + a.number), b.series + std::to_string(1000000 + b.number)); case 1: return Cmp3(a.issue_date, b.issue_date);
                             case 2: return CmpText(g_app.userName(a.user_id), g_app.userName(b.user_id)); case 3: return Cmp3(refs(a), refs(b));
                             case 4: return Cmp3(ToCents(a.total), ToCents(b.total)); default: return Cmp3(a.status, b.status); } });
            for (int i = from; i < to; i++) {
                const Receipt& r = res.receipts[i];
                ImGui::PushID(r.id); ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                char no[32]; std::snprintf(no, sizeof(no), "%s-%06d", SeriesLabel(r.series).c_str(), r.number);
                ImGui::PushStyleColor(ImGuiCol_Text, link);
                if (ImGui::Selectable(no, false, ImGuiSelectableFlags_SpanAllColumns)) OpenReceiptView(r.id);
                ImGui::PopStyleColor();
                ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(DateGr(r.issue_date).c_str());
                ImGui::TableSetColumnIndex(2); SqCell(r.user_id > 0 ? g_app.userName(r.user_id) : "—", hl);
                std::string refs = r.paper_ref.empty() ? "" : "χειρ. " + r.paper_ref;
                if (!r.bank_ref.empty()) refs += (refs.empty() ? "" : " · ") + r.bank_ref;
                ImGui::TableSetColumnIndex(3); SqCell(refs, hl);
                ImGui::TableSetColumnIndex(4); ImGui::Text("%s", Eur(r.total).c_str());
                ImGui::TableSetColumnIndex(5);
                if (r.status == "void") ImGui::TextColored(kColOut, "ΑΚΥΡΩΜΕΝΗ");
                else ImGui::TextColored(ImVec4(0.45f, 0.80f, 0.50f, 1), "σε ισχύ");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        SqPagerDraw(g_sqCat, (int)res.receipts.size(), pages, "pgr");
    } else if (g_sqCat == 8) {
        beginSec("Συνεργάτες", (int)res.partners.size(), ImVec4(0.80f, 0.60f, 0.95f, 1));
        if (ImGui::BeginTable("sqtp", 4, tf)) {
            ImGui::TableSetupColumn("Συνεργάτης", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("Ειδικότητα", ImGuiTableColumnFlags_WidthStretch, 1.5f);
            ImGui::TableSetupColumn("Τηλέφωνο", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Email", ImGuiTableColumnFlags_WidthStretch, 1.5f);
            ImGui::TableHeadersRow();
            SortByTableSpecs(res.partners, [](int c, const Partner& a, const Partner& b) {
                switch (c) { case 0: return CmpText(a.name, b.name); case 1: return CmpText(a.trade, b.trade); case 2: return Cmp3(a.phone, b.phone); default: return CmpText(a.email, b.email); } });
            for (int i = from; i < to; i++) {
                const Partner& pr = res.partners[i];
                ImGui::PushID(pr.id); ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushStyleColor(ImGuiCol_Text, link);
                if (ImGui::Selectable(pr.name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) OpenPartnerProfile(pr.id);
                ImGui::PopStyleColor();
                ImGui::TableSetColumnIndex(1); SqCell(pr.trade, hl);
                ImGui::TableSetColumnIndex(2); SqCell(pr.phone, hl);
                ImGui::TableSetColumnIndex(3); SqCell(pr.email, hl);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        SqPagerDraw(g_sqCat, (int)res.partners.size(), pages, "pgpt");
    } else if (g_sqCat == 9) {
        beginSec("Προσφορές", (int)res.offers.size(), ImVec4(0.95f, 0.65f, 0.35f, 1));
        if (ImGui::BeginTable("sqto", 5, tf)) {
            ImGui::TableSetupColumn("Προσφορά", ImGuiTableColumnFlags_WidthStretch, 2.0f); ImGui::TableSetupColumn("Έκπτωση", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Για", ImGuiTableColumnFlags_WidthStretch, 1.5f); ImGui::TableSetupColumn("Διάρκεια", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            ImGui::TableSetupColumn("Κατάσταση", ImGuiTableColumnFlags_WidthStretch, 1.6f);
            ImGui::TableHeadersRow();
            SortByTableSpecs(res.offers, [](int c, const Offer& a, const Offer& b) {
                switch (c) { case 0: return CmpText(a.name, b.name); case 1: return Cmp3(a.value, b.value); case 2: return Cmp3(a.user_id, b.user_id);
                             case 3: return Cmp3(a.valid_from, b.valid_from); default: return Cmp3(a.valid_to, b.valid_to); } });
            for (int i = from; i < to; i++) {
                const Offer& o = res.offers[i];
                ImGui::PushID(o.id); ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::PushStyleColor(ImGuiCol_Text, link);
                if (ImGui::Selectable(o.name.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) OpenOffersPage();
                ImGui::PopStyleColor();
                ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(OfferValueText(o).c_str());
                ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(o.user_id > 0 ? g_app.userName(o.user_id).c_str() : "Όλοι οι πελάτες");
                ImGui::TableSetColumnIndex(3); ImGui::Text("%s - %s", DateGr(o.valid_from).c_str(), DateGr(o.valid_to).c_str());
                ImGui::TableSetColumnIndex(4); { ImVec4 oc; std::string st = OfferStateText(o, oc); ImGui::TextColored(oc, "%s", st.c_str()); }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        SqPagerDraw(g_sqCat, (int)res.offers.size(), pages, "pgof");
    } else if (g_sqCat == 10) {
        beginSec("Σημειώσεις", (int)res.notes.size(), ImVec4(0.90f, 0.85f, 0.45f, 1));
        if (ImGui::BeginTable("sqtn", 3, tf)) {
            ImGui::TableSetupColumn("Τίτλος", ImGuiTableColumnFlags_WidthStretch, 1.5f); ImGui::TableSetupColumn("Κείμενο", ImGuiTableColumnFlags_WidthStretch, 4.0f);
            ImGui::TableSetupColumn("Ημ/νία", ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableHeadersRow();
            SortByTableSpecs(res.notes, [](int c, const Note& a, const Note& b) {
                switch (c) { case 0: return CmpText(a.title, b.title); case 1: return CmpText(a.content, b.content); default: return Cmp3(a.created_at, b.created_at); } });
            for (int i = from; i < to; i++) {
                const Note& n = res.notes[i];
                ImGui::PushID(n.id); ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::PushStyleColor(ImGuiCol_Text, link);
                if (ImGui::Selectable(n.title.empty() ? "(χωρίς τίτλο)" : n.title.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) OpenNoteFromSearch(n);
                ImGui::PopStyleColor();
                ImGui::TableSetColumnIndex(1); SqCell(n.content.substr(0, 200), hl);
                ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(DateGr(n.created_at.substr(0, 10)).c_str());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        SqPagerDraw(g_sqCat, (int)res.notes.size(), pages, "pgnt");
    }
    ImGui::EndChild();
}

static void DrawNotesScreen() {
    if (ImGui::Button("+ Νέα Σημείωση")) OpenAddNote();
    ImGui::SameLine();
    if (ImGui::Button("Ανανέωση")) g_app.reloadNotes();
    ImGui::Separator();

    ImGui::BeginChild("notecards", ImVec2(0, 0), false);
    const float cardW = 230, cardH = 160;
    for (auto& n : g_app.notes) {
        ImGui::PushID(n.id);
        ImU32 bg; ImVec4 textCol;
        NoteCardColor(n.id, bg, textCol);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(bg));
        ImGui::PushStyleColor(ImGuiCol_Text, textCol);
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0.15f));
        BeginCard("notecard", ImVec2(cardW, cardH));
        ImGui::TextWrapped("%s", n.title.empty() ? "(χωρίς τίτλο)" : n.title.c_str());
        ImGui::Separator();
        std::string preview = n.content.size() > 160 ? n.content.substr(0, 160) + "..." : n.content;
        ImGui::TextWrapped("%s", preview.c_str());
        ImGui::SetCursorPosY(cardH - 46);
        ImGui::TextDisabled("%s", DateGr(n.created_at).c_str());
        if (ImGui::SmallButton("Επεξ.")) OpenEditNote(n);
        ImGui::SameLine();
        if (ImGui::SmallButton("Διαγρ.")) {
            int id = n.id;
            RequestConfirm("Διαγραφή σημείωσης", { {"Τίτλος", n.title, "(διαγραφή)"} }, [id]() { g_app.conn->removeNote(id); g_app.reloadNotes(); });
        }
        EndCard();
        ImGui::PopStyleColor(3);
        ImGui::PopID();
        CardWrapNext(cardW);
    }
    if (g_app.notes.empty()) ImGui::TextDisabled("Καμία σημείωση ακόμα.");
    ImGui::EndChild();
    DrawNoteDialog();
}

// ---------------------------------------------------------------------------
// GENERAL REPORTS ("Γενικές Αναφορές") -- a summary landing page: totals, the
// progression over time (weekly / monthly / yearly), then per-service and per-client
// logistics. Only real (non-future) payments count as money moved; "Προγραμματισμένες"
// shows the future ones separately. Clicking a bar in the progression chart narrows
// every figure below it to that single period.
// ---------------------------------------------------------------------------
static int g_repPeriod = 1;                 // 0 weekly, 1 monthly, 2 yearly
static long long RepDaysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}


// ---------------------------------------------------------------------------
// APARTMENTS TAB ("Διαμερίσματα") -- the history of one apartment, whoever lived in it.
// The database keeps a residency history (who lived there, from/to) and every payment
// remembers the apartment it was made from (decided by the residency valid on the payment's
// date), so moving a client never re-attributes old money. Movements are summed per
// minute / day / week / month / year. Totals are always computed over EVERYTHING; only the
// long lists of rows are paged. Left: all buildings (with ΑΦΜ) that open into their apartments.
// ---------------------------------------------------------------------------
static int g_aptSelId = -1;
static int g_aptPeriod = 3;                 // 0 minute, 1 day, 2 week, 3 month, 4 year
static bool g_aptGroup = true;              // split the movements by the client who lived there
static TextBuf apt_search;
static std::set<int> g_aptOpenB;            // buildings expanded in the left list

static std::string AptDateLabel(const std::string& ymd) {            // 2026-03-07 -> 07/03/2026
    if (ymd.size() < 10) return ymd;
    return ymd.substr(8, 2) + "/" + ymd.substr(5, 2) + "/" + ymd.substr(0, 4);
}

static int g_aptSelBld = -1;      // a building (not an apartment) is selected: show the building's own detail

static void DrawApartmentsScreen() {
    if (ImGui::Button("+ Νέα πολυκατοικία")) {
        g_showBldgDialog = true; g_bldgDialogIsEdit = false; g_bldgDialogId = -1;
        bd_addr.set(""); bd_area.set(""); bd_postal.set(""); bd_afm.set("");
    }
    ImGui::SameLine(); ImGui::TextDisabled("Άνοιγμα πολυκατοικίας: διαμερίσματα, τωρινοί κάτοικοι και κοινόχρηστα. Κλικ σε διαμέρισμα: το ιστορικό του.");
    DrawBuildingDialogs();
    // ---- jump requested from another tab (Πολυκατοικίες / Αναζήτηση) ----
    if (g_aptGotoId > 0) {
        g_aptSelId = g_aptGotoId; g_aptGotoId = -1; g_aptSelBld = -1;
        for (auto& b : g_app.conn->getBuildings())
            for (auto& a : g_app.conn->getApartmentsForBuilding(b.id)) if (a.id == g_aptSelId) g_aptOpenB.insert(b.id);
    }
    if (g_aptGotoBuilding > 0) { g_aptOpenB.insert(g_aptGotoBuilding); g_aptSelBld = g_aptGotoBuilding; g_aptSelId = -1; g_aptGotoBuilding = -1; }

    // ---- every apartment with what happened there (per-apartment totals from one GROUP BY; cached) ----
    struct AptEntry { Apartment a; std::string current; int moves = 0; double in = 0, out = 0; };
    struct BldEntry { Building b; std::vector<AptEntry> apts; int moves = 0; double total = 0; };
    static Cached<std::vector<BldEntry>> bldsCache;
    const std::vector<BldEntry>& blds = bldsCache.get("", [] {
        std::unordered_map<int, DataConn::MoneyTotals> byApt;
        DataConn::PaymentFilter f; f.status = 1;
        for (auto& g : g_app.conn->paymentTotalsBy(DataConn::PayGroup::Apartment, f)) byApt[std::atoi(g.key.c_str())] = g.t;
        std::unordered_map<int, std::string> currentOf;
        for (auto& u : g_app.users) if (u.apartment_id > 0) currentOf[u.apartment_id] += (currentOf[u.apartment_id].empty() ? "" : ", ") + u.full_name;
        std::vector<BldEntry> out;
        for (auto& b : g_app.conn->getBuildings()) {
            BldEntry be; be.b = b;
            for (auto& a : g_app.conn->getApartmentsForBuilding(b.id)) {
                AptEntry e; e.a = a;
                if (auto c = currentOf.find(a.id); c != currentOf.end()) e.current = c->second;
                if (auto t = byApt.find(a.id); t != byApt.end()) { e.moves = (int)t->second.count; e.in = t->second.collected; e.out = t->second.paidOut; }
                be.moves += e.moves; be.total += e.in + e.out;
                be.apts.push_back(std::move(e));
            }
            out.push_back(std::move(be));
        }
        std::sort(out.begin(), out.end(), [](const BldEntry& x, const BldEntry& y) { return x.b.address < y.b.address; });
        return out;
    });
    bool selOk = false; int allApts = 0;
    for (auto& b : blds) for (auto& a : b.apts) { allApts++; if (a.a.id == g_aptSelId) selOk = true; }
    if (!selOk && g_aptSelBld <= 0) { g_aptSelId = -1; for (auto& b : blds) if (!b.apts.empty() && g_aptSelId < 0) { g_aptSelId = b.apts[0].a.id; g_aptOpenB.insert(b.b.id); } }
    if (blds.empty()) { ImGui::TextDisabled("Δεν υπάρχουν διαμερίσματα — προσθέστε πολυκατοικίες και διαμερίσματα από την καρτέλα «Πολυκατοικίες»."); return; }

    float totalW = ImGui::GetContentRegionAvail().x;
    float leftW = std::max(320.0f, totalW * 0.30f);

    // ================================================================ LEFT: buildings -> apartments
    ImGui::BeginChild("aptlist", ImVec2(leftW, -1), true);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##aptsearch", "Αναζήτηση: διεύθυνση, περιοχή, Τ.Κ., ΑΦΜ, διαμέρισμα ή κάτοικος...", apt_search.data, sizeof(apt_search.data));
    const std::string q = apt_search.str();
    ImGui::TextDisabled("%d πολυκατοικίες · %d διαμερίσματα (και τα κενά)", (int)blds.size(), allApts);
    ImGui::Separator();
    int shownB = 0;
    for (auto& be : blds) {
        const Building& b = be.b;
        const bool bMatch = SubstrCI(b.address + " " + b.area + " " + b.postal_code + " " + b.afm, q);
        std::vector<const AptEntry*> aptShow;
        for (auto& e : be.apts) {
            std::string hay = e.a.label + " " + e.current;
            for (auto& r : g_app.conn->getResidencies(e.a.id)) hay += " " + g_app.userName(r.user_id);
            if (q.empty() || bMatch || SubstrCI(hay, q)) aptShow.push_back(&e);
        }
        if (!q.empty() && !bMatch && aptShow.empty()) continue;
        shownB++;
        const bool open = !q.empty() ? true : g_aptOpenB.count(b.id) > 0;
        ImGui::PushID(b.id);
        char head[480];
        std::snprintf(head, sizeof(head), "%s\n%s · Τ.Κ. %s · ΑΦΜ %s\n%d διαμερίσματα",
                      b.address.c_str(), b.area.c_str(), b.postal_code.c_str(), b.afm.empty() ? "—" : b.afm.c_str(), (int)be.apts.size());
        // the arrow opens / closes the apartments; the building itself opens its summary on the right
        if (ImGui::Button(open ? "▼" : "►", ImVec2(28, 58))) { if (g_aptOpenB.count(b.id)) g_aptOpenB.erase(b.id); else g_aptOpenB.insert(b.id); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(open ? "Κλείσιμο διαμερισμάτων" : "Άνοιγμα διαμερισμάτων");
        ImGui::SameLine(0, 4);
        bool selB = g_aptSelBld == b.id && g_aptSelId <= 0;
        ImGui::PushStyleColor(ImGuiCol_Header, selB ? ImVec4(0.22f, 0.36f, 0.52f, 0.95f) : ImVec4(0.16f, 0.24f, 0.33f, 0.80f));
        if (ImGui::Selectable(head, true, 0, ImVec2(0, 58))) { g_aptSelBld = b.id; g_aptSelId = -1; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Σύνοψη πολυκατοικίας: οι χρεώσεις της σαν πελάτης");
        ImGui::PopStyleColor();
        if (open) {
            ImGui::Indent(14.0f);
            for (auto* e : aptShow) {
                ImGui::PushID(e->a.id);
                char line[360];
                std::snprintf(line, sizeof(line), "%s\n   %s", e->a.label.c_str(),
                              e->current.empty() ? "κανένας κάτοικος αυτή τη στιγμή" : e->current.c_str());
                if (ImGui::Selectable(line, e->a.id == g_aptSelId, 0, ImVec2(0, 42))) { g_aptSelId = e->a.id; g_aptSelBld = -1; }
                ImGui::PopID();
            }
            ImGui::Unindent(14.0f);
        }
        ImGui::PopID();
        ImGui::Spacing();
    }
    if (shownB == 0) ImGui::TextDisabled("Κανένα αποτέλεσμα.");
    ImGui::EndChild();
    ImGui::SameLine();

    // ================================================================ RIGHT: history of the selected apartment
    const AptEntry* sel = nullptr; const Building* selB = nullptr;
    for (auto& b : blds) for (auto& a : b.apts) if (a.a.id == g_aptSelId) { sel = &a; selB = &b.b; }
    ImGui::BeginChild("aptdetail", ImVec2(0, -1), false);
    if (sel && selB) {
        ImGui::SetWindowFontScale(1.2f);
        ImGui::TextColored(ImVec4(0.92f, 0.95f, 1.0f, 1.0f), "%s — διαμέρισμα %s", selB->address.c_str(), sel->a.label.c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::TextDisabled("%s · Τ.Κ. %s · ΑΦΜ %s", selB->area.c_str(), selB->postal_code.c_str(), selB->afm.empty() ? "—" : selB->afm.c_str());
        ImGui::Spacing();

        // 1. who lives here now
        SectionTitle("Ποιος μένει εδώ");
        {
            bool any = false;
            for (auto& r : g_app.conn->getResidencies(sel->a.id)) if (r.to_date.empty()) {
                any = true; ImGui::PushID(r.user_id);
                ImGui::PushStyleColor(ImGuiCol_Text, kColLink);
                if (ImGui::Selectable(g_app.userName(r.user_id).c_str(), false, 0, ImVec2(ImGui::CalcTextSize(g_app.userName(r.user_id).c_str()).x, 0)))
                    OpenUserProfile(r.user_id);
                ImGui::PopStyleColor();
                ImGui::SameLine(); ImGui::TextDisabled("από %s", AptDateLabel(r.from_date).c_str());
                ImGui::PopID();
            }
            if (!any) {
                ImGui::TextDisabled("Κανένας κάτοικος αυτή τη στιγμή - το διαμέρισμα είναι κενό.");
                static Cached<int> openN;
                int n = openN.get(std::to_string(sel->a.id), [&] { return g_app.conn->countShares(-1, sel->a.id, "", "", true); });
                if (n > 0) ImGui::TextColored(kColOwed, "Οι %d εκκρεμότητες παρακάτω είναι πρώην ενοίκων (χρεώθηκαν όσο έμεναν εδώ) και τις οφείλουν εκείνοι.", n);
            }
        }
        // 2. everyone who lived here, with the empty periods (the only collapsible part)
        if (ImGui::CollapsingHeader("Ιστορικό κατοίκων")) DrawAptTimeline(sel->a.id, "apttl", 300.0f);
        ImGui::Spacing();

        // 3. the apartment's joint charges: its share of each, paid or not (only this apartment, nothing else of the client)
        {
            static std::map<int, ListModel<JointShare>> openM, histM;
            const std::string sa = std::to_string(sel->a.id);
            SectionTitle("Τρέχοντα κοινόχρηστα (εκκρεμή από τον ένοικο ή από εμάς)");
            DrawSharesTable(("aptopen" + sa).c_str(), openM[sel->a.id], -1, sel->a.id, "", "", true, 190.0f);
            SectionTitle("Ιστορικό κοινοχρήστων του διαμερίσματος");
            DrawSharesTable(("apthist" + sa).c_str(), histM[sel->a.id], -1, sel->a.id, "", "", false, 290.0f);
        }
    } else if (g_aptSelBld > 0) {
        bool found = false;
        for (auto& x : BuildBuildings()) if (x.b.id == g_aptSelBld) { DrawBuildingDetail(x); found = true; }
        if (!found) g_aptSelBld = -1;
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// SETTINGS SCREEN: DB info, CSV backup, quick-add shortcuts, collapsed audit
// log ("Χρονολόγιο"), Raw SQL console, floating node-graph raw DB view.
// ---------------------------------------------------------------------------
static char g_sqlInput[2048] = "SELECT * FROM users LIMIT 20;";
static std::vector<std::string> g_sqlCols;
static std::vector<std::vector<std::string>> g_sqlRows;
static std::string g_sqlError;
static bool g_sqlRan = false;

struct GraphNode { std::string table; ImVec2 pos; ImVec2 vel; std::vector<std::string> cols; std::vector<std::vector<std::string>> rows; int offset = 0; int total = 0; };
static std::vector<GraphNode> g_graphNodes;
static bool g_showNodeGraph = false;
static const int kPageSize = 20;
static const ImVec2 kNodeSize(360, 320);

static void InitGraphNodes() {
    g_graphNodes.clear();
    // 3-2 grid so 5 tables don't overlap: three across the top row, two below.
    std::vector<std::pair<std::string, ImVec2>> layout = {
        {"users", {40, 40}}, {"services", {440, 40}}, {"payments", {840, 40}},
        {"service_users", {240, 400}}, {"notes", {640, 400}}
    };
    for (auto& [name, pos] : layout) {
        GraphNode n; n.table = name; n.pos = pos; n.vel = ImVec2(0, 0); n.offset = 0;
        n.total = g_app.conn->getRowCount(name);
        n.cols = g_app.conn->getTableColumns(name);
        n.rows = g_app.conn->getPaginatedData(name, 0, kPageSize);
        g_graphNodes.push_back(n);
    }
}

// Distinct, tasteful color per table (fill, title-bar accent).
static void TableColors(const std::string& table, ImU32& fill, ImU32& titleBar) {
    if (table == "users")          { fill = IM_COL32(46, 60, 92, 235);  titleBar = IM_COL32(70, 110, 200, 255); }
    else if (table == "services")  { fill = IM_COL32(40, 74, 66, 235);  titleBar = IM_COL32(60, 160, 130, 255); }
    else if (table == "service_users") { fill = IM_COL32(78, 62, 40, 235); titleBar = IM_COL32(200, 140, 60, 255); }
    else if (table == "payments")  { fill = IM_COL32(70, 45, 78, 235);  titleBar = IM_COL32(190, 90, 190, 255); }
    else if (table == "notes")     { fill = IM_COL32(80, 74, 40, 235);  titleBar = IM_COL32(210, 190, 70, 255); }
    else if (table == "audit_log") { fill = IM_COL32(70, 45, 45, 235);  titleBar = IM_COL32(200, 80, 80, 255); }
    else                            { fill = IM_COL32(55, 55, 60, 235);  titleBar = IM_COL32(140, 140, 150, 255); }
}

// Gentle always-on force-directed settle: spring along FK edges toward a rest
// distance, mild pairwise repulsion so nodes don't overlap, heavy damping so
// it calms down instead of oscillating. The node actively being dragged is
// pinned to the mouse and excluded from the simulation for that frame.
static void StepGraphPhysics(const std::vector<std::pair<int, int>>& edgeIdx, int draggedIdx, float dt) {
    if (dt <= 0.0f) return;
    dt = std::min(dt, 1.0f / 30.0f);
    const float restLen = 300.0f;
    const float springK = 0.0f;       // rest-length spring disabled: don't fight manual placement
    const float repelK = 250000.0f;   // pairwise repulsion strength (reduced range/strength)
    const float damping = 0.95f;      // velocity retained per step (settles almost immediately)
    int n = (int)g_graphNodes.size();
    std::vector<ImVec2> force(n, ImVec2(0, 0));

    if (springK > 0.0f) {
        for (auto& e : edgeIdx) {
            GraphNode& a = g_graphNodes[e.first];
            GraphNode& b = g_graphNodes[e.second];
            ImVec2 d(b.pos.x - a.pos.x, b.pos.y - a.pos.y);
            float dist = sqrtf(d.x * d.x + d.y * d.y);
            if (dist < 1.0f) dist = 1.0f;
            float stretch = dist - restLen;
            float f = springK * stretch;
            ImVec2 dir(d.x / dist, d.y / dist);
            force[e.first].x += dir.x * f; force[e.first].y += dir.y * f;
            force[e.second].x -= dir.x * f; force[e.second].y -= dir.y * f;
        }
    }
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            ImVec2 d(g_graphNodes[j].pos.x - g_graphNodes[i].pos.x, g_graphNodes[j].pos.y - g_graphNodes[i].pos.y);
            float dist2 = d.x * d.x + d.y * d.y;
            float dist = sqrtf(std::max(dist2, 1.0f));
            if (dist > 260.0f) continue; // repulsion only matters at close range (reduced from 500)
            float f = repelK / (dist * dist);
            ImVec2 dir(d.x / dist, d.y / dist);
            force[i].x -= dir.x * f; force[i].y -= dir.y * f;
            force[j].x += dir.x * f; force[j].y += dir.y * f;
        }
    }
    for (int i = 0; i < n; i++) {
        if (i == draggedIdx) { g_graphNodes[i].vel = ImVec2(0, 0); continue; }
        GraphNode& node = g_graphNodes[i];
        node.vel.x = (node.vel.x + force[i].x * dt) * damping;
        node.vel.y = (node.vel.y + force[i].y * dt) * damping;
        node.pos.x += node.vel.x * dt;
        node.pos.y += node.vel.y * dt;
        if (node.pos.x < 0) { node.pos.x = 0; node.vel.x = 0; }
        if (node.pos.y < 0) { node.pos.y = 0; node.vel.y = 0; }
    }
}

static int g_draggedNodeIdx = -1;
static ImVec2 g_canvasPan = ImVec2(0, 0); // screen-space pan offset, applied to render/hit-test only
static bool g_canvasPanning = false;

// --- Row-level FK connectors ------------------------------------------------
// Replaces the old "generic line between two tables" connectors with a
// per-ROW reveal: hover a specific payments/service_users row and a thin,
// low-opacity line is drawn from that exact row to the exact matching row in
// the target table (only if that target row happens to be in the currently
// loaded page), so it reads as a "magical" reveal on hover rather than a
// permanent tangle of lines. Screen rects for every rendered row (any table)
// are recorded fresh each frame so a hovered payments/service_users row can
// look up where its matching users/services row currently is on screen.
struct RowScreenRect { std::string table; std::string key; ImVec2 center; };
static std::vector<RowScreenRect> g_rowScreenRects; // rebuilt every frame
struct HoveredRowLinks { bool active = false; std::string table; std::string originKey; std::vector<std::string> targetKeys; };
static HoveredRowLinks g_hoveredRowLinks; // set while iterating rows, consumed right after

static ImVec2* FindRowCenter(const std::string& table, const std::string& key) {
    for (auto& r : g_rowScreenRects) if (r.table == table && r.key == key) return &r.center;
    return nullptr;
}

static void DrawNodeGraphOverlay() {
    if (!g_showNodeGraph) return;
    g_rowScreenRects.clear();
    g_hoveredRowLinks = HoveredRowLinks{};
    ImGui::SetNextWindowSize(ImVec2(1000, 700), ImGuiCond_Appearing);
    ImGui::Begin("Χάρτης Βάσης Δεδομένων (Raw DB)", &g_showNodeGraph);
    ImGui::TextDisabled("Σύρετε τους κόμβους - επανατοποθετούνται μόνοι τους γύρω από τη θέση σας. Σύρετε το κενό φόντο για μετακίνηση (pan) του καμβά.");
    // No ImGuiWindowFlags_HorizontalScrollbar / NoScrollWithMouse here: panning is
    // handled manually below via an InvisibleButton drag, which gives smoother,
    // more "node canvas"-like click-and-drag behavior than ImGui's own scrollbars
    // in both axes at once.
    ImGui::BeginChild("canvas", ImVec2(0, 0), true, ImGuiWindowFlags_NoScrollbar);
    // Captured AFTER entering the child (not before) so it reflects the child's
    // own content origin in screen space, including its border/padding -- using
    // the pre-child position here was the bug: it made every node's drawn card
    // land at a different screen position than its actual clickable hit-box
    // (SetCursorPos is window-local), so drags and buttons silently missed.
    ImVec2 canvasOrigin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Subtle background grid, drawn in pan-translated space so it reads as part
    // of the "world" the nodes live in (moves with the pan, giving visual feedback).
    {
        ImVec2 childMin = ImGui::GetWindowPos();
        ImVec2 childMax = ImVec2(childMin.x + ImGui::GetWindowSize().x, childMin.y + ImGui::GetWindowSize().y);
        const float grid = 40.0f;
        float offX = fmodf(g_canvasPan.x, grid);
        float offY = fmodf(g_canvasPan.y, grid);
        ImU32 gridCol = IM_COL32(255, 255, 255, 12);
        for (float x = childMin.x + offX; x < childMax.x; x += grid)
            dl->AddLine(ImVec2(x, childMin.y), ImVec2(x, childMax.y), gridCol, 1.0f);
        for (float y = childMin.y + offY; y < childMax.y; y += grid)
            dl->AddLine(ImVec2(childMin.x, y), ImVec2(childMax.x, y), gridCol, 1.0f);
    }

    // Invisible full-area button drawn BEFORE the nodes, so nodes (drawn/placed
    // after, and thus on top in z/hit-test order) still receive clicks
    // preferentially. Dragging on this background pans the whole canvas.
    ImGui::SetCursorScreenPos(canvasOrigin);
    ImGui::InvisibleButton("canvasbg", ImGui::GetContentRegionAvail(), ImGuiButtonFlags_MouseButtonLeft);
    if (ImGui::IsItemActivated() && g_draggedNodeIdx < 0) g_canvasPanning = true;
    if (g_canvasPanning) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            g_canvasPan.x += ImGui::GetIO().MouseDelta.x;
            g_canvasPan.y += ImGui::GetIO().MouseDelta.y;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) g_canvasPanning = false;
    }
    // Secondary nice-to-have: mouse wheel pans vertically (and horizontally with
    // Shift) while hovering the canvas, on top of click-drag panning.
    if (ImGui::IsItemHovered()) {
        ImGuiIO& io = ImGui::GetIO();
        if (io.MouseWheel != 0.0f) {
            if (io.KeyShift) g_canvasPan.x += io.MouseWheel * 60.0f;
            else g_canvasPan.y += io.MouseWheel * 60.0f;
        }
    }
    // canvasOrigin used for node placement below is translated by the pan offset;
    // node.pos (logical/untranslated) is left untouched so the physics simulation
    // keeps operating in stable logical space.
    canvasOrigin.x += g_canvasPan.x;
    canvasOrigin.y += g_canvasPan.y;

    // FK relationships, used only to feed StepGraphPhysics's (currently disabled,
    // springK=0) rest-length spring -- the actual visual connectors are now drawn
    // per-row, hover-only, further below (see g_hoveredRowLinks).
    auto findIdx = [](const std::string& t) -> int {
        for (int i = 0; i < (int)g_graphNodes.size(); i++) if (g_graphNodes[i].table == t) return i;
        return -1;
    };
    struct Edge { const char* a; const char* b; };
    static const Edge edgeDefs[] = {
        {"services", "service_users"}, {"users", "service_users"},
        {"services", "payments"}, {"users", "payments"},
    };
    std::vector<std::pair<int, int>> edgeIdx;
    for (auto& e : edgeDefs) {
        int ia = findIdx(e.a), ib = findIdx(e.b);
        if (ia >= 0 && ib >= 0) edgeIdx.push_back({ia, ib});
    }

    // Run the settle simulation every frame (ImGui re-renders continuously) --
    // EXCEPT while any node is actively being dragged.
    //
    // ROOT CAUSE of "dragging one node visibly drags others together": the dragged
    // node WAS excluded from receiving forces/moving via physics (see the
    // `i == draggedIdx` skip inside StepGraphPhysics's integration loop below), but
    // it was NOT excluded as a SOURCE of force. The pairwise-repulsion loop iterates
    // every pair (i, j) including pairs where one of them is the dragged node, using
    // its live, just-moved `pos` for that frame. So as soon as a drag brought the
    // dragged node within `dist > 260.0f` (the repulsion falloff radius) of any other
    // node, that other node felt a real repulsion force computed from the dragged
    // node's current mouse-following position and visibly slid away from it, every
    // single frame, for as long as the drag continued -- reading exactly like "the
    // other nodes are being dragged along too". This was traced concretely by
    // reading the actual force-accumulation loop above (the `for i, for j` pairwise
    // repulsion) against the integration loop below: the exclusion of `draggedIdx`
    // only happens in the SECOND loop (whether a node's own velocity/position gets
    // updated), never in the FIRST (whether a node's `pos` this frame is used to
    // push force onto some other node `j`). No interactive/GUI mouse-drag session
    // was run to reproduce it visually (no such harness is available in this
    // environment) -- this is a static trace of the data flow, not a live
    // repro+fix-confirmed cycle, and should be spot-checked by dragging a node in
    // the running app.
    //
    // Fix (client's preferred option): pause the WHOLE simulation while ANY node is
    // being dragged, so a drag is a pure, predictable "this node follows the mouse,
    // nothing else moves" operation. Physics resumes the instant the drag ends.
    if (g_draggedNodeIdx < 0) {
        StepGraphPhysics(edgeIdx, g_draggedNodeIdx, ImGui::GetIO().DeltaTime);
    }

    // REMOVED: the old static per-table-relationship connector curves (one
    // permanent bezier per FK column between whole table cards, always drawn,
    // regardless of whether any actual row data matched). Per the client, this
    // read as a fixed tangle of lines rather than something that reveals real
    // relationships. Replaced below with row-level, hover-only connectors: a
    // thin line from one specific payments/service_users ROW to the specific
    // users/services ROW its foreign key actually points at, shown only while
    // hovering that source row (see g_hoveredRowLinks, populated while drawing
    // each node's row list below, and drawn once after all nodes are laid out
    // so target positions -- possibly on a node drawn later in this same loop
    // -- are already known). g_rowScreenRects/g_hoveredRowLinks are cleared at
    // the top of this function, before "canvas" is even entered (see the
    // very top of DrawNodeGraphOverlay).

    for (int i = 0; i < (int)g_graphNodes.size(); i++) {
        GraphNode& n = g_graphNodes[i];
        ImGui::PushID(n.table.c_str());

        ImVec2 screenPos(canvasOrigin.x + n.pos.x, canvasOrigin.y + n.pos.y);
        ImU32 fillCol, titleCol;
        TableColors(n.table, fillCol, titleCol);
        const float rounding = 8.0f;
        const float titleH = 28.0f;

        // Drop shadow.
        dl->AddRectFilled(ImVec2(screenPos.x + 5, screenPos.y + 6), ImVec2(screenPos.x + kNodeSize.x + 5, screenPos.y + kNodeSize.y + 6),
                           IM_COL32(0, 0, 0, 90), rounding);
        // Card body.
        dl->AddRectFilled(screenPos, ImVec2(screenPos.x + kNodeSize.x, screenPos.y + kNodeSize.y), fillCol, rounding);
        // Title bar (only the top rounded).
        dl->AddRectFilled(screenPos, ImVec2(screenPos.x + kNodeSize.x, screenPos.y + titleH), titleCol, rounding, ImDrawFlags_RoundCornersTop);
        dl->AddRect(screenPos, ImVec2(screenPos.x + kNodeSize.x, screenPos.y + kNodeSize.y), IM_COL32(255, 255, 255, 40), rounding);
        dl->AddText(ImVec2(screenPos.x + 10, screenPos.y + 6), IM_COL32(255, 255, 255, 255), n.table.c_str());

        ImGui::SetCursorScreenPos(screenPos);
        ImGui::InvisibleButton("draghandle", ImVec2(kNodeSize.x, titleH));
        if (ImGui::IsItemActivated()) g_draggedNodeIdx = i;
        if (g_draggedNodeIdx == i) {
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                n.pos.x += ImGui::GetIO().MouseDelta.x;
                n.pos.y += ImGui::GetIO().MouseDelta.y;
            }
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) g_draggedNodeIdx = -1;
        }

        ImGui::SetCursorScreenPos(ImVec2(screenPos.x + 10, screenPos.y + titleH + 4));
        ImGui::BeginChild("body", ImVec2(kNodeSize.x - 20, kNodeSize.y - titleH - 10), false, ImGuiWindowFlags_NoScrollbar);
        // Smaller text inside the card body only -- scoped to this child window,
        // so it never leaks onto the title bar or the connector labels drawn
        // outside it. Lets more column/row content fit before truncation kicks in.
        ImGui::SetWindowFontScale(0.8f);
        // Truncate any cell too long to fit the card's width at this font scale,
        // rather than letting it overflow/wrap messily -- ids/dates stay intact,
        // long free-text columns (addresses, notes) get a trailing "...".
        auto truncateToWidth = [&](const std::string& s, float maxW) -> std::string {
            if (ImGui::CalcTextSize(s.c_str()).x <= maxW) return s;
            std::string cur = s;
            while (!cur.empty() && ImGui::CalcTextSize((cur + "...").c_str()).x > maxW) cur.pop_back();
            return cur + "...";
        };
        float cellMaxW = kNodeSize.x - 40.0f;
        ImGui::TextUnformatted((std::string("Στήλες: ") + std::to_string(n.cols.size()) + "   Σειρές: " + std::to_string(n.total)).c_str());
        {
            std::string header;
            for (size_t c = 0; c < n.cols.size(); c++) header += (c ? " | " : "") + n.cols[c];
            ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1), "%s", truncateToWidth(header, cellMaxW).c_str());
        }
        ImGui::Separator();
        ImGui::BeginChild("rows", ImVec2(0, 180), false);
        // Nested child windows don't inherit the parent's FontWindowScale -- set it
        // again here so row text stays small too, and reset before EndChild.
        ImGui::SetWindowFontScale(0.8f);
        // Column layout of `SELECT * FROM <table>`, used only to pull out the FK
        // values (service_id/user_id) and a row's own key for the hover-connector
        // lookup below. Matches CREATE TABLE order in SqliteConn.cpp.
        //   users/services:  id=0
        //   service_users:   service_id=0, user_id=1  (composite PK, no id column)
        //   payments:        id=0, service_id=1, ..., user_id=10, ...
        for (size_t ri = 0; ri < n.rows.size(); ri++) {
            auto& row = n.rows[ri];
            std::string line;
            for (size_t c = 0; c < row.size(); c++) line += (c ? " | " : "") + row[c];
            ImGui::PushID((int)ri);
            ImGui::Selectable(truncateToWidth(line, cellMaxW).c_str(), false);
            bool rowHovered = ImGui::IsItemHovered();
            ImVec2 rmin = ImGui::GetItemRectMin(), rmax = ImGui::GetItemRectMax();
            ImVec2 center((rmin.x + rmax.x) * 0.5f, (rmin.y + rmax.y) * 0.5f);

            std::string ownKey;
            std::vector<std::string> targetKeys; // (table implicit from n.table's FK columns)
            if (n.table == "users" || n.table == "services") {
                if (!row.empty()) ownKey = row[0];
            } else if (n.table == "service_users" && row.size() >= 2) {
                ownKey = row[0] + ":" + row[1];
                targetKeys.push_back(row[0]); // -> services.id
                targetKeys.push_back(row[1]); // -> users.id
            } else if (n.table == "payments" && row.size() > 10) {
                ownKey = row[0];
                targetKeys.push_back(row[1]);  // -> services.id
                targetKeys.push_back(row[10]); // -> users.id
            }
            if (!ownKey.empty()) g_rowScreenRects.push_back({n.table, ownKey, center});
            if (rowHovered && !targetKeys.empty()) {
                g_hoveredRowLinks.active = true;
                g_hoveredRowLinks.table = n.table;
                g_hoveredRowLinks.originKey = ownKey;
                g_hoveredRowLinks.targetKeys = targetKeys;
            }
            ImGui::PopID();
        }
        ImGui::SetWindowFontScale(1.0f);
        ImGui::EndChild();
        if (ImGui::SmallButton("Περισσότερα")) {
            n.offset += kPageSize;
            if (n.offset >= n.total) n.offset = 0;
            n.rows = g_app.conn->getPaginatedData(n.table, n.offset, kPageSize);
        }
        ImGui::SetWindowFontScale(1.0f);
        ImGui::EndChild();
        ImGui::PopID();
    }

    // Draw the hovered row's per-row connector line(s), on top of everything
    // (drawn after all node cards, so it always reads above their bodies).
    // Only drawn for the specific row under the mouse, and only to whichever
    // target rows are actually in the currently-loaded page of the target
    // table -- if the matching row isn't loaded (paged elsewhere), nothing is
    // drawn for that link, per spec.
    if (g_hoveredRowLinks.active) {
        ImVec2* origin = FindRowCenter(g_hoveredRowLinks.table, g_hoveredRowLinks.originKey);
        if (origin) {
            // service_users carries two FK targets (services, users); payments too
            // (services, users). Both target tables are tried for every key since
            // the key alone doesn't say which table it belongs to -- FindRowCenter
            // simply won't find a match in the wrong table.
            static const char* targetTables[] = { "services", "users" };
            for (auto& key : g_hoveredRowLinks.targetKeys) {
                for (auto* tbl : targetTables) {
                    ImVec2* dst = FindRowCenter(tbl, key);
                    if (dst) {
                        dl->AddLine(*origin, *dst, IM_COL32(255, 220, 120, 130), 2.0f);
                        dl->AddCircleFilled(*dst, 4.0f, IM_COL32(255, 220, 120, 200));
                        break; // this key matched one table, don't also try the other
                    }
                }
            }
            dl->AddCircleFilled(*origin, 4.0f, IM_COL32(255, 220, 120, 200));
        }
    }

    ImGui::EndChild();
    ImGui::End();
}

// --- Χρονολόγιο (Audit Log) as its own filterable window --------------------
// Three independent toggles, not mutually exclusive:
//   Επιχείρηση (Business) - real company-data CRUD (users/services/payments/service_users)
//   Βάση (DB)              - literally everything, unfiltered (the superset, incl. raw SQL console writes)
//   Ειδοποιήσεις (App)     - same rows as Business, rendered as friendly human-readable notices
// NOTE: the client's own wording for the third category was ambiguous ("the actions
// that cause api calls") -- this is our best-effort mapping to their earlier "App
// actions = Business + notices" spec. Flag to the client for confirmation/correction.
static bool g_showAuditWindow = false;
static bool g_auditShowBusiness = true;
static bool g_auditShowDb = false;
static bool g_auditShowApp = false;

static void DrawAuditLogWindow() {
    if (!g_showAuditWindow) return;
    ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_Appearing);
    ImGui::Begin("Χρονολόγιο Ενεργειών", &g_showAuditWindow);
    ImGui::Checkbox("Επιχείρηση (Business)", &g_auditShowBusiness);
    ImGui::SameLine();
    ImGui::Checkbox("Βάση - τα πάντα (DB)", &g_auditShowDb);
    ImGui::SameLine();
    ImGui::Checkbox("Ειδοποιήσεις (App)", &g_auditShowApp);
    ImGui::Separator();

    ImGui::BeginChild("auditScroll", ImVec2(0, 0), true);
    if (g_auditShowDb) {
        ImGui::TextDisabled("-- Βάση: όλες οι ενέργειες, χωρίς φίλτρο --");
        for (auto& a : g_app.conn->getTimelineActions("", 0, 300, "", false)) ImGui::TextUnformatted(a.c_str());
        ImGui::Spacing();
    }
    if (g_auditShowBusiness) {
        ImGui::TextDisabled("-- Επιχείρηση: αλλαγές σε χρήστες/υπηρεσίες/πληρωμές --");
        for (auto& a : g_app.conn->getTimelineActions("", 0, 300, "business", false)) ImGui::TextUnformatted(a.c_str());
        ImGui::Spacing();
    }
    if (g_auditShowApp) {
        ImGui::TextDisabled("-- Ειδοποιήσεις: ίδιες ενέργειες, σε απλή γλώσσα --");
        for (auto& a : g_app.conn->getTimelineActions("", 0, 300, "business", true)) ImGui::TextUnformatted(a.c_str());
    }
    if (!g_auditShowDb && !g_auditShowBusiness && !g_auditShowApp) {
        ImGui::TextDisabled("Επιλέξτε τουλάχιστον μία κατηγορία παραπάνω.");
    }
    ImGui::EndChild();
    ImGui::End();
}

static char g_dbPathEdit[512] = "";
static bool g_dbPathEditInit = false;
static bool g_dbPathSaved = false;

// Settings -> "Αρχείο σφαλμάτων": logs/errors.csv (every error the app showed, plus graphics/crash errors).
static bool g_showErrorLog = false;
static long long g_errorLogStamp = -1;
static void DrawErrorLogWindow() {
    if (!g_showErrorLog) return;
    static std::vector<errlog::Row> rows;
    static char filter[128] = "";
    namespace fs = std::filesystem;
    std::error_code ec;
    long long stamp = (long long)fs::file_size(fs::u8path(errlog::Path()), ec);
    if (ec) stamp = 0;
    if (stamp != g_errorLogStamp) { rows = errlog::ReadAll(); g_errorLogStamp = stamp; }
    ImGui::SetNextWindowSize(ImVec2(900, 500), ImGuiCond_Appearing);
    if (ImGui::Begin("Αρχείο σφαλμάτων", &g_showErrorLog)) {
        if (ImGui::SmallButton("Άνοιγμα αρχείου (Excel)")) OpenWithSystem(errlog::Path());
        ImGui::SameLine();
        if (ImGui::SmallButton("Φάκελος")) RevealInFolder(errlog::Path());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", errlog::Path().c_str());
        ImGui::SetNextItemWidth(300);
        ImGui::InputTextWithHint("##errf", "Φίλτρο...", filter, sizeof(filter));
        ImGui::SameLine(); ImGui::TextDisabled("%d εγγραφές (νεότερες πρώτα)", (int)rows.size());
        if (rows.empty()) ImGui::TextDisabled("Κανένα σφάλμα δεν έχει καταγραφεί.");
        else if (ImGui::BeginTable("errlog", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                               ImGuiTableFlags_Resizable, ImVec2(0, -1))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Ώρα", ImGuiTableColumnFlags_WidthFixed, 175);
            ImGui::TableSetupColumn("Πηγή", ImGuiTableColumnFlags_WidthFixed, 90);
            ImGui::TableSetupColumn("Μήνυμα", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < rows.size(); i++) {
                auto& r = rows[i];
                if (filter[0] && !SubstrCI(r.message, filter) && !SubstrCI(r.source, filter) && !SubstrCI(r.time, filter)) continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r.time.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r.source.c_str());
                ImGui::TableNextColumn(); ImGui::TextWrapped("%s", r.message.c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

// The archive of everything deleted: hidden rows (services, partners, notes, clients) and snapshots of removed ones
// (charges, buildings, apartments). Nothing is ever erased, so past information stays searchable (taxes...).
static bool g_showArchive = false;
static void DrawArchiveWindow() {
    if (!g_showArchive) return;
    enum { AR_DATE, AR_KIND, AR_TITLE, AR_STATUS, AR_ACT, AR_COUNT };
    static const ListColDef cols[AR_COUNT] = {
        { "Διαγράφηκε", ImGuiTableColumnFlags_WidthFixed, 150.0f, true }, { "Είδος", ImGuiTableColumnFlags_WidthFixed, 120.0f, true },
        { "Τίτλος", ImGuiTableColumnFlags_WidthFixed, 380.0f, true },     { "Κατάσταση", ImGuiTableColumnFlags_WidthFixed, 150.0f, true },
        { "", ImGuiTableColumnFlags_WidthFixed, 110.0f, false } };
    static ListModel<DataConn::DeletionRow> m; static char q[128] = ""; static std::string msg;
    m.Init(AR_COUNT, { { AR_DATE, false } }, 20);
    ImGui::SetNextWindowSize(ImVec2(920, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Αρχείο διαγραφών", &g_showArchive)) { ImGui::End(); return; }
    ImGui::TextWrapped("Ό,τι διαγράφεται δεν σβήνεται από τη βάση: οι υπηρεσίες, οι συνεργάτες, οι σημειώσεις και οι πελάτες απλώς κρύβονται· οι χρεώσεις, τα κτίρια και τα διαμερίσματα κρατούν πλήρες αντίγραφο εδώ. Τα κρυμμένα μπορούν να επανέλθουν.");
    ImGui::SetNextItemWidth(320);
    if (ImGui::InputTextWithHint("##arq", "Αναζήτηση σε τίτλο, είδος ή περιεχόμενο...", q, sizeof(q))) m.g.pager.Reset();
    if (!msg.empty()) { ImGui::SameLine(); ImGui::TextColored(kColIn, "%s", msg.c_str()); }
    const auto& pg = m.Load(q, [&](const SortSpec& s, int off, int lim) { return g_app.conn->pageDeletions(q, MapSort(s, { { AR_DATE, 0 }, { AR_KIND, 1 }, { AR_TITLE, 2 }, { AR_STATUS, 3 } }), off, lim); },
                               [&] { return g_app.conn->countDeletions(q); });
    if (ImGui::BeginTable("archive", AR_COUNT, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                          ImVec2(0, std::max(120.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 2.2f)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ListSetupColumns(m.g.order, cols);
        ListHeaderRow(m.g.order, cols, m.g.sort, m.g.pager);
        auto P = [&](int c) { return ListPos(m.g.order, c); };
        for (auto& r : pg.rows) {
            ImGui::PushID(r.id); ImGui::TableNextRow();
            const char* kind = r.entity == "service" ? "Υπηρεσία" : r.entity == "partner" ? "Συνεργάτης" : r.entity == "note" ? "Σημείωση" :
                               r.entity == "charge" ? "Χρέωση" : r.entity == "building" ? "Πολυκατοικία" : r.entity == "apartment" ? "Διαμέρισμα" :
                               r.entity == "payment" ? "Πληρωμή" : r.entity == "user" ? "Πελάτης" : r.entity.c_str();
            ImGui::TableSetColumnIndex(P(AR_DATE)); ImGui::TextUnformatted(r.deleted_at.c_str());
            ImGui::TableSetColumnIndex(P(AR_KIND)); ImGui::TextUnformatted(kind);
            ImGui::TableSetColumnIndex(P(AR_TITLE)); ImGui::TextUnformatted(r.label.c_str());
            if (ImGui::IsItemHovered() && !r.snapshot.empty()) {
                ImGui::BeginTooltip(); ImGui::PushTextWrapPos(560);
                ImGui::TextUnformatted(r.snapshot.size() > 1500 ? (r.snapshot.substr(0, 1500) + "…").c_str() : r.snapshot.c_str());
                ImGui::PopTextWrapPos(); ImGui::EndTooltip();
            }
            ImGui::TableSetColumnIndex(P(AR_STATUS));
            if (!r.restored_at.empty()) ImGui::TextColored(kColIn, "επαναφέρθηκε");
            else if (r.hidden) ImGui::TextColored(kColOwed, "κρυμμένο");
            else ImGui::TextDisabled("αρχειοθετημένο");
            ImGui::TableSetColumnIndex(P(AR_ACT));
            if (r.hidden && r.restored_at.empty() && ImGui::SmallButton("Επαναφορά")) {
                if (g_app.conn->restoreDeletion(r.id)) {
                    msg = "Επαναφέρθηκε: " + r.label;
                    g_app.reloadAll();
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    DrawPager(m.g.pager, "archivepg");
    ImGui::End();
}

static void DrawSettingsScreen() {
    DrawArchiveWindow();    ImGui::Text("Βάση Δεδομένων: %s", g_app.dbPath.c_str());
    if (!g_dbPathEditInit) {
        // Pre-fill with the currently-active path (relative, if it already is one)
        // so editing starts from something meaningful rather than an empty box.
        std::snprintf(g_dbPathEdit, sizeof(g_dbPathEdit), "%s", g_app.dbPath.c_str());
        g_dbPathEditInit = true;
    }
    ImGui::TextDisabled("Νέα τοποθεσία (σχετική διαδρομή = μεταφέρσιμο σε άλλο PC):");
    ImGui::SetNextItemWidth(400);
    ImGui::InputText("##dbpathedit", g_dbPathEdit, sizeof(g_dbPathEdit));
    ImGui::SameLine();
    if (ImGui::Button("Αποθήκευση (απαιτεί επανεκκίνηση)")) {
        WriteConfiguredDbPath(g_dbPathEdit);
        g_dbPathSaved = true;
    }
    if (g_dbPathSaved) {
        ImGui::TextColored(ImVec4(0.90f, 0.65f, 0.20f, 1),
            "Αποθηκεύτηκε. Κλείστε και ανοίξτε ξανά την εφαρμογή για να εφαρμοστεί.");
    }
    ImGui::Separator();
    if (ImGui::Button("Αντίγραφο ασφαλείας (CSV)")) {
#ifdef _WIN32
        std::string folder = PickFolderDialogWin32();
        if (!folder.empty()) g_app.conn->exportAllTablesToCSV(folder);
#else
        std::string folder = CurrentTimestampFolder();
        g_app.conn->exportAllTablesToCSV(folder);
#endif
    }
    ImGui::SameLine();
    if (ImGui::Button("Γρήγορη προσθήκη ιδιοκτήτη")) OpenAddUser();
    ImGui::SameLine();
    if (ImGui::Button("Χάρτης Βάσης (Node Graph)")) { InitGraphNodes(); g_showNodeGraph = true; }
    ImGui::SameLine();
    if (ImGui::Button("Χρονολόγιο Ενεργειών")) g_showAuditWindow = true;
    ImGui::SameLine();
    if (ImGui::Button("Αρχείο διαγραφών")) g_showArchive = true;
    ImGui::SameLine();
    {
        const bool onEmpty = g_app.dbPath.size() >= 8 && g_app.dbPath.compare(g_app.dbPath.size() - 8, 8, "empty.db") == 0;
        if (ImGui::Button(onEmpty ? "Επιστροφή στην πλήρη βάση" : "Χρήση βάσης 2 (κενή)"))
            RequestConfirm("Αλλαγή βάσης", { {"Βάση", onEmpty ? "κενή (empty.db)" : "πλήρης", onEmpty ? "πλήρης" : "κενή (empty.db)"} }, []() {
                if (SwitchDatabaseAndRelaunch(g_app.dbPath) && g_glfwWindow) glfwSetWindowShouldClose(g_glfwWindow, GLFW_TRUE);
            });
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Κλείνει το πρόγραμμα και το ανοίγει ξανά με την άλλη βάση.\nΗ βάση 2 έχει την ίδια δομή αλλά είναι κενή: ξεκινάτε από το μηδέν.\nΗ πλήρης βάση δεν αλλάζει.");
        ImGui::SameLine(); ImGui::TextColored(onEmpty ? kColOwed : kColMuted, "%s", onEmpty ? "(τώρα: κενή βάση 2)" : "(τώρα: πλήρης βάση)");
    }
    ImGui::SameLine();
    if (ImGui::Button("Αρχείο σφαλμάτων")) { g_showErrorLog = true; g_errorLogStamp = -1; }

    ImGui::Separator();
    ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "Κονσόλα Raw SQL");
    ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "\xE2\x9A\xA0 Εκτελείται απευθείας στη βάση δεδομένων - χωρίς δυνατότητα αναίρεσης.");
    ImGui::TextDisabled("Παραδείγματα:");
    struct SqlExample { const char* label; const char* sql; };
    static const SqlExample sqlExamples[] = {
        { "Χρήστες (20)", "SELECT * FROM users LIMIT 20;" },
        { "Εκκρεμείς πληρωμές", "SELECT * FROM payments WHERE status='pending';" },
        { "Πλήθος υπηρεσιών ανά χρήστη",
          "SELECT u.full_name, COUNT(su.service_id) AS service_count "
          "FROM users u JOIN service_users su ON su.user_id = u.id "
          "GROUP BY u.id ORDER BY service_count DESC;" },
        { "Σύνολο πληρωμών ανά χρήστη",
          "SELECT u.full_name, SUM(p.amount_paid) AS total_paid "
          "FROM users u JOIN payments p ON p.user_id = u.id "
          "GROUP BY u.id ORDER BY total_paid DESC;" },
    };
    for (auto& ex : sqlExamples) {
        if (ImGui::SmallButton(ex.label)) {
            std::snprintf(g_sqlInput, sizeof(g_sqlInput), "%s", ex.sql);
        }
        ImGui::SameLine();
    }
    ImGui::NewLine();
    ImGui::InputTextMultiline("##sql", g_sqlInput, sizeof(g_sqlInput), ImVec2(-1, 100));
    if (ImGui::Button("Εκτέλεση SQL")) {
        // Only a leading SELECT runs immediately; anything else (INSERT/UPDATE/
        // DELETE/DROP/ALTER/etc.) modifies the DB and gets a confirm first.
        std::string q = g_sqlInput;
        size_t start = q.find_first_not_of(" \t\r\n");
        std::string trimmed = (start == std::string::npos) ? "" : q.substr(start);
        std::string upperHead = trimmed.substr(0, 6);
        for (auto& c : upperHead) c = (char)toupper((unsigned char)c);
        bool isSelect = upperHead == "SELECT";
        auto runQuery = [q]() {
            g_sqlError.clear();
            bool ok = g_app.conn->executeRawQuery(q, g_sqlCols, g_sqlRows, g_sqlError);
            g_sqlRan = true;
            if (ok) { g_app.reloadAll(); }
        };
        if (isSelect) {
            runQuery();
        } else {
            RequestConfirm("Αυτή η εντολή θα τροποποιήσει τη βάση δεδομένων - είστε σίγουροι;",
                            { {"SQL", "-", trimmed} }, runQuery);
        }
    }
    if (g_sqlRan) {
        if (!g_sqlError.empty()) {
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "Σφάλμα SQLite: %s", g_sqlError.c_str());
        } else if (!g_sqlCols.empty()) {
            ImGui::Text("%zu σειρές", g_sqlRows.size());
            if (ImGui::BeginTable("sqlres", (int)g_sqlCols.size(), ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 200))) {
                for (auto& c : g_sqlCols) ImGui::TableSetupColumn(c.c_str());
                ImGui::TableHeadersRow();
                for (auto& row : g_sqlRows) {
                    ImGui::TableNextRow();
                    for (size_t i = 0; i < row.size(); i++) { ImGui::TableSetColumnIndex((int)i); ImGui::TextUnformatted(row[i].c_str()); }
                }
                ImGui::EndTable();
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Remembered view settings (column order, sort, list/card choice ...) -- stored in
// ui_prefs.ini next to the database and re-read at start, written whenever anything changes.
// ---------------------------------------------------------------------------
static std::string g_prefsPath, g_prefsLast;

static std::string PrefJoin(const std::vector<int>& v) {
    std::string s; for (size_t i = 0; i < v.size(); i++) s += (i ? "," : "") + std::to_string(v[i]); return s;
}
static std::vector<int> PrefSplit(const std::string& s) {
    std::vector<int> v; std::stringstream ss(s); std::string tok;
    while (std::getline(ss, tok, ',')) { try { v.push_back(std::stoi(tok)); } catch (...) { return {}; } }
    return v;
}
static std::string PrefSortStr(const ColSort& c) {
    std::string s; for (auto& k : c.keys) s += (s.empty() ? "" : ";") + std::to_string(k.first) + ":" + (k.second ? "1" : "0"); return s;
}
static void PrefSortParse(const std::string& s, ColSort& c, int ncols) {
    SortSpec keys; std::stringstream ss(s); std::string tok;
    while (std::getline(ss, tok, ';')) {
        size_t colon = tok.find(':'); if (colon == std::string::npos) continue;
        try { int col = std::stoi(tok.substr(0, colon)); if (col >= 0 && col < ncols) keys.push_back({ col, tok.substr(colon + 1) != "0" }); } catch (...) {}
    }
    c.keys = keys;
}
static void PrefOrderApply(const std::string& s, std::vector<int>& order) {
    auto v = PrefSplit(s);
    if (v.size() != order.size()) return;
    std::vector<int> sorted = v; std::sort(sorted.begin(), sorted.end());
    for (size_t i = 0; i < sorted.size(); i++) if (sorted[i] != (int)i) return;      // must be a permutation
    if (v.back() != (int)v.size() - 1) return;                                       // the actions column stays last
    order = v;
}
static std::string PrefsSerialize() {
    std::string s;
    auto kv = [&](const char* k, const std::string& v) { s += std::string(k) + "=" + v + "\n"; };
    kv("users_order", PrefJoin(g_usersColOrder));       kv("users_sort", PrefSortStr(g_usersSort));
    kv("services_order", PrefJoin(g_servicesColOrder)); kv("services_sort", PrefSortStr(g_servicesSort));
    kv("payments_order", PrefJoin(g_paymentsColOrder)); kv("payments_sort", PrefSortStr(g_paymentsSort));
    kv("users_table", g_usersTableView ? "1" : "0");    kv("users_residence", g_usersShowResidence ? "1" : "0");
    kv("services_table", g_servicesTableView ? "1" : "0"); kv("payments_table", g_paymentsTableView ? "1" : "0");
    kv("report_period", std::to_string(g_repPeriod));
    kv("apt_period", std::to_string(g_aptPeriod));      kv("apt_group", g_aptGroup ? "1" : "0");
    return s;
}
static void PrefsLoad(const std::string& path) {
    g_prefsPath = path;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t eq = line.find('='); if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        auto flag = [&](bool& b) { b = (v != "0"); };
        auto num = [&](int& n, int lo, int hi) { try { int x = std::stoi(v); if (x >= lo && x <= hi) n = x; } catch (...) {} };
        if (k == "users_order") PrefOrderApply(v, g_usersColOrder);
        else if (k == "users_sort") PrefSortParse(v, g_usersSort, 8);
        else if (k == "services_order") PrefOrderApply(v, g_servicesColOrder);
        else if (k == "services_sort") PrefSortParse(v, g_servicesSort, 5);
        else if (k == "payments_order") PrefOrderApply(v, g_paymentsColOrder);
        else if (k == "payments_sort") PrefSortParse(v, g_paymentsSort, 9);
        else if (k == "users_table") flag(g_usersTableView);
        else if (k == "users_residence") flag(g_usersShowResidence);
        else if (k == "services_table") flag(g_servicesTableView);
        else if (k == "payments_table") flag(g_paymentsTableView);
        else if (k == "report_period") num(g_repPeriod, 0, 2);
        else if (k == "apt_period") num(g_aptPeriod, 0, 4);
        else if (k == "apt_group") flag(g_aptGroup);
    }
    g_prefsLast = PrefsSerialize();
}
static void PrefsSaveIfChanged() {
    if (g_prefsPath.empty()) return;
    std::string now = PrefsSerialize();
    if (now == g_prefsLast) return;
    std::ofstream f(g_prefsPath, std::ios::trunc);
    if (f.is_open()) { f << now; g_prefsLast = now; }
}

// A database error pops up as a red bar at the bottom until it is dismissed (or 12 s pass).
static std::string g_errorText; static double g_errorUntil = 0.0;
static void DrawErrorBanner() {
    std::string e = g_app.conn ? g_app.conn->takeLastError() : "";
    if (!e.empty()) { g_errorText = e; g_errorUntil = ImGui::GetTime() + 12.0; }
    if (g_errorText.empty() || ImGui::GetTime() > g_errorUntil) { g_errorText.clear(); return; }
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y - 18.0f), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.45f, 0.10f, 0.12f, 0.96f));
    if (ImGui::Begin("##dberror", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove |
                                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::TextUnformatted("Σφάλμα βάσης δεδομένων:");
        ImGui::SameLine(); ImGui::TextWrapped("%s", g_errorText.c_str());
        ImGui::SameLine(); if (ImGui::SmallButton("Κλείσιμο")) g_errorText.clear();
    }
    ImGui::End();
    ImGui::PopStyleColor();
}


// =====================================================================================================
// RECEIPTS (Αποδείξεις) -- filtered list, read-only receipt view, cancel (void) + reissue,
// and "Νέα είσπραξη": money against a client's open bills (partial, several bills at once, rest = credit).
// =====================================================================================================
static int  g_receiptViewId = -1;
static bool g_showReceiptView = false;

static void OpenReceiptView(int id) { g_receiptViewId = id; g_showReceiptView = id > 0; }

static std::string ReceiptNo(const Receipt& r) {
    char b[32]; std::snprintf(b, sizeof(b), "%s-%06d", SeriesLabel(r.series).c_str(), r.number); return b;
}

// --- filters (each one set narrows the list) ---
static int g_rcClient = -1, g_rcService = -1, g_rcPackage = -1, g_rcBuilding = -1, g_rcMethod = -1;
static std::string g_rcFrom, g_rcTo;
static int g_rcStatus = 0;
static TextBuf g_rcNumber;
static int g_rcOffset = 0;
static const int kRcPage = 50;

static void DrawReceiptsScreen() {
    ImGui::TextDisabled("Οι αποδείξεις δεν αλλάζουν και δεν διαγράφονται. Ένα λάθος διορθώνεται με ακύρωση (με αιτιολογία) και νέα απόδειξη.");
    // ---------------- filters ----------------
    ImGui::BeginChild("rcfilters", ImVec2(0, 0), ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY);
    ImGui::TextDisabled("Από:"); ImGui::SameLine(); DatePicker("rc_from", g_rcFrom, ImVec2(0, 1));
    ImGui::SameLine(); ImGui::TextDisabled("Έως:"); ImGui::SameLine(); DatePicker("rc_to", g_rcTo, ImVec2(0, 1));
    ImGui::SameLine(); ImGui::SetNextItemWidth(150);
    { const char* st[] = { "Όλες", "Σε ισχύ", "Ακυρωμένες" }; ImGui::Combo("##rcstatus", &g_rcStatus, st, 3); }
    ImGui::SameLine(); ImGui::SetNextItemWidth(260);
    ImGui::InputTextWithHint("##rcnum", "Αρ. απόδειξης / χειρόγραφης / τράπεζας / RF", g_rcNumber.data, sizeof(g_rcNumber.data));

    // one row of plain filters, like the other tabs (each one set narrows the list); every picker searches and pages
    DrawUserFilterPicker("Πελάτης:", g_rcClient, 200);
    ImGui::SameLine(); DrawServiceFilterPicker("Υπηρεσία:", g_rcService, 200);
    ImGui::SameLine(); ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Πακέτο:"); ImGui::SameLine();
    PickPackage("rcpkg", g_rcPackage, 200, "Όλα");
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Πολυκατοικία:"); ImGui::SameLine();
    PickBuilding("rcbld", g_rcBuilding, 220, "Όλες");
    ImGui::SameLine(); ImGui::TextDisabled("Τρόπος:"); ImGui::SameLine();
    {
        ImGui::SetNextItemWidth(170);
        if (ImGui::BeginCombo("##rcmeth", g_rcMethod < 0 ? "Όλοι" : kReceiptMethodNames[g_rcMethod])) {
            if (ImGui::Selectable("Όλοι", g_rcMethod < 0)) g_rcMethod = -1;
            for (int i = 0; i < IM_ARRAYSIZE(kReceiptMethodNames); i++)
                if (ImGui::Selectable(kReceiptMethodNames[i], i == g_rcMethod)) g_rcMethod = i;
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Καθαρισμός φίλτρων")) {
        g_rcClient = g_rcService = g_rcPackage = g_rcBuilding = g_rcMethod = -1;
        g_rcFrom.clear(); g_rcTo.clear(); g_rcStatus = 0; g_rcNumber.set("");
    }
    ImGui::EndChild();

    // ---------------- query (cached) ----------------
    DataConn::ReceiptQuery q;
    q.matchAll = true; q.fromDate = g_rcFrom; q.toDate = g_rcTo; q.status = g_rcStatus; q.number = g_rcNumber.str();
    auto cond = [&](DataConn::ReceiptCond::Field f, int id, const std::string& text = "") {
        DataConn::ReceiptCond rc; rc.field = f; rc.id = id; rc.text = text; q.conds.push_back(rc);
    };
    if (g_rcClient > 0)   cond(DataConn::ReceiptCond::Field::Client, g_rcClient);
    if (g_rcService > 0)  cond(DataConn::ReceiptCond::Field::Service, g_rcService);
    if (g_rcPackage > 0)  cond(DataConn::ReceiptCond::Field::Package, g_rcPackage);
    if (g_rcBuilding > 0) cond(DataConn::ReceiptCond::Field::Building, g_rcBuilding);
    if (g_rcMethod >= 0)  cond(DataConn::ReceiptCond::Field::Method, -1, kReceiptMethodCodes[g_rcMethod]);
    static GridState rcGrid;
    if (rcGrid.order.empty()) rcGrid.order = { 0, 1, 2, 7, 3, 4, 5, 6, 8 };
    q.sort = rcGrid.sort.keys;
    std::string key = SortKeyStr(q.sort) + "#" + g_rcFrom + "|" + g_rcTo + "|" + std::to_string(g_rcStatus) + "|" + q.number + "|" +
                      std::to_string(g_rcClient) + "|" + std::to_string(g_rcService) + "|" + std::to_string(g_rcPackage) + "|" +
                      std::to_string(g_rcBuilding) + "|" + std::to_string(g_rcMethod);
    static std::string lastKey;
    if (key != lastKey) { g_rcOffset = 0; lastKey = key; }
    struct RcPage { std::vector<Receipt> rows; long long count = 0; double total = 0; };
    static Cached<RcPage> pageCache;
    const RcPage& pg = pageCache.get(key + "#" + std::to_string(g_rcOffset), [&] {
        RcPage r;
        r.count = g_app.conn->countReceipts(q, &r.total);
        r.rows = g_app.conn->searchReceipts(q, g_rcOffset, kRcPage);
        return r;
    });

    ImGui::Text("%lld αποδείξεις", pg.count);
    ImGui::SameLine(); ImGui::TextDisabled("·  σύνολο σε ισχύ: %s", Eur(pg.total).c_str());
    ImGui::SameLine(0, 30);
    if (ImGui::SmallButton("◄") && g_rcOffset > 0) g_rcOffset = std::max(0, g_rcOffset - kRcPage);
    ImGui::SameLine(); ImGui::Text("%d-%d", pg.count ? g_rcOffset + 1 : 0, (int)std::min<long long>(pg.count, g_rcOffset + kRcPage));
    ImGui::SameLine();
    if (ImGui::SmallButton("►") && g_rcOffset + kRcPage < pg.count) g_rcOffset += kRcPage;

    if (ImGui::BeginTable("rctable", 9, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        static const ListColDef rcCols[9] = {
            { "Αριθμός", ImGuiTableColumnFlags_WidthFixed, 130.0f, true },   { "Ημ/νία", ImGuiTableColumnFlags_WidthFixed, 130.0f, true },
            { "Πελάτης", ImGuiTableColumnFlags_WidthFixed, 230.0f, true },   { "Τρόπος", ImGuiTableColumnFlags_WidthFixed, 150.0f, true },
            { "Αναφορές", ImGuiTableColumnFlags_WidthFixed, 200.0f, true },  { "Σύνολο", ImGuiTableColumnFlags_WidthFixed, 110.0f, true },
            { "Κατάσταση", ImGuiTableColumnFlags_WidthFixed, 130.0f, true },
            { "Για", ImGuiTableColumnFlags_WidthFixed, 160.0f, true }, { "", ImGuiTableColumnFlags_WidthFixed, 90.0f, false } };
        ListSetupColumns(rcGrid.order, rcCols);
        ListHeaderRow(rcGrid.order, rcCols, rcGrid.sort, rcGrid.pager);
        if (rcGrid.pager.offset != 0) { rcGrid.pager.Reset(); g_rcOffset = 0; }       // a new sort starts at the first page
        auto P = [&](int c) { return ListPos(rcGrid.order, c); };
        for (auto& r : pg.rows) {
            bool isVoid = r.status == "void";
            ImGui::PushID(r.id); ImGui::TableNextRow();
            if (isVoid) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.58f, 1));
            ImGui::TableSetColumnIndex(P(0));
            if (ImGui::Selectable(ReceiptNo(r).c_str(), g_receiptViewId == r.id, ImGuiSelectableFlags_SpanAllColumns)) OpenReceiptView(r.id);
            ImGui::TableSetColumnIndex(P(1)); ImGui::TextUnformatted(DateGr(r.issue_date).c_str());
            ImGui::TableSetColumnIndex(P(2)); ImGui::TextUnformatted(r.user_id > 0 ? g_app.userName(r.user_id).c_str() : "—");
            ImGui::TableSetColumnIndex(P(3)); ImGui::TextUnformatted(ReceiptMethodName(r.method).c_str());
            ImGui::TableSetColumnIndex(P(4));
            std::string refs = r.paper_ref.empty() ? "" : "χειρ. " + r.paper_ref;
            if (!r.bank_ref.empty()) refs += (refs.empty() ? "" : " · ") + r.bank_ref;
            ImGui::TextUnformatted(refs.c_str());
            ImGui::TableSetColumnIndex(P(5)); ImGui::Text("%s", Eur(r.total).c_str());
            ImGui::TableSetColumnIndex(P(6));
            if (isVoid) ImGui::TextColored(kColOut, "ΑΚΥΡΩΜΕΝΗ");
            else ImGui::TextColored(ImVec4(0.45f, 0.80f, 0.50f, 1), "σε ισχύ");
            ImGui::TableSetColumnIndex(P(7)); ImGui::TextUnformatted(r.kind == "joint" ? "Κοινόχρηστα" : "Προσωπική χρήση");
            ImGui::TableSetColumnIndex(P(8)); if (ImGui::SmallButton("Προβολή")) OpenReceiptView(r.id);
            if (isVoid) ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// ---------------- receipt view (read-only) + void + reissue ----------------
static TextBuf g_voidReason;
static ReceiptForm g_reissueForm;

static void DrawReceiptViewWindow() {
    if (!g_showReceiptView || g_receiptViewId <= 0) return;
    static Cached<std::tuple<Receipt, std::vector<DataConn::ReceiptLine>, std::vector<MoneyPayment>>> cache;
    const auto& data = cache.get(std::to_string(g_receiptViewId), [] {
        return std::make_tuple(g_app.conn->getReceiptById(g_receiptViewId), g_app.conn->getReceiptLines(g_receiptViewId),
                               g_app.conn->getReceiptMoney(g_receiptViewId));
    });
    const Receipt& r = std::get<0>(data);
    const auto& lines = std::get<1>(data);
    const auto& money = std::get<2>(data);
    if (r.id < 0) { g_showReceiptView = false; return; }

    ImGui::SetNextWindowSize(ImVec2(720, 560), ImGuiCond_FirstUseEver);
    std::string title = "Απόδειξη " + ReceiptNo(r) + "###receiptview";
    if (!ImGui::Begin(title.c_str(), &g_showReceiptView, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    bool isVoid = r.status == "void";

    ImGui::SetWindowFontScale(1.3f);
    ImGui::Text("ΑΠΟΔΕΙΞΗ ΕΙΣΠΡΑΞΗΣ  %s", ReceiptNo(r).c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::SameLine(0, 20);
    if (isVoid) ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1), "[ ΑΚΥΡΩΜΕΝΗ ]");
    else ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.50f, 1), "[ σε ισχύ ]");
    ImGui::Text("%s  ·  %s", r.issue_date.c_str(), r.user_id > 0 ? g_app.userName(r.user_id).c_str() : "—");
    if (r.user_id > 0) { ImGui::SameLine(); if (ImGui::SmallButton("Προφίλ πελάτη")) OpenUserProfile(r.user_id); }
    ImGui::TextColored(r.kind == "joint" ? ImVec4(0.95f, 0.75f, 0.30f, 1) : ImVec4(0.55f, 0.80f, 0.95f, 1), "Είδος: %s", r.kind == "joint" ? "κοινόχρηστα (μερίδια πολυκατοικίας)" : "προσωπική χρήση (δικές του υπηρεσίες)");
    ImGui::Text("Τρόπος: %s", ReceiptMethodName(r.method).c_str());
    if (!r.bank_ref.empty()) { ImGui::SameLine(); ImGui::TextDisabled("·  Αναφορά: %s", r.bank_ref.c_str()); }
    if (!r.paper_ref.empty()) { ImGui::SameLine(); ImGui::TextDisabled("·  Χειρόγραφη: %s", r.paper_ref.c_str()); }
    ImGui::Text("RF: %s", r.rf_code.c_str());
    ImGui::SameLine(); if (ImGui::SmallButton("Αντιγραφή RF")) ImGui::SetClipboardText(r.rf_code.c_str());
    if (isVoid) ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1), "Ακυρώθηκε %s: %s", r.voided_at.c_str(), r.void_reason.c_str());
    ImGui::Separator();

    SectionTitle("Τι εξοφλεί η απόδειξη");
    if (ImGui::BeginTable("rclines", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Ημ/νία", ImGuiTableColumnFlags_WidthFixed, 95);
        ImGui::TableSetupColumn("Υπηρεσία / πακέτο / κοινόχρηστο", ImGuiTableColumnFlags_WidthStretch, 2.4f);
        ImGui::TableSetupColumn("Σε ποιον", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Χρέωση", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Εξοφλείται", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableHeadersRow();
        for (auto& l : lines) {
            ImGui::PushID(l.charge.id); ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(DateGr(l.charge.year, l.charge.month, l.charge.day).c_str());
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(ChargeLabel(l.charge).c_str());
            if (!l.charge.notes.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", l.charge.notes.c_str());
            ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(g_app.userName(l.charge.user_id).c_str());
            ImGui::TableSetColumnIndex(3); ImGui::Text("%s", Eur(l.charge.amount_due).c_str());
            ImGui::TableSetColumnIndex(4); ImGui::Text("%s", Eur(l.amount).c_str());
            ImGui::TableSetColumnIndex(5); if (ImGui::SmallButton("Χρέωση")) OpenPaymentView(l.charge.id);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    SectionTitle("Πληρωμές που αναφέρονται σε αυτήν");
    if (money.empty()) ImGui::TextDisabled("Καμία.");
    for (auto& m : money) ImGui::Text("%s · %s · %s%s", DateGr(m.pay_date).c_str(), ReceiptMethodName(m.method).c_str(), Eur(m.amount).c_str(), m.reference.empty() ? "" : ("  ·  " + m.reference).c_str());
    double unallocated = 0;
    for (auto& m : money) unallocated += m.amount - m.allocated;
    DrawAttachmentsPanel("receipt", r.id);
    ImGui::Text("Σύνολο: %s", Eur(r.total).c_str());
    if (ToCents(unallocated) > 0) { ImGui::SameLine(); ImGui::TextDisabled("(εκ των οποίων %s πίστωση πελάτη)", Eur(unallocated).c_str()); }
    ImGui::Spacing();
    if (!isVoid) {
        if (ImGui::Button("Ακύρωση απόδειξης…")) { g_voidReason.set(""); ImGui::OpenPopup("Ακύρωση απόδειξης"); }
        if (ImGui::BeginPopupModal("Ακύρωση απόδειξης", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("Η απόδειξη %s θα μείνει στα βιβλία ως ΑΚΥΡΩΜΕΝΗ. Οι πληρωμές της ξεκλειδώνουν για διόρθωση και νέα απόδειξη.", ReceiptNo(r).c_str());
            ImGui::InputTextMultiline("Αιτιολογία", g_voidReason.data, sizeof(g_voidReason.data), ImVec2(420, 60));
            bool ok = g_voidReason.str().size() >= 5;
            if (!ok) ImGui::TextDisabled("Απαιτείται αιτιολογία (τουλάχιστον 5 χαρακτήρες).");
            ImGui::BeginDisabled(!ok);
            if (ImGui::Button("Ακύρωση απόδειξης", ImVec2(180, 0))) {
                g_app.conn->setAuditReason(g_voidReason.str());
                g_app.conn->voidReceipt(r.id, g_voidReason.str());
                g_app.conn->setAuditReason("");
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Πίσω", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    } else {
        // reissue for the same money (whose receipt is now void)
        bool anyFree = false;
        for (auto& m : money) if (m.receipt_id == r.id) anyFree = true;
        if (anyFree) {
            if (ImGui::CollapsingHeader("Νέα απόδειξη για τις ίδιες πληρωμές")) {
                double tot = 0; for (auto& m : money) tot += m.amount;
                bool ok = DrawReceiptForm("reissue", g_reissueForm, tot, (int)money.size());
                ImGui::BeginDisabled(!ok || !g_reissueForm.issue);
                if (ImGui::Button("Έκδοση", ImVec2(150, 0))) {
                    std::vector<int> ids; for (auto& m : money) ids.push_back(m.id);
                    int nid = g_app.conn->issueReceiptForMoney(ReceiptFromForm(g_reissueForm, r.user_id), ids);
                    if (nid > 0) { g_reissueForm = ReceiptForm{}; OpenReceiptView(nid); }
                }
                ImGui::EndDisabled();
            }
        } else {
            ImGui::TextDisabled("Οι πληρωμές αυτής της απόδειξης έχουν ήδη νέα απόδειξη.");
        }
    }
    ImGui::End();
}

// ---------------- Νέα είσπραξη: money against a client's open bills ----------------
static bool g_showRecordPay = false, g_recordPayWasOpen = false;
static int g_rpUser = -1;
static std::string g_rpDate;
static TextBuf g_rpAmount, g_rpNotes;
static std::map<int, TextBuf> g_rpAlloc;     // charge id -> amount typed
static std::map<int, double> g_rpPreset;     // filled in by «Είσπραξη» on one bill, applied when the dialog opens
static std::set<int> g_rpExtra;              // scheduled bills shown in the dialog besides the open ones
static ReceiptForm g_rpReceipt;

static void OpenRecordPayment(int userId) {
    g_showRecordPay = true; g_rpUser = userId; g_rpDate = CurrentDateString();
    g_rpAmount.set(""); g_rpNotes.set(""); g_rpAlloc.clear(); g_rpReceipt = ReceiptForm{}; g_rpExtra.clear();
}

static void DrawRecordPaymentDialog() {
    if (g_showRecordPay && !g_recordPayWasOpen) ImGui::OpenPopup("Νέα είσπραξη");
    g_recordPayWasOpen = g_showRecordPay;
    if (!g_showRecordPay) return;
    ImGui::SetNextWindowSize(ImVec2(760, 0), ImGuiCond_Appearing);
    if (!BeginGuardedModal("Νέα είσπραξη", g_showRecordPay, ImGuiWindowFlags_AlwaysAutoResize)) return;

    DrawUserFilterPicker("Πελάτης:", g_rpUser, 260);
    if (g_rpUser <= 0) { ImGui::SameLine(); ImGui::TextDisabled("επιλέξτε πελάτη"); }
    static int lastUser = -2;
    if (lastUser != g_rpUser) { g_rpAlloc.clear(); lastUser = g_rpUser; }
    if (!g_rpPreset.empty()) { for (auto& [cid, v] : g_rpPreset) g_rpAlloc[cid].set(FormatMoney(v)); g_rpPreset.clear(); }
    ImGui::TextUnformatted("Ημερομηνία:"); ImGui::SameLine(); DatePicker("rpdate", g_rpDate, ImVec2(0, 1));
    ImGui::SetNextItemWidth(160);
    ImGui::InputTextWithHint("Ποσό που εισπράχθηκε (€)", "π.χ. 120,50", g_rpAmount.data, sizeof(g_rpAmount.data));
    bool ok = g_rpAmount.str().empty() ? false : MoneyFieldHint(g_rpAmount);   // empty: just not ready yet, no red text
    double amount = 0; ParseMoney(g_rpAmount.str(), amount);

    static Cached<std::vector<Payment>> openCache;
    const auto& openOnly = openCache.get(std::to_string(g_rpUser), [] { return g_rpUser > 0 ? g_app.conn->getOpenCharges(g_rpUser) : std::vector<Payment>{}; });
    std::vector<Payment> open = openOnly;                // + a scheduled bill this dialog was opened for
    for (int id : g_rpExtra) {
        bool have = false; for (auto& c : open) if (c.id == id) have = true;
        Payment c = g_app.conn->getPaymentById(id);
        if (!have && c.id > 0 && c.user_id == g_rpUser) { c.balance = MoneySub(c.amount_due, c.amount_paid); open.insert(open.begin(), c); }
    }
    double credit = 0;
    if (g_rpUser > 0) { static Cached<double> cr; credit = cr.get(std::to_string(g_rpUser), [] { return g_app.conn->getClientCredit(g_rpUser); }); }
    if (ToCents(credit) > 0) ImGui::TextColored(ImVec4(0.55f, 0.80f, 0.95f, 1), "Ο πελάτης έχει ήδη πίστωση %s.", Eur(credit).c_str());

    SectionTitle("Τι εξοφλεί");
    if (ImGui::SmallButton("Αυτόματα: παλαιότερες πρώτα")) {
        long long left = ToCents(amount);
        g_rpAlloc.clear();
        for (auto& c : open) {
            long long take = std::min(left, ToCents(c.balance));
            if (take <= 0) break;
            g_rpAlloc[c.id].set(FormatMoney(FromCents(take)));
            left -= take;
        }
    }
    long long allocCents = 0;
    if (open.empty()) ImGui::TextDisabled(g_rpUser > 0 ? "Ο πελάτης δεν έχει ανοιχτές χρεώσεις. Το ποσό θα μείνει ως πίστωση." : "Επιλέξτε πελάτη.");
    else if (ImGui::BeginTable("rpopen", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0, 220))) {
        ImGui::TableSetupColumn("Ημ/νία", ImGuiTableColumnFlags_WidthFixed, 95);
        ImGui::TableSetupColumn("Υπηρεσία", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Χρέωση", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Υπόλοιπο", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Εξοφλείται τώρα", ImGuiTableColumnFlags_WidthFixed, 140);
        ImGui::TableHeadersRow();
        for (auto& c : open) {
            ImGui::PushID(c.id); ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(DateGr(c.year, c.month, c.day).c_str());
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(ChargeLabel(c).c_str());
            ImGui::TableSetColumnIndex(2); ImGui::Text("%s", FormatMoney(c.amount_due).c_str());
            ImGui::TableSetColumnIndex(3); ImGui::Text("%s", FormatMoney(c.balance).c_str());
            ImGui::TableSetColumnIndex(4);
            auto& buf = g_rpAlloc[c.id];
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##a", "0,00", buf.data, sizeof(buf.data));
            double v = 0;
            if (!buf.str().empty()) {
                if (!ParseMoney(buf.str(), v)) { ok = false; ImGui::SameLine(); ImGui::TextColored(kInvalidMoneyCol, "!"); }
                else if (ToCents(v) > ToCents(c.balance)) { ok = false; ImGui::TextColored(kInvalidMoneyCol, "πάνω από το υπόλοιπο"); }
                else allocCents += ToCents(v);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    long long rest = ToCents(amount) - allocCents;
    if (rest < 0) { ImGui::TextColored(kInvalidMoneyCol, "Κατανέμετε %s περισσότερα από όσα εισπράχθηκαν.", Eur(FromCents(-rest)).c_str()); ok = false; }
    else if (rest > 0) ImGui::TextColored(ImVec4(0.55f, 0.80f, 0.95f, 1), "Θα μείνουν %s ως πίστωση του πελάτη.", Eur(FromCents(rest)).c_str());
    if (ToCents(amount) <= 0 || g_rpUser <= 0) ok = false;
    ImGui::InputTextMultiline("Σημειώσεις", g_rpNotes.data, sizeof(g_rpNotes.data), ImVec2(0, 40));
    {   // money that only pays installments of "one receipt at the end" plans: the plan issues that receipt
        bool allEndPlan = true, any = false;
        for (auto& [cid, buf] : g_rpAlloc) {
            double v = 0; if (!ParseMoney(buf.str(), v) || ToCents(v) <= 0) continue;
            any = true;
            int batch = -1; for (auto& c : open) if (c.id == cid) batch = c.batch_id;
            auto pl = g_app.conn->getPlanForBatch(batch);
            if (!(pl.id > 0 && pl.receipt_mode == "end")) allEndPlan = false;
        }
        static bool lastEnd = false;
        bool endOnly = any && allEndPlan;
        if (endOnly && !lastEnd) g_rpReceipt.issue = false;
        lastEnd = endOnly;
        if (endOnly) ImGui::TextColored(ImVec4(0.55f, 0.80f, 0.95f, 1), "Δόσεις προγράμματος με μία απόδειξη στο τέλος: η απόδειξη εκδίδεται αυτόματα με την τελευταία δόση.");
    }
    ok = DrawReceiptForm("rprec", g_rpReceipt, amount, 1) && ok;

    ImGui::BeginDisabled(!ok);
    if (ImGui::Button("Καταχώριση", ImVec2(150, 0))) {
        MoneyPayment m;
        m.user_id = g_rpUser; m.pay_date = g_rpDate; m.amount = amount; m.direction = "in"; m.notes = g_rpNotes.str();
        m.method = kReceiptMethodCodes[g_rpReceipt.method];
        std::vector<Allocation> allocs;
        for (auto& [cid, buf] : g_rpAlloc) { double v = 0; if (ParseMoney(buf.str(), v) && ToCents(v) > 0) allocs.push_back({-1, cid, v}); }
        bool withReceipt = g_rpReceipt.issue;
        Receipt rc = ReceiptFromForm(g_rpReceipt, g_rpUser);
        std::vector<FieldDiff> d = { {"Πελάτης", "-", g_app.userName(g_rpUser)}, {"Ποσό", "-", Eur(amount)},
                                     {"Χρεώσεις που εξοφλούνται", "-", std::to_string(allocs.size())},
                                     {"Πίστωση", "-", Eur(FromCents(rest))}, {"Απόδειξη", "-", ReceiptSummary(g_rpReceipt)} };
        RequestConfirm("Νέα είσπραξη", d, [m, allocs, withReceipt, rc]() {
            g_app.conn->recordPayment(m, allocs, withReceipt ? &rc : nullptr);
        });
        g_showRecordPay = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Άκυρο", ImVec2(150, 0))) { g_showRecordPay = false; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

// =====================================================================================================
// NAVIGATION -- a thin bar of icons on the left that widens over the page while the mouse is on it.
// Icons are drawn as shapes (no icon font needed), so they look the same on every machine.
// =====================================================================================================
enum Page { PAGE_HOME, PAGE_CLIENTS, PAGE_BUILDINGS, PAGE_APARTMENTS, PAGE_SERVICES, PAGE_JOINT, PAGE_PACKAGES,
            PAGE_PAYMENTS, PAGE_RECEIPTS, PAGE_OFFERS, PAGE_SEARCH, PAGE_NOTES, PAGE_PARTNERS, PAGE_SETTINGS, PAGE_DASHBOARD, PAGE_BILLS, PAGE_CASH };
static int g_page = PAGE_HOME;
static const float kNavCollapsed = 56.0f, kNavExpanded = 236.0f;
enum NavIcon { IC_HOME, IC_PERSON, IC_BUILDING, IC_DOOR, IC_TAG, IC_PIE, IC_BOX, IC_COINS, IC_RECEIPT, IC_PERCENT,
               IC_SEARCH, IC_NOTE, IC_CHART, IC_GEAR, IC_PEOPLE };

static void DrawNavIcon(ImDrawList* dl, ImVec2 c, float s, int icon, ImU32 col) {
    const float t = std::max(1.6f, s * 0.08f);          // stroke
    const float h = s * 0.5f;
    auto P = [&](float x, float y) { return ImVec2(c.x + x * h, c.y + y * h); };
    switch (icon) {
    case IC_HOME:
        dl->AddTriangle(P(-0.95f, -0.05f), P(0, -0.9f), P(0.95f, -0.05f), col, t);
        dl->AddRect(P(-0.7f, -0.05f), P(0.7f, 0.85f), col, 1.0f, 0, t);
        dl->AddRectFilled(P(-0.18f, 0.3f), P(0.18f, 0.85f), col);
        break;
    case IC_PERSON:
        dl->AddCircle(P(0, -0.45f), h * 0.38f, col, 20, t);
        dl->PathArcTo(P(0, 0.95f), h * 0.8f, 3.14159f * 1.05f, 3.14159f * 1.95f, 20); dl->PathStroke(col, 0, t);
        break;
    case IC_BUILDING:
        dl->AddRect(P(-0.6f, -0.9f), P(0.6f, 0.9f), col, 1.0f, 0, t);
        for (int r = 0; r < 3; r++) for (int k = 0; k < 2; k++) {
            float x = -0.32f + k * 0.42f, y = -0.6f + r * 0.42f;
            dl->AddRectFilled(P(x, y), P(x + 0.22f, y + 0.22f), col);
        }
        dl->AddRectFilled(P(-0.12f, 0.55f), P(0.12f, 0.9f), col);
        break;
    case IC_DOOR:
        dl->AddRect(P(-0.5f, -0.9f), P(0.5f, 0.9f), col, 1.0f, 0, t);
        dl->AddCircleFilled(P(0.25f, 0.05f), h * 0.1f, col);
        dl->AddLine(P(-0.85f, 0.9f), P(0.85f, 0.9f), col, t);
        break;
    case IC_TAG:
        dl->PathLineTo(P(-0.9f, -0.15f)); dl->PathLineTo(P(-0.15f, -0.9f)); dl->PathLineTo(P(0.85f, -0.9f));
        dl->PathLineTo(P(0.85f, 0.1f)); dl->PathLineTo(P(0.1f, 0.85f)); dl->PathStroke(col, ImDrawFlags_Closed, t);
        dl->AddCircle(P(0.45f, -0.5f), h * 0.13f, col, 12, t);
        break;
    case IC_PIE:
        dl->AddCircle(c, h * 0.85f, col, 28, t);
        dl->AddLine(c, P(0, -0.85f), col, t); dl->AddLine(c, P(0.74f, 0.42f), col, t); dl->AddLine(c, P(-0.74f, 0.42f), col, t);
        break;
    case IC_BOX:
        dl->AddRect(P(-0.8f, -0.35f), P(0.8f, 0.85f), col, 1.0f, 0, t);
        dl->AddQuad(P(-0.8f, -0.35f), P(-0.45f, -0.85f), P(0.45f, -0.85f), P(0.8f, -0.35f), col, t);
        dl->AddLine(P(-0.2f, -0.35f), P(-0.2f, 0.2f), col, t); dl->AddLine(P(0.2f, -0.35f), P(0.2f, 0.2f), col, t);
        break;
    case IC_COINS:
        dl->AddRect(P(-0.95f, -0.55f), P(0.95f, 0.55f), col, 2.0f, 0, t);
        dl->AddCircle(c, h * 0.3f, col, 18, t);
        dl->AddCircleFilled(P(-0.65f, 0), h * 0.08f, col); dl->AddCircleFilled(P(0.65f, 0), h * 0.08f, col);
        break;
    case IC_RECEIPT: {
        dl->PathLineTo(P(-0.65f, 0.9f)); dl->PathLineTo(P(-0.65f, -0.9f)); dl->PathLineTo(P(0.65f, -0.9f)); dl->PathLineTo(P(0.65f, 0.9f));
        dl->PathLineTo(P(0.43f, 0.7f)); dl->PathLineTo(P(0.22f, 0.9f)); dl->PathLineTo(P(0.0f, 0.7f)); dl->PathLineTo(P(-0.22f, 0.9f));
        dl->PathLineTo(P(-0.43f, 0.7f)); dl->PathStroke(col, ImDrawFlags_Closed, t);
        for (int i = 0; i < 3; i++) dl->AddLine(P(-0.38f, -0.55f + i * 0.38f), P(0.38f, -0.55f + i * 0.38f), col, t);
        break; }
    case IC_PERCENT:
        dl->AddLine(P(0.7f, -0.8f), P(-0.7f, 0.8f), col, t * 1.2f);
        dl->AddCircle(P(-0.5f, -0.5f), h * 0.25f, col, 16, t); dl->AddCircle(P(0.5f, 0.5f), h * 0.25f, col, 16, t);
        break;
    case IC_SEARCH:
        dl->AddCircle(P(-0.2f, -0.2f), h * 0.55f, col, 24, t * 1.1f);
        dl->AddLine(P(0.22f, 0.22f), P(0.85f, 0.85f), col, t * 1.6f);
        break;
    case IC_NOTE:
        dl->PathLineTo(P(-0.7f, -0.85f)); dl->PathLineTo(P(0.35f, -0.85f)); dl->PathLineTo(P(0.7f, -0.5f)); dl->PathLineTo(P(0.7f, 0.85f));
        dl->PathLineTo(P(-0.7f, 0.85f)); dl->PathStroke(col, ImDrawFlags_Closed, t);
        for (int i = 0; i < 3; i++) dl->AddLine(P(-0.4f, -0.3f + i * 0.35f), P(0.4f, -0.3f + i * 0.35f), col, t);
        break;
    case IC_CHART:
        dl->AddLine(P(-0.9f, 0.85f), P(0.9f, 0.85f), col, t);
        dl->AddRectFilled(P(-0.7f, 0.1f), P(-0.3f, 0.85f), col); dl->AddRectFilled(P(-0.2f, -0.6f), P(0.2f, 0.85f), col);
        dl->AddRectFilled(P(0.3f, -0.2f), P(0.7f, 0.85f), col);
        break;
    case IC_PEOPLE:
        dl->AddCircle(P(-0.35f, -0.45f), h * 0.3f, col, 18, t);
        dl->PathArcTo(P(-0.35f, 0.95f), h * 0.62f, 3.14159f * 1.05f, 3.14159f * 1.95f, 18); dl->PathStroke(col, 0, t);
        dl->AddCircle(P(0.5f, -0.25f), h * 0.24f, col, 16, t);
        dl->PathArcTo(P(0.5f, 0.95f), h * 0.5f, 3.14159f * 1.1f, 3.14159f * 1.9f, 16); dl->PathStroke(col, 0, t);
        break;
    case IC_GEAR: {
        for (int i = 0; i < 8; i++) {
            float a = i * 3.14159f / 4.0f;
            dl->AddLine(P(std::cos(a) * 0.55f, std::sin(a) * 0.55f), P(std::cos(a) * 0.92f, std::sin(a) * 0.92f), col, t * 2.0f);
        }
        dl->AddCircle(c, h * 0.6f, col, 24, t * 1.4f);
        dl->AddCircle(c, h * 0.22f, col, 16, t);
        break; }
    }
}

struct NavItem { int page; int icon; const char* label; };
static const NavItem kNavItems[] = {
    { PAGE_HOME, IC_HOME, "Αρχική" },             { PAGE_CLIENTS, IC_PERSON, "Πελάτες" },
    { PAGE_BUILDINGS, IC_BUILDING, "Πολυκατοικίες" },
    { PAGE_SERVICES, IC_TAG, "Υπηρεσίες" },        { PAGE_JOINT, IC_PIE, "Κοινόχρηστα" },
    { PAGE_PACKAGES, IC_BOX, "Πακέτα Υπηρεσιών" }, { PAGE_BILLS, IC_RECEIPT, "Χρεώσεις" },
    { PAGE_CASH, IC_COINS, "Ταμείο" },             { PAGE_PAYMENTS, IC_COINS, "Πληρωμές" },
    { PAGE_RECEIPTS, IC_RECEIPT, "Αποδείξεις" },   { PAGE_OFFERS, IC_PERCENT, "Προσφορές" },
    { PAGE_SEARCH, IC_SEARCH, "Αναζήτηση" },       { PAGE_NOTES, IC_NOTE, "Σημειώσεις" },
    { PAGE_PARTNERS, IC_PEOPLE, "Συνεργάτες" },
};

static void DrawNavBar() {
    static float width = kNavCollapsed;
    static bool hovered = false;
    float target = hovered ? kNavExpanded : kNavCollapsed;
    width += (target - width) * std::min(1.0f, ImGui::GetIO().DeltaTime * 14.0f);
    if (std::fabs(width - target) < 0.5f) width = target;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(width, vp->WorkSize.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 10));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.12f, 0.16f, 0.98f));
    ImGui::Begin("##nav", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rowH = 44.0f;
    auto item = [&](int page, int icon, const char* label) {
        ImGui::PushID(page);
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool sel = g_page == page;
        if (ImGui::InvisibleButton("nav", ImVec2(width, rowH))) g_page = page;
        bool hov = ImGui::IsItemHovered();
        if (sel) dl->AddRectFilled(p, ImVec2(p.x + width, p.y + rowH), ImGui::GetColorU32(ImVec4(0.20f, 0.42f, 0.62f, 0.55f)));
        else if (hov) dl->AddRectFilled(p, ImVec2(p.x + width, p.y + rowH), ImGui::GetColorU32(ImVec4(1, 1, 1, 0.06f)));
        if (sel) dl->AddRectFilled(p, ImVec2(p.x + 3, p.y + rowH), ImGui::GetColorU32(kColLink));
        ImU32 col = ImGui::GetColorU32(sel ? ImVec4(0.92f, 0.96f, 1.0f, 1) : ImVec4(0.70f, 0.75f, 0.82f, 1));
        DrawNavIcon(dl, ImVec2(p.x + kNavCollapsed * 0.5f, p.y + rowH * 0.5f), 22.0f, icon, col);
        if (width > kNavCollapsed + 40.0f) {
            float a = std::min(1.0f, (width - kNavCollapsed - 40.0f) / 80.0f);
            ImVec4 tc = sel ? ImVec4(0.92f, 0.96f, 1.0f, a) : ImVec4(0.78f, 0.82f, 0.88f, a);
            dl->AddText(ImVec2(p.x + kNavCollapsed, p.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f), ImGui::GetColorU32(tc), label);
        }
        ImGui::PopID();
    };
    for (auto& it : kNavItems) item(it.page, it.icon, it.label);
    // settings at the bottom (a developer page; it will not stay in the final product)
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ImGui::GetWindowHeight() - rowH - 14.0f));
    item(PAGE_SETTINGS, IC_GEAR, "Ρυθμίσεις");
    hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

// =====================================================================================================
// OFFERS (Προσφορές) -- discounts for one client or everyone, on all services, one service or one package, for a
// limited time. They never change prices: a matching offer is suggested when charging, and can be claimed or ignored.
// =====================================================================================================
static int DaysBetween(const std::string& a, const std::string& b) {        // b - a in days
    int y1, m1, d1, y2, m2, d2;
    if (std::sscanf(a.c_str(), "%d-%d-%d", &y1, &m1, &d1) != 3 || std::sscanf(b.c_str(), "%d-%d-%d", &y2, &m2, &d2) != 3) return 0;
    return (int)(RepDaysFromCivil(y2, m2, d2) - RepDaysFromCivil(y1, m1, d1));
}
static std::string OfferValueText(const Offer& o) {
    if (o.kind == "percent") { char b[24]; std::snprintf(b, sizeof(b), "-%g%%", o.value); return b; }
    return "-" + Eur(o.value);
}
static std::string OfferTargetText(const Offer& o) {
    if (o.target_type == "service") return "Υπηρεσία: " + g_app.serviceLabel(o.target_id);
    if (o.target_type == "package") {
        static std::map<int, std::string> names;
        if (!names.count(o.target_id)) names[o.target_id] = g_app.conn->getPackageLabel(o.target_id);
        return "Πακέτο: " + names[o.target_id];
    }
    return "Όλες οι υπηρεσίες";
}
// "Ενεργή · λήγει σε 5 ημέρες" / "Ξεκινά σε 3 ημέρες" / "Έληξε" / "Ακυρώθηκε"
static std::string OfferStateText(const Offer& o, ImVec4& col) {
    std::string today = CurrentDateString();
    if (o.status == "cancelled") { col = kColMuted; return "Ακυρώθηκε"; }
    if (o.valid_to < today) { col = kColMuted; return "Έληξε (" + DateGr(o.valid_from) + " - " + DateGr(o.valid_to) + ")"; }
    if (o.valid_from > today) { col = kColSched; return "Ξεκινά σε " + std::to_string(DaysBetween(today, o.valid_from)) + " ημ."; }
    int left = DaysBetween(today, o.valid_to);
    col = left <= 3 ? kColOwed : kColIn;
    return left == 0 ? "Ενεργή · λήγει σήμερα" : "Ενεργή · λήγει σε " + std::to_string(left) + (left == 1 ? " ημέρα" : " ημέρες");
}

static bool g_showOffer = false, g_offerWasOpen = false;
static Offer g_of;
static TextBuf of_name, of_note; static double of_value = 10.0; static int of_kind = 0, of_who = 0, of_target = 0;
static int of_user = -1, of_svc = -1, of_pkg = -1;
static std::string of_from, of_to;

static void OpenOfferDialog(const Offer* o) {
    g_showOffer = true;
    g_of = o ? *o : Offer{};
    of_name.set(g_of.name); of_note.set(g_of.note);
    of_kind = g_of.kind == "fixed" ? 1 : 0; of_value = o ? g_of.value : 10.0;
    of_who = g_of.user_id > 0 ? 1 : 0; of_user = g_of.user_id;
    of_target = g_of.target_type == "service" ? 1 : g_of.target_type == "package" ? 2 : 0;
    of_svc = of_target == 1 ? g_of.target_id : -1; of_pkg = of_target == 2 ? g_of.target_id : -1;
    of_from = o ? g_of.valid_from : CurrentDateString();
    of_to = o ? g_of.valid_to : AddPeriodIso(CurrentDateString(), 1, 0);
}

static void DrawOfferDialog() {
    const char* title = "Προσφορά###offerdlg";
    if (g_showOffer && !g_offerWasOpen) ImGui::OpenPopup(title);
    g_offerWasOpen = g_showOffer;
    if (!g_showOffer) return;
    ImGui::SetNextWindowSize(ImVec2(620, 0), ImGuiCond_Appearing);
    if (!BeginGuardedModal(title, g_showOffer, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextColored(kColLink, "%s", g_of.id > 0 ? "Επεξεργασία προσφοράς" : "Νέα προσφορά");
    ImGui::SetNextItemWidth(420); ImGui::InputTextWithHint("Όνομα", "π.χ. Φθινοπωρινή έκπτωση απεντόμωσης", of_name.data, sizeof(of_name.data));
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Έκπτωση:"); ImGui::SameLine();
    ImGui::RadioButton("Ποσοστό %", &of_kind, 0); ImGui::SameLine(); ImGui::RadioButton("Ποσό €", &of_kind, 1); ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    if (of_kind == 0) { ImGui::InputDouble("##ofv", &of_value, 0, 0, "%.0f %%"); of_value = std::max(0.0, std::min(of_value, 100.0)); }
    else InputMoney("##ofm", &of_value);
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Για:"); ImGui::SameLine();
    ImGui::RadioButton("Όλους τους πελάτες", &of_who, 0); ImGui::SameLine(); ImGui::RadioButton("Συγκεκριμένο πελάτη", &of_who, 1);
    if (of_who == 1) { ImGui::SameLine(); PickClient("ofcli", of_user, 220); }
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Σε:"); ImGui::SameLine();
    ImGui::RadioButton("Όλες τις υπηρεσίες", &of_target, 0); ImGui::SameLine();
    ImGui::RadioButton("Μία υπηρεσία", &of_target, 1); ImGui::SameLine(); ImGui::RadioButton("Ένα πακέτο", &of_target, 2);
    if (of_target == 1) PickService("ofsvc", of_svc, 420);
    if (of_target == 2) PickPackage("ofpkg", of_pkg, 420);
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Ισχύει από:"); ImGui::SameLine(); DatePicker("offrom", of_from, ImVec2(0, 1));
    ImGui::SameLine(); ImGui::TextUnformatted("έως:"); ImGui::SameLine(); DatePicker("ofto", of_to, ImVec2(0, 1));
    if (!of_from.empty() && !of_to.empty() && of_to >= of_from)
        ImGui::TextDisabled("Διάρκεια %d ημέρες · λήγει αυτόματα στις %s", DaysBetween(of_from, of_to) + 1, DateGr(of_to).c_str());
    ImGui::SetNextItemWidth(420); ImGui::InputTextWithHint("Σημείωση", "προαιρετικά", of_note.data, sizeof(of_note.data));
    std::string problem;
    if (of_name.str().find_first_not_of(" ") == std::string::npos) problem = "Δώστε όνομα.";
    else if (of_value <= 0) problem = "Δώστε την έκπτωση.";
    else if (of_who == 1 && of_user <= 0) problem = "Επιλέξτε πελάτη.";
    else if ((of_target == 1 && of_svc <= 0) || (of_target == 2 && of_pkg <= 0)) problem = "Επιλέξτε υπηρεσία / πακέτο.";
    else if (of_from.empty() || of_to.empty() || of_to < of_from) problem = "Ελέγξτε τις ημερομηνίες.";
    if (!problem.empty()) ImGui::TextColored(kInvalidMoneyCol, "%s", problem.c_str());
    ImGui::Spacing();
    ImGui::BeginDisabled(!problem.empty());
    if (ImGui::Button("Αποθήκευση...", ImVec2(160, 0))) {
        Offer o = g_of;
        o.name = of_name.str(); o.note = of_note.str(); o.kind = of_kind == 0 ? "percent" : "fixed"; o.value = of_value;
        o.user_id = of_who == 1 ? of_user : -1;
        o.target_type = of_target == 1 ? "service" : of_target == 2 ? "package" : "all";
        o.target_id = of_target == 1 ? of_svc : of_target == 2 ? of_pkg : -1;
        o.valid_from = of_from; o.valid_to = of_to;
        std::vector<FieldDiff> d = { {"Όνομα", g_of.id > 0 ? g_of.name : "-", o.name},
            {"Έκπτωση", g_of.id > 0 ? OfferValueText(g_of) : "-", OfferValueText(o)},
            {"Για", g_of.id > 0 ? (g_of.user_id > 0 ? g_app.userName(g_of.user_id) : "Όλους") : "-", o.user_id > 0 ? g_app.userName(o.user_id) : "Όλους"},
            {"Σε", g_of.id > 0 ? OfferTargetText(g_of) : "-", OfferTargetText(o)},
            {"Ισχύς", g_of.id > 0 ? DateGr(g_of.valid_from) + " – " + DateGr(g_of.valid_to) : "-", DateGr(o.valid_from) + " – " + DateGr(o.valid_to)} };
        RequestConfirm(g_of.id > 0 ? "Αλλαγή προσφοράς (ό,τι χρεώθηκε ήδη δεν αλλάζει)" : "Νέα προσφορά", d, [o]() { g_app.conn->saveOffer(o); });
        g_showOffer = false; ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Κλείσιμο", ImVec2(120, 0))) { g_showOffer = false; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}

static void DrawOffersListTab() {
    if (ImGui::Button("+ Νέα προσφορά")) OpenOfferDialog(nullptr);
    ImGui::SameLine();
    static int show = 0;
    ImGui::RadioButton("Ενεργές & επόμενες", &show, 0); ImGui::SameLine(); ImGui::RadioButton("Όλες", &show, 1); ImGui::SameLine(); ImGui::RadioButton("Έληξαν", &show, 2);
    ImGui::TextDisabled("Οι προσφορές δεν αλλάζουν τιμές: όταν χρεώνετε, η εφαρμογή προτείνει όποια ταιριάζει και επιλέγετε «Εφαρμογή» ή την αγνοείτε.");
    ImGui::Separator();
    static Cached<std::vector<Offer>> cache;
    const auto& all = cache.get("", [] { return g_app.conn->getOffers(); });
    std::string today = CurrentDateString();
    enum { OC_NAME, OC_VAL, OC_WHO, OC_TARGET, OC_FROM, OC_TO, OC_STATE, OC_USES, OC_GIVEN, OC_ACT, OC_COUNT };
    static const ListColDef cols[OC_COUNT] = {
        { "Προσφορά", ImGuiTableColumnFlags_WidthFixed, 230.0f, true }, { "Έκπτωση", ImGuiTableColumnFlags_WidthFixed, 100.0f, true },
        { "Για", ImGuiTableColumnFlags_WidthFixed, 190.0f, true },      { "Σε", ImGuiTableColumnFlags_WidthFixed, 230.0f, true },
        { "Από", ImGuiTableColumnFlags_WidthFixed, 100.0f, true },      { "Έως", ImGuiTableColumnFlags_WidthFixed, 100.0f, true },
        { "Κατάσταση", ImGuiTableColumnFlags_WidthFixed, 240.0f, true },{ "Χρήσεις", ImGuiTableColumnFlags_WidthFixed, 90.0f, true },
        { "Έκπτωση που δόθηκε", ImGuiTableColumnFlags_WidthFixed, 160.0f, true }, { "", ImGuiTableColumnFlags_WidthFixed, 200.0f, false },
    };
    static GridState g;
    if (g.order.empty()) { for (int i = 0; i < OC_COUNT; i++) g.order.push_back(i); g.sort.keys = { { OC_TO, true } }; }
    std::vector<const Offer*> rows;
    for (auto& o : all) {
        bool live = o.status == "active" && o.valid_to >= today;
        if (show == 1 || (show == 0 && live) || (show == 2 && !live)) rows.push_back(&o);
    }
    GridSort(rows, g.sort, [](int c, const Offer& a, const Offer& b) {
        switch (c) {
            case OC_NAME: return CmpText(a.name, b.name);   case OC_VAL: return Cmp3(a.value, b.value);
            case OC_WHO: return Cmp3(a.user_id, b.user_id); case OC_TARGET: return CmpText(a.target_type, b.target_type);
            case OC_FROM: return Cmp3(a.valid_from, b.valid_from); case OC_TO: return Cmp3(a.valid_to, b.valid_to);
            case OC_STATE: return Cmp3(a.status, b.status);  case OC_USES: return Cmp3(a.uses, b.uses);
            case OC_GIVEN: return Cmp3(ToCents(a.given), ToCents(b.given)); default: return 0;
        }
    });
    if (rows.empty()) { ImGui::TextDisabled(show == 0 ? "Καμία ενεργή προσφορά. «+ Νέα προσφορά» για να δημιουργήσετε μία." : "Καμία προσφορά."); }
    g.pager.total = (int)rows.size();
    if (g.pager.offset >= g.pager.total) g.pager.Reset();
    static int cancelId = -1; static TextBuf cancelWhy;
    float h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.6f;
    if (!rows.empty() && ImGui::BeginTable("offers", OC_COUNT, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                           ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable, ImVec2(0, std::max(160.0f, h)))) {
        ImGui::TableSetupScrollFreeze(1, 1);
        ListSetupColumns(g.order, cols);
        ListHeaderRow(g.order, cols, g.sort, g.pager);
        auto P = [&](int c) { return ListPos(g.order, c); };
        for (int i = g.pager.offset; i < g.pager.ShowingTo(); i++) {
            const Offer& o = *rows[i];
            ImGui::PushID(o.id); ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(P(OC_NAME)); ImGui::TextUnformatted(o.name.c_str());
            if (!o.note.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", o.note.c_str());
            ImGui::TableSetColumnIndex(P(OC_VAL)); ImGui::TextColored(kColOwed, "%s", OfferValueText(o).c_str());
            ImGui::TableSetColumnIndex(P(OC_WHO)); ImGui::TextUnformatted(o.user_id > 0 ? g_app.userName(o.user_id).c_str() : "Όλοι οι πελάτες");
            ImGui::TableSetColumnIndex(P(OC_TARGET)); ImGui::TextUnformatted(OfferTargetText(o).c_str());
            ImGui::TableSetColumnIndex(P(OC_FROM)); ImGui::TextUnformatted(DateGr(o.valid_from).c_str());
            ImGui::TableSetColumnIndex(P(OC_TO)); ImGui::TextUnformatted(DateGr(o.valid_to).c_str());
            ImGui::TableSetColumnIndex(P(OC_STATE)); { ImVec4 c; std::string s = OfferStateText(o, c); ImGui::TextColored(c, "%s", s.c_str());
                if (o.status == "cancelled" && ImGui::IsItemHovered()) ImGui::SetTooltip("Αιτία: %s", o.cancel_reason.c_str()); }
            ImGui::TableSetColumnIndex(P(OC_USES)); ImGui::Text("%d", o.uses);
            ImGui::TableSetColumnIndex(P(OC_GIVEN)); ImGui::TextUnformatted(Eur(o.given).c_str());
            ImGui::TableSetColumnIndex(P(OC_ACT));
            bool editable = o.status == "active";
            ImGui::BeginDisabled(!editable);
            if (ImGui::SmallButton("Επεξεργασία")) OpenOfferDialog(&o);
            ImGui::SameLine();
            if (ImGui::SmallButton("Ακύρωση")) { cancelId = o.id; cancelWhy.set(""); ImGui::OpenPopup("Ακύρωση προσφοράς"); }
            ImGui::EndDisabled();
            if (ImGui::BeginPopupModal("Ακύρωση προσφοράς", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("Ακύρωση: %s", o.name.c_str());
                ImGui::TextDisabled("Ό,τι χρεώθηκε ήδη με την έκπτωση μένει όπως είναι. Η προσφορά δεν θα προτείνεται πλέον.");
                ImGui::SetNextItemWidth(380); ImGui::InputTextWithHint("##why", "Αιτία (υποχρεωτικό)", cancelWhy.data, sizeof(cancelWhy.data));
                bool ok = cancelWhy.str().find_first_not_of(" ") != std::string::npos;
                ImGui::BeginDisabled(!ok);
                if (ImGui::Button("Ακύρωση προσφοράς", ImVec2(180, 0))) { g_app.conn->cancelOffer(cancelId, cancelWhy.str()); ImGui::CloseCurrentPopup(); }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button("Πίσω", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
        DrawPager(g.pager, "offerspg");
    }
}

// The ranking of clients as payers (Προσφορές; and one client's own row in his profile). Three blocks of four columns:
//   Υπηρεσίες   = only the single services charged to the client (income = charged minus what it cost us)
//   Κοινόχρηστα = only his parts of joint charges (income = what he was charged)
//   Σύνολο      = both, computed again from the raw charges and payments (never an average of the two blocks)
// Per block: income, days (amount-weighted, fully paid charges), the speed index, what he owes now.
// Speed index 0-100: every payment counts with its OWN amount and delay (weight 2^(-days/30): paid at once = 1, after a month 1/2,
// after two months 1/4...); what is still unpaid counts as if paid 30 days from today. index = 100 * sum(amount*weight) / sum(amount).
static void DrawQuality(const char* id, QualityView& v, int onlyUser, float height) {
    enum { QC_NAME, S_INC, S_PSC, S_DAYS, S_IDX, S_WORTH, S_OWE, J_INC, J_DAYS, J_IDX, J_OWE, T_INC, T_DAYS, T_IDX, T_OWE, QC_ACT, QC_COUNT };
    // compact: short names (the group is the prefix: Πελ = as a client, Πολ = building, Σύν = all together), widths scaled to the page
    static ListColDef cols[QC_COUNT] = {
        { "Πελάτης", ImGuiTableColumnFlags_WidthFixed, 190.0f, true },
        { "Πελ·Κέρδος", ImGuiTableColumnFlags_WidthFixed, 100.0f, true },   { "Πελ·Βαθμός κέρδους", ImGuiTableColumnFlags_WidthFixed, 120.0f, true },
        { "Πελ·Μέρες", ImGuiTableColumnFlags_WidthFixed, 100.0f, true },
        { "Πελ·Βαθμός πληρωμής", ImGuiTableColumnFlags_WidthFixed, 130.0f, true },
        { "Πελ·Αξίζει", ImGuiTableColumnFlags_WidthFixed, 100.0f, true },   { "Πελ·Οφείλει", ImGuiTableColumnFlags_WidthFixed, 100.0f, true },
        { "Πολ·Χρέωση", ImGuiTableColumnFlags_WidthFixed, 100.0f, true },   { "Πολ·Μέρες", ImGuiTableColumnFlags_WidthFixed, 100.0f, true },
        { "Πολ·Βαθμός", ImGuiTableColumnFlags_WidthFixed, 130.0f, true },   { "Πολ·Οφείλει", ImGuiTableColumnFlags_WidthFixed, 100.0f, true },
        { "Σύν·Έσοδα", ImGuiTableColumnFlags_WidthFixed, 100.0f, true },    { "Σύν·Μέρες", ImGuiTableColumnFlags_WidthFixed, 100.0f, true },
        { "Σύν·Βαθμός", ImGuiTableColumnFlags_WidthFixed, 130.0f, true },   { "Σύν·Οφείλει", ImGuiTableColumnFlags_WidthFixed, 100.0f, true },
        { "", ImGuiTableColumnFlags_WidthFixed, 70.0f, false } };
    {   // scale the widths of the visible columns to the room there is
        float want = 0; bool blk[QC_COUNT];
        for (int c = 0; c < QC_COUNT; c++) { blk[c] = c < S_INC || c == QC_ACT || (c <= S_OWE ? v.blkS : (c <= J_OWE ? v.blkJ : v.blkT)); if (blk[c]) want += cols[c].w; }
        static float base[QC_COUNT]; static bool init = false;
        if (!init) { for (int c = 0; c < QC_COUNT; c++) base[c] = cols[c].w; init = true; }
        float sc = std::max(0.4f, (ImGui::GetContentRegionAvail().x - 24.0f) / std::max(1.0f, want));
        for (int c = 0; c < QC_COUNT; c++) cols[c].w = base[c] * sc;
    }    GridState& g = v.g;
    if (g.order.empty()) { for (int i = 0; i < QC_COUNT; i++) g.order.push_back(i); g.sort.keys = { { S_WORTH, false } }; g.pager.pageSize = 20; }
    ImGui::PushID(id);
    if (onlyUser == 0) {
        ImGui::SetNextItemWidth(260);
        if (ImGui::InputTextWithHint("##qq", "Αναζήτηση πελάτη...", v.q, sizeof(v.q))) g.pager.Reset();
        ImGui::SameLine();
    }
    // by default only the three scores of the clients; everything else on request
    ImGui::Checkbox("Περισσότερες στήλες", &v.more);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Κέρδος σε €, μέρες πληρωμής, οφειλές, και τα ίδια για τα κοινόχρηστα");
    if (v.more) {
        ImGui::SameLine(); ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("  Ομάδες:"); ImGui::SameLine();
        ImGui::Checkbox("Μεμονωμένες υπηρεσίες", &v.blkS); ImGui::SameLine();
        ImGui::Checkbox("Κοινόχρηστα", &v.blkJ);
    } else { v.blkS = true; v.blkJ = false; }
    v.blkT = v.blkS && v.blkJ;       // both ticked = the combined block too
    if (DrawDateRange("qdates", v.from, v.to)) g.pager.Reset();
    if (!v.blkS && !v.blkJ) { ImGui::TextDisabled("Επιλέξτε τουλάχιστον μία ομάδα στηλών για να δείτε τη βαθμολογία."); ImGui::PopID(); return; }

    struct Blk { double inc = 0, days = 0, owe = 0, idx = -1, psc = -1, worth = -1; bool any = false, hasDays = false; };
    struct Row { int uid; std::string name; Blk s, j, t; };
    static std::map<std::string, Cached<std::vector<DataConn::QualityRow>>> caches;
    const auto& all = caches[id].get(std::to_string(onlyUser) + "|" + v.from + "|" + v.to, [&] { return g_app.conn->clientQuality(onlyUser, v.from, v.to); });
    // what an average client brings us (own services: charged - our cost), over EVERY client, so one client's score
    // is relative to all of them even inside his own profile
    static std::map<std::string, Cached<double>> avgCaches;
    const double avgProfit = avgCaches[id].get(v.from + "|" + v.to, [&] {
        auto every = onlyUser ? g_app.conn->clientQuality(0, v.from, v.to) : all;
        long long sum = 0; int n = 0;
        for (auto& r : every) if (r.chargesClient > 0) { sum += std::max(0LL, ToCents(r.income) - ToCents(r.cost)); n++; }
        return n ? FromCents(sum) / n : 0.0; });
    std::vector<Row> built;
    std::string fq = FoldGreek(v.q);
    for (auto& r : all) {
        if (!fq.empty() && FoldGreek(r.name).find(fq) == std::string::npos) continue;
        Row w; w.uid = r.user_id; w.name = r.name;
        w.s.any = r.chargesClient > 0; w.s.inc = MoneySub(r.income, r.cost); w.s.owe = r.openClient; w.s.idx = DataConn::QualityRow::Index(r.idxNumClient, r.idxDenClient);
        if (r.paidAmtClient > 0.005) { w.s.hasDays = true; w.s.days = r.daysClient / r.paidAmtClient; }
        if (w.s.any) {
            // profit score 0-100: 100 * (1 - 2^(-profit / average profit)). No profit = 0, an average client = 50,
            // twice the average = 75, three times = 87.5 ... (the same halving curve as the payment score)
            w.s.psc = (w.s.inc <= 0 || avgProfit <= 0) ? 0.0 : 100.0 * (1.0 - std::pow(2.0, -w.s.inc / avgProfit));
            // worth it: both together (geometric mean), so a big client who never pays, or a fast payer who brings
            // nothing, both score low
            w.s.worth = w.s.idx < 0 ? w.s.psc : std::sqrt(w.s.psc * w.s.idx);
        }
        w.j.any = r.chargesJoint > 0; w.j.inc = r.incomeJoint; w.j.owe = r.openJoint; w.j.idx = DataConn::QualityRow::Index(r.idxNumJoint, r.idxDenJoint);
        if (r.paidAmtJoint > 0.005) { w.j.hasDays = true; w.j.days = r.daysJoint / r.paidAmtJoint; }
        w.t.any = w.s.any || w.j.any; w.t.inc = FromCents(ToCents(w.s.inc) + ToCents(w.j.inc)); w.t.owe = FromCents(ToCents(w.s.owe) + ToCents(w.j.owe));
        w.t.idx = DataConn::QualityRow::Index(r.idxNumClient + r.idxNumJoint, r.idxDenClient + r.idxDenJoint);
        double pa = r.paidAmtClient + r.paidAmtJoint;
        if (pa > 0.005) { w.t.hasDays = true; w.t.days = (r.daysClient + r.daysJoint) / pa; }
        if (w.t.any) built.push_back(w);
    }
    std::vector<const Row*> rows;
    for (auto& w : built) rows.push_back(&w);
    auto incOf = [](const Blk& b) { return b.any ? ToCents(b.inc) : (long long)INT64_MIN / 2; };
    auto daysCmp = [](const Blk& a, const Blk& b) { return a.hasDays != b.hasDays ? (a.hasDays ? -1 : 1) : Cmp3(a.days, b.days); };
    auto idxCmp = [](const Blk& a, const Blk& b) { return Cmp3(a.idx, b.idx); };           // "nothing to rate" (-1) sorts lowest
    GridSort(rows, g.sort, [&](int c, const Row& a, const Row& b) {
        switch (c) {
            case QC_NAME: return CmpText(a.name, b.name);
            case S_PSC: return Cmp3(a.s.psc, b.s.psc); case S_WORTH: return Cmp3(a.s.worth, b.s.worth);
            case S_INC: return Cmp3(incOf(a.s), incOf(b.s)); case S_DAYS: return daysCmp(a.s, b.s); case S_IDX: return idxCmp(a.s, b.s); case S_OWE: return Cmp3(ToCents(a.s.owe), ToCents(b.s.owe));
            case J_INC: return Cmp3(incOf(a.j), incOf(b.j)); case J_DAYS: return daysCmp(a.j, b.j); case J_IDX: return idxCmp(a.j, b.j); case J_OWE: return Cmp3(ToCents(a.j.owe), ToCents(b.j.owe));
            case T_INC: return Cmp3(incOf(a.t), incOf(b.t)); case T_DAYS: return daysCmp(a.t, b.t); case T_IDX: return idxCmp(a.t, b.t); case T_OWE: return Cmp3(ToCents(a.t.owe), ToCents(b.t.owe));
            default: return 0;
        }
    });
    g.pager.total = (int)rows.size(); if (g.pager.offset >= g.pager.total) g.pager.Reset();
    if (onlyUser == 0) { ImGui::SameLine(); ImGui::TextDisabled("%d πελάτες", g.pager.total); }
    if (rows.empty()) { ImGui::TextDisabled(onlyUser ? "Δεν υπάρχουν χρεώσεις σε αυτό το διάστημα." : "Δεν υπάρχουν δεδομένα ακόμα."); ImGui::PopID(); return; }
    float h = height > 0 ? height : std::max(160.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.6f);
    ImGui::SetWindowFontScale(0.86f);     // small type so all columns fit the page width without sideways scrolling
    if (ImGui::BeginTable("payerq", QC_COUNT, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | (onlyUser ? 0 : ImGuiTableFlags_ScrollY) | ImGuiTableFlags_Hideable |
                                              ImGuiTableFlags_Resizable, onlyUser ? ImVec2(0, 0) : ImVec2(0, h))) {
        ImGui::TableSetupScrollFreeze(onlyUser ? 0 : 1, 1);
        g_listCompact = true;
        for (int l : g.order)       // stretch columns: they always share exactly the page width
            ImGui::TableSetupColumn(cols[l].name, ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoReorder | ImGuiTableColumnFlags_NoSort, l == QC_NAME ? 1.6f : (l == QC_ACT ? 0.8f : (l == S_IDX || l == J_IDX || l == T_IDX || l == S_PSC ? 1.25f : 1.0f)));
        if (onlyUser) ImGui::TableSetColumnEnabled(ListPos(g.order, QC_NAME), false);      // one client: his name is the window
        ImGui::TableSetColumnEnabled(ListPos(g.order, QC_ACT), false);                      // no buttons in the ranking
        for (int c = S_INC; c <= S_OWE; c++) ImGui::TableSetColumnEnabled(ListPos(g.order, c), v.blkS && (v.more || c == S_PSC || c == S_IDX || c == S_WORTH));
        for (int c = J_INC; c <= J_OWE; c++) ImGui::TableSetColumnEnabled(ListPos(g.order, c), v.blkJ);
        for (int c = T_INC; c <= T_OWE; c++) ImGui::TableSetColumnEnabled(ListPos(g.order, c), v.blkT);
        ListHeaderRow(g.order, cols, g.sort, g.pager);
        auto P = [&](int c) { return ListPos(g.order, c); };
        auto inc = [&](int col, const Blk& b) { ImGui::TableSetColumnIndex(P(col)); if (b.any) ImGui::TextColored(b.inc >= 0 ? kColIn : kColOut, "%s", Eur(b.inc).c_str()); else ImGui::TextDisabled("—"); };
        auto days = [&](int col, const Blk& b) {
            ImGui::TableSetColumnIndex(P(col));
            if (!b.any || !b.hasDays) ImGui::TextDisabled("—");
            else if (b.days < 1.0) ImGui::TextColored(kColIn, "0");
            else ImGui::TextColored(b.days <= 7 ? kColIn : (b.days <= 30 ? kColOwed : kColOut), "%.0f", b.days);
        };
        auto idx = [&](int col, const Blk& b) {
            ImGui::TableSetColumnIndex(P(col));
            if (!b.any || b.idx < 0) { ImGui::TextDisabled("—"); return; }
            ImGui::TextColored(b.idx >= 75 ? kColIn : (b.idx >= 35 ? kColOwed : kColOut), "%.0f", b.idx);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Βαθμός πληρωμής (0-100) = 100 x Σ(ποσό x 2^(-μέρες καθυστέρησης/30)) / Σ(ποσό)\n"
                                                          "Κάθε πληρωμή: αμέσως = 1 · μετά από 30 μέρες = 1/2 · μετά από 60 = 1/4.\n"
                                                          "Ό,τι είναι απλήρωτο μετράει σαν να πληρωθεί σε 30 μέρες από σήμερα.\n"
                                                          "Παράδειγμα: χρέωση 100 €, 50 € αμέσως + 50 € μετά από 30 μέρες\n"
                                                          "  = 100 x (50x1 + 50x0,5) / 100 = 75.");
        };
        auto score = [&](int col, const Blk& b, double val, const char* tip) {
            ImGui::TableSetColumnIndex(P(col));
            if (!b.any || val < 0) { ImGui::TextDisabled("—"); return; }
            ImGui::TextColored(val >= 75 ? kColIn : (val >= 35 ? kColOwed : kColOut), "%.0f", val);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        };
        auto owe = [&](int col, const Blk& b) { ImGui::TableSetColumnIndex(P(col)); if (b.owe > 0.005) ImGui::TextColored(kColOwed, "%s", Eur(b.owe).c_str()); else ImGui::TextDisabled("—"); };
        int from = onlyUser ? 0 : g.pager.offset, to = onlyUser ? (int)rows.size() : g.pager.ShowingTo();
        for (int i = from; i < to; i++) {
            const Row& r = *rows[i];
            ImGui::PushID(r.uid); ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(P(QC_NAME)); ImGui::TextUnformatted(r.name.c_str());
            inc(S_INC, r.s);
            score(S_PSC, r.s, r.s.psc, ("Βαθμός κέρδους (0-100) = 100 x (1 - 2^(-κέρδος / μέσο κέρδος πελάτη))\n"
                                        "κέρδος = ό,τι του χρεώσαμε - το κόστος μας. Μέσο κέρδος πελάτη τώρα: " + Eur(avgProfit) + "\n"
                                        "Χωρίς κέρδος = 0 · όσο ο μέσος πελάτης = 50 · διπλάσιο = 75 · τριπλάσιο = 87,5\n"
                                        "Ο πελάτης αυτός: κέρδος " + Eur(r.s.inc) + " -> " + std::to_string((int)std::lround(r.s.psc))).c_str());
            days(S_DAYS, r.s); idx(S_IDX, r.s);
            score(S_WORTH, r.s, r.s.worth, ("Αξίζει (0-100) = ρίζα(βαθμός κέρδους x βαθμός πληρωμής)  (γεωμετρικός μέσος)\n"
                                            "Παράδειγμα: 80 και 100 -> ρίζα(8000) = 89 (όχι 90 όπως ο απλός μέσος όρος).\n"
                                            "Ο γεωμετρικός μέσος τιμωρεί το αδύνατο σημείο: 100 και 0 -> 0, όχι 50.\n"
                                            "Άρα υψηλό μόνο αν φέρνει κέρδος ΚΑΙ πληρώνει γρήγορα.\n"
                                            "Ο πελάτης αυτός: ρίζα(" + std::to_string((int)std::lround(r.s.psc)) + " x " +
                                            (r.s.idx < 0 ? std::string("-") : std::to_string((int)std::lround(r.s.idx))) + ") = " +
                                            std::to_string((int)std::lround(r.s.worth))).c_str());
            owe(S_OWE, r.s);
            inc(J_INC, r.j); days(J_DAYS, r.j); idx(J_IDX, r.j); owe(J_OWE, r.j);
            inc(T_INC, r.t); days(T_DAYS, r.t); idx(T_IDX, r.t); owe(T_OWE, r.t);
            ImGui::TableSetColumnIndex(P(QC_ACT)); if (!onlyUser && ImGui::SmallButton("Καρτέλα")) OpenResidentLedger(r.uid);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::SetWindowFontScale(1.0f);
    g_listCompact = false;
    if (onlyUser == 0) DrawPager(g.pager, "payerqpg");
    ImGui::PopID();
}

static void DrawOffersListTab();
static void DrawOffersScreen() {
    // the offers on top, the clients rated as payers right under them (one page, both always visible)
    float avail = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("offerslist", ImVec2(0, std::max(220.0f, avail * 0.42f)), false);
    DrawOffersListTab();
    ImGui::EndChild();
    { ImVec2 e = SectionTitle("Αξιολόγηση πελατών (ποιοι πληρώνουν καλά)"); ImVec2 keep = ImGui::GetCursorScreenPos(); ImGui::SetCursorScreenPos(e);
    if (ImGui::SmallButton("i##rankinfo")) ImGui::OpenPopup("rankinfo");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Πώς υπολογίζεται η βαθμολογία");
    ImGui::SetCursorScreenPos(keep); }
    ImGui::SetNextWindowSize(ImVec2(640, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopup("rankinfo")) {
        ImGui::PushTextWrapPos(610);
        const ImVec4 hd(0.35f, 0.80f, 1.0f, 1.0f);
        ImGui::TextColored(hd, "Τι δείχνει ο βαθμός");
        ImGui::TextWrapped("Ένας αριθμός 0-100 για το πόσο γρήγορα (και πόσο πλήρως) πληρώνει ένας πελάτης. 100 = πληρώνει πάντα αμέσως. Όσο πιο χαμηλά, τόσο πιο αργά ή πιο λίγο πληρώνει. Χρησιμεύει για να συγκρίνετε πελάτες μεταξύ τους και να δείτε ποιοι θέλουν παρακολούθηση.");
        ImGui::Spacing(); ImGui::TextColored(hd, "Πώς υπολογίζεται");
        ImGui::TextWrapped("Κάθε χρέωση και κάθε πληρωμή της μετράει ξεχωριστά. Για κάθε πληρωμή βρίσκουμε πόσες μέρες πέρασαν από την ημερομηνία της χρέωσης και της δίνουμε ένα βάρος:");
        ImGui::TextUnformatted("      βάρος = 2 ^ ( - μέρες / 30 )");
        ImGui::TextUnformatted("      βαθμός = 100 × Σ ( ποσό × βάρος ) / Σ ( ποσό )");
        ImGui::TextWrapped("Δηλαδή ο βαθμός είναι ο μέσος όρος των βαρών, με βάση το ποσό: ένα μεγάλο ποσό που πληρώθηκε αργά ρίχνει τον βαθμό περισσότερο από ένα μικρό.");
        ImGui::Spacing(); ImGui::TextColored(hd, "Τα βάρη");
        ImGui::TextUnformatted("      αυθημερόν = 1,00   ·   σε 15 μέρες = 0,71   ·   σε 30 = 0,50");
        ImGui::TextUnformatted("      σε 60 = 0,25   ·   σε 90 = 0,125   ·   σε 180 = 0,016");
        ImGui::Spacing(); ImGui::TextColored(hd, "Ό,τι δεν έχει πληρωθεί ακόμα");
        ImGui::TextWrapped("Το απλήρωτο ποσό μετράει σαν να πληρωνόταν 30 μέρες από σήμερα. Άρα μια παλιά απλήρωτη χρέωση (π.χ. 100 μέρες) έχει ήδη πολύ μικρό βάρος και ρίχνει τον βαθμό. Μια χρέωση του χθες δεν τον πειράζει σχεδόν καθόλου.");
        ImGui::Spacing(); ImGui::TextColored(hd, "Παραδείγματα");
        ImGui::TextWrapped("• Πληρώνει 100 € αυθημερόν: βαθμός 100.");
        ImGui::TextWrapped("• Πληρώνει 100 € σε 30 μέρες: βαθμός 50.");
        ImGui::TextWrapped("• 100 € αυθημερόν + 100 € σε 60 μέρες: (100×1 + 100×0,25) / 200 = 62,5.");
        ImGui::TextWrapped("• 100 € αυθημερόν και 300 € ακόμα απλήρωτα εδώ και 90 μέρες: (100×1 + 300×0,0625) / 400 = περίπου 29.");
        ImGui::Spacing(); ImGui::TextColored(hd, "Γιατί έτσι και από πού προέρχεται");
        ImGui::TextWrapped("Δεν είναι πρότυπο πιστοληπτικής αξιολόγησης ούτε κάτι που βρήκαμε έτοιμο: είναι ο δικός μας απλός κανόνας για αυτή την εφαρμογή. Βασίζεται σε δύο γνωστές ιδέες: (1) τον «μέσο χρόνο είσπραξης» που χρησιμοποιεί η λογιστική των απαιτήσεων, και (2) τη σταδιακή εκθετική μείωση με «χρόνο ημιζωής», όπως στη βαθμολόγηση πρόσφατης συμπεριφοράς.");
        ImGui::TextWrapped("• Γιατί 30 μέρες: οι χρεώσεις είναι κυρίως μηνιαίες, άρα ένας μήνας καθυστέρηση «κοστίζει» το μισό βάρος.");
        ImGui::TextWrapped("• Γιατί με βάση το ποσό: ένα μεγάλο χρέος είναι πιο σημαντικό από ένα μικρό.");
        ImGui::TextWrapped("• Γιατί εκθετικά και όχι γραμμικά: οι πρώτες μέρες καθυστέρησης δεν τιμωρούνται σκληρά, αλλά οι πολύ μεγάλες καθυστερήσεις πέφτουν γρήγορα προς το 0 χωρίς όριο.");
        ImGui::TextWrapped("• Οι αριθμοί (30 μέρες, ο ρόλος του απλήρωτου) είναι επιλογή μας και αλλάζουν εύκολα στον κώδικα αν θέλετε πιο αυστηρή ή πιο επιεική βαθμολογία. Να τον διαβάζετε ως σχετική σειρά κατάταξης, όχι ως επίσημη αξιολόγηση.");
        ImGui::Spacing(); ImGui::TextColored(hd, "Οι στήλες");
        ImGui::TextWrapped("Πελ = ως πελάτης (μεμονωμένες υπηρεσίες) · Πολ = ως ένοικος (κοινόχρηστα) · Σύν = και τα δύο (υπολογισμένο ξανά από όλες τις χρεώσεις, όχι μέσος όρος των δύο βαθμών).");
        ImGui::TextWrapped("«Μέρες» = οι μέσες μέρες που περνούν μέχρι να εξοφληθεί πλήρως μια χρέωση (με βάση το ποσό, μόνο για όσες εξοφλήθηκαν). «Κέρδος» = όσα χρεώσαμε μείον το κόστος μας.");
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }    static QualityView offerQuality;
    DrawQuality("offerq", offerQuality, 0, 0);
}

// The «Διαθέσιμες προσφορές» box used by the charge dialogs. Returns true while an offer is applied (offerId set).
// listTotal = the price before any discount. onApply(discount) is called when the person clicks «Εφαρμογή».
static bool DrawOfferClaim(const char* id, int userId, int serviceId, int packageId, const std::string& date, double listTotal,
                           int& offerId, const std::function<void(double)>& onApply, const std::function<void()>& onRemove) {
    if (userId <= 0 || (serviceId <= 0 && packageId <= 0) || ToCents(listTotal) <= 0) { offerId = -1; return false; }
    static Cached<std::vector<Offer>> cache;
    std::string key = std::string(id) + "|" + std::to_string(userId) + "|" + std::to_string(serviceId) + "|" + std::to_string(packageId) + "|" + date;
    const auto& offers = cache.get(key, [&] { return g_app.conn->findOffers(userId, serviceId, packageId, date); });
    if (offers.empty()) { offerId = -1; return false; }
    ImGui::PushID(id);
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.25f, 0.20f, 0.08f, 0.45f));
    ImGui::BeginChild("offerbox", ImVec2(0, 0), ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY);
    ImGui::TextColored(kColOwed, "%% Διαθέσιμες προσφορές (%d)", (int)offers.size());
    bool applied = false;
    for (auto& o : offers) {
        ImGui::PushID(o.id);
        double d = OfferDiscount(o, listTotal);
        ImVec4 c; std::string st = OfferStateText(o, c);
        bool on = offerId == o.id;
        if (on) {
            applied = true;
            if (ImGui::SmallButton("Αφαίρεση")) { offerId = -1; onRemove(); }
            ImGui::SameLine(); ImGui::TextColored(kColIn, "εφαρμόστηκε ·");
        } else if (ImGui::SmallButton("Εφαρμογή")) { offerId = o.id; onApply(d); applied = true; }
        ImGui::SameLine();
        ImGui::Text("%s  %s  (%s)", o.name.c_str(), OfferValueText(o).c_str(), Eur(-d).c_str());
        ImGui::TextColored(c, "    %s%s", st.c_str(), o.user_id > 0 ? " · προσωπική προσφορά" : "");
        ImGui::PopID();
    }
    if (!applied) { ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 480); ImGui::TextDisabled("Χωρίς «Εφαρμογή» η χρέωση γίνεται στην κανονική τιμή."); ImGui::PopTextWrapPos(); }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopID();
    return applied;
}

// =====================================================================================================
// ΣΥΝΕΡΓΑΤΕΣ -- the people who do jobs for us (plumber, electrician, cleaner...). The list shows each partner with
// what we paid / owe them; under it the most recent jobs; a click opens the partner's profile with every cost line
// paid to them (open it to get back to the charge it belongs to; hover to see its payments).
// =====================================================================================================
static int g_partnerProfileId = 0;          // 0 = none open, -1 = the #-1 (no partner) line
static bool g_partnerEditOpen = false;
static Partner g_partnerEdit;
static TextBuf pe_name, pe_trade, pe_phone, pe_email, pe_afm, pe_notes;
static int g_partnerRemoveId = -1; static std::string g_partnerRemoveName;

static void OpenPartnerEdit(const Partner* p) {
    g_partnerEdit = p ? *p : Partner{};
    pe_name.set(g_partnerEdit.name); pe_trade.set(g_partnerEdit.trade); pe_phone.set(g_partnerEdit.phone);
    pe_email.set(g_partnerEdit.email); pe_afm.set(g_partnerEdit.afm); pe_notes.set(g_partnerEdit.notes);
    g_partnerEditOpen = true;
}

static void DrawPartnerDialogs() {
    if (g_partnerEditOpen) { ImGui::OpenPopup("Συνεργάτης"); g_partnerEditOpen = false; }
    if (ImGui::BeginPopupModal("Συνεργάτης", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        auto field = [&](const char* label, const char* id, TextBuf& b, float w = 360.0f) {
            ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(label); ImGui::SameLine(110);
            ImGui::SetNextItemWidth(w); ImGui::InputText(id, b.data, sizeof(b.data));
        };
        field("Όνομα:", "##pn", pe_name); field("Ειδικότητα:", "##pt", pe_trade); field("Τηλέφωνο:", "##pp", pe_phone);
        field("Email:", "##pe", pe_email); field("ΑΦΜ:", "##pa", pe_afm, 160.0f);
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Σημειώσεις:"); ImGui::SameLine(110);
        ImGui::InputTextMultiline("##pno", pe_notes.data, sizeof(pe_notes.data), ImVec2(360, 70));
        bool ok = pe_name.str().find_first_not_of(" \t") != std::string::npos;
        ImGui::BeginDisabled(!ok);
        if (ImGui::Button("Αποθήκευση", ImVec2(150, 0))) {
            Partner p = g_partnerEdit; p.name = pe_name.str(); p.trade = pe_trade.str(); p.phone = pe_phone.str();
            p.email = pe_email.str(); p.afm = pe_afm.str(); p.notes = pe_notes.str();
            if (g_app.conn->savePartner(p) > 0) { g_app.reloadGen++; ImGui::CloseCurrentPopup(); }
        }
        ImGui::EndDisabled();
        ImGui::SameLine(); if (ImGui::Button("Άκυρο", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (g_partnerRemoveId > 0) { ImGui::OpenPopup("Διαγραφή συνεργάτη"); }
    if (ImGui::BeginPopupModal("Διαγραφή συνεργάτη", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Διαγραφή του συνεργάτη «%s»;", g_partnerRemoveName.c_str());
        ImGui::TextDisabled("Ο συνεργάτης κρύβεται από τις λίστες· οι δουλειές του μένουν στο ιστορικό και βρίσκεται στο Αρχείο διαγραφών (Ρυθμίσεις).");
        if (ImGui::Button("Διαγραφή", ImVec2(150, 0))) {
            g_app.conn->removePartner(g_partnerRemoveId); g_app.reloadGen++; g_partnerRemoveId = -1; ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine(); if (ImGui::Button("Πίσω", ImVec2(120, 0))) { g_partnerRemoveId = -1; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

static std::string PartnerJobStatus(const DataConn::PartnerJob& j, ImVec4& col) {
    if (j.balance > 0.005) { col = kColWeOwe; return "Οφειλούμενο " + Eur(j.balance); }
    col = kColOut; return "Πληρώθηκε";
}

// Paying (all or part of) what we owe on a cost line to a partner.
static int g_poCharge = -1; static double g_poBalance = 0.0; static std::string g_poTitle, g_poDate; static TextBuf g_poAmount; static bool g_poOpenNext = false; static std::string g_poError;
static void OpenPayOut(const DataConn::PartnerJob& j) {
    g_poCharge = j.charge_id; g_poBalance = j.balance; g_poTitle = j.partner + " · " + j.service; g_poDate = CurrentDateString();
    g_poAmount.set(FormatMoney(j.balance)); g_poOpenNext = true; g_poError.clear();
}
static void DrawPayOutDialog() {
    if (g_poOpenNext) { ImGui::OpenPopup("Πληρωμή προς συνεργάτη"); g_poOpenNext = false; }
    if (g_poCharge < 0) return;
    ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Πληρωμή προς συνεργάτη", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(g_poTitle.c_str());
        ImGui::TextColored(kColWeOwe, "Οφειλούμενο: %s", Eur(g_poBalance).c_str());
        ImGui::Spacing();
        ImGui::SetNextItemWidth(160); ImGui::InputText("Ποσό που πληρώνουμε (€)", g_poAmount.data, sizeof(g_poAmount.data));
        double amt = 0.0; bool ok = MoneyFieldHint(g_poAmount) && ParseMoney(g_poAmount.str(), amt) && ToCents(amt) > 0 && ToCents(amt) <= ToCents(g_poBalance);
        if (!ok) ImGui::TextColored(kColOut, "Ποσό από 0,01 έως το οφειλούμενο.");
        else if (ToCents(amt) < ToCents(g_poBalance)) ImGui::TextColored(kColWeOwe, "Θα μείνει οφειλούμενο: %s", Eur(MoneySub(g_poBalance, amt)).c_str());
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Ημερομηνία πληρωμής:"); ImGui::SameLine(); DatePicker("podate", g_poDate, ImVec2(0, 1));
        if (!g_poError.empty()) ImGui::TextColored(kColOut, "%s", g_poError.c_str());
        ImGui::Spacing();
        ImGui::BeginDisabled(!ok);
        if (ImGui::Button("Πληρωμή", ImVec2(150, 0))) {
            if (g_app.conn->payOutCharge(g_poCharge, amt, g_poDate)) { g_app.reloadAll(); g_poCharge = -1; ImGui::CloseCurrentPopup(); }
            else g_poError = g_app.conn->takeLastError();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Ακύρωση", ImVec2(150, 0))) { g_poCharge = -1; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}
// The cost lines paid to one partner, with the payments of a line shown while hovering it.
enum { PJ_DATE, PJ_PARTNER, PJ_SERVICE, PJ_FOR, PJ_AMOUNT, PJ_PAID, PJ_STATUS, PJ_KIND, PJ_ACT, PJ_COUNT };
static const ListColDef kPartnerJobCols[PJ_COUNT] = {
    { "Ημ/νία", ImGuiTableColumnFlags_WidthFixed, 110.0f, true },         { "Συνεργάτης", ImGuiTableColumnFlags_WidthFixed, 190.0f, true },
    { "Υπηρεσία", ImGuiTableColumnFlags_WidthFixed, 240.0f, true },        { "Για", ImGuiTableColumnFlags_WidthFixed, 260.0f, true },
    { "Ποσό", ImGuiTableColumnFlags_WidthFixed, 100.0f, true },            { "Πληρωμένο", ImGuiTableColumnFlags_WidthFixed, 110.0f, true },
    { "Κατάσταση", ImGuiTableColumnFlags_WidthFixed, 150.0f, true },       { "Είδος", ImGuiTableColumnFlags_WidthFixed, 110.0f, true },
    { "", ImGuiTableColumnFlags_WidthFixed, 170.0f, false } };
static SortSpec PartnerJobSort(const SortSpec& s) {           // the SQL's own column numbers (0 date .. 7 kind)
    return MapSort(s, {{PJ_DATE, 0}, {PJ_PARTNER, 1}, {PJ_SERVICE, 2}, {PJ_FOR, 3}, {PJ_AMOUNT, 4}, {PJ_PAID, 5}, {PJ_STATUS, 6}, {PJ_KIND, 7}});
}

// The cost lines paid to partners, with the payments of a line shown while hovering it. showPartner: the recent-actions
// table (all partners) shows who was paid; the profile of one partner hides that column and shows how much was paid.
static void DrawPartnerJobsTable(const char* id, const std::vector<DataConn::PartnerJob>& jobs, bool showPartner, float height, GridState& g) {
    if (g.order.empty()) { for (int i = 0; i < PJ_COUNT; i++) g.order.push_back(i); }
    if (!ImGui::BeginTable(id, PJ_COUNT, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                                         ImGuiTableFlags_Resizable | ImGuiTableFlags_Hideable, ImVec2(0, height))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ListSetupColumns(g.order, kPartnerJobCols);
    ImGui::TableSetColumnEnabled(ListPos(g.order, showPartner ? PJ_PAID : PJ_PARTNER), false);
    ListHeaderRow(g.order, kPartnerJobCols, g.sort, g.pager);
    auto P = [&](int c) { return ListPos(g.order, c); };
    for (auto& j : jobs) {
        ImGui::PushID(j.charge_id); ImGui::TableNextRow();
        auto tip = [&] {      // hovering a cell of the row: the payments made for this cost line
            if (!ImGui::IsItemHovered()) return;
            auto money = g_app.conn->getMoneyForCharge(j.charge_id);
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(j.service.c_str());
            if (!j.notes.empty()) ImGui::TextDisabled("%s", j.notes.c_str());
            ImGui::Separator();
            if (money.empty()) ImGui::TextDisabled("Καμία πληρωμή ακόμα.");
            for (auto& m : money) ImGui::Text("%s · %s · %s", DateGr(m.pay_date).c_str(), ReceiptMethodName(m.method).c_str(), Eur(m.allocated > 0 ? m.allocated : m.amount).c_str());
            ImGui::EndTooltip();
        };
        std::string who = j.jointOpId > 0 ? "Κοινόχρηστο: " + j.jointLabel : (j.client.empty() ? std::string("—") : j.client);
        ImGui::TableSetColumnIndex(P(PJ_DATE)); ImGui::TextUnformatted(DateGr(j.date).c_str()); tip();
        if (showPartner) { ImGui::TableSetColumnIndex(P(PJ_PARTNER)); ImGui::TextUnformatted(j.partner.c_str()); tip(); }
        ImGui::TableSetColumnIndex(P(PJ_SERVICE)); ImGui::TextUnformatted(j.service.c_str()); tip();
        ImGui::TableSetColumnIndex(P(PJ_FOR)); ImGui::TextUnformatted(who.c_str()); tip();
        ImGui::TableSetColumnIndex(P(PJ_AMOUNT)); ImGui::TextUnformatted(Eur(j.amount).c_str());
        if (!showPartner) { ImGui::TableSetColumnIndex(P(PJ_PAID)); ImGui::TextColored(kColOut, "%s", Eur(j.paid).c_str()); }
        ImGui::TableSetColumnIndex(P(PJ_STATUS)); { ImVec4 c; std::string s = PartnerJobStatus(j, c); ImGui::TextColored(c, "%s", s.c_str()); }
        ImGui::TableSetColumnIndex(P(PJ_KIND));
        if (j.kind == "joint") ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.30f, 1), "Κοινόχρηστο");
        else if (j.kind == "package") ImGui::TextColored(ImVec4(0.55f, 0.80f, 0.95f, 1), "Πακέτο");
        else ImGui::TextUnformatted("Κανονική");
        ImGui::TableSetColumnIndex(P(PJ_ACT));
        // the window of that specific charge (joint, normal or package) with its owings and the payment resolution
        if (ImGui::SmallButton("Άνοιγμα")) { if (j.jointOpId > 0) OpenJointView(j.jointOpId); else OpenPaymentView(j.charge_id); }
        if (j.balance > 0.005) { ImGui::SameLine(); if (ImGui::SmallButton("Πληρωμή")) OpenPayOut(j); }      // we owe it: pay (all or part) now
        ImGui::PopID();
    }
    ImGui::EndTable();
}

// One distinct list of a partner's jobs (kind 1 = for the clients' own services and packages, 2 = for joint charges), as a model:
// every sort key and page change fetches that page again. partnerId 0 = every partner.
static void DrawPartnerJobsModel(const char* id, int partnerId, int kind, const std::string& from, const std::string& to, ListModel<DataConn::PartnerJob>& m, bool showPartner, float height) {
    m.Init(PJ_COUNT, {{PJ_DATE, false}}, 20);
    const auto& pg = m.Load(std::to_string(partnerId) + "|" + std::to_string(kind) + "|" + from + "|" + to,
        [&](const SortSpec& s, int off, int lim) { return g_app.conn->partnerJobs(partnerId, from, to, PartnerJobSort(s), off, lim, kind); },
        [&] { return g_app.conn->countPartnerJobs(partnerId, from, to, kind); });
    if (pg.count == 0) { ImGui::TextDisabled(kind == 2 ? "Καμία δουλειά σε κοινόχρηστα σε αυτή την περίοδο." : "Καμία δουλειά σε υπηρεσίες πελατών σε αυτή την περίοδο."); return; }
    DrawPartnerJobsTable(id, pg.rows, showPartner, height, m.g);
    DrawPager(m.g.pager, (std::string(id) + "pg").c_str());
}

static void DrawPartnerProfile() {
    if (g_partnerProfileId == 0) return;
    static int lastId = -1; static std::string from, to; static ListModel<DataConn::PartnerJob> mClients, mJoint; static std::string deleteMsg;
    if (lastId != g_partnerProfileId) { lastId = g_partnerProfileId; from.clear(); to.clear(); mClients = ListModel<DataConn::PartnerJob>{}; mJoint = ListModel<DataConn::PartnerJob>{}; deleteMsg.clear(); }
    struct Prof { Partner p; bool found = false; DataConn::PartnerStat st; std::string lastJob; };
    static Cached<Prof> cache;
    std::string key = std::to_string(g_partnerProfileId) + "|" + from + "|" + to + "|" + "stats";
    const Prof& pf = cache.get(key, [&] {
        Prof r;
        if (g_partnerProfileId > 0) { r.p = g_app.conn->getPartner(g_partnerProfileId); r.found = r.p.id > 0; }
        if (g_partnerProfileId == -1) { r.p.id = -1; r.p.name = "#-1 Χωρίς συνεργάτη"; r.p.trade = "Πληρωμές χωρίς καταχωρημένο συνεργάτη"; r.found = true; }
        for (auto& s : g_app.conn->partnerStats(from, to)) if (s.partner_id == g_partnerProfileId) r.st = s;
        r.lastJob = r.st.lastJob;
        return r; });
    if (!pf.found) { g_partnerProfileId = 0; return; }
    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(1000, 760), ImGuiCond_Appearing);
    std::string title = "Συνεργάτης: " + pf.p.name + "###partnerprofile";
    if (ImGui::Begin(title.c_str(), &open)) {
        ImGui::SetWindowFontScale(1.3f); ImGui::TextColored(ImVec4(0.92f, 0.95f, 1.0f, 1.0f), "%s", pf.p.name.c_str()); ImGui::SetWindowFontScale(1.0f);
        if (pf.p.id > 0) { ImGui::SameLine(ImGui::GetContentRegionMax().x - 130); if (ImGui::Button("Επεξεργασία", ImVec2(120, 0))) OpenPartnerEdit(&pf.p); }
        ImGui::Text("%s%s%s%s%s", pf.p.trade.empty() ? "" : pf.p.trade.c_str(), pf.p.phone.empty() ? "" : "  ·  ", pf.p.phone.c_str(),
                    pf.p.email.empty() ? "" : "  ·  ", pf.p.email.c_str());
        if (!pf.p.afm.empty()) ImGui::TextDisabled("ΑΦΜ %s", pf.p.afm.c_str());
        if (!pf.p.notes.empty()) ImGui::TextWrapped("%s", pf.p.notes.c_str());
        ImGui::Spacing();
        if (DrawDateRange("pprange", from, to)) { mClients.g.pager.Reset(); mJoint.g.pager.Reset(); }
        ImGui::Spacing();
        if (ImGui::BeginTable("pptiles", 5, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextRow();
            struct T { const char* l; std::string v; ImVec4 c; std::string s; };
            const auto& st = pf.st; bool ranged = !from.empty() || !to.empty();
            T tiles[5] = { { "Δουλειές", std::to_string(st.jobs), kColLink, ranged ? "στην επιλεγμένη περίοδο" : "συνολικά" },
                           { "Κόστος", Eur(st.cost), kColOut, st.jobs ? "μέσο " + Eur(st.cost / st.jobs) + " ανά δουλειά" : std::string("—") },
                           { "Πληρώθηκε", Eur(st.paid), kColOut, st.cost > 0.005 ? std::to_string((int)std::round(st.paid / st.cost * 100.0)) + "% του κόστους" : std::string("—") },
                           { "Οφειλόμενα τώρα", Eur(st.owedNow), st.owedNow > 0.005 ? kColWeOwe : kColMuted, "όλες οι ημερομηνίες" },
                           { "Τελευταία δουλειά", pf.lastJob.empty() ? std::string("—") : DateGr(pf.lastJob), kColSched,
                             pf.lastJob.empty() ? "" : std::to_string(DaysBetween(pf.lastJob, CurrentDateString())) + " ημέρες πριν" } };
            for (int i = 0; i < 5; i++) { ImGui::TableNextColumn(); ImGui::PushID(i);
                StatTile(tiles[i].l, tiles[i].v, tiles[i].c, tiles[i].s.c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 96)); ImGui::PopID(); }
            ImGui::EndTable();
        }
        SectionTitle("Δουλειές σε υπηρεσίες πελατών");
        ImGui::TextDisabled("«Άνοιγμα» ανοίγει τη χρέωση του πελάτη. Περάστε το ποντίκι πάνω από μια γραμμή για να δείτε τις πληρωμές της.");
        DrawPartnerJobsModel("pjc", g_partnerProfileId, 1, from, to, mClients, false, 230.0f);
        SectionTitle("Δουλειές σε κοινόχρηστα");
        ImGui::TextDisabled("«Άνοιγμα» ανοίγει το κοινόχρηστο με τους ενοίκους και τις πληρωμές του.");
        DrawPartnerJobsModel("pjj", g_partnerProfileId, 2, from, to, mJoint, false, 230.0f);

        // the very end: edit and delete this partner
        if (pf.p.id > 0) {
            ImGui::Spacing(); ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.18f, 1.0f));
            if (ImGui::Button("Διαγραφή συνεργάτη")) { g_partnerRemoveId = pf.p.id; g_partnerRemoveName = pf.p.name; deleteMsg.clear(); }
            ImGui::PopStyleColor();
            ImGui::TextDisabled("Ο συνεργάτης κρύβεται από τις λίστες· οι δουλειές του μένουν στο ιστορικό και στο Αρχείο διαγραφών.");
        }
    }
    ImGui::End();
    if (!open) g_partnerProfileId = 0;
}

static void OpenPartnerProfile(int id) { g_partnerProfileId = id; g_page = PAGE_PARTNERS; }
static void OpenOffersPage() { g_page = PAGE_OFFERS; }
static void OpenNoteFromSearch(const Note& n) { g_page = PAGE_NOTES; OpenEditNote(n); }

static void DrawPartnersScreen() {
    enum { PT_NAME, PT_TRADE, PT_PHONE, PT_EMAIL, PT_JOBS, PT_OWED, PT_LAST, PT_COUNT };
    static const ListColDef cols[PT_COUNT] = {
        { "Όνομα", ImGuiTableColumnFlags_WidthFixed, 230.0f, true },       { "Ειδικότητα", ImGuiTableColumnFlags_WidthFixed, 190.0f, true },
        { "Τηλέφωνο", ImGuiTableColumnFlags_WidthFixed, 140.0f, true },    { "Email", ImGuiTableColumnFlags_WidthFixed, 220.0f, true },
        { "Δουλειές", ImGuiTableColumnFlags_WidthFixed, 100.0f, true },    { "Οφειλόμενα", ImGuiTableColumnFlags_WidthFixed, 130.0f, true },
        { "Τελευταία δουλειά", ImGuiTableColumnFlags_WidthFixed, 150.0f, true } };
    static ListModel<DataConn::PartnerRow> pm; static char q[128] = "";
    static std::string rfrom, rto; static GridState rg; Pager& rpager = rg.pager;
    pm.Init(PT_COUNT, { { PT_NAME, true } }, 20);
    if (rg.pager.pageSize == 10 || rg.pager.pageSize == 0) rg.pager.pageSize = 20;
    GridState& g = pm.g;

    if (ImGui::Button("+ Νέος συνεργάτης")) OpenPartnerEdit(nullptr);
    ImGui::SameLine(); ImGui::SetNextItemWidth(300);
    if (ImGui::InputTextWithHint("##ptq", "Αναζήτηση: όνομα, ειδικότητα, τηλέφωνο...", q, sizeof(q))) g.pager.Reset();

    // the page comes from the database: every sort click / page change / search asks it again
    const auto& data = pm.Load(q, [&](const SortSpec& s, int off, int lim) { return g_app.conn->pagePartnerRows(q, s, off, lim); },
                                  [&] { return g_app.conn->countPartnerRows(q); });
    const auto& rows = data.rows;
    ImGui::SameLine(); ImGui::TextDisabled("%d συνεργάτες", g.pager.total);
    ImGui::TextDisabled("Κλικ στο όνομα ενός συνεργάτη: το προφίλ του με όλες τις δουλειές (εκεί και η επεξεργασία / διαγραφή). Το «Κόστος για εμάς» κάθε χρέωσης μπορεί να συνδεθεί με συνεργάτη.");
    float avail = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("partnerlist", ImVec2(0, std::max(180.0f, avail * 0.45f)), false);
    if (rows.empty()) ImGui::TextDisabled(q[0] ? "Κανένας συνεργάτης με αυτή την αναζήτηση." : "Δεν έχετε καταχωρήσει συνεργάτες ακόμα. Πατήστε «+ Νέος συνεργάτης».");
    else {
        float h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.6f;
        if (ImGui::BeginTable("partners", PT_COUNT, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
                                                    ImGuiTableFlags_Resizable, ImVec2(0, std::max(100.0f, h)))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ListSetupColumns(g.order, cols);
            ListHeaderRow(g.order, cols, g.sort, g.pager);
            auto P = [&](int c) { return ListPos(g.order, c); };
            for (const auto& r : rows) {
                const Partner& p = r.p;
                ImGui::PushID(p.id); ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(P(PT_NAME)); if (LinkCell(p.name)) g_partnerProfileId = p.id;
                ImGui::TableSetColumnIndex(P(PT_TRADE)); ImGui::TextUnformatted(p.trade.c_str());
                ImGui::TableSetColumnIndex(P(PT_PHONE)); ImGui::TextUnformatted(p.phone.c_str());
                ImGui::TableSetColumnIndex(P(PT_EMAIL)); ImGui::TextUnformatted(p.email.c_str());
                ImGui::TableSetColumnIndex(P(PT_JOBS)); ImGui::Text("%d", r.jobs);
                ImGui::TableSetColumnIndex(P(PT_OWED)); if (r.owed > 0.005) ImGui::TextColored(kColOwed, "%s", Eur(r.owed).c_str()); else ImGui::TextDisabled("—");
                ImGui::TableSetColumnIndex(P(PT_LAST)); if (r.last.empty()) ImGui::TextDisabled("—"); else ImGui::TextUnformatted(DateGr(r.last).c_str());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        DrawPager(g.pager, "partnerspg");
    }
    ImGui::EndChild();

    SectionTitle("Πρόσφατες ενέργειες");
    static ListModel<DataConn::PartnerJob> dummyReset;
    if (DrawDateRange("recrange", rfrom, rto)) { rg.pager.Reset(); }
    static ListModel<DataConn::PartnerJob> mrClients, mrJoint;
    if (ImGui::BeginTabBar("recenttabs")) {
        if (ImGui::BeginTabItem("Σε υπηρεσίες πελατών")) {
            DrawPartnerJobsModel("recentc", 0, 1, rfrom, rto, mrClients, true, std::max(100.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.8f));
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Σε κοινόχρηστα")) {
            DrawPartnerJobsModel("recentj", 0, 2, rfrom, rto, mrJoint, true, std::max(100.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.8f));
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    DrawPartnerProfile();
    DrawPartnerDialogs();
}

// Everything we owe, as one list: which partner, for which service / joint charge, how much is left. Opened by the purple tiles.
static bool g_showOwed = false;
static void DrawOwedWindow() {
    if (!g_showOwed) return;
    static ListModel<DataConn::PartnerJob> m;
    ImGui::SetNextWindowSize(ImVec2(1180, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Οφειλόμενα: όσα οφείλουμε", &g_showOwed)) { ImGui::End(); return; }
    m.Init(PJ_COUNT, {{PJ_DATE, true}}, 20);
    const auto& pg = m.Load("owed",
        [&](const SortSpec& s, int off, int lim) { return g_app.conn->partnerJobs(0, "", "", PartnerJobSort(s), off, lim, 0, true); },
        [&] { return g_app.conn->countPartnerJobs(0, "", "", 0, true); });
    double total = g_app.conn->cashNow().owedOut;
    ImGui::TextColored(kColWeOwe, "Σύνολο οφειλόμενων: %s", Eur(total).c_str()); ImGui::SameLine(); ImGui::TextDisabled("· %d γραμμές", pg.count);
    ImGui::TextDisabled("Κάθε γραμμή είναι κόστος που δεν έχουμε πληρώσει (ή πληρώσαμε μερικώς). «Πληρωμή» πληρώνει όλο ή μέρος· «Άνοιγμα» δείχνει τη χρέωση.");
    if (pg.count == 0) ImGui::TextColored(kColIn, "Δεν οφείλουμε τίποτα.");
    else {
        DrawPartnerJobsTable("owedlist", pg.rows, true, std::max(160.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 2.2f), m.g);
        DrawPager(m.g.pager, "owedpg");
    }
    ImGui::End();
}
static void HomeFiguresRow(const char* id, const DataConn::HomeFigures& f) {
    if (!ImGui::BeginTable(id, 4, ImGuiTableFlags_SizingStretchSame)) return;
    ImGui::TableNextRow();
    struct T { const char* l; std::string v; ImVec4 c; std::string s; };
    const double weOwe = MoneySub(f.cost, f.costPaid);
    T tiles[4] = { { "Εισπράξεις", Eur(f.collected), kColIn, "από " + Eur(f.charged) + " που χρεώσαμε" },
                   { "Οφειλές", Eur(f.owed), kColOwed, std::to_string(f.charges) + " χρεώσεις" },
                   { "Πληρωμές", Eur(f.costPaid), kColOut, "από κόστος " + Eur(f.cost) },
                   { "Οφειλόμενα", Eur(weOwe), kColWeOwe, "κόστος απλήρωτο · κλικ" } };
    for (int i = 0; i < 4; i++) { ImGui::TableNextColumn(); ImGui::PushID(i);
        StatTile(tiles[i].l, tiles[i].v, tiles[i].c, tiles[i].s.c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 104));
        if (i == 3) { if (ImGui::IsItemHovered()) { ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); ImGui::SetTooltip("Λίστα με όλα όσα οφείλουμε"); } if (ImGui::IsItemClicked(0)) g_showOwed = true; }
        ImGui::PopID(); }
    ImGui::EndTable();
}

// ---- Home: one look at the whole business. Period figures follow the date filter; what is owed (to us / by us) is always
// the total, any date. Our own income is kept apart from κοινόχρηστα, which is the buildings' money passing through us.
static bool HomeTile(const char* label, const std::string& value, ImVec4 col, const std::string& sub, const char* tip = nullptr) {
    StatTile(label, value, col, sub.c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 96));
    bool hov = ImGui::IsItemHovered();
    if (hov && tip) { ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); ImGui::SetTooltip("%s", tip); }
    return tip && ImGui::IsItemClicked(0);
}
static bool HomeSection(const char* title, const char* hint) {
    ImGui::Spacing();
    ImGui::SetWindowFontScale(1.15f); ImGui::TextColored(ImVec4(0.85f, 0.90f, 1.0f, 1), "%s", title); ImGui::SetWindowFontScale(1.0f);
    if (hint) { ImGui::SameLine(); ImGui::TextDisabled("  %s", hint); }
    return true;
}

// Who owes us: clients' own services (joint = false) or residents' κοινόχρηστα shares (joint = true). Click a name to
// see the open charges, «Είσπραξη» to record money from them.
static void DrawHomeOwings(const char* id, bool joint, float height) {
    enum { HC_NAME, HC_OWING, HC_N, HC_OLDEST, HC_ACT, HC_COUNT };
    static const ListColDef cols[HC_COUNT] = {
        { "Όνομα", ImGuiTableColumnFlags_WidthStretch, 0.0f, true }, { "Οφείλει", ImGuiTableColumnFlags_WidthFixed, 95.0f, true },
        { "Χρεώσεις", ImGuiTableColumnFlags_WidthFixed, 70.0f, true }, { "Από", ImGuiTableColumnFlags_WidthFixed, 85.0f, true },
        { "", ImGuiTableColumnFlags_WidthFixed, 72.0f, false } };
    struct St { GridState g; char q[128] = ""; std::set<int> open; };
    static St st[2]; St& s = st[joint ? 1 : 0];
    GridState& g = s.g;
    if (g.order.empty()) { for (int i = 0; i < HC_COUNT; i++) g.order.push_back(i); g.sort.keys = { { HC_OWING, false } }; g.pager.pageSize = 10; }
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##q", joint ? "Αναζήτηση ενοίκου..." : "Αναζήτηση πελάτη...", s.q, sizeof(s.q))) g.pager.Reset();
    std::string sk;
    for (auto& k : g.sort.keys) sk += std::to_string(k.first) + (k.second ? "a" : "d");
    struct Pg { std::vector<DataConn::ClientOwing> rows; int count = 0; };
    static Cached<Pg> pc[2];
    const Pg& pg = pc[joint ? 1 : 0].get(std::string(s.q) + "|" + sk + "|" + std::to_string(g.pager.offset) + "|" + std::to_string(g.pager.pageSize), [&] {
        Pg p; p.count = g_app.conn->countClientOwings(s.q, nullptr, joint);
        p.rows = g_app.conn->pageClientOwings(s.q, g.sort.keys, g.pager.offset, g.pager.pageSize, joint); return p; });
    g.pager.total = pg.count;
    if (pg.count == 0) ImGui::TextColored(kColIn, joint ? "Κανένας ένοικος δεν οφείλει κοινόχρηστα." : "Κανένας πελάτης δεν οφείλει.");
    else {
        ImGui::BeginChild("##list", ImVec2(0, height), ImGuiChildFlags_None);
        if (ImGui::BeginTable("##t", HC_COUNT, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
            ListSetupColumns(g.order, cols);
            ListHeaderRow(g.order, cols, g.sort, g.pager);
            auto P = [&](int c) { return ListPos(g.order, c); };
            for (auto& r : pg.rows) {
                ImGui::PushID(r.user_id); ImGui::TableNextRow();
                bool isOpen = s.open.count(r.user_id) > 0;
                ImGui::TableSetColumnIndex(P(HC_NAME));
                std::string nm = std::string(isOpen ? "▼ " : "► ") + r.name;
                if (ImGui::Selectable(nm.c_str(), isOpen, ImGuiSelectableFlags_AllowOverlap)) { if (isOpen) s.open.erase(r.user_id); else s.open.insert(r.user_id); }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Κλικ: τι οφείλει · διπλό κλικ: καρτέλα πελάτη");
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) OpenUserProfile(r.user_id);
                ImGui::TableSetColumnIndex(P(HC_OWING)); ImGui::TextColored(kColOwed, "%s", Eur(r.owing).c_str());
                ImGui::TableSetColumnIndex(P(HC_N)); ImGui::Text("%d", r.openCharges);
                ImGui::TableSetColumnIndex(P(HC_OLDEST)); ImGui::TextUnformatted(DateGr(r.oldest).c_str());
                ImGui::TableSetColumnIndex(P(HC_ACT)); if (ImGui::SmallButton("Είσπραξη")) OpenRecordPayment(r.user_id);
                if (isOpen) {
                    ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
                    ImGui::Indent(14);
                    static std::map<int, Cached<std::vector<Payment>>> chargeCaches;
                    for (auto& p : chargeCaches[r.user_id].get("", [&] { return g_app.conn->getOpenCharges(r.user_id); })) {
                        if ((p.joint_op_id > 0) != joint) continue;
                        ImGui::PushID(p.id);
                        ImGui::TextDisabled("%s", DateGr(p.year, p.month, p.day).c_str()); ImGui::SameLine();
                        ImGui::TextUnformatted(ChargeLabel(p).c_str());
                        ImGui::SameLine(); ImGui::TextColored(kColOwed, "%s", Eur(p.balance).c_str());
                        ImGui::SameLine(); if (ImGui::SmallButton("Προβολή")) { if (joint) OpenJointView(p.joint_op_id); else OpenPaymentView(p.id); }
                        ImGui::PopID();
                    }
                    ImGui::Unindent(14);
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
        DrawPager(g.pager, "pg");
    }
    ImGui::PopID();
}

// What we owe our partners, biggest first, with «Πληρωμή» on each line.
static void DrawHomeWeOwe(float height) {
    static Pager pager; static bool init = false; if (!init) { pager.pageSize = 10; init = true; }
    struct Pg { std::vector<DataConn::PartnerJob> rows; int count = 0; };
    static Cached<Pg> pc;
    const Pg& pg = pc.get(std::to_string(pager.offset) + "|" + std::to_string(pager.pageSize), [&] {
        Pg p; p.count = g_app.conn->countPartnerJobs(0, "", "", 0, true);
        p.rows = g_app.conn->partnerJobs(0, "", "", {{6, false}}, pager.offset, pager.pageSize, 0, true); return p; });
    pager.total = pg.count;
    if (pg.count == 0) { ImGui::TextColored(kColIn, "Δεν χρωστάμε τίποτα σε κανέναν."); return; }
    ImGui::BeginChild("##weowe", ImVec2(0, height), ImGuiChildFlags_None);
    if (ImGui::BeginTable("##wo", 4, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Σε ποιον", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Για", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Οφείλουμε", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 68.0f);
        ImGui::TableHeadersRow();
        for (auto& j : pg.rows) {
            ImGui::PushID(j.charge_id); ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Selectable(j.partner.empty() ? "(χωρίς συνεργάτη)" : j.partner.c_str(), false, ImGuiSelectableFlags_AllowOverlap) && j.partner_id > 0) OpenPartnerProfile(j.partner_id);
            ImGui::TableNextColumn();
            std::string what = j.service + (j.jointLabel.empty() ? (j.client.empty() ? "" : " · " + j.client) : " · " + j.jointLabel);
            ImGui::TextUnformatted(what.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s · %s", DateGr(j.date).c_str(), what.c_str());
            ImGui::TableNextColumn(); ImGui::TextColored(kColWeOwe, "%s", Eur(j.balance).c_str());
            ImGui::TableNextColumn(); if (ImGui::SmallButton("Πληρωμή")) OpenPayOut(j);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    DrawPager(pager, "wopg");
}

// Home only: a slim tab on the right edge of the window. Hovering it slides a board in from the right, up to half
// the window wide, with the 10 newest notes as sticky notes; it slides away when the mouse leaves it.
static void DrawNotesDrawer() {
    static Cached<std::vector<Note>> nc;
    const auto& notes = nc.get("", [] {
        auto v = g_app.conn->getAllNotes();
        std::sort(v.begin(), v.end(), [](const Note& a, const Note& b) { return a.created_at > b.created_at; });
        if (v.size() > 10) v.resize(10);
        return v; });
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float tab = 24.0f, maxW = vp->WorkSize.x * 0.5f;
    const float right = vp->WorkPos.x + vp->WorkSize.x;
    static float w = 0.0f; static bool open = false;
    ImVec2 mp = ImGui::GetIO().MousePos;
    bool inY = mp.y >= vp->WorkPos.y && mp.y <= vp->WorkPos.y + vp->WorkSize.y;
    bool anyPopup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    if (inY && mp.x >= right - tab && mp.x <= right) open = true;
    else if (open && !anyPopup && (mp.x < right - tab - w - 12.0f || !inY)) open = false;
    float target = open ? maxW : 0.0f;
    w += (target - w) * std::min(1.0f, ImGui::GetIO().DeltaTime * 12.0f);
    if (std::fabs(w - target) < 0.5f) w = target;

    ImGui::SetNextWindowPos(ImVec2(right - tab - w, vp->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(tab + w, vp->WorkSize.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.11f, 0.14f, w > 1.0f ? 0.98f : 0.0f));
    ImGui::Begin("##notesdrawer", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                           ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoScrollbar);
    if (!anyPopup) ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());     // above the page, below dialogs
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // the tab: a yellow strip on the window's right edge with «ΣΗΜΕΙΩΣΕΙΣ» written downwards
    ImVec2 t0(right - tab, vp->WorkPos.y + vp->WorkSize.y * 0.30f), t1(right, vp->WorkPos.y + vp->WorkSize.y * 0.70f);
    dl->AddRectFilled(t0, t1, open ? IM_COL32(240, 200, 80, 255) : IM_COL32(200, 165, 60, 220), 6.0f, ImDrawFlags_RoundCornersLeft);
    {
        const char* word = "ΣΗΜΕΙΩΣΕΙΣ"; float yy = t0.y + 14.0f;
        for (const char* c = word; *c; ) {
            int len = ((unsigned char)*c >= 0xC0) ? 2 : 1;
            std::string ch(c, len); ImVec2 sz = ImGui::CalcTextSize(ch.c_str());
            dl->AddText(ImVec2(t0.x + (tab - sz.x) * 0.5f, yy), IM_COL32(40, 30, 10, 255), ch.c_str());
            yy += sz.y; c += len;
        }
    }
    if (w > 60.0f) {
        ImGui::SetCursorPos(ImVec2(16, 14));
        ImGui::BeginChild("##board", ImVec2(w - 24.0f, vp->WorkSize.y - 28.0f), ImGuiChildFlags_None);
        ImGui::SetWindowFontScale(1.25f); ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1), "Σημειώσεις"); ImGui::SetWindowFontScale(1.0f);
        ImGui::SameLine(); ImGui::TextDisabled("  οι 10 πιο πρόσφατες");
        if (ImGui::Button("+ Νέα σημείωση")) OpenAddNote();
        ImGui::SameLine(); if (ImGui::Button("Όλες οι σημειώσεις ►")) { g_page = PAGE_NOTES; open = false; }
        ImGui::Spacing();
        if (notes.empty()) ImGui::TextDisabled("Καμία σημείωση. Γράψτε ό,τι θέλετε να θυμάστε.");
        const float cardW = 230.0f, cardH = 170.0f, gap = 14.0f;
        int perRow = std::max(1, (int)((ImGui::GetContentRegionAvail().x + gap) / (cardW + gap)));
        static const ImVec4 paper[] = { {0.98f, 0.90f, 0.55f, 1}, {0.70f, 0.90f, 0.98f, 1}, {0.98f, 0.75f, 0.78f, 1}, {0.75f, 0.95f, 0.72f, 1} };
        for (int i = 0; i < (int)notes.size(); i++) {
            const Note& n = notes[i];
            if (i % perRow) ImGui::SameLine(0, gap);
            ImGui::PushID(n.id);
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            float tilt = (i % 3 - 1) * 2.0f;          // a little untidy, like a real board
            ImU32 bg = ImGui::GetColorU32(paper[i % 4]);
            dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(ImVec2(p0.x + 4, p0.y + 6 + tilt), ImVec2(p0.x + cardW + 4, p0.y + cardH + 6 + tilt), IM_COL32(0, 0, 0, 90), 4.0f);
            dl->AddRectFilled(ImVec2(p0.x, p0.y + tilt), ImVec2(p0.x + cardW, p0.y + cardH + tilt), bg, 4.0f);
            dl->AddCircleFilled(ImVec2(p0.x + cardW * 0.5f, p0.y + 8 + tilt), 6.0f, IM_COL32(200, 50, 50, 255));      // the pin
            bool clicked = ImGui::InvisibleButton("##card", ImVec2(cardW, cardH));
            if (ImGui::IsItemHovered()) { ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); ImGui::SetTooltip("Κλικ για επεξεργασία"); }
            if (clicked) OpenEditNote(n);
            ImVec4 ink(0.15f, 0.12f, 0.08f, 1);
            std::string title = n.title.empty() ? std::string("(χωρίς τίτλο)") : n.title;
            dl->PushClipRect(ImVec2(p0.x + 8, p0.y + tilt), ImVec2(p0.x + cardW - 8, p0.y + cardH + tilt - 6), true);
            dl->AddText(nullptr, ImGui::GetFontSize() * 1.1f, ImVec2(p0.x + 10, p0.y + 20 + tilt), ImGui::GetColorU32(ink), title.c_str(), nullptr, cardW - 20);
            std::string body = n.content.substr(0, std::min<size_t>(n.content.size(), 220));
            dl->AddText(nullptr, ImGui::GetFontSize() * 0.92f, ImVec2(p0.x + 10, p0.y + 46 + tilt), IM_COL32(60, 50, 35, 255), body.c_str(), nullptr, cardW - 20);
            if (!n.created_at.empty())
                dl->AddText(ImVec2(p0.x + 10, p0.y + cardH - 22 + tilt), IM_COL32(110, 95, 70, 255), DateGr(n.created_at.substr(0, 10)).c_str());
            dl->PopClipRect();
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

static void DrawHomeScreen() {
    std::string today = CurrentDateString();
    int y = 0, m = 0, d = 0; std::sscanf(today.c_str(), "%d-%d-%d", &y, &m, &d);
    {
        std::time_t t = std::time(nullptr); std::tm tmv{}; localtime_s(&tmv, &t);
        const char* hello = tmv.tm_hour < 12 ? "Καλημέρα" : (tmv.tm_hour < 18 ? "Καλό απόγευμα" : "Καλησπέρα");
        ImGui::SetWindowFontScale(1.4f); ImGui::TextColored(ImVec4(0.92f, 0.95f, 1.0f, 1.0f), "%s", hello); ImGui::SetWindowFontScale(1.0f);
        ImGui::SameLine(); ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("  %s", DateGr(today).c_str());
    }
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 190);
    if (ImGui::Button("Αναλυτικά σύνολα ►", ImVec2(190, 0))) g_page = PAGE_DASHBOARD;

    // ---- quick actions: the things done every day, one click away
    {
        const float bw = 190, bh = 34;
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.30f, 1)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.55f, 0.36f, 1));
        if (ImGui::Button("+ Είσπραξη", ImVec2(bw, bh))) OpenRecordPayment(-1);
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ήρθαν χρήματα από πελάτη ή ένοικο");
        ImGui::SameLine();
        if (ImGui::Button("+ Χρέωση πελάτη", ImVec2(bw, bh))) ImGui::OpenPopup("##quickcharge");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Κάναμε μια δουλειά σε πελάτη: χρεώνουμε υπηρεσία ή πακέτο");
        ImGui::SameLine();
        if (ImGui::Button("+ Κοινόχρηστο", ImVec2(bw, bh))) OpenJointCharge(-1);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Χρέωση σε πολυκατοικία, με τα ποσά κάθε διαμερίσματος");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.42f, 0.28f, 0.55f, 1)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.50f, 0.34f, 0.65f, 1));
        if (ImGui::Button("Πληρωμή συνεργάτη", ImVec2(bw, bh))) g_showOwed = true;
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Πληρώνουμε κάτι που χρωστάμε σε συνεργάτη");
        ImGui::SameLine();
        if (ImGui::Button("+ Σημείωση", ImVec2(bw * 0.7f, bh))) OpenAddNote();
        ImGui::PopStyleVar();

        if (ImGui::BeginPopup("##quickcharge")) {
            static int qUser = -1, qKind = 0, qSvc = -1, qPkg = -1;
            ImGui::TextDisabled("Γρήγορη χρέωση");
            ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Πελάτης:"); ImGui::SameLine(90); PickClient("qcli", qUser, 320);
            ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Τι:"); ImGui::SameLine(90);
            ImGui::RadioButton("Υπηρεσία", &qKind, 0); ImGui::SameLine(); ImGui::RadioButton("Πακέτο υπηρεσιών", &qKind, 1);
            ImGui::Dummy(ImVec2(82, 0)); ImGui::SameLine();
            if (qKind == 0) PickService("qsvc", qSvc, 320); else PickPackage("qpkg", qPkg, 320);
            bool ready = qUser > 0 && (qKind == 0 ? qSvc > 0 : qPkg > 0);
            ImGui::BeginDisabled(!ready);
            if (ImGui::Button("Συνέχεια ►", ImVec2(160, 0))) { g_ccUser = qUser; g_ccService = qSvc; g_ccPkg = qPkg; g_ccOpenNext = qKind == 0 ? 1 : 2; ImGui::CloseCurrentPopup(); }
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }
    }

    // ---- the period: quick choices + any custom dates
    static std::string from, to;
    static bool inited = false;
    if (!inited) { char b[16]; std::snprintf(b, sizeof(b), "%04d-%02d-01", y, m); from = b; to = IsoFromDays(DaysFromCivil(m == 12 ? y + 1 : y, m == 12 ? 1 : m + 1, 1) - 1); inited = true; }
    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Περίοδος:"); ImGui::SameLine();
    if (ImGui::SmallButton("Σήμερα")) { from = to = today; }
    ImGui::SameLine(); if (ImGui::SmallButton("Προηγ. μήνας")) {
        int py = m == 1 ? y - 1 : y, pm = m == 1 ? 12 : m - 1; char b[16]; std::snprintf(b, sizeof(b), "%04d-%02d-01", py, pm); from = b;
        to = IsoFromDays(DaysFromCivil(y, m, 1) - 1); }
    ImGui::SameLine(); if (ImGui::SmallButton("Πέρσι")) { from = std::to_string(y - 1) + "-01-01"; to = std::to_string(y - 1) + "-12-31"; }
    ImGui::SameLine(0, 14);
    DrawDateRange("homerange", from, to);
    std::string periodTitle = from.empty() && to.empty() ? std::string("όλες οι ημερομηνίες")
                            : (from.empty() ? std::string("…") : DateGr(from)) + " – " + (to.empty() ? std::string("…") : DateGr(to));

    struct HomeData { DataConn::CashNow cash; DataConn::HomeFigures cl, bd; double clientsOwing = 0, residentsOwing = 0; int clientsN = 0, residentsN = 0; };
    static Cached<HomeData> cache;
    const HomeData& hd = cache.get(from + "|" + to, [&] {
        HomeData r;
        r.cash = g_app.conn->cashNow();
        r.cl = g_app.conn->homeFigures(0, from, to);
        r.bd = g_app.conn->homeFigures(1, from, to);
        r.clientsN = g_app.conn->countClientOwings("", &r.clientsOwing, false);
        r.residentsN = g_app.conn->countClientOwings("", &r.residentsOwing, true);
        return r;
    });

    // ---- 1. our own business in the period
    HomeSection("Η επιχείρησή μας", ("έσοδα από τις δικές μας υπηρεσίες · " + periodTitle).c_str());
    if (ImGui::BeginTable("homebiz", 4, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        const double costUnpaid = MoneySub(hd.cl.cost, hd.cl.costPaid);
        ImGui::TableNextColumn(); HomeTile("Έσοδα (χρεώσαμε)", Eur(hd.cl.charged), ImVec4(0.55f, 0.80f, 1.0f, 1), std::to_string(hd.cl.charges) + " χρεώσεις");
        ImGui::TableNextColumn(); HomeTile("Εισπράξαμε", Eur(hd.cl.collected), kColIn, "μένουν " + Eur(hd.cl.owed));
        ImGui::TableNextColumn(); HomeTile("Κόστος (συνεργάτες)", Eur(hd.cl.cost), kColOut, costUnpaid > 0.005 ? "απλήρωτα " + Eur(costUnpaid) : std::string("όλα πληρωμένα"));
        ImGui::TableNextColumn(); HomeTile("Κέρδος από δουλειές", Eur(hd.cl.profit()), hd.cl.profit() >= 0 ? kColIn : kColOut, "χρεώσαμε − κόστος · εισπραγμένο: " + Eur(hd.cl.cashProfit()),
                                          "Πόσα κερδίζουμε από τις δουλειές της περιόδου (με ημερομηνία χρέωσης),\nείτε πληρώθηκαν είτε όχι. Δεν είναι το ταμείο: αυτό δείχνει πότε μπήκαν/βγήκαν τα χρήματα.");
        ImGui::EndTable();
    }

    // ---- 2. κοινόχρηστα: the buildings' money, shown apart so it never looks like our income
    HomeSection("Κοινόχρηστα πολυκατοικιών", ("χρήματα των κτιρίων που περνούν από εμάς · " + periodTitle).c_str());
    if (ImGui::BeginTable("homejoint", 4, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        const double margin = MoneySub(hd.bd.charged, hd.bd.cost);
        const double held = MoneySub(hd.bd.collected, hd.bd.costPaid);
        ImGui::TableNextColumn(); HomeTile("Χρεώθηκαν σε ενοίκους", Eur(hd.bd.charged), ImVec4(0.95f, 0.75f, 0.45f, 1), std::to_string(hd.bd.charges) + " μερίδια");
        ImGui::TableNextColumn(); HomeTile("Εισπράχθηκαν", Eur(hd.bd.collected), kColIn, "μένουν " + Eur(hd.bd.owed));
        ImGui::TableNextColumn(); HomeTile("Πληρώθηκαν σε συνεργάτες", Eur(hd.bd.costPaid), kColOut, "από κόστος " + Eur(hd.bd.cost));
        ImGui::TableNextColumn(); HomeTile("Διαφορά (δική μας)", Eur(margin), margin >= 0 ? kColIn : kColOut, "κρατάμε τώρα: " + Eur(held));
        ImGui::EndTable();
    }

    // ---- 3. right now, any date
    HomeSection("Τώρα", "σύνολα όλων των ημερομηνιών");
    if (ImGui::BeginTable("homenow", 4, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); HomeTile("Μας χρωστάνε", Eur(hd.cash.owedIn), kColOwed,
            "πελάτες " + Eur(hd.clientsOwing) + " · ένοικοι " + Eur(hd.residentsOwing));
        ImGui::TableNextColumn(); if (HomeTile("Χρωστάμε", Eur(hd.cash.owedOut), kColWeOwe, "σε συνεργάτες · κλικ για λίστα", "Όλα όσα οφείλουμε, σε ποιον και για τι")) g_showOwed = true;
        ImGui::TableNextColumn(); HomeTile("Ταμείο", Eur(hd.cash.net()), hd.cash.net() >= 0 ? kColIn : kColOut, "μπήκαν " + Eur(hd.cash.moneyIn) + " · βγήκαν " + Eur(hd.cash.moneyOut));
        ImGui::TableNextColumn(); HomeTile("Αν εξοφληθούν όλα", Eur(hd.cash.expected()), hd.cash.expected() >= 0 ? kColIn : kColOut, "ταμείο + μας χρωστάνε − χρωστάμε");
        ImGui::EndTable();
    }

    // ---- 4. lists: who owes us · whom we owe · notes
    ImGui::Spacing();
    float W = ImGui::GetContentRegionAvail().x, sp = ImGui::GetStyle().ItemSpacing.x;
    float colW = std::max(320.0f, (W - sp - 26.0f) / 2.0f);      // room on the right for the notes tab
    float listH = std::max(180.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 5.0f);
    float boxH = listH + ImGui::GetFrameHeightWithSpacing() * 4.2f;

    ImGui::BeginChild("homeowes", ImVec2(colW, boxH), ImGuiChildFlags_Border);
    ImGui::TextColored(kColOwed, "Μας χρωστάνε");
    if (ImGui::BeginTabBar("##owetabs")) {
        std::string t1 = "Πελάτες (" + std::to_string(hd.clientsN) + ")###cl";
        std::string t2 = "Ένοικοι · κοινόχρηστα (" + std::to_string(hd.residentsN) + ")###rs";
        if (ImGui::BeginTabItem(t1.c_str())) { DrawHomeOwings("cl", false, listH - ImGui::GetFrameHeightWithSpacing()); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(t2.c_str())) { DrawHomeOwings("rs", true, listH - ImGui::GetFrameHeightWithSpacing()); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::SameLine(0, sp);

    ImGui::BeginChild("homeweowe", ImVec2(-26.0f, boxH), ImGuiChildFlags_Border);
    ImGui::TextColored(kColWeOwe, "Χρωστάμε");
    ImGui::SameLine(); ImGui::TextDisabled("· %s", Eur(hd.cash.owedOut).c_str());
    ImGui::SameLine(); if (ImGui::SmallButton("Συνεργάτες ►")) g_page = PAGE_PARTNERS;
    DrawHomeWeOwe(listH);
    ImGui::EndChild();
    DrawNotesDrawer();

    // ---- 5. partners in the period
    {
        static Cached<std::vector<DataConn::PartnerStat>> pcache;
        std::vector<DataConn::PartnerStat> ps = pcache.get(from + "|" + to, [&] { return g_app.conn->partnerStats(from, to); });
        double tc = 0, tp = 0, to_ = 0; for (auto& s : ps) { tc += s.cost; tp += s.paid; to_ += s.owedNow; }
        std::string head = "Συνεργάτες στην περίοδο  ·  κόστος " + Eur(tc) + "  ·  πληρώσαμε " + Eur(tp) + "  ·  χρωστάμε τώρα " + Eur(to_) + "###homepartnershdr";
        if (ImGui::CollapsingHeader(head.c_str())) {
            if (ps.empty()) ImGui::TextDisabled("Δεν υπάρχουν συνεργάτες. Προσθέστε από την καρτέλα «Συνεργάτες».");
            else if (ImGui::BeginTable("homepartners", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | kSortableTable)) {
                ImGui::TableSetupColumn("Συνεργάτης", 0, 2.0f); ImGui::TableSetupColumn("Ειδικότητα", 0, 1.5f);
                ImGui::TableSetupColumn("Δουλειές", 0, 1.0f); ImGui::TableSetupColumn("Κόστος", 0, 1.0f);
                ImGui::TableSetupColumn("Πληρώσαμε", 0, 1.0f); ImGui::TableSetupColumn("Χρωστάμε τώρα", 0, 1.0f);
                ImGui::TableHeadersRow();
                SortByTableSpecs(ps, [](int c, const DataConn::PartnerStat& a, const DataConn::PartnerStat& b) {
                    switch (c) { case 0: return CmpText(a.name, b.name); case 1: return CmpText(a.trade, b.trade); case 2: return Cmp3(a.jobs, b.jobs);
                                 case 3: return Cmp3(ToCents(a.cost), ToCents(b.cost)); case 4: return Cmp3(ToCents(a.paid), ToCents(b.paid)); default: return Cmp3(ToCents(a.owedNow), ToCents(b.owedNow)); } });
                for (auto& s : ps) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::PushID(s.partner_id); if (ImGui::Selectable(s.name.c_str(), false)) { g_partnerProfileId = s.partner_id; g_page = PAGE_PARTNERS; } ImGui::PopID();
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(s.trade.c_str());
                    ImGui::TableNextColumn(); ImGui::Text("%d", s.jobs);
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(Eur(s.cost).c_str());
                    ImGui::TableNextColumn(); ImGui::TextColored(kColOut, "%s", Eur(s.paid).c_str());
                    ImGui::TableNextColumn(); if (s.owedNow > 0.005) ImGui::TextColored(kColWeOwe, "%s", Eur(s.owedNow).c_str()); else ImGui::TextDisabled("—");
                }
                ImGui::EndTable();
            }
        }
    }
    DrawNoteDialog();
}



// =====================================================================================================
// ΧΡΕΩΣΕΙΣ / ΤΑΜΕΙΟ
// A bill says "you owe us this". It stays open until money fills it: every payment, whole or part, is recorded
// with «Είσπραξη» (money in, with its receipt) or «Πληρωμή» (money out to a partner) and the bill closes only when
// nothing is left. Clients are billed one by one; a building is billed once (a κοινόχρηστο) and that total is split
// into one bill per resident, each paid and receipted on its own. The Ταμείο lists every movement of money.
// =====================================================================================================
// Paid / partly / not yet, as a coloured word.
static void PayBadge(double due, double paid, bool future = false) {
    if (future) { ImGui::TextColored(kColSched, "Προγραμματισμένη"); return; }
    long long d = ToCents(due), p = ToCents(paid);
    if (p >= d && d > 0) ImGui::TextColored(kColIn, "Εξοφλήθηκε");
    else if (p > 0) ImGui::TextColored(kColOwed, "Μερικώς");
    else ImGui::TextColored(kColOut, "Απλήρωτη");
}

// «Είσπραξη» / «Πληρωμή» on one bill: the usual dialog with this bill filled in. Any amount up to what is left can
// be entered; the bill closes only when it is filled.
static void OpenRecordPaymentFor(const Payment& p) {
    double left = MoneySub(p.amount_due, p.amount_paid);
    if (left < 0) left = 0;
    if (p.direction == "out") {
        DataConn::PartnerJob j; j.charge_id = p.id; j.balance = left; j.service = ChargeLabel(p);
        j.partner = p.partner_id > 0 ? g_app.conn->getPartner(p.partner_id).name : std::string("Πληρωμή");
        OpenPayOut(j);
        return;
    }
    OpenRecordPayment(p.user_id);
    g_rpAmount.set(FormatMoney(left));
    g_rpPreset.clear(); g_rpPreset[p.id] = left;
    if (p.status == "future") g_rpExtra.insert(p.id);
}

static void DrawBillsClients() {
    static std::string from, to; static int userId = -1, payState = 1; static bool showFuture = true;
    static char text[128] = "";
    enum { BC_DATE, BC_WHO, BC_WHAT, BC_DUE, BC_PAID, BC_LEFT, BC_STATE, BC_ACT, BC_COUNT };
    static const ListColDef cols[BC_COUNT] = {
        { "Ημ/νία", ImGuiTableColumnFlags_WidthFixed, 95.0f, true },      { "Πελάτης", ImGuiTableColumnFlags_WidthFixed, 200.0f, true },
        { "Τι χρεώθηκε", ImGuiTableColumnFlags_WidthFixed, 260.0f, true }, { "Ποσό", ImGuiTableColumnFlags_WidthFixed, 95.0f, true },
        { "Πληρώθηκαν", ImGuiTableColumnFlags_WidthFixed, 100.0f, true },  { "Μένουν", ImGuiTableColumnFlags_WidthFixed, 95.0f, true },
        { "Κατάσταση", ImGuiTableColumnFlags_WidthFixed, 130.0f, true },   { "", ImGuiTableColumnFlags_WidthFixed, 150.0f, false } };
    static ListModel<Payment> m;
    m.Init(BC_COUNT, {{BC_DATE, false}}, 20);
    Pager& pager = m.g.pager;
    ImGui::TextDisabled("Οι υπηρεσίες και τα πακέτα που χρεώσαμε σε πελάτες. Μένουν ανοιχτά μέχρι να εξοφληθούν - η είσπραξη μπορεί να είναι και μέρος του ποσού.");
    bool ch = DrawDateRange("bilcl", from, to);
    DrawUserFilterPicker("Πελάτης:", userId, 240); ImGui::SameLine();
    ImGui::SetNextItemWidth(150); ch |= ImGui::Combo("##pay", &payState, "Όλες\0Ανοιχτές (οφείλονται)\0Εξοφλημένες\0"); ImGui::SameLine();
    ch |= ImGui::Checkbox("και προγραμματισμένες", &showFuture); ImGui::SameLine();
    ImGui::SetNextItemWidth(200); ch |= ImGui::InputTextWithHint("##bt", "Αναζήτηση...", text, sizeof(text));
    if (ch) pager.Reset();
    DataConn::PaymentFilter f; f.fromDate = from; f.toDate = to; f.userId = userId > 0 ? userId : -1; f.direction = "in"; f.joint = 2;
    f.status = showFuture ? 0 : 1; f.owed = payState == 1 ? 2 : (payState == 2 ? 1 : 0); f.text = text;
    std::string key = from + "|" + to + "|" + std::to_string(f.userId) + "|" + std::to_string(f.status) + "|" + std::to_string(f.owed) + "|" + text;
    static Cached<DataConn::MoneyTotals> tc;
    const DataConn::MoneyTotals& t = tc.get(key, [&] { return g_app.conn->paymentTotals(f); });
    // every sort / page change fetches exactly that page from the database
    const auto& pg = m.Load(key,
        [&](const SortSpec& s, int off, int lim) {
            DataConn::PaymentFilter q = f;
            q.sort = MapSort(s, {{BC_DATE, 0}, {BC_WHO, 1}, {BC_WHAT, 2}, {BC_DUE, 3}, {BC_PAID, 4}, {BC_LEFT, 5}, {BC_STATE, 6}});
            return g_app.conn->findPayments(q, DataConn::PayOrder::DateDesc, off, lim); },
        [&] { return (int)t.count; });
    ImGui::Text("%d χρεώσεις  ·  ", pg.count); ImGui::SameLine(0, 0);
    ImGui::TextColored(ImVec4(0.55f, 0.80f, 1.0f, 1), "χρεώθηκαν %s", Eur(t.dueAll).c_str()); ImGui::SameLine();
    ImGui::TextColored(kColIn, "· εισπράχθηκαν %s", Eur(t.collected).c_str()); ImGui::SameLine();
    ImGui::TextColored(kColOwed, "· οφείλονται %s", Eur(MoneySub(t.dueAll, t.paidAll)).c_str());
    ImGui::SameLine(); ImGui::TextDisabled("  (κλικ στον τίτλο στήλης: ταξινόμηση · σύρσιμο: μετακίνηση)");
    float h = std::max(160.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.6f);
    if (ImGui::BeginTable("bilcltab", BC_COUNT, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable, ImVec2(0, h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ListSetupColumns(m.g.order, cols);
        ListHeaderRow(m.g.order, cols, m.g.sort, m.g.pager);
        auto P = [&](int c) { return ListPos(m.g.order, c); };
        for (auto& p : pg.rows) {
            ImGui::PushID(p.id); ImGui::TableNextRow();
            bool fut = p.status == "future";
            double left = MoneySub(p.amount_due, p.amount_paid);
            ImGui::TableSetColumnIndex(P(BC_DATE)); ImGui::TextUnformatted(DateGr(p.year, p.month, p.day).c_str());
            ImGui::TableSetColumnIndex(P(BC_WHO)); if (LinkCell(g_app.userName(p.user_id))) OpenUserProfile(p.user_id);
            ImGui::TableSetColumnIndex(P(BC_WHAT)); ImGui::TextUnformatted(ChargeLabel(p).c_str());
            if (!p.notes.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.notes.c_str());
            ImGui::TableSetColumnIndex(P(BC_DUE)); ImGui::TextUnformatted(Eur(p.amount_due).c_str());
            ImGui::TableSetColumnIndex(P(BC_PAID)); if (p.amount_paid > 0.005) ImGui::TextColored(kColIn, "%s", Eur(p.amount_paid).c_str()); else ImGui::TextDisabled("—");
            ImGui::TableSetColumnIndex(P(BC_LEFT)); if (left > 0.005) ImGui::TextColored(kColOwed, "%s", Eur(left).c_str()); else ImGui::TextDisabled("—");
            ImGui::TableSetColumnIndex(P(BC_STATE)); PayBadge(p.amount_due, p.amount_paid, fut);
            ImGui::TableSetColumnIndex(P(BC_ACT));
            if (left > 0.005) { if (ImGui::SmallButton("Είσπραξη")) OpenRecordPaymentFor(p); if (ImGui::IsItemHovered()) ImGui::SetTooltip("Καταχώριση πληρωμής (όλο ή μέρος) με απόδειξη"); ImGui::SameLine(); }
            if (ImGui::SmallButton("Προβολή")) OpenPaymentView(p.id);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    DrawPager(m.g.pager, "bilclpg");
}

static void DrawBillsBuildings() {
    static int building = -1; static Pager pager; static bool openOnly = false;
    ImGui::TextDisabled("Μία χρέωση στην πολυκατοικία (κοινόχρηστο), μοιρασμένη σε έναν λογαριασμό ανά ένοικο. Κάθε ένοικος πληρώνει και παίρνει απόδειξη χωριστά.");
    bool ch = false;
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Πολυκατοικία:"); ImGui::SameLine();
    ch |= PickBuilding("bilbd", building, 300, "Όλες");
    ImGui::SameLine(); ch |= ImGui::Checkbox("Μόνο όσες δεν εισπράχθηκαν όλες", &openOnly);
    ImGui::SameLine(); if (ImGui::Button("+ Χρέωση πολυκατοικίας")) OpenJointCharge(building > 0 ? building : -1);
    ImGui::SameLine(); if (ImGui::Button("Διαχείριση Κοινοχρήστων")) OpenGenericJointCharge(building > 0 ? building : -1);
    if (ch) pager.Reset();
    struct Row { JointOp op; double cost = 0; };
    struct Pg { std::vector<Row> rows; int count = 0; };
    static Cached<Pg> pc;
    const Pg& pg = pc.get(std::to_string(building) + "|" + std::to_string(openOnly) + "|" + std::to_string(pager.offset) + "|" + std::to_string(pager.pageSize), [&] {
        Pg p; int bid = building > 0 ? building : -1;
        if (!openOnly) {
            p.count = g_app.conn->countJointOps(bid);
            for (auto& o : g_app.conn->getJointOps(bid, pager.offset, pager.pageSize)) p.rows.push_back({o, g_app.conn->getJointOpCost(o.id)});
        } else {                                       // filter in memory (a building has few charges)
            std::vector<JointOp> all = g_app.conn->getJointOps(bid, 0, 100000);
            std::vector<JointOp> open; for (auto& o : all) if (ToCents(o.paid) < ToCents(o.total)) open.push_back(o);
            p.count = (int)open.size();
            for (int i = pager.offset; i < (int)open.size() && i < pager.offset + pager.pageSize; i++) p.rows.push_back({open[i], g_app.conn->getJointOpCost(open[i].id)});
        }
        return p; });
    pager.total = pg.count;
    float h = std::max(160.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.6f);
    if (ImGui::BeginTable("bilbdtab", 9, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp, ImVec2(0, h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Ημ/νία", ImGuiTableColumnFlags_WidthFixed, 90); ImGui::TableSetupColumn("Πολυκατοικία", 0, 1.6f);
        ImGui::TableSetupColumn("Τι χρεώθηκε", 0, 1.8f); ImGui::TableSetupColumn("Σύνολο", ImGuiTableColumnFlags_WidthFixed, 85);
        ImGui::TableSetupColumn("Από ενοίκους", ImGuiTableColumnFlags_WidthFixed, 95); ImGui::TableSetupColumn("Μένουν", ImGuiTableColumnFlags_WidthFixed, 85);
        ImGui::TableSetupColumn("Κόστος μας", ImGuiTableColumnFlags_WidthFixed, 90); ImGui::TableSetupColumn("Κατάσταση", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableHeadersRow();
        for (auto& r : pg.rows) {
            const JointOp& o = r.op;
            double left = MoneySub(o.total, o.paid);
            ImGui::PushID(o.id); ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(DateGr(o.op_date).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(o.building_address.empty() ? "(επιλεγμένα διαμερίσματα)" : o.building_address.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(o.label.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%d διαμερίσματα χρεώθηκαν%s", o.included, o.excluded ? (", " + std::to_string(o.excluded) + " εξαιρέθηκαν").c_str() : "");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(Eur(o.total).c_str());
            ImGui::TableNextColumn(); if (o.paid > 0.005) ImGui::TextColored(kColIn, "%s", Eur(o.paid).c_str()); else ImGui::TextDisabled("—");
            ImGui::TableNextColumn(); if (left > 0.005) ImGui::TextColored(kColOwed, "%s", Eur(left).c_str()); else ImGui::TextDisabled("—");
            ImGui::TableNextColumn(); if (r.cost > 0.005) ImGui::TextColored(kColOut, "%s", Eur(r.cost).c_str()); else ImGui::TextDisabled("—");
            ImGui::TableNextColumn(); PayBadge(o.total, o.paid);
            ImGui::TableNextColumn(); if (ImGui::SmallButton("Άνοιγμα")) OpenJointView(o.id);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Τι χρεώθηκε κάθε ένοικος, τι πλήρωσε, είσπραξη ανά ένοικο");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    DrawPager(pager, "bilbdpg");
}

static void DrawBillsResidents() {
    ImGui::TextDisabled("Κάθε ένοικος με τα μερίδια κοινοχρήστων που χρωστάει. Κλικ στο όνομα: τα μερίδια · «Είσπραξη»: πληρωμή (όλο ή μέρος) με απόδειξη.");
    DrawHomeOwings("bilrs", true, std::max(160.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 2.6f));
}

static void DrawBillsPartners() {
    static std::string from, to; static int partner = 0; static bool owedOnly = false;
    enum { BP_DATE, BP_PARTNER, BP_WHAT, BP_FOR, BP_COST, BP_PAID, BP_OWE, BP_STATE, BP_ACT, BP_COUNT };
    static const ListColDef cols[BP_COUNT] = {
        { "Ημ/νία", ImGuiTableColumnFlags_WidthFixed, 95.0f, true },       { "Συνεργάτης", ImGuiTableColumnFlags_WidthFixed, 180.0f, true },
        { "Τι", ImGuiTableColumnFlags_WidthFixed, 200.0f, true },           { "Για (πελάτη / πολυκατοικία)", ImGuiTableColumnFlags_WidthFixed, 230.0f, true },
        { "Κόστος", ImGuiTableColumnFlags_WidthFixed, 95.0f, true },        { "Πληρώσαμε", ImGuiTableColumnFlags_WidthFixed, 100.0f, true },
        { "Χρωστάμε", ImGuiTableColumnFlags_WidthFixed, 95.0f, true },      { "Κατάσταση", ImGuiTableColumnFlags_WidthFixed, 110.0f, false },
        { "", ImGuiTableColumnFlags_WidthFixed, 150.0f, false } };
    static ListModel<DataConn::PartnerJob> m;
    m.Init(BP_COUNT, {{BP_DATE, false}}, 20);
    Pager& pager = m.g.pager;
    ImGui::TextDisabled("Τα κόστη μας: τι χρωστάμε σε κάθε συνεργάτη για κάθε δουλειά. Ένα πακέτο μπορεί να έχει κόστη σε διαφορετικούς συνεργάτες.");
    bool ch = DrawDateRange("bilout", from, to);
    static Cached<std::vector<DataConn::PartnerStat>> ps;
    const auto& parts = ps.get("", [] { return g_app.conn->partnerStats("", ""); });
    std::string cur = "Όλοι οι συνεργάτες";
    for (auto& s : parts) if (s.partner_id == partner && partner != 0) cur = s.name;
    ImGui::SetNextItemWidth(240);
    if (ImGui::BeginCombo("##partner", cur.c_str())) {
        if (ImGui::Selectable("Όλοι οι συνεργάτες", partner == 0)) { partner = 0; ch = true; }
        for (auto& s : parts) { ImGui::PushID(s.partner_id); if (ImGui::Selectable(s.name.c_str(), partner == s.partner_id)) { partner = s.partner_id; ch = true; } ImGui::PopID(); }
        ImGui::EndCombo();
    }
    ImGui::SameLine(); ch |= ImGui::Checkbox("Μόνο όσα χρωστάμε", &owedOnly);
    if (ch) pager.Reset();
    std::string key = from + "|" + to + "|" + std::to_string(partner) + "|" + std::to_string(owedOnly);
    const auto& pg = m.Load(key,
        [&](const SortSpec& s, int off, int lim) {
            return g_app.conn->partnerJobs(partner, from, to, MapSort(s, {{BP_DATE, 0}, {BP_PARTNER, 1}, {BP_WHAT, 2}, {BP_FOR, 3}, {BP_COST, 4}, {BP_PAID, 5}, {BP_OWE, 6}}),
                                           off, lim, 0, owedOnly); },
        [&] { return g_app.conn->countPartnerJobs(partner, from, to, 0, owedOnly); });
    ImGui::Text("%d κόστη", pg.count); ImGui::SameLine();
    ImGui::TextColored(kColWeOwe, "· χρωστάμε συνολικά %s", Eur(g_app.conn->cashNow().owedOut).c_str());
    ImGui::SameLine(); ImGui::TextDisabled("  (κλικ στον τίτλο στήλης: ταξινόμηση · σύρσιμο: μετακίνηση)");
    float h = std::max(160.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.6f);
    if (ImGui::BeginTable("bilouttab", BP_COUNT, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_Resizable, ImVec2(0, h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ListSetupColumns(m.g.order, cols);
        ListHeaderRow(m.g.order, cols, m.g.sort, m.g.pager);
        auto P = [&](int c) { return ListPos(m.g.order, c); };
        for (auto& j : pg.rows) {
            ImGui::PushID(j.charge_id); ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(P(BP_DATE)); ImGui::TextUnformatted(DateGr(j.date).c_str());
            ImGui::TableSetColumnIndex(P(BP_PARTNER)); if (j.partner_id > 0) { if (LinkCell(j.partner)) OpenPartnerProfile(j.partner_id); } else ImGui::TextDisabled("χωρίς συνεργάτη");
            ImGui::TableSetColumnIndex(P(BP_WHAT)); ImGui::TextUnformatted(j.service.c_str());
            ImGui::TableSetColumnIndex(P(BP_FOR)); ImGui::TextUnformatted((j.jointLabel.empty() ? j.client : j.jointLabel).c_str());
            ImGui::TableSetColumnIndex(P(BP_COST)); ImGui::TextUnformatted(Eur(j.amount).c_str());
            ImGui::TableSetColumnIndex(P(BP_PAID)); if (j.paid > 0.005) ImGui::TextColored(kColOut, "%s", Eur(j.paid).c_str()); else ImGui::TextDisabled("—");
            ImGui::TableSetColumnIndex(P(BP_OWE)); if (j.balance > 0.005) ImGui::TextColored(kColWeOwe, "%s", Eur(j.balance).c_str()); else ImGui::TextDisabled("—");
            ImGui::TableSetColumnIndex(P(BP_STATE)); PayBadge(j.amount, j.paid);
            ImGui::TableSetColumnIndex(P(BP_ACT));
            if (j.balance > 0.005) { if (ImGui::SmallButton("Πληρωμή")) OpenPayOut(j); if (ImGui::IsItemHovered()) ImGui::SetTooltip("Πληρώνουμε όλο ή μέρος"); ImGui::SameLine(); }
            if (ImGui::SmallButton("Προβολή")) OpenPaymentView(j.charge_id);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    DrawPager(m.g.pager, "biloutpg");
}

static void DrawBillsScreen() {
    ImGui::SetWindowFontScale(1.2f); ImGui::TextUnformatted("Χρεώσεις"); ImGui::SetWindowFontScale(1.0f);
    ImGui::SameLine(); ImGui::TextDisabled("  χρεώσαμε → οφείλεται → εισπράττουμε (όλο ή μέρος, κάθε φορά με απόδειξη) → εξοφλείται");
    if (ImGui::BeginTabBar("billtabs")) {
        if (ImGui::BeginTabItem("Πελάτες")) { DrawBillsClients(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Πολυκατοικίες (κοινόχρηστα)")) { DrawBillsBuildings(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Ένοικοι")) { DrawBillsResidents(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Συνεργάτες (κόστη μας)")) { DrawBillsPartners(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
}

// ---- Ταμείο ----
static int g_cashReceiptMoney = -1, g_cashReceiptUser = -1; static double g_cashReceiptAmount = 0; static ReceiptForm g_cashReceiptForm;
static bool g_cashReceiptOpenNext = false;

static void DrawCashScreen() {
    ImGui::SetWindowFontScale(1.2f); ImGui::TextUnformatted("Ταμείο"); ImGui::SetWindowFontScale(1.0f);
    ImGui::SameLine(); ImGui::TextDisabled("  κάθε χρήμα που μπήκε ή βγήκε (εισπράξεις πελατών και ενοίκων, πληρωμές συνεργατών)");
    static DataConn::MoneyQuery q; static bool inited = false; static int dir = 0, method = 0, rc = 0; static char text[128] = ""; static Pager pager;
    if (!inited) {
        std::string t = CurrentDateString(); q.fromDate = t.substr(0, 8) + "01"; inited = true;
    }
    bool ch = DrawDateRange("cash", q.fromDate, q.toDate);
    ImGui::SetNextItemWidth(150); ch |= ImGui::Combo("##dir", &dir, "Μέσα και έξω\0Μόνο εισπράξεις\0Μόνο πληρωμές\0"); ImGui::SameLine();
    {
        std::string cur = method == 0 ? std::string("Κάθε τρόπος") : kReceiptMethodNames[method - 1];
        ImGui::SetNextItemWidth(170);
        if (ImGui::BeginCombo("##meth", cur.c_str())) {
            if (ImGui::Selectable("Κάθε τρόπος", method == 0)) { method = 0; ch = true; }
            for (int i = 0; i < IM_ARRAYSIZE(kReceiptMethodNames); i++) if (ImGui::Selectable(kReceiptMethodNames[i], method == i + 1)) { method = i + 1; ch = true; }
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine(); ImGui::SetNextItemWidth(200); ch |= ImGui::Combo("##rc", &rc, "Με ή χωρίς απόδειξη\0Με απόδειξη\0Εισπράξεις χωρίς απόδειξη\0");
    ImGui::SameLine(); ImGui::SetNextItemWidth(200); ch |= ImGui::InputTextWithHint("##ct", "Όνομα, αναφορά...", text, sizeof(text));
    ImGui::SameLine(); if (ImGui::Button("+ Είσπραξη")) OpenRecordPayment(-1);
    ImGui::SameLine(); if (ImGui::Button("Πληρωμή συνεργάτη")) g_showOwed = true;
    q.direction = dir == 1 ? "in" : (dir == 2 ? "out" : ""); q.method = method == 0 ? "" : kReceiptMethodCodes[method - 1]; q.receipt = rc; q.text = text;
    if (ch) pager.Reset();
    struct Pg { std::vector<MoneyPayment> rows; long long count = 0; double in = 0, out = 0; };
    static Cached<Pg> pc;
    std::string key = q.fromDate + "|" + q.toDate + "|" + q.direction + "|" + q.method + "|" + std::to_string(q.receipt) + "|" + q.text + "|" +
                      std::to_string(pager.offset) + "|" + std::to_string(pager.pageSize);
    const Pg& pg = pc.get(key, [&] { Pg p; p.count = g_app.conn->countMoney(q, &p.in, &p.out); p.rows = g_app.conn->pageMoney(q, pager.offset, pager.pageSize); return p; });
    pager.total = (int)pg.count;

    if (ImGui::BeginTable("cashtiles", 3, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        double net = MoneySub(pg.in, pg.out);
        ImGui::TableNextColumn(); StatTile("Μπήκαν", Eur(pg.in), kColIn, "εισπράξεις", ImVec2(ImGui::GetContentRegionAvail().x, 86));
        ImGui::TableNextColumn(); StatTile("Βγήκαν", Eur(pg.out), kColOut, "πληρωμές", ImVec2(ImGui::GetContentRegionAvail().x, 86));
        ImGui::TableNextColumn(); StatTile("Κίνηση ταμείου (μπήκαν − βγήκαν)", Eur(net), net >= 0 ? kColIn : kColOut, (std::to_string(pg.count) + " κινήσεις").c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 86));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Χρήματα που κινήθηκαν στο διάστημα, με ημερομηνία πληρωμής: και κοινόχρηστα,\nκαι πληρωμές συνεργατών για παλαιότερες δουλειές. Αρνητικό = βγήκαν περισσότερα από όσα μπήκαν.\nΔεν είναι το κέρδος - αυτό είναι στην Αρχική («Κέρδος από δουλειές»).");
        ImGui::EndTable();
    }
    float h = std::max(160.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.6f);
    if (ImGui::BeginTable("cashtab", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp, ImVec2(0, h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Ημ/νία", ImGuiTableColumnFlags_WidthFixed, 90); ImGui::TableSetupColumn("Κίνηση", ImGuiTableColumnFlags_WidthFixed, 95);
        ImGui::TableSetupColumn("Ποιος", 0, 1.6f); ImGui::TableSetupColumn("Για τι", 0, 2.0f);
        ImGui::TableSetupColumn("Τρόπος", ImGuiTableColumnFlags_WidthFixed, 130); ImGui::TableSetupColumn("Ποσό", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("Απόδειξη", 0, 1.3f); ImGui::TableSetupColumn("Αναφορά / σημειώσεις", 0, 1.4f);
        ImGui::TableHeadersRow();
        for (auto& m : pg.rows) {
            bool in = m.direction == "in";
            ImGui::PushID(m.id); ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(DateGr(m.pay_date).c_str());
            ImGui::TableNextColumn(); if (in) ImGui::TextColored(kColIn, "▲ Είσπραξη"); else ImGui::TextColored(kColOut, "▼ Πληρωμή");
            ImGui::TableNextColumn();
            if (in && m.user_id > 0) { if (LinkCell(m.who)) OpenUserProfile(m.user_id); } else ImGui::TextUnformatted(m.who.empty() ? "—" : m.who.c_str());
            ImGui::TableNextColumn();
            if (m.what.empty()) ImGui::TextDisabled(in ? "πίστωση (δεν εξοφλεί χρέωση)" : "—"); else ImGui::TextUnformatted(m.what.c_str());
            if (in && m.amount - m.allocated > 0.005 && !m.what.empty()) { ImGui::SameLine(); ImGui::TextDisabled("(+%s πίστωση)", Eur(m.amount - m.allocated).c_str()); }
            ImGui::TableNextColumn(); ImGui::TextUnformatted(m.method == "unspecified" ? "—" : ReceiptMethodName(m.method).c_str());
            ImGui::TableNextColumn(); ImGui::TextColored(in ? kColIn : kColOut, "%s%s", in ? "+" : "−", Eur(m.amount).c_str());
            ImGui::TableNextColumn();
            if (m.receipt_id > 0) {
                static std::map<int, Receipt> rcache; static long long ver = -1;
                if (ver != g_app.conn->dataVersion()) { rcache.clear(); ver = g_app.conn->dataVersion(); }
                auto it = rcache.find(m.receipt_id);
                if (it == rcache.end()) it = rcache.emplace(m.receipt_id, g_app.conn->getReceiptById(m.receipt_id)).first;
                bool ok = it->second.status == "issued";
                if (LinkCell(ReceiptNo(it->second) + (ok ? "" : " (ακυρ.)"))) OpenReceiptView(m.receipt_id);
                if (!ok && in) { ImGui::SameLine(); if (ImGui::SmallButton("Νέα")) { g_cashReceiptMoney = m.id; g_cashReceiptUser = m.user_id; g_cashReceiptAmount = m.amount; g_cashReceiptForm = ReceiptForm{}; g_cashReceiptOpenNext = true; } }
            } else if (in) {
                ImGui::TextColored(kColOwed, "⚠"); ImGui::SameLine();
                if (ImGui::SmallButton("Έκδοση απόδειξης")) { g_cashReceiptMoney = m.id; g_cashReceiptUser = m.user_id; g_cashReceiptAmount = m.amount; g_cashReceiptForm = ReceiptForm{}; g_cashReceiptOpenNext = true; }
            } else ImGui::TextDisabled("—");
            ImGui::TableNextColumn();
            std::string ref = m.reference + (m.reference.empty() || m.notes.empty() ? "" : " · ") + m.notes;
            ImGui::TextDisabled("%s", ref.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    DrawPager(pager, "cashpg");

    if (g_cashReceiptOpenNext) { ImGui::OpenPopup("Απόδειξη είσπραξης"); g_cashReceiptOpenNext = false; }
    if (ImGui::BeginPopupModal("Απόδειξη είσπραξης", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        static std::string err;
        bool ok = DrawReceiptForm("cashrc", g_cashReceiptForm, g_cashReceiptAmount, 1);
        if (!err.empty()) ImGui::TextColored(kColOut, "%s", err.c_str());
        ImGui::BeginDisabled(!ok || !g_cashReceiptForm.issue);
        if (ImGui::Button("Έκδοση", ImVec2(150, 0))) {
            int id = g_app.conn->issueReceiptForMoney(ReceiptFromForm(g_cashReceiptForm, g_cashReceiptUser), {g_cashReceiptMoney});
            if (id > 0) { err.clear(); ImGui::CloseCurrentPopup(); OpenReceiptView(id); } else err = g_app.conn->takeLastError();
        }
        ImGui::EndDisabled();
        ImGui::SameLine(); if (ImGui::Button("Άκυρο", ImVec2(120, 0))) { err.clear(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

int RunApp(DataConn* conn, const std::string& dbPath) {
    g_app.conn = conn;
    g_app.dbPath = dbPath;
    g_app.reloadAll();
    PrefsLoad((std::filesystem::path(dbPath).parent_path() / "ui_prefs.ini").string());

    glfwSetErrorCallback(GlfwErrorCallback);
    if (!glfwInit()) return 1;

    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    GLFWwindow* window = glfwCreateWindow(1400, 900, "PropertyManager", nullptr, nullptr);
    g_glfwWindow = window;
    if (!window) return 1;
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();
    // A considered pass on top of the raw defaults: warmer/softer dark palette,
    // rounded corners instead of hard rectangles, slightly more breathing room.
    {
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 10.0f;
        style.ChildRounding = 8.0f;
        style.FrameRounding = 6.0f;
        style.PopupRounding = 7.0f;
        style.ScrollbarRounding = 8.0f;
        style.GrabRounding = 4.0f;
        style.TabRounding = 7.0f;
        style.ScrollbarSize = 13.0f;
        style.GrabMinSize = 11.0f;
        style.TabBarBorderSize = 1.0f;
        style.SeparatorTextBorderSize = 2.0f;
        style.WindowTitleAlign = ImVec2(0.02f, 0.5f);
        style.WindowBorderSize = 1.0f;
        style.FrameBorderSize = 0.0f;
        style.WindowPadding = ImVec2(10, 10);
        style.FramePadding = ImVec2(10, 6);
        style.ItemSpacing = ImVec2(10, 8);
        style.ItemInnerSpacing = ImVec2(6, 6);
        style.CellPadding = ImVec2(8, 5);
        style.IndentSpacing = 18.0f;

        ImVec4* colors = style.Colors;
        colors[ImGuiCol_WindowBg]           = ImVec4(0.098f, 0.102f, 0.125f, 1.00f);
        colors[ImGuiCol_ChildBg]            = ImVec4(0.118f, 0.124f, 0.153f, 0.70f);
        colors[ImGuiCol_PopupBg]            = ImVec4(0.106f, 0.110f, 0.133f, 0.98f);
        colors[ImGuiCol_Border]             = ImVec4(0.27f, 0.29f, 0.36f, 0.45f);
        colors[ImGuiCol_FrameBg]            = ImVec4(0.16f, 0.17f, 0.21f, 1.00f);
        colors[ImGuiCol_FrameBgHovered]     = ImVec4(0.21f, 0.23f, 0.29f, 1.00f);
        colors[ImGuiCol_FrameBgActive]      = ImVec4(0.24f, 0.27f, 0.35f, 1.00f);
        colors[ImGuiCol_TitleBg]            = ImVec4(0.10f, 0.10f, 0.13f, 1.00f);
        colors[ImGuiCol_TitleBgActive]      = ImVec4(0.16f, 0.30f, 0.42f, 1.00f);
        colors[ImGuiCol_TitleBgCollapsed]   = ImVec4(0.09f, 0.09f, 0.11f, 1.00f);
        colors[ImGuiCol_MenuBarBg]          = ImVec4(0.12f, 0.12f, 0.15f, 1.00f);
        colors[ImGuiCol_ScrollbarBg]        = ImVec4(0.09f, 0.09f, 0.11f, 0.60f);
        colors[ImGuiCol_ScrollbarGrab]      = ImVec4(0.30f, 0.32f, 0.40f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.38f, 0.41f, 0.51f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.24f, 0.55f, 0.75f, 1.00f);
        colors[ImGuiCol_CheckMark]          = ImVec4(0.30f, 0.72f, 0.85f, 1.00f);
        colors[ImGuiCol_SliderGrab]         = ImVec4(0.28f, 0.62f, 0.80f, 1.00f);
        colors[ImGuiCol_SliderGrabActive]   = ImVec4(0.32f, 0.70f, 0.90f, 1.00f);
        colors[ImGuiCol_Button]             = ImVec4(0.19f, 0.34f, 0.44f, 0.85f);
        colors[ImGuiCol_ButtonHovered]      = ImVec4(0.24f, 0.45f, 0.58f, 0.95f);
        colors[ImGuiCol_ButtonActive]       = ImVec4(0.20f, 0.55f, 0.70f, 1.00f);
        colors[ImGuiCol_Header]             = ImVec4(0.21f, 0.33f, 0.42f, 0.75f);
        colors[ImGuiCol_HeaderHovered]      = ImVec4(0.26f, 0.42f, 0.54f, 0.90f);
        colors[ImGuiCol_HeaderActive]       = ImVec4(0.24f, 0.48f, 0.62f, 1.00f);
        colors[ImGuiCol_Separator]          = ImVec4(0.26f, 0.27f, 0.33f, 0.65f);
        colors[ImGuiCol_SeparatorHovered]   = ImVec4(0.30f, 0.55f, 0.70f, 0.80f);
        colors[ImGuiCol_SeparatorActive]    = ImVec4(0.32f, 0.62f, 0.80f, 1.00f);
        colors[ImGuiCol_ResizeGrip]         = ImVec4(0.30f, 0.55f, 0.70f, 0.35f);
        colors[ImGuiCol_ResizeGripHovered]  = ImVec4(0.30f, 0.55f, 0.70f, 0.70f);
        colors[ImGuiCol_ResizeGripActive]   = ImVec4(0.32f, 0.62f, 0.80f, 0.95f);
        colors[ImGuiCol_Tab]                = ImVec4(0.13f, 0.15f, 0.19f, 1.00f);
        colors[ImGuiCol_TabHovered]         = ImVec4(0.24f, 0.42f, 0.54f, 0.90f);
        colors[ImGuiCol_TabActive]          = ImVec4(0.17f, 0.42f, 0.58f, 1.00f);
        colors[ImGuiCol_TabUnfocused]       = ImVec4(0.11f, 0.12f, 0.15f, 1.00f);
        colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.15f, 0.24f, 0.31f, 1.00f);
        colors[ImGuiCol_PlotLines]          = ImVec4(0.55f, 0.75f, 1.0f, 1.00f);
        colors[ImGuiCol_PlotHistogram]      = ImVec4(0.35f, 0.85f, 0.45f, 1.00f);
        colors[ImGuiCol_TableHeaderBg]      = ImVec4(0.15f, 0.20f, 0.27f, 1.00f);
        colors[ImGuiCol_TableBorderStrong]  = ImVec4(0.28f, 0.29f, 0.35f, 1.00f);
        colors[ImGuiCol_TableBorderLight]   = ImVec4(0.22f, 0.23f, 0.28f, 1.00f);
        colors[ImGuiCol_TableRowBg]         = ImVec4(1, 1, 1, 0.00f);
        colors[ImGuiCol_TableRowBgAlt]      = ImVec4(1, 1, 1, 0.035f);
        colors[ImGuiCol_Text]               = ImVec4(0.90f, 0.92f, 0.96f, 1.00f);
        colors[ImGuiCol_TextDisabled]       = ImVec4(0.52f, 0.56f, 0.64f, 1.00f);
        colors[ImGuiCol_TabSelectedOverline] = ImVec4(0.45f, 0.88f, 1.00f, 1.00f);
        colors[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.30f, 0.72f, 0.85f, 0.50f);
        colors[ImGuiCol_NavHighlight]       = ImVec4(0.30f, 0.72f, 0.85f, 0.80f);
        colors[ImGuiCol_TextSelectedBg]     = ImVec4(0.24f, 0.55f, 0.75f, 0.35f);
    }

    // The built-in ImGui font is ASCII-only, so Greek text (the app's own UI
    // labels, and any user/service/payment data typed in Greek) renders as
    // "?????". Load a system font with Greek glyph coverage instead. Segoe UI
    // ships on every Windows install and covers Greek + Latin; fall back to
    // the default font if it's somehow missing rather than failing to start.
    // NOTE: true color emoji (e.g. a bell glyph) is NOT covered by this --
    // ImGui's stb_truetype backend doesn't render color glyphs, only single-
    // color outlines, and Segoe UI Emoji is a color-only font. Any emoji used
    // in UI labels should be swapped for plain ASCII/text (e.g. "[!]" instead
    // of a bell icon) rather than relying on a font fix here.
    // 0x20A0-0x20CF = Currency Symbols block (includes the Euro sign, U+20AC) --
    // missing this was why every "€" rendered as "?" throughout the app.
    // This font-range list has repeatedly been the source of "?" glyphs whenever any
    // UI text used a symbol outside plain Latin/Greek -- Euro sign (Currency Symbols),
    // then arrow characters (Arrows block, used in direction badges), then triangles
    // (Geometric Shapes, used in the future-payments divider). Rather than keep
    // patching one symbol at a time, this now covers every block any UI text in this
    // app actually draws from.
    static const ImWchar greekRanges[] = {
        0x0020, 0x00FF, // Latin
        0x0370, 0x03FF, // Greek
        0x1F00, 0x1FFF, // Greek Extended
        0x2010, 0x2027, // General Punctuation (en/em dash, bullet, etc.)
        0x20A0, 0x20CF, // Currency Symbols (Euro sign)
        0x2190, 0x21FF, // Arrows
        0x25A0, 0x25FF, // Geometric Shapes (triangles used in dividers)
        0
    };
    ImFontConfig fontCfg;
    fontCfg.OversampleH = 2;
    fontCfg.OversampleV = 2;
    // First font file that exists wins (ImGui asserts on a missing file, so probe first).
    static const char* kFontCandidates[] = {
        "C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
    };
    ImFont* greekFont = nullptr;
    for (const char* f : kFontCandidates) {
        if (!std::ifstream(f).good()) continue;
        greekFont = io.Fonts->AddFontFromFileTTF(f, 18.0f, &fontCfg, greekRanges);
        if (greekFont) break;
    }
    if (greekFont) io.FontDefault = greekFont;

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    glfwSetDropCallback(window, OnFilesDropped);      // drag & drop files onto «Συνημμένα» panels
    ImGui_ImplOpenGL3_Init(glsl_version);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("PropertyManagerMain", nullptr,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                      ImGuiWindowFlags_NoBringToFrontOnFocus);   // the left navigation always stays on top of the page

        // Left navigation: a thin icon bar that widens over the content while the mouse is on it.
        if (g_tabToApartments) { g_page = PAGE_BUILDINGS; g_tabToApartments = false; }
        ImGui::SetCursorPos(ImVec2(kNavCollapsed + 8.0f, ImGui::GetCursorPosY()));
        ImGui::BeginChild("pagecontent", ImVec2(0, 0), false);
        switch (g_page) {
            case PAGE_HOME:       DrawHomeScreen(); break;
            case PAGE_CLIENTS:    DrawUsersScreen(); break;
            case PAGE_BUILDINGS:
            case PAGE_APARTMENTS: DrawApartmentsScreen(); break;      // one page: buildings -> apartments -> residents
            case PAGE_SERVICES:   DrawServicesScreen(); break;
            case PAGE_JOINT:      DrawJointServicesScreen(); break;
            case PAGE_PACKAGES:   DrawMasterServicesScreen(); break;
            case PAGE_PAYMENTS:   DrawPaymentsScreen(); break;
            case PAGE_BILLS:      DrawBillsScreen(); break;
            case PAGE_CASH:       DrawCashScreen(); break;
            case PAGE_RECEIPTS:
                if (ImGui::BeginTabBar("rcmain")) {
                    if (ImGui::BeginTabItem("Αποδείξεις είσπραξης")) {
                        if (ImGui::Button("+ Νέα είσπραξη")) OpenRecordPayment(-1);
                        ImGui::SameLine();
                        DrawReceiptsScreen();
                        ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Πληρωμές μας (συνεργάτες · #-1)")) {
                        static std::string pf, pt; static ListModel<DataConn::PartnerJob> pm;
                        ImGui::TextDisabled("Ό,τι πληρώσαμε εμείς: σε συνεργάτες, ή χωρίς καταχωρημένο συνεργάτη (#-1). «Άνοιγμα» δείχνει τη χρέωση και τις πληρωμές της.");
                        if (DrawDateRange("rcpaydates", pf, pt)) pm.g.pager.Reset();
                        DrawPartnerJobsModel("rcpay", 0, 0, pf, pt, pm, true, std::max(160.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 2.4f));
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
                break;
            case PAGE_OFFERS:     DrawOffersScreen(); break;
            case PAGE_SEARCH:     DrawSearchScreen(); break;
            case PAGE_NOTES:      DrawNotesScreen(); break;
            case PAGE_PARTNERS:   DrawPartnersScreen(); break;
            case PAGE_SETTINGS:   DrawSettingsScreen(); break;
            case PAGE_DASHBOARD:
                if (ImGui::Button("◄ Πίσω στην Αρχική")) g_page = PAGE_HOME;
                ImGui::SameLine(); ImGui::TextDisabled("Αναλυτικά σύνολα");
                DrawDashboardScreen(); break;
            default: break;
        }
        ImGui::EndChild();
        // Each of these is driven by a global flag/id (g_show*Dialog / g_*Id) and can be
        // triggered from several different screens -- BUG THAT WAS HERE: they used to
        // also be called individually from inside those triggering screens (e.g.
        // DrawPaymentDialog() from both DrawPaymentsScreen AND DrawServiceDetailWindow),
        // so if two such screens were both "live" in the same frame (a detail window
        // open while its tab is also active), ImGui::OpenPopup/BeginPopupModal got
        // called TWICE with the same string ID in one frame -- a known ImGui footgun
        // that corrupts the shared popup-ID stack for the rest of the session (matches
        // the "close one window and then NOTHING reopens until restart" report exactly).
        // Fix: call each of these exactly once per frame, only here.
        DrawConfirmModal();
        DrawUserProfileWindow();
        DrawServiceDetailWindow();
        DrawPackageViewWindow();
        DrawResidentLedgerWindow();
        DrawErrorBanner();
        PrefsSaveIfChanged();
        DrawPaymentViewWindow();
        DrawJointViewWindow();
        DrawJointChargeDialog();
        DrawClientChargeDialog();
        DrawServiceDialog();
        DrawPaymentDialog();
        DrawPayOutDialog();
        DrawOwedWindow();
        DrawPackageChargeDialog();
        DrawResolveDialog();
        DrawReceiptViewWindow();
        DrawRecordPaymentDialog();
        DrawAttachmentViewer();
        DrawPayOwnerPickPopup();
        DrawNodeGraphOverlay();
        DrawAuditLogWindow();
        DrawErrorLogWindow();
        DrawOfferDialog();
        ImGui::End();
        DrawNavBar();

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.08f, 0.08f, 0.10f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
