#include "social_http.h"

#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#endif

namespace rbrew_native {

bool split_url(const std::string& url, std::string& base_url, std::string& path) {
    const size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return false;
    const std::string scheme = url.substr(0, scheme_end);
    if (scheme != "https" && scheme != "http") return false;
    const size_t authority_start = scheme_end + 3;
    const size_t slash = url.find('/', authority_start);
    if (slash == authority_start || (slash == std::string::npos && authority_start >= url.size())) return false;
    if (slash == std::string::npos) {
        base_url = url;
        path = "/";
    } else {
        base_url = url.substr(0, slash);
        path = url.substr(slash);
    }
    return true;
}

namespace {

Reply parse_json_reply(int status, const std::string& text) {
    Reply reply;
    const bool ok = status >= 200 && status < 300;
    nlohmann::json parsed = nlohmann::json::parse(text.empty() ? std::string("{}") : text, nullptr, false);
    if (parsed.is_discarded()) {
        if (ok) return reply; // treated as unreachable
        parsed = nlohmann::json::object();
    }
    reply.status = status;
    reply.body = parsed.is_object() ? parsed : nlohmann::json::object();
    return reply;
}

#if defined(_WIN32)

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
        // The password and the session token only go to the address configured: no redirect is followed.
        DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(session_.h, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof never);
    }

    Reply post(const std::string& route, const nlohmann::json& body) override {
        return send("POST", prefix_ + L"/social/" + widen(route), &body);
    }

    Reply request(const std::string& method, const std::string& abs_path, const nlohmann::json* body) override {
        std::string normalized = abs_path.empty() || abs_path[0] != '/' ? "/" + abs_path : abs_path;
        return send(method, widen(normalized), body);
    }

private:
    Reply send(const std::string& method, const std::wstring& full_path, const nlohmann::json* body) {
        Reply reply;
        if (!session_.h) return reply;
        Handle connection{WinHttpConnect(session_.h, host_.c_str(), port_, 0)};
        if (!connection.h) return reply;
        const std::wstring wmethod = widen(method);
        Handle req{WinHttpOpenRequest(connection.h, wmethod.c_str(), full_path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, secure_ ? WINHTTP_FLAG_SECURE : 0)};
        if (!req.h) return reply;
        std::string payload = body ? body->dump() : std::string();
        const wchar_t* headers = body ? L"Content-Type: application/json\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS;
        const DWORD header_len = body ? DWORD(-1L) : 0;
        if (!WinHttpSendRequest(req.h, headers, header_len, body ? payload.data() : WINHTTP_NO_REQUEST_DATA,
                                DWORD(payload.size()), DWORD(payload.size()), 0) ||
            !WinHttpReceiveResponse(req.h, nullptr))
            return reply;
        DWORD status = 0, size = sizeof status;
        if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
            return reply;
        std::string text;
        for (DWORD available = 0; WinHttpQueryDataAvailable(req.h, &available) && available;) {
            std::vector<char> chunk(available);
            DWORD read = 0;
            if (!WinHttpReadData(req.h, chunk.data(), available, &read) || !read) break;
            text.append(chunk.data(), read);
            if (text.size() > (1u << 20)) return reply; // no answer of the server is this large
        }
        return parse_json_reply(int(status), text);
    }

    Handle session_;
    std::wstring host_;
    INTERNET_PORT port_;
    bool secure_;
    std::wstring prefix_;
};

#else

size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t total = size * nmemb;
    auto* out = static_cast<std::string*>(userdata);
    if (out->size() + total > (1u << 20)) return 0;
    out->append(ptr, total);
    return total;
}

class CurlTransport final : public Transport {
public:
    CurlTransport(std::string base_url, int timeout_ms) : base_(std::move(base_url)), timeout_ms_(timeout_ms) {
        while (!base_.empty() && base_.back() == '/') base_.pop_back();
        std::string root, path;
        if (split_url(base_, root, path)) {
            origin_ = root;
            prefix_ = path == "/" ? std::string() : path;
        }
    }

    Reply post(const std::string& route, const nlohmann::json& body) override {
        return send("POST", origin_ + prefix_ + "/social/" + route, &body);
    }

    Reply request(const std::string& method, const std::string& abs_path, const nlohmann::json* body) override {
        std::string normalized = abs_path.empty() || abs_path[0] != '/' ? "/" + abs_path : abs_path;
        return send(method, origin_ + normalized, body);
    }

private:
    Reply send(const std::string& method, const std::string& url, const nlohmann::json* body) {
        Reply reply;
        if (origin_.empty()) return reply;
        CURL* curl = curl_easy_init();
        if (!curl) return reply;
        struct curl_slist* headers = nullptr;
        std::string response;
        std::string payload;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, long(timeout_ms_));
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, long(timeout_ms_));
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "SplatoonPort-Friends/1");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        if (method == "POST") {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            if (body) {
                payload = body->dump();
                headers = curl_slist_append(headers, "Content-Type: application/json");
                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.data());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, long(payload.size()));
            } else {
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
            }
        } else if (method != "GET") {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        }
        const CURLcode res = curl_easy_perform(curl);
        long status = 0;
        if (res == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (headers) curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        if (res != CURLE_OK || status <= 0) return reply;
        return parse_json_reply(int(status), response);
    }

    std::string base_, origin_, prefix_;
    int timeout_ms_;
};

#endif

} // namespace

std::unique_ptr<Transport> make_http_transport(const std::string& base_url, int timeout_ms) {
#if defined(_WIN32)
    return make_winhttp_transport(base_url, timeout_ms);
#else
    std::string root, path;
    if (!split_url(base_url, root, path)) return nullptr;
    return std::make_unique<CurlTransport>(base_url, timeout_ms);
#endif
}

#if defined(_WIN32)
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
#endif

} // namespace rbrew_native
