// online_settings.h - read/write access to settings.ini (next to the
// executable) for the keys of the online mode: online_server=,
// online_nncs1=, online_nncs2=, online_register=, pseudo=, and player_name=.
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
#include <vector>

#if defined(_WIN32)
// Same declaration as <windows.h>, so this header does not pull it in.
extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameW(struct HINSTANCE__*, wchar_t*,
                                                                            unsigned long);
extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentProcessId();
#else
#include <unistd.h>
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
#else
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        const auto exe_settings = std::filesystem::path(buf).parent_path() / "settings.ini";
        std::error_code ec;
        if (std::filesystem::exists(exe_settings, ec)) return exe_settings;
        if (std::filesystem::exists("settings.ini", ec)) return "settings.ini";
        return exe_settings;
    }
#endif
    return "settings.ini";
}
// Environment first, then settings.ini, then `fallback`. Read on each call.
inline std::string resolved(const char* env, const std::string& key, const std::string& fallback) {
    if (env && *env)
        if (const char* v = std::getenv(env); v && *v) return v;
    std::ifstream f(settings_path());
    if (f) {
        const Values file = parse(f);
        const auto it = file.find(key);
        if (it != file.end() && !it->second.empty()) return it->second;
    }
    return fallback;
}
// Updates or appends specific keys in settings.ini while preserving all other lines and comments.
inline bool update_settings(const Values& updates, const std::filesystem::path& path = settings_path()) {
    std::vector<std::string> lines;
    std::map<std::string, bool> seen;
    {
        std::ifstream in(path, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const std::string t = trim(line);
            const size_t eq = t.find('=');
            if (!t.empty() && t[0] != '#' && eq != std::string::npos) {
                const std::string key = trim(t.substr(0, eq));
                const auto it = updates.find(key);
                if (it != updates.end()) {
                    lines.push_back(key + "=" + it->second);
                    seen[key] = true;
                    continue;
                }
            }
            lines.push_back(line);
        }
    }
    for (const auto& [k, v] : updates) {
        if (!seen[k]) lines.push_back(k + "=" + v);
    }
    std::error_code ec;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path tmp = path;
#if defined(_WIN32)
    tmp += "." + std::to_string(GetCurrentProcessId()) + ".tmp";
#else
    tmp += "." + std::to_string(getpid()) + ".tmp";
#endif
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        for (const auto& l : lines) out << l << '\n';
        if (!out.flush()) return false;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace rbrew_online_cfg
