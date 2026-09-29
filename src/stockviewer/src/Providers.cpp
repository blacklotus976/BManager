#include "Providers.h"
#include "HttpFetch.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <memory>
#include <mutex>
#include <thread>
#include <functional>
#include <chrono>
#include <atomic>
#include <sstream>

using json = nlohmann::json;

static std::wstring Widen(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}
static std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

static int64_t ParseIso8601Ms(const std::string& iso) {
    if (iso.size() < 19) return 0;
    std::tm tm{};
    try {
        tm.tm_year = std::stoi(iso.substr(0, 4)) - 1900;
        tm.tm_mon  = std::stoi(iso.substr(5, 2)) - 1;
        tm.tm_mday = std::stoi(iso.substr(8, 2));
        tm.tm_hour = std::stoi(iso.substr(11, 2));
        tm.tm_min  = std::stoi(iso.substr(14, 2));
        tm.tm_sec  = std::stoi(iso.substr(17, 2));
    } catch (...) { return 0; }
    time_t t = _mkgmtime(&tm);
    return (int64_t)t * 1000;
}

// ---------------------------------------------------------------------------
// Async symbol cache.
// ---------------------------------------------------------------------------
struct SymbolCache {
    std::mutex                mtx;
    std::vector<std::string>  symbols;
    std::atomic<bool>         loading{false};
    std::atomic<bool>         loaded{false};
    std::atomic<int64_t>      lastAttemptMs{0};
};
using SymbolCachePtr = std::shared_ptr<SymbolCache>;

// Background, retryable load. Only marks "loaded" when we actually got
// symbols so a bad/empty response retries (throttled to 3s).
static void EnsureSymbolLoad(SymbolCachePtr cache,
                             std::function<void(std::vector<std::string>&)> loader) {
    if (cache->loaded.load()) return;
    bool expected = false;
    if (!cache->loading.compare_exchange_strong(expected, true)) return;

    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    int64_t prev = cache->lastAttemptMs.exchange(now);
    if (now - prev < 3000) { cache->loading = false; return; }

    std::thread([cache, loader]{
        std::vector<std::string> loaded;
        try { loader(loaded); } catch (...) {}
        {
            std::lock_guard<std::mutex> lk(cache->mtx);
            if (!loaded.empty()) {
                cache->symbols = std::move(loaded);
                cache->loaded = true;
            }
        }
        cache->loading = false;
    }).detach();
}

static std::vector<std::string> FilterCache(const SymbolCachePtr& c,
                                            const std::string& query, int limit) {
    std::string q = Lower(query);
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lk(c->mtx);
    for (auto& s : c->symbols) {
        if (q.empty() || Lower(s).find(q) != std::string::npos) {
            out.push_back(s);
            if ((int)out.size() >= limit) break;
        }
    }
    return out;
}

// ===========================================================================
// biquote
// ===========================================================================
class BiquoteForexProvider : public IMarketDataProvider {
    SymbolCachePtr m_cache = std::make_shared<SymbolCache>();
public:
    std::string Id()          const override { return "biquote"; }
    std::string DisplayName() const override { return "Forex / Metals / Crypto (biquote)"; }
    std::string AssetClass()  const override { return "Forex"; }

    std::string TestConnection() override {
        std::string body;
        if (!HttpGet("biquote.io/api/EURUSD", body))
            return "Δεν ήταν δυνατή η σύνδεση με το biquote.io";
        return "";
    }

    ProviderCapabilities Capabilities() const override {
        ProviderCapabilities c;
        c.supportedTimeframes = {
            {"1m",  "1 λεπτό",   60},
            {"5m",  "5 λεπτά",   300},
            {"15m", "15 λεπτά",  900},
            {"1h",  "1 ώρα",     3600},
            {"4h",  "4 ώρες",    14400},
            {"1d",  "1 μέρα",    86400},
        };
        c.defaultTimeframeId  = "1h";
        c.supportsHistorical  = true;
        c.supportsSymbolSearch = true;
        c.maxBarsPerRequest   = 500;
        c.suggestedPollSeconds = 5;
        return c;
    }

