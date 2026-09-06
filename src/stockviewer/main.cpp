// StockViewer -- quick standalone Greek-stocks ROI viewer.
//
// This is a deliberately small "quick thingy": no persistence, no
// background polling, a single hardcoded starter ticker list plus an
// "add your own ticker" box, and a manual refresh button per row. It is
// built as its own separate exe (own window/process) from the same
// CMakeLists.txt / build_imgui.bat as PropertyManager, sharing the same
// vcpkg-provided ImGui/GLFW/OpenGL3 + nlohmann_json.
//
// Data source: Yahoo Finance's UNOFFICIAL "chart" endpoint --
//   https://query1.finance.yahoo.com/v8/finance/chart/{TICKER}.AT
// This endpoint is undocumented, requires no API key, and works for Athens
// Exchange tickers via the ".AT" suffix. It has NO SLA: Yahoo can change
// its response format, rate-limit, or kill it entirely without notice.
// That is an accepted tradeoff for a free/quick solution here, not a bug
// to "fix" -- if it breaks, swap in a different quote source later.

#include <windows.h>
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <nlohmann/json.hpp>

#include <string>
#include <vector>
#include <cstdio>
#include <algorithm>

#include "HttpFetch.h"

using json = nlohmann::json;

// ---------------------------------------------------------------------
// Data model
// ---------------------------------------------------------------------

struct Stock {
    std::string ticker;      // without ".AT", e.g. "OPAP"
    bool selected = true;    // shown in the list?
    bool fetched = false;    // did we ever get a successful fetch?
    bool fetchFailed = false;
    double currentPrice = 0.0;
    std::string currency = "EUR";
    float entryPrice = 0.0f; // user-settable
    std::vector<double> sparkline; // optional recent closes, may be empty
};

static std::vector<Stock> g_stocks;
static float g_feePercent = 0.25f; // generic round-trip fee, applied on entry AND exit
static char g_newTickerBuf[32] = "";

// Starter set of well-known Greek (Athens Exchange) tickers.
static const char* kStarterTickers[] = {
    "OPAP", "ETE", "ALPHA", "EUROB", "TPEIR", "PPC", "HTO", "MYTIL",
    "TITC", "GEKTERNA", "LAMDA", "SAR", "EEE", "EXAE", "AEGN"
};

static void InitStarterStocks() {
    for (const char* t : kStarterTickers) {
        Stock s;
        s.ticker = t;
        g_stocks.push_back(s);
    }
}

// ---------------------------------------------------------------------
// Yahoo Finance fetch + parse
// ---------------------------------------------------------------------

// Fetches the live quote for one stock via the unofficial Yahoo chart
// endpoint and fills in currentPrice/currency/sparkline. Never throws;
// on any failure it sets fetchFailed and leaves prior data untouched so
// the UI can show "--" or the last-known value.
static void RefreshStock(Stock& s) {
    std::wstring host = L"query1.finance.yahoo.com";
    std::wstring wticker(s.ticker.begin(), s.ticker.end());
    std::wstring path = L"/v8/finance/chart/" + wticker + L".AT";

    std::string body;
    if (!HttpGetJson(host, path, body)) {
        s.fetchFailed = true;
        return;
    }

    try {
        json j = json::parse(body);
        auto result = j.at("chart").at("result");
        if (!result.is_array() || result.empty()) {
            s.fetchFailed = true;
            return;
        }
        auto meta = result[0].at("meta");
        s.currentPrice = meta.at("regularMarketPrice").get<double>();
        if (meta.contains("currency") && meta["currency"].is_string())
            s.currency = meta["currency"].get<std::string>();

        // Optional nice-to-have: a simple sparkline from recent closes.
        s.sparkline.clear();
        if (result[0].contains("indicators") &&
            result[0]["indicators"].contains("quote") &&
            result[0]["indicators"]["quote"].is_array() &&
            !result[0]["indicators"]["quote"].empty()) {
            auto closes = result[0]["indicators"]["quote"][0].value("close", json::array());
            for (auto& c : closes) {
                if (c.is_number()) s.sparkline.push_back(c.get<double>());
            }
        }

        s.fetched = true;
        s.fetchFailed = false;
    } catch (...) {
        // Malformed/unexpected JSON (e.g. Yahoo changed format, or returned
        // an error payload for an unknown ticker) -- surface as a failure,
        // never crash.
        s.fetchFailed = true;
    }
}

static void RefreshAll() {
    for (auto& s : g_stocks) {
        if (s.selected) RefreshStock(s);
    }
}

// ---------------------------------------------------------------------
// ROI / break-even math
// ---------------------------------------------------------------------

