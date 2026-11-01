#pragma once
// Online mode: local identity, pseudo and NEX service token (testable).
//
// Off by default. online_server=ip:port in settings.ini (RBREW_ONLINE_SERVER
// wins) turns it on: the account becomes a network account whose NEX token
// points at that server. No console or Nintendo identity is used: the first
// online launch creates online_identity.ini next to the executable with a
// random principal id and NEX password, which identify the player (the
// pseudo is only a display name, also used as the Mii name in game).
#include "online_settings.h"
#include <algorithm>
#include <charconv>
#include <stdexcept>
#include <limits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>

// Shared network trace (nsysnet.cpp, RBREW_NET_TRACE).
void rbrew_net_note(const char* what, const char* detail);

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#ifdef min
#undef min
#undef max
#endif
#pragma comment(lib,"bcrypt.lib")
#endif
namespace rbrew_online {

struct Identity {
    uint32_t pid = 0;          // NEX principal id
    std::string password;      // NEX password (Kerberos key material)
};

constexpr uint32_t kNexTokenSize = 0x25C;   // ACTNexAuthenticationResult (Cemu; Gambit 02BFC660)
constexpr uint32_t kTokenOffset = 0x000, kTokenMax = 0x200, kPasswordOffset = 0x204, kPasswordMax = 0x40,
                   kHostOffset = 0x248, kHostMax = 0x0F, kPortOffset = 0x258;

inline bool valid(const Identity& id) {
    if (!id.pid || id.pid > 0x7FFFFFFFu || id.password.size() != 16) return false;
    for (char c : id.password) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return false;
    return true;
}
inline std::optional<Identity> parse(std::istream& in) {
    Identity id;
    for (const auto& [k, v] : rbrew_online_cfg::parse(in)) {
        if (k == "pid") {
            uint32_t n=0; const auto result=std::from_chars(v.data(),v.data()+v.size(),n,16);
            if (!v.empty() && result.ec==std::errc{} && result.ptr==v.data()+v.size()) id.pid=n;
        }
        else if (k == "password") id.password = v;
    }
    return valid(id) ? std::optional<Identity>(id) : std::nullopt;
}
template <class Rng> Identity create(Rng& rng) {
    static constexpr char kAlphabet[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    Identity id;
    id.pid = 0x10000000u + uint32_t(rng() % 0x6FFFFFFFu);   // non-zero, positive as int32
    for (int i = 0; i < 16; ++i) id.password += kAlphabet[rng() % 62];
    return id;
}
inline std::string serialize(const Identity& id) {
    char pid[16]; std::snprintf(pid, sizeof pid, "%08x", id.pid);
    return "# Splatoon PC online identity. Keep this file: it identifies you to friends and rooms.\n"
           "pid=" + std::string(pid) + "\npassword=" + id.password + "\n";
}
// Use the OS CSPRNG directly; an MT engine is not suitable for account secrets.
struct SecureRandom {
    using result_type=uint64_t;
    static constexpr result_type min(){return 0;}
    static constexpr result_type max(){return UINT64_MAX;}
    result_type operator()() {
        result_type value=0;
#if defined(_WIN32)
        if (BCryptGenRandom(nullptr,reinterpret_cast<unsigned char*>(&value),sizeof(value),2)<0)
            throw std::runtime_error("OS random generator failed");
#else
        std::random_device device;
        value=(uint64_t(device())<<32)^device();
#endif
        return value;
    }
};
// Existing invalid/unreadable files are preserved. Failure never returns an
// unpersisted identity, which would change the account on the next launch.
inline Identity load_or_create(const std::filesystem::path& file) {
    std::error_code ec;
    if(std::filesystem::exists(file,ec)) {
        std::ifstream in(file);
        if(in)if(auto id=parse(in))return *id;
        throw std::runtime_error("Online identity is unreadable or corrupt");
    }
    if(ec)throw std::runtime_error("Cannot inspect online identity");
    SecureRandom rng;
    const Identity id=create(rng);
    char suffix[48];std::snprintf(suffix,sizeof suffix,".tmp.%016llx",(unsigned long long)rng());
    auto tmp=file;tmp+=suffix;
    struct Cleanup { std::filesystem::path path; ~Cleanup(){std::error_code ignored;std::filesystem::remove(path,ignored);} } cleanup{tmp};
    { std::ofstream out(tmp,std::ios::binary|std::ios::trunc);
      if(!out || !(out<<serialize(id)) || !out.flush())throw std::runtime_error("Cannot persist online identity"); out.close();if(!out)throw std::runtime_error("Cannot close online identity"); }
#if defined(_WIN32)
    const bool published=MoveFileExW(tmp.c_str(),file.c_str(),8)!=0; // no REPLACE_EXISTING: first creator wins
#else
    std::filesystem::create_hard_link(tmp,file,ec);const bool published=!ec;
#endif
    if(published)return id;
    // Another process may have won the atomic publication race.
    std::ifstream existing(file);
    if(existing)if(auto winner=parse(existing))return *winner;
    throw std::runtime_error("Cannot publish online identity");
}
// "a.b.c.d:port" only (no names: DNS is confined).
inline bool parse_server(const std::string& text, std::string& host, uint16_t& port) {
    const size_t colon=text.rfind(':');
    if(colon==std::string::npos || !colon || colon>kHostMax || colon+1==text.size())return false;
    size_t start=0;
    for(unsigned part=0;part<4;++part) {
        const size_t end=part==3?colon:text.find('.',start);
        if(end==std::string::npos || end>=colon+(part==3) || end==start)return false;
        unsigned n=0;const auto r=std::from_chars(text.data()+start,text.data()+end,n);
        if(r.ec!=std::errc{} || r.ptr!=text.data()+end || n>255)return false;
        start=end+1;
    }
    unsigned p=0;const auto r=std::from_chars(text.data()+colon+1,text.data()+text.size(),p);
    if(r.ec!=std::errc{} || r.ptr!=text.data()+text.size() || p==0 || p>65535)return false;
    host=text.substr(0,colon);port=uint16_t(p);return true;
}
// Pseudo (UTF-8) -> Mii name: at most 10 UTF-16 units, control characters dropped.
inline std::u16string mii_name(const std::string& utf8, std::u16string_view fallback) {
    std::u16string out;
    for(size_t i=0;i<utf8.size() && out.size()<10;) {
        const uint8_t first=uint8_t(utf8[i]);uint32_t cp=0;size_t n=0;
        if(first<0x80){cp=first;n=1;}
        else if(first>=0xC2 && first<=0xDF){cp=first&31;n=2;}
        else if(first>=0xE0 && first<=0xEF){cp=first&15;n=3;}
        else if(first>=0xF0 && first<=0xF4){cp=first&7;n=4;}
        else {++i;continue;}
        bool good=i+n<=utf8.size();
        for(size_t k=1;good && k<n;++k){const uint8_t c=uint8_t(utf8[i+k]);good=(c&0xC0)==0x80;cp=(cp<<6)|(c&63);}
        if(!good){++i;continue;}
        i+=n;
        if((n==2 && cp<0x80)||(n==3 && cp<0x800)||(n==4 && cp<0x10000))continue;
        // Also dropped: invisible and direction-changing characters (zero-width, bidi marks and overrides,
        // line/paragraph separators, byte-order mark), which let a pseudo read backwards or look empty.
        const bool invisible=(cp>=0x200B && cp<=0x200F)||(cp>=0x2028 && cp<=0x202E)||(cp>=0x2066 && cp<=0x2069)||cp==0xFEFF;
        if(cp>=0x20 && !(cp>=0x7F && cp<0xA0) && cp<=0xFFFF && !(cp>=0xD800 && cp<=0xDFFF) && !invisible)out+=char16_t(cp);
    }
    while(!out.empty() && out.back()==u' ')out.pop_back();
    if(out.empty()) {
        for(char16_t c:fallback)if(out.size()<10 && c>=0x20 && c!=0x7F && !(c>=0xD800 && c<=0xDFFF))out+=c;
        while(!out.empty() && out.back()==u' ')out.pop_back();
    }
    return out;
}
// Writes ACTNexAuthenticationResult into `out` (kNexTokenSize bytes): every
// byte written, padding zero, strings truncated and NUL-terminated, port big-endian.
inline void fill_nex_token(uint8_t* out, std::string_view token, std::string_view password, std::string_view host, uint16_t port) {
    std::memset(out, 0, kNexTokenSize);
    std::memcpy(out + kTokenOffset, token.data(), std::min<size_t>(token.size(), kTokenMax));
    std::memcpy(out + kPasswordOffset, password.data(), std::min<size_t>(password.size(), kPasswordMax));
    std::memcpy(out + kHostOffset, host.data(), std::min<size_t>(host.size(), kHostMax));
    out[kPortOffset] = uint8_t(port >> 8); out[kPortOffset + 1] = uint8_t(port);
}

// Current online configuration (read on each call; settings change rarely).
struct Config { bool online = false; std::string host; uint16_t port = 0; };
inline const Identity& identity();
inline Config config() {
    Config c;
    const std::string server = rbrew_online_cfg::resolved("RBREW_ONLINE_SERVER", "online_server", "");
    c.online = !server.empty() && parse_server(server,c.host,c.port);
    if(c.online && !valid(identity()))c.online=false; // failed persistence stays offline, with no invented account
    return c;
}
inline std::string pseudo() { return rbrew_online_cfg::resolved("RBREW_PSEUDO", "pseudo", ""); }
inline const Identity& identity() {
    static const Identity id = [] {
        try { return load_or_create(rbrew_online_cfg::settings_path().parent_path()/"online_identity.ini"); }
        catch(const std::exception& e){rbrew_net_note("identity unavailable",e.what());return Identity{};}
    }();
    return id;
}

}  // namespace rbrew_online

