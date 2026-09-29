// ImGui/GLFW/OpenGL3 UI shell for PropertyManager, replacing the old FLTK UI.
// Single translation unit by design (small app, keeps the FLTK->ImGui swap easy to review).
#define NOMINMAX
#include "App.h"
#include "DataConn.h"
#include "models/User.h"
#include "models/Service.h"
#include "models/MasterService.h"
#include "models/Payment.h"
#include "models/Note.h"
#include "Config.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#ifdef _WIN32
#include <windows.h>
#include <GL/gl.h>
#include <shobjidl.h>
#endif
#include <GLFW/glfw3.h>

#include <vector>
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
    if (ImGui::BeginPopupModal("Επιβεβαίωση", &g_confirm.open, ImGuiWindowFlags_AlwaysAutoResize)) {
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
// USERS SCREEN
// ---------------------------------------------------------------------------
static bool g_showUserDialog = false;
static bool g_userDialogWasOpen = false;   // ← new
static bool g_userDialogIsEdit = false;
static int g_userDialogId = -1;
static TextBuf ub_name, ub_phone, ub_addr, ub_area, ub_postal, ub_contract, ub_special;

static void OpenAddUser() {
    g_showUserDialog = true; g_userDialogIsEdit = false; g_userDialogId = -1;
    ub_name.set(""); ub_phone.set(""); ub_addr.set(""); ub_area.set(""); ub_postal.set(""); ub_contract.set(""); ub_special.set("");
}
static void OpenEditUser(const User& u) {
    g_showUserDialog = true; g_userDialogIsEdit = true; g_userDialogId = u.id;
    ub_name.set(u.full_name); ub_phone.set(u.phone); ub_addr.set(u.address);
    ub_area.set(u.area); ub_postal.set(u.postal_code); ub_contract.set(u.contract_code); ub_special.set(u.special_code);
}

static void DrawUserDialog() {
    if (g_showUserDialog && !g_userDialogWasOpen) {
        ImGui::OpenPopup("Στοιχεία Ιδιοκτήτη");
    }
    g_userDialogWasOpen = g_showUserDialog;
    if (!g_showUserDialog) return;
    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Στοιχεία Ιδιοκτήτη", &g_showUserDialog, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Ονοματεπώνυμο", ub_name.data, sizeof(ub_name.data));
        ImGui::InputText("Τηλέφωνο", ub_phone.data, sizeof(ub_phone.data));
        ImGui::InputText("Διεύθυνση", ub_addr.data, sizeof(ub_addr.data));
        ImGui::InputText("Περιοχή", ub_area.data, sizeof(ub_area.data));
        ImGui::InputText("Τ.Κ.", ub_postal.data, sizeof(ub_postal.data));
        ImGui::InputText("Κωδ. Συμβολαίου", ub_contract.data, sizeof(ub_contract.data));
        ImGui::InputText("Ειδικός Κωδικός", ub_special.data, sizeof(ub_special.data));
        ImGui::Spacing();
        if (ImGui::Button("Αποθήκευση", ImVec2(150, 0))) {
            User nu;
            nu.full_name = ub_name.str(); nu.phone = ub_phone.str(); nu.address = ub_addr.str();
            nu.area = ub_area.str(); nu.postal_code = ub_postal.str(); nu.contract_code = ub_contract.str();
            nu.special_code = ub_special.str();
            if (g_userDialogIsEdit) {
                User old = g_app.conn->getUserById(g_userDialogId);
                std::vector<FieldDiff> diffs = {
                    {"Όνομα", old.full_name, nu.full_name}, {"Τηλέφωνο", old.phone, nu.phone},
                    {"Διεύθυνση", old.address, nu.address}, {"Περιοχή", old.area, nu.area},
                };
                int id = g_userDialogId;
                RequestConfirm("Επεξεργασία ιδιοκτήτη", diffs, [id, nu]() {
                    g_app.conn->editUser(id, nu);
                    g_app.reloadUsers();
                });
            } else {
                std::vector<FieldDiff> diffs = { {"Όνομα", "-", nu.full_name}, {"Τηλέφωνο", "-", nu.phone} };
                RequestConfirm("Νέος ιδιοκτήτης", diffs, [nu]() {
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
static void DrawUsersScreen() {
    ImGui::InputTextWithHint("##usersearch", "Αναζήτηση ιδιοκτήτη...", g_userSearch, sizeof(g_userSearch));
    ImGui::SameLine();
    if (ImGui::Button("+ Νέος Ιδιοκτήτης")) OpenAddUser();
    ImGui::SameLine();
    if (ImGui::Button("Ανανέωση")) g_app.reloadUsers();
    ImGui::Separator();

    static std::vector<User> filtered;
    filtered = g_app.conn->searchUsers(g_userSearch, 0, 5000);

    ImGui::BeginChild("usercards", ImVec2(0, 0), false);
    const float cardW = 240;
    // Dynamic height, not a hardcoded magic number: name/phone/area/address are
    // always shown (4 lines), special_code only when non-empty (0-1 more), plus
    // the button row. A fixed height here previously clipped the button row the
    // moment special_code was added -- same class of bug fixed earlier for
    // payment cards and joint-service cards; fixed the same way, for good.
    float lineH = ImGui::GetTextLineHeightWithSpacing();
    for (size_t i = 0; i < filtered.size(); i++) {
        auto& u = filtered[i];
        int lines = 4 + (u.special_code.empty() ? 0 : 1);
        float cardH = lines * lineH + 8.0f /*Spacing()*/ + 30.0f /*button row*/ + 16.0f /*padding*/;
        ImGui::PushID(u.id);
        BeginCard("card", ImVec2(cardW, cardH));
        ImGui::TextColored(ImVec4(0.9f,0.9f,0.3f,1), "%s", u.full_name.c_str());
        ImGui::Text("Τηλ: %s", u.phone.c_str());
        ImGui::Text("Περιοχή: %s", u.area.c_str());
        ImGui::TextWrapped("%s", u.address.c_str());
        if (!u.special_code.empty()) ImGui::TextDisabled("Κωδ.: %s", u.special_code.c_str());
        ImGui::Spacing();
        if (ImGui::SmallButton("Προφίλ")) OpenUserProfile(u.id);
        ImGui::SameLine();
        if (ImGui::SmallButton("Επεξ.")) OpenEditUser(u);
        ImGui::SameLine();
        if (ImGui::SmallButton("+Υπηρεσία")) {
            // quick-add: jump the shared "add service" flow to this user (handled in Services screen state)
            extern int g_prefillOwnerUserId; extern bool g_showServiceDialog; extern bool g_serviceDialogIsEdit;
            extern void ResetServiceDialogFields();
            g_prefillOwnerUserId = u.id;
            ResetServiceDialogFields();
            g_serviceDialogIsEdit = false;
            g_showServiceDialog = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Διαγρ.")) {
            int id = u.id; std::string name = u.full_name;
            RequestConfirm("Διαγραφή ιδιοκτήτη", { {"Όνομα", name, "(διαγραφή)"} }, [id]() {
                g_app.conn->removeUser(id);
                g_app.reloadUsers();
            });
        }
        EndCard();
        ImGui::PopID();
        CardWrapNext(cardW);
    }
    ImGui::EndChild();
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
// "Χρήση υπάρχουσας υπηρεσίας" -- per the client ("we won't have a million oil
// services for a million users, but one oil, and opening it will say to which
// users is applied distinctly"), creating a brand-new services row should not
// be the only/default path: joining an existing one (just adding a
// service_users row for this owner) is offered at least as prominently.
static bool sb_joinExisting = false;
static int sb_existingServiceId = -1;
// Package (master-service) creation mode -- see sb_pkgSelected below.
static bool sb_isPackage = false;
static TextBuf sb_pkgLabel;
static std::vector<char> sb_pkgSelected; // parallel to g_app.services
static TextBuf sb_pkgNewServiceLabel;

void ResetServiceDialogFields() {
    sb_label.set(""); sb_notes.set("");
    sb_joinExisting = false; sb_existingServiceId = g_app.services.empty() ? -1 : g_app.services[0].id;
    sb_isPackage = false; sb_pkgLabel.set(""); sb_pkgNewServiceLabel.set("");
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
    if (ImGui::Button("+ Νέα Υπηρεσία")) {
        std::string lbl = sb_pkgNewServiceLabel.str();
        if (!lbl.empty() && !g_app.users.empty()) {
            Service ns; ns.label = lbl;
            // A brand-new catalog service still needs at least one owner to exist
            // (service_users has no "ownerless" concept) -- default to the first
            // user; owners can be adjusted afterward like any other service.
            int newId = g_app.conn->addServiceGetId(g_app.users[0].id, ns);
            g_app.reloadServices();
            sb_pkgSelected.assign(g_app.services.size(), 0);
            if (newId >= 0) {
                for (size_t i = 0; i < g_app.services.size(); i++)
                    if (g_app.services[i].id == newId) sb_pkgSelected[i] = 1;
            }
            sb_pkgNewServiceLabel.set("");
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
    if (ImGui::BeginPopupModal("Στοιχεία Υπηρεσίας", &g_showServiceDialog, ImGuiWindowFlags_AlwaysAutoResize)) {
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

        if (!g_serviceDialogIsEdit) {
            // "Δημιουργία νέας" vs "Χρήση υπάρχουσας" are equally prominent radio
            // options -- per the client, a new services row per user is the
            // mistake to avoid; joining an existing catalog entry is the norm.
            if (ImGui::RadioButton("Νέα υπηρεσία", !sb_joinExisting)) sb_joinExisting = false;
            ImGui::SameLine();
            if (ImGui::RadioButton("Χρήση υπάρχουσας υπηρεσίας", sb_joinExisting)) sb_joinExisting = true;
            ImGui::Separator();
        }

        if (sb_joinExisting && !g_serviceDialogIsEdit) {
            if (ImGui::BeginCombo("Υπάρχουσα υπηρεσία", sb_existingServiceId < 0 ? "-" : g_app.serviceLabel(sb_existingServiceId).c_str())) {
                for (auto& s : g_app.services) if (ImGui::Selectable(s.label.c_str(), s.id == sb_existingServiceId)) sb_existingServiceId = s.id;
                ImGui::EndCombo();
            }
        } else {
            ImGui::InputText("Ετικέτα", sb_label.data, sizeof(sb_label.data));
            ImGui::InputTextMultiline("Σημειώσεις", sb_notes.data, sizeof(sb_notes.data), ImVec2(0, 60));
        }

        ImGui::Separator();
        ImGui::Text("Ιδιοκτήτης:");
        for (auto& u : g_app.users) {
            bool sel = (g_prefillOwnerUserId == u.id);
            if (ImGui::RadioButton(u.full_name.c_str(), sel)) g_prefillOwnerUserId = u.id;
        }
        ImGui::Spacing();
        if (ImGui::Button("Αποθήκευση", ImVec2(150, 0))) {
            if (g_serviceDialogIsEdit) {
                Service ns; ns.label = sb_label.str(); ns.extra_notes = sb_notes.str();
                Service old = g_app.conn->getServiceById(g_serviceDialogId);
                std::vector<FieldDiff> diffs = { {"Ετικέτα", old.label, ns.label} };
                int id = g_serviceDialogId;
                RequestConfirm("Επεξεργασία υπηρεσίας", diffs, [id, ns]() {
                    g_app.conn->editService(id, ns);
                    g_app.reloadServices();
                });
            } else if (sb_joinExisting) {
                int ownerId = g_prefillOwnerUserId;
                int serviceId = sb_existingServiceId;
                std::vector<FieldDiff> diffs = { {"Υπηρεσία", "-", g_app.serviceLabel(serviceId)}, {"Ιδιοκτήτης", "-", g_app.userName(ownerId)} };
                RequestConfirm("Σύνδεση σε υπάρχουσα υπηρεσία", diffs, [serviceId, ownerId]() {
                    g_app.conn->addServiceUser(serviceId, ownerId);
                    g_app.reloadServices();
                    g_app.reloadJoint();
                });
            } else {
                Service ns; ns.label = sb_label.str(); ns.extra_notes = sb_notes.str();
                int ownerId = g_prefillOwnerUserId;
                std::vector<FieldDiff> diffs = { {"Ετικέτα", "-", ns.label}, {"Ιδιοκτήτης", "-", g_app.userName(ownerId)} };
                RequestConfirm("Νέα υπηρεσία", diffs, [ns, ownerId]() {
                    g_app.conn->addService(ownerId, ns);
                    g_app.reloadServices();
                    g_app.reloadJoint();
                });
            }
            g_showServiceDialog = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Ακύρωση", ImVec2(150, 0))) { g_showServiceDialog = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

static void DrawServicesScreen() {
    if (ImGui::Button("+ Νέα Υπηρεσία")) {
        g_serviceDialogIsEdit = false; g_prefillOwnerUserId = g_app.users.empty() ? -1 : g_app.users[0].id;
        ResetServiceDialogFields();
        g_showServiceDialog = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Ανανέωση")) { g_app.reloadServices(); g_app.reloadJoint(); }
    ImGui::TextDisabled("(Οι κοινόχρηστες υπηρεσίες εμφανίζονται στην ξεχωριστή καρτέλα)");
    ImGui::Separator();

    std::set<int> jointIds;
    for (auto& j : g_app.jointServices) jointIds.insert(j.service.id);

    ImGui::BeginChild("svccards", ImVec2(0, 0), false);
    const float cardW = 260, cardH = 150;
    for (auto& s : g_app.services) {
        if (jointIds.count(s.id)) continue; // shown on the joint-services tab instead
        ImGui::PushID(s.id);
        BeginCard("card", ImVec2(cardW, cardH));
        ImGui::TextColored(ImVec4(0.5f,0.8f,1.0f,1), "%s", s.label.c_str());
        if (!s.extra_notes.empty()) ImGui::TextWrapped("%s", s.extra_notes.c_str());
        ImGui::Spacing();
        if (ImGui::SmallButton("Άνοιγμα")) OpenServiceDetail(s.id);
        ImGui::SameLine();
        if (ImGui::SmallButton("Επεξ.")) {
            g_serviceDialogIsEdit = true; g_serviceDialogId = s.id;
            sb_label.set(s.label); sb_notes.set(s.extra_notes);
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
    jb_label.set(""); jb_notes.set("");
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
    if (ImGui::BeginPopupModal("Νέα Κοινόχρηστη Υπηρεσία", &g_showJointDialog, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Ετικέτα", jb_label.data, sizeof(jb_label.data));
        ImGui::InputTextMultiline("Σημειώσεις", jb_notes.data, sizeof(jb_notes.data), ImVec2(0, 50));
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
        ImGui::BeginDisabled(selCount < 2 || jb_label.str().empty());
        if (ImGui::Button("Δημιουργία", ImVec2(150, 0))) {
            Service ns; ns.label = jb_label.str(); ns.extra_notes = jb_notes.str();
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
        ImGui::TextColored(ImVec4(1.0f,0.8f,0.3f,1), "%s  (%zu συνιδιοκτήτες)", j.service.label.c_str(), j.users.size());
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
static void DrawMasterServicesScreen() {
    if (ImGui::Button("+ Νέο Πακέτο")) {
        g_serviceDialogIsEdit = false; g_prefillOwnerUserId = g_app.users.empty() ? -1 : g_app.users[0].id;
        ResetServiceDialogFields();
        sb_isPackage = true;
        g_showServiceDialog = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Ανανέωση")) g_app.reloadServices();
    ImGui::Separator();

    auto summaries = g_app.conn->getMasterServiceSummaries();
    auto allPayments = g_app.conn->getAllPayments(); // fetched once, filtered per component below

    ImGui::BeginChild("mastercards", ImVec2(0, 0), false);
    for (auto& ms : summaries) {
        ImGui::PushID(ms.masterService.id);
        float lineH = ImGui::GetTextLineHeightWithSpacing();
        // Narrative layout per client request: package header sentence, then per
        // component service a sub-header naming it, an "Εφαρμόζεται σε:" line, and
        // one line per user ("-> Name -- Οφειλή: X€, Πληρωμένο: Y€") instead of a
        // raw grid/table. Height must track this line count (same lesson as every
        // other card-height bug in this file) -- not pixel-perfect since BeginCard
        // now always has a scrollbar safety net, but close enough to avoid dead space.
        float innerH = 0.0f;
        for (size_t ci = 0; ci < ms.components.size(); ci++)
            innerH += lineH /*sub-header + Άνοιγμα*/ + lineH /*"Εφαρμόζεται σε:"*/
                      + lineH * (float)ms.componentRosters[ci].size() /*per-user lines*/
                      + (ms.componentRosters[ci].empty() ? lineH : 0.0f) /*"κανένας χρήστης" fallback*/
                      + 10.0f /*spacing*/;
        float cardH = lineH /*package header sentence*/ + innerH + 24.0f /*padding*/;
        BeginCard("card", ImVec2(ImGui::GetContentRegionAvail().x, cardH));
        ImGui::TextColored(ImVec4(0.6f,0.9f,1.0f,1), "Το πακέτο \"%s\" προσφέρει τις εξής υπηρεσίες:", ms.masterService.label.c_str());
        for (size_t ci = 0; ci < ms.components.size(); ci++) {
            Service& comp = ms.components[ci];
            auto& roster = ms.componentRosters[ci];
            ImGui::PushID(comp.id);
            ImGui::Spacing();
            ImGui::Bullet(); ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.9f,0.9f,0.3f,1), "%s", comp.label.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Άνοιγμα")) OpenServiceDetail(comp.id);
            ImGui::Indent();
            if (roster.empty()) {
                ImGui::TextDisabled("Δεν εφαρμόζεται σε κανέναν χρήστη ακόμα.");
            } else {
                ImGui::TextDisabled("Εφαρμόζεται σε:");
                for (auto& u : roster) {
                    double due = 0, paid = 0;
                    for (auto& p : allPayments) {
                        if (p.service_id != comp.id || p.user_id != u.id || p.status == "future") continue;
                        due += p.amount_due; paid += p.amount_paid;
                    }
                    ImGui::Text("\xE2\x86\x92 %s \xE2\x80\x94 Οφειλή: %.2f\xE2\x82\xAC, Πληρωμένο: %.2f\xE2\x82\xAC", u.full_name.c_str(), due, paid);
                }
            }
            ImGui::Unindent();
            ImGui::PopID();
        }
        EndCard();
        ImGui::PopID();
    }
    if (summaries.empty()) ImGui::TextDisabled("Κανένα πακέτο ακόμα.");
    ImGui::EndChild();
    // g_showServiceDialog is drawn once centrally per frame (see RunApp) to avoid
    // the double-OpenPopup ImGui footgun -- this screen only sets the flag above.
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
    pb_direction = 0; pb_isFuture = false;
}

static void DrawPaymentDialog() {
    if (g_showPaymentDialog && !g_paymentDialogWasOpen) {
        ImGui::OpenPopup("Στοιχεία Πληρωμής");
    }
    g_paymentDialogWasOpen = g_showPaymentDialog;
    if (!g_showPaymentDialog) return;
    ImGui::SetNextWindowSize(ImVec2(440, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Στοιχεία Πληρωμής", &g_showPaymentDialog, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::BeginCombo("Χρήστης", g_app.userName(pb_userId).c_str())) {
            for (auto& u : g_app.users) if (ImGui::Selectable(u.full_name.c_str(), u.id == pb_userId)) pb_userId = u.id;
            ImGui::EndCombo();
        }
        if (ImGui::BeginCombo("Υπηρεσία", g_app.serviceLabel(pb_serviceId).c_str())) {
            for (auto& s : g_app.services) if (ImGui::Selectable(s.label.c_str(), s.id == pb_serviceId)) pb_serviceId = s.id;
            ImGui::EndCombo();
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
    if (ImGui::BeginPopupModal("Επίλυση Μελλοντικής Πληρωμής", &g_showResolveDialog, ImGuiWindowFlags_AlwaysAutoResize)) {
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

static void DrawPaymentsScreen() {
    if (ImGui::Button("+ Νέα Πληρωμή")) OpenAddPayment();
    ImGui::SameLine();
    if (ImGui::Button("Ανανέωση")) g_app.reloadPayments();
    ImGui::Separator();
    ImGui::TextDisabled("Φίλτρα:");
    ImGui::TextDisabled("Από:"); ImGui::SameLine();
    DatePicker("payfilt_start", g_payStartDate, ImVec2(0, 1));
    ImGui::SameLine();
    ImGui::TextDisabled("Έως:"); ImGui::SameLine();
    DatePicker("payfilt_end", g_payEndDate, ImVec2(0, 1));
    ImGui::SameLine();

    ImGui::SetNextItemWidth(180);
    if (ImGui::BeginCombo("Χρήστης##filt", g_payUserFilter < 0 ? "Όλοι" : g_app.userName(g_payUserFilter).c_str())) {
        if (ImGui::Selectable("Όλοι", g_payUserFilter < 0)) g_payUserFilter = -1;
        for (auto& u : g_app.users) if (ImGui::Selectable(u.full_name.c_str(), u.id == g_payUserFilter)) g_payUserFilter = u.id;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180);
    if (ImGui::BeginCombo("Υπηρεσία##filt", g_payServiceFilter < 0 ? "Όλες" : g_app.serviceLabel(g_payServiceFilter).c_str())) {
        if (ImGui::Selectable("Όλες", g_payServiceFilter < 0)) g_payServiceFilter = -1;
        for (auto& s : g_app.services) if (ImGui::Selectable(s.label.c_str(), s.id == g_payServiceFilter)) g_payServiceFilter = s.id;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::Combo("Κατάσταση##filt", &g_payStatusFilter, kStatusNames, 3);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    ImGui::Combo("Τύπος##filt", &g_payDirectionFilter, kDirectionFilterNames, 3);
    ImGui::Separator();

    ImGui::BeginChild("paycards", ImVec2(0, 0), false);
    const float cardW = 260;
    for (auto& p : g_app.payments) {
        char payDate[16];
        std::snprintf(payDate, sizeof(payDate), "%04d-%02d-%02d", p.year, p.month, p.day);
        if (!g_payStartDate.empty() && std::string(payDate) < g_payStartDate) continue;
        if (!g_payEndDate.empty()   && std::string(payDate) > g_payEndDate)   continue;
        if (g_payUserFilter >= 0 && p.user_id != g_payUserFilter) continue;
        if (g_payServiceFilter >= 0 && p.service_id != g_payServiceFilter) continue;
        // Simplified status filter: 1=future, 2=resolved (anything that isn't "future" --
        // covers "paid" plus any leftover pending/overdue rows from before this change).
        if (g_payStatusFilter == 1 && p.status != "future") continue;
        if (g_payStatusFilter == 2 && p.status == "future") continue;
        if (g_payDirectionFilter == 1 && p.direction != "in") continue;
        if (g_payDirectionFilter == 2 && p.direction != "out") continue;

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
    if (ImGui::BeginPopupModal("Για ποιον ιδιοκτήτη είναι η πληρωμή;", &g_showPayOwnerPick, ImGuiWindowFlags_AlwaysAutoResize)) {
        auto roster = g_app.conn->getUsersForService(g_payPickServiceId);
        for (auto& u : roster) {
            if (ImGui::Selectable(u.full_name.c_str())) {
                OpenAddPayment();
                pb_serviceId = g_payPickServiceId;
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

// User Profile: the user's own info + every service tied to them (via the
// service_users roster), each clickable through to its Service Detail window.
static void DrawUserProfileWindow() {
    // Snapshot the ID list: drawing a profile can push a new profile onto the
    // vector via its own clickable children (unlikely but possible), so we
    // iterate over a copy to avoid iterator invalidation.
    std::vector<int> ids = g_profileUserIds;
    for (size_t i = 0; i < ids.size(); ++i) {
        int uid = ids[i];
        User u = g_app.conn->getUserById(uid);
        bool open = true;

        // Cascade position: each open profile offset a bit from the previous,
        // so multiple profiles don't spawn stacked on top of each other.
        ImGuiIO& io = ImGui::GetIO();
        float cascade = (float)i * 26.0f;
        std::string title = "Προφίλ: " + u.full_name + "###profile_" + std::to_string(uid);
        ImGui::SetNextWindowSize(ImVec2(560, 520), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 620.0f - cascade, 60.0f + cascade),
                                ImGuiCond_FirstUseEver);
        ImGui::Begin(title.c_str(), &open);

        ImGui::Text("Τηλέφωνο: %s", u.phone.c_str());
        ImGui::Text("Περιοχή: %s", u.area.c_str());
        ImGui::TextWrapped("Διεύθυνση: %s", u.address.c_str());
        if (!u.postal_code.empty()) ImGui::Text("Τ.Κ.: %s", u.postal_code.c_str());
        if (!u.contract_code.empty()) ImGui::Text("Κωδικός Συμβολαίου: %s", u.contract_code.c_str());
        if (!u.special_code.empty()) ImGui::Text("Ειδικός Κωδικός: %s", u.special_code.c_str());
        if (!u.created_at.empty()) ImGui::TextDisabled("Δημιουργήθηκε: %s", u.created_at.c_str());
        if (ImGui::SmallButton("+ Υπηρεσία")) {
            g_serviceDialogIsEdit = false;
            g_prefillOwnerUserId = u.id;
            ResetServiceDialogFields();
            g_showServiceDialog = true;
        }
        ImGui::Separator();

        auto services = g_app.conn->getServicesForUser(u.id);
        auto allPayments = g_app.conn->getPaymentsForUser(u.id);

        double totalDue = 0, totalPaid = 0, totalNet = 0;
        ImGui::BeginChild("profileBottom", ImVec2(0, -1), false);
        float colW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

        ImGui::BeginChild("profileServicesCol", ImVec2(colW, -1), true);
        ImGui::Text("Υπηρεσίες:");
        ImGui::Separator();
        for (auto& s : services) {
            double due = 0, paid = 0, paidOut = 0, collected = 0;
            int paymentCount = 0;
            for (auto& p : allPayments) {
                if (p.service_id != s.id) continue;
                paymentCount++;
                if (p.status == "future") continue;
                due += p.amount_due; paid += p.amount_paid;
                if (p.direction == "out") paidOut += p.amount_paid; else collected += p.amount_paid;
            }
            double net = paidOut - collected;
            totalDue += due; totalPaid += paid; totalNet += net;
            ImGui::PushID(s.id);
            ImGui::PushStyleColor(ImGuiCol_Text, net > 0.005 ? ImVec4(0.95f,0.55f,0.25f,1) : ImVec4(0.35f,0.85f,0.45f,1));
            bool sel = ImGui::Selectable(("Υπηρεσία: " + s.label + "  —  Σύνολο: " + std::to_string(paid) +
                                           "  —  Πληρωμές: " + std::to_string(paymentCount)).c_str());
            ImGui::PopStyleColor();
            if (sel) OpenServiceDetail(s.id);
            ImGui::PopID();
        }
        if (services.empty()) ImGui::TextDisabled("Καμία υπηρεσία ακόμα.");
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("profilePaymentsCol", ImVec2(colW, -1), true);
        ImGui::Text("Πληρωμές (όλες οι υπηρεσίες):");
        ImGui::Separator();
        for (auto& p : allPayments) {
            ImGui::PushID(p.id);
            std::string label = !p.notes.empty() ? p.notes
                : (std::to_string(p.year) + "-" + std::to_string(p.month) + "-" + std::to_string(p.day));
            std::string line = "Πληρωμή: " + label + "  —  " + std::to_string(p.amount_paid) +
                                " €  —  " + g_app.serviceLabel(p.service_id);
            ImGui::PushStyleColor(ImGuiCol_Text, DirectionColor(p.direction));
            bool clicked = ImGui::Selectable(line.c_str());
            ImGui::PopStyleColor();
            if (clicked) OpenPaymentView(p.id);
            ImGui::PopID();
        }
        if (allPayments.empty()) ImGui::TextDisabled("Καμία πληρωμή ακόμα.");
        ImGui::EndChild();

        ImGui::EndChild();
        ImGui::Text("Σύνολο (όλες οι υπηρεσίες) - Οφειλή: %.2f  Πληρωμένο: %.2f", totalDue, totalPaid);
        ImGui::TextColored(totalNet > 0.005 ? ImVec4(0.95f,0.55f,0.25f,1) : ImVec4(0.35f,0.85f,0.45f,1),
                            "Συνολικό υπόλοιπο μέχρι μηδενισμού: %.2f €", totalNet);
        ImGui::End();

        if (!open) {
            g_profileUserIds.erase(std::remove(g_profileUserIds.begin(), g_profileUserIds.end(), uid),
                                   g_profileUserIds.end());
        }
    }
}


// Service Detail: differs by roster size. Single owner -> a flat payment list.
// Joint (2+ owners) -> per-owner totals table (like the Joint screen) plus every
// payment tagged with whose share it is.
static void DrawServiceDetailWindow() {
    std::vector<int> ids = g_detailServiceIds;
    for (size_t i = 0; i < ids.size(); ++i) {
        int sid = ids[i];
        Service s = g_app.conn->getServiceById(sid);
        auto roster = g_app.conn->getUsersForService(s.id);
        bool joint = roster.size() > 1;
        bool open = true;

        ImGuiIO& io = ImGui::GetIO();
        float cascade = (float)i * 26.0f;
        std::string title = std::string(joint ? "Κοινόχρηστη Υπηρεσία: " : "Υπηρεσία: ") + s.label
                          + "###service_" + std::to_string(sid);
        ImGui::SetNextWindowSize(ImVec2(640, 560), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 700.0f - cascade, 100.0f + cascade),
                                ImGuiCond_FirstUseEver);
        ImGui::Begin(title.c_str(), &open);

        if (!s.extra_notes.empty()) ImGui::TextWrapped("%s", s.extra_notes.c_str());
        if (!s.created_at.empty()) ImGui::TextDisabled("Δημιουργήθηκε: %s", s.created_at.c_str());
        if (!joint && !roster.empty()) {
            ImGui::Text("Ιδιοκτήτης: %s", roster[0].full_name.c_str());
            if (ImGui::SmallButton("Προφίλ Ιδιοκτήτη")) OpenUserProfile(roster[0].id);
        } else {
            ImGui::Text("Συνιδιοκτήτες (%zu):", roster.size());
            for (auto& ru : roster) {
                ImGui::SameLine();
                ImGui::PushID(ru.id);
                if (ImGui::SmallButton(ru.full_name.c_str())) OpenUserProfile(ru.id);
                ImGui::PopID();
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("+ Πληρωμή")) OpenAddPaymentForService(s.id);
        ImGui::Separator();

        auto payments = g_app.conn->getPaymentsForService(s.id);
        double paidOut = 0, collected = 0, pendingCollect = 0, pendingPay = 0;
        for (auto& p : payments) {
            if (p.status == "future") continue;
            if (p.direction == "out") {
                if (p.status == "paid") paidOut += p.amount_paid; else pendingPay += p.balance;
            } else {
                if (p.status == "paid") collected += p.amount_paid; else pendingCollect += p.balance;
            }
        }
        double net = paidOut - collected;
        ImGui::Text("Πληρώθηκε (για λογαριασμό πελάτη): %.2f €   Εισπράχθηκε: %.2f €", paidOut, collected);
        ImVec4 netCol = net > 0.005 ? ImVec4(0.95f,0.55f,0.25f,1) : ImVec4(0.35f,0.85f,0.45f,1);
        ImGui::TextColored(netCol, "Υπόλοιπο μέχρι μηδενισμού: %.2f €  (%s)", net,
                            net > 0.005 ? "ο πελάτης χρωστάει ακόμα" : "μηδενισμένο / πιστωτικό");
        if (pendingCollect > 0.005) ImGui::TextDisabled("Εκκρεμεί προς είσπραξη: %.2f €", pendingCollect);
        if (pendingPay > 0.005) ImGui::TextDisabled("Εκκρεμεί προς πληρωμή: %.2f €", pendingPay);
        ImGui::Text("Πληρωμές:");
        ImGui::BeginChild("svcPayments", ImVec2(0, -1), true);
        for (auto& p : payments) {
            ImGui::PushID(p.id);
            std::string line = std::string(DirectionBadge(p.direction)) + "  " +
                (joint ? g_app.userName(p.user_id) + "  |  " : std::string()) +
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
        if (ImGui::Selectable(("Ιδιοκτήτης: " + g_app.userName(p.user_id)).c_str()))
            OpenUserProfile(p.user_id);
        if (ImGui::Selectable(("Υπηρεσία: " + g_app.serviceLabel(p.service_id)).c_str()))
            OpenServiceDetail(p.service_id);
        ImGui::PopStyleColor();

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

// Small stat tile used across the top of the dashboard: big bold value, small
// label under it, optional accent color and subtitle (e.g. period-over-period).
static void StatTile(const char* label, const std::string& value, ImVec4 accent, const char* sub, ImVec2 size) {
    // BUG THAT WAS HERE, root cause of every "only one tile renders" incident this
    // session: BeginCard used the literal fixed id "stat" for every single tile.
    // Sibling child-windows sharing one ID in the same ID-stack scope (e.g. three
    // StatTile calls as columns of one table) collide in ImGui's ID system -- only
    // the first one is a real, distinct window; the rest silently fail to render
    // correctly. `label` is unique among sibling tiles within any one group/table
    // (BeginTable itself pushes a fresh ID scope, so identical labels reused across
    // *different* tables don't collide with each other).
    BeginCard(label, size);
    ImGui::TextDisabled("%s", label);
    ImGui::PushStyleColor(ImGuiCol_Text, accent);
    ImGui::SetWindowFontScale(1.35f);
    ImGui::TextUnformatted(value.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
    if (sub && sub[0]) ImGui::TextDisabled("%s", sub);
    EndCard();
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

    if (g_showNotifPopup) {
        ImGui::OpenPopup("Επερχόμενες Πληρωμές");
        g_showNotifPopup = false;
    }
    if (ImGui::BeginPopupModal("Επερχόμενες Πληρωμές", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
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
            if (ImGui::SmallButton("Επίλυση")) { ImGui::CloseCurrentPopup(); OpenResolvePayment(p); }
            ImGui::PopID();
        }
        if (future.empty()) ImGui::TextDisabled("Καμία επερχόμενη πληρωμή.");
        ImGui::Spacing();
        if (ImGui::Button("Κλείσιμο")) ImGui::CloseCurrentPopup();
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
    if (ImGui::BeginPopupModal("Σημείωση", &g_showNoteDialog, ImGuiWindowFlags_AlwaysAutoResize)) {
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
// ADVANCED SEARCH SCREEN ("Αναζήτηση")
// ---------------------------------------------------------------------------
// UX choice: single-select category (radio buttons), not true multi-select checkboxes.
// Each category has a different field set and a different results-card layout, so
// searching several categories "at once" would just mean stacking three independent
// forms + three independent results lists on one screen -- a single-select with one
// clean form/result area underneath is simpler to use and to implement, and is what
// the client's own example ("payments for Παπαδόπουλος") implies: one focused search
// at a time. Selecting a category clears the previous category's results.
enum class SearchCategory { None = -1, Users = 0, Services = 1, Payments = 2 };
static SearchCategory g_searchCategory = SearchCategory::None;

static TextBuf sq_u_id, sq_u_name, sq_u_phone, sq_u_address, sq_u_area, sq_u_postal, sq_u_contract, sq_u_special;
static TextBuf sq_s_id, sq_s_label, sq_s_notes;
static TextBuf sq_p_id, sq_p_year, sq_p_month, sq_p_day, sq_p_due, sq_p_paid, sq_p_status, sq_p_direction, sq_p_notes, sq_p_resdate, sq_p_userName, sq_p_serviceLabel, sq_p_serviceId, sq_p_userId;

static std::vector<User> g_searchUserResults;
static std::vector<Service> g_searchServiceResults;
static std::vector<Payment> g_searchPaymentResults;
static bool g_searchHasRun = false; // true once the user has clicked Αναζήτηση at least once for the current category

static void ResetSearchForm() {
    sq_u_id.set(""); sq_u_name.set(""); sq_u_phone.set(""); sq_u_address.set(""); sq_u_area.set(""); sq_u_postal.set(""); sq_u_contract.set(""); sq_u_special.set("");
    sq_s_id.set(""); sq_s_label.set(""); sq_s_notes.set("");
    sq_p_id.set(""); sq_p_year.set(""); sq_p_month.set(""); sq_p_day.set(""); sq_p_due.set(""); sq_p_paid.set(""); sq_p_status.set("");
    sq_p_direction.set(""); sq_p_notes.set(""); sq_p_resdate.set(""); sq_p_userName.set(""); sq_p_serviceLabel.set(""); sq_p_serviceId.set(""); sq_p_userId.set("");
    g_searchUserResults.clear(); g_searchServiceResults.clear(); g_searchPaymentResults.clear();
    g_searchHasRun = false;
}

// Runs the query for the active category against whatever is currently typed in the
// form. Called live (every frame any field changed), not just on button click / Enter --
// this is what makes the search feel like a social-media-style live search: type, see
// results narrow immediately, keep typing, narrow further.
static void RunAdvancedSearch() {
    g_searchHasRun = true;
    switch (g_searchCategory) {
        case SearchCategory::Users: {
            User criteria;
            criteria.full_name = sq_u_name.str(); criteria.phone = sq_u_phone.str(); criteria.address = sq_u_address.str();
            criteria.area = sq_u_area.str(); criteria.postal_code = sq_u_postal.str(); criteria.contract_code = sq_u_contract.str();
            criteria.special_code = sq_u_special.str();
            g_searchUserResults = g_app.conn->advancedSearchUsers(criteria, sq_u_id.str());
            break;
        }
        case SearchCategory::Services: {
            g_searchServiceResults = g_app.conn->advancedSearchServices(sq_s_label.str(), sq_s_notes.str(), sq_s_id.str());
            break;
        }
        case SearchCategory::Payments: {
            g_searchPaymentResults = g_app.conn->advancedSearchPayments(sq_p_year.str(), sq_p_month.str(), sq_p_day.str(), sq_p_due.str(), sq_p_paid.str(),
                sq_p_status.str(), sq_p_direction.str(), sq_p_notes.str(), sq_p_resdate.str(), sq_p_userName.str(), sq_p_serviceLabel.str(),
                sq_p_id.str(), sq_p_serviceId.str(), sq_p_userId.str());
            break;
        }
        default: break;
    }
}

static bool AnySearchCriteriaFilled() {
    switch (g_searchCategory) {
        case SearchCategory::Users:
            return !sq_u_id.str().empty() || !sq_u_name.str().empty() || !sq_u_phone.str().empty() || !sq_u_address.str().empty() ||
                   !sq_u_area.str().empty() || !sq_u_postal.str().empty() || !sq_u_contract.str().empty() ||
                   !sq_u_special.str().empty();
        case SearchCategory::Services:
            return !sq_s_id.str().empty() || !sq_s_label.str().empty() || !sq_s_notes.str().empty();
        case SearchCategory::Payments:
            return !sq_p_id.str().empty() || !sq_p_year.str().empty() || !sq_p_month.str().empty() || !sq_p_day.str().empty() ||
                   !sq_p_due.str().empty() || !sq_p_paid.str().empty() || !sq_p_status.str().empty() ||
                   !sq_p_direction.str().empty() || !sq_p_notes.str().empty() || !sq_p_resdate.str().empty() ||
                   !sq_p_userName.str().empty() || !sq_p_serviceLabel.str().empty() || !sq_p_serviceId.str().empty() || !sq_p_userId.str().empty();
        default: return false;
    }
}

static void DrawSearchScreen() {
    ImGui::TextDisabled("Επιλέξτε κατηγορία και συμπληρώστε όσα πεδία θέλετε (κενό = χωρίς φίλτρο):");
    ImGui::Spacing();

    SearchCategory prevCat = g_searchCategory;
    bool isUsers = (g_searchCategory == SearchCategory::Users);
    bool isServices = (g_searchCategory == SearchCategory::Services);
    bool isPayments = (g_searchCategory == SearchCategory::Payments);
    if (ImGui::RadioButton("Πελάτες", isUsers)) g_searchCategory = SearchCategory::Users;
    ImGui::SameLine();
    if (ImGui::RadioButton("Υπηρεσίες", isServices)) g_searchCategory = SearchCategory::Services;
    ImGui::SameLine();
    if (ImGui::RadioButton("Πληρωμές", isPayments)) g_searchCategory = SearchCategory::Payments;
    if (g_searchCategory != prevCat) ResetSearchForm();

    ImGui::Separator();

    if (g_searchCategory == SearchCategory::None) {
        ImGui::TextDisabled("Επιλέξτε μια κατηγορία παραπάνω για να ξεκινήσετε.");
        return;
    }

    BeginCard("searchform", ImVec2(0, isPayments ? 150 : 90));
    // `changed` tracks whether ANY field's InputText edited its buffer this frame (typing,
    // pasting, backspacing, etc -- not just Enter). That is the live-search trigger: as soon
    // as a keystroke lands, re-run the query same frame, so results narrow/widen live as the
    // user types, social-media-search style. Enter still works too (same InputText call
    // returns true on both edit and Enter with EnterReturnsTrue), and the button below stays
    // as a harmless manual "search now" affordance.
    bool changed = false;
    if (g_searchCategory == SearchCategory::Users) {
        ImGui::SetNextItemWidth(70);  changed |= ImGui::InputTextWithHint("##sqid", "ID", sq_u_id.data, sizeof(sq_u_id.data), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsDecimal); ImGui::SameLine();
        ImGui::SetNextItemWidth(180); changed |= ImGui::InputTextWithHint("##sqname", "Ονοματεπώνυμο", sq_u_name.data, sizeof(sq_u_name.data), ImGuiInputTextFlags_EnterReturnsTrue); ImGui::SameLine();
        ImGui::SetNextItemWidth(140); changed |= ImGui::InputTextWithHint("##sqphone", "Τηλέφωνο", sq_u_phone.data, sizeof(sq_u_phone.data), ImGuiInputTextFlags_EnterReturnsTrue); ImGui::SameLine();
        ImGui::SetNextItemWidth(180); changed |= ImGui::InputTextWithHint("##sqaddr", "Διεύθυνση", sq_u_address.data, sizeof(sq_u_address.data), ImGuiInputTextFlags_EnterReturnsTrue); ImGui::SameLine();
        ImGui::SetNextItemWidth(140); changed |= ImGui::InputTextWithHint("##sqarea", "Περιοχή", sq_u_area.data, sizeof(sq_u_area.data), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SetNextItemWidth(120); changed |= ImGui::InputTextWithHint("##sqpostal", "Τ.Κ.", sq_u_postal.data, sizeof(sq_u_postal.data), ImGuiInputTextFlags_EnterReturnsTrue); ImGui::SameLine();
        ImGui::SetNextItemWidth(160); changed |= ImGui::InputTextWithHint("##sqcontract", "Κωδικός συμβολαίου", sq_u_contract.data, sizeof(sq_u_contract.data), ImGuiInputTextFlags_EnterReturnsTrue); ImGui::SameLine();
        ImGui::SetNextItemWidth(160); changed |= ImGui::InputTextWithHint("##sqspecial", "Ειδικός κωδικός", sq_u_special.data, sizeof(sq_u_special.data), ImGuiInputTextFlags_EnterReturnsTrue);
    } else if (g_searchCategory == SearchCategory::Services) {
        ImGui::SetNextItemWidth(70);  changed |= ImGui::InputTextWithHint("##sqsid", "ID", sq_s_id.data, sizeof(sq_s_id.data), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsDecimal); ImGui::SameLine();
        ImGui::SetNextItemWidth(180); changed |= ImGui::InputTextWithHint("##sqlabel", "Ετικέτα", sq_s_label.data, sizeof(sq_s_label.data), ImGuiInputTextFlags_EnterReturnsTrue); ImGui::SameLine();
        ImGui::SetNextItemWidth(300); changed |= ImGui::InputTextWithHint("##sqsnotes", "Σημειώσεις", sq_s_notes.data, sizeof(sq_s_notes.data), ImGuiInputTextFlags_EnterReturnsTrue);
    } else { // Payments
        ImGui::SetNextItemWidth(70);  changed |= ImGui::InputTextWithHint("##sqpid", "ID", sq_p_id.data, sizeof(sq_p_id.data), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsDecimal); ImGui::SameLine();
        ImGui::SetNextItemWidth(70); changed |= ImGui::InputTextWithHint("##sqyear", "Έτος", sq_p_year.data, sizeof(sq_p_year.data), ImGuiInputTextFlags_EnterReturnsTrue); ImGui::SameLine();
        ImGui::SetNextItemWidth(60); changed |= ImGui::InputTextWithHint("##sqmonth", "Μήνας", sq_p_month.data, sizeof(sq_p_month.data), ImGuiInputTextFlags_EnterReturnsTrue); ImGui::SameLine();
        ImGui::SetNextItemWidth(60); changed |= ImGui::InputTextWithHint("##sqday", "Ημέρα", sq_p_day.data, sizeof(sq_p_day.data), ImGuiInputTextFlags_EnterReturnsTrue); ImGui::SameLine();
        ImGui::SetNextItemWidth(110); changed |= ImGui::InputTextWithHint("##sqdue", "Οφειλή", sq_p_due.data, sizeof(sq_p_due.data), ImGuiInputTextFlags_EnterReturnsTrue); ImGui::SameLine();
        ImGui::SetNextItemWidth(110); changed |= ImGui::InputTextWithHint("##sqpaid", "Πληρωμένο", sq_p_paid.data, sizeof(sq_p_paid.data), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SetNextItemWidth(120); changed |= ImGui::InputTextWithHint("##sqstatus", "Κατάσταση", sq_p_status.data, sizeof(sq_p_status.data), ImGuiInputTextFlags_EnterReturnsTrue); ImGui::SameLine();
        ImGui::SetNextItemWidth(100); changed |= ImGui::InputTextWithHint("##sqdir", "Τύπος (in/out)", sq_p_direction.data, sizeof(sq_p_direction.data), ImGuiInputTextFlags_EnterReturnsTrue); ImGui::SameLine();
        ImGui::SetNextItemWidth(150); changed |= ImGui::InputTextWithHint("##sqresdate", "Ημ/νία επίλυσης", sq_p_resdate.data, sizeof(sq_p_resdate.data), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SetNextItemWidth(200); changed |= ImGui::InputTextWithHint("##sqpnotes", "Σημειώσεις", sq_p_notes.data, sizeof(sq_p_notes.data), ImGuiInputTextFlags_EnterReturnsTrue); ImGui::SameLine();
        ImGui::SetNextItemWidth(180); changed |= ImGui::InputTextWithHint("##sqpuname", "Όνομα πελάτη", sq_p_userName.data, sizeof(sq_p_userName.data), ImGuiInputTextFlags_EnterReturnsTrue); ImGui::SameLine();
        ImGui::SetNextItemWidth(180); changed |= ImGui::InputTextWithHint("##sqpslabel", "Ετικέτα υπηρεσίας", sq_p_serviceLabel.data, sizeof(sq_p_serviceLabel.data), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SetNextItemWidth(90);  changed |= ImGui::InputTextWithHint("##sqpsid", "ID υπηρεσίας", sq_p_serviceId.data, sizeof(sq_p_serviceId.data), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsDecimal); ImGui::SameLine();
        ImGui::SetNextItemWidth(90);  changed |= ImGui::InputTextWithHint("##sqpuid", "ID πελάτη", sq_p_userId.data, sizeof(sq_p_userId.data), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsDecimal);
    }
    EndCard();

    // "Αναζήτηση" is kept as a harmless manual affordance -- live search below already
    // re-runs on every keystroke, so clicking it just re-issues the same query.
    bool clicked = ImGui::Button("Αναζήτηση");
    ImGui::SameLine();
    if (ImGui::Button("Καθαρισμός")) ResetSearchForm();
    if ((clicked || changed) && AnySearchCriteriaFilled()) RunAdvancedSearch();
    if (!AnySearchCriteriaFilled() && g_searchHasRun) { g_searchUserResults.clear(); g_searchServiceResults.clear(); g_searchPaymentResults.clear(); g_searchHasRun = false; }
    ImGui::Separator();

    if (!g_searchHasRun) {
        ImGui::TextDisabled("Συμπληρώστε τουλάχιστον ένα πεδίο και πατήστε Αναζήτηση.");
        return;
    }

    ImGui::BeginChild("searchresults", ImVec2(0, 0), false);
    if (g_searchCategory == SearchCategory::Users) {
        ImGui::Text("Αποτελέσματα: %d", (int)g_searchUserResults.size());
        const float cardW = 240, cardH = 150;
        for (auto& u : g_searchUserResults) {
            ImGui::PushID(u.id);
            BeginCard("card", ImVec2(cardW, cardH));
            ImGui::TextColored(ImVec4(0.9f,0.9f,0.3f,1), "%s", u.full_name.c_str());
            ImGui::Text("Τηλ: %s", u.phone.c_str());
            ImGui::Text("Περιοχή: %s", u.area.c_str());
            ImGui::TextWrapped("%s", u.address.c_str());
            if (!u.special_code.empty()) ImGui::TextDisabled("Κωδ.: %s", u.special_code.c_str());
            ImGui::Spacing();
            if (ImGui::SmallButton("Προφίλ")) OpenUserProfile(u.id);
            EndCard();
            ImGui::PopID();
            CardWrapNext(cardW);
        }
    } else if (g_searchCategory == SearchCategory::Services) {
        ImGui::Text("Αποτελέσματα: %d", (int)g_searchServiceResults.size());
        const float cardW = 240, cardH = 150;
        for (auto& s : g_searchServiceResults) {
            ImGui::PushID(s.id);
            BeginCard("card", ImVec2(cardW, cardH));
            ImGui::TextColored(ImVec4(0.9f,0.9f,0.3f,1), "%s", s.label.c_str());
            if (!s.extra_notes.empty()) ImGui::TextWrapped("%s", s.extra_notes.c_str());
            ImGui::Spacing();
            if (ImGui::SmallButton("Άνοιγμα")) OpenServiceDetail(s.id);
            EndCard();
            ImGui::PopID();
            CardWrapNext(cardW);
        }
    } else if (g_searchCategory == SearchCategory::Payments) {
        ImGui::Text("Αποτελέσματα: %d", (int)g_searchPaymentResults.size());
        const float cardW = 260;
        for (auto& p : g_searchPaymentResults) {
            ImVec4 col = StatusColor(p.status);
            ImGui::PushID(p.id);
            float lineH = ImGui::GetTextLineHeightWithSpacing();
            int textLines = 5 + (p.notes.empty() ? 0 : 1);
            float cardH = textLines * lineH + 30.0f + 16.0f;
            BeginCard("card", ImVec2(cardW, cardH));
            ImGui::TextColored(DirectionColor(p.direction), "%s", DirectionBadge(p.direction));
            ImGui::Text("%s / %s", g_app.userName(p.user_id).c_str(), g_app.serviceLabel(p.service_id).c_str());
            ImGui::Text("%04d-%02d-%02d", p.year, p.month, p.day);
            ImGui::TextColored(col, "%s", p.status.c_str());
            ImGui::Text("Οφειλή: %.2f  Πληρωμένο: %.2f", p.amount_due, p.amount_paid);
            if (!p.notes.empty()) ImGui::TextWrapped("%s", p.notes.c_str());
            if (ImGui::SmallButton("Προβολή")) OpenPaymentView(p.id);
            EndCard();
            ImGui::PopID();
            CardWrapNext(cardW);
        }
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
    if (ImGui::Button("Γρήγορη προσθήκη πληρωμής")) OpenAddPayment();
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
int RunApp(DataConn* conn, const std::string& dbPath) {
    g_app.conn = conn;
    g_app.dbPath = dbPath;
    g_app.reloadAll();

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
        style.WindowRounding = 8.0f;
        style.ChildRounding = 6.0f;
        style.FrameRounding = 5.0f;
        style.PopupRounding = 7.0f;
        style.ScrollbarRounding = 8.0f;
        style.GrabRounding = 4.0f;
        style.TabRounding = 5.0f;
        style.WindowBorderSize = 1.0f;
        style.FrameBorderSize = 0.0f;
        style.WindowPadding = ImVec2(10, 10);
        style.FramePadding = ImVec2(8, 5);
        style.ItemSpacing = ImVec2(9, 7);
        style.ItemInnerSpacing = ImVec2(6, 6);
        style.CellPadding = ImVec2(6, 4);
        style.IndentSpacing = 18.0f;

        ImVec4* colors = style.Colors;
        colors[ImGuiCol_WindowBg]           = ImVec4(0.098f, 0.102f, 0.125f, 1.00f);
        colors[ImGuiCol_ChildBg]            = ImVec4(0.114f, 0.118f, 0.145f, 0.55f);
        colors[ImGuiCol_PopupBg]            = ImVec4(0.106f, 0.110f, 0.133f, 0.98f);
        colors[ImGuiCol_Border]             = ImVec4(0.25f, 0.26f, 0.32f, 0.55f);
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
        colors[ImGuiCol_TableHeaderBg]      = ImVec4(0.17f, 0.19f, 0.24f, 1.00f);
        colors[ImGuiCol_TableBorderStrong]  = ImVec4(0.28f, 0.29f, 0.35f, 1.00f);
        colors[ImGuiCol_TableBorderLight]   = ImVec4(0.22f, 0.23f, 0.28f, 1.00f);
        colors[ImGuiCol_TableRowBg]         = ImVec4(1, 1, 1, 0.00f);
        colors[ImGuiCol_TableRowBgAlt]      = ImVec4(1, 1, 1, 0.025f);
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
            if (ImGui::BeginTabItem("Υπηρεσίες")) { DrawServicesScreen(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Κοινόχρηστες Υπηρεσίες")) { DrawJointServicesScreen(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Πακέτα Υπηρεσιών")) { DrawMasterServicesScreen(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Πληρωμές")) { DrawPaymentsScreen(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Αναζήτηση")) { DrawSearchScreen(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Σημειώσεις")) { DrawNotesScreen(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Ρυθμίσεις")) { DrawSettingsScreen(); ImGui::EndTabItem(); }
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
        DrawPaymentViewWindow();
        DrawServiceDialog();
        DrawPaymentDialog();
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