// Fee is applied on both legs of the round trip (buy AND sell), per the
// client's "generic fees per transaction" wording -- one fee% number
// covers both.
static double ComputeRoiPercent(double currentPrice, double entryPrice, double feePercent) {
    if (entryPrice <= 0.0) return 0.0;
    double feeFrac = feePercent / 100.0;
    double effectiveExit = currentPrice * (1.0 - feeFrac);
    double effectiveEntry = entryPrice * (1.0 + feeFrac);
    return ((effectiveExit - effectiveEntry) / effectiveEntry) * 100.0;
}

// The break-even price: the price at which selling (after fees) exactly
// recovers what was paid (after fees) to enter. Above this = win zone,
// below = loss zone.
static double ComputeBreakEvenPrice(double entryPrice, double feePercent) {
    double feeFrac = feePercent / 100.0;
    if (feeFrac >= 1.0) return entryPrice; // degenerate guard
    return (entryPrice * (1.0 + feeFrac)) / (1.0 - feeFrac);
}

// ---------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------

// Draws the win(above)/lose(below) band: a horizontal bar split at the
// fee-adjusted break-even price, red below and green above, with a
// marker dot showing where the current price sits. Kept intentionally
// simple (ImDrawList rect + circle), same technique as the stat-tile /
// colored-bar visuals in the main PropertyManager app -- not a chart
// library.
static void DrawWinLossBand(double currentPrice, double entryPrice, double feePercent, bool haveData) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x;
    float height = 22.0f;

    ImVec2 barMin = origin;
    ImVec2 barMax = ImVec2(origin.x + width, origin.y + height);

    if (!haveData || entryPrice <= 0.0) {
        dl->AddRectFilled(barMin, barMax, IM_COL32(60, 62, 70, 255), 4.0f);
        ImGui::Dummy(ImVec2(width, height));
        return;
    }

    double breakEven = ComputeBreakEvenPrice(entryPrice, feePercent);

    // Establish a display range around breakEven/currentPrice so the band
    // is readable regardless of absolute price scale.
    double lo = std::min(breakEven, currentPrice) * 0.85;
    double hi = std::max(breakEven, currentPrice) * 1.15;
    if (hi <= lo) hi = lo + 1.0;

    double beFrac = (breakEven - lo) / (hi - lo);
    beFrac = std::min(std::max(beFrac, 0.0), 1.0);
    float splitX = origin.x + (float)beFrac * width;

    // Red (loss) zone below break-even, green (win) zone above.
    dl->AddRectFilled(barMin, ImVec2(splitX, barMax.y), IM_COL32(150, 45, 45, 255), 4.0f, ImDrawFlags_RoundCornersLeft);
    dl->AddRectFilled(ImVec2(splitX, barMin.y), barMax, IM_COL32(45, 140, 70, 255), 4.0f, ImDrawFlags_RoundCornersRight);
    dl->AddLine(ImVec2(splitX, barMin.y), ImVec2(splitX, barMax.y), IM_COL32(230, 230, 230, 255), 2.0f);

    // Marker for current price position.
    double curFrac = (currentPrice - lo) / (hi - lo);
    curFrac = std::min(std::max(curFrac, 0.0), 1.0);
    float markerX = origin.x + (float)curFrac * width;
    float markerY = origin.y + height * 0.5f;
    dl->AddCircleFilled(ImVec2(markerX, markerY), 6.0f, IM_COL32(255, 220, 60, 255));
    dl->AddCircle(ImVec2(markerX, markerY), 6.0f, IM_COL32(20, 20, 20, 255), 12, 1.5f);

    ImGui::Dummy(ImVec2(width, height));

    ImGui::TextColored(ImVec4(0.75f, 0.78f, 0.85f, 1.0f),
                        "Break-even: %.3f", breakEven);
}