    std::vector<std::string> DefaultTickers() const override {
        return { "EURUSD", "GBPUSD", "USDJPY", "AUDUSD",
                 "USDCAD", "USDCHF", "EURGBP", "EURJPY",
                 "XAUUSD", "XAGUSD" };
    }

    std::vector<std::string> SearchSymbols(const std::string& query, int limit) override {
        EnsureSymbolLoad(m_cache, [](std::vector<std::string>& out){
            std::string body;
            if (!HttpGet("biquote.io/api/symbols", body)) return;
            try {
                json j = json::parse(body);
                // Response may be a bare array or an object wrapping one.
                if (j.is_object()) {
                    for (auto it = j.begin(); it != j.end(); ++it) {
                        if (it.value().is_array()) { j = it.value(); break; }
                    }
                }
                if (!j.is_array()) return;
                for (auto& s : j) {
                    if (!s.is_object()) continue;
                    if (s.value("isActive", true) == false) continue;
                    if (s.value("hasData",  false) == false) continue;
                    std::string type = s.value("type", std::string());
                    if (type == "Forex" || type == "Commodity" ||
                        type == "Crypto" || type == "Index" || type.empty()) {
                        if (s.contains("name") && s["name"].is_string())
                            out.push_back(s["name"].get<std::string>());
                    }
                }
            } catch (...) {}
        });
        return FilterCache(m_cache, query, limit);
    }

    bool FetchQuote(const std::string& ticker, Quote& out) override {
        std::string body;
        if (!HttpGet("biquote.io/api/" + ticker, body)) {
            out.success = false; out.error = "biquote: σφάλμα δικτύου"; return false;
        }
        try {
            json j = json::parse(body);
            out.price       = j.at("mid").get<double>();
            out.currency    = ticker.size() >= 3 ? ticker.substr(ticker.size() - 3) : "USD";
            out.timestampMs = ParseIso8601Ms(j.value("timestamp", std::string()));
            out.success     = true;
            return true;
        } catch (...) {
            out.success = false; out.error = "biquote: μη αναμενόμενη απάντηση"; return false;
        }
    }

    bool FetchOHLCV(const std::string& ticker,
                    const std::string& timeframeId,
                    int64_t /*endTimeMs*/,
                    int limit,
                    std::vector<OHLCVBar>& out) override {
        std::string body;
        std::string url = "biquote.io/api/" + ticker + "/ohlc?interval=" + timeframeId
                        + "&limit=" + std::to_string(limit);
        if (!HttpGet(url, body)) return false;
        try {
            json j = json::parse(body);
            out.clear();
            for (auto& bar : j.at("bars")) {
                OHLCVBar b;
                b.timestampMs = ParseIso8601Ms(bar.value("openTime", std::string()));
                b.open  = bar.at("open").get<double>();
                b.high  = bar.at("high").get<double>();
                b.low   = bar.at("low").get<double>();
                b.close = bar.at("close").get<double>();
                out.push_back(b);
            }
            std::reverse(out.begin(), out.end());
            return !out.empty();
        } catch (...) { return false; }
    }
};

// ===========================================================================
// Binance
// ===========================================================================
class BinanceCryptoProvider : public IMarketDataProvider {
    SymbolCachePtr m_cache = std::make_shared<SymbolCache>();
public:
    std::string Id()          const override { return "binance"; }
    std::string DisplayName() const override { return "Crypto (Binance)"; }
    std::string AssetClass()  const override { return "Crypto"; }

    std::string TestConnection() override {
        std::string body;
        if (!HttpGetJson(L"data-api.binance.vision",
                         L"/api/v3/exchangeInfo?symbol=BTCUSDT", body))
            return "Δεν ήταν δυνατή η σύνδεση με data-api.binance.vision";
        return "";
    }

    ProviderCapabilities Capabilities() const override {
        ProviderCapabilities c;
        c.supportedTimeframes = {
            {"1m",  "1 λεπτό",  60},
            {"5m",  "5 λεπτά",  300},
            {"15m", "15 λεπτά", 900},
            {"1h",  "1 ώρα",    3600},
            {"4h",  "4 ώρες",   14400},
            {"1d",  "1 μέρα",   86400},
            {"1w",  "1 εβδομάδα", 604800},
        };
        c.defaultTimeframeId = "1h";
        c.supportsHistorical = true;
        c.supportsSymbolSearch = true;
        c.maxBarsPerRequest = 1000;
        c.suggestedPollSeconds = 3;
        return c;
    }

