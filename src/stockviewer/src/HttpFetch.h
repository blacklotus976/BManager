#pragma once
#include <string>
#include <cstdint>

struct HttpMetrics {
    int64_t latencyMs   = 0;
    int64_t bytes       = 0;
    int     statusCode  = 0;
    bool    ok          = false;
};

// Low-level: HTTPS GET to https://host/path. If `outMetrics` is non-null,
// fills it with timing/size/status. Returns false on any failure.
bool HttpGetJson(const std::wstring& host, const std::wstring& path,
                 std::string& outBody, HttpMetrics* outMetrics = nullptr);

// Convenience: "host/path" form.
bool HttpGet(const std::string& url, std::string& outBody,
             HttpMetrics* outMetrics = nullptr);