// StockViewer -- multi-provider multi-asset terminal.
#include <windows.h>
#include <shellapi.h>
#include <GLFW/glfw3.h>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <nlohmann/json.hpp>

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <unordered_set>

#include "IMarketDataProvider.h"
#include "HttpFetch.h"
#include "Providers.h"
#include "PluginLoader.h"
#include "Persistence.h"
#include "RequestStats.h"

#pragma comment(lib, "shell32.lib")

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------
static const int    kMinViewBars     = 20;
static const int    kMaxViewBars     = 300;
static const size_t kBufferMaxBars   = 1500;   // hard cap of cached bars per stock
static const int    kOlderFetchLimit = 500;    // bars requested when paging back
static const int    kScrollEdge      = 20;     // trigger "load older" this close to the edge

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------
struct ProviderRuntime {
    std::unique_ptr<IMarketDataProvider> impl;
    std::string apiKey;
    std::string testResult;
    bool        testPassed = false;
    RequestStats stats;
    bool        isPlugin = false;

    ProviderRuntime() = default;
    ProviderRuntime(const ProviderRuntime&) = delete;
    ProviderRuntime& operator=(const ProviderRuntime&) = delete;
    ProviderRuntime(ProviderRuntime&& o) noexcept
        : impl(std::move(o.impl)), apiKey(std::move(o.apiKey))
        , testResult(std::move(o.testResult)), testPassed(o.testPassed)
        , isPlugin(o.isPlugin) {}
    ProviderRuntime& operator=(ProviderRuntime&& o) noexcept {
        impl = std::move(o.impl); apiKey = std::move(o.apiKey);
        testResult = std::move(o.testResult); testPassed = o.testPassed;
        isPlugin = o.isPlugin; return *this;
    }
};

struct StockRuntime {
    SavedStock cfg;
    Quote latest;
    std::vector<OHLCVBar> bars;        // bounded, sorted oldest-first

    int     viewStartIdx = -1;         // -1 = rightmost (newest)
    int     viewBars     = 60;

    bool    loading        = false;    // any bars fetch in flight
    bool    loadingOlder   = false;    // an older-paging fetch is in flight
    bool    needsOlder     = false;
    bool    canLoadOlder   = true;

    bool    pendingRefresh = false;
    int     refreshPreCount = 0;

    bool    scrollToMe     = false;
    bool    staleProbeTried = false;
    bool    mayBeUnsupported = false;

    std::chrono::steady_clock::time_point lastQuoteFetch;
};

static std::vector<ProviderRuntime> g_providers;
static std::vector<StockRuntime>    g_stocks;
static int                          g_activeTab = 0;
static std::string                  g_statePath;

static bool         g_showAddModal = false;
static int          g_addProviderIdx = 0;
static char         g_addSearch[128] = "";
static std::vector<std::string> g_addResults;
static std::chrono::steady_clock::time_point g_addLastSearch;
static bool         g_addSearchDirty = false;
static std::chrono::steady_clock::time_point g_addModalOpened;

static bool         g_showKeyModal = false;
static int          g_keyProviderIdx = 0;
static char         g_keyBuffer[256] = "";

static bool         g_infoPopupRequested = false;
static std::string  g_infoTitle, g_infoBody, g_infoLink;

static std::string g_toastMsg;
static std::chrono::steady_clock::time_point g_toastTime;
static void ShowToast(const std::string& m) {
    g_toastMsg = m;
    g_toastTime = std::chrono::steady_clock::now();
}

static std::atomic<bool> g_pollerRun{true};
static std::atomic<bool> g_shuttingDown{false};
static std::thread       g_pollerThread;

struct AsyncResult {
    std::string providerId, ticker;
    Quote quote;
    std::vector<OHLCVBar> bars;
    int64_t requestedEndMs = 0;
    int     requestedLimit = 0;
    bool    hasQuote = false, hasBars = false;
    bool    isRefresh = false;
    bool    isStaleProbe = false;
    bool    isOlder = false;
};
static std::mutex               g_asyncMutex;
static std::vector<AsyncResult> g_asyncQueue;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static ProviderRuntime* FindProvider(const std::string& id) {
    for (auto& p : g_providers) if (p.impl->Id() == id) return &p;
    return nullptr;
}
static StockRuntime* FindStock(const std::string& pid, const std::string& tk) {
    for (auto& s : g_stocks) if (s.cfg.providerId == pid && s.cfg.ticker == tk) return &s;
    return nullptr;
}
static std::string ProviderHomepage(const std::string& id) {
    if (id == "binance")      return "https://data-api.binance.vision";
    if (id == "biquote")      return "https://biquote.io";
    if (id == "naftemporiki") return "https://services.naftemporiki.gr";
    return "";
}
static bool IsLastBarStale(const std::vector<OHLCVBar>& bars, int graceDays = 5) {
    if (bars.empty()) return true;
    auto now_t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tmv{}; gmtime_s(&tmv, &now_t);
    tmv.tm_hour = tmv.tm_min = tmv.tm_sec = 0;
    time_t today_t = _mkgmtime(&tmv);
    int64_t today_ms = (int64_t)today_t * 1000;
    int64_t last_ms  = bars.back().timestampMs;
    int64_t age_ms   = today_ms - last_ms;
    int64_t grace_ms = (int64_t)graceDays * 86400LL * 1000;
    return age_ms > grace_ms;
}

// ---------------------------------------------------------------------------
// Math
// ---------------------------------------------------------------------------
static double ComputeRoiPercent(double cur, double entry, double feePct) {
    if (entry <= 0.0) return 0.0;
    double f = feePct / 100.0;
    double eExit  = cur   * (1.0 - f);
    double eEntry = entry * (1.0 + f);
    return ((eExit - eEntry) / eEntry) * 100.0;
}
static double ComputeBreakEvenPrice(double entry, double feePct) {
    double f = feePct / 100.0;
    if (f >= 1.0) return entry;
    return (entry * (1.0 + f)) / (1.0 - f);
}
static double ComputeTargetPrice(double entry, double feePct, double targetRoiPct) {
    double f = feePct / 100.0;
    double t = targetRoiPct / 100.0;
    if (entry <= 0 || f >= 1.0) return 0;
    return entry * (1.0 + f) * (1.0 + t) / (1.0 - f);
}

