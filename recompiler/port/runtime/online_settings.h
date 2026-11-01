// online_settings.h - read-only access to settings.ini (next to the
// executable) for the keys of the online mode: online_server= and pseudo=.
//
// Precedence: environment variable > settings.ini > default. The ESC menu
// (port_settings.cpp) owns the file and preserves keys it does not know, so
// these lines survive its saves. Header-only, no SDL.
#pragma once
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#if defined(_WIN32)
// Same declaration as <windows.h>, so this header does not pull it in.
extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameW(struct HINSTANCE__*, wchar_t*, unsigned long);
#endif

namespace rbrew_online_cfg {

using Values = std::map<std::string, std::string>;

inline std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}
// Plain `key=value` lines; '#' starts a comment.
inline Values parse(std::istream& in) {
    Values values;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq));
        if (!key.empty()) values[key] = trim(line.substr(eq + 1));
    }
    return values;
}
inline std::filesystem::path settings_path() {
#if defined(_WIN32)
    wchar_t buf[1024];
    const unsigned long n = GetModuleFileNameW(nullptr, buf, 1024);
    if (n && n < 1024) return std::filesystem::path(buf).parent_path() / L"settings.ini";
#endif
    return "settings.ini";
}
// Environment first, then settings.ini, then `fallback`. Read on each call.
inline std::string resolved(const char* env, const std::string& key, const std::string& fallback) {
    if (env && *env) if (const char* v = std::getenv(env); v && *v) return v;
    std::ifstream f(settings_path());
    if (f) {
        const Values file = parse(f);
        const auto it = file.find(key);
        if (it != file.end() && !it->second.empty()) return it->second;
    }
    return fallback;
}

}  // namespace rbrew_online_cfg