    std::vector<std::string> DefaultTickers() const override {
        return { "BTCUSDT", "ETHUSDT", "BNBUSDT", "SOLUSDT", "XRPUSDT" };
    }

    std::vector<std::string> SearchSymbols(const std::string& query, int limit) override {
        EnsureSymbolLoad(m_cache, [](std::vector<std::string>& out){
            std::string body;
            if (!HttpGetJson(L"data-api.binance.vision", L"/api/v3/exchangeInfo", body)) return;
            try {
                json j = json::parse(body);
                for (auto& s : j.at("symbols")) {
                    std::string status = s.value("status", std::string(""));
                    if (status == "TRADING") out.push_back(s.at("symbol").get<std::string>());
                }
            } catch (...) {}
        });
        return FilterCache(m_cache, query, limit);
    }

    bool FetchQuote(const std::string& ticker, Quote& out) override {
        std::string body;
        std::wstring path = L"/api/v3/ticker/price?symbol=" + Widen(ticker);
        if (!HttpGetJson(L"data-api.binance.vision", path, body)) {
            out.success = false; out.error = "Binance: σφάλμα δικτύου"; return false;
        }
        try {
            json j = json::parse(body);
            out.price    = std::stod(j.at("price").get<std::string>());
            out.currency = "USDT";
            out.success  = true;
            return true;
        } catch (...) {
            out.success = false; out.error = "Binance: μη αναμενόμενη απάντηση"; return false;
        }
    }

    bool FetchOHLCV(const std::string& ticker,
                    const std::string& timeframeId,
                    int64_t endTimeMs,
                    int limit,
                    std::vector<OHLCVBar>& out) override {
        std::string body;
        std::wstring path = L"/api/v3/klines?symbol=" + Widen(ticker) +
                             L"&interval=" + Widen(timeframeId) +
                             L"&limit=" + std::to_wstring(limit);
        if (endTimeMs > 0)
            path += L"&endTime=" + std::to_wstring(endTimeMs);
        if (!HttpGetJson(L"data-api.binance.vision", path, body)) return false;
        try {
            json j = json::parse(body);
            out.clear();
            for (auto& k : j) {
                OHLCVBar b;
                b.timestampMs = k[0].get<int64_t>();
                b.open  = std::stod(k[1].get<std::string>());
                b.high  = std::stod(k[2].get<std::string>());
                b.low   = std::stod(k[3].get<std::string>());
                b.close = std::stod(k[4].get<std::string>());
                out.push_back(b);
            }
            return !out.empty();
        } catch (...) { return false; }
    }
};

// ===========================================================================
// Naftemporiki -- Greek stocks via services.naftemporiki.gr. Daily only.
//
// Symbol list: the service has NO discovery endpoint that I've been able to
// confirm. The kNaSym table below is a hardcoded baseline that is loaded
// into the symbol cache immediately at construction (so searches work right
// away). A background thread additionally tries the most plausible
// discovery endpoints and, if any respond with a list, enriches the cache.
//
// The kNaSym table ALSO serves as the Latin -> Greek name mapping needed to
// build the history URL. For any ticker not in the table, the Latin name is
// sent as the Greek name, which the naftemporiki endpoint accepts for most
// symbols.
// ===========================================================================

struct NaftemporikiSymEntry { const char* latin; const char* greek; };