static void DrawStockRow(Stock& s) {
    ImGui::PushID(s.ticker.c_str());
    ImGui::Separator();

    ImGui::Checkbox("##sel", &s.selected);
    ImGui::SameLine();
    ImGui::Text("%s.AT", s.ticker.c_str());

    ImGui::SameLine();
    if (ImGui::Button("Ανανέωση")) {
        RefreshStock(s);
    }

    ImGui::SameLine();
    if (!s.fetched && !s.fetchFailed) {
        ImGui::TextDisabled("—");
    } else if (s.fetchFailed && !s.fetched) {
        ImGui::TextColored(ImVec4(0.85f, 0.35f, 0.35f, 1.0f), "Σφάλμα λήψης");
    } else {
        ImGui::Text("Τιμή: %.3f %s", s.currentPrice, s.currency.c_str());
        if (s.fetchFailed) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.85f, 0.6f, 0.2f, 1.0f), "(παλιά τιμή - αποτυχία ανανέωσης)");
        }
    }

    ImGui::SetNextItemWidth(140);
    ImGui::InputFloat("Τιμή εισόδου", &s.entryPrice, 0.0f, 0.0f, "%.3f");

    if (s.fetched && s.entryPrice > 0.0f) {
        double roi = ComputeRoiPercent(s.currentPrice, s.entryPrice, g_feePercent);
        ImVec4 roiColor = roi >= 0.0 ? ImVec4(0.35f, 0.85f, 0.45f, 1.0f) : ImVec4(0.90f, 0.35f, 0.35f, 1.0f);
        ImGui::TextColored(roiColor, "ROI: %+.2f%%", roi);

        DrawWinLossBand(s.currentPrice, s.entryPrice, g_feePercent, true);
    } else {
        ImGui::TextDisabled("ROI: --  (ορίστε τιμή εισόδου και κάντε ανανέωση)");
        DrawWinLossBand(0.0, s.entryPrice, g_feePercent, false);
    }

    ImGui::PopID();
}

static void DrawMainWindow() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("StockViewerMain", nullptr,
                  ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                  ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    ImGui::Text("StockViewer - Μετοχές Ελληνικού Χρηματιστηρίου (live τιμές από Yahoo Finance)");
    ImGui::TextDisabled("Ανεπίσημο endpoint, χωρίς εγγύηση διαθεσιμότητας - ενδεικτικό εργαλείο.");
    ImGui::Spacing();

    ImGui::SetNextItemWidth(150);
    ImGui::InputFloat("Τέλη ανά συναλλαγή % (round-trip)", &g_feePercent, 0.0f, 0.0f, "%.3f");
    ImGui::SameLine();
    if (ImGui::Button("Ανανέωση όλων")) {
        RefreshAll();
    }

    ImGui::Spacing();
    ImGui::SetNextItemWidth(140);
    ImGui::InputText("##newTicker", g_newTickerBuf, sizeof(g_newTickerBuf));
    ImGui::SameLine();
    if (ImGui::Button("+ Προσθήκη")) {
        std::string t(g_newTickerBuf);
        // Loose validation only: trim, uppercase, don't check it exists --
        // we just try to fetch it and show an error state if that fails.
        while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.pop_back();
        size_t start = t.find_first_not_of(" \t");
        if (start != std::string::npos) t = t.substr(start);
        for (auto& c : t) c = (char)toupper((unsigned char)c);
        if (!t.empty()) {
            bool exists = std::any_of(g_stocks.begin(), g_stocks.end(),
                                       [&](const Stock& s) { return s.ticker == t; });
            if (!exists) {
                Stock s;
                s.ticker = t;
                RefreshStock(s); // try immediately
                g_stocks.push_back(s);
            }
            g_newTickerBuf[0] = '\0';
        }
    }

    ImGui::Spacing();
    ImGui::BeginChild("StockList", ImVec2(0, 0), false);
    for (auto& s : g_stocks) {
        if (!s.selected) {
            // Still show a compact hidden-row toggle so the user can re-show it.
            ImGui::PushID(s.ticker.c_str());
            ImGui::Checkbox(s.ticker.c_str(), &s.selected);
            ImGui::PopID();
            continue;
        }
        DrawStockRow(s);
    }
    ImGui::EndChild();

    ImGui::End();
}

// ---------------------------------------------------------------------
// GLFW + ImGui boilerplate (mirrors PropertyManager's src/ui/App.cpp
// RunApp(), including the Greek-glyph font setup -- this project has hit
// the "labels render as ???" bug before when that range list was skipped).
// ---------------------------------------------------------------------

static void GlfwErrorCallback(int error, const char* description) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

int main(int, char**) {
    glfwSetErrorCallback(GlfwErrorCallback);
    if (!glfwInit()) return 1;

    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    GLFWwindow* window = glfwCreateWindow(1200, 900, "StockViewer", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    (void)io;

    ImGui::StyleColorsDark();

    // Same Greek+Latin+symbols glyph range list as PropertyManager's
    // App.cpp -- covers Latin, Greek, Greek Extended, punctuation, the
    // Euro sign, arrows, and geometric shapes, so no UI text here can
    // regress into rendering as "?????".
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

    InitStarterStocks();
    RefreshAll(); // best-effort initial fetch; failures are shown per-row, not fatal

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        DrawMainWindow();

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
