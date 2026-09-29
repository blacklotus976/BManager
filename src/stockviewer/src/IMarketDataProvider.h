#pragma once
#include <string>
#include <vector>
#include <cstdint>

// ---------------------------------------------------------------------------
// Stable, DLL-safe interface. Any plugin built against this exact header
// with the same MSVC toolset can be dropped into plugins/ and be picked up
// at startup -- no rebuild of the host app required.
//
// Rules to keep the ABI stable across the DLL boundary:
//   * Only pure virtual methods, no data members, no inline implementations
//     of anything that touches state.
//   * Return/param types are POD, std::string, std::vector, or the structs
//     below (all trivially layout-compatible under a single MSVC version).
//   * Providers NEVER call back into the host; they only answer questions.
//   * Lifetime is managed via Destroy(); the host must not call `delete`
//     on a provider pointer (the DLL owns its allocator).
// ---------------------------------------------------------------------------

struct Quote {
    double      price    = 0.0;
    std::string currency = "EUR";
    bool        success  = false;
    std::string error;
    int64_t     timestampMs = 0;   // 0 = unknown
};

struct OHLCVBar {
    int64_t timestampMs = 0;       // bar open time, UTC ms
    double  open = 0, high = 0, low = 0, close = 0;
    double  volume = 0;
};

struct Timeframe {
    std::string id;                // provider-native token: "1m", "1h", "1d"
    std::string display;           // "1 λεπτό", "1 ώρα", "1 μέρα"
    int         seconds = 0;       // for the x-axis math
};

struct ProviderCapabilities {
    std::vector<Timeframe> supportedTimeframes;   // must be non-empty
    std::string            defaultTimeframeId;    // must be one of the above
    bool  supportsHistorical   = true;            // can fetch past bars
    bool  supportsStreaming    = false;           // has WS in future
    bool  supportsSymbolSearch = false;           // SearchSymbols() works
    int   maxBarsPerRequest    = 500;
    int   suggestedPollSeconds = 30;              // used for the quote loop
};

class IMarketDataProvider {
public:
    virtual ~IMarketDataProvider() = default;

    // --- Identity -----------------------------------------------------
    virtual std::string Id()          const = 0;   // "binance", "athex-eod"
    virtual std::string DisplayName() const = 0;   // "Crypto (Binance)"
    virtual std::string AssetClass()  const = 0;   // "Crypto", "Greek Stocks"

    // --- Auth ---------------------------------------------------------
    virtual bool        NeedsApiKey()             const { return false; }
    virtual void        SetApiKey(const std::string&)   {}
    virtual std::string GetApiKeyHelpUrl()        const { return ""; }

    // Empty string = success. Non-empty = human-readable error, shown in UI.
    virtual std::string TestConnection() = 0;

    // --- Capabilities ------------------------------------------------
    virtual ProviderCapabilities Capabilities() const = 0;

    // --- Data ---------------------------------------------------------
    // Returns the provider's default starter list (small, always available).
    virtual std::vector<std::string> DefaultTickers() const = 0;

    // Search or enumerate symbols. If !supportsSymbolSearch, the provider
    // returns its DefaultTickers filtered by `query` (substring, case-insensitive).
    // Otherwise it hits the upstream symbol-list endpoint. Empty query = list all.
    virtual std::vector<std::string> SearchSymbols(const std::string& query,
                                                     int limit = 100) = 0;

    // Latest quote for one ticker.
    virtual bool FetchQuote(const std::string& ticker, Quote& out) = 0;

    // OHLCV history. endTimeMs=0 means "up to now". limit is capped by
    // Capabilities().maxBarsPerRequest internally. Bars must be oldest-first.
    virtual bool FetchOHLCV(const std::string& ticker,
                             const std::string& timeframeId,
                             int64_t endTimeMs,
                             int     limit,
                             std::vector<OHLCVBar>& out) = 0;

    // Lifetime: the DLL owns `this`, host must call Destroy() instead of delete.
    virtual void Destroy() { delete this; }
};

// ---------------------------------------------------------------------------
// Plugin ABI. A plugin DLL must export exactly these three symbols:
//
//   extern "C" __declspec(dllexport) const char* StockViewer_PluginApiVersion();
//       -> must return the string "1"
//   extern "C" __declspec(dllexport) IMarketDataProvider* StockViewer_CreateProvider();
//       -> returns a NEW provider instance owned by the caller
//   extern "C" __declspec(dllexport) const char* StockViewer_PluginName();
//       -> human-readable plugin name for logs
// ---------------------------------------------------------------------------
#define STOCKVIEWER_PLUGIN_API_VERSION "1"