// social_http.h - JSON HTTP client for /social/<route>, /health and /register
// (WinHTTP on Windows, libcurl on Linux/Unix; redirects are never followed).
#pragma once
#include <nlohmann/json.hpp>
#include <memory>
#include <string>

namespace rbrew_native {

struct Reply {
    int status = 0; // 0: unreachable, or a 2xx answer that is not JSON (as friends_relay.py's call())
    nlohmann::json body = nlohmann::json::object();
};

class Transport {
public:
    virtual ~Transport() = default;
    virtual Reply post(const std::string& route, const nlohmann::json& body) = 0;
    virtual Reply request(const std::string& /*method*/, const std::string& /*abs_path*/,
                          const nlohmann::json* /*body*/) {
        return {};
    }
};

// Splits "https://host[:port]/prefix/register" into base ("https://host[:port]") and path ("/prefix/register").
bool split_url(const std::string& url, std::string& base_url, std::string& path);

// base_url: "https://host[:port]" or "http://127.0.0.1[:port]"; nullptr when it cannot be used.
std::unique_ptr<Transport> make_http_transport(const std::string& base_url, int timeout_ms);
#if defined(_WIN32)
std::unique_ptr<Transport> make_winhttp_transport(const std::string& base_url, int timeout_ms);
#endif

} // namespace rbrew_native
