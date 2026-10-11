// ImGui/GLFW/OpenGL3 UI shell for PropertyManager, replacing the old FLTK UI.
// Single translation unit by design (small app, keeps the FLTK->ImGui swap easy to review).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "App.h"
#include "DataConn.h"
#include "SqliteConn.h"
#include "models/User.h"
#include "models/Service.h"
#include "models/MasterService.h"
#include "models/Payment.h"
#include "models/Note.h"
#include "Config.h"
#include "Seed.h"
#include "imgui.h"
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
#include <cstdio>
#include <cstring>
#include <functional>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <ctime>


#include "DatePicker.h"  


// ---------------------------------------------------------------------------
// Small shared helpers
// ---------------------------------------------------------------------------
static void GlfwErrorCallback(int error, const char* description) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
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
// Global app state
// ---------------------------------------------------------------------------
struct AppState {
    DataConn* conn = nullptr;
    std::string dbPath;

    // cached lists (reloaded on demand)
    std::vector<User> users;
    std::vector<Service> services;
    std::vector<Payment> payments;
    std::vector<DataConn::JointServiceSummary> jointServices;
    std::vector<Note> notes;

    void reloadUsers() { users = conn->searchUsers("", 0, 5000); }
    void reloadServices() { services = conn->searchServices("", 0, 5000); }
    void reloadPayments() { payments = conn->getAllPayments(); }
    void reloadJoint() { jointServices = conn->getJointServices(); }
    void reloadNotes() { notes = conn->getAllNotes(); }
    void reloadAll() { reloadUsers(); reloadServices(); reloadPayments(); reloadJoint(); reloadNotes(); }

    std::string userName(int id) const {
        for (auto& u : users) if (u.id == id) return u.full_name;
        return "#" + std::to_string(id);
    }
    std::string serviceLabel(int id) const {
        for (auto& s : services) if (s.id == id) return s.label;
        return "#" + std::to_string(id);
    }
};
static AppState g_app;

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
    static const char* kSizes[] = { "20", "50", "100", "200" };
    static const int   kSizeVals[] = { 20, 50, 100, 200 };
    int curIdx = 1;
    for (int i = 0; i < 4; i++) if (kSizeVals[i] == p.pageSize) curIdx = i;
    if (ImGui::Combo("##pagesize", &curIdx, kSizes, 4)) {
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
static void SortToggle(ColSort& st, int col, bool asc) {
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

static void ListSetupColumns(const std::vector<int>& order, const ListColDef* defs) {
    for (int l : order)
        ImGui::TableSetupColumn(defs[l].name, defs[l].flags | ImGuiTableColumnFlags_NoReorder | ImGuiTableColumnFlags_NoSort, defs[l].w);
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

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(1, 0));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        const float bw = ImGui::CalcTextSize("▲").x + 4.0f;
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
        const float w = ImGui::GetContentRegionAvail().x;
        if (btn("◄##l", canL, false)) { swapA = d; swapB = d - 1; }
        ImGui::SameLine();
        int ki = -1;
        for (size_t q = 0; q < sort.keys.size(); q++) if (sort.keys[q].first == l) ki = (int)q;
        const bool sorted = ki >= 0;
        const bool sAsc = sorted && sort.keys[ki].second;
        const bool sDesc = sorted && !sort.keys[ki].second;
        ImGui::PushStyleColor(ImGuiCol_Text, sorted ? accent : ImGui::GetStyleColorVec4(ImGuiCol_Text));
        { float room = w - bw * 4.0f - 10.0f; if (room > 14.0f) ImGui::TextUnformatted(TruncToWidth(std::string(def.name) + ((sorted && sort.keys.size() > 1) ? " " + std::to_string(ki + 1) : std::string()), room).c_str()); }
        ImGui::PopStyleColor();
        ImGui::SameLine(w - bw * 3.0f - 2.0f);   // offset is relative to the cell start inside a table
        if (btn("▲##a", true, sAsc))  { SortToggle(sort, l, true);  pager.Reset(); }
        ImGui::SameLine();
        if (btn("▼##d", true, sDesc)) { SortToggle(sort, l, false); pager.Reset(); }
        ImGui::SameLine();
        if (btn("►##r", canR, false)) { swapA = d; swapB = d + 1; }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        ImGui::PopID();
    }
    if (swapA >= 0) std::swap(order[swapA], order[swapB]);
}

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
struct AptAgg {
    Apartment apt;
    std::vector<int> userIndices;    // residents, indices into g_app.users
    double collected = 0.0, paidOut = 0.0, owedIn = 0.0, owedOut = 0.0;
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

static bool g_showVacantApts = false;   // default: only apartments with active clients

static std::vector<BuildingAgg> BuildBuildings() {
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
            for (size_t i = 0; i < g_app.users.size(); i++)
                if (g_app.users[i].apartment_id == a.id) aa.userIndices.push_back((int)i);
            if (aa.userIndices.empty() && !g_showVacantApts) continue;   // not "active" -> not listed
            if (!aa.userIndices.empty()) agg.active++;
            agg.residents += (int)aa.userIndices.size();
            std::set<int> ids;
            for (int idx : aa.userIndices) ids.insert(g_app.users[idx].id);
            for (auto& p : g_app.payments) {
                if (p.status == "future" || !ids.count(p.user_id)) continue;
                if (p.direction == "out") { aa.paidOut += p.amount_paid; if (p.balance > 0.005) aa.owedOut += p.balance; }
                else                      { aa.collected += p.amount_paid; if (p.balance > 0.005) aa.owedIn += p.balance; }
            }
            agg.collected += aa.collected; agg.paidOut += aa.paidOut;
            agg.owedIn += aa.owedIn;       agg.owedOut += aa.owedOut;
            agg.apts.push_back(std::move(aa));
        }
        out.push_back(std::move(agg));
    }
    std::sort(out.begin(), out.end(), [](const BuildingAgg& a, const BuildingAgg& b) {
        return a.displayName < b.displayName;
    });
    return out;
}

static int  g_buildingSelectedIdx = 0;
static char g_buildingSearch[128] = "";

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

