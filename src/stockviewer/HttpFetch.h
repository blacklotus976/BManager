// Minimal WinHTTP-based HTTPS GET helper for StockViewer.
// No extra dependency beyond what ships with Windows.
#pragma once
#include <string>

// Performs an HTTPS GET to https://host/path and returns the response body.
// Returns false (without throwing/crashing) on any failure -- offline, DNS
// failure, TLS failure, non-2xx status, timeout, etc. Callers must treat a
// false return as "this stock's data is unavailable right now", not a fatal
// error: network calls to a free third-party endpoint fail routinely.
bool HttpGetJson(const std::wstring& host, const std::wstring& path, std::string& outBody);