static const NaftemporikiSymEntry kNaSym[] = {
    {"ALPHA",    "ΑΛΦΑ"},
    {"EUROB",    "ΕΥΡΩΒ"},
    {"ETE",      "ΕΤΕ"},
    {"TPEIR",    "ΠΕΙΡ"},
    {"OPAP",     "ΟΠΑΠ"},
    {"PPC",      "ΔΕΗ"},
    {"OTE",      "ΟΤΕ"},
    {"MYTIL",    "ΜΥΤΙΛ"},
    {"TITC",     "ΤΙΤΚ"},
    {"MOH",      "ΜΟΗ"},
    {"BELA",     "ΜΠΕΛΑ"},
    {"GEKTERNA", "ΓΕΚΤΕΡΝΑ"},
    {"LAMDA",    "ΛΑΜΔΑ"},
    {"VIO",      "ΒΙΟ"},
    {"ELLAKTOR", "ΕΛΛΑΚΤΩΡ"},
    {"EYDAP",    "ΕΥΔΑΠ"},
    {"EYAPS",    "ΕΥΑΘ"},
    {"ADMIE",    "ΑΔΜΗΕ"},
    {"AIA",      "ΑΙΑ"},
    {"SAR",      "ΣΑΡ"},
    {"FRL",      "ΦΟΥΡΛΗΣ"},
    {"KRI",      "ΚΡΙΚΡΙ"},
    {"KARE",     "ΚΑΡΕΛΙΑΣ"},
    {"PAP",      "ΠΑΠΑΣΤΡΑΤΟΣ"},
    {"OTOEL",    "ΟΤΟΕΛ"},
    {"QUEST",    "QUEST"},
    {"INTRK",    "ΙΝΤΡΑΚΑΤ"},
    {"PLAT",     "ΠΛΑΙΣΙΟ"},
    {"EEE",      "ΕΕΕ"},
    {"MATHIO",   "ΜΑΘΙΟ"},
};

// Try to scrape the full ticker list from likely naftemporiki endpoints.
// Returns raw symbol strings (may include .ATH suffix; we strip it).
static std::vector<std::string> TryScrapeNaftemporikiSymbols() {
    std::vector<std::string> result;

    const char* urls[] = {
        "services.naftemporiki.gr/finance/Data/GetStocks.aspx",
        "services.naftemporiki.gr/finance/Data/GetStockList.aspx",
        "services.naftemporiki.gr/finance/Data/GetSymbols.aspx",
        "services.naftemporiki.gr/finance/Data/GetAllStocks.aspx",
    };

    for (auto url : urls) {
        std::string body;
        if (!HttpGet(url, body)) continue;
        if (body.empty()) continue;

        // --- Try JSON first ---
        bool parsedJson = false;
        try {
            json j = json::parse(body);
            std::vector<std::string> tmp;

            auto extractObj = [&](const json& item) {
                if (!item.is_object()) return std::string();
                for (auto& k : { "symbol", "Symbol", "ticker", "Ticker",
                                 "code", "Code", "name", "Name" }) {
                    if (item.contains(k) && item[k].is_string())
                        return item[k].get<std::string>();
                }
                return std::string();
            };
            auto extractArr = [&](const json& arr) {
                if (!arr.is_array()) return;
                for (auto& item : arr) {
                    if (item.is_string()) tmp.push_back(item.get<std::string>());
                    else { auto s = extractObj(item); if (!s.empty()) tmp.push_back(s); }
                }
            };

            if (j.is_array()) { extractArr(j); parsedJson = true; }
            else if (j.is_object()) {
                for (auto it = j.begin(); it != j.end(); ++it)
                    if (it.value().is_array()) extractArr(it.value());
                parsedJson = true;
            }
            if (!tmp.empty()) result = std::move(tmp);
        } catch (...) { parsedJson = false; }

        if (!result.empty()) break;

        // --- Try CSV ---
        std::istringstream iss(body);
        std::string line;
        std::vector<std::string> tmp;
        while (std::getline(iss, line)) {
            if (line.empty()) continue;
            if (line[0] == '<') break;   // HTML, not CSV
            auto comma = line.find(',');
            std::string sym = (comma == std::string::npos) ? line : line.substr(0, comma);
            while (!sym.empty() &&
                   (sym.back() == '\r' || sym.back() == ' ' || sym.back() == '\t'))
                sym.pop_back();
            if (sym.size() < 2 || sym.size() > 20) continue;
            bool valid = true;
            for (char ch : sym)
                if (!isalnum((unsigned char)ch) && ch != '.' && ch != '_' && ch != '-') {
                    valid = false; break;
                }
            if (valid) tmp.push_back(sym);
        }
        if (!tmp.empty()) result = std::move(tmp);
        if (!result.empty()) break;
    }

    // Strip .ATH suffix if present.
    for (auto& s : result) {
        if (s.size() > 4 && s.substr(s.size() - 4) == ".ATH")
            s = s.substr(0, s.size() - 4);
    }
    return result;
}

