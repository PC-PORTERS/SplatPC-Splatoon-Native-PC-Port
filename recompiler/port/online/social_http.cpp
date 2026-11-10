#include "social_http.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include <vector>

namespace rbrew_native {
namespace {

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n);
    return out;
}

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
};

class WinHttpTransport final : public Transport {
public:
    WinHttpTransport(std::wstring host, INTERNET_PORT port, bool secure, std::wstring prefix, int timeout_ms)
        : host_(std::move(host)), port_(port), secure_(secure), prefix_(std::move(prefix)) {
        session_.h = WinHttpOpen(L"SplatoonPort-Friends/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_.h) return;
        WinHttpSetTimeouts(session_.h, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
        // The password and the session token only go to the address the launcher gave: no redirect is followed.
        DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(session_.h, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof never);
    }

    Reply post(const std::string& route, const nlohmann::json& body) override {
        Reply reply;
        if (!session_.h) return reply;
        Handle connection{WinHttpConnect(session_.h, host_.c_str(), port_, 0)};
        if (!connection.h) return reply;
        const std::wstring path = prefix_ + L"/social/" + widen(route);
        Handle request{WinHttpOpenRequest(connection.h, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, secure_ ? WINHTTP_FLAG_SECURE : 0)};
        if (!request.h) return reply;
        std::string payload = body.dump();
        static const wchar_t kHeaders[] = L"Content-Type: application/json\r\n";
        if (!WinHttpSendRequest(request.h, kHeaders, DWORD(-1L), payload.data(), DWORD(payload.size()),
                                DWORD(payload.size()), 0) ||
            !WinHttpReceiveResponse(request.h, nullptr))
            return reply;
        DWORD status = 0, size = sizeof status;
        if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
            return reply;
        std::string text;
        for (DWORD available = 0; WinHttpQueryDataAvailable(request.h, &available) && available;) {
            std::vector<char> chunk(available);
            DWORD read = 0;
            if (!WinHttpReadData(request.h, chunk.data(), available, &read) || !read) break;
            text.append(chunk.data(), read);
            if (text.size() > (1u << 20)) return reply; // no answer of the server is this large
        }
        const bool ok = status >= 200 && status < 300;
        nlohmann::json parsed = nlohmann::json::parse(text.empty() ? std::string("{}") : text, nullptr, false);
        if (parsed.is_discarded()) {
            if (ok) return reply; // as urllib's ValueError in friends_relay.py: treated as unreachable
            parsed = nlohmann::json::object();
        }
        reply.status = int(status);
        reply.body = parsed.is_object() ? parsed : nlohmann::json::object();
        return reply;
    }

private:
    Handle session_;
    std::wstring host_;
    INTERNET_PORT port_;
    bool secure_;
    std::wstring prefix_;
};

} // namespace

std::unique_ptr<Transport> make_winhttp_transport(const std::string& base_url, int timeout_ms) {
    std::wstring url = widen(base_url);
    while (!url.empty() && url.back() == L'/') url.pop_back();
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    wchar_t host[256], path[1024];
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(url.c_str(), DWORD(url.size()), 0, &parts)) return nullptr;
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    if (!secure && parts.nScheme != INTERNET_SCHEME_HTTP) return nullptr;
    return std::make_unique<WinHttpTransport>(std::wstring(host, parts.dwHostNameLength), parts.nPort, secure,
                                              std::wstring(path, parts.dwUrlPathLength), timeout_ms);
}

} // namespace rbrew_native