// ---------------------------------------------------------------------------
// Async fetchers
// ---------------------------------------------------------------------------
static void AsyncFetchQuote(const std::string& pid, const std::string& tk) {
    if (g_shuttingDown.load()) return;
    ProviderRuntime* pr = FindProvider(pid);
    if (!pr) return;
    Quote q;
    auto t0 = std::chrono::steady_clock::now();
    bool ok = false;
    try { ok = pr->impl->FetchQuote(tk, q); } catch (...) {}
    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    if (g_shuttingDown.load()) return;
    pr->stats.Record(ok, dt, 0);
    AsyncResult r;
    r.providerId = pid; r.ticker = tk; r.quote = q; r.hasQuote = ok;
    std::lock_guard<std::mutex> lk(g_asyncMutex);
    g_asyncQueue.push_back(std::move(r));
}

static void AsyncFetchBars(const std::string& pid, const std::string& tk,
                            const std::string& tfId, int64_t endMs, int limit,
                            bool isRefresh = false,
                            bool isStaleProbe = false,
                            bool isOlder = false) {
    if (g_shuttingDown.load()) return;
    ProviderRuntime* pr = FindProvider(pid);
    if (!pr) return;

    std::vector<OHLCVBar> bars;
    auto t0 = std::chrono::steady_clock::now();
    bool ok = false;
    try { ok = pr->impl->FetchOHLCV(tk, tfId, endMs, limit, bars); } catch (...) {}
    auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    if (g_shuttingDown.load()) return;
    pr->stats.Record(ok, dt, (int64_t)bars.size() * sizeof(OHLCVBar));

    AsyncResult r;
    r.providerId = pid; r.ticker = tk; r.bars = std::move(bars);
    r.requestedEndMs = endMs; r.requestedLimit = limit;
    r.hasBars = ok;
    r.isRefresh = isRefresh;
    r.isStaleProbe = isStaleProbe;
    r.isOlder = isOlder;
    std::lock_guard<std::mutex> lk(g_asyncMutex);
    g_asyncQueue.push_back(std::move(r));
}

