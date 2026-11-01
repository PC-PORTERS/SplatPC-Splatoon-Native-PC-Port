// nn::fp friend list for the local two-game lab.
//
// Private Battle is joined through the friend list. Splatoon's friend manager
// (singleton 101E5668, refresh 027A0A5C) calls GetFriendList, then
// AccountId / ScreenName / Mii / Profile / ApprovalTime / Presence for the
// whole list, and publishes its own state with UpdateGameMode (027A0C68).
// It asks for a new refresh by itself (0279FE78), so no notification is sent.
// All of these were empty PLT stubs: no friends.
//
// With RBREW_FRIENDS_DIR set (Launch-Duo.ps1) and online mode on, every game
// writes <pid>.txt in that folder (pid, pseudo, last GameMode), and every
// other file there is a friend who is online, with that GameMode as presence.
// Layouts read from the copy at 027A0120: FriendPresence is 0x34 bytes,
// GameMode the first 0x2C, +0x2C goes through the 0..7 switch at 0279FF54,
// +0x30 and +0x31 are flags (valid, online). Profile 4 bytes, DateTime 8,
// account id char[17], screen name wchar[11], Mii FFLStoreData (0x60).
// Lab only: a real friends server replaces this.

#include "../os_common.h"
#include "stubs_addrs.h"
#include "../nn_act/nn_act.h"
#include "../../runtime/online_identity.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

namespace {
namespace fs = std::filesystem;
constexpr uint32_t kGameModeSize = 0x2C;
constexpr uint32_t kPresenceSize = 0x34;

struct Friend { uint32_t pid = 0; std::string name; std::vector<uint8_t> mode; };

fs::path s_dir;
std::vector<uint8_t> s_mode(kGameModeSize, 0);

bool guest_ok(const CPUState* cpu, uint32_t address, uint64_t length) {
    return address && uint64_t(address) + length <= cpu->guest_memory_size &&
           rbrew_memory_accessible(cpu->mem, address, length);
}

void publish() {
    char name[16];
    std::snprintf(name, sizeof name, "%08x", rbrew_online::identity().pid);
    std::string hex;
    for (uint8_t b : s_mode) { char h[3]; std::snprintf(h, sizeof h, "%02x", b); hex += h; }
    const fs::path tmp = s_dir / (std::string(name) + ".tmp");
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << "pid=" << name << "\nname=" << rbrew_online::pseudo() << "\nmode=" << hex << "\n";
    }
    std::error_code ec;
    fs::rename(tmp, s_dir / (std::string(name) + ".txt"), ec);
}

std::map<uint32_t, Friend> friends() {
    std::map<uint32_t, Friend> result;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(s_dir, ec)) {
        if (entry.path().extension() != ".txt") continue;
        std::ifstream in(entry.path());
        Friend f;
        for (std::string line; std::getline(in, line);) {
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
            if (key == "pid") f.pid = uint32_t(std::strtoul(value.c_str(), nullptr, 16));
            else if (key == "name") f.name = value;
            else if (key == "mode" && value.size() == kGameModeSize * 2)
                for (uint32_t i = 0; i < kGameModeSize; ++i)
                    f.mode.push_back(uint8_t(std::strtoul(value.substr(i * 2, 2).c_str(), nullptr, 16)));
        }
        if (f.pid && f.pid != rbrew_online::identity().pid) result[f.pid] = f;
    }
    return result;
}

// Fills count entries of size bytes at out, one per pid in pids, with fill(entry, friend).
template <typename Fill>
void for_each_friend(CPUState* cpu, uint32_t out, uint32_t pids, uint32_t count, uint32_t size, Fill fill) {
    if (count > 100 || !guest_ok(cpu, out, uint64_t(size) * count) || !guest_ok(cpu, pids, 4ull * count)) { RET = 0; return; }
    const auto all = friends();
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t entry = out + i * size;
        for (uint32_t b = 0; b < size; ++b) rbrew_write8(MEM, entry + b, 0);
        const auto it = all.find(rbrew_read32(MEM, pids + 4 * i));
        if (it != all.end()) fill(entry, it->second);
    }
    RET = 0;
}

std::u16string display_name(const Friend& f) { return rbrew_online::mii_name(f.name, u"Ami"); }

// GetFriendList(u32* pids, u32* count, u32 offset, u32 max)
void GetFriendList(CPUState* cpu) {
    const uint32_t out = ARG0, count_out = ARG1, offset = ARG2, max = ARG3;
    uint32_t n = 0, index = 0;
    if (guest_ok(cpu, out, 4ull * max))
        for (const auto& [pid, f] : friends())
            if (index++ >= offset && n < max) rbrew_write32(MEM, out + 4 * n++, pid);
    if (guest_ok(cpu, count_out, 4)) rbrew_write32(MEM, count_out, n);
    RET = 0;
}
void GetFriendAccountId(CPUState* cpu) {
    for_each_friend(cpu, ARG0, ARG1, ARG2, 17, [&](uint32_t e, const Friend& f) {
        char id[17]; std::snprintf(id, sizeof id, "rb%08x", f.pid);
        for (uint32_t i = 0; id[i]; ++i) rbrew_write8(MEM, e + i, uint8_t(id[i]));
    });
}
void GetFriendScreenName(CPUState* cpu) {
    for_each_friend(cpu, ARG0, ARG1, ARG2, 22, [&](uint32_t e, const Friend& f) {
        const auto name = display_name(f);
        for (uint32_t i = 0; i < name.size() && i < 10; ++i) rbrew_write16(MEM, e + 2 * i, uint16_t(name[i]));
    });
}
void GetFriendMii(CPUState* cpu) {
    for_each_friend(cpu, ARG0, ARG1, ARG2, 0x60, [&](uint32_t e, const Friend& f) {
        const auto mii = rbrew_act_default_mii(display_name(f));
        for (uint32_t i = 0; i < mii.size(); ++i) rbrew_write8(MEM, e + i, mii[i]);
    });
}
void GetFriendProfile(CPUState* cpu) { for_each_friend(cpu, ARG0, ARG1, ARG2, 4, [](uint32_t, const Friend&) {}); }
void GetFriendApprovalTime(CPUState* cpu) { for_each_friend(cpu, ARG0, ARG1, ARG2, 8, [](uint32_t, const Friend&) {}); }
void GetFriendPresence(CPUState* cpu) {
    for_each_friend(cpu, ARG0, ARG1, ARG2, kPresenceSize, [&](uint32_t e, const Friend& f) {
        for (uint32_t i = 0; i < f.mode.size(); ++i) rbrew_write8(MEM, e + i, f.mode[i]);
        rbrew_write8(MEM, e + 0x2C, 1);
        rbrew_write8(MEM, e + 0x30, 1);
        rbrew_write8(MEM, e + 0x31, 1);
    });
}
// UpdateGameMode(const GameMode* mode, const wchar_t* message, u32)
void UpdateGameMode(CPUState* cpu) {
    if (guest_ok(cpu, ARG0, kGameModeSize)) {
        for (uint32_t i = 0; i < kGameModeSize; ++i) s_mode[i] = rbrew_read8(MEM, ARG0 + i);
        std::fprintf(stderr, "[fp_lab] UpdateGameMode flags=%08x type=%08x game=%08x mode=%08x host=%08x group=%08x\n",
                     rbrew_read32(MEM, ARG0), rbrew_read32(MEM, ARG0 + 4), rbrew_read32(MEM, ARG0 + 8),
                     rbrew_read32(MEM, ARG0 + 12), rbrew_read32(MEM, ARG0 + 16), rbrew_read32(MEM, ARG0 + 20));
        publish();
    }
    RET = 0;
}
}

void fp_lab_register(CPUState* cpu) {
    const char* dir = std::getenv("RBREW_FRIENDS_DIR");
    if (!dir || !*dir || !rbrew_online::config().online) return;
    s_dir = dir;
    std::error_code ec;
    fs::create_directories(s_dir, ec);
    publish();
    rbrew_register_func(cpu, ADDR_GetFriendList__Q2_2nn2fpFPUiT1UiT3, GetFriendList);
    rbrew_register_func(cpu, ADDR_GetFriendAccountId__Q2_2nn2fpFPA17_cPCUiUi, GetFriendAccountId);
    rbrew_register_func(cpu, ADDR_GetFriendScreenName__Q2_2nn2fpFPA11_wPCUiUibPUc, GetFriendScreenName);
    rbrew_register_func(cpu, ADDR_GetFriendMii__Q2_2nn2fpFP12FFLStoreDataPCUiUi, GetFriendMii);
    rbrew_register_func(cpu, ADDR_GetFriendProfile__Q2_2nn2fpFPQ3_2nn2fp7ProfilePCUiUi, GetFriendProfile);
    rbrew_register_func(cpu, ADDR_GetFriendApprovalTime__Q2_2nn2fpFPQ3_2nn2fp8DateTimePCUiUi, GetFriendApprovalTime);
    rbrew_register_func(cpu, ADDR_GetFriendPresence__Q2_2nn2fpFPQ3_2nn2fp14FriendPresencePCUiUi, GetFriendPresence);
    rbrew_register_func(cpu, ADDR_UpdateGameMode__Q2_2nn2fpFPCQ3_2nn2fp8GameModePCwUi, UpdateGameMode);
    std::fprintf(stderr, "[fp_lab] friends folder %s, pid %08x\n", dir, rbrew_online::identity().pid);
}