class NaftemporikiProvider : public IMarketDataProvider {
    SymbolCachePtr           m_cache = std::make_shared<SymbolCache>();
    std::vector<std::string> m_latinList;

    static std::string UrlEncode(const std::string& s) {
        static const char* hex = "0123456789ABCDEF";
        std::string out; out.reserve(s.size() * 3);
        for (unsigned char c : s) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
            else { out += '%'; out += hex[(c >> 4) & 0xF]; out += hex[c & 0xF]; }
        }
        return out;
    }

    static int64_t ParseGreekDate(const std::string& d) {
        size_t p1 = d.find('/'); if (p1 == std::string::npos) return 0;
        size_t p2 = d.find('/', p1 + 1); if (p2 == std::string::npos) return 0;
        int m = 0, day = 0, y = 0;
        try {
            m   = std::stoi(d.substr(0, p1));
            day = std::stoi(d.substr(p1 + 1, p2 - p1 - 1));
            y   = std::stoi(d.substr(p2 + 1));
        } catch (...) { return 0; }
        if (y < 1900 || y > 2200 || m < 1 || m > 12 || day < 1 || day > 31) return 0;
        std::tm tm{};
        tm.tm_year = y - 1900; tm.tm_mon = m - 1; tm.tm_mday = day;
        time_t t = _mkgmtime(&tm);
        return (int64_t)t * 1000;
    }

    static std::vector<std::string> SplitCSV(const std::string& line) {
        std::vector<std::string> out; size_t start = 0;
        for (size_t i = 0; i <= line.size(); ++i) {
            if (i == line.size() || line[i] == ',') {
                out.push_back(line.substr(start, i - start));
                start = i + 1;
            }
        }
        return out;
    }

    static const char* GreekFor(const std::string& latin) {
        for (auto& e : kNaSym) if (latin == e.latin) return e.greek;
        return nullptr;
    }

    static std::string BuildUrl(const std::string& latin) {
        std::string sym   = latin + ".ATH";
        const char* g     = GreekFor(latin);
        std::string tick  = g ? g : latin;
        return "services.naftemporiki.gr/finance/Data/GetHistoryMetastock.aspx"
               "?symbol=" + UrlEncode(sym) + "&ticker=" + UrlEncode(tick);
    }