static void DrawBuildingsScreen() {
    if (ImGui::Button("Νέα πολυκατοικία")) {
        g_showBldgDialog = true; g_bldgDialogIsEdit = false; g_bldgDialogId = -1;
        bd_addr.set(""); bd_area.set(""); bd_postal.set(""); bd_afm.set("");
    }
    ImGui::SameLine();
    if (ImGui::Button("Ανανέωση")) g_app.reloadAll();
    ImGui::SameLine();
    ImGui::Checkbox("Εμφάνιση κενών διαμερισμάτων", &g_showVacantApts);
    ImGui::SameLine();
    ImGui::TextDisabled("Οι πολυκατοικίες και τα διαμερίσματά τους αποθηκεύονται ανεξάρτητα από τους πελάτες.");
    ImGui::Separator();

    auto buildings = BuildBuildings();
    DrawBuildingDialogs();
    if (buildings.empty()) {
        ImGui::TextDisabled("Καμία πολυκατοικία ακόμα — πατήστε «Νέα πολυκατοικία».");
        return;
    }
    if (g_buildingSelectedIdx >= (int)buildings.size()) g_buildingSelectedIdx = 0;

    ImGui::SetNextItemWidth(260);
    ImGui::InputTextWithHint("##bldgsearch", "Αναζήτηση κτιρίου...", g_buildingSearch, sizeof(g_buildingSearch));
    ImGui::Separator();

    float totalW  = ImGui::GetContentRegionAvail().x;
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float leftW   = std::max(260.0f, totalW * 0.32f);
    float rightW  = totalW - leftW - spacing;

    // ---- LEFT: building index ----
    ImGui::BeginChild("bldglist", ImVec2(leftW, -1), true);
    ImGui::TextColored(ImVec4(0.70f,0.85f,1.0f,1.0f), "Κτίρια (%d)", (int)buildings.size());
    ImGui::Separator();
    for (int i = 0; i < (int)buildings.size(); i++) {
        auto& b = buildings[i];
        if (g_buildingSearch[0] && !SubstrCI(b.displayName, g_buildingSearch)) continue;
        ImGui::PushID(i);
        // Multi-line entry: address on line 1, area/ΤΚ on line 2, ΑΦΜ on line 3,
        // then the counts. Apartment count uses totalApts (ALL apartments, not
        // just occupied), so adding a new empty apartment updates the count
        // immediately -- per client.
        std::string area_tk;
        if      (!b.b.area.empty() && !b.b.postal_code.empty()) area_tk = b.b.area + " · Τ.Κ. " + b.b.postal_code;
        else if (!b.b.area.empty())                              area_tk = b.b.area;
        else if (!b.b.postal_code.empty())                       area_tk = "Τ.Κ. " + b.b.postal_code;
        std::string afmLine = "ΑΦΜ: " + (b.b.afm.empty() ? std::string("—") : b.b.afm);

        char line[1024];
        std::snprintf(line, sizeof(line),
                      "%s\n%s\n%s\n   %d διαμερίσματα · %d κάτοικοι",
                      b.b.address.empty() ? "(χωρίς διεύθυνση)" : b.b.address.c_str(),
                      area_tk.empty() ? "—" : area_tk.c_str(),
                      afmLine.c_str(),
                      b.totalApts, b.residents);
        if (ImGui::Selectable(line, i == g_buildingSelectedIdx, 0, ImVec2(0, 90)))
            g_buildingSelectedIdx = i;
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // ---- RIGHT: selected building detail ----
    ImGui::BeginChild("bldgdetail", ImVec2(rightW, -1), false);
    BuildingAgg& b = buildings[g_buildingSelectedIdx];

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
    ImGui::Spacing();

    // ---- Logistics: 4 stat tiles (same visual language as profile/dashboard) ----
    char buf[64];
    if (ImGui::BeginTable("bldgstats", 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextRow();
        struct Tile { const char* label; double v; ImVec4 col; };
        Tile tiles[4] = {
            {"Εισπράχθηκαν", b.collected, ImVec4(0.35f,0.80f,0.50f,1)},
            {"Πληρώθηκαν", b.paidOut, ImVec4(0.90f,0.40f,0.40f,1)},
            {"Οφείλονται να εισπραχθούν", b.owedIn, ImVec4(0.95f,0.65f,0.25f,1)},
            {"Οφείλονται να πληρωθούν", b.owedOut, ImVec4(0.90f,0.65f,0.20f,1)} };
        for (int i = 0; i < 4; i++) {
            ImGui::TableNextColumn(); ImGui::PushID(i);
            float w = ImGui::GetContentRegionAvail().x;
            std::snprintf(buf, sizeof(buf), "%.2f €", tiles[i].v);
            StatTile(tiles[i].label, buf, tiles[i].col, nullptr, ImVec2(w, 88));
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.70f,0.85f,1.0f,1.0f), "Διαμερίσματα & Κάτοικοι");
    ImGui::Separator();

    if (ImGui::BeginTable("bldgapts", 7,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
            ImVec2(0, -1))) {
        ImGui::TableSetupColumn("Διαμέρισμα",           ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("Κάτοικος",             ImGuiTableColumnFlags_WidthStretch, 2.2f);
        ImGui::TableSetupColumn("Εισπράχθηκαν",         ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("Πληρώθηκαν",           ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("Οφ. να εισπραχθούν",   ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableSetupColumn("Οφ. να πληρωθούν",     ImGuiTableColumnFlags_WidthStretch, 1.3f);
        ImGui::TableSetupColumn("",                     ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableHeadersRow();

        // One row per RESIDENT: payments belong to the client, never to the apartment.
        // A vacant apartment gets a single "κενό" row with no money.
        for (auto& a : b.apts) {
            size_t nrows = a.userIndices.empty() ? 1 : a.userIndices.size();
            for (size_t r = 0; r < nrows; r++) {
                int uidx = a.userIndices.empty() ? -1 : a.userIndices[r];
                ImGui::PushID((int)(a.apt.id * 1000 + r));
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (r == 0) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.78f, 1.0f, 1.0f));
                    if (ImGui::Selectable(a.apt.label.c_str())) { g_aptGotoId = a.apt.id; g_tabToApartments = true; }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Άνοιγμα στην καρτέλα Διαμερίσματα");
                    ImGui::PopStyleColor();
                }

                ImGui::TableSetColumnIndex(1);
                double c = 0, po = 0, oi = 0, oo = 0;
                if (uidx < 0) {
                    ImGui::TextDisabled("— κενό —");
                } else {
                    auto& u = g_app.users[uidx];
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f,0.78f,1.0f,1.0f));
                    if (ImGui::Selectable(u.full_name.c_str())) OpenResidentLedger(u.id);
                    ImGui::PopStyleColor();
                    for (auto& p : g_app.payments) {
                        if (p.user_id != u.id || p.status == "future") continue;
                        if (p.direction == "out") { po += p.amount_paid; if (p.balance > 0.005) oo += p.balance; }
                        else                      { c += p.amount_paid;  if (p.balance > 0.005) oi += p.balance; }
                    }
                }
                if (uidx >= 0) {
                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextColored(ImVec4(0.35f,0.80f,0.50f,1), "%.2f €", c);
                    ImGui::TableSetColumnIndex(3);
                    ImGui::TextColored(ImVec4(0.90f,0.40f,0.40f,1), "%.2f €", po);
                    ImGui::TableSetColumnIndex(4);
                    ImGui::TextColored(oi > 0.005 ? ImVec4(0.95f,0.65f,0.25f,1) : ImVec4(0.55f,0.58f,0.65f,1), "%.2f €", oi);
                    ImGui::TableSetColumnIndex(5);
                    ImGui::TextColored(oo > 0.005 ? ImVec4(0.90f,0.65f,0.20f,1) : ImVec4(0.55f,0.58f,0.65f,1), "%.2f €", oo);
                }

                ImGui::TableSetColumnIndex(6);
                if (r == 0) {
                    if (ImGui::SmallButton("Μετονομασία")) {
                        g_showAptDialog = true; g_aptDialogIsEdit = true; g_aptDialogId = a.apt.id;
                        g_aptDialogBuilding = b.b.id; ad_label.set(a.apt.label);
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("X")) {
                        int id = a.apt.id; std::string lbl = a.apt.label;
                        RequestConfirm("Διαγραφή διαμερίσματος (οι κάτοικοι μένουν χωρίς διαμέρισμα)",
                                       { {"Διαμέρισμα", lbl, "(διαγραφή)"} }, [id]() {
                            g_app.conn->removeApartment(id); g_app.reloadUsers(); });
                    }
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    ImGui::EndChild();
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
static bool g_usersReorderCols = false;            // Αναδιάταξη Κολονών: columns draggable only while ON
static bool g_usersShowResidence = true;           // Στοιχεία κατοικίας: address / ΤΚ / area / apartment
static bool g_usersTableView = true;               // false = cards, true = table (default: list)

// Sort controls -- one dropdown for the attribute, submenu for direction.
// Field indices match the column order in DrawUsersTable.
static int g_usersSortField = 2;   // default: ΟΝΟΜΑ
static bool g_usersSortAsc = true; // default: ascending
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

// First-character (UTF-8 aware) extractor -- one Greek letter or one ASCII char.
static std::string FirstUtf8Char(const std::string& s) {
    if (s.empty()) return "";
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) return std::string(1, (char)c);
    if ((c & 0xE0) == 0xC0 && s.size() >= 2) return s.substr(0, 2);
    if ((c & 0xF0) == 0xE0 && s.size() >= 3) return s.substr(0, 3);
    return std::string(1, (char)c);
}

// ASCII-case-insensitive substring test (matches the app-wide search behaviour;
// Greek is stored uppercase, so byte-exact match on Greek is what we want).
// Fold text for searching: ASCII lower-cased; Greek lower/upper case, accents and the
// final sigma are all mapped to one unaccented capital, so "παπαδοπουλος" matches
// "ΠΑΠΑΔΟΠΟΥΛΟΣ" and "ελενη" matches "Ελένη".
static std::string FoldGreek(const std::string& in) {
    std::string out; out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        unsigned char c = (unsigned char)in[i];
        unsigned cp; int len;
        if (c < 0x80) { out += (char)std::tolower(c); i++; continue; }
        else if ((c >> 5) == 0x6 && i + 1 < in.size()) { cp = ((c & 0x1F) << 6) | ((unsigned char)in[i+1] & 0x3F); len = 2; }
        else if ((c >> 4) == 0xE && i + 2 < in.size()) { cp = ((c & 0x0F) << 12) | (((unsigned char)in[i+1] & 0x3F) << 6) | ((unsigned char)in[i+2] & 0x3F); len = 3; }
        else { out += (char)c; i++; continue; }
        unsigned m = cp;
        if (cp >= 0x3B1 && cp <= 0x3C9) m = (cp == 0x3C2) ? 0x3A3 : cp - 0x20;          // lower -> upper, final sigma
        else switch (cp) {
            case 0x386: case 0x3AC: m = 0x391; break;                                      // Ά ά
            case 0x388: case 0x3AD: m = 0x395; break;                                      // Έ έ
            case 0x389: case 0x3AE: m = 0x397; break;                                      // Ή ή
            case 0x38A: case 0x3AF: case 0x3AA: case 0x3CA: case 0x390: m = 0x399; break; // Ί ί Ϊ ϊ ΐ
            case 0x38C: case 0x3CC: m = 0x39F; break;                                      // Ό ό
            case 0x38E: case 0x3CD: case 0x3AB: case 0x3CB: case 0x3B0: m = 0x3A5; break; // Ύ ύ Ϋ ϋ ΰ
            case 0x38F: case 0x3CE: m = 0x3A9; break;                                      // Ώ ώ
            default: break;
        }
        if (m < 0x80) out += (char)m;
        else if (m < 0x800) { out += (char)(0xC0 | (m >> 6)); out += (char)(0x80 | (m & 0x3F)); }
        else { out += (char)(0xE0 | (m >> 12)); out += (char)(0x80 | ((m >> 6) & 0x3F)); out += (char)(0x80 | (m & 0x3F)); }
        i += len;
    }
    return out;
}

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
    UCOL_AREA, UCOL_TK, UCOL_SPECIAL, UCOL_ACTIONS,
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
    { "ΕΝΕΡΓΕΙΕΣ", ImGuiTableColumnFlags_WidthFixed,   145.0f, true,  false },
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
    { "ΕΝΕΡΓΕΙΕΣ", ImGuiTableColumnFlags_WidthFixed,   145.0f, false },
};
static std::vector<int> g_usersColOrder = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };

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
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
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
        if (ImGui::TableSetColumnIndex(P(UCOL_ADDR)))    { colW = ImGui::GetContentRegionAvail().x; TextHighlighted(TruncToWidth(u.address,      colW), hl(4)); }
        if (ImGui::TableSetColumnIndex(P(UCOL_APT)))     { colW = ImGui::GetContentRegionAvail().x; TextHighlighted(TruncToWidth(u.apartment,    colW), "");     }
        if (ImGui::TableSetColumnIndex(P(UCOL_NAME)))    { colW = ImGui::GetContentRegionAvail().x; TextHighlighted(TruncToWidth(u.full_name,    colW), hl(1)); }
        if (ImGui::TableSetColumnIndex(P(UCOL_PHONE)))   { colW = ImGui::GetContentRegionAvail().x; TextHighlighted(TruncToWidth(u.phone,        colW), hl(3)); }
        if (ImGui::TableSetColumnIndex(P(UCOL_AREA)))    { colW = ImGui::GetContentRegionAvail().x; TextHighlighted(TruncToWidth(u.area,         colW), hl(0)); }
        if (ImGui::TableSetColumnIndex(P(UCOL_TK)))      { colW = ImGui::GetContentRegionAvail().x; TextHighlighted(TruncToWidth(u.postal_code,  colW), hl(0)); }
        if (ImGui::TableSetColumnIndex(P(UCOL_SPECIAL))) { colW = ImGui::GetContentRegionAvail().x; TextHighlighted(TruncToWidth(u.special_code, colW), hl(2)); }

        // Inline actions -- profile + delete only; editing happens from inside
        // the profile window (single entry point, per client).
        if (ImGui::TableSetColumnIndex(P(UCOL_ACTIONS))) {
            if (ImGui::SmallButton("Προφίλ")) OpenUserProfile(u.id);
            ImGui::SameLine();
            if (ImGui::SmallButton("Διαγρ.")) {
                int id = u.id; std::string name = u.full_name;
                RequestConfirm("Διαγραφή πελάτη", { {"Όνομα", name, "(διαγραφή)"} }, [id]() {
                    g_app.conn->removeUser(id);
                    g_app.reloadUsers();
                });
            }
        }
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
            const float btnW = (ImGui::GetContentRegionAvail().x - btnSpacing * 2.0f) / 3.0f;
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
            ImGui::SameLine();
            if (ImGui::Button("Διαγρ.", ImVec2(btnW, 0))) {
                int id = u.id; std::string name = u.full_name;
                RequestConfirm("Διαγραφή πελάτη", { {"Όνομα", name, "(διαγραφή)"} }, [id]() {
                    g_app.conn->removeUser(id);
                    g_app.reloadUsers();
                });
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
    sb_label.set(""); sb_notes.set(""); sb_price = 0.0;
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
    ImGui::InputDouble("€##pkgnewprice", &sb_pkgNewServicePrice, 0.0, 0.0, "%.2f");
    ImGui::SameLine();
    if (ImGui::Button("+ Νέα Υπηρεσία") && sb_pkgNewServicePrice > 0.0) {
        std::string lbl = sb_pkgNewServiceLabel.str();
        if (!lbl.empty()) {
            Service ns; ns.label = lbl; ns.suggested_price = sb_pkgNewServicePrice;
            // A brand-new catalog service still needs at least one owner to exist
            // (service_users has no "ownerless" concept) -- default to the first
            // user; owners can be adjusted afterward like any other service.
            int newId = g_app.conn->addServiceGetId(-1, ns);
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
            char a[32], b[32]; std::snprintf(a, sizeof(a), "%.2f €", o.suggested_price); std::snprintf(b, sizeof(b), "%.2f €", sb_price);
            d.push_back({"Τιμή", a, b});
        }
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
        ImGui::InputDouble("Προτεινόμενη τιμή (€)", &sb_price, 1.0, 10.0, "%.2f");
        ImGui::Spacing();
        bool needPrice = sb_price <= 0.0;
        if (needPrice) ImGui::TextColored(ImVec4(1,0.5f,0.3f,1), "Η τιμή είναι υποχρεωτική (> 0).");
        ImGui::BeginDisabled(needPrice || sb_label.str().empty());
        if (ImGui::Button("Αποθήκευση", ImVec2(150, 0))) {
            if (g_serviceDialogIsEdit) {
                Service ns; ns.label = sb_label.str(); ns.extra_notes = sb_notes.str(); ns.suggested_price = sb_price;
                Service old = g_app.conn->getServiceById(g_serviceDialogId);
                std::vector<FieldDiff> diffs = { {"Ετικέτα", old.label, ns.label}, {"Τιμή", std::to_string((int)(old.suggested_price + 0.5)), std::to_string((int)(ns.suggested_price + 0.5))} };
                int id = g_serviceDialogId;
                RequestConfirm("Επεξεργασία υπηρεσίας", diffs, [id, ns]() {
                    g_app.conn->editService(id, ns);
                    g_app.reloadServices();
                });
            } else {
                Service ns; ns.label = sb_label.str(); ns.extra_notes = sb_notes.str(); ns.suggested_price = sb_price;
                std::vector<FieldDiff> diffs = { {"Ετικέτα", "-", ns.label}, {"Τιμή", "-", std::to_string((int)(ns.suggested_price + 0.5)) + " €"} };
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

static void DrawServicesTable(const std::set<int>& jointIds) {
    if (!ImGui::BeginTable("servicestable", 5,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("ΚΩΔ",        ImGuiTableColumnFlags_WidthFixed, 50.0f);
    ImGui::TableSetupColumn("ΥΠΗΡΕΣΙΑ",   ImGuiTableColumnFlags_WidthStretch, 2.4f);
    ImGui::TableSetupColumn("ΤΙΜΗ",       ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableSetupColumn("ΣΗΜΕΙΩΣΕΙΣ", ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn("ΕΝΕΡΓΕΙΕΣ",  ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoReorder | ImGuiTableColumnFlags_NoHide, 250.0f);
    ImGui::TableHeadersRow();
    for (auto& s : g_app.services) {
        if (jointIds.count(s.id)) continue;
        ImGui::PushID(s.id);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::Text("%d", s.id);
        ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(s.label.c_str());
        ImGui::TableSetColumnIndex(2); ImGui::Text("%.2f €", s.suggested_price);
        ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(TruncToWidth(s.extra_notes, ImGui::GetContentRegionAvail().x).c_str());
        ImGui::TableSetColumnIndex(4);
        if (ImGui::SmallButton("Χρέωση")) OpenChargeService(s.id);
        ImGui::SameLine();
        if (ImGui::SmallButton("Άνοιγμα")) OpenServiceDetail(s.id);
        ImGui::SameLine();
        if (ImGui::SmallButton("Επεξ.")) {
            g_serviceDialogIsEdit = true; g_serviceDialogId = s.id;
            sb_label.set(s.label); sb_notes.set(s.extra_notes); sb_price = s.suggested_price;
            g_showServiceDialog = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Διαγρ.")) {
            int id = s.id; std::string label = s.label;
            RequestConfirm("Διαγραφή υπηρεσίας", { {"Ετικέτα", label, "(διαγραφή)"} }, [id]() {
                g_app.conn->removeService(id); g_app.reloadServices(); g_app.reloadJoint();
            });
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

static const ListColDef kServiceListCols[5] = {
    { "ΚΩΔ",        ImGuiTableColumnFlags_WidthFixed,   110.0f, true  },
    { "ΥΠΗΡΕΣΙΑ",   ImGuiTableColumnFlags_WidthStretch, 2.4f,   true  },
    { "ΤΙΜΗ",       ImGuiTableColumnFlags_WidthFixed,   130.0f, true  },
    { "ΣΗΜΕΙΩΣΕΙΣ", ImGuiTableColumnFlags_WidthStretch, 3.0f,   true  },
    { "ΕΝΕΡΓΕΙΕΣ",  ImGuiTableColumnFlags_WidthFixed,   300.0f, false },
};
static std::vector<int> g_servicesColOrder = { 0, 1, 2, 3, 4 };
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
    ImGui::TextDisabled("(Οι κοινόχρηστες υπηρεσίες εμφανίζονται στην ξεχωριστή καρτέλα)");
    ImGui::Separator();

    // --- Query one page from SQL. Joint services are excluded at SQL level. ---
    std::vector<Service> page = g_app.conn->pageServices("", g_servicesPager.offset, g_servicesPager.pageSize, g_servicesSort.keys);
    g_servicesPager.total = g_app.conn->countServices("");

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
        if (ImGui::BeginTable("servicestable", 5,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
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
                ImGui::TableSetColumnIndex(P(1)); ImGui::TextUnformatted(s.label.c_str());
                ImGui::TableSetColumnIndex(P(2)); ImGui::Text("%.2f €", s.suggested_price);
                ImGui::TableSetColumnIndex(P(3)); ImGui::TextUnformatted(TruncToWidth(s.extra_notes, ImGui::GetContentRegionAvail().x).c_str());
                ImGui::TableSetColumnIndex(P(4));
                if (ImGui::SmallButton("Χρέωση")) OpenChargeService(s.id);
                ImGui::SameLine();
                if (ImGui::SmallButton("Άνοιγμα")) OpenServiceDetail(s.id);
                ImGui::SameLine();
                if (ImGui::SmallButton("Επεξ.")) {
                    g_serviceDialogIsEdit = true; g_serviceDialogId = s.id;
                    sb_label.set(s.label); sb_notes.set(s.extra_notes); sb_price = s.suggested_price;
                    g_showServiceDialog = true;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Διαγρ.")) {
                    int id = s.id; std::string label = s.label;
                    RequestConfirm("Διαγραφή υπηρεσίας", { {"Ετικέτα", label, "(διαγραφή)"} }, [id]() {
                        g_app.conn->removeService(id); g_app.reloadServices(); g_app.reloadJoint();
                    });
                }
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
            ImGui::TextColored(ImVec4(0.5f,0.8f,1.0f,1), "%s", s.label.c_str());
            if (s.suggested_price > 0.0) ImGui::TextColored(ImVec4(0.35f,0.80f,0.50f,1), "Προτεινόμενη τιμή: %.2f €", s.suggested_price);
            if (!s.extra_notes.empty()) ImGui::TextWrapped("%s", s.extra_notes.c_str());
            ImGui::Spacing();
            if (ImGui::SmallButton("Χρέωση")) OpenChargeService(s.id);
            ImGui::SameLine();
            if (ImGui::SmallButton("Άνοιγμα")) OpenServiceDetail(s.id);
            ImGui::SameLine();
            if (ImGui::SmallButton("Επεξ.")) {
                g_serviceDialogIsEdit = true; g_serviceDialogId = s.id;
                sb_label.set(s.label); sb_notes.set(s.extra_notes); sb_price = s.suggested_price;
                g_showServiceDialog = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Διαγρ.")) {
                int id = s.id; std::string label = s.label;
                RequestConfirm("Διαγραφή υπηρεσίας", { {"Ετικέτα", label, "(διαγραφή)"} }, [id]() {
                    g_app.conn->removeService(id);
                    g_app.reloadServices();
                });
            }
            EndCard();
            ImGui::PopID();
            CardWrapNext(cardW);
        }
        ImGui::EndChild();
    }

    DrawPager(g_servicesPager, "servicespager");
}

// ---------------------------------------------------------------------------
// JOINT SERVICES SCREEN ("Κοινόχρηστες Υπηρεσίες")
// ---------------------------------------------------------------------------
static bool g_showJointDialog = false;
static bool g_jointDialogWasOpen = false;
static TextBuf jb_label, jb_notes;
static std::vector<char> jb_selected; // parallel to g_app.users
static TextBuf jb_newUserName, jb_newUserPhone;

static void OpenAddJoint() {
    g_showJointDialog = true;
    jb_label.set(""); jb_notes.set(""); jb_price = 0.0;
    jb_selected.assign(g_app.users.size(), 0);
    jb_newUserName.set(""); jb_newUserPhone.set("");
}

static void DrawJointDialog() {
    if (g_showJointDialog && !g_jointDialogWasOpen) {
        ImGui::OpenPopup("Νέα Κοινόχρηστη Υπηρεσία");
    }
    g_jointDialogWasOpen = g_showJointDialog;
    if (!g_showJointDialog) return;
    ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_Appearing);
    if (BeginGuardedModal("Νέα Κοινόχρηστη Υπηρεσία", g_showJointDialog, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Ετικέτα", jb_label.data, sizeof(jb_label.data));
        ImGui::InputTextMultiline("Σημειώσεις", jb_notes.data, sizeof(jb_notes.data), ImVec2(0, 50));
        ImGui::InputDouble("Προτεινόμενη τιμή (€)", &jb_price, 1.0, 10.0, "%.2f");
        ImGui::Separator();
        ImGui::Text("Επιλογή συνιδιοκτητών (2+):");
        if (jb_selected.size() != g_app.users.size()) jb_selected.assign(g_app.users.size(), 0);
        ImGui::BeginChild("rosterpick", ImVec2(0, 140), true);
        for (size_t i = 0; i < g_app.users.size(); i++) {
            bool v = jb_selected[i] != 0;
            if (ImGui::Checkbox(g_app.users[i].full_name.c_str(), &v)) jb_selected[i] = v ? 1 : 0;
        }
        ImGui::EndChild();

        ImGui::Separator();
        ImGui::TextDisabled("Γρήγορη προσθήκη νέου χρήστη:");
        ImGui::SetNextItemWidth(180); ImGui::InputTextWithHint("##nuname", "Ονοματεπώνυμο", jb_newUserName.data, sizeof(jb_newUserName.data));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140); ImGui::InputTextWithHint("##nuphone", "Τηλέφωνο", jb_newUserPhone.data, sizeof(jb_newUserPhone.data));
        ImGui::SameLine();
        if (ImGui::Button("+ Νέος")) {
            if (jb_newUserName.str().size() > 0) {
                User nu; nu.full_name = jb_newUserName.str(); nu.phone = jb_newUserPhone.str();
                g_app.conn->addUser(nu);
                g_app.reloadUsers();
                jb_selected.assign(g_app.users.size(), 0);
                // select the newly-created user (last added, matched by name)
                for (size_t i = 0; i < g_app.users.size(); i++)
                    if (g_app.users[i].full_name == nu.full_name) jb_selected[i] = 1;
                jb_newUserName.set(""); jb_newUserPhone.set("");
            }
        }

        ImGui::Spacing();
        int selCount = 0; for (char c : jb_selected) if (c) selCount++;
        if (selCount < 2) ImGui::TextColored(ImVec4(1,0.5f,0.3f,1), "Επιλέξτε τουλάχιστον 2 χρήστες.");
        ImGui::BeginDisabled(selCount < 2 || jb_label.str().empty() || jb_price <= 0.0);
        if (ImGui::Button("Δημιουργία", ImVec2(150, 0))) {
            Service ns; ns.label = jb_label.str(); ns.extra_notes = jb_notes.str(); ns.suggested_price = jb_price;
            std::vector<int> userIds;
            for (size_t i = 0; i < jb_selected.size(); i++) if (jb_selected[i]) userIds.push_back(g_app.users[i].id);
            RequestConfirm("Νέα κοινόχρηστη υπηρεσία", { {"Ετικέτα", "-", ns.label}, {"Συνιδιοκτήτες", "-", std::to_string(userIds.size())} },
                [ns, userIds]() {
                    int sid = g_app.conn->addServiceGetId(userIds[0], ns);
                    if (sid >= 0) for (size_t i = 1; i < userIds.size(); i++) g_app.conn->addServiceUser(sid, userIds[i]);
                    g_app.reloadServices();
                    g_app.reloadJoint();
                });
            g_showJointDialog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Ακύρωση", ImVec2(150, 0))) { g_showJointDialog = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

static void DrawJointServicesScreen() {
    if (ImGui::Button("+ Νέα Κοινόχρηστη Υπηρεσία")) OpenAddJoint();
    ImGui::SameLine();
    if (ImGui::Button("Ανανέωση")) g_app.reloadJoint();
    ImGui::Separator();

    ImGui::BeginChild("jointcards", ImVec2(0, 0), false);
    for (auto& j : g_app.jointServices) {
        ImGui::PushID(j.service.id);
        // Big and spacious, per the client ("not suppressed and tiny"): a base
        // height for the title/totals/button block plus a real row height per
        // roster entry, so a 4-owner service's card is taller than a 2-owner one
        // and the whole roster is always visible without needing to scroll (cards
        // have scrolling disabled by BeginCard, so an undersized table silently
        // hid rows before this fix).
        float lineH = ImGui::GetTextLineHeightWithSpacing();
        const float kRosterRowH = 28.0f;
        const float kRosterHeaderH = 30.0f;
        const float kBaseH = 3 * lineH /*title/totals/net lines*/ + 30.0f /*Άνοιγμα button*/ + 24.0f /*padding*/;
        float rosterH = kRosterHeaderH + (float)j.users.size() * kRosterRowH;
        float cardH = kBaseH + rosterH;
        BeginCard("card", ImVec2(ImGui::GetContentRegionAvail().x, cardH));
        ImGui::TextColored(ImVec4(1.0f,0.8f,0.3f,1), "%s  (%zu συνιδιοκτήτες)%s", j.service.label.c_str(), j.users.size(),
                           j.service.suggested_price > 0.0 ? (std::string("  ·  Προτεινόμενη τιμή: ") + std::to_string((int)(j.service.suggested_price + 0.5)) + " €").c_str() : "");
        ImGui::Text("Σύνολο οφειλής: %.2f €   Σύνολο πληρωμένο: %.2f €", j.totalDue, j.totalPaid);
        ImVec4 jNetCol = j.totalNet > 0.005 ? ImVec4(0.95f,0.55f,0.25f,1) : ImVec4(0.35f,0.85f,0.45f,1);
        ImGui::TextColored(jNetCol, "Υπόλοιπο μέχρι μηδενισμού (πληρώθηκε - εισπράχθηκε): %.2f €", j.totalNet);
        if (ImGui::SmallButton("Άνοιγμα")) OpenServiceDetail(j.service.id);
        if (ImGui::BeginTable("roster", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg, ImVec2(0, rosterH))) {
            ImGui::TableSetupColumn("Χρήστης");
            ImGui::TableSetupColumn("Οφειλή");
            ImGui::TableSetupColumn("Πληρωμένο");
            ImGui::TableSetupColumn("Υπόλοιπο");
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < j.users.size(); i++) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(j.users[i].full_name.c_str());
                ImGui::TableSetColumnIndex(1); ImGui::Text("%.2f", j.userTotalDue[i]);
                ImGui::TableSetColumnIndex(2); ImGui::Text("%.2f", j.userTotalPaid[i]);
                ImGui::TableSetColumnIndex(3);
                double un = i < j.userNet.size() ? j.userNet[i] : 0.0;
                ImGui::TextColored(un > 0.005 ? ImVec4(0.95f,0.55f,0.25f,1) : ImVec4(0.35f,0.85f,0.45f,1), "%.2f", un);
            }
            ImGui::EndTable();
        }
        EndCard();
        ImGui::PopID();
    }
    ImGui::EndChild();
    DrawJointDialog();
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

    auto summaries = g_app.conn->getMasterServiceSummaries();
    if (summaries.empty()) { ImGui::TextDisabled("Κανένα πακέτο ακόμα."); return; }
    bool selValid = false;
    for (auto& ms : summaries) if (ms.masterService.id == g_selectedPackageId) selValid = true;
    if (!selValid) g_selectedPackageId = summaries[0].masterService.id;

    float totalW  = ImGui::GetContentRegionAvail().x;
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float leftW   = (totalW - spacing) * 0.5f;
    float rightW  = totalW - leftW - spacing;

    // ---------------- LEFT: package info + prices ----------------
    ImGui::BeginChild("pkglist", ImVec2(leftW, -1), false);
    for (auto& ms : summaries) {
        bool selected = ms.masterService.id == g_selectedPackageId;
        double pkgTotal = 0.0;
        for (auto& c : ms.components) pkgTotal += c.suggested_price;
        float lineH = ImGui::GetTextLineHeightWithSpacing();
        float cardH = (2.0f + (float)ms.components.size()) * lineH + 24.0f;
        ImGui::PushID(ms.masterService.id);
        if (selected) ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.30f, 0.65f, 1.0f, 1.0f));
        BeginCard("card", ImVec2(ImGui::GetContentRegionAvail().x, cardH));
        char head[256];
        std::snprintf(head, sizeof(head), "%s   —   %.2f €", ms.masterService.label.c_str(), pkgTotal);
        if (ImGui::Selectable(head, selected, 0, ImVec2(ImGui::GetContentRegionAvail().x - 130.0f, 0)))
            g_selectedPackageId = ms.masterService.id;
        ImGui::SameLine();
        if (ImGui::SmallButton("Χρέωση πακέτου")) OpenPackageCharge(ms.masterService.id);
        for (auto& c : ms.components) {
            ImGui::Bullet(); ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.9f,0.9f,0.3f,1), "%s", c.label.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("(%.2f €)", c.suggested_price);
        }
        // Whole card is the click target (not just the title).
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseReleased(0))
            g_selectedPackageId = ms.masterService.id;
        EndCard();
        if (selected) ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // ---------------- RIGHT: who bought it ----------------
    ImGui::BeginChild("pkgbuyers", ImVec2(rightW, -1), true);
    const DataConn::MasterServiceSummary* sel = nullptr;
    for (auto& ms : summaries) if (ms.masterService.id == g_selectedPackageId) sel = &ms;
    if (sel) {
        int N = (int)sel->components.size();
        ImGui::TextColored(ImVec4(0.6f,0.9f,1.0f,1), "Πακέτο: %s  (%d υπηρεσίες)", sel->masterService.label.c_str(), N);
        ImGui::Separator();

        // user id -> (services bought, sum of amount_due on them)
        struct Buyer { int userId; std::vector<std::string> parts; double due = 0.0; };
        std::map<int, Buyer> buyers;
        for (auto& c : sel->components) {
            std::set<int> seen;
            for (auto& p : g_app.payments) {
                if (p.service_id != c.id) continue;
                auto& b = buyers[p.user_id];
                b.userId = p.user_id;
                b.due += p.amount_due;
                if (seen.insert(p.user_id).second) b.parts.push_back(c.label);
            }
        }
        std::vector<Buyer> full, partial;
        for (auto& [uid, b] : buyers) (N > 0 && (int)b.parts.size() == N ? full : partial).push_back(b);
        std::sort(partial.begin(), partial.end(), [](const Buyer& a, const Buyer& b) {
            if (a.parts.size() != b.parts.size()) return a.parts.size() > b.parts.size();
            return g_app.userName(a.userId) < g_app.userName(b.userId);
        });
        std::sort(full.begin(), full.end(), [](const Buyer& a, const Buyer& b) {
            return g_app.userName(a.userId) < g_app.userName(b.userId);
        });

        auto drawBuyers = [&](const char* tableId, const std::vector<Buyer>& list) {
            if (!ImGui::BeginTable(tableId, 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) return;
            ImGui::TableSetupColumn("Πελάτης", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("Υπηρεσίες", ImGuiTableColumnFlags_WidthStretch, 4.0f);
            ImGui::TableSetupColumn("Χρεώσεις", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableHeadersRow();
            for (auto& b : list) {
                ImGui::PushID(b.userId);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f,0.78f,1.0f,1.0f));
                if (ImGui::Selectable(g_app.userName(b.userId).c_str())) OpenUserProfile(b.userId);
                ImGui::PopStyleColor();
                ImGui::TableSetColumnIndex(1);
                std::string txt = std::to_string(b.parts.size()) + "/" + std::to_string(N) + ": ";
                for (size_t i = 0; i < b.parts.size(); i++) txt += (i ? ", " : "") + b.parts[i];
                ImGui::TextWrapped("%s", txt.c_str());
                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%.2f €", b.due);
                ImGui::PopID();
            }
            ImGui::EndTable();
        };

        ImGui::TextColored(ImVec4(0.35f,0.80f,0.50f,1), "Αγόρασαν ολόκληρο το πακέτο (%d)", (int)full.size());
        if (full.empty()) ImGui::TextDisabled("Κανένας πελάτης ακόμα."); else drawBuyers("buyersfull", full);
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.95f,0.65f,0.25f,1), "Αγόρασαν μέρος του πακέτου (%d) — πρώτοι όσοι πήραν τις περισσότερες υπηρεσίες", (int)partial.size());
        if (partial.empty()) ImGui::TextDisabled("Κανένας πελάτης ακόμα."); else drawBuyers("buyerspart", partial);
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------
// PAYMENTS SCREEN (heaviest filter set, per spec)
// ---------------------------------------------------------------------------
static std::string g_payStartDate, g_payEndDate;
static int g_payUserFilter = -1;
static int g_payServiceFilter = -1;
static int g_payStatusFilter = 0; // 0=all,1=future,2=resolved
static const char* kStatusNames[] = { "Όλα", "Μελλοντικές", "Επιλυμένες" };
static int g_payDirectionFilter = 0; // 0=all,1=in,2=out
static const char* kDirectionFilterNames[] = { "Όλες", "Είσπραξη (in)", "Πληρωμή (out)" };

// Direction/type badge: green "down" arrow for money coming in from the client,
// red "up" arrow for money the manager fronted (client owes it back). "comment"
// is not a direction anymore -- a payment's optional comment lives in its `notes`
// field, an always-visible attribute alongside the amount, not a substitute type.
// Banking-app-style restraint: green = money in, red = money out.
static ImVec4 DirectionColor(const std::string& direction) {
    return direction == "out" ? ImVec4(0.90f, 0.40f, 0.40f, 1) : ImVec4(0.35f, 0.80f, 0.50f, 1);
}
static const char* DirectionBadge(const std::string& direction) {
    return direction == "out" ? "\xE2\x86\x91 Πληρωμή" // "↑ Πληρωμή"
                               : "\xE2\x86\x93 Είσπραξη"; // "↓ Είσπραξη"
}

// Human-readable status text for the profile payment list. Replaces the raw DB
// status word ("paid"/"future") with one of four real-world outcomes:
//   collected from client  /  paid out for client
//   still owed by client   /  still owed to third party
// "Owed" covers both an unresolved future payment AND a paid-but-partial one
// (balance > 0), so partial payments read as still-owed too.
static const char* PaymentStatusLabel(const Payment& p) {
    bool owed = (p.status == "future") || (p.balance > 0.005);
    if (p.direction == "out") return owed ? "Οφείλεται να πληρωθεί" : "Πληρώθηκε";
    return owed ? "Οφείλεται να εισπραχθεί" : "Εισπράχθηκε";
}

// Status color, restrained to the same 3-4 color banking-app system: green = resolved
// (settled/real), red = overdue (leftover pre-simplification data only), neutral
// gray-blue = future (just "scheduled", not itself an alert -- the amber "needs
// attention soon" tier lives only in the notifications popup, tied to proximity to
// resolution_date, not to this general status dot). Old "pending" rows fall back to
// the same neutral tone as future since they're not urgent by themselves either.
static ImVec4 StatusColor(const std::string& status) {
    if (status == "future") return ImVec4(0.60f, 0.65f, 0.72f, 1);
    if (status == "overdue") return ImVec4(0.90f, 0.40f, 0.40f, 1);
    if (status == "pending") return ImVec4(0.60f, 0.65f, 0.72f, 1);
    return ImVec4(0.35f, 0.80f, 0.50f, 1); // paid / resolved (default)
}

static bool g_showPaymentDialog = false;
static bool g_paymentDialogWasOpen = false;
static bool g_paymentDialogIsEdit = false;
static int g_paymentDialogId = -1;
static int pb_userId = -1, pb_serviceId = -1;
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

static void OpenAddPayment() {
    g_showPaymentDialog = true; g_paymentDialogIsEdit = false;
    pb_userId = g_app.users.empty() ? -1 : g_app.users[0].id;
    pb_serviceId = g_app.services.empty() ? -1 : g_app.services[0].id;
    pb_dateStr = CurrentDateString();
    pb_due.set("0"); pb_paid.set("0"); pb_notes.set(""); pb_resolutionDate.clear();
    pb_direction = 0; pb_isFuture = false; pb_lockUser = false; pb_lockService = false;
}

static void DrawPaymentDialog() {
    if (g_showPaymentDialog && !g_paymentDialogWasOpen) {
        ImGui::OpenPopup("Στοιχεία Πληρωμής");
    }
    g_paymentDialogWasOpen = g_showPaymentDialog;
    if (!g_showPaymentDialog) return;
    ImGui::SetNextWindowSize(ImVec2(440, 0), ImGuiCond_Appearing);
    if (BeginGuardedModal("Στοιχεία Πληρωμής", g_showPaymentDialog, ImGuiWindowFlags_AlwaysAutoResize, []() {
        std::vector<FieldDiff> d; if (!g_paymentDialogIsEdit) return d;
        Payment o = g_app.conn->getPaymentById(g_paymentDialogId);
        char od[16]; std::snprintf(od, sizeof(od), "%04d-%02d-%02d", o.year, o.month, o.day);
        DiffAdd(d, "Ημερομηνία", od, pb_dateStr);
        if (o.user_id != pb_userId) d.push_back({"Πελάτης", g_app.userName(o.user_id), g_app.userName(pb_userId)});
        if (o.service_id != pb_serviceId) d.push_back({"Υπηρεσία", g_app.serviceLabel(o.service_id), g_app.serviceLabel(pb_serviceId)});
        double nd = atof(pb_due.str().c_str()), np = atof(pb_paid.str().c_str());
        char a[32], b[32];
        if (std::fabs(o.amount_due - nd) > 0.004) { std::snprintf(a, sizeof(a), "%.2f", o.amount_due); std::snprintf(b, sizeof(b), "%.2f", nd); d.push_back({"Οφειλή", a, b}); }
        if (!pb_isFuture && std::fabs(o.amount_paid - np) > 0.004) { std::snprintf(a, sizeof(a), "%.2f", o.amount_paid); std::snprintf(b, sizeof(b), "%.2f", np); d.push_back({"Πληρωμένο", a, b}); }
        if ((o.status == "future") != pb_isFuture) d.push_back({"Κατάσταση", o.status == "future" ? "μελλοντική" : "εξοφλημένη", pb_isFuture ? "μελλοντική" : "εξοφλημένη"});
        DiffAdd(d, "Τύπος", o.direction == "out" ? "Πληρωμή" : "Είσπραξη", pb_direction == 1 ? "Πληρωμή" : "Είσπραξη");
        DiffAdd(d, "Σημειώσεις", o.notes, pb_notes.str());
        if (pb_isFuture) DiffAdd(d, "Ημ/νία επίλυσης", o.resolution_date, pb_resolutionDate);
        return d; })) {
        if (pb_lockUser) {
            ImGui::TextDisabled("Χρήστης: %s", g_app.userName(pb_userId).c_str());
        } else {
            if (ImGui::BeginCombo("Χρήστης", g_app.userName(pb_userId).c_str())) {
                for (auto& u : g_app.users) if (ImGui::Selectable(u.full_name.c_str(), u.id == pb_userId)) pb_userId = u.id;
                ImGui::EndCombo();
            }
        }
        if (pb_lockService) {
            ImGui::TextDisabled("Υπηρεσία: %s", g_app.serviceLabel(pb_serviceId).c_str());
        } else {
            if (ImGui::BeginCombo("Υπηρεσία", g_app.serviceLabel(pb_serviceId).c_str())) {
                for (auto& s : g_app.services) if (ImGui::Selectable(s.label.c_str(), s.id == pb_serviceId)) pb_serviceId = s.id;
                ImGui::EndCombo();
            }
        }
        ImGui::TextUnformatted("Ημερομηνία πληρωμής:");
        DatePicker("paydate", pb_dateStr, ImVec2(0, 1));
        ImGui::Spacing();
        ImGui::TextDisabled("Τύπος:");
        ImGui::RadioButton("Είσπραξη από πελάτη", &pb_direction, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Πληρωμή εκ μέρους πελάτη", &pb_direction, 1);
        ImGui::Spacing();
        ImGui::InputText("Οφειλόμενο ποσό", pb_due.data, sizeof(pb_due.data));
        ImGui::Checkbox("Μελλοντική πληρωμή", &pb_isFuture);
        if (pb_isFuture) {
            ImGui::TextDisabled("Δεν έχει πληρωθεί ακόμα -- δεν προσμετράται σε κανένα σύνολο μέχρι να επιλυθεί.");
            // Real calendar popup, not handwritten text -- and the picker itself
            // refuses to let you click today or any past day, so the "must be in
            // the future from now" rule is enforced at selection time, not after.
            ImGui::TextUnformatted("Ημ/νία επίλυσης:");
            DatePicker("resdate", pb_resolutionDate, ImVec2(0, 1), /*futureOnly=*/true);
            ImGui::TextDisabled("Πότε αναμένεται αυτή η μελλοντική πληρωμή να γίνει πραγματική.");
        } else {
            ImGui::InputText("Πληρωμένο ποσό", pb_paid.data, sizeof(pb_paid.data));
        }
        // Notes/comment is a normal, always-visible, optional attribute of every
        // payment -- never a substitute for the amount fields (per client correction).
        ImGui::InputTextMultiline("Σημειώσεις/Σχόλιο", pb_notes.data, sizeof(pb_notes.data), ImVec2(0, 60));
        ImGui::Spacing();
        if (ImGui::Button("Αποθήκευση", ImVec2(150, 0))) {
            Payment np;
            np.user_id = pb_userId; np.service_id = pb_serviceId;
                        int iy = 0, im = 0, id = 0;
            std::sscanf(pb_dateStr.c_str(), "%d-%d-%d", &iy, &im, &id);
            np.year = iy; np.month = im; np.day = id;
            if (pb_isFuture) {
                np.amount_due = atof(pb_due.str().c_str()); np.amount_paid = 0.0;
                np.balance = np.amount_due;
                np.status = "future";
                np.direction = pb_direction == 1 ? "out" : "in";
            } else {
                np.amount_due = atof(pb_due.str().c_str()); np.amount_paid = atof(pb_paid.str().c_str());
                np.balance = np.amount_due - np.amount_paid;
                np.status = "paid"; // resolved immediately -- same status value the Resolve flow uses
                np.direction = pb_direction == 1 ? "out" : "in";
            }
            np.notes = pb_notes.str();
            np.resolution_date = np.status == "future" ? pb_resolutionDate : "";
            if (g_paymentDialogIsEdit) {
                Payment old = g_app.conn->getPaymentById(g_paymentDialogId);
                np.resolved_at = old.resolved_at; // editing via the normal dialog never touches resolved_at
                std::vector<FieldDiff> diffs = {
                    {"Ποσό οφειλής", std::to_string(old.amount_due), std::to_string(np.amount_due)},
                    {"Πληρωμένο", std::to_string(old.amount_paid), std::to_string(np.amount_paid)},
                    {"Κατάσταση", old.status, np.status},
                };
                int id = g_paymentDialogId;
                RequestConfirm("Επεξεργασία πληρωμής", diffs, [id, np]() { g_app.conn->editPayment(id, np); g_app.reloadPayments(); });
            } else {
                std::vector<FieldDiff> diffs = { {"Χρήστης", "-", g_app.userName(np.user_id)}, {"Ποσό", "-", std::to_string(np.amount_due)} };
                RequestConfirm("Νέα πληρωμή", diffs, [np]() { g_app.conn->addPayment(np); g_app.reloadPayments(); });
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

static void OpenPackageCharge(int pkgId) {
    g_showPkgChargeDialog = true;
    pc_userId = g_app.users.empty() ? -1 : g_app.users[0].id;
    pc_pkgId = pkgId; pc_checkedFor = -2;
    pc_dateStr = CurrentDateString(); pc_resolutionDate.clear();
    pc_direction = 0; pc_isFuture = false;
}

// Charge a single catalog service to a client (opens the normal payment dialog with
// the service and its suggested price pre-filled). Used from the Υπηρεσίες tab.
static void OpenChargeService(int serviceId) {
    OpenAddPayment();
    pb_serviceId = serviceId;
    pb_lockService = true;   // opened from this specific service -- locked
    Service s = g_app.conn->getServiceById(serviceId);
    pb_due.set(std::to_string(s.suggested_price));
    pb_paid.set(std::to_string(s.suggested_price));
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
        for (auto& m : g_app.conn->getMasterServices()) if (m.id == pc_pkgId) pkgName = m.label;
        ImGui::TextDisabled("Πακέτο: %s", pkgName.c_str());
        if (ImGui::BeginCombo("Πελάτης", g_app.userName(pc_userId).c_str())) {
            for (auto& u : g_app.users) if (ImGui::Selectable(u.full_name.c_str(), u.id == pc_userId)) pc_userId = u.id;
            ImGui::EndCombo();
        }
        ImGui::TextUnformatted("Ημερομηνία:");
        DatePicker("pkgdate", pc_dateStr, ImVec2(0, 1));
        ImGui::RadioButton("Είσπραξη από πελάτη", &pc_direction, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Πληρωμή εκ μέρους πελάτη", &pc_direction, 1);
        ImGui::Checkbox("Μελλοντικές πληρωμές (δεν έχουν εξοφληθεί ακόμα)", &pc_isFuture);
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
            pc_checkedFor = pc_pkgId;
        }
        ImGui::Separator();
        ImGui::TextDisabled("Επιλέξτε ποιες υπηρεσίες χρεώνονται (ξεχωριστή πληρωμή η καθεμία):");
        std::vector<Service> charge;
        double total = 0.0;
        for (size_t i = 0; i < comps.size(); i++) {
            auto& s = comps[i];
            bool on = pc_checked[i] != 0;
            ImGui::PushID((int)i);
            std::string lbl = s.label + "  —  " + std::to_string((int)(s.suggested_price + 0.5)) + " €"
                            + (jointIds.count(s.id) ? "  (κοινόχρηστη)" : "");
            if (ImGui::Checkbox(lbl.c_str(), &on)) pc_checked[i] = on ? 1 : 0;
            ImGui::PopID();
            if (pc_checked[i]) { charge.push_back(s); total += s.suggested_price; }
        }
        if (ImGui::SmallButton("Όλες")) std::fill(pc_checked.begin(), pc_checked.end(), 1);
        ImGui::SameLine();
        if (ImGui::SmallButton("Καμία")) std::fill(pc_checked.begin(), pc_checked.end(), 0);
        ImGui::Text("Σύνολο: %.2f € σε %d πληρωμές", total, (int)charge.size());
        ImGui::Spacing();
        bool ok = pc_userId >= 0 && !charge.empty() && (!pc_isFuture || !pc_resolutionDate.empty());
        ImGui::BeginDisabled(!ok);
        if (ImGui::Button("Χρέωση", ImVec2(150, 0))) {
            int iy = 0, im = 0, id = 0;
            std::sscanf(pc_dateStr.c_str(), "%d-%d-%d", &iy, &im, &id);
            int uid = pc_userId; bool fut = pc_isFuture; std::string dir = pc_direction == 1 ? "out" : "in";
            std::string res = pc_resolutionDate, note = "Πακέτο: " + pkgName;
            RequestConfirm("Χρέωση πακέτου", { {"Πελάτης", "-", g_app.userName(uid)}, {"Πακέτο", "-", pkgName},
                                                 {"Πληρωμές", "-", std::to_string(charge.size()) + " / " + std::to_string((int)(total + 0.5)) + " €"} },
                [=]() {
                    for (auto& s : charge) {
                        Payment p;
                        p.user_id = uid; p.service_id = s.id;
                        p.year = iy; p.month = im; p.day = id;
                        p.amount_due = s.suggested_price;
                        p.amount_paid = fut ? 0.0 : s.suggested_price;
                        p.balance = p.amount_due - p.amount_paid;
                        p.status = fut ? "future" : "paid";
                        p.direction = dir;
                        p.resolution_date = fut ? res : "";
                        p.notes = note;
                        g_app.conn->addPayment(p);
                    }
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

static void OpenResolvePayment(const Payment& p) {
    g_showResolveDialog = true;
    g_resolveDialogId = p.id;
    rb_paid.set(std::to_string(p.amount_due));
}

static void DrawResolveDialog() {
    if (g_showResolveDialog && !g_resolveDialogWasOpen) {
        ImGui::OpenPopup("Επίλυση Μελλοντικής Πληρωμής");
    }
    g_resolveDialogWasOpen = g_showResolveDialog;
    if (!g_showResolveDialog) return;
    ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_Appearing);
    if (BeginGuardedModal("Επίλυση Μελλοντικής Πληρωμής", g_showResolveDialog, ImGuiWindowFlags_AlwaysAutoResize)) {
        Payment p = g_app.conn->getPaymentById(g_resolveDialogId);
        ImGui::Text("%s / %s", g_app.userName(p.user_id).c_str(), g_app.serviceLabel(p.service_id).c_str());
        ImGui::TextDisabled("Αρχική ημερομηνία: %04d-%02d-%02d (δεν αλλάζει)", p.year, p.month, p.day);
        if (!p.resolution_date.empty()) ImGui::TextDisabled("Ημ/νία επίλυσης: %s", p.resolution_date.c_str());
        ImGui::Separator();
        ImGui::InputText("Τελικό πληρωμένο ποσό", rb_paid.data, sizeof(rb_paid.data));
        ImGui::Spacing();
        if (ImGui::Button("Επίλυση", ImVec2(150, 0))) {
            Payment np = p; // preserve year/month/day and everything else not explicitly changed
            np.amount_paid = atof(rb_paid.str().c_str());
            np.balance = np.amount_due - np.amount_paid;
            np.status = "paid";
            np.resolved_at = CurrentTimestampString();
            int id = g_resolveDialogId;
            std::vector<FieldDiff> diffs = {
                {"Κατάσταση", p.status, np.status},
                {"Πληρωμένο", std::to_string(p.amount_paid), std::to_string(np.amount_paid)},
                {"Επιλύθηκε στις", "-", np.resolved_at},
            };
            RequestConfirm("Επίλυση μελλοντικής πληρωμής", diffs, [id, np]() {
                g_app.conn->editPayment(id, np);
                g_app.reloadPayments();
            });
            g_showResolveDialog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Ακύρωση", ImVec2(150, 0))) { g_showResolveDialog = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

static const ListColDef kPaymentListCols[10] = {
    { "ΚΩΔ",        ImGuiTableColumnFlags_WidthFixed, 100.0f, true  },
    { "ΗΜ/ΝΙΑ",     ImGuiTableColumnFlags_WidthFixed, 140.0f, true  },
    { "ΠΕΛΑΤΗΣ",    ImGuiTableColumnFlags_WidthFixed, 200.0f, true  },
    { "ΥΠΗΡΕΣΙΑ",   ImGuiTableColumnFlags_WidthFixed, 260.0f, true  },
    { "ΤΥΠΟΣ",      ImGuiTableColumnFlags_WidthFixed, 120.0f, true  },
    { "ΚΑΤΑΣΤΑΣΗ",  ImGuiTableColumnFlags_WidthFixed, 150.0f, true  },
    { "ΟΦΕΙΛΗ",     ImGuiTableColumnFlags_WidthFixed, 120.0f, true  },
    { "ΠΛΗΡΩΜΕΝΟ",  ImGuiTableColumnFlags_WidthFixed, 150.0f, true  },
    { "ΣΗΜΕΙΩΣΕΙΣ", ImGuiTableColumnFlags_WidthFixed, 190.0f, true  },
    { "ΕΝΕΡΓΕΙΕΣ",  ImGuiTableColumnFlags_WidthFixed, 310.0f, false },
};
static std::vector<int> g_paymentsColOrder = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
static const char* const kPaymentSortNames[] = { "ΚΩΔ", "ΗΜ/ΝΙΑ", "ΠΕΛΑΤΗΣ", "ΥΠΗΡΕΣΙΑ", "ΤΥΠΟΣ", "ΚΑΤΑΣΤΑΣΗ", "ΟΦΕΙΛΗ", "ΠΛΗΡΩΜΕΝΟ", "ΣΗΜΕΙΩΣΕΙΣ" };
static const int kPaymentSortCols[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };

static void DrawPaymentsTable(const std::vector<const Payment*>& rows) {
    if (!ImGui::BeginTable("paymentstable", 10,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX |
            ImGuiTableFlags_Resizable | ImGuiTableFlags_Hideable)) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ListSetupColumns(g_paymentsColOrder, kPaymentListCols);
    ListHeaderRow(g_paymentsColOrder, kPaymentListCols, g_paymentsSort, g_paymentsPager);
    auto P = [&](int logical) { return ListPos(g_paymentsColOrder, logical); };
    for (const Payment* pp : rows) {
        const Payment& p = *pp;
        ImGui::PushID(p.id);
        ImGui::TableNextRow();
        ImGuiID rowId = 0;
        { char idb[16]; std::snprintf(idb, sizeof(idb), "%d", p.id); ImGui::TableSetColumnIndex(P(0)); rowId = RowSelectable(idb); }
        ImGui::TableSetColumnIndex(P(1)); ImGui::Text("%04d-%02d-%02d", p.year, p.month, p.day);
        ImGui::TableSetColumnIndex(P(2)); ImGui::TextUnformatted(TruncToWidth(g_app.userName(p.user_id), ImGui::GetContentRegionAvail().x).c_str());
        ImGui::TableSetColumnIndex(P(3)); ImGui::TextUnformatted(TruncToWidth(g_app.serviceLabel(p.service_id), ImGui::GetContentRegionAvail().x).c_str());
        ImGui::TableSetColumnIndex(P(4)); ImGui::TextColored(DirectionColor(p.direction), "%s", DirectionBadge(p.direction));
        ImGui::TableSetColumnIndex(P(5)); ImGui::TextColored(StatusColor(p.status), "%s", p.status.c_str());
        ImGui::TableSetColumnIndex(P(6)); ImGui::Text("%.2f", p.amount_due);
        ImGui::TableSetColumnIndex(P(7)); ImGui::Text("%.2f", p.amount_paid);
        ImGui::TableSetColumnIndex(P(8)); ImGui::TextUnformatted(TruncToWidth(p.notes, ImGui::GetContentRegionAvail().x).c_str());
        ImGui::TableSetColumnIndex(P(9));
        if (ImGui::SmallButton("Προβολή")) OpenPaymentView(p.id);
        ImGui::SameLine();
        if (ImGui::SmallButton("Επεξ.")) {
            g_paymentDialogIsEdit = true; g_paymentDialogId = p.id;
            pb_userId = p.user_id; pb_serviceId = p.service_id;
            char db[16];
            std::snprintf(db, sizeof(db), "%04d-%02d-%02d", p.year, p.month, p.day);
            pb_dateStr = db;
            pb_due.set(std::to_string(p.amount_due)); pb_paid.set(std::to_string(p.amount_paid));
            pb_isFuture = (p.status == "future"); pb_notes.set(p.notes); pb_resolutionDate = p.resolution_date;
            pb_direction = p.direction == "out" ? 1 : 0;
            pb_lockService = false;   // editing an existing payment -- service is editable
            g_showPaymentDialog = true;
        }
        if (p.status == "future") {
            ImGui::SameLine();
            if (ImGui::SmallButton("Επίλυση")) OpenResolvePayment(p);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Διαγρ.")) {
            int id = p.id;
            RequestConfirm("Διαγραφή πληρωμής", { {"Πληρωμή #", std::to_string(id), "(διαγραφή)"} }, [id]() {
                g_app.conn->removePayment(id); g_app.reloadPayments();
            });
        }
        if (RowDoubleClicked(rowId)) OpenPaymentView(p.id);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

static void DrawPaymentsScreen() {
    ImGui::TextDisabled("Προβολή πληρωμών. Για νέα χρέωση: Υπηρεσίες → Χρέωση, ή Πακέτα → Χρέωση πακέτου.");
    ImGui::SameLine();
    if (ImGui::Button("Ανανέωση")) { g_app.reloadPayments(); g_paymentsPager.Reset(); }
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
    ImGui::Separator();
    ImGui::TextDisabled("Φίλτρα:");
    ImGui::TextDisabled("Από:"); ImGui::SameLine();
    DatePicker("payfilt_start", g_payStartDate, ImVec2(0, 1));
    ImGui::SameLine();
    ImGui::TextDisabled("Έως:"); ImGui::SameLine();
    DatePicker("payfilt_end", g_payEndDate, ImVec2(0, 1));
    ImGui::SameLine();

    // Label on the LEFT, value/selector on the RIGHT -- "Χρήστης:  [Όλοι ▾]"
    // rather than ImGui's default "[Όλοι ▾] Χρήστης". Each combo uses a hidden
    // ##id so it doesn't re-render a visible label next to the widget.
    ImGui::TextDisabled("Χρήστης:"); ImGui::SameLine();
    ImGui::SetNextItemWidth(180);
    if (ImGui::BeginCombo("##filt_user", g_payUserFilter < 0 ? "Όλοι" : g_app.userName(g_payUserFilter).c_str())) {
        if (ImGui::Selectable("Όλοι", g_payUserFilter < 0)) g_payUserFilter = -1;
        for (auto& u : g_app.users) if (ImGui::Selectable(u.full_name.c_str(), u.id == g_payUserFilter)) g_payUserFilter = u.id;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Υπηρεσία:"); ImGui::SameLine();
    ImGui::SetNextItemWidth(180);
    if (ImGui::BeginCombo("##filt_service", g_payServiceFilter < 0 ? "Όλες" : g_app.serviceLabel(g_payServiceFilter).c_str())) {
        if (ImGui::Selectable("Όλες", g_payServiceFilter < 0)) g_payServiceFilter = -1;
        for (auto& s : g_app.services) if (ImGui::Selectable(s.label.c_str(), s.id == g_payServiceFilter)) g_payServiceFilter = s.id;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Κατάσταση:"); ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::Combo("##filt_status", &g_payStatusFilter, kStatusNames, 3);
    ImGui::SameLine();
    ImGui::TextDisabled("Τύπος:"); ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    ImGui::Combo("##filt_direction", &g_payDirectionFilter, kDirectionFilterNames, 3);
    ImGui::Separator();

    // --- Detect filter changes -> reset pager ---
    static std::string lastStart, lastEnd;
    static int lastUser = -2, lastSvc = -2, lastStat = -1, lastDir = -1;
    if (lastStart != g_payStartDate || lastEnd != g_payEndDate ||
        lastUser != g_payUserFilter || lastSvc != g_payServiceFilter ||
        lastStat != g_payStatusFilter || lastDir != g_payDirectionFilter) {
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

    // --- Query one page from SQL ---
    std::vector<Payment> page = g_app.conn->pagePayments(
        g_payStartDate, g_payEndDate,
        g_payUserFilter, g_payServiceFilter,
        statusStr, dirStr,
        g_paymentsPager.offset, g_paymentsPager.pageSize, g_paymentsSort.keys);
    g_paymentsPager.total = g_app.conn->countPayments(
        g_payStartDate, g_payEndDate,
        g_payUserFilter, g_payServiceFilter,
        statusStr, dirStr);

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
        ImGui::Text("%s / %s", g_app.userName(p.user_id).c_str(), g_app.serviceLabel(p.service_id).c_str());
        ImGui::Text("%04d-%02d-%02d", p.year, p.month, p.day);
        ImGui::TextColored(col, "%s", p.status.c_str());
        ImGui::Text("Οφειλή: %.2f  Πληρωμένο: %.2f", p.amount_due, p.amount_paid);
        if (showResDate) ImGui::TextDisabled("Ημ/νία επίλυσης: %s", p.resolution_date.c_str());
        if (showResolvedAt) ImGui::TextDisabled("Επιλύθηκε: %s", p.resolved_at.c_str());
        if (!p.notes.empty()) ImGui::TextWrapped("%s", p.notes.c_str());
        if (ImGui::SmallButton("Προβολή")) OpenPaymentView(p.id);
        ImGui::SameLine();
        if (ImGui::SmallButton("Επεξ.")) {
            g_paymentDialogIsEdit = true; g_paymentDialogId = p.id;
            pb_userId = p.user_id; pb_serviceId = p.service_id;
                        char db[16];
            std::snprintf(db, sizeof(db), "%04d-%02d-%02d", p.year, p.month, p.day);
            pb_dateStr = db;
            pb_due.set(std::to_string(p.amount_due)); pb_paid.set(std::to_string(p.amount_paid));
            pb_isFuture = (p.status == "future"); pb_notes.set(p.notes); pb_resolutionDate = p.resolution_date;
            pb_direction = p.direction == "out" ? 1 : 0;
            pb_lockService = false;   // editing an existing payment -- service is editable
            g_showPaymentDialog = true;
        }
        if (p.status == "future") {
            ImGui::SameLine();
            if (ImGui::SmallButton("Επίλυση")) OpenResolvePayment(p);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Διαγρ.")) {
            int id = p.id;
            RequestConfirm("Διαγραφή πληρωμής", { {"Πληρωμή #", std::to_string(id), "(διαγραφή)"} }, [id]() {
                g_app.conn->removePayment(id); g_app.reloadPayments();
            });
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

static void OpenAddPaymentForService(int serviceId) {
    auto roster = g_app.conn->getUsersForService(serviceId);
    if (roster.empty()) return; // shouldn't happen: a service always has >=1 owner
    if (roster.size() == 1) {
        OpenAddPayment();
        pb_serviceId = serviceId;
        pb_lockService = true;
        pb_userId = roster[0].id;
    } else {
        g_payPickServiceId = serviceId;
        g_showPayOwnerPick = true;
    }
}

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


static std::map<int, int> g_profileHoveredSvc; // uid -> currently-shown service id

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
        ImGui::Begin(title.c_str(), nullptr, ImGuiWindowFlags_NoCollapse);

        // --- Top-right Κλείσιμο -----------------------------------------
        {
            const char* closeLbl = "Κλείσιμο";
            float btnW = ImGui::CalcTextSize(closeLbl).x + ImGui::GetStyle().FramePadding.x * 2.0f;
            float targetX = ImGui::GetWindowContentRegionMax().x - btnW;
            if (targetX > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(targetX);
            if (ImGui::Button(closeLbl)) closeThis = true;
        }

        auto allPayments = g_app.conn->getPaymentsForUser(u.id);

        // --- Per-user aggregates. Future payments excluded (scheduled, not real).
        //     "Οφείλονται να εισπραχθούν" = in-direction balance still outstanding.
        //     "Οφείλονται να πληρωθούν"  = out-direction balance still outstanding.
        double totalCollected      = 0.0;
        double totalPaidOut        = 0.0;
        double owedToBeCollected   = 0.0;
        double owedToBePaid        = 0.0;
        for (auto& p : allPayments) {
            if (p.status == "future") continue;
            if (p.direction == "out") {
                totalPaidOut += p.amount_paid;
                if (p.balance > 0.005) owedToBePaid += p.balance;
            } else {
                totalCollected += p.amount_paid;
                if (p.balance > 0.005) owedToBeCollected += p.balance;
            }
        }

        // --- Stat bar: 4 equal columns, shrink-to-fit labels/values. -----
        char buf[64];
        if (ImGui::BeginTable("userstats", 4,
                ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::PushID(0); {
                float w = ImGui::GetContentRegionAvail().x;
                std::snprintf(buf, sizeof(buf), "%.2f €", totalCollected);
                StatTile("Εισπράχθηκαν", buf, ImVec4(0.35f,0.80f,0.50f,1), nullptr, ImVec2(w, 88));
            } ImGui::PopID();

            ImGui::TableNextColumn();
            ImGui::PushID(1); {
                float w = ImGui::GetContentRegionAvail().x;
                std::snprintf(buf, sizeof(buf), "%.2f €", totalPaidOut);
                StatTile("Πληρώθηκαν", buf, ImVec4(0.90f,0.40f,0.40f,1), nullptr, ImVec2(w, 88));
            } ImGui::PopID();

            ImGui::TableNextColumn();
            ImGui::PushID(2); {
                float w = ImGui::GetContentRegionAvail().x;
                std::snprintf(buf, sizeof(buf), "%.2f €", owedToBeCollected);
                StatTile("Οφείλονται να εισπραχθούν", buf, ImVec4(0.95f,0.65f,0.25f,1), nullptr, ImVec2(w, 88));
            } ImGui::PopID();

            ImGui::TableNextColumn();
            ImGui::PushID(3); {
                float w = ImGui::GetContentRegionAvail().x;
                std::snprintf(buf, sizeof(buf), "%.2f €", owedToBePaid);
                StatTile("Οφείλονται να πληρωθούν", buf, ImVec4(0.90f,0.65f,0.20f,1), nullptr, ImVec2(w, 88));
            } ImGui::PopID();

            ImGui::EndTable();
        }

        ImGui::Spacing();

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

        // --- Action buttons ---------------------------------------------
        ImGui::Separator();

        // --- Services (left) + hovered service's payments (right) -------
        // Services tied to this client = the ones they have payments on.
        std::vector<Service> services;
        {
            std::set<int> seen;
            for (auto& p : allPayments) if (seen.insert(p.service_id).second) services.push_back(g_app.conn->getServiceById(p.service_id));
            std::sort(services.begin(), services.end(), [](const Service& a, const Service& b) { return a.label < b.label; });
        }

        int& hovered = g_profileHoveredSvc[uid];
        if (hovered == 0) hovered = services.empty() ? -1 : services[0].id;

        // Proportional split; the right pane gets the remainder. Both children
        // fill remaining vertical space (-1), so resizing the window just
        // redistributes the same layout.
        float halfW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.42f;

        ImGui::BeginChild("profServicesCol", ImVec2(halfW, -1), true);
        ImGui::TextColored(ImVec4(0.70f,0.85f,1.0f,1.0f), "Υπηρεσίες");
        ImGui::Separator();
        if (services.empty()) {
            ImGui::TextDisabled("Καμία υπηρεσία ακόμα.");
        } else {
            for (auto& s : services) {
                int svcPayCount = 0;
                for (auto& p : allPayments) if (p.service_id == s.id) svcPayCount++;
                ImGui::PushID(s.id);
                bool isShown = (s.id == hovered);
                std::string line = s.label + "  (" + std::to_string(svcPayCount) + " πληρ.)";
                bool clicked = ImGui::Selectable(line.c_str(), isShown);
                if (ImGui::IsItemHovered()) hovered = s.id;
                if (clicked) OpenServiceDetail(s.id);
                ImGui::PopID();
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("profPaymentsCol", ImVec2(0, -1), true);
        if (hovered < 0) {
            ImGui::TextDisabled("Επιλέξτε μια υπηρεσία για να δείτε τις πληρωμές της.");
        } else {
            Service hs = g_app.conn->getServiceById(hovered);
            ImGui::TextColored(ImVec4(0.70f,0.85f,1.0f,1.0f),
                               "Πληρωμές: %s", hs.label.c_str());
            ImGui::Separator();
            bool any = false;
            for (auto& p : allPayments) {
                if (p.service_id != hovered) continue;
                any = true;
                ImGui::PushID(p.id);
                char amtBuf[48];
                std::snprintf(amtBuf, sizeof(amtBuf), "%.2f / %.2f", p.amount_paid, p.amount_due);
                std::string line = std::string(DirectionBadge(p.direction)) + "  " +
                                   std::to_string(p.year) + "-" +
                                   std::to_string(p.month) + "-" +
                                   std::to_string(p.day) + "   " +
                                   amtBuf + "   " + PaymentStatusLabel(p);
                ImGui::PushStyleColor(ImGuiCol_Text, DirectionColor(p.direction));
                if (ImGui::Selectable(line.c_str())) OpenPaymentView(p.id);
                ImGui::PopStyleColor();
                ImGui::PopID();
            }
            if (!any) ImGui::TextDisabled("Καμία πληρωμή για αυτή την υπηρεσία.");
        }
        ImGui::EndChild();

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
static void DrawServiceDetailWindow() {
    std::vector<int> ids = g_detailServiceIds;
    for (size_t i = 0; i < ids.size(); ++i) {
        int sid = ids[i];
        Service s = g_app.conn->getServiceById(sid);
        bool open = true;

        ImGuiIO& io = ImGui::GetIO();
        float cascade = (float)i * 26.0f;
        std::string title = "Υπηρεσία: " + s.label + "###service_" + std::to_string(sid);
        ImGui::SetNextWindowSize(ImVec2(760, 640), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 800.0f - cascade, 80.0f + cascade),
                                ImGuiCond_FirstUseEver);
        ImGui::Begin(title.c_str(), &open);

        // --- Attributes + actions -----------------------------------------
        ImGui::TextColored(ImVec4(0.35f,0.80f,0.50f,1), "Προτεινόμενη τιμή: %.2f €", s.suggested_price);
        if (!s.extra_notes.empty()) ImGui::TextWrapped("%s", s.extra_notes.c_str());
        if (!s.created_at.empty()) ImGui::TextDisabled("Δημιουργήθηκε: %s", s.created_at.c_str());
        if (ImGui::SmallButton("Χρέωση")) OpenChargeService(s.id);
        ImGui::SameLine();
        if (ImGui::SmallButton("Επεξεργασία")) {
            g_serviceDialogIsEdit = true; g_serviceDialogId = s.id;
            sb_label.set(s.label); sb_notes.set(s.extra_notes); sb_price = s.suggested_price;
            g_showServiceDialog = true;
        }
        ImGui::Separator();

        // --- Logistics (this service alone) -------------------------------
        auto payments = g_app.conn->getPaymentsForService(s.id);
        struct Agg { int count = 0; double due = 0, paid = 0, owed = 0, scheduled = 0; };
        std::map<int, Agg> perClient;
        double collected = 0, paidOut = 0, owedIn = 0, owedOut = 0, scheduled = 0;
        for (auto& p : payments) {
            auto& a = perClient[p.user_id];
            a.count++;
            if (p.status == "future") { scheduled += p.amount_due; a.scheduled += p.amount_due; continue; }
            a.due += p.amount_due; a.paid += p.amount_paid;
            if (p.balance > 0.005) a.owed += p.balance;
            if (p.direction == "out") { paidOut += p.amount_paid; if (p.balance > 0.005) owedOut += p.balance; }
            else                      { collected += p.amount_paid; if (p.balance > 0.005) owedIn += p.balance; }
        }
        ImGui::TextDisabled("%d πελάτες  ·  %d πληρωμές", (int)perClient.size(), (int)payments.size());
        char buf[64];
        if (ImGui::BeginTable("svcstats", 5, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableNextRow();
            struct Tile { const char* label; double v; ImVec4 col; };
            Tile tiles[5] = {
                {"Εισπράχθηκαν",              collected, ImVec4(0.35f,0.80f,0.50f,1)},
                {"Πληρώθηκαν",                paidOut,   ImVec4(0.90f,0.40f,0.40f,1)},
                {"Οφείλονται να εισπραχθούν", owedIn,    ImVec4(0.95f,0.65f,0.25f,1)},
                {"Οφείλονται να πληρωθούν",   owedOut,   ImVec4(0.90f,0.65f,0.20f,1)},
                {"Προγραμματισμένες",         scheduled, ImVec4(0.60f,0.70f,0.85f,1)} };
            for (int k = 0; k < 5; k++) {
                ImGui::TableNextColumn(); ImGui::PushID(k);
                float w = ImGui::GetContentRegionAvail().x;
                std::snprintf(buf, sizeof(buf), "%.2f €", tiles[k].v);
                StatTile(tiles[k].label, buf, tiles[k].col, nullptr, ImVec2(w, 80));
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        // --- Analysis: clients tied to this service through payments -------
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.70f,0.85f,1.0f,1.0f), "Πελάτες (μέσω πληρωμών)");
        std::vector<std::pair<int, Agg>> clients(perClient.begin(), perClient.end());
        std::sort(clients.begin(), clients.end(), [](auto& a, auto& b) {
            if (a.second.count != b.second.count) return a.second.count > b.second.count;
            return g_app.userName(a.first) < g_app.userName(b.first);
        });
        float avail = ImGui::GetContentRegionAvail().y;
        if (ImGui::BeginTable("svcclients", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                              ImVec2(0, std::max(90.0f, avail * 0.38f)))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Πελάτης",   ImGuiTableColumnFlags_WidthStretch, 2.4f);
            ImGui::TableSetupColumn("Πληρωμές",  ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("Χρεώσεις",  ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Πληρωμένο", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Οφείλεται", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Μελλοντικά", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableHeadersRow();
            for (auto& [uid, a] : clients) {
                ImGui::PushID(uid);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f,0.78f,1.0f,1.0f));
                if (ImGui::Selectable(g_app.userName(uid).c_str())) OpenUserProfile(uid);
                ImGui::PopStyleColor();
                ImGui::TableSetColumnIndex(1); ImGui::Text("%d", a.count);
                ImGui::TableSetColumnIndex(2); ImGui::Text("%.2f €", a.due);
                ImGui::TableSetColumnIndex(3); ImGui::Text("%.2f €", a.paid);
                ImGui::TableSetColumnIndex(4);
                ImGui::TextColored(a.owed > 0.005 ? ImVec4(0.95f,0.65f,0.25f,1) : ImVec4(0.55f,0.58f,0.65f,1), "%.2f €", a.owed);
                ImGui::TableSetColumnIndex(5); ImGui::Text("%.2f €", a.scheduled);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (clients.empty()) ImGui::TextDisabled("Κανένας πελάτης δεν έχει χρεωθεί αυτή την υπηρεσία ακόμα.");

        // --- All payments on this service ---------------------------------
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.70f,0.85f,1.0f,1.0f), "Πληρωμές");
        ImGui::BeginChild("svcPayments", ImVec2(0, -1), true);
        for (auto& p : payments) {
            ImGui::PushID(p.id);
            std::string line = std::string(DirectionBadge(p.direction)) + "  " +
                g_app.userName(p.user_id) + "  |  " +
                std::to_string(p.year) + "-" + std::to_string(p.month) + "-" + std::to_string(p.day) +
                "  " + p.status + "  " + std::to_string(p.amount_paid) + "/" + std::to_string(p.amount_due) +
                (p.status == "future" && !p.resolution_date.empty() ? "  (αναμ. " + p.resolution_date + ")" : std::string());
            ImGui::PushStyleColor(ImGuiCol_Text, DirectionColor(p.direction));
            bool clicked = ImGui::Selectable(line.c_str());
            ImGui::PopStyleColor();
            if (clicked) OpenPaymentView(p.id);
            if (p.status == "future") {
                ImGui::SameLine();
                if (ImGui::SmallButton("Επίλυση")) OpenResolvePayment(p);
                ImGui::SameLine();
                if (ImGui::SmallButton("Επεξ.")) {
                    g_paymentDialogIsEdit = true; g_paymentDialogId = p.id;
                    pb_userId = p.user_id; pb_serviceId = p.service_id;
                    char db[16];
                    std::snprintf(db, sizeof(db), "%04d-%02d-%02d", p.year, p.month, p.day);
                    pb_dateStr = db;
                    pb_due.set(std::to_string(p.amount_due)); pb_paid.set(std::to_string(p.amount_paid));
                    pb_isFuture = (p.status == "future"); pb_notes.set(p.notes); pb_resolutionDate = p.resolution_date;
                    pb_direction = p.direction == "out" ? 1 : 0;
                    pb_lockService = false;   // editing an existing payment -- service is editable
                    g_showPaymentDialog = true;
                }
            }
            ImGui::PopID();
        }
        if (payments.empty()) ImGui::TextDisabled("Καμία πληρωμή ακόμα.");
        ImGui::EndChild();
        ImGui::End();

        if (!open) {
            g_detailServiceIds.erase(std::remove(g_detailServiceIds.begin(), g_detailServiceIds.end(), sid),
                                     g_detailServiceIds.end());
        }
    }
}


// ---------------------------------------------------------------------------
// RESIDENT LEDGER: opened from a building's resident list. A separate window with
// that resident's money organised for the building context: totals, a month-by-month
// breakdown (building dues are monthly), then their services grouped by package, each
// with its own payments. Payments always stay per client; the building only gives the
// context (where the client lives).
// ---------------------------------------------------------------------------
static void DrawResidentLedgerWindow() {
    static const char* kMonths[] = { "Ιανουάριος","Φεβρουάριος","Μάρτιος","Απρίλιος","Μάιος","Ιούνιος",
                                     "Ιούλιος","Αύγουστος","Σεπτέμβριος","Οκτώβριος","Νοέμβριος","Δεκέμβριος" };
    std::vector<int> ids = g_ledgerUserIds;
    for (size_t i = 0; i < ids.size(); ++i) {
        int uid = ids[i];
        User u = g_app.conn->getUserById(uid);
        bool open = true;
        ImGuiIO& io = ImGui::GetIO();
        float cascade = (float)i * 26.0f;
        std::string title = "Κινήσεις: " + u.full_name + "###ledger_" + std::to_string(uid);
        ImGui::SetNextWindowSize(ImVec2(820, 660), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 880.0f - cascade, 70.0f + cascade), ImGuiCond_FirstUseEver);
        ImGui::Begin(title.c_str(), &open);

        std::string loc = u.address.empty() ? "(χωρίς διαμέρισμα)" : u.address;
        if (!u.apartment.empty())   loc += "  ·  διαμ. " + u.apartment;
        if (!u.area.empty())        loc += "  ·  " + u.area;
        if (!u.postal_code.empty()) loc += "  ·  Τ.Κ. " + u.postal_code;
        ImGui::TextColored(ImVec4(0.70f, 0.85f, 1.0f, 1.0f), "%s", loc.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Προφίλ πελάτη")) OpenUserProfile(uid);

        auto pays = g_app.conn->getPaymentsForUser(uid);
        double collected = 0, paidOut = 0, owed = 0, scheduled = 0;
        struct Mo { double in = 0, out = 0, owed = 0, sched = 0; int n = 0; };
        std::map<int, Mo, std::greater<int>> byMonth;     // newest first
        for (auto& p : pays) {
            auto& m = byMonth[p.year * 100 + p.month];
            m.n++;
            if (p.status == "future") { scheduled += p.amount_due; m.sched += p.amount_due; continue; }
            if (p.direction == "out") { paidOut += p.amount_paid; m.out += p.amount_paid; }
            else                      { collected += p.amount_paid; m.in += p.amount_paid; }
            if (p.balance > 0.005) { owed += p.balance; m.owed += p.balance; }
        }

        char buf[48];
        if (ImGui::BeginTable("ledgerstats", 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableNextRow();
            struct Tile { const char* label; double v; ImVec4 col; };
            Tile tiles[4] = { {"Εισπράχθηκαν", collected, ImVec4(0.35f,0.80f,0.50f,1)},
                              {"Πληρώθηκαν", paidOut, ImVec4(0.90f,0.40f,0.40f,1)},
                              {"Οφείλονται", owed, ImVec4(0.95f,0.65f,0.25f,1)},
                              {"Προγραμματισμένες", scheduled, ImVec4(0.60f,0.70f,0.85f,1)} };
            for (int k = 0; k < 4; k++) {
                ImGui::TableNextColumn(); ImGui::PushID(k);
                std::snprintf(buf, sizeof(buf), "%.2f €", tiles[k].v);
                StatTile(tiles[k].label, buf, tiles[k].col, nullptr, ImVec2(ImGui::GetContentRegionAvail().x, 78));
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        // ---- Month by month ----
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.70f, 0.85f, 1.0f, 1.0f), "Ανά μήνα");
        if (byMonth.empty()) {
            ImGui::TextDisabled("Καμία κίνηση ακόμα.");
        } else if (ImGui::BeginTable("ledgermonths", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Μήνας", ImGuiTableColumnFlags_WidthStretch, 1.8f);
            ImGui::TableSetupColumn("Εισπράχθηκαν", ImGuiTableColumnFlags_WidthStretch, 1.2f);
            ImGui::TableSetupColumn("Πληρώθηκαν", ImGuiTableColumnFlags_WidthStretch, 1.2f);
            ImGui::TableSetupColumn("Οφείλεται", ImGuiTableColumnFlags_WidthStretch, 1.1f);
            ImGui::TableSetupColumn("Προγραμματισμένα", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            ImGui::TableSetupColumn("Κινήσεις", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableHeadersRow();
            for (auto& [key, m] : byMonth) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("%s %d", kMonths[(key % 100 - 1 + 12) % 12], key / 100);
                ImGui::TableSetColumnIndex(1); ImGui::TextColored(ImVec4(0.35f,0.80f,0.50f,1), "%.2f €", m.in);
                ImGui::TableSetColumnIndex(2); ImGui::TextColored(ImVec4(0.90f,0.40f,0.40f,1), "%.2f €", m.out);
                ImGui::TableSetColumnIndex(3);
                ImGui::TextColored(m.owed > 0.005 ? ImVec4(0.95f,0.65f,0.25f,1) : ImVec4(0.55f,0.58f,0.65f,1), "%.2f €", m.owed);
                ImGui::TableSetColumnIndex(4); ImGui::Text("%.2f €", m.sched);
                ImGui::TableSetColumnIndex(5); ImGui::Text("%d", m.n);
            }
            ImGui::EndTable();
        }

        // ---- Services grouped by package, each with its own payments ----
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.70f, 0.85f, 1.0f, 1.0f), "Υπηρεσίες και πληρωμές");
        std::map<int, std::string> pkgOf;                  // service -> first package name
        for (auto& ms : g_app.conn->getMasterServiceSummaries())
            for (auto& c : ms.components) if (!pkgOf.count(c.id)) pkgOf[c.id] = ms.masterService.label;
        std::map<std::string, std::map<int, std::vector<const Payment*>>> grouped;   // package -> service -> payments
        for (auto& p : pays) grouped[pkgOf.count(p.service_id) ? pkgOf[p.service_id] : std::string("Άλλες υπηρεσίες")][p.service_id].push_back(&p);
        for (auto& [pkg, svcs] : grouped) {
            double pkgTotal = 0; for (auto& [sid, v] : svcs) for (auto* p : v) pkgTotal += p->amount_due;
            std::string head = pkg + "   —   " + std::to_string((int)(pkgTotal + 0.5)) + " €###pk_" + std::to_string(uid) + pkg;
            if (!ImGui::CollapsingHeader(head.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) continue;
            for (auto& [sid, v] : svcs) {
                double st = 0; for (auto* p : v) st += p->amount_due;
                std::string sh = g_app.serviceLabel(sid) + "   (" + std::to_string(v.size()) + " κινήσεις, " + std::to_string((int)(st + 0.5)) + " €)###sv_" + std::to_string(uid) + "_" + std::to_string(sid);
                ImGui::Indent();
                if (ImGui::TreeNode(sh.c_str())) {
                    std::vector<const Payment*> sorted = v;
                    std::sort(sorted.begin(), sorted.end(), [](const Payment* a, const Payment* b) {
                        return std::make_tuple(a->year, a->month, a->day, a->id) > std::make_tuple(b->year, b->month, b->day, b->id); });
                    for (auto* p : sorted) {
                        ImGui::PushID(p->id);
                        char line[200];
                        std::snprintf(line, sizeof(line), "%04d-%02d-%02d   %s   %.2f / %.2f €   %s", p->year, p->month, p->day,
                                      DirectionBadge(p->direction), p->amount_paid, p->amount_due, PaymentStatusLabel(*p));
                        ImGui::PushStyleColor(ImGuiCol_Text, DirectionColor(p->direction));
                        if (ImGui::Selectable(line)) OpenPaymentView(p->id);
                        ImGui::PopStyleColor();
                        ImGui::PopID();
                    }
                    ImGui::TreePop();
                }
                ImGui::Unindent();
            }
        }
        ImGui::End();

        if (!open) g_ledgerUserIds.erase(std::remove(g_ledgerUserIds.begin(), g_ledgerUserIds.end(), uid), g_ledgerUserIds.end());
    }
}

// Payment Viewer: the bare minimum, read-only, nowhere further to go.
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
        ImGui::Separator();

        // Clickable: opens a *new* profile / service window (or focuses the
        // existing one if this exact id already has a window open).
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.72f, 1.0f, 1.0f));
        if (ImGui::Selectable(("Πελάτης: " + g_app.userName(p.user_id)).c_str()))
            OpenUserProfile(p.user_id);
        if (ImGui::Selectable(("Υπηρεσία: " + g_app.serviceLabel(p.service_id)).c_str()))
            OpenServiceDetail(p.service_id);
        ImGui::PopStyleColor();
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
        ImGui::Text("Οφειλόμενο: %.2f €", p.amount_due);
        ImGui::Text("Πληρωμένο: %.2f €", p.amount_paid);
        ImGui::Text("Κατάσταση: %s", p.status.c_str());
        if (p.status == "future" && !p.resolution_date.empty()) ImGui::Text("Ημ/νία επίλυσης: %s", p.resolution_date.c_str());
        if (!p.resolved_at.empty()) ImGui::TextDisabled("Επιλύθηκε στις: %s", p.resolved_at.c_str());
        if (!p.notes.empty()) ImGui::TextWrapped("Σημειώσεις: %s", p.notes.c_str());
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
    ImVec4 netCol = dayNet >= 0 ? ImVec4(0.35f,0.80f,0.50f,1) : ImVec4(0.90f,0.40f,0.40f,1);
    ImGui::PushStyleColor(ImGuiCol_Text, netCol);
    ImGui::Text("%s%.2f €", dayNet >= 0 ? "+" : "", dayNet);
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
    ImVec4 dirCol = isOut ? ImVec4(0.90f,0.40f,0.40f,1) : ImVec4(0.35f,0.80f,0.50f,1);

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
    std::string sub = g_app.serviceLabel(p.service_id);
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
        ImGui::Text("%s%.2f €", isOut ? "-" : "+", isPaid ? p.amount_paid : p.amount_due);
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
    if (ImGui::Button("Ανανέωση")) { g_app.reloadPayments(); g_dashPageOffset = 0; g_dashScrollToBoundary = true; }

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
            ImVec4 dateCol = days < 0 ? ImVec4(0.90f,0.40f,0.40f,1) : days <= 7 ? ImVec4(0.90f,0.65f,0.20f,1) : ImVec4(0.55f,0.65f,0.72f,1);
            ImGui::PushID(p.id);
            ImGui::TextColored(dateCol, "%s", p.resolution_date.empty() ? "(χωρίς ημ/νία)" : p.resolution_date.c_str());
            ImGui::SameLine();
            ImGui::Text("%s / %s  %.2f€", g_app.userName(p.user_id).c_str(), g_app.serviceLabel(p.service_id).c_str(), p.amount_due);
            if (days < 0) { ImGui::SameLine(); ImGui::TextColored(ImVec4(0.90f,0.40f,0.40f,1), "[ΕΛΗΞΕ]"); }
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

    // --- Compute filtered set + stats ------------------------------------
    std::vector<Payment> shown;
    std::time_t nowT = std::time(nullptr);
    std::tm nowTm{}; localtime_s(&nowTm, &nowT);
    int curYear = nowTm.tm_year + 1900, curMonth = nowTm.tm_mon + 1, curDay = nowTm.tm_mday;

    // --- 3x3 stat spec, verbatim per client: "total outcome, total input, the
    // sum, and then the same of future payments only, and then another time the
    // same with future and the past together as one."
    //   Group A (resolved only, status != "future"): outcome/input from amount_paid.
    //   Group B (future only,   status == "future"): outcome/input from amount_due
    //                                                  (nothing has actually been
    //                                                  paid yet, so amount_due is
    //                                                  the only real figure).
    //   Group C (combined): A + B for each of outcome/input/sum.
    double outcomeA = 0, inputA = 0; // resolved
    double outcomeB = 0, inputB = 0; // future
    for (auto& p : g_app.payments) {
        char payDate[16];
        std::snprintf(payDate, sizeof(payDate), "%04d-%02d-%02d", p.year, p.month, p.day);
        if (!g_dashStartDate.empty() && std::string(payDate) < g_dashStartDate) continue;
        if (!g_dashEndDate.empty()   && std::string(payDate) > g_dashEndDate)   continue;
        if (g_dashUserFilter >= 0 && p.user_id != g_dashUserFilter) continue;
        if (g_dashServiceFilter >= 0 && p.service_id != g_dashServiceFilter) continue;
        shown.push_back(p);

        bool isOut = p.direction == "out";
        if (p.status == "future") {
            if (isOut) outcomeB += p.amount_due; else inputB += p.amount_due;
        } else {
            if (isOut) outcomeA += p.amount_paid; else inputA += p.amount_paid;
        }
    }
    double sumA = inputA - outcomeA;
    double sumB = inputB - outcomeB;
    double outcomeC = outcomeA + outcomeB, inputC = inputA + inputB, sumC = inputC - outcomeC;
    // Newest-first (descending): the client wants "recent 20, then next recent 20"
    // paging further down the list, so the most recent entry must sort to index 0.
    std::sort(shown.begin(), shown.end(), [](const Payment& a, const Payment& b) {
        if (a.year != b.year) return a.year > b.year;
        if (a.month != b.month) return a.month > b.month;
        if (a.day != b.day) return a.day > b.day;
        return a.id > b.id;
    });

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
    if (ImGui::BeginTable("dashstats3x3", 3, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Επιλυμένες Πληρωμές");
        ImGui::TableSetupColumn("Μελλοντικές Πληρωμές");
        ImGui::TableSetupColumn("Σύνολο (Επιλυμένες + Μελλοντικές)");
        ImGui::TableHeadersRow();

        double outcomes[3] = { outcomeA, outcomeB, outcomeC };
        double inputs[3]   = { inputA,   inputB,   inputC   };
        double sums[3]     = { sumA,     sumB,     sumC     };

        ImGui::TableNextRow();
        for (int c = 0; c < 3; c++) {
            ImGui::TableNextColumn();
            ImGui::PushID(c);
            float colW = ImGui::GetContentRegionAvail().x;
            std::snprintf(buf, sizeof(buf), "%.2f €", outcomes[c]);
            StatTile("Σύνολο Εξόδων", buf, ImVec4(0.90f,0.40f,0.40f,1), "Πληρωμές προς τρίτους", ImVec2(colW, 96));
            ImGui::PopID();
        }

        ImGui::TableNextRow();
        for (int c = 0; c < 3; c++) {
            ImGui::TableNextColumn();
            ImGui::PushID(c);
            float colW = ImGui::GetContentRegionAvail().x;
            std::snprintf(buf, sizeof(buf), "%.2f €", inputs[c]);
            StatTile("Σύνολο Εσόδων", buf, ImVec4(0.35f,0.80f,0.50f,1), "Εισπράξεις από πελάτες", ImVec2(colW, 96));
            ImGui::PopID();
        }

        ImGui::TableNextRow();
        for (int c = 0; c < 3; c++) {
            ImGui::TableNextColumn();
            ImGui::PushID(c);
            float colW = ImGui::GetContentRegionAvail().x;
            std::snprintf(buf, sizeof(buf), "%.2f €", sums[c]);
            StatTile("Άθροισμα", buf, sums[c] >= 0 ? ImVec4(0.35f,0.80f,0.50f,1) : ImVec4(0.90f,0.40f,0.40f,1),
                      "Έσοδα μείον έξοδα", ImVec2(colW, 96));
            ImGui::PopID();
        }
        ImGui::EndTable();
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

    // --- Apply the type filter to `shown` (timeline only) -----------------
    if (g_dashTypeFilter != 0) {
        std::vector<Payment> typed;
        typed.reserve(shown.size());
        for (auto& p : shown) {
            if (g_dashTypeFilter == 1 && p.direction != "in")   continue;
            if (g_dashTypeFilter == 2 && p.direction != "out")  continue;
            if (g_dashTypeFilter == 3 && p.status != "future")  continue;
            typed.push_back(p);
        }
        shown.swap(typed);
    }

    if (shown.empty()) {
        ImGui::TextDisabled("Καμία κίνηση στο τρέχον φίλτρο.");
        return;
    }

    // Split shown into future (top section) and resolved (bottom, paged).
    std::vector<Payment> futureList, resolvedList;
    for (auto& p : shown) {
        if (p.status == "future") futureList.push_back(p);
        else                      resolvedList.push_back(p);
    }
    // Future: nearest-due at the BOTTOM (closest to the boundary). More-distant
    // ones sit above -- scroll up to see them. ISO dates sort lexicographically.
    std::sort(futureList.begin(), futureList.end(), [](const Payment& a, const Payment& b) {
        return a.resolution_date > b.resolution_date;
    });
    // Resolved: newest first.
    std::sort(resolvedList.begin(), resolvedList.end(), [](const Payment& a, const Payment& b) {
        if (a.year != b.year) return a.year > b.year;
        if (a.month != b.month) return a.month > b.month;
        if (a.day != b.day) return a.day > b.day;
        return a.id > b.id;
    });

    int pageEnd = std::min((int)resolvedList.size(), g_dashPageOffset + kDashPageSize);

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
        Payment& p = resolvedList[i];
        any = true;
        if (p.year != lastY || p.month != lastM || p.day != lastD) {
            // Day net computed from the FULL resolved list (not just this page)
            // so the banner total is truthful when a day spans a page boundary.
            double dayNet = 0.0;
            for (auto& q : resolvedList) {
                if (q.year == p.year && q.month == p.month && q.day == p.day) {
                    dayNet += (q.direction == "out") ? -q.amount_paid : q.amount_paid;
                }
            }
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
    if (pageEnd < (int)resolvedList.size()) {
        ImGui::Spacing();
        ImGui::Indent(kRowIndent);
        int remaining = (int)resolvedList.size() - pageEnd;
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
            if (g_noteDialogIsEdit) g_app.conn->editNote(g_noteDialogId, n);
            else g_app.conn->addNote(n);
            g_app.reloadNotes();
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
static int g_sqCat = 0;   // 0 Όλα, 1 Πελάτες, 2 Πολυκατοικίες, 3 Διαμερίσματα, 4 Υπηρεσίες, 5 Πακέτα, 6 Πληρωμές
static const char* kSqCats[] = { "Όλα", "Πελάτες", "Πολυκατοικίες", "Διαμερίσματα", "Υπηρεσίες", "Πακέτα", "Πληρωμές" };
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
static int g_sqPage[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };   // pager position per category
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
    std::vector<const Payment*> payments;
    double payDue = 0.0, payPaid = 0.0;
    int Count(int cat) const {
        switch (cat) { case 1: return (int)users.size(); case 2: return (int)buildings.size(); case 3: return (int)apartments.size();
                       case 4: return (int)services.size(); case 5: return (int)packages.size(); case 6: return (int)payments.size(); default: return 0; }
    }
    int Total() const { int t = 0; for (int c = 1; c <= 6; c++) t += Count(c); return t; }
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
    case 1: {
        std::set<int> bought; const bool useBought = form && !sq_u_service.str().empty();
        if (useBought)
            for (auto& p : g_app.payments) { auto it = cx.svcById.find(p.service_id); if (it != cx.svcById.end() && SqHit(it->second->label, sq_u_service)) bought.insert(p.user_id); }
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
    case 6:
        for (auto& p : g_app.payments) {
            auto uit = cx.userById.find(p.user_id); auto sit = cx.svcById.find(p.service_id);
            std::string client = uit != cx.userById.end() ? uit->second->full_name : "";
            std::string svc = sit != cx.svcById.end() ? sit->second->label : "";
            std::string bld = uit != cx.userById.end() ? uit->second->address : "";
            char date[16]; std::snprintf(date, sizeof(date), "%04d-%02d-%02d", p.year, p.month, p.day);
            if (!SqHit(client + " " + svc + " " + p.notes + " " + p.status + " " + date + " " + bld, quick)) continue;
            if (form) {
                if (!SqHit(client, sq_p_client) || !SqHit(svc, sq_p_service) || !SqHit(p.notes, sq_p_notes) || !SqHit(bld, sq_p_building)) continue;
                if (!sq_p_from.empty() && std::string(date) < sq_p_from) continue;
                if (!sq_p_to.empty() && std::string(date) > sq_p_to) continue;
                if (sq_p_dir == 1 && p.direction != "in") continue;
                if (sq_p_dir == 2 && p.direction != "out") continue;
                if (sq_p_status == 1 && (p.status == "future" || p.balance > 0.005)) continue;
                if (sq_p_status == 2 && p.status != "future") continue;
                if (sq_p_status == 3 && !(p.status != "future" && p.balance > 0.005)) continue;
                if (sq_p_min > 0.0 && p.amount_due < sq_p_min) continue;
                if (sq_p_max > 0.0 && p.amount_due > sq_p_max) continue;
            }
            out.payments.push_back(&p);
            out.payDue += p.amount_due; out.payPaid += p.amount_paid;
        }
        break;
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
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.78f, 1.0f, 1.0f));
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
    if (l1Link) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.78f, 1.0f, 1.0f));
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

    // ---- data used by every category ------------------------------------------
    SqContext cx;
    for (auto& u : g_app.users) cx.userById[u.id] = &u;
    for (auto& s : g_app.services) cx.svcById[s.id] = &s;
    cx.summaries = g_app.conn->getMasterServiceSummaries();
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
    for (auto& p : g_app.payments) if (p.apartment_id > 0 && p.status != "future") cx.aptMoves[p.apartment_id]++;
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
        const char* sub = "Πελάτες, πολυκατοικίες, διαμερίσματα, υπηρεσίες, πακέτα και πληρωμές — σε ένα σημείο";
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
    SqResults res;
    for (int c = 1; c <= 6; c++) SqCompute(c, c == g_sqCat, quick, res, cx);

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
                      (int)g_app.users.size(), (int)cx.buildings.size(), nApts, (int)g_app.services.size(), (int)cx.summaries.size(), (int)g_app.payments.size());
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
            ImGui::InputDouble((std::string("##sqm") + hint).c_str(), &v, 0.0, 0.0, "%.2f");
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
        SqResults again;
        SqCompute(g_sqCat, true, quick, again, cx);
        switch (g_sqCat) {
            case 1: res.users = again.users; break;           case 2: res.buildings = again.buildings; break;
            case 3: res.apartments = again.apartments; break; case 4: res.services = again.services; break;
            case 5: res.packages = again.packages; break;
            case 6: res.payments = again.payments; res.payDue = again.payDue; res.payPaid = again.payPaid; break;
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

    // ================================================================ ALL: four equal columns
    if (all) {
        const float resH = ImGui::GetContentRegionAvail().y;
        const float colW = (ImGui::GetContentRegionAvail().x - 3.0f * sp) / 4.0f;
        const int pageSize = std::max(4, (int)((resH - 130.0f) / 50.0f));
        struct Col { int cat; const char* title; ImVec4 accent; };
        const Col cols[4] = { {1, "Πελάτες", ImVec4(0.35f, 0.65f, 1.0f, 1)}, {4, "Υπηρεσίες", ImVec4(0.95f, 0.75f, 0.30f, 1)},
                              {3, "Διαμερίσματα", ImVec4(0.55f, 0.85f, 0.45f, 1)}, {2, "Πολυκατοικίες", ImVec4(0.30f, 0.80f, 0.70f, 1)} };
        for (int ci = 0; ci < 4; ci++) {
            const Col& c = cols[ci];
            if (ci) ImGui::SameLine();
            ImGui::BeginChild((std::string("sqcol") + std::to_string(c.cat)).c_str(), ImVec2(colW, resH), true);
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y + 2), ImVec2(p.x + 4, p.y + ImGui::GetTextLineHeight() + 2), ImGui::ColorConvertFloat4ToU32(c.accent), 2.0f);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12);
            ImGui::TextColored(ImVec4(0.92f, 0.95f, 1.0f, 1.0f), "%s", c.title);
            ImGui::SameLine(); ImGui::TextDisabled("%d", res.Count(c.cat));
            ImGui::Separator();
            const int cnt = res.Count(c.cat);
            int from, to; int pages = SqPageRange(c.cat, cnt, pageSize, from, to);
            const auto hl = SqNeedles(c.cat, quick);
            if (cnt == 0) ImGui::TextDisabled("Κανένα αποτέλεσμα.");
            for (int i = from; i < to; i++) {
                ImGui::PushID(i);
                if (c.cat == 1) {
                    const User& u = *res.users[i];
                    std::string l2 = u.phone + (u.address.empty() ? "" : "  ·  " + u.address + (u.apartment.empty() ? "" : ", " + u.apartment));
                    if (SqRow(u.full_name, l2, hl, colW, true)) OpenUserProfile(u.id);
                } else if (c.cat == 4) {
                    const Service& s = *res.services[i];
                    char pr[32]; std::snprintf(pr, sizeof(pr), "%.2f €", s.suggested_price);
                    auto pit = cx.pkgsOfService.find(s.id);
                    if (SqRow(s.label, std::string(pr) + (pit != cx.pkgsOfService.end() ? "  ·  " + pit->second : ""), hl, colW, true)) OpenServiceDetail(s.id);
                } else if (c.cat == 3) {
                    const SqARow& a = res.apartments[i];
                    if (SqRow(a.b.address + " — " + a.a.label, (a.current.empty() ? "κενό" : a.current) + "  ·  " + std::to_string(a.moves) + " κινήσεις", hl, colW, true)) { g_aptGotoId = a.a.id; g_tabToApartments = true; }
                } else {
                    const SqBRow& b = res.buildings[i];
                    std::string l2 = b.b.area + " · Τ.Κ. " + b.b.postal_code + " · ΑΦΜ " + (b.b.afm.empty() ? "—" : b.b.afm) + " · " + std::to_string(b.apts) + " διαμ.";
                    if (SqRow(b.b.address, l2, hl, colW, true)) { g_aptGotoBuilding = b.b.id; g_tabToApartments = true; }
                }
                ImGui::PopID();
            }
            ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), ImGui::GetWindowHeight() - ImGui::GetFrameHeightWithSpacing() - 8.0f));
            SqPagerDraw(c.cat, cnt, pages, "pg");
            ImGui::EndChild();
        }
        return;
    }

    // ================================================================ ONE CATEGORY: wide, paged table
    ImGui::BeginChild("searchresults", ImVec2(0, 0), false);
    const ImGuiTableFlags tf = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
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
            for (int i = from; i < to; i++) {
                const Service& s = *res.services[i];
                std::set<int> clients;
                for (auto& p : g_app.payments) if (p.service_id == s.id) clients.insert(p.user_id);
                auto pit = cx.pkgsOfService.find(s.id);
                ImGui::PushID(s.id); ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (SqLinkCell(s.label, hl)) OpenServiceDetail(s.id);
                ImGui::TableSetColumnIndex(1); ImGui::Text("%.2f €", s.suggested_price);
                ImGui::TableSetColumnIndex(2); SqCell(pit != cx.pkgsOfService.end() ? pit->second : "—", hl);
                ImGui::TableSetColumnIndex(3); ImGui::Text("%d", (int)clients.size());
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
            for (int i = from; i < to; i++) {
                const auto* ms = res.packages[i];
                double tot = 0; std::string comps;
                for (auto& c : ms->components) { tot += c.suggested_price; comps += (comps.empty() ? "" : ", ") + c.label; }
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); SqCell(ms->masterService.label, hl);
                ImGui::TableSetColumnIndex(1); ImGui::Text("%d", (int)ms->components.size());
                ImGui::TableSetColumnIndex(2); ImGui::Text("%.2f €", tot);
                ImGui::TableSetColumnIndex(3); SqCell(comps, hl);
            }
            ImGui::EndTable();
        }
        SqPagerDraw(g_sqCat, (int)res.packages.size(), pages, "pgk");
    } else if (g_sqCat == 6) {
        beginSec("Πληρωμές", (int)res.payments.size(), ImVec4(0.95f, 0.45f, 0.55f, 1));
        ImGui::TextDisabled("Σύνολο οφειλής: %.2f €   ·   Σύνολο πληρωμένου: %.2f €   (όλων των αποτελεσμάτων)", res.payDue, res.payPaid);
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
            for (int i = from; i < to; i++) {
                const Payment& p = *res.payments[i];
                auto uit = cx.userById.find(p.user_id);
                ImGui::PushID(p.id); ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                char date[16]; std::snprintf(date, sizeof(date), "%04d-%02d-%02d", p.year, p.month, p.day);
                ImGui::PushStyleColor(ImGuiCol_Text, link);
                if (ImGui::Selectable(date, false, ImGuiSelectableFlags_SpanAllColumns)) OpenPaymentView(p.id);
                ImGui::PopStyleColor();
                ImGui::TableSetColumnIndex(1); SqCell(g_app.userName(p.user_id), hl);
                ImGui::TableSetColumnIndex(2); SqCell(g_app.serviceLabel(p.service_id), hl);
                ImGui::TableSetColumnIndex(3); SqCell(uit != cx.userById.end() && !uit->second->address.empty() ? uit->second->address : "—", hl);
                ImGui::TableSetColumnIndex(4); ImGui::TextColored(DirectionColor(p.direction), "%s", DirectionBadge(p.direction));
                ImGui::TableSetColumnIndex(5); ImGui::TextColored(StatusColor(p.status), "%s", p.status.c_str());
                ImGui::TableSetColumnIndex(6); ImGui::Text("%.2f", p.amount_due);
                ImGui::TableSetColumnIndex(7); ImGui::Text("%.2f", p.amount_paid);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        SqPagerDraw(g_sqCat, (int)res.payments.size(), pages, "pgp");
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
        ImGui::TextDisabled("%s", n.created_at.c_str());
        if (ImGui::SmallButton("Επεξ.")) OpenEditNote(n);
        ImGui::SameLine();
        if (ImGui::SmallButton("Διαγρ.")) {
            int id = n.id;
            g_app.conn->removeNote(id);
            g_app.reloadNotes();
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
static long long g_repSelKey = -1;          // selected bucket (-1 = whole visible range)

static long long RepDaysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}
static void RepCivilFromDays(long long z, int& y, int& m, int& d) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = (int)(yoe) + (int)era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    m = (int)(mp < 10 ? mp + 3 : mp - 9);
    y += (m <= 2);
}

struct RepBucket { long long key = 0; std::string label; double collected = 0, paidOut = 0; int n = 0; };

static long long RepKeyOf(const Payment& p) {
    if (g_repPeriod == 2) return p.year;
    if (g_repPeriod == 1) return (long long)p.year * 100 + p.month;
    long long days = RepDaysFromCivil(p.year, p.month, p.day);
    long long wd = ((days + 3) % 7 + 7) % 7;            // Monday = 0
    return days - wd;                                    // key = day number of the week's Monday
}
static std::string RepLabelOf(long long key) {
    char b[32];
    if (g_repPeriod == 2) { std::snprintf(b, sizeof(b), "%lld", key); return b; }
    if (g_repPeriod == 1) {
        static const char* mn[] = { "Ιαν","Φεβ","Μαρ","Απρ","Μάι","Ιούν","Ιούλ","Αύγ","Σεπ","Οκτ","Νοέ","Δεκ" };
        int y = (int)(key / 100), m = (int)(key % 100);
        std::snprintf(b, sizeof(b), "%s %02d", mn[(m - 1) % 12], y % 100); return b;
    }
    int y, m, d; RepCivilFromDays(key, y, m, d);
    std::snprintf(b, sizeof(b), "%02d/%02d", d, m); return b;
}
static long long RepNextKey(long long key) {
    if (g_repPeriod == 2) return key + 1;
    if (g_repPeriod == 1) { int y = (int)(key / 100), m = (int)(key % 100) + 1; if (m > 12) { m = 1; y++; } return (long long)y * 100 + m; }
    return key + 7;
}

// Grouped vertical bars (collected green / paid-out red) with grid, an average line,
// value labels when there is room, and a hover read-out. Returns the clicked bucket index or -1.
static int RepDrawProgression(const std::vector<RepBucket>& bk, long long selKey, float height) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float W = ImGui::GetContentRegionAvail().x;
    ImGui::InvisibleButton("##repchart", ImVec2(W, height));
    bool hovered = ImGui::IsItemHovered();
    const float padL = 70.0f, padB = 28.0f, padT = 14.0f, padR = 14.0f;
    ImVec2 a(p0.x + padL, p0.y + padT), b(p0.x + W - padR, p0.y + height - padB);
    dl->AddRectFilled(p0, ImVec2(p0.x + W, p0.y + height), IM_COL32(24, 26, 34, 215), 8.0f);
    double mx = 1.0, avgIn = 0.0;
    for (auto& x : bk) { mx = std::max(mx, std::max(x.collected, x.paidOut)); avgIn += x.collected; }
    if (!bk.empty()) avgIn /= (double)bk.size();
    double mag = std::pow(10.0, std::floor(std::log10(mx)));
    double ceilV = std::ceil(mx / mag * 2.0) / 2.0 * mag;
    for (int g = 0; g <= 4; g++) {
        float y = b.y - (b.y - a.y) * g / 4.0f;
        dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), IM_COL32(255, 255, 255, g == 0 ? 80 : 26));
        char t[32]; std::snprintf(t, sizeof(t), "%.0f €", ceilV * g / 4.0);
        ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(a.x - ts.x - 8, y - ts.y * 0.5f), IM_COL32(150, 156, 170, 255), t);
    }
    int n = (int)bk.size();
    int clicked = -1, hov = -1;
    if (n > 0) {
        float slot = (b.x - a.x) / n;
        ImVec2 mp = ImGui::GetIO().MousePos;
        if (hovered && mp.x >= a.x && mp.x <= b.x) hov = std::min(n - 1, (int)((mp.x - a.x) / slot));
        float maxLab = 40.0f;
        for (auto& x : bk) maxLab = std::max(maxLab, ImGui::CalcTextSize(x.label.c_str()).x + 14.0f);
        int every = std::max(1, (int)std::ceil(maxLab / std::max(slot, 1.0f)));
        const bool labels = slot > 62.0f;
        for (int i = 0; i < n; i++) {
            float x0 = a.x + slot * i;
            bool sel = bk[i].key == selKey;
            if (sel) { dl->AddRectFilled(ImVec2(x0 + 1, a.y), ImVec2(x0 + slot - 1, b.y), IM_COL32(70, 130, 190, 55), 4.0f);
                       dl->AddRect(ImVec2(x0 + 1, a.y), ImVec2(x0 + slot - 1, b.y), IM_COL32(90, 160, 220, 140), 4.0f); }
            else if (i == hov) dl->AddRectFilled(ImVec2(x0 + 1, a.y), ImVec2(x0 + slot - 1, b.y), IM_COL32(255, 255, 255, 16), 4.0f);
            float bw = std::max(2.0f, std::min(slot * 0.32f, 30.0f));
            float cx = x0 + slot * 0.5f;
            auto bar = [&](double v, float xl, ImU32 col) {
                float h = (float)(v / ceilV) * (b.y - a.y);
                if (h > 0.5f) dl->AddRectFilled(ImVec2(xl, b.y - h), ImVec2(xl + bw, b.y), col, 3.0f, ImDrawFlags_RoundCornersTop);
                if (labels && v > 0.0) {
                    char t[24]; std::snprintf(t, sizeof(t), "%.0f", v);
                    ImVec2 ts = ImGui::CalcTextSize(t);
                    dl->AddText(ImVec2(xl + bw * 0.5f - ts.x * 0.5f, b.y - h - ts.y - 2), IM_COL32(200, 205, 215, 220), t);
                }
            };
            bar(bk[i].collected, cx - bw - 1, IM_COL32(90, 205, 125, 255));
            bar(bk[i].paidOut,   cx + 1,      IM_COL32(230, 100, 100, 255));
            if (i % every == 0) {
                ImVec2 ts = ImGui::CalcTextSize(bk[i].label.c_str());
                dl->AddText(ImVec2(cx - ts.x * 0.5f, b.y + 6), sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(150, 156, 170, 255), bk[i].label.c_str());
            }
        }
        // average of collections (dashed)
        float ya = b.y - (float)(avgIn / ceilV) * (b.y - a.y);
        for (float x = a.x; x < b.x; x += 10.0f) dl->AddLine(ImVec2(x, ya), ImVec2(std::min(x + 5.0f, b.x), ya), IM_COL32(255, 205, 90, 190), 1.5f);
        if (hov >= 0) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(bk[hov].label.c_str());
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.35f, 0.80f, 0.50f, 1), "Εισπράχθηκαν: %.2f €", bk[hov].collected);
            ImGui::TextColored(ImVec4(0.90f, 0.40f, 0.40f, 1), "Πληρώθηκαν: %.2f €", bk[hov].paidOut);
            ImGui::TextDisabled("%d πληρωμές  ·  κλικ για επιλογή περιόδου", bk[hov].n);
            ImGui::EndTooltip();
            if (ImGui::IsMouseClicked(0) && hovered) clicked = hov;
        }
    } else {
        const char* t = "Δεν υπάρχουν πληρωμές για την περίοδο.";
        ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(p0.x + (W - ts.x) * 0.5f, p0.y + height * 0.5f), IM_COL32(150, 156, 170, 255), t);
    }
    return clicked;
}

// Horizontal ranking bars: one row per name, green = collected, red = paid out.
static void RepDrawRanking(const std::vector<std::tuple<std::string, double, double>>& rows, int maxRows) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    int n = std::min((int)rows.size(), maxRows);
    if (n == 0) { ImGui::TextDisabled("Δεν υπάρχουν δεδομένα."); return; }
    double mx = 1.0;
    for (int i = 0; i < n; i++) mx = std::max(mx, std::max(std::get<1>(rows[i]), std::get<2>(rows[i])));
    float W = ImGui::GetContentRegionAvail().x;
    const float rowH = 26.0f, nameW = std::min(230.0f, W * 0.40f), valW = 84.0f;
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(W, rowH * n));
    float barX0 = p0.x + nameW + 6, barW = std::max(20.0f, W - nameW - valW - 12);
    for (int i = 0; i < n; i++) {
        float y = p0.y + rowH * i;
        std::string nm = TruncToWidth(std::get<0>(rows[i]), nameW - 4);
        dl->AddText(ImVec2(p0.x, y + 4), IM_COL32(222, 226, 235, 255), nm.c_str());
        float wc = (float)(std::get<1>(rows[i]) / mx) * barW, wp = (float)(std::get<2>(rows[i]) / mx) * barW;
        dl->AddRectFilled(ImVec2(barX0, y + 3), ImVec2(barX0 + std::max(wc, 1.0f), y + 12), IM_COL32(90, 205, 125, 255), 2.0f);
        dl->AddRectFilled(ImVec2(barX0, y + 14), ImVec2(barX0 + std::max(wp, 1.0f), y + 22), IM_COL32(230, 100, 100, 255), 2.0f);
        char t[32]; std::snprintf(t, sizeof(t), "%.0f €", std::get<1>(rows[i]) + std::get<2>(rows[i]));
        ImVec2 ts = ImGui::CalcTextSize(t);
        dl->AddText(ImVec2(p0.x + W - ts.x, y + 4), IM_COL32(150, 156, 170, 255), t);
    }
}

struct RepRow { std::string name; int id = -1; int n = 0; int others = 0; double in = 0, out = 0, owed = 0; };

static std::string RepPctText(double now, double prev) {
    if (prev <= 0.005) return now > 0.005 ? "νέο" : "—";
    char b[32]; std::snprintf(b, sizeof(b), "%+.0f%%", (now - prev) / prev * 100.0);
    return b;
}

// One breakdown tab: ranking chart on the left, full table on the right.
static void RepDrawBreakdown(const char* id, std::vector<RepRow>& rows, const char* nameCol, const char* othersCol,
                             double grand, std::function<void(int)> open) {
    std::sort(rows.begin(), rows.end(), [](const RepRow& a, const RepRow& b) { return a.in + a.out > b.in + b.out; });
    std::vector<std::tuple<std::string, double, double>> rank;
    for (auto& r : rows) rank.emplace_back(r.name, r.in, r.out);
    float totalW = ImGui::GetContentRegionAvail().x, sp = ImGui::GetStyle().ItemSpacing.x;
    float leftW = (totalW - sp) * 0.42f;
    float h = std::max(300.0f, ImGui::GetContentRegionAvail().y);
    ImGui::BeginChild((std::string(id) + "_chart").c_str(), ImVec2(leftW, h), true);
    ImGui::TextDisabled("Κορυφαία %d βάσει συνολικού ποσού", std::min((int)rows.size(), 10));
    ImGui::Spacing();
    RepDrawRanking(rank, 10);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild((std::string(id) + "_tbl").c_str(), ImVec2(0, h), true);
    if (ImGui::BeginTable((std::string(id) + "_t").c_str(), 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(nameCol, ImGuiTableColumnFlags_WidthStretch, 2.6f);
        ImGui::TableSetupColumn(othersCol, ImGuiTableColumnFlags_WidthFixed, 56.0f);
        ImGui::TableSetupColumn("Πληρ.", ImGuiTableColumnFlags_WidthFixed, 52.0f);
        ImGui::TableSetupColumn("Εισπράχθ.", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Πληρώθ.", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Οφείλεται", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Μ.Ο.", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("Μερίδιο", ImGuiTableColumnFlags_WidthFixed, 66.0f);
        ImGui::TableHeadersRow();
        for (auto& r : rows) {
            ImGui::PushID(r.id * 131 + (int)(&r - rows.data()));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (open && r.id >= 0) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.78f, 1.0f, 1.0f));
                if (ImGui::Selectable(r.name.c_str())) open(r.id);
                ImGui::PopStyleColor();
            } else ImGui::TextUnformatted(r.name.c_str());
            ImGui::TableSetColumnIndex(1); ImGui::Text("%d", r.others);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%d", r.n);
            ImGui::TableSetColumnIndex(3); ImGui::TextColored(ImVec4(0.35f,0.80f,0.50f,1), "%.2f €", r.in);
            ImGui::TableSetColumnIndex(4); ImGui::TextColored(ImVec4(0.90f,0.40f,0.40f,1), "%.2f €", r.out);
            ImGui::TableSetColumnIndex(5); ImGui::TextColored(r.owed > 0.005 ? ImVec4(0.95f,0.65f,0.25f,1) : ImVec4(0.55f,0.58f,0.65f,1), "%.2f €", r.owed);
            ImGui::TableSetColumnIndex(6); ImGui::Text("%.2f €", r.n ? (r.in + r.out) / r.n : 0.0);
            ImGui::TableSetColumnIndex(7); ImGui::Text("%.1f%%", grand > 0.005 ? (r.in + r.out) / grand * 100.0 : 0.0);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

static int g_repTab = 0;

static void DrawReportsScreen() {
    // ================================================================ header
    ImGui::SetWindowFontScale(1.45f);
    ImGui::TextColored(ImVec4(0.92f, 0.95f, 1.0f, 1.0f), "Γενικές Αναφορές");
    ImGui::SetWindowFontScale(1.0f);
    ImGui::TextDisabled("Σύνοψη κινήσεων, πορεία στον χρόνο και ανάλυση ανά υπηρεσία, πελάτη, πολυκατοικία και πακέτο.");
    ImGui::Spacing();

    // segmented period control (joined buttons)
    const char* names[] = { "Εβδομαδιαία", "Μηνιαία", "Ετήσια" };
    ImGui::TextDisabled("Περίοδος");
    ImGui::SameLine();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    for (int i = 0; i < 3; i++) {
        if (i) ImGui::SameLine();
        bool on = g_repPeriod == i;
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, on ? ImVec4(0.24f, 0.55f, 0.75f, 1.0f) : ImVec4(0.16f, 0.18f, 0.24f, 1.0f));
        if (ImGui::Button(names[i], ImVec2(118, 0))) { g_repPeriod = i; g_repSelKey = -1; }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }
    ImGui::PopStyleVar();
    ImGui::SameLine(0, 18);
    if (ImGui::Button("Ανανέωση")) g_app.reloadAll();

    // ================================================================ buckets
    std::map<long long, RepBucket> byKey;
    double scheduled = 0.0;
    for (auto& p : g_app.payments) {
        if (p.status == "future") { scheduled += p.amount_due; continue; }
        long long k = RepKeyOf(p);
        auto& bk = byKey[k];
        bk.key = k; bk.n++;
        (p.direction == "out" ? bk.paidOut : bk.collected) += p.amount_paid;
    }
    std::vector<RepBucket> buckets;
    if (!byKey.empty()) {
        long long last = byKey.rbegin()->first, first = byKey.begin()->first;
        const int maxBuckets = g_repPeriod == 0 ? 20 : (g_repPeriod == 1 ? 14 : 12);
        std::vector<long long> keys;
        for (long long k = first; k <= last && (int)keys.size() < 2000; k = RepNextKey(k)) keys.push_back(k);
        if ((int)keys.size() > maxBuckets) keys.erase(keys.begin(), keys.end() - maxBuckets);
        for (long long k : keys) {
            RepBucket bk = byKey.count(k) ? byKey[k] : RepBucket();
            bk.key = k; bk.label = RepLabelOf(k);
            buckets.push_back(bk);
        }
    }
    long long firstVisible = buckets.empty() ? 0 : buckets.front().key;
    bool selValid = false;
    for (auto& b : buckets) if (b.key == g_repSelKey) selValid = true;
    if (!selValid) g_repSelKey = -1;

    // range caption + selection
    {
        std::string cap = buckets.empty() ? "Καμία κίνηση" : (g_repSelKey >= 0 ? "Περίοδος: " + RepLabelOf(g_repSelKey)
                          : "Διάστημα: " + buckets.front().label + " — " + buckets.back().label + "  (" + std::to_string(buckets.size()) + " περίοδοι)");
        ImGui::TextColored(ImVec4(0.70f, 0.85f, 1.0f, 1.0f), "%s", cap.c_str());
        if (g_repSelKey >= 0) { ImGui::SameLine(); if (ImGui::SmallButton("Εμφάνιση όλου του διαστήματος")) g_repSelKey = -1; }
        else if (!buckets.empty()) { ImGui::SameLine(); ImGui::TextDisabled("— κάντε κλικ σε μια στήλη του γραφήματος για να δείτε μόνο εκείνη την περίοδο"); }
    }
    ImGui::Separator();

    // ================================================================ aggregates
    std::map<int, RepRow> perSvc, perCli, perBld, perPkg;
    std::map<int, std::string> bldOfUser;
    for (auto& u : g_app.users) bldOfUser[u.id] = u.address.empty() ? "Χωρίς διαμέρισμα" : u.address;
    std::map<int, std::string> pkgOfSvc;
    for (auto& ms : g_app.conn->getMasterServiceSummaries())
        for (auto& c : ms.components) if (!pkgOfSvc.count(c.id)) pkgOfSvc[c.id] = ms.masterService.label;
    std::map<std::string, int> bldIdx, pkgIdx;       // name -> synthetic id
    std::map<int, std::set<int>> bldClients, pkgSvcs, svcClients, cliSvcs;
    double collected = 0, paidOut = 0, owed = 0; int payCount = 0;
    std::set<int> activeClients, usedServices;
    for (auto& p : g_app.payments) {
        if (p.status == "future") continue;
        long long k = RepKeyOf(p);
        if (g_repSelKey >= 0 ? k != g_repSelKey : k < firstVisible) continue;
        payCount++;
        double c = p.direction == "out" ? 0.0 : p.amount_paid, o = p.direction == "out" ? p.amount_paid : 0.0;
        double ow = p.balance > 0.005 ? p.balance : 0.0;
        collected += c; paidOut += o; owed += ow;
        activeClients.insert(p.user_id); usedServices.insert(p.service_id);
        auto add = [&](RepRow& r) { r.n++; r.in += c; r.out += o; r.owed += ow; };
        { auto& r = perSvc[p.service_id]; r.id = p.service_id; r.name = g_app.serviceLabel(p.service_id); add(r); svcClients[p.service_id].insert(p.user_id); r.others = (int)svcClients[p.service_id].size(); }
        { auto& r = perCli[p.user_id];    r.id = p.user_id;    r.name = g_app.userName(p.user_id);       add(r); cliSvcs[p.user_id].insert(p.service_id); r.others = (int)cliSvcs[p.user_id].size(); }
        { std::string bn = bldOfUser.count(p.user_id) ? bldOfUser[p.user_id] : "Χωρίς διαμέρισμα";
          int bid = bldIdx.emplace(bn, (int)bldIdx.size()).first->second;
          auto& r = perBld[bid]; r.id = -1; r.name = bn; add(r); bldClients[bid].insert(p.user_id); r.others = (int)bldClients[bid].size(); }
        { std::string pn = pkgOfSvc.count(p.service_id) ? pkgOfSvc[p.service_id] : "Άλλες υπηρεσίες";
          int pid = pkgIdx.emplace(pn, (int)pkgIdx.size()).first->second;
          auto& r = perPkg[pid]; r.id = -1; r.name = pn; add(r); pkgSvcs[pid].insert(p.service_id); r.others = (int)pkgSvcs[pid].size(); }
    }
    // last-vs-previous period (for the tiles' small print)
    double lastIn = 0, prevIn = 0, lastOut = 0, prevOut = 0; std::string lastLbl, prevLbl;
    if (!buckets.empty()) {
        int li = (int)buckets.size() - 1;
        if (g_repSelKey >= 0) for (int i = 0; i < (int)buckets.size(); i++) if (buckets[i].key == g_repSelKey) li = i;
        lastIn = buckets[li].collected; lastOut = buckets[li].paidOut; lastLbl = buckets[li].label;
        if (li > 0) { prevIn = buckets[li - 1].collected; prevOut = buckets[li - 1].paidOut; prevLbl = buckets[li - 1].label; }
    }

    // ================================================================ scrolling body
    ImGui::BeginChild("repbody", ImVec2(0, 0), false);

    // ---- KPI tiles
    {
        char buf[64];
        auto eur = [&](double v) { std::snprintf(buf, sizeof(buf), "%.2f €", v); return std::string(buf); };
        std::string subIn  = lastLbl.empty() ? "" : lastLbl + ": " + eur(lastIn)  + " (" + RepPctText(lastIn, prevIn)  + ")";
        std::string subOut = lastLbl.empty() ? "" : lastLbl + ": " + eur(lastOut) + " (" + RepPctText(lastOut, prevOut) + ")";
        double ratio = (collected + paidOut) > 0.005 ? owed / (collected + paidOut) * 100.0 : 0.0;
        char subOwed[64]; std::snprintf(subOwed, sizeof(subOwed), "%.1f%% του κύκλου εργασιών", ratio);
        char subAvg[64]; std::snprintf(subAvg, sizeof(subAvg), "Μ.Ο. %.2f € ανά πληρωμή", payCount ? (collected + paidOut) / payCount : 0.0);
        char subCli[64]; std::snprintf(subCli, sizeof(subCli), "Μ.Ο. %.2f € ανά πελάτη", activeClients.empty() ? 0.0 : (collected + paidOut) / (double)activeClients.size());
        if (ImGui::BeginTable("reptiles", 6, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableNextRow();
            struct Tile { const char* label; std::string v; ImVec4 col; std::string sub; };
            Tile tiles[6] = {
                {"Εισπράχθηκαν", eur(collected), ImVec4(0.35f,0.80f,0.50f,1), subIn},
                {"Πληρώθηκαν", eur(paidOut), ImVec4(0.90f,0.40f,0.40f,1), subOut},
                {"Οφείλονται", eur(owed), ImVec4(0.95f,0.65f,0.25f,1), subOwed},
                {"Προγραμματισμένες", eur(scheduled), ImVec4(0.60f,0.70f,0.85f,1), "μελλοντικές πληρωμές"},
                {"Πληρωμές", std::to_string(payCount), ImVec4(0.85f,0.88f,0.95f,1), subAvg},
                {"Ενεργοί πελάτες", std::to_string(activeClients.size()), ImVec4(0.85f,0.88f,0.95f,1), subCli} };
            for (int i = 0; i < 6; i++) {
                ImGui::TableNextColumn(); ImGui::PushID(i);
                StatTile(tiles[i].label, tiles[i].v, tiles[i].col, tiles[i].sub.c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 104));
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    // ---- progression
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.70f, 0.85f, 1.0f, 1.0f), "Πορεία στον χρόνο");
    ImGui::SameLine(); ImGui::TextColored(ImVec4(0.35f,0.80f,0.50f,1), "■ Εισπράξεις");
    ImGui::SameLine(); ImGui::TextColored(ImVec4(0.90f,0.40f,0.40f,1), "■ Πληρωμές");
    { double avg = 0; for (auto& b : buckets) avg += b.collected; if (!buckets.empty()) avg /= (double)buckets.size();
      ImGui::SameLine(); ImGui::TextColored(ImVec4(1.0f,0.80f,0.35f,1), "- - Μ.Ο. εισπράξεων %.0f €", avg); }
    int clicked = RepDrawProgression(buckets, g_repSelKey, 260.0f);
    if (clicked >= 0) g_repSelKey = (buckets[clicked].key == g_repSelKey) ? -1 : buckets[clicked].key;

    // ---- findings
    std::vector<RepRow> svcRows, cliRows, bldRows, pkgRows;
    for (auto& [id, r] : perSvc) svcRows.push_back(r);
    for (auto& [id, r] : perCli) cliRows.push_back(r);
    for (auto& [id, r] : perBld) bldRows.push_back(r);
    for (auto& [id, r] : perPkg) pkgRows.push_back(r);
    auto tot = [](const RepRow& r) { return r.in + r.out; };
    auto byTot = [&](const RepRow& a, const RepRow& b) { return tot(a) > tot(b); };
    std::sort(svcRows.begin(), svcRows.end(), byTot); std::sort(cliRows.begin(), cliRows.end(), byTot);
    std::sort(bldRows.begin(), bldRows.end(), byTot); std::sort(pkgRows.begin(), pkgRows.end(), byTot);
    const double grand = collected + paidOut;
    ImGui::Spacing();
    ImGui::BeginChild("repfindings", ImVec2(0, 0), ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY);
    ImGui::TextColored(ImVec4(0.70f, 0.85f, 1.0f, 1.0f), "Ευρήματα");
    if (payCount == 0) {
        ImGui::TextDisabled("Καμία πληρωμή στο επιλεγμένο διάστημα.");
    } else {
        auto share = [&](const RepRow& r) { return grand > 0.005 ? tot(r) / grand * 100.0 : 0.0; };
        double top3 = 0; for (size_t i = 0; i < std::min<size_t>(3, svcRows.size()); i++) top3 += share(svcRows[i]);
        ImGui::BulletText("%d πληρωμές από %d πελάτες για %d υπηρεσίες· εισπράχθηκαν %.2f € και πληρώθηκαν %.2f €, ενώ %.2f € παραμένουν ανεξόφλητα.",
                          payCount, (int)activeClients.size(), (int)usedServices.size(), collected, paidOut, owed);
        if (!svcRows.empty()) ImGui::BulletText("Κορυφαία υπηρεσία: %s (%.2f €, %.1f%% του συνόλου)· οι 3 πρώτες υπηρεσίες αντιστοιχούν στο %.0f%%.",
                          svcRows[0].name.c_str(), tot(svcRows[0]), share(svcRows[0]), top3);
        if (!cliRows.empty()) ImGui::BulletText("Κορυφαίος πελάτης: %s (%.2f €, %.1f%%).", cliRows[0].name.c_str(), tot(cliRows[0]), share(cliRows[0]));
        if (!bldRows.empty()) ImGui::BulletText("Κορυφαία πολυκατοικία: %s (%.2f €, %.1f%%).", bldRows[0].name.c_str(), tot(bldRows[0]), share(bldRows[0]));
        if (!pkgRows.empty()) ImGui::BulletText("Κορυφαίο πακέτο: %s (%.2f €, %.1f%%).", pkgRows[0].name.c_str(), tot(pkgRows[0]), share(pkgRows[0]));
        if (g_repSelKey < 0 && buckets.size() > 1) {
            const RepBucket* best = &buckets[0];
            for (auto& b : buckets) if (b.collected > best->collected) best = &b;
            ImGui::BulletText("Καλύτερη περίοδος εισπράξεων: %s (%.2f €).", best->label.c_str(), best->collected);
        }
    }
    ImGui::EndChild();

    // ---- breakdown tabs
    ImGui::Spacing();
    if (ImGui::BeginTabBar("repbreak")) {
        if (ImGui::BeginTabItem("Ανά υπηρεσία"))     { RepDrawBreakdown("rb_svc", svcRows, "Υπηρεσία", "Πελ.", grand, [](int id) { OpenServiceDetail(id); }); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Ανά πελάτη"))       { RepDrawBreakdown("rb_cli", cliRows, "Πελάτης", "Υπ.", grand, [](int id) { OpenUserProfile(id); }); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Ανά πολυκατοικία")) { RepDrawBreakdown("rb_bld", bldRows, "Πολυκατοικία", "Πελ.", grand, nullptr); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Ανά πακέτο"))       { RepDrawBreakdown("rb_pkg", pkgRows, "Πακέτο", "Υπ.", grand, nullptr); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Στα ποσά προσμετρώνται μόνο πραγματοποιημένες πληρωμές· οι μελλοντικές εμφανίζονται χωριστά ως «Προγραμματισμένες».");
    ImGui::EndChild();
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
static std::map<std::string, int> g_aptPages;
static const int kAptPageSize = 12;

struct AptPeriod { std::string key, label; int n = 0; double in = 0, out = 0, owed = 0; std::set<std::string> services; };

static std::string AptDateLabel(const std::string& ymd) {            // 2026-03-07 -> 07/03/2026
    if (ymd.size() < 10) return ymd;
    return ymd.substr(8, 2) + "/" + ymd.substr(5, 2) + "/" + ymd.substr(0, 4);
}

// Key (sortable) and label of the bucket a payment falls in for the chosen granularity.
static void AptBucketOf(const Payment& p, int period, std::string& key, std::string& label) {
    static const char* mn[] = { "Ιανουάριος","Φεβρουάριος","Μάρτιος","Απρίλιος","Μάιος","Ιούνιος","Ιούλιος","Αύγουστος","Σεπτέμβριος","Οκτώβριος","Νοέμβριος","Δεκέμβριος" };
    char d[16]; std::snprintf(d, sizeof(d), "%04d-%02d-%02d", p.year, p.month, p.day);
    switch (period) {
    case 0: {                                                          // minute (when it was entered)
        std::string t = p.created_at.size() >= 16 ? p.created_at.substr(0, 16) : std::string(d) + " 00:00";
        key = t; label = AptDateLabel(t) + " " + t.substr(11, 5); break; }
    case 1: key = d; label = AptDateLabel(d); break;
    case 2: {
        long long days = RepDaysFromCivil(p.year, p.month, p.day);
        long long mon = days - (((days + 3) % 7) + 7) % 7;
        int y, m, dd; RepCivilFromDays(mon, y, m, dd);
        char k[16]; std::snprintf(k, sizeof(k), "%04d-%02d-%02d", y, m, dd);
        key = k; label = "Εβδομάδα από " + AptDateLabel(k); break; }
    case 3: { char k[16]; std::snprintf(k, sizeof(k), "%04d-%02d", p.year, p.month);
              key = k; label = std::string(mn[(p.month - 1 + 12) % 12]) + " " + std::to_string(p.year); break; }
    default: key = std::to_string(p.year); label = key; break;
    }
}

static std::vector<AptPeriod> AptBuildPeriods(const std::vector<const Payment*>& ps, int period) {
    std::map<std::string, AptPeriod> by;
    for (auto* p : ps) {
        if (p->status == "future") continue;
        std::string key, label; AptBucketOf(*p, period, key, label);
        auto& b = by[key]; b.key = key; b.label = label; b.n++;
        (p->direction == "out" ? b.out : b.in) += p->amount_paid;
        if (p->balance > 0.005) b.owed += p->balance;
        b.services.insert(g_app.serviceLabel(p->service_id));
    }
    std::vector<AptPeriod> out;
    for (auto& [k, v] : by) out.push_back(v);
    std::reverse(out.begin(), out.end());                              // newest first
    return out;
}

// Small pager row ("◄ 2 / 5 (13-24 από 49) ►"); `page` is remembered per id. Returns the first row.
static int AptPagerRow(const std::string& id, int total, int pageSize, int& from, int& to) {
    int& page = g_aptPages[id];
    int pages = std::max(1, (total + pageSize - 1) / pageSize);
    page = std::max(0, std::min(page, pages - 1));
    from = page * pageSize; to = std::min(total, from + pageSize);
    return pages;
}
static void AptPagerDraw(const std::string& id, int total, int pages) {
    if (pages <= 1) return;
    int& page = g_aptPages[id];
    ImGui::PushID(id.c_str());
    if (page == 0) ImGui::BeginDisabled();
    if (ImGui::SmallButton("◄##p")) page--;
    if (page == 0) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("Σελίδα %d / %d  (%d εγγραφές)", page + 1, pages, total);
    ImGui::SameLine();
    if (page >= pages - 1) ImGui::BeginDisabled();
    if (ImGui::SmallButton("►##n")) page++;
    if (page >= pages - 1) ImGui::EndDisabled();
    ImGui::PopID();
}

// Paged table of period rows. The caller passes the COMPLETE list; only the rows shown are sliced.
static void AptDrawPeriodTable(const std::string& id, const std::vector<AptPeriod>& per) {
    if (per.empty()) { ImGui::TextDisabled("Καμία κίνηση."); return; }
    int from, to; int pages = AptPagerRow(id, (int)per.size(), kAptPageSize, from, to);
    if (ImGui::BeginTable(id.c_str(), 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Περίοδος", ImGuiTableColumnFlags_WidthStretch, 1.8f);
        ImGui::TableSetupColumn("Κινήσεις", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Εισπράχθηκαν", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Πληρώθηκαν", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Οφείλεται", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Υπηρεσίες", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableHeadersRow();
        for (int i = from; i < to; i++) {
            auto& r = per[i];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(r.label.c_str());
            ImGui::TableSetColumnIndex(1); ImGui::Text("%d", r.n);
            ImGui::TableSetColumnIndex(2); ImGui::TextColored(ImVec4(0.35f,0.80f,0.50f,1), "%.2f €", r.in);
            ImGui::TableSetColumnIndex(3); ImGui::TextColored(ImVec4(0.90f,0.40f,0.40f,1), "%.2f €", r.out);
            ImGui::TableSetColumnIndex(4); ImGui::TextColored(r.owed > 0.005 ? ImVec4(0.95f,0.65f,0.25f,1) : ImVec4(0.55f,0.58f,0.65f,1), "%.2f €", r.owed);
            std::string svc; for (auto& s : r.services) svc += (svc.empty() ? "" : ", ") + s;
            ImGui::TableSetColumnIndex(5); ImGui::TextUnformatted(TruncToWidth(svc, ImGui::GetContentRegionAvail().x).c_str());
        }
        ImGui::EndTable();
    }
    AptPagerDraw(id, (int)per.size(), pages);
}

static void DrawApartmentsScreen() {
    // ---- jump requested from another tab (Πολυκατοικίες / Αναζήτηση) ----
    if (g_aptGotoId > 0) {
        g_aptSelId = g_aptGotoId; g_aptGotoId = -1;
        for (auto& b : g_app.conn->getBuildings())
            for (auto& a : g_app.conn->getApartmentsForBuilding(b.id)) if (a.id == g_aptSelId) g_aptOpenB.insert(b.id);
    }
    if (g_aptGotoBuilding > 0) { g_aptOpenB.insert(g_aptGotoBuilding); g_aptGotoBuilding = -1; }

    // ---- every apartment with what happened there ----
    std::map<int, std::vector<const Payment*>> payOf;                  // apartment -> its payments
    for (auto& p : g_app.payments) if (p.apartment_id > 0) payOf[p.apartment_id].push_back(&p);
    struct AptEntry { Apartment a; std::string current; int moves = 0; double in = 0, out = 0; };
    struct BldEntry { Building b; std::vector<AptEntry> apts; int moves = 0; double total = 0; };
    std::vector<BldEntry> blds;
    for (auto& b : g_app.conn->getBuildings()) {
        BldEntry be; be.b = b;
        for (auto& a : g_app.conn->getApartmentsForBuilding(b.id)) {
            AptEntry e; e.a = a;
            for (auto& u : g_app.users) if (u.apartment_id == a.id) e.current += (e.current.empty() ? "" : ", ") + u.full_name;
            for (auto* p : payOf[a.id]) { if (p->status == "future") continue; e.moves++; (p->direction == "out" ? e.out : e.in) += p->amount_paid; }
            be.moves += e.moves; be.total += e.in + e.out;
            be.apts.push_back(std::move(e));
        }
        blds.push_back(std::move(be));
    }
    std::sort(blds.begin(), blds.end(), [](const BldEntry& x, const BldEntry& y) { return x.b.address < y.b.address; });
    bool selOk = false; int allApts = 0;
    for (auto& b : blds) for (auto& a : b.apts) { allApts++; if (a.a.id == g_aptSelId) selOk = true; }
    if (!selOk) { g_aptSelId = -1; for (auto& b : blds) if (!b.apts.empty() && g_aptSelId < 0) { g_aptSelId = b.apts[0].a.id; g_aptOpenB.insert(b.b.id); } }
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
        std::snprintf(head, sizeof(head), "%s  %s\n      %s · Τ.Κ. %s · ΑΦΜ %s\n      %d διαμερίσματα · %d κινήσεις · %.0f €",
                      open ? "▼" : "►", b.address.c_str(), b.area.c_str(), b.postal_code.c_str(), b.afm.empty() ? "—" : b.afm.c_str(),
                      (int)be.apts.size(), be.moves, be.total);
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.16f, 0.24f, 0.33f, 0.80f));
        if (ImGui::Selectable(head, true, 0, ImVec2(0, 58))) { if (g_aptOpenB.count(b.id)) g_aptOpenB.erase(b.id); else g_aptOpenB.insert(b.id); }
        ImGui::PopStyleColor();
        if (open) {
            ImGui::Indent(14.0f);
            for (auto* e : aptShow) {
                ImGui::PushID(e->a.id);
                char line[360];
                std::snprintf(line, sizeof(line), "%s\n   %s  ·  %d κινήσεις  ·  %.0f €", e->a.label.c_str(),
                              e->current.empty() ? "κενό" : e->current.c_str(), e->moves, e->in + e->out);
                if (ImGui::Selectable(line, e->a.id == g_aptSelId, 0, ImVec2(0, 42))) g_aptSelId = e->a.id;
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
        auto& ps = payOf[sel->a.id];
        auto residencies = g_app.conn->getResidencies(sel->a.id);
        const std::string pageKey = std::to_string(sel->a.id) + "_" + std::to_string(g_aptPeriod);

        ImGui::SetWindowFontScale(1.2f);
        ImGui::TextColored(ImVec4(0.92f, 0.95f, 1.0f, 1.0f), "%s — διαμέρισμα %s", selB->address.c_str(), sel->a.label.c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::TextDisabled("%s · Τ.Κ. %s · ΑΦΜ %s · Σήμερα: %s", selB->area.c_str(), selB->postal_code.c_str(),
                            selB->afm.empty() ? "—" : selB->afm.c_str(), sel->current.empty() ? "κενό" : sel->current.c_str());

        // complete totals (over everything, never paged)
        double in = 0, out = 0, owed = 0; int n = 0; std::set<int> who;
        for (auto* p : ps) { if (p->status == "future") continue; n++; who.insert(p->user_id); (p->direction == "out" ? out : in) += p->amount_paid; if (p->balance > 0.005) owed += p->balance; }

        if (ImGui::CollapsingHeader("Σύνολα και πορεία", ImGuiTreeNodeFlags_DefaultOpen)) {
            const char* pn[] = { "Λεπτό", "Ημέρα", "Εβδομάδα", "Μήνας", "Έτος" };
            ImGui::TextDisabled("Ανάλυση ανά");
            ImGui::SameLine();
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
            for (int i = 0; i < 5; i++) {
                if (i) ImGui::SameLine();
                ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
                ImGui::PushStyleColor(ImGuiCol_Button, g_aptPeriod == i ? ImVec4(0.24f, 0.55f, 0.75f, 1.0f) : ImVec4(0.16f, 0.18f, 0.24f, 1.0f));
                if (ImGui::Button(pn[i], ImVec2(92, 0))) g_aptPeriod = i;
                ImGui::PopStyleColor(); ImGui::PopStyleVar();
            }
            ImGui::PopStyleVar();
            ImGui::SameLine(0, 18);
            ImGui::Checkbox("Ανά πελάτη", &g_aptGroup);

            char buf[48];
            auto eur = [&](double v) { std::snprintf(buf, sizeof(buf), "%.2f €", v); return std::string(buf); };
            if (ImGui::BeginTable("aptstats", 4, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableNextRow();
                struct Tile { const char* l; std::string v; ImVec4 c; std::string sub; };
                Tile t[4] = { {"Εισπράχθηκαν", eur(in), ImVec4(0.35f,0.80f,0.50f,1), ""}, {"Πληρώθηκαν", eur(out), ImVec4(0.90f,0.40f,0.40f,1), ""},
                              {"Οφείλονται", eur(owed), ImVec4(0.95f,0.65f,0.25f,1), ""},
                              {"Κινήσεις", std::to_string(n), ImVec4(0.85f,0.88f,0.95f,1), std::to_string(who.size()) + " πελάτες συνολικά"} };
                for (int i = 0; i < 4; i++) { ImGui::TableNextColumn(); ImGui::PushID(i);
                    StatTile(t[i].l, t[i].v, t[i].c, t[i].sub.c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 84)); ImGui::PopID(); }
                ImGui::EndTable();
            }
            auto periods = AptBuildPeriods(ps, g_aptPeriod);
            std::vector<RepBucket> chart;                              // oldest left, newest right, last 24
            for (int i = std::min((int)periods.size(), 24) - 1; i >= 0; i--) {
                RepBucket b; b.key = i; b.label = periods[i].label; b.collected = periods[i].in; b.paidOut = periods[i].out; b.n = periods[i].n;
                chart.push_back(b);
            }
            RepDrawProgression(chart, -1, 170.0f);
            ImGui::TextDisabled("Η «ώρα» (λεπτό) είναι η στιγμή που καταχωρήθηκε η κίνηση. Προσμετρώνται μόνο πραγματοποιημένες πληρωμές.");
        }

        // ---- who lived here (paged) ----
        std::map<int, std::vector<const Payment*>> byUser;
        for (auto* p : ps) byUser[p->user_id].push_back(p);
        std::vector<int> userOrder; std::set<int> seen;
        for (auto& r : residencies) if (seen.insert(r.user_id).second) userOrder.push_back(r.user_id);
        for (auto& [uid, v] : byUser) if (seen.insert(uid).second) userOrder.push_back(uid);
        if (ImGui::CollapsingHeader("Ιστορικό κατοίκων", ImGuiTreeNodeFlags_DefaultOpen)) {
            int from, to; int pages = AptPagerRow("res_" + std::to_string(sel->a.id), (int)residencies.size(), 8, from, to);
            if (ImGui::BeginTable("aptres", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Πελάτης", ImGuiTableColumnFlags_WidthStretch, 2.4f);
                ImGui::TableSetupColumn("Από", ImGuiTableColumnFlags_WidthFixed, 96.0f);
                ImGui::TableSetupColumn("Έως", ImGuiTableColumnFlags_WidthFixed, 96.0f);
                ImGui::TableSetupColumn("Κινήσεις", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                ImGui::TableSetupColumn("Εισπράχθηκαν", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Πληρώθηκαν", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Οφείλεται", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableHeadersRow();
                for (int i = from; i < to; i++) {
                    auto& r = residencies[i];
                    double ri = 0, ro = 0, rw = 0; int rn = 0;
                    for (auto* p : byUser[r.user_id]) {
                        char d[16]; std::snprintf(d, sizeof(d), "%04d-%02d-%02d", p->year, p->month, p->day);
                        if (p->status == "future" || std::string(d) < r.from_date || (!r.to_date.empty() && std::string(d) > r.to_date)) continue;
                        rn++; (p->direction == "out" ? ro : ri) += p->amount_paid; if (p->balance > 0.005) rw += p->balance;
                    }
                    ImGui::PushID(r.id);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.78f, 1.0f, 1.0f));
                    if (ImGui::Selectable(g_app.userName(r.user_id).c_str())) OpenUserProfile(r.user_id);
                    ImGui::PopStyleColor();
                    ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(AptDateLabel(r.from_date).c_str());
                    ImGui::TableSetColumnIndex(2); if (r.to_date.empty()) ImGui::TextColored(ImVec4(0.35f,0.80f,0.50f,1), "σήμερα"); else ImGui::TextUnformatted(AptDateLabel(r.to_date).c_str());
                    ImGui::TableSetColumnIndex(3); ImGui::Text("%d", rn);
                    ImGui::TableSetColumnIndex(4); ImGui::Text("%.2f €", ri);
                    ImGui::TableSetColumnIndex(5); ImGui::Text("%.2f €", ro);
                    ImGui::TableSetColumnIndex(6); ImGui::Text("%.2f €", rw);
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            AptPagerDraw("res_" + std::to_string(sel->a.id), (int)residencies.size(), pages);
            if (residencies.empty()) ImGui::TextDisabled("Δεν υπάρχει καταγεγραμμένος κάτοικος.");
        }

        // ---- the movements, paged. Grouped = split per client (no combined table). ----
        static const char* pn2[] = { "λεπτό", "ημέρα", "εβδομάδα", "μήνα", "έτος" };
        std::string title = std::string("Κινήσεις ανά ") + pn2[g_aptPeriod] + (g_aptGroup ? " — ανά πελάτη" : "");
        if (ImGui::CollapsingHeader((title + "###aptmoves").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            if (!g_aptGroup) {
                AptDrawPeriodTable("all_" + pageKey, AptBuildPeriods(ps, g_aptPeriod));
            } else {
                for (int uid : userOrder) {
                    auto& list = byUser[uid];
                    double ui = 0, uo = 0, uw = 0; int un = 0;                         // complete client totals
                    for (auto* p : list) { if (p->status == "future") continue; un++; (p->direction == "out" ? uo : ui) += p->amount_paid; if (p->balance > 0.005) uw += p->balance; }
                    char head[400];
                    std::snprintf(head, sizeof(head), "%s   —   %d κινήσεις · εισπράχθηκαν %.2f € · πληρώθηκαν %.2f € · οφείλονται %.2f €###ap_%d_%d",
                                  g_app.userName(uid).c_str(), un, ui, uo, uw, sel->a.id, uid);
                    if (ImGui::CollapsingHeader(head, ImGuiTreeNodeFlags_DefaultOpen)) {
                        ImGui::PushID(uid);
                        ImGui::TextDisabled("Στην ιστορία αυτού του διαμερίσματος:");
                        for (auto& r : residencies) if (r.user_id == uid) {
                            ImGui::SameLine(); ImGui::TextDisabled("%s – %s", AptDateLabel(r.from_date).c_str(), r.to_date.empty() ? "σήμερα" : AptDateLabel(r.to_date).c_str());
                        }
                        AptDrawPeriodTable("u" + std::to_string(uid) + "_" + pageKey, AptBuildPeriods(list, g_aptPeriod));
                        ImGui::PopID();
                    }
                }
                if (userOrder.empty()) ImGui::TextDisabled("Καμία κίνηση.");
            }
        }
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

static ImVec2 BezierCubicCalc(const ImVec2& p0, const ImVec2& p1, const ImVec2& p2, const ImVec2& p3, float t) {
    float u = 1.0f - t;
    float w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
    return ImVec2(w0 * p0.x + w1 * p1.x + w2 * p2.x + w3 * p3.x,
                  w0 * p0.y + w1 * p1.y + w2 * p2.y + w3 * p3.y);
}

static ImVec2 NodeCenter(const GraphNode& n, ImVec2 canvasOrigin) {
    return ImVec2(canvasOrigin.x + n.pos.x + kNodeSize.x * 0.5f, canvasOrigin.y + n.pos.y + kNodeSize.y * 0.5f);
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

static void DrawSettingsScreen() {
    ImGui::Text("Βάση Δεδομένων: %s", g_app.dbPath.c_str());
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
        else if (k == "services_sort") PrefSortParse(v, g_servicesSort, 4);
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
        colors[ImGuiCol_TabActive]          = ImVec4(0.18f, 0.32f, 0.42f, 1.00f);
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
        colors[ImGuiCol_TabSelectedOverline] = ImVec4(0.30f, 0.72f, 0.85f, 1.00f);
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
    ImFont* greekFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 18.0f, &fontCfg, greekRanges);
    if (!greekFont) {
        greekFont = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\arial.ttf", 18.0f, &fontCfg, greekRanges);
    }
    if (greekFont) io.FontDefault = greekFont;

    ImGui_ImplGlfw_InitForOpenGL(window, true);
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
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

        // Docking wasn't available in this vcpkg imgui build (no docking-experimental
        // feature installed), so the multi-panel layout uses a tab bar instead.
        if (ImGui::BeginTabBar("MainTabs")) {
            if (ImGui::BeginTabItem("Αρχική")) { DrawDashboardScreen(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Πελάτες")) { DrawUsersScreen(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Πολυκατοικίες")) { DrawBuildingsScreen(); ImGui::EndTabItem(); }
            bool aptTab = ImGui::BeginTabItem("Διαμερίσματα", nullptr, g_tabToApartments ? ImGuiTabItemFlags_SetSelected : 0);
            g_tabToApartments = false;
            if (aptTab) { DrawApartmentsScreen(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Υπηρεσίες")) { DrawServicesScreen(); ImGui::EndTabItem(); }
            // Legacy/deprecated tab: text is dimmed to signal it isn't current,
            // and selecting it shows a clear notice at the top before the old
            // screen renders below. Kept around purely as a reference -- the
            // actual joint/shared services still exist in the data model, they
            // just no longer have a live editing surface here.
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.60f, 0.75f));
            bool jointTab = ImGui::BeginTabItem("Κοινόχρηστες Υπηρεσίες");
            ImGui::PopStyleColor();
            if (jointTab) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.75f, 0.35f, 1.0f));
                ImGui::TextWrapped("%s",
                    u8"⚠  ΠΑΛΙΑ ΛΕΙΤΟΥΡΓΙΑ  —  Αυτή η καρτέλα διατηρείται μόνο για αναφορά. "
                    u8"Η διαχείριση κοινόχρηστων υπηρεσιών δεν χρησιμοποιείται πλέον από την "
                    u8"καθημερινή ροή της εφαρμογής. Μπορείτε να την δείτε παρακάτω αν χρειαστεί "
                    u8"να ελέγξετε παλαιότερα δεδομένα, αλλά δεν συνιστάται η επεξεργασία από εδώ.");
                ImGui::PopStyleColor();
                ImGui::Separator();
                DrawJointServicesScreen();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Πακέτα Υπηρεσιών")) { DrawMasterServicesScreen(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Πληρωμές")) { DrawPaymentsScreen(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Αναζήτηση")) { DrawSearchScreen(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Σημειώσεις")) { DrawNotesScreen(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Ρυθμίσεις")) { DrawSettingsScreen(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Γενικές Αναφορές")) { DrawReportsScreen(); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
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
        DrawResidentLedgerWindow();
        DrawErrorBanner();
        PrefsSaveIfChanged();
        DrawPaymentViewWindow();
        DrawServiceDialog();
        DrawPaymentDialog();
        DrawPackageChargeDialog();
        DrawResolveDialog();
        DrawPayOwnerPickPopup();
        DrawNodeGraphOverlay();
        DrawAuditLogWindow();
        ImGui::End();

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
