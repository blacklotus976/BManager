#include "HttpFetch.h"
#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

bool HttpGetJson(const std::wstring& host, const std::wstring& path, std::string& outBody) {
    outBody.clear();

    HINTERNET hSession = WinHttpOpen(L"StockViewer/1.0",
                                      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    // Reasonable timeouts so a dead/slow endpoint doesn't hang the UI thread
    // forever -- this is a synchronous call made from a refresh button click.
    WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 5000);

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
                                             nullptr, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    bool ok = false;
    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        if (WinHttpReceiveResponse(hRequest, nullptr)) {
            // Check HTTP status; Yahoo returns e.g. 404 for an unknown ticker.
            DWORD statusCode = 0;
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

            // Treat any 2xx as success; Yahoo sometimes still returns a JSON
            // error payload with 200, which the caller's JSON parsing will
            // reject on its own.
            ok = (statusCode >= 200 && statusCode < 300) && !outBody.empty();
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
}