public:
    NaftemporikiProvider() {
        // Pre-populate cache with the hardcoded baseline list so searches
        // work immediately.
        {
            std::lock_guard<std::mutex> lk(m_cache->mtx);
            for (auto& e : kNaSym) {
                m_cache->symbols.push_back(e.latin);
                m_latinList.push_back(e.latin);
            }
            m_cache->loaded = true;   // we already have a usable list
        }
        // Background: try to enrich the list from the service.
        std::thread([cache = m_cache]{
            auto scraped = TryScrapeNaftemporikiSymbols();
            if (scraped.empty()) return;
            std::lock_guard<std::mutex> lk(cache->mtx);
            for (auto& s : scraped) {
                if (std::find(cache->symbols.begin(), cache->symbols.end(), s)
                        == cache->symbols.end())
                    cache->symbols.push_back(s);
            }
        }).detach();
    }

    std::string Id()          const override { return "naftemporiki"; }
    std::string DisplayName() const override { return "Ελληνικές Μετοχές (Ναυτεμπορική)"; }
    std::string AssetClass()  const override { return "Greek Stocks"; }

    std::string TestConnection() override {
        std::string body;
        if (!HttpGet(BuildUrl("MATHIO"), body))
            return "Δεν ήταν δυνατή η σύνδεση με services.naftemporiki.gr";
        if (body.empty()) return "Κενή απάντηση από services.naftemporiki.gr";
        return "";
    }

    ProviderCapabilities Capabilities() const override {
        ProviderCapabilities c;
        c.supportedTimeframes = { {"D", "Ημερήσιο (D)", 86400} };
        c.defaultTimeframeId   = "D";
        c.supportsHistorical   = true;
        c.supportsSymbolSearch = true;
        c.maxBarsPerRequest    = 5000;
        c.suggestedPollSeconds = 300;
        return c;
    }

    std::vector<std::string> DefaultTickers() const override {
        return { "MATHIO", "ALPHA", "EUROB", "ETE", "TPEIR",
                 "OPAP", "PPC", "OTE", "MYTIL", "TITC" };
    }

    std::vector<std::string> SearchSymbols(const std::string& query, int limit) override {
        return FilterCache(m_cache, query, limit);
    }

    bool FetchQuote(const std::string& ticker, Quote& out) override {
        std::vector<OHLCVBar> bars;
        if (!FetchOHLCV(ticker, "D", 0, 1, bars)) {
            out.success = false; out.error = "Naftemporiki: σφάλμα δικτύου"; return false;
        }
        if (bars.empty()) {
            out.success = false; out.error = "Naftemporiki: δεν βρέθηκαν δεδομένα"; return false;
        }
        out.price       = bars.back().close;
        out.currency    = "EUR";
        out.timestampMs = bars.back().timestampMs;
        out.success     = true;
        return true;
    }

    bool FetchOHLCV(const std::string& ticker,
                    const std::string& /*timeframeId*/,
                    int64_t endTimeMs,
                    int limit,
                    std::vector<OHLCVBar>& out) override {
        std::string body;
        if (!HttpGet(BuildUrl(ticker), body)) return false;

        out.clear();
        size_t pos = 0;
        while (pos < body.size()) {
            size_t eol = body.find('\n', pos);
            if (eol == std::string::npos) eol = body.size();
            std::string line = body.substr(pos, eol - pos);
            pos = (eol == body.size()) ? body.size() : eol + 1;
            while (!line.empty() &&
                   (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                line.pop_back();
            if (line.empty()) continue;

            auto f = SplitCSV(line);
            if (f.size() < 8) continue;

            OHLCVBar b;
            b.timestampMs = ParseGreekDate(f[2]);
            if (b.timestampMs == 0) continue;
            try {
                b.high   = std::stod(f[3]);
                b.low    = std::stod(f[4]);
                b.close  = std::stod(f[5]);
                b.open   = std::stod(f[6]);
                b.volume = std::stod(f[7]);
            } catch (...) { continue; }

            if (b.open == 0 && b.high == 0 && b.low == 0 && b.close == 0) continue;
            if (b.open == 0) b.open = b.close;
            if (b.high == 0) b.high = b.close;
            if (b.low  == 0) b.low  = b.close;

            out.push_back(b);
        }
        if (out.empty()) return false;

        std::sort(out.begin(), out.end(),
            [](const OHLCVBar& a, const OHLCVBar& c){ return a.timestampMs < c.timestampMs; });
        if (endTimeMs > 0) {
            out.erase(std::remove_if(out.begin(), out.end(),
                [&](const OHLCVBar& b){ return b.timestampMs > endTimeMs; }), out.end());
        }
        if (limit > 0 && (int)out.size() > limit) {
            out.erase(out.begin(), out.begin() + (out.size() - limit));
        }
        return !out.empty();
    }
};

// ---------------------------------------------------------------------------
// Factories
// ---------------------------------------------------------------------------
std::unique_ptr<IMarketDataProvider> MakeBiquoteForexProvider()  { return std::make_unique<BiquoteForexProvider>(); }
std::unique_ptr<IMarketDataProvider> MakeBinanceCryptoProvider() { return std::make_unique<BinanceCryptoProvider>(); }
std::unique_ptr<IMarketDataProvider> MakeNaftemporikiProvider()  { return std::make_unique<NaftemporikiProvider>(); }

std::vector<std::unique_ptr<IMarketDataProvider>> MakeAllProviders() {
    std::vector<std::unique_ptr<IMarketDataProvider>> v;
    v.push_back(MakeNaftemporikiProvider());
    v.push_back(MakeBiquoteForexProvider());
    v.push_back(MakeBinanceCryptoProvider());
    return v;
}