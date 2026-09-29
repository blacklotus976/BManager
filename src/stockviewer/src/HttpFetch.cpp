#include "HttpFetch.h"
#include <windows.h>
#include <winhttp.h>
#include <chrono>

#pragma comment(lib, "winhttp.lib")

bool HttpGetJson(const std::wstring& host, const std::wstring& path,
                 std::string& outBody, HttpMetrics* outMetrics) {
    outBody.clear();
    auto t0 = std::chrono::steady_clock::now();
    if (outMetrics) *outMetrics = HttpMetrics{};

    HINTERNET hSession = WinHttpOpen(L"StockViewer/1.0",
                                      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 5000);

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
                                             nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    bool ok = false;
    DWORD statusCode = 0;
    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        if (WinHttpReceiveResponse(hRequest, nullptr)) {
            DWORD statusSize = sizeof(statusCode);
            WinHttpQueryHeaders(hRequest,
                                 WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize,
                                 WINHTTP_NO_HEADER_INDEX);

            DWORD bytesAvailable = 0;
            do {
                bytesAvailable = 0;
                if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) break;
                if (bytesAvailable == 0) break;
                std::string chunk(bytesAvailable, '\0');
                DWORD bytesRead = 0;
                if (!WinHttpReadData(hRequest, &chunk[0], bytesAvailable, &bytesRead)) break;
                chunk.resize(bytesRead);
                outBody += chunk;
            } while (bytesAvailable > 0);

            ok = (statusCode >= 200 && statusCode < 300) && !outBody.empty();
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (outMetrics) {
        auto t1 = std::chrono::steady_clock::now();
        outMetrics->latencyMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        outMetrics->bytes = (int64_t)outBody.size();
        outMetrics->statusCode = (int)statusCode;
        outMetrics->ok = ok;
    }
    return ok;
}

bool HttpGet(const std::string& url, std::string& outBody, HttpMetrics* outMetrics) {
    size_t slash = url.find('/');
    if (slash == std::string::npos) return false;
    std::string host = url.substr(0, slash);
    std::string path = url.substr(slash);
    return HttpGetJson(std::wstring(host.begin(), host.end()),
                        std::wstring(path.begin(), path.end()),
                        outBody, outMetrics);
}