// ---------------------------------------------------------------------------
// Poller
// ---------------------------------------------------------------------------
static void PollerLoop() {
    while (g_pollerRun.load()) {
        auto now = std::chrono::steady_clock::now();
        for (auto& s : g_stocks) {
            if (!s.cfg.visible) continue;
            ProviderRuntime* pr = FindProvider(s.cfg.providerId);
            if (!pr) continue;
            auto caps = pr->impl->Capabilities();
            int pollSec = caps.suggestedPollSeconds; if (pollSec <= 0) pollSec = 30;

            if (now - s.lastQuoteFetch >= std::chrono::seconds(pollSec)) {
                s.lastQuoteFetch = now;
                std::thread(AsyncFetchQuote, s.cfg.providerId, s.cfg.ticker).detach();
            }
            if (s.needsOlder && !s.loading && !s.bars.empty() && s.canLoadOlder) {
                s.needsOlder = false;
                s.loading = true;
                s.loadingOlder = true;
                int64_t endMs = s.bars.front().timestampMs - 1;
                std::thread(AsyncFetchBars, s.cfg.providerId, s.cfg.ticker,
                            s.cfg.timeframeId, endMs, kOlderFetchLimit,
                            false, false, true).detach();
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// Merge fresh bars into dst. Returns number of NEW bars added.
static int MergeBars(std::vector<OHLCVBar>& dst, const std::vector<OHLCVBar>& fresh) {
    if (fresh.empty()) return 0;
    size_t oldSize = dst.size();

    std::unordered_set<int64_t> existing;
    existing.reserve(dst.size() * 2 + 16);
    for (auto& b : dst) existing.insert(b.timestampMs);

    for (auto& b : fresh) {
        if (existing.insert(b.timestampMs).second) dst.push_back(b);
    }
    if (dst.size() == oldSize) return 0;

    std::sort(dst.begin(), dst.end(),
        [](const OHLCVBar& a, const OHLCVBar& b){ return a.timestampMs < b.timestampMs; });
    return (int)(dst.size() - oldSize);
}

// Trim cache to kBufferMaxBars, keeping the region the user is viewing.
static void BoundBars(StockRuntime& s) {
    if (s.bars.size() <= kBufferMaxBars) return;
    size_t excess = s.bars.size() - kBufferMaxBars;

    int total = (int)s.bars.size();
    int shown = std::min(s.viewBars, total);
    int maxStart = std::max(0, total - shown);
    int focus = (s.viewStartIdx < 0) ? maxStart : std::min(s.viewStartIdx, maxStart);
    bool nearOldest = focus < total / 4;

    if (nearOldest) {
        s.bars.erase(s.bars.end() - excess, s.bars.end());
    } else {
        s.bars.erase(s.bars.begin(), s.bars.begin() + excess);
        if (s.viewStartIdx >= 0)
            s.viewStartIdx = std::max(0, s.viewStartIdx - (int)excess);
    }
}

static void DrainAsyncResults() {
    std::vector<AsyncResult> local;
    {
        std::lock_guard<std::mutex> lk(g_asyncMutex);
        local.swap(g_asyncQueue);
    }
    for (auto& r : local) {
        StockRuntime* s = FindStock(r.providerId, r.ticker);
        if (!s) continue;
        if (r.hasQuote) s->latest = r.quote;

        if (r.hasBars) {
            size_t oldSize = s->bars.size();
            int64_t oldFrontTs = oldSize > 0 ? s->bars.front().timestampMs : 0;

            int added = MergeBars(s->bars, r.bars);

            // If older bars were prepended, shift viewStartIdx to keep the
            // same visible bars.
            if (added > 0 && oldSize > 0 && s->viewStartIdx >= 0) {
                int prepended = 0;
                for (size_t i = 0; i < s->bars.size(); ++i) {
                    if (s->bars[i].timestampMs < oldFrontTs) prepended++;
                    else break;
                }
                s->viewStartIdx += prepended;
            }

            BoundBars(*s);
            s->loading = false;
            s->loadingOlder = false;

            if (r.isOlder) {
                if (added == 0 && !r.bars.empty()) {
                    s->canLoadOlder = false;   // all dupes -- hit the wall
                } else if (r.requestedLimit > 0 &&
                           (int)r.bars.size() < r.requestedLimit) {
                    s->canLoadOlder = false;
                }
            }

            if (r.isRefresh && s->pendingRefresh) {
                s->pendingRefresh = false;
                int grew = (int)s->bars.size() - s->refreshPreCount;
                if (grew > 0) {
                    ShowToast("Βρέθηκαν " + std::to_string(grew) + " νέες μπάρες.");
                } else {
                    ProviderRuntime* pr = FindProvider(r.providerId);
                    g_infoPopupRequested = true;
                    g_infoTitle = "Δεν βρέθηκαν νέα δεδομένα";
                    g_infoBody = std::string("Σύμβολο: ") + r.ticker +
                                 "\nΠάροχος: " +
                                 (pr ? pr->impl->DisplayName() : r.providerId);
                    g_infoLink = ProviderHomepage(r.providerId);
                }
            }

            if (!r.isRefresh && !r.isStaleProbe && !r.isOlder && !s->staleProbeTried &&
                !s->bars.empty() && IsLastBarStale(s->bars)) {
                s->staleProbeTried = true;
                s->loading = true;
                ProviderRuntime* pr = FindProvider(r.providerId);
                if (pr) {
                    auto caps = pr->impl->Capabilities();
                    std::thread(AsyncFetchBars, r.providerId, r.ticker,
                                s->cfg.timeframeId, (int64_t)0,
                                caps.maxBarsPerRequest, false, true, false).detach();
                }
            }
            if (r.isStaleProbe) {
                s->loading = false;
                s->mayBeUnsupported = IsLastBarStale(s->bars);
            }
        } else {
            // Fetch failed / returned nothing.
            if (r.isOlder) {
                s->loading = false;
                s->loadingOlder = false;
                s->canLoadOlder = false;
                ShowToast("Δεν υπάρχουν παλαιότερα δεδομένα.");
            } else if (r.isStaleProbe) {
                s->loading = false;
                s->mayBeUnsupported = true;
            } else if (r.isRefresh && s->pendingRefresh) {
                s->pendingRefresh = false;
                s->loading = false;
                ProviderRuntime* pr = FindProvider(r.providerId);
                g_infoPopupRequested = true;
                g_infoTitle = "Δεν βρέθηκαν νέα δεδομένα";
                g_infoBody = std::string("Σύμβολο: ") + r.ticker +
                             "\nΠάροχος: " +
                             (pr ? pr->impl->DisplayName() : r.providerId);
                g_infoLink = ProviderHomepage(r.providerId);
            } else {
                s->loading = false;
                s->loadingOlder = false;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------
static void LoadSaved() {
    SavedState st;
    if (!LoadState(g_statePath, st)) return;
    g_activeTab = st.activeTab;
    for (auto& [pid, key] : st.apiKeys) {
        ProviderRuntime* pr = FindProvider(pid);
        if (pr) { pr->apiKey = key; pr->impl->SetApiKey(key); pr->testPassed = true; }
    }
    for (auto& ss : st.stocks) {
        ProviderRuntime* pr = FindProvider(ss.providerId);
        if (!pr) continue;
        StockRuntime sr;
        sr.cfg = ss;
        if (sr.cfg.timeframeId.empty())
            sr.cfg.timeframeId = pr->impl->Capabilities().defaultTimeframeId;
        g_stocks.push_back(std::move(sr));
    }
    for (auto& s : g_stocks) {
        auto* pr = FindProvider(s.cfg.providerId);
        if (!pr) continue;
        auto caps = pr->impl->Capabilities();
        s.loading = true;
        std::thread(AsyncFetchBars, s.cfg.providerId, s.cfg.ticker,
                    s.cfg.timeframeId, (int64_t)0, caps.maxBarsPerRequest,
                    false, false, false).detach();
        std::thread(AsyncFetchQuote, s.cfg.providerId, s.cfg.ticker).detach();
    }
}

static void SaveNow() {
    SavedState st;
    st.activeTab = g_activeTab;
    for (auto& s : g_stocks) st.stocks.push_back(s.cfg);
    for (auto& p : g_providers)
        if (!p.apiKey.empty()) st.apiKeys.emplace_back(p.impl->Id(), p.apiKey);
    SaveState(g_statePath, st);
}

// ---------------------------------------------------------------------------
// Chart
// ---------------------------------------------------------------------------
static std::string FormatDate(int64_t ms) {
    if (ms <= 0) return "--";
    time_t t = (time_t)(ms / 1000);
    std::tm tmv{}; gmtime_s(&tmv, &t);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    return buf;
}

static void DrawChart(StockRuntime& s, double entry, double feePct, double targetP) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float width  = ImGui::GetContentRegionAvail().x;
    float height = 220.0f;
    float labelW = 78.0f;
    float plotW  = std::max(40.0f, width - labelW);
    ImVec2 chartMax = ImVec2(origin.x + plotW, origin.y + height);
    ImVec2 outerMax = ImVec2(origin.x + width, origin.y + height);

    dl->AddRectFilled(origin, outerMax, IM_COL32(20, 22, 28, 255), 3.0f);

    if (s.bars.empty()) {
        dl->AddRect(origin, outerMax, IM_COL32(60, 62, 70, 255), 3.0f);
        const char* msg = s.loading ? "Φόρτωση ιστορικού..." : "Δεν υπάρχει ιστορικό ακόμα.";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2(origin.x + (width - ts.x)*0.5f, origin.y + (height - ts.y)*0.5f),
                    IM_COL32(160, 165, 175, 255), msg);
        ImGui::Dummy(ImVec2(width, height));
        return;
    }

    int total    = (int)s.bars.size();
    int shown    = std::min(s.viewBars, total);
    int maxStart = total - shown;
    int firstIdx = (s.viewStartIdx < 0) ? maxStart : std::min(s.viewStartIdx, maxStart);
    if (firstIdx < 0) firstIdx = 0;

    double lo = s.bars[firstIdx].low, hi = s.bars[firstIdx].high;
    for (int i = firstIdx; i < firstIdx + shown; i++) {
        lo = std::min(lo, s.bars[i].low);
        hi = std::max(hi, s.bars[i].high);
    }
    double be = entry > 0 ? ComputeBreakEvenPrice(entry, feePct) : 0;
    if (entry   > 0) { lo = std::min(lo, entry);   hi = std::max(hi, entry); }
    if (be      > 0) { lo = std::min(lo, be);      hi = std::max(hi, be);    }
    if (targetP > 0) { lo = std::min(lo, targetP); hi = std::max(hi, targetP); }
    double pad = (hi - lo) * 0.08;
    if (pad <= 0) pad = hi * 0.01 + 0.01;
    lo -= pad; hi += pad;
    if (hi <= lo) hi = lo + 1;

    auto priceToY = [&](double p) {
        double f = (p - lo) / (hi - lo);
        f = std::min(std::max(f, 0.0), 1.0);
        return (float)(chartMax.y - f * height);
    };

    for (int i = 0; i <= 4; i++) {
        double p = lo + (hi - lo) * i / 4.0;
        float y = priceToY(p);
        dl->AddLine(ImVec2(origin.x, y), ImVec2(chartMax.x, y),
                    IM_COL32(40, 44, 52, 255), 1.0f);
        char buf[32]; snprintf(buf, sizeof(buf), "%.4f", p);
        ImVec2 ts = ImGui::CalcTextSize(buf);
        dl->AddText(ImVec2(chartMax.x + 6, y - ts.y*0.5f),
                    IM_COL32(180, 190, 200, 255), buf);
    }

    float slotW = plotW / (float)shown;
    float bodyW = std::max(1.0f, slotW * 0.6f);
    for (int i = 0; i < shown; i++) {
        const OHLCVBar& b = s.bars[firstIdx + i];
        float cx = origin.x + slotW * ((float)i + 0.5f);
        bool up = b.close >= b.open;
        ImU32 col = up ? IM_COL32(60, 170, 90, 255) : IM_COL32(200, 70, 70, 255);
        dl->AddLine(ImVec2(cx, priceToY(b.high)), ImVec2(cx, priceToY(b.low)), col, 1.0f);
        float yO = priceToY(b.open), yC = priceToY(b.close);
        float top = std::min(yO, yC), bot = std::max(yO, yC);
        if (bot - top < 1) bot = top + 1;
        dl->AddRectFilled(ImVec2(cx - bodyW*0.5f, top), ImVec2(cx + bodyW*0.5f, bot), col);
    }

    if (targetP > 0) {
        float y = priceToY(targetP);
        for (float x = origin.x; x < chartMax.x; x += 12)
            dl->AddLine(ImVec2(x, y), ImVec2(std::min(x+6, chartMax.x), y),
                        IM_COL32(80, 220, 130, 220), 1.5f);
        dl->AddText(ImVec2(origin.x + 4, y - 14),
                    IM_COL32(80, 220, 130, 255), "Στόχος");
    }
    if (entry > 0) {
        float y = priceToY(entry);
        dl->AddLine(ImVec2(origin.x, y), ImVec2(chartMax.x, y),
                    IM_COL32(90, 165, 255, 255), 1.5f);
        dl->AddText(ImVec2(origin.x + 4, y - 14),
                    IM_COL32(90, 165, 255, 255), "Είσοδος");
    }
    if (be > 0) {
        float y = priceToY(be);
        for (float x = origin.x; x < chartMax.x; x += 8)
            dl->AddLine(ImVec2(x, y), ImVec2(std::min(x+4, chartMax.x), y),
                        IM_COL32(80, 200, 220, 255), 1.5f);
        dl->AddText(ImVec2(origin.x + 4, y + 2),
                    IM_COL32(80, 200, 220, 255), "Break-even");
    }

    dl->AddRect(origin, outerMax, IM_COL32(60, 62, 70, 255), 3.0f);
    ImGui::Dummy(ImVec2(width, height));

    const char* loadHint = "";
    if (s.loadingOlder) loadHint = "   φόρτωση παλαιότερων...";
    else if (s.loading) loadHint = "   φόρτωση...";
    ImGui::TextDisabled("%s  →  %s   (%d bars, buffer %d)%s",
        FormatDate(s.bars[firstIdx].timestampMs).c_str(),
        FormatDate(s.bars[firstIdx + shown - 1].timestampMs).c_str(),
        shown, (int)s.bars.size(),
        loadHint);

    if (maxStart > 0) {
        int pos = firstIdx;
        ImGui::SetNextItemWidth(-90);
        if (ImGui::SliderInt("##scroll", &pos, 0, maxStart, "")) {
            s.viewStartIdx = pos;
            if (pos < kScrollEdge && s.canLoadOlder && !s.loading)
                s.needsOlder = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("τώρα")) s.viewStartIdx = -1;
    } else {
        ImGui::TextDisabled("(όλο το διαθέσιμο ιστορικό εμφανίζεται)");
    }

    ImGui::SetNextItemWidth(160);
    ImGui::SliderInt("Zoom", &s.viewBars, kMinViewBars, kMaxViewBars);
}

// ---------------------------------------------------------------------------
// Stock card
// ---------------------------------------------------------------------------
static bool g_removeRequested = false;
static StockRuntime* g_removeTarget = nullptr;

static void DrawStaleBanner(const StockRuntime& s) {
    if (!s.mayBeUnsupported || s.bars.empty()) return;
    std::string lastDate = FormatDate(s.bars.back().timestampMs);
    char buf[512];
    snprintf(buf, sizeof(buf),
        "  ⚠️  Τα τελευταία δεδομένα είναι από %s — το σύμβολο μπορεί να μην "
        "υποστηρίζεται πλέον.",
        lastDate.c_str());

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    float h = 30.0f;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), IM_COL32(70, 25, 30, 255), 4.0f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h),
                IM_COL32(160, 60, 60, 255), 4.0f, 0, 1.5f);
    dl->AddText(ImVec2(p.x + 8, p.y + 6), IM_COL32(255, 200, 200, 255), buf);
    ImGui::Dummy(ImVec2(w, h));
    ImGui::Spacing();
}

static void DrawStockCard(StockRuntime& s) {
    ProviderRuntime* pr = FindProvider(s.cfg.providerId);
    if (!pr) return;
    auto caps = pr->impl->Capabilities();

    ImGui::PushID((s.cfg.providerId + ":" + s.cfg.ticker).c_str());
    float cardTopY = ImGui::GetCursorPosY();

    ImGui::TextColored(ImVec4(0.55f, 0.78f, 1.0f, 1.0f), "%s", s.cfg.ticker.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("· %s", pr->impl->DisplayName().c_str());
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 190);
    if (ImGui::SmallButton("Ανανέωση")) {
        s.pendingRefresh = true;
        s.refreshPreCount = (int)s.bars.size();
        std::thread(AsyncFetchBars, s.cfg.providerId, s.cfg.ticker,
                    s.cfg.timeframeId, (int64_t)0, caps.maxBarsPerRequest,
                    true, false, false).detach();
        std::thread(AsyncFetchQuote, s.cfg.providerId, s.cfg.ticker).detach();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Αφαίρεση")) { g_removeTarget = &s; g_removeRequested = true; }

    DrawStaleBanner(s);

    if (s.latest.success) {
        ImGui::Text("Τιμή: %.4f %s", s.latest.price, s.latest.currency.c_str());
    } else if (!s.bars.empty()) {
        ImGui::Text("Τιμή: %.4f (τελευταίο κλείσιμο)", s.bars.back().close);
    } else {
        ImGui::TextDisabled("Τιμή: φόρτωση...");
    }

    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Είσοδος:");
    ImGui::SameLine(); ImGui::SetNextItemWidth(110);
    ImGui::InputDouble("##entry", &s.cfg.entryPrice, 0.0, 0.0, "%.4f");
    ImGui::SameLine(0, 20);
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Τέλη %:");
    ImGui::SameLine(); ImGui::SetNextItemWidth(80);
    ImGui::InputDouble("##fee", &s.cfg.feePercent, 0.0, 0.0, "%.3f");
    ImGui::SameLine(0, 20);
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Στόχος %:");
    ImGui::SameLine(); ImGui::SetNextItemWidth(80);
    ImGui::InputDouble("##target", &s.cfg.targetRoiPct, 0.0, 0.0, "%.2f");

    if (caps.supportedTimeframes.size() <= 1) {
        std::string disp = caps.supportedTimeframes.empty()
                            ? "—" : caps.supportedTimeframes[0].display;
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Timeframe:");
        ImGui::SameLine(); ImGui::TextDisabled("%s  (μόνο αυτό διαθέσιμο)", disp.c_str());
    } else {
        ImGui::SetNextItemWidth(160);
        std::string curTfDisplay;
        for (auto& tf : caps.supportedTimeframes)
            if (tf.id == s.cfg.timeframeId) curTfDisplay = tf.display;
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Timeframe:");
        ImGui::SameLine();
        if (ImGui::BeginCombo("##tf", curTfDisplay.c_str())) {
            for (auto& tf : caps.supportedTimeframes) {
                if (ImGui::Selectable(tf.display.c_str(), tf.id == s.cfg.timeframeId)) {
                    if (tf.id != s.cfg.timeframeId) {
                        s.cfg.timeframeId = tf.id;
                        s.bars.clear();
                        s.viewStartIdx = -1;
                        s.loading = true;
                        s.loadingOlder = false;
                        s.canLoadOlder = true;
                        s.mayBeUnsupported = false;
                        std::thread(AsyncFetchBars, s.cfg.providerId, s.cfg.ticker,
                                    tf.id, (int64_t)0, caps.maxBarsPerRequest,
                                    false, false, false).detach();
                    }
                }
            }
            ImGui::EndCombo();
        }
    }

    // Entry / break-even / target price line
    if (s.cfg.entryPrice > 0) {
        double be      = ComputeBreakEvenPrice(s.cfg.entryPrice, s.cfg.feePercent);
        double targetP = ComputeTargetPrice(s.cfg.entryPrice, s.cfg.feePercent,
                                            s.cfg.targetRoiPct);
        ImGui::TextDisabled(
            "Κόστος εισόδου: %.4f  ·  Break-even: %.4f  ·  Στόχος: %.4f  "
            "(καθαρά +%.2f%% μετά από τέλη)",
            s.cfg.entryPrice * (1.0 + s.cfg.feePercent / 100.0),
            be, targetP, s.cfg.targetRoiPct);

        double targetP2 = targetP;   // for DrawChart below
        (void)targetP2;
    }

    if (s.latest.success && s.cfg.entryPrice > 0) {
        double roi = ComputeRoiPercent(s.latest.price, s.cfg.entryPrice, s.cfg.feePercent);
        ImVec4 c = roi >= 0 ? ImVec4(0.35f,0.85f,0.45f,1) : ImVec4(0.90f,0.35f,0.35f,1);
        ImGui::TextColored(c, "ROI (καθαρό): %+.2f%%", roi);
        if (roi >= s.cfg.targetRoiPct) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.55f,0.78f,1.0f,1.0f),
                               " ✔ στόχος %+.1f%%", s.cfg.targetRoiPct);
        } else {
            ImGui::SameLine();
            ImGui::TextDisabled(" · στόχος %+.1f%% (μένουν %+.2f)",
                                s.cfg.targetRoiPct, s.cfg.targetRoiPct - roi);
        }
    } else {
        ImGui::TextDisabled("ROI: --  (ορίστε τιμή εισόδου)");
    }

    double targetP = (s.cfg.entryPrice > 0)
        ? ComputeTargetPrice(s.cfg.entryPrice, s.cfg.feePercent, s.cfg.targetRoiPct)
        : 0.0;
    DrawChart(s, s.cfg.entryPrice, s.cfg.feePercent, targetP);

    if (s.scrollToMe) {
        ImGui::SetScrollY(cardTopY - 8);
        s.scrollToMe = false;
    }

    ImGui::Separator();
    ImGui::PopID();
}

// ---------------------------------------------------------------------------
// Add-stock modal
// ---------------------------------------------------------------------------
static void DrawAddModal() {
    if (!g_showAddModal) return;
    ImGui::OpenPopup("Προσθήκη νέου στοιχείου");
    ImGui::SetNextWindowSize(ImVec2(560, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Προσθήκη νέου στοιχείου", &g_showAddModal,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    std::string curName = g_providers.empty() ? "" : g_providers[g_addProviderIdx].impl->DisplayName();
    ImGui::SetNextItemWidth(360);
    if (ImGui::BeginCombo("Πάροχος", curName.c_str())) {
        for (int i = 0; i < (int)g_providers.size(); i++) {
            if (ImGui::Selectable(g_providers[i].impl->DisplayName().c_str(), i == g_addProviderIdx)) {
                g_addProviderIdx = i;
                g_addResults.clear();
                g_addSearch[0] = 0;
                g_addSearchDirty = true;
                g_addModalOpened = std::chrono::steady_clock::now();
                g_addLastSearch = std::chrono::steady_clock::time_point{};
            }
        }
        ImGui::EndCombo();
    }
    if (g_providers.empty()) { ImGui::TextDisabled("Κανένας πάροχος."); ImGui::EndPopup(); return; }
    ProviderRuntime& pr = g_providers[g_addProviderIdx];

    if (pr.impl->NeedsApiKey() && pr.apiKey.empty()) {
        ImGui::TextColored(ImVec4(0.55f,0.78f,1.0f,1.0f), "Απαιτείται API key για αυτόν τον πάροχο.");
        ImGui::TextDisabled("Αποκτήστε ένα εδώ: %s", pr.impl->GetApiKeyHelpUrl().c_str());
        if (ImGui::Button("Εισαγωγή API key")) {
            g_keyProviderIdx = g_addProviderIdx;
            g_keyBuffer[0] = 0;
            g_showKeyModal = true;
        }
        ImGui::EndPopup();
        return;
    }

    ImGui::SetNextItemWidth(320);
    if (ImGui::InputTextWithHint("##addsearch", "Αναζήτηση συμβόλου...",
                                  g_addSearch, sizeof(g_addSearch))) {
        g_addSearchDirty = true;
        g_addLastSearch = std::chrono::steady_clock::now();
    }
    ImGui::SameLine();
    bool clicked = ImGui::Button("Αναζήτηση");
    bool idle = g_addSearchDirty &&
                std::chrono::steady_clock::now() - g_addLastSearch > std::chrono::milliseconds(400);

    // Auto-poll: if the (empty-query) result set is empty, keep re-asking
    // every 500 ms until the async loader in the provider finishes.
    bool autoPoll = g_addResults.empty() && g_addSearch[0] == 0 &&
                    std::chrono::steady_clock::now() - g_addLastSearch >
                        std::chrono::milliseconds(500);

    if (clicked || idle || autoPoll) {
        g_addLastSearch = std::chrono::steady_clock::now();
        g_addSearchDirty = false;
        g_addResults = pr.impl->SearchSymbols(g_addSearch, 200);
    }
    ImGui::SameLine();
    if (ImGui::Button("⟳")) {
        g_addResults = pr.impl->SearchSymbols(g_addSearch, 200);
        g_addModalOpened = std::chrono::steady_clock::now();
        g_addLastSearch = std::chrono::steady_clock::now();
    }

    ImGui::Separator();
    ImGui::BeginChild("##addlist", ImVec2(500, 240), true);

    auto addTicker = [&](const std::string& t) {
        StockRuntime s;
        s.cfg.providerId   = pr.impl->Id();
        s.cfg.ticker       = t;
        s.cfg.timeframeId  = pr.impl->Capabilities().defaultTimeframeId;
        s.cfg.addedAtMs    = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
        s.loading = true;
        s.scrollToMe = true;
        g_stocks.push_back(std::move(s));
        auto caps = pr.impl->Capabilities();
        std::thread(AsyncFetchBars, pr.impl->Id(), t,
                    pr.impl->Capabilities().defaultTimeframeId,
                    (int64_t)0, caps.maxBarsPerRequest, false, false, false).detach();
        std::thread(AsyncFetchQuote, pr.impl->Id(), t).detach();
        SaveNow();
        g_showAddModal = false;
    };

    auto renderList = [&](const std::vector<std::string>& list) {
        for (auto& t : list) {
            ImGui::PushID(t.c_str());
            bool exists = FindStock(pr.impl->Id(), t) != nullptr;
            if (exists) ImGui::BeginDisabled();
            if (ImGui::Selectable(t.c_str())) addTicker(t);
            if (exists) ImGui::EndDisabled();
            ImGui::PopID();
        }
    };

    bool hasRemote = (pr.impl->Id() == "binance" || pr.impl->Id() == "biquote");
    bool waited = std::chrono::steady_clock::now() - g_addModalOpened >
                  std::chrono::seconds(3);

    if (!g_addResults.empty()) {
        renderList(g_addResults);
    } else {
        if (g_addSearch[0] == 0) {
            if (hasRemote && waited)
                ImGui::TextDisabled("Η λίστα συμβόλων δεν έχει φορτωθεί ακόμα "
                                    "-- δοκιμάστε ⟳ ή ελέγξτε τη σύνδεση.");
            else if (hasRemote)
                ImGui::TextDisabled("Φόρτωση λίστας συμβόλων...");
            else
                ImGui::TextDisabled("(προεπιλεγμένη λίστα)");
        } else {
            ImGui::TextDisabled("Δεν βρέθηκε σύμβολο. Δοκιμάστε ⟳ ή άλλη αναζήτηση.");
        }
        renderList(pr.impl->DefaultTickers());
    }
    ImGui::EndChild();

    if (ImGui::Button("Κλείσιμο")) g_showAddModal = false;
    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// API-key modal + info popup
// ---------------------------------------------------------------------------
static void DrawKeyModal() {
    if (!g_showKeyModal) return;
    ImGui::OpenPopup("API Key");
    if (!ImGui::BeginPopupModal("API Key", &g_showKeyModal, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    if (g_keyProviderIdx < 0 || g_keyProviderIdx >= (int)g_providers.size()) {
        ImGui::TextDisabled("Άκυρος πάροχος."); ImGui::EndPopup(); return;
    }
    ProviderRuntime& pr = g_providers[g_keyProviderIdx];
    ImGui::Text("Πάροχος: %s", pr.impl->DisplayName().c_str());
    ImGui::TextDisabled("Αποκτήστε κλειδί: %s", pr.impl->GetApiKeyHelpUrl().c_str());
    ImGui::SetNextItemWidth(400);
    ImGui::InputText("##key", g_keyBuffer, sizeof(g_keyBuffer));
    if (ImGui::Button("Δοκιμή σύνδεσης")) {
        pr.impl->SetApiKey(g_keyBuffer);
        pr.apiKey = g_keyBuffer;
        pr.testResult = pr.impl->TestConnection();
        pr.testPassed = pr.testResult.empty();
    }
    if (!pr.testResult.empty()) ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), "%s", pr.testResult.c_str());
    else if (pr.testPassed)    ImGui::TextColored(ImVec4(0.4f,1,0.4f,1), "Επιτυχής σύνδεση.");
    ImGui::SameLine();
    if (ImGui::Button("Αποθήκευση")) { SaveNow(); g_showKeyModal = false; }
    ImGui::SameLine();
    if (ImGui::Button("Άκυρο")) g_showKeyModal = false;
    ImGui::EndPopup();
}

static void DrawInfoPopup() {
    if (g_infoPopupRequested) {
        ImGui::OpenPopup("Πληροφορία");
        g_infoPopupRequested = false;
    }
    if (!ImGui::BeginPopupModal("Πληροφορία", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f,0.78f,1.0f,1.0f));
    ImGui::TextUnformatted(g_infoTitle.c_str());
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextWrapped("%s", g_infoBody.c_str());
    if (!g_infoLink.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Πηγή:");
        ImGui::SameLine();
        if (ImGui::SmallButton(g_infoLink.c_str())) {
            ShellExecuteA(nullptr, "open", g_infoLink.c_str(),
                          nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
    ImGui::Spacing();
    if (ImGui::Button("OK", ImVec2(80, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------
// Main window
// ---------------------------------------------------------------------------
static void DrawMainWindow() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("SV", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    if (ImGui::BeginTabBar("providers")) {
        for (int i = 0; i < (int)g_providers.size(); i++) {
            auto& pr = g_providers[i];
            std::string label = pr.impl->DisplayName();
            if (pr.impl->NeedsApiKey() && pr.apiKey.empty()) label += "  🔒";
            if (ImGui::BeginTabItem(label.c_str())) {
                g_activeTab = i;
                ImGui::TextDisabled("Πάροχος: %s · %s",
                    pr.impl->AssetClass().c_str(), pr.impl->Id().c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("· %ld req, %ld errors, μέσο %0.1f ms, τώρα %0.1f req/s, τελευταίο %lld ms",
                    (long)pr.stats.totalRequests.load(),
                    (long)pr.stats.failedRequests.load(),
                    pr.stats.AvgLatencyMs(),
                    pr.stats.RecentRequestsPerSec(),
                    (long long)pr.stats.lastLatencyMs.load());
                ImGui::Separator();

                if (ImGui::Button("+ Προσθήκη")) {
                    g_showAddModal = true;
                    g_addProviderIdx = i;
                    g_addSearch[0] = 0;
                    g_addResults.clear();
                    g_addSearchDirty = true;
                    g_addModalOpened = std::chrono::steady_clock::now();
                    g_addLastSearch = std::chrono::steady_clock::time_point{};
                }
                ImGui::SameLine();
                if (ImGui::Button("Ανανέωση όλων")) {
                    for (auto& s : g_stocks) if (s.cfg.providerId == pr.impl->Id()) {
                        std::thread(AsyncFetchQuote, s.cfg.providerId, s.cfg.ticker).detach();
                        std::thread(AsyncFetchBars, s.cfg.providerId, s.cfg.ticker,
                                    s.cfg.timeframeId, (int64_t)0,
                                    pr.impl->Capabilities().maxBarsPerRequest,
                                    false, false, false).detach();
                    }
                    ShowToast("Ανανέωση...");
                }
                if (pr.impl->NeedsApiKey()) {
                    ImGui::SameLine();
                    if (ImGui::Button(pr.apiKey.empty() ? "Ορισμός API key" : "Αλλαγή API key")) {
                        g_keyProviderIdx = i;
                        snprintf(g_keyBuffer, sizeof(g_keyBuffer), "%s", pr.apiKey.c_str());
                        g_showKeyModal = true;
                    }
                }
                ImGui::Separator();

                ImGui::BeginChild("##cards", ImVec2(0, 0), false);
                for (auto& s : g_stocks) {
                    if (s.cfg.providerId != pr.impl->Id()) continue;
                    if (!s.cfg.visible) continue;
                    DrawStockCard(s);
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }

    DrawAddModal();
    DrawKeyModal();
    DrawInfoPopup();

    if (!g_toastMsg.empty() &&
        std::chrono::steady_clock::now() - g_toastTime < std::chrono::seconds(4)) {
        ImVec2 wpos = ImGui::GetMainViewport()->WorkPos;
        ImVec2 wsize = ImGui::GetMainViewport()->WorkSize;
        ImGui::SetNextWindowPos(ImVec2(wpos.x + wsize.x*0.5f, wpos.y + wsize.y - 40),
                                0, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowBgAlpha(0.88f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 8));
        ImGui::Begin("##toast", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs);
        ImGui::TextUnformatted(g_toastMsg.c_str());
        ImGui::End();
        ImGui::PopStyleVar();
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Boilerplate
// ---------------------------------------------------------------------------
static void GlfwErrorCallback(int e, const char* d) { fprintf(stderr, "GLFW %d: %s\n", e, d); }

static bool CheckInternet() {
    std::string body;
    if (HttpGet("biquote.io/api/EURUSD", body)) return true;
    if (HttpGetJson(L"data-api.binance.vision", L"/api/v3/ping", body)) return true;
    return false;
}

int main(int, char**) {
    g_statePath = DefaultStatePath();

    if (!CheckInternet()) {
        int r = MessageBoxA(nullptr,
            "Δεν βρέθηκε σύνδεση στο Internet.\n\n"
            "Η εφαρμογή δεν μπορεί να φορτώσει δεδομένα αγοράς χωρίς σύνδεση:\n"
            "δεν θα λειτουργήσουν οι ανανεώσεις, η αναζήτηση συμβόλων,\n"
            "ούτε τα ιστορικά διαγράμματα.\n\n"
            "Πατήστε OK για έξοδο και επαναλάβετε με σύνδεση, ή\n"
            "Cancel για να συνεχίσετε και να επεξεργαστείτε ρυθμίσεις.",
            "StockViewer -- Χωρίς σύνδεση",
            MB_OKCANCEL | MB_ICONWARNING);
        if (r == IDOK) return 0;
    }

    for (auto& p : MakeAllProviders()) {
        ProviderRuntime r;
        r.impl = std::move(p);
        g_providers.push_back(std::move(r));
    }
    std::vector<std::string> pluginLog;
    for (auto& p : LoadPluginsFrom("plugins", pluginLog)) {
        ProviderRuntime r;
        r.impl = std::move(p);
        r.isPlugin = true;
        g_providers.push_back(std::move(r));
    }
    for (auto& line : pluginLog) fprintf(stderr, "[plugin] %s\n", line.c_str());

    LoadSaved();
    g_pollerThread = std::thread(PollerLoop);

    glfwSetErrorCallback(GlfwErrorCallback);
    if (!glfwInit()) return 1;
    const char* glsl = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    GLFWwindow* window = glfwCreateWindow(1400, 950, "StockViewer", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    ImGui::StyleColorsDark();

    {
        ImGuiStyle& st = ImGui::GetStyle();
        st.WindowRounding = 4; st.FrameRounding = 3; st.GrabRounding = 3;
        st.ScrollbarRounding = 3; st.WindowPadding = ImVec2(12,12);
        ImVec4* c = st.Colors;
        c[ImGuiCol_WindowBg]       = ImVec4(0.071f,0.075f,0.086f,1);
        c[ImGuiCol_ChildBg]        = ImVec4(0.098f,0.106f,0.125f,1);
        c[ImGuiCol_PopupBg]        = ImVec4(0.098f,0.106f,0.125f,0.98f);
        c[ImGuiCol_Border]         = ImVec4(0.180f,0.196f,0.227f,1);
        c[ImGuiCol_FrameBg]        = ImVec4(0.137f,0.149f,0.176f,1);
        c[ImGuiCol_FrameBgHovered] = ImVec4(0.180f,0.196f,0.227f,1);
        c[ImGuiCol_Button]         = ImVec4(0.220f,0.500f,0.850f,1);
        c[ImGuiCol_ButtonHovered]  = ImVec4(0.300f,0.600f,0.950f,1);
        c[ImGuiCol_ButtonActive]   = ImVec4(0.160f,0.420f,0.780f,1);
        c[ImGuiCol_Header]         = ImVec4(0.180f,0.196f,0.227f,1);
        c[ImGuiCol_HeaderHovered]  = ImVec4(0.220f,0.240f,0.278f,1);
        c[ImGuiCol_Tab]                = ImVec4(0.098f,0.106f,0.125f,1);
        c[ImGuiCol_TabHovered]         = ImVec4(0.200f,0.300f,0.420f,1);
        c[ImGuiCol_TabActive]          = ImVec4(0.200f,0.480f,0.820f,1);
        c[ImGuiCol_TabUnfocused]       = ImVec4(0.098f,0.106f,0.125f,1);
        c[ImGuiCol_TabUnfocusedActive] = ImVec4(0.140f,0.260f,0.440f,1);
        c[ImGuiCol_Text]           = ImVec4(0.925f,0.937f,0.949f,1);
        c[ImGuiCol_TextDisabled]   = ImVec4(0.510f,0.545f,0.596f,1);
        c[ImGuiCol_SliderGrab]     = ImVec4(0.350f,0.620f,0.950f,1);
        c[ImGuiCol_SliderGrabActive] = ImVec4(0.450f,0.720f,1.000f,1);
    }

    static const ImWchar ranges[] = {
        0x0020,0x00FF, 0x0370,0x03FF, 0x1F00,0x1FFF,
        0x2010,0x2027, 0x20A0,0x20CF, 0x2190,0x21FF, 0x25A0,0x25FF, 0
    };
    ImFontConfig fc; fc.OversampleH = 2; fc.OversampleV = 2;
    ImFont* f = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 18.0f, &fc, ranges);
    if (!f) f = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\arial.ttf", 18.0f, &fc, ranges);
    if (f) io.FontDefault = f;

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        DrainAsyncResults();

        if (g_removeRequested && g_removeTarget) {
            auto it = std::find_if(g_stocks.begin(), g_stocks.end(),
                [&](const StockRuntime& s){ return &s == g_removeTarget; });
            if (it != g_stocks.end()) g_stocks.erase(it);
            g_removeTarget = nullptr;
            g_removeRequested = false;
            SaveNow();
        }

        DrawMainWindow();

        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.05f, 0.05f, 0.07f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    SaveNow();
    g_shuttingDown.store(true);
    g_pollerRun.store(false);
    if (g_pollerThread.joinable()) g_pollerThread.join();
    UnloadAllPlugins();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}