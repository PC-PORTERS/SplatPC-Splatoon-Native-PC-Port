// social_http.h - JSON POSTs to the server's /social/<route> endpoints (WinHTTP, Windows' own HTTP stack).
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
};

// base_url: "https://host[:port]" or "http://127.0.0.1:port"; nullptr when it cannot be used.
std::unique_ptr<Transport> make_winhttp_transport(const std::string& base_url, int timeout_ms);

} // namespace rbrew_native
