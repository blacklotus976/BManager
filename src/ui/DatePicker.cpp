#include "DatePicker.h"
#include <cstdio>
#include <ctime>
#include <algorithm>
#include <map>

// -----------------------------------------------------------------------------
// Private calendar helpers — duplicated here rather than shared with App.cpp
// so this component is fully self-contained (no hidden linkage back into the
// UI translation unit). If App.cpp ever needs them, it can keep its own copy;
// three trivial date functions are cheaper than a shared util header.
// -----------------------------------------------------------------------------
static bool IsLeapYearLocal(int y) {
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}
static int DaysInMonthLocal(int y, int m) {
    static const int dim[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (m < 1 || m > 12) return 30;
    if (m == 2 && IsLeapYearLocal(y)) return 29;
    return dim[m - 1];
}
// Sakamoto's algorithm: 0=Sunday..6=Saturday.
static int DayOfWeekLocal(int y, int m, int d) {
    static const int t[] = { 0,3,2,5,0,3,5,1,4,6,2,4 };
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

// Working state per picker id: what month/year the grid is showing, and (once
// the user has clicked a day) the uncommitted selection. Snapshotted from the
// incoming string each time the popup opens, so live edits inside the popup
// don't touch `dateInOut` until OK is pressed.
struct DatePickerState {
    int  navYear      = 0;
    int  navMonth     = 0;
    int  selYear      = 0;
    int  selMonth     = 0;
    int  selDay       = 0;
    bool hasSelection = false;
};
static std::map<std::string, DatePickerState> g_datePickerStates;

bool DatePicker(const char* id, std::string& dateInOut,
                ImVec2 offset, bool futureOnly) {
    ImGui::PushID(id);
    bool accepted = false;

    DatePickerState& st = g_datePickerStates[id];

    std::time_t nowT = std::time(nullptr);
    std::tm nowTm{}; localtime_s(&nowTm, &nowT);
    int todayY = nowTm.tm_year + 1900;
    int todayM = nowTm.tm_mon + 1;
    int todayD = nowTm.tm_mday;

    // --- Trigger button ---------------------------------------------------
    std::string btnLabel = dateInOut.empty() ? "Επιλογή ημερομηνίας..." : dateInOut;
    if (ImGui::Button(btnLabel.c_str(), ImVec2(180, 0))) {
        // Snapshot current value into working state so nothing commits until OK.
        int y = 0, m = 0, d = 0;
        if (dateInOut.size() == 10 && std::sscanf(dateInOut.c_str(), "%d-%d-%d", &y, &m, &d) == 3) {
            st.selYear = y; st.selMonth = m; st.selDay = d; st.hasSelection = true;
            st.navYear = y; st.navMonth = m;
        } else {
            st.selYear = 0; st.selMonth = 0; st.selDay = 0; st.hasSelection = false;
            st.navYear = todayY; st.navMonth = todayM;
        }
        ImGui::OpenPopup("##datepickerpopup");
    }
    ImVec2 btnMin = ImGui::GetItemRectMin();
    ImVec2 btnMax = ImGui::GetItemRectMax();

    // --- Popup position (see header comment for the rules) ----------------
    const float gap = 4.0f;
    ImVec2 popupPos;
    ImVec2 popupPivot;
    if (offset.y > 0.0f) {
        // Below the button, pivot from its top-left so it grows downward.
        popupPos   = ImVec2(btnMin.x + offset.x, btnMax.y + gap + offset.y);
        popupPivot = ImVec2(0.0f, 0.0f);
    } else {
        // Above (default when offset.y == 0): pivot (0,1) anchors the popup's
        // bottom-left at the given position, so it grows upward off the button.
        popupPos   = ImVec2(btnMin.x + offset.x, btnMin.y - gap + offset.y);
        popupPivot = ImVec2(0.0f, 1.0f);
    }

    // --- Popup ------------------------------------------------------------
    const float kPopupW = 300.0f;
    ImGui::SetNextWindowSize(ImVec2(kPopupW, 0), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(popupPos, ImGuiCond_Appearing, popupPivot);

    if (ImGui::BeginPopup("##datepickerpopup",
                          ImGuiWindowFlags_NoMove |
                          ImGuiWindowFlags_NoResize |
                          ImGuiWindowFlags_NoSavedSettings)) {
        static const char* kMonthNames[] = {
            "Ιανουάριος","Φεβρουάριος","Μάρτιος","Απρίλιος","Μάιος","Ιούνιος",
            "Ιούλιος","Αύγουστος","Σεπτέμβριος","Οκτώβριος","Νοέμβριος","Δεκέμβριος"
        };
        int& navY = st.navYear;
        int& navM = st.navMonth;

        // --- Month + Year dropdowns, side by side -------------------------
        float styleW = ImGui::GetStyle().WindowPadding.x;
        float monthW = kPopupW * 0.62f - styleW;
        float yearW  = kPopupW * 0.38f - styleW * 1.4f;

        ImGui::SetNextItemWidth(monthW);
        if (ImGui::BeginCombo("##monthsel", kMonthNames[navM - 1])) {
            for (int m = 0; m < 12; m++)
                if (ImGui::Selectable(kMonthNames[m], navM == m + 1)) navM = m + 1;
            ImGui::EndCombo();
        }
        ImGui::SameLine();

        int loY = std::min(todayY - 20, navY - 5);
        int hiY = std::max(todayY + 20, navY + 5);
        char ybuf[8]; std::snprintf(ybuf, sizeof(ybuf), "%d", navY);
        ImGui::SetNextItemWidth(yearW);
        if (ImGui::BeginCombo("##yearsel", ybuf)) {
            for (int y = loY; y <= hiY; y++) {
                char b[8]; std::snprintf(b, sizeof(b), "%d", y);
                if (ImGui::Selectable(b, navY == y)) navY = y;
            }
            ImGui::EndCombo();
        }

        ImGui::Spacing();

        // --- Day grid, Monday-first --------------------------------------
        static const char* kDayHdr[] = { "Δε","Τρ","Τε","Πε","Πα","Σα","Κυ" };
        int startDowMon = (DayOfWeekLocal(navY, navM, 1) + 6) % 7;  // Sunday=0 -> Monday=0
        int dim         = DaysInMonthLocal(navY, navM);

        float cellW = (kPopupW - styleW * 2.0f) / 7.0f;
        if (ImGui::BeginTable("##daygrid", 7,
                              ImGuiTableFlags_SizingFixedFit |
                              ImGuiTableFlags_NoPadOuterX)) {
            for (int c = 0; c < 7; c++)
                ImGui::TableSetupColumn(kDayHdr[c], ImGuiTableColumnFlags_WidthFixed, cellW);

            // Header row — dim, centred over each column.
            ImGui::TableNextRow();
            for (int c = 0; c < 7; c++) {
                ImGui::TableSetColumnIndex(c);
                float padX = (cellW - ImGui::CalcTextSize(kDayHdr[c]).x) * 0.5f;
                if (padX > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padX);
                ImGui::TextDisabled("%s", kDayHdr[c]);
            }

            int day = 1;
            for (int row = 0; row < 6 && day <= dim; row++) {
                ImGui::TableNextRow();
                for (int col = 0; col < 7; col++) {
                    ImGui::TableSetColumnIndex(col);
                    if ((row == 0 && col < startDowMon) || day > dim) continue;

                    bool isPastOrToday = (navY < todayY) ||
                                          (navY == todayY && navM < todayM) ||
                                          (navY == todayY && navM == todayM && day <= todayD);
                    bool disabled   = futureOnly && isPastOrToday;
                    bool isSelected = st.hasSelection &&
                                      st.selYear == navY && st.selMonth == navM && st.selDay == day;
                    bool isToday    = (navY == todayY && navM == todayM && day == todayD);

                    char lbl[8]; std::snprintf(lbl, sizeof(lbl), "%d", day);
                    ImGui::PushID(day);
                    if (disabled) {
                        ImGui::BeginDisabled();
                        ImGui::Selectable(lbl, false, 0, ImVec2(cellW - 4.0f, 0));
                        ImGui::EndDisabled();
                    } else {
                        if (isSelected)
                            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.24f, 0.55f, 0.75f, 1.00f));
                        else if (isToday)
                            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.24f, 0.45f, 0.60f, 0.55f));

                        if (ImGui::Selectable(lbl, isSelected, 0, ImVec2(cellW - 4.0f, 0))) {
                            st.selYear = navY; st.selMonth = navM; st.selDay = day;
                            st.hasSelection = true;
                        }
                        if (isSelected || isToday) ImGui::PopStyleColor();
                    }
                    ImGui::PopID();
                    day++;
                }
            }
            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // --- OK / Cancel footer ------------------------------------------
        const float footerBtnW = 110.0f;
        float avail = ImGui::GetContentRegionAvail().x;
        float rightStart = ImGui::GetCursorPosX() + avail
                           - footerBtnW * 2.0f - ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPosX(rightStart);

        ImGui::BeginDisabled(!st.hasSelection);
        if (ImGui::Button("OK", ImVec2(footerBtnW, 0))) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                          st.selYear, st.selMonth, st.selDay);
            dateInOut = buf;
            accepted = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Ακύρωση", ImVec2(footerBtnW, 0))) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    ImGui::PopID();
    return accepted;
}