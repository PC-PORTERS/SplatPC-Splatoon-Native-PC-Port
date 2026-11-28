#include "mod_loader.h"

#include "mod_trampoline.h"
#include "native_control_defaults.h"
#include "rebrewu/logger.hpp"
#include "rebrewu_runtime.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace rbrew_mod {

namespace detail {
std::atomic<bool> has_vpad_hooks{false};
std::atomic<bool> has_kpad_providers{false};
std::atomic<bool> has_audio_taps{false};
std::atomic<bool> has_frame_capture{false};
std::atomic<bool> dual_view{false};
std::atomic<bool> suppress_cinema_hud{false};
std::atomic<bool> relative_mouse_gamepad{false};
std::atomic<bool> has_window_title_override{false};
} // namespace detail

namespace {

// ---- logging & path helpers ----

std::string path_to_utf8(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::filesystem::path path_from_utf8(const char* s) {
    if (!s || !*s) return {};
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s), std::strlen(s)));
}

void host_log_v(SplatModLogLevel level, const char* tag, const char* fmt, va_list ap) {
    char buf[2048];
    const int n = std::vsnprintf(buf, sizeof(buf), fmt ? fmt : "", ap);
    if (n < 0) return;
    size_t len = std::min<size_t>(size_t(n), sizeof(buf) - 1);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = '\0';
    const char* safe_tag = (tag && *tag) ? tag : "mod";
    switch (level) {
    case SPLATMOD_LOG_WARN: RBREW_LOG_WARN(safe_tag, "%s\n", buf); break;
    case SPLATMOD_LOG_ERROR: RBREW_LOG_ERROR(safe_tag, "%s\n", buf); break;
    case SPLATMOD_LOG_DEBUG: RBREW_LOG_DEBUG(safe_tag, "%s\n", buf); break;
    case SPLATMOD_LOG_INFO:
    default: RBREW_LOG_INFO(safe_tag, "%s\n", buf); break;
    }
}

void host_log(SplatModLogLevel level, const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    host_log_v(level, tag, fmt, ap);
    va_end(ap);
}

std::mutex s_mod_mutex;
std::mutex s_toggle_mutex;
std::string s_mods_dir_utf8;
std::unordered_set<std::string> s_disabled_mod_ids;
bool s_runtime_initialized = false;
thread_local std::string t_active_mod_id;

struct ActiveModScope {
    std::string prev;
    explicit ActiveModScope(const std::string& id) : prev(t_active_mod_id) { t_active_mod_id = id; }
    ~ActiveModScope() { t_active_mod_id = std::move(prev); }
};

void ensure_mods_dir_initialized() {
    if (!s_mods_dir_utf8.empty()) return;
    if (const char* env_dir = std::getenv("RBREW_MODS_DIR"); env_dir && *env_dir) {
        s_mods_dir_utf8 = env_dir;
        return;
    }
    std::filesystem::path base;
#if defined(_WIN32)
    wchar_t exe_buf[MAX_PATH * 4]{};
    const DWORD n = GetModuleFileNameW(nullptr, exe_buf, DWORD(std::size(exe_buf)));
    if (n > 0 && n < std::size(exe_buf)) base = std::filesystem::path(exe_buf).parent_path();
#else
    char exe_buf[4096]{};
    const ssize_t n = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
    if (n > 0) base = std::filesystem::path(std::string(exe_buf, size_t(n))).parent_path();
#endif
    const std::filesystem::path mods_path = base.empty() ? std::filesystem::path("mods") : (base / "mods");
    s_mods_dir_utf8 = path_to_utf8(mods_path);
}

std::string trim_ascii(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::filesystem::path mods_state_file_path_locked() {
    ensure_mods_dir_initialized();
    return path_from_utf8(s_mods_dir_utf8.c_str()) / "mods_state.ini";
}

void load_disabled_mods_locked() {
    s_disabled_mod_ids.clear();
    const std::filesystem::path state_path = mods_state_file_path_locked();
    std::ifstream in(state_path);
    if (!in.is_open()) return;
    std::string line;
    while (std::getline(in, line)) {
        line = trim_ascii(line);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string id = trim_ascii(line.substr(0, eq));
        std::string val = trim_ascii(line.substr(eq + 1));
        for (char& ch : val) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        if (!id.empty() && (val == "0" || val == "false" || val == "off" || val == "disabled")) {
            s_disabled_mod_ids.insert(id);
        }
    }
}

void save_disabled_mods_locked() {
    const std::filesystem::path state_path = mods_state_file_path_locked();
    std::error_code ec;
    if (state_path.has_parent_path()) std::filesystem::create_directories(state_path.parent_path(), ec);
    const std::filesystem::path tmp_path = path_from_utf8((path_to_utf8(state_path) + ".tmp").c_str());
    {
        std::ofstream out(tmp_path, std::ios::trunc);
        if (!out.is_open()) {
            RBREW_LOG_WARN("mods", "Failed to save mod state to '%s'\n", path_to_utf8(state_path).c_str());
            return;
        }
        out << "# SplatPC mod state (0 = disabled)\n[mods]\n";
        std::vector<std::string> sorted_ids(s_disabled_mod_ids.begin(), s_disabled_mod_ids.end());
        std::sort(sorted_ids.begin(), sorted_ids.end());
        for (const auto& id : sorted_ids) out << id << "=0\n";
    }
    std::filesystem::rename(tmp_path, state_path, ec);
    if (ec) {
        std::filesystem::remove(state_path, ec);
        ec.clear();
        std::filesystem::rename(tmp_path, state_path, ec);
    }
}

const char* host_mods_dir() {
    std::lock_guard lock(s_mod_mutex);
    ensure_mods_dir_initialized();
    return s_mods_dir_utf8.c_str();
}

// ---- loaded mod tracking ----

struct LoadedMod {
    void* lib_handle = nullptr;
    std::string path;
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    SplatModEarlyInitFn early_init = nullptr;
    SplatModInitFn init = nullptr;
    SplatModShutdownFn shutdown = nullptr;
    bool enabled = true;
    bool initialized = false;
};

std::vector<LoadedMod> s_mods;

void* open_shared_lib(const std::filesystem::path& file_path) {
#if defined(_WIN32)
    HMODULE mod = LoadLibraryExW(file_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    return reinterpret_cast<void*>(mod);
#else
    const std::string p = path_to_utf8(file_path);
    return dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* lookup_symbol(void* lib, const char* name) {
    if (!lib || !name) return nullptr;
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}

void close_shared_lib(void* lib) {
    if (!lib) return;
#if defined(_WIN32)
    FreeLibrary(reinterpret_cast<HMODULE>(lib));
#else
    dlclose(lib);
#endif
}

bool try_load_mod_locked(const std::filesystem::path& file_path, LoadedMod*& out_mod) {
    out_mod = nullptr;
    const std::string utf8_path = path_to_utf8(file_path);
    void* lib = open_shared_lib(file_path);
    if (!lib) {
#if defined(_WIN32)
        RBREW_LOG_WARN("mods", "Failed to load mod '%s' (Win32 error %lu)\n", utf8_path.c_str(),
                       static_cast<unsigned long>(GetLastError()));
#else
        const char* err = dlerror();
        RBREW_LOG_WARN("mods", "Failed to load mod '%s': %s\n", utf8_path.c_str(), err ? err : "unknown error");
#endif
        return false;
    }

    auto info_fn = reinterpret_cast<SplatModInfoFn>(lookup_symbol(lib, "splatmod_info"));
    auto init_fn = reinterpret_cast<SplatModInitFn>(lookup_symbol(lib, "splatmod_init"));
    auto early_fn = reinterpret_cast<SplatModEarlyInitFn>(lookup_symbol(lib, "splatmod_early_init"));
    auto shutdown_fn = reinterpret_cast<SplatModShutdownFn>(lookup_symbol(lib, "splatmod_shutdown"));

    if (!info_fn || !init_fn) {
        RBREW_LOG_WARN("mods", "Skipping '%s': missing required splatmod_info or splatmod_init symbol\n",
                       utf8_path.c_str());
        close_shared_lib(lib);
        return false;
    }

    const SplatModInfo* info = info_fn();
    if (!info || info->abi_version != SPLATMOD_ABI_VERSION || !info->id || !*info->id || !info->name || !*info->name) {
        RBREW_LOG_WARN("mods", "Skipping '%s': invalid SplatModInfo or ABI mismatch (expected %u, got %u)\n",
                       utf8_path.c_str(), SPLATMOD_ABI_VERSION, info ? info->abi_version : 0u);
        close_shared_lib(lib);
        return false;
    }

    for (const auto& existing : s_mods) {
        if (existing.id == info->id) {
            RBREW_LOG_WARN("mods", "Skipping duplicate mod id '%s' from '%s'\n", info->id, utf8_path.c_str());
            close_shared_lib(lib);
            return false;
        }
    }

    LoadedMod mod;
    mod.lib_handle = lib;
    mod.path = utf8_path;
    mod.id = info->id;
    mod.name = info->name;
    mod.version = info->version ? info->version : "";
    mod.author = info->author ? info->author : "";
    mod.early_init = early_fn;
    mod.init = init_fn;
    mod.shutdown = shutdown_fn;
    mod.enabled = (s_disabled_mod_ids.find(mod.id) == s_disabled_mod_ids.end());
    s_mods.push_back(std::move(mod));
    out_mod = &s_mods.back();
    RBREW_LOG_INFO("mods", "Loaded mod '%s' (%s %s)\n", out_mod->name.c_str(), out_mod->id.c_str(),
                   out_mod->version.c_str());
    return true;
}

// ---- unique handle allocation ----

std::atomic<uint64_t> s_next_handle{1};

uint64_t allocate_handle() {
    return s_next_handle.fetch_add(1, std::memory_order_relaxed);
}

// ---- 3-tier guest hooks + arbitrary-order native hooks ----

using GuestBodyFn = void (*)(CPUState* __restrict, uint8_t* __restrict);

enum class GuestSlotKind { Manifest, BodyTrampoline, Dispatch };

struct GuestHookNode {
    uint64_t handle = 0;
    unsigned int priority = 0u;
    SplatModGuestHook callback = nullptr;
    void* orig_stub = nullptr;
    std::string owner_mod_id;
};

struct GuestHookSlot {
    uint32_t guest_addr = 0;
    GuestSlotKind kind = GuestSlotKind::Manifest;
    std::vector<GuestHookNode> nodes; // bottom [0] -> top [back()]

    // Tier 1 (manifest hook)
    RbrewGuestHook prev_raw_hook = nullptr;
    void* body_fn = nullptr;
    std::atomic<HostFunc> captured_orig{nullptr};

    // Tier 2 (x86-64 body trampoline)
    void* orig_trampoline = nullptr;
    uint64_t trampoline_id = 0;
    void* bridge = nullptr;

    // Tier 3 (dispatch_table slot wrapper)
    HostFunc prev_dispatch = nullptr;
};

struct NativeHookNode {
    uint64_t handle = 0;
    unsigned int priority = 0u;
    void* detour = nullptr;
    void* orig_stub = nullptr;
    std::string owner_mod_id;
};

struct NativeHookSlot {
    void* target = nullptr;
    void* entry_stub = nullptr;
    void* orig_trampoline = nullptr;
    uint64_t trampoline_id = 0;
    std::vector<NativeHookNode> nodes; // bottom [0] -> top [back()]
};

std::mutex s_hook_mutex;
CPUState* s_dispatch_cpu = nullptr;
std::unordered_map<uint32_t, std::shared_ptr<GuestHookSlot>> s_guest_slots;
std::unordered_map<uint64_t, uint32_t> s_guest_handle_to_addr;
std::unordered_map<void*, std::unique_ptr<NativeHookSlot>> s_native_slots;
std::unordered_map<uint64_t, void*> s_native_handle_to_target;

void guest_orig_dispatcher(CPUState* cpu, uintptr_t cookie) {
    const uint64_t handle = uint64_t(cookie);
    SplatModGuestHook next_cb = nullptr;
    uint32_t guest_addr = 0;
    std::shared_ptr<GuestHookSlot> slot;
    GuestSlotKind kind = GuestSlotKind::Manifest;
    RbrewGuestHook prev_raw = nullptr;
    HostFunc manifest_orig = nullptr;
    void* body_fn = nullptr;
    void* orig_trampoline = nullptr;
    HostFunc prev_dispatch = nullptr;
    bool invoke_bottom = false;

    {
        std::lock_guard lock(s_hook_mutex);
        const auto hit = s_guest_handle_to_addr.find(handle);
        if (hit == s_guest_handle_to_addr.end()) return;
        const auto sit = s_guest_slots.find(hit->second);
        if (sit == s_guest_slots.end()) return;
        slot = sit->second;
        guest_addr = slot->guest_addr;
        for (size_t i = 0; i < slot->nodes.size(); ++i) {
            if (slot->nodes[i].handle == handle) {
                if (i > 0) {
                    next_cb = slot->nodes[i - 1].callback;
                } else {
                    invoke_bottom = true;
                    kind = slot->kind;
                    prev_raw = slot->prev_raw_hook;
                    manifest_orig = slot->captured_orig.load(std::memory_order_acquire);
                    body_fn = slot->body_fn;
                    orig_trampoline = slot->orig_trampoline;
                    prev_dispatch = slot->prev_dispatch;
                }
                break;
            }
        }
    }

    if (next_cb) {
        next_cb(cpu, guest_addr);
        return;
    }
    if (!invoke_bottom || !cpu) return;

    switch (kind) {
    case GuestSlotKind::Manifest:
        if (prev_raw) {
            prev_raw(cpu, guest_addr, manifest_orig);
        } else if (manifest_orig) {
            manifest_orig(cpu);
        } else if (body_fn) {
            reinterpret_cast<GuestBodyFn>(body_fn)(cpu, cpu->mem);
        }
        break;
    case GuestSlotKind::BodyTrampoline:
        if (orig_trampoline) reinterpret_cast<GuestBodyFn>(orig_trampoline)(cpu, cpu->mem);
        break;
    case GuestSlotKind::Dispatch:
        if (prev_dispatch) prev_dispatch(cpu);
        break;
    }
}

void tier1_manifest_entry(CPUState* cpu, uint32_t guest_addr, HostFunc orig) {
    SplatModGuestHook top_cb = nullptr;
    RbrewGuestHook prev_raw = nullptr;
    {
        std::lock_guard lock(s_hook_mutex);
        const auto it = s_guest_slots.find(guest_addr);
        if (it != s_guest_slots.end()) {
            auto& slot = it->second;
            if (orig) slot->captured_orig.store(orig, std::memory_order_release);
            if (!slot->nodes.empty())
                top_cb = slot->nodes.back().callback;
            else
                prev_raw = slot->prev_raw_hook;
        }
    }
    if (top_cb) {
        top_cb(cpu, guest_addr);
    } else if (prev_raw) {
        prev_raw(cpu, guest_addr, orig);
    } else if (orig) {
        orig(cpu);
    }
}

void guest_body_dispatcher(CPUState* cpu, uint8_t* mem, uint32_t guest_addr, void* /*orig_trampoline*/) {
    SplatModGuestHook top_cb = nullptr;
    void* orig_trampoline = nullptr;
    {
        std::lock_guard lock(s_hook_mutex);
        const auto it = s_guest_slots.find(guest_addr);
        if (it != s_guest_slots.end()) {
            const auto& slot = it->second;
            if (!slot->nodes.empty())
                top_cb = slot->nodes.back().callback;
            else
                orig_trampoline = slot->orig_trampoline;
        }
    }
    if (top_cb) {
        top_cb(cpu, guest_addr);
    } else if (orig_trampoline) {
        reinterpret_cast<GuestBodyFn>(orig_trampoline)(cpu, mem);
    }
}

void guest_dispatch_dispatcher(CPUState* cpu, uint32_t guest_addr, void* prev_host_fn) {
    SplatModGuestHook top_cb = nullptr;
    {
        std::lock_guard lock(s_hook_mutex);
        const auto it = s_guest_slots.find(guest_addr);
        if (it != s_guest_slots.end() && !it->second->nodes.empty()) top_cb = it->second->nodes.back().callback;
    }
    if (top_cb) {
        top_cb(cpu, guest_addr);
    } else if (prev_host_fn) {
        reinterpret_cast<HostFunc>(prev_host_fn)(cpu);
    }
}

SplatModHookHandle api_hook_guest(uint32_t guest_addr, SplatModGuestHook hook, SplatModGuestOrig* out_orig,
                                  unsigned int priority) {
    if (!hook || (guest_addr & 3u) != 0) return 0;

    std::lock_guard lock(s_hook_mutex);
    const uint64_t handle = allocate_handle();
    void* orig_stub = create_guest_orig_stub(uintptr_t(handle), reinterpret_cast<void*>(&guest_orig_dispatcher));
    if (!orig_stub) return 0;

    auto existing = s_guest_slots.find(guest_addr);
    if (existing != s_guest_slots.end()) {
        auto& nodes = existing->second->nodes;
        const auto pos = std::upper_bound(nodes.begin(), nodes.end(), priority,
                                          [](unsigned int p, const GuestHookNode& n) { return p < n.priority; });
        nodes.insert(pos, {handle, priority, hook, orig_stub, t_active_mod_id});
        s_guest_handle_to_addr[handle] = guest_addr;
        if (out_orig) *out_orig = reinterpret_cast<SplatModGuestOrig>(orig_stub);
        return handle;
    }

    auto slot = std::make_shared<GuestHookSlot>();
    slot->guest_addr = guest_addr;

    bool manifest_hooked = false;
    void* body_fn = rbrew_find_guest_body(guest_addr, &manifest_hooked);

    if (body_fn && manifest_hooked) {
        slot->kind = GuestSlotKind::Manifest;
        slot->body_fn = body_fn;
        slot->prev_raw_hook = rbrew_guest_hook(guest_addr);
        if (!rbrew_set_guest_hook(guest_addr, &tier1_manifest_entry)) return 0;
    } else if (body_fn) {
        slot->kind = GuestSlotKind::BodyTrampoline;
        slot->body_fn = body_fn;
        slot->bridge = create_guest_body_bridge(guest_addr, nullptr, reinterpret_cast<void*>(&guest_body_dispatcher));
        if (!slot->bridge ||
            !install_native_trampoline(body_fn, slot->bridge, &slot->orig_trampoline, &slot->trampoline_id)) {
            RBREW_LOG_WARN("mods", "Failed to install guest body trampoline at 0x%08X\n", guest_addr);
            return 0;
        }
    } else {
        const uint32_t idx = guest_addr >> 2;
        if (!s_dispatch_cpu || !s_dispatch_cpu->dispatch_table || idx >= s_dispatch_cpu->dispatch_size ||
            !s_dispatch_cpu->dispatch_table[idx]) {
            RBREW_LOG_WARN("mods", "Cannot hook unmapped guest address 0x%08X\n", guest_addr);
            return 0;
        }
        slot->kind = GuestSlotKind::Dispatch;
        slot->prev_dispatch = s_dispatch_cpu->dispatch_table[idx];
        slot->bridge = create_dispatch_bridge(guest_addr, reinterpret_cast<void*>(slot->prev_dispatch),
                                              reinterpret_cast<void*>(&guest_dispatch_dispatcher));
        if (!slot->bridge) return 0;
        s_dispatch_cpu->dispatch_table[idx] = reinterpret_cast<HostFunc>(slot->bridge);
    }

    slot->nodes.push_back({handle, priority, hook, orig_stub, t_active_mod_id});
    s_guest_slots.emplace(guest_addr, std::move(slot));
    s_guest_handle_to_addr[handle] = guest_addr;
    if (out_orig) *out_orig = reinterpret_cast<SplatModGuestOrig>(orig_stub);
    return handle;
}

bool remove_guest_hook_locked(uint64_t handle) {
    const auto hit = s_guest_handle_to_addr.find(handle);
    if (hit == s_guest_handle_to_addr.end()) return false;
    const uint32_t addr = hit->second;
    s_guest_handle_to_addr.erase(hit);

    const auto sit = s_guest_slots.find(addr);
    if (sit == s_guest_slots.end()) return false;
    auto& slot = sit->second;

    bool erased = false;
    for (size_t i = 0; i < slot->nodes.size(); ++i) {
        if (slot->nodes[i].handle == handle) {
            slot->nodes.erase(slot->nodes.begin() + ptrdiff_t(i));
            erased = true;
            break;
        }
    }
    if (!erased) return false;

    if (slot->nodes.empty()) {
        switch (slot->kind) {
        case GuestSlotKind::Manifest: rbrew_set_guest_hook(addr, slot->prev_raw_hook); break;
        case GuestSlotKind::BodyTrampoline: remove_native_trampoline(slot->trampoline_id); break;
        case GuestSlotKind::Dispatch: {
            const uint32_t idx = addr >> 2;
            if (s_dispatch_cpu && s_dispatch_cpu->dispatch_table && idx < s_dispatch_cpu->dispatch_size) {
                s_dispatch_cpu->dispatch_table[idx] = slot->prev_dispatch;
            }
            break;
        }
        }
        s_guest_slots.erase(sit);
    }
    return true;
}

bool api_unhook_guest(SplatModHookHandle handle) {
    if (!handle) return false;
    std::lock_guard lock(s_hook_mutex);
    return remove_guest_hook_locked(handle);
}

SplatModHookHandle api_hook_native(void* target, void* detour, void** out_orig, unsigned int priority) {
    if (!target || !detour) return 0;

    std::lock_guard lock(s_hook_mutex);
    const uint64_t handle = allocate_handle();

    if (const auto it = s_native_slots.find(target); it != s_native_slots.end()) {
        auto& slot = it->second;
        const auto pos = std::upper_bound(slot->nodes.begin(), slot->nodes.end(), priority,
                                          [](unsigned int p, const NativeHookNode& n) { return p < n.priority; });
        const size_t insert_idx = size_t(pos - slot->nodes.begin());
        void* below_target = (insert_idx == 0) ? slot->orig_trampoline : slot->nodes[insert_idx - 1].detour;
        void* orig_stub = create_jump_stub(below_target);
        void* above_stub = (insert_idx == slot->nodes.size()) ? slot->entry_stub : slot->nodes[insert_idx].orig_stub;
        if (!orig_stub || !patch_jump_stub_target(above_stub, detour)) return 0;
        slot->nodes.insert(pos, {handle, priority, detour, orig_stub, t_active_mod_id});
        s_native_handle_to_target[handle] = target;
        if (out_orig) *out_orig = orig_stub;
        return handle;
    }

    auto slot = std::make_unique<NativeHookSlot>();
    slot->target = target;
    slot->entry_stub = create_jump_stub(detour);
    if (!slot->entry_stub) return 0;

    if (!install_native_trampoline(target, slot->entry_stub, &slot->orig_trampoline, &slot->trampoline_id)) {
        RBREW_LOG_WARN("mods", "Failed to install native trampoline at %p\n", target);
        return 0;
    }

    void* orig_stub = create_jump_stub(slot->orig_trampoline);
    if (!orig_stub) {
        remove_native_trampoline(slot->trampoline_id);
        return 0;
    }

    slot->nodes.push_back({handle, priority, detour, orig_stub, t_active_mod_id});
    s_native_slots.emplace(target, std::move(slot));
    s_native_handle_to_target[handle] = target;
    if (out_orig) *out_orig = orig_stub;
    return handle;
}

bool remove_native_hook_locked(uint64_t handle) {
    const auto hit = s_native_handle_to_target.find(handle);
    if (hit == s_native_handle_to_target.end()) return false;
    void* target = hit->second;
    s_native_handle_to_target.erase(hit);

    const auto sit = s_native_slots.find(target);
    if (sit == s_native_slots.end()) return false;
    auto& slot = sit->second;

    size_t idx = slot->nodes.size();
    for (size_t i = 0; i < slot->nodes.size(); ++i) {
        if (slot->nodes[i].handle == handle) {
            idx = i;
            break;
        }
    }
    if (idx == slot->nodes.size()) return false;

    slot->nodes.erase(slot->nodes.begin() + ptrdiff_t(idx));
    if (slot->nodes.empty()) {
        remove_native_trampoline(slot->trampoline_id);
        s_native_slots.erase(sit);
    } else if (idx == slot->nodes.size()) {
        // Removed the top detour: point entry_stub to the new top.
        patch_jump_stub_target(slot->entry_stub, slot->nodes.back().detour);
    } else {
        // Removed a lower/middle detour: relink the node immediately above it.
        void* new_below = (idx == 0) ? slot->orig_trampoline : slot->nodes[idx - 1].detour;
        patch_jump_stub_target(slot->nodes[idx].orig_stub, new_below);
    }
    return true;
}

bool api_unhook_native(SplatModHookHandle handle) {
    if (!handle) return false;
    std::lock_guard lock(s_hook_mutex);
    return remove_native_hook_locked(handle);
}

bool api_call_guest(CPUState* cpu, uint32_t guest_addr, const uint32_t* args, uint32_t arg_count, uint32_t* out_r3) {
    if (!cpu || (guest_addr & 3u) != 0 || arg_count > 8u || (arg_count > 0 && !args)) return false;

    HostFunc dispatch_fn = nullptr;
    const uint32_t idx = guest_addr >> 2;
    if (cpu->dispatch_table && idx < cpu->dispatch_size) dispatch_fn = cpu->dispatch_table[idx];

    void* body_fn = dispatch_fn ? nullptr : rbrew_find_guest_body(guest_addr, nullptr);
    if (!dispatch_fn && !body_fn) return false;

    const uint32_t saved_lr = cpu->lr;
    uint32_t saved_args[8];
    for (uint32_t i = 0; i < 8u; ++i) saved_args[i] = cpu->r[3u + i];

    for (uint32_t i = 0; i < arg_count; ++i) cpu->r[3u + i] = args[i];
    if (dispatch_fn) {
        dispatch_fn(cpu);
    } else {
        reinterpret_cast<GuestBodyFn>(body_fn)(cpu, cpu->mem);
    }
    if (out_r3) *out_r3 = cpu->r[3];

    for (uint32_t i = 0; i < 8u; ++i) cpu->r[3u + i] = saved_args[i];
    cpu->lr = saved_lr;
    return true;
}

int api_active_scene_type(uint8_t* mem, int* out_scene_id) {
    return rbrew_control_defaults::active_scene_type(mem, out_scene_id);
}

bool api_memory_accessible(const uint8_t* mem, uint32_t addr, uint64_t size) {
    return rbrew_memory_accessible(mem, addr, size);
}

void api_write_memory(uint8_t* mem, uint32_t addr, const void* bytes, uint32_t size) {
    rbrew_write_memory(mem, addr, bytes, size);
}

// ---- host extensions & mailboxes ----

struct VpadEntry {
    uint64_t handle = 0;
    SplatModVpadCallback cb = nullptr;
    void* user = nullptr;
    std::string owner_mod_id;
};

struct KpadEntry {
    uint64_t handle = 0;
    SplatModKpadProvider cb = nullptr;
    void* user = nullptr;
    std::string owner_mod_id;
};

struct AudioEntry {
    uint64_t handle = 0;
    SplatModAudioCallback cb = nullptr;
    void* user = nullptr;
    std::string owner_mod_id;
};

struct CaptureEntry {
    uint64_t handle = 0;
    SplatModCaptureQuery query = nullptr;
    SplatModCaptureSubmit submit = nullptr;
    void* user = nullptr;
    std::string owner_mod_id;
};

struct MenuPageEntry {
    uint64_t handle = 0;
    std::string owner_mod_id;
    std::string title;
    SplatModMenuPageCallback draw = nullptr;
    SplatModMenuStatusCallback status = nullptr;
    void* user = nullptr;
};

std::mutex s_ext_mutex;
std::vector<VpadEntry> s_vpad_entries;
std::vector<KpadEntry> s_kpad_entries;
std::vector<AudioEntry> s_audio_entries;
CaptureEntry s_capture_entry;
std::vector<MenuPageEntry> s_menu_pages;
std::string s_window_title_override;

std::atomic<bool> s_relative_mouse_changed{false};
std::atomic<bool> s_window_title_changed{false};
std::atomic<bool> s_screen_mode_requested{false};
std::atomic<int> s_requested_screen_mode{SPLATMOD_SCREEN_TV};
std::atomic<int> s_current_screen_mode{SPLATMOD_SCREEN_TV};
std::atomic<bool> s_quit_requested{false};
std::atomic<uint64_t> s_open_menu_page_req{0};
std::atomic<bool> s_close_menu_req{false};

void sync_ext_flags_locked() {
    detail::has_vpad_hooks.store(!s_vpad_entries.empty(), std::memory_order_release);
    detail::has_kpad_providers.store(!s_kpad_entries.empty(), std::memory_order_release);
    detail::has_audio_taps.store(!s_audio_entries.empty(), std::memory_order_release);
    detail::has_frame_capture.store(s_capture_entry.handle != 0 && s_capture_entry.query && s_capture_entry.submit,
                                    std::memory_order_release);
}

SplatModHookHandle api_add_menu_page(const char* title, SplatModMenuPageCallback draw,
                                     SplatModMenuStatusCallback status, void* user) {
    if (!title || !*title || !draw) return 0;
    std::lock_guard lock(s_ext_mutex);
    const uint64_t h = allocate_handle();
    s_menu_pages.push_back({h, t_active_mod_id, title, draw, status, user});
    return h;
}

SplatModHookHandle api_add_vpad_hook(SplatModVpadCallback cb, void* user) {
    if (!cb) return 0;
    std::lock_guard lock(s_ext_mutex);
    const uint64_t h = allocate_handle();
    s_vpad_entries.push_back({h, cb, user, t_active_mod_id});
    sync_ext_flags_locked();
    return h;
}

SplatModHookHandle api_add_kpad_provider(SplatModKpadProvider provider, void* user) {
    if (!provider) return 0;
    std::lock_guard lock(s_ext_mutex);
    const uint64_t h = allocate_handle();
    s_kpad_entries.push_back({h, provider, user, t_active_mod_id});
    sync_ext_flags_locked();
    return h;
}

SplatModHookHandle api_add_audio_tap(SplatModAudioCallback cb, void* user) {
    if (!cb) return 0;
    std::lock_guard lock(s_ext_mutex);
    const uint64_t h = allocate_handle();
    s_audio_entries.push_back({h, cb, user, t_active_mod_id});
    sync_ext_flags_locked();
    return h;
}

SplatModHookHandle api_set_frame_capture(SplatModCaptureQuery query, SplatModCaptureSubmit submit, void* user) {
    std::lock_guard lock(s_ext_mutex);
    if (!query || !submit) {
        s_capture_entry = {};
        sync_ext_flags_locked();
        return 0;
    }
    const uint64_t h = allocate_handle();
    s_capture_entry = {h, query, submit, user, t_active_mod_id};
    sync_ext_flags_locked();
    return h;
}

bool api_unhook_extension(SplatModHookHandle handle) {
    if (!handle) return false;
    std::lock_guard lock(s_ext_mutex);
    auto erase_by_handle = [handle](auto& vec) {
        for (size_t i = 0; i < vec.size(); ++i) {
            if (vec[i].handle == handle) {
                vec.erase(vec.begin() + ptrdiff_t(i));
                return true;
            }
        }
        return false;
    };
    bool removed = false;
    removed = erase_by_handle(s_vpad_entries) || removed;
    removed = erase_by_handle(s_kpad_entries) || removed;
    removed = erase_by_handle(s_audio_entries) || removed;
    removed = erase_by_handle(s_menu_pages) || removed;
    if (s_capture_entry.handle == handle) {
        s_capture_entry = {};
        removed = true;
    }
    if (removed) sync_ext_flags_locked();
    return removed;
}

void api_set_dual_view_active(bool active) {
    detail::dual_view.store(active, std::memory_order_relaxed);
}

void api_set_suppress_cinema_hud(bool suppress) {
    detail::suppress_cinema_hud.store(suppress, std::memory_order_relaxed);
}

void api_set_relative_mouse_in_gamepad(bool enable) {
    detail::relative_mouse_gamepad.store(enable, std::memory_order_relaxed);
    s_relative_mouse_changed.store(true, std::memory_order_release);
}

void api_set_window_title(const char* title) {
    {
        std::lock_guard lock(s_ext_mutex);
        if (title && *title) {
            s_window_title_override = title;
            detail::has_window_title_override.store(true, std::memory_order_release);
        } else {
            s_window_title_override.clear();
            detail::has_window_title_override.store(false, std::memory_order_release);
        }
    }
    s_window_title_changed.store(true, std::memory_order_release);
}

void api_request_screen_mode(int screen_mode) {
    const int normalized = (screen_mode == SPLATMOD_SCREEN_GAMEPAD) ? SPLATMOD_SCREEN_GAMEPAD : SPLATMOD_SCREEN_TV;
    s_requested_screen_mode.store(normalized, std::memory_order_relaxed);
    s_screen_mode_requested.store(true, std::memory_order_release);
}

int api_current_screen_mode() {
    return s_current_screen_mode.load(std::memory_order_relaxed);
}

void api_request_quit() {
    s_quit_requested.store(true, std::memory_order_release);
}

void api_open_menu_page(SplatModHookHandle page_handle) {
    if (!page_handle) return;
    s_open_menu_page_req.store(page_handle, std::memory_order_release);
}

void api_close_menu() {
    s_close_menu_req.store(true, std::memory_order_release);
}

SplatModEarlyHost s_early_host = {
    SPLATMOD_ABI_VERSION, sizeof(SplatModEarlyHost), 0u, &host_log, &host_mods_dir,
};

SplatModHost s_host = {
    SPLATMOD_ABI_VERSION,
    sizeof(SplatModHost),
    0u,
    &host_log,
    &host_mods_dir,
    &api_hook_guest,
    &api_unhook_guest,
    &api_hook_native,
    &api_unhook_native,
    &api_call_guest,
    &api_active_scene_type,
    &api_memory_accessible,
    &api_write_memory,
    &api_add_menu_page,
    &api_add_vpad_hook,
    &api_add_kpad_provider,
    &api_add_audio_tap,
    &api_set_frame_capture,
    &api_unhook_extension,
    &api_set_dual_view_active,
    &api_set_suppress_cinema_hud,
    &api_set_relative_mouse_in_gamepad,
    &api_set_window_title,
    &api_request_screen_mode,
    &api_current_screen_mode,
    &api_request_quit,
    &api_open_menu_page,
    &api_close_menu,
};

void reset_all_hooks_and_extensions() {
    {
        std::lock_guard lock(s_hook_mutex);
        std::vector<uint64_t> guest_handles;
        guest_handles.reserve(s_guest_handle_to_addr.size());
        for (const auto& [h, _] : s_guest_handle_to_addr) guest_handles.push_back(h);
        for (uint64_t h : guest_handles) remove_guest_hook_locked(h);

        std::vector<uint64_t> native_handles;
        native_handles.reserve(s_native_handle_to_target.size());
        for (const auto& [h, _] : s_native_handle_to_target) native_handles.push_back(h);
        for (uint64_t h : native_handles) remove_native_hook_locked(h);
    }
    {
        std::lock_guard lock(s_ext_mutex);
        s_vpad_entries.clear();
        s_kpad_entries.clear();
        s_audio_entries.clear();
        s_capture_entry = {};
        s_menu_pages.clear();
        s_window_title_override.clear();
        sync_ext_flags_locked();
        detail::has_window_title_override.store(false, std::memory_order_release);
    }
    detail::dual_view.store(false, std::memory_order_relaxed);
    detail::suppress_cinema_hud.store(false, std::memory_order_relaxed);
    detail::relative_mouse_gamepad.store(false, std::memory_order_relaxed);
    s_relative_mouse_changed.store(false, std::memory_order_relaxed);
    s_window_title_changed.store(false, std::memory_order_relaxed);
    s_screen_mode_requested.store(false, std::memory_order_relaxed);
    s_quit_requested.store(false, std::memory_order_relaxed);
    s_open_menu_page_req.store(0, std::memory_order_relaxed);
    s_close_menu_req.store(false, std::memory_order_relaxed);
}

void cleanup_mod_hooks_and_extensions(const std::string& mod_id) {
    if (mod_id.empty()) return;
    {
        std::lock_guard lock(s_hook_mutex);
        std::vector<uint64_t> guest_handles;
        for (const auto& [addr, slot] : s_guest_slots) {
            (void)addr;
            if (!slot) continue;
            for (const auto& node : slot->nodes) {
                if (node.owner_mod_id == mod_id) guest_handles.push_back(node.handle);
            }
        }
        for (uint64_t h : guest_handles) remove_guest_hook_locked(h);

        std::vector<uint64_t> native_handles;
        for (const auto& [target, slot] : s_native_slots) {
            (void)target;
            if (!slot) continue;
            for (const auto& node : slot->nodes) {
                if (node.owner_mod_id == mod_id) native_handles.push_back(node.handle);
            }
        }
        for (uint64_t h : native_handles) remove_native_hook_locked(h);
    }
    std::vector<uint64_t> removed_pages;
    {
        std::lock_guard lock(s_ext_mutex);
        auto erase_by_owner = [&mod_id](auto& vec) {
            vec.erase(
                std::remove_if(vec.begin(), vec.end(), [&mod_id](const auto& e) { return e.owner_mod_id == mod_id; }),
                vec.end());
        };
        erase_by_owner(s_vpad_entries);
        erase_by_owner(s_kpad_entries);
        erase_by_owner(s_audio_entries);
        s_menu_pages.erase(std::remove_if(s_menu_pages.begin(), s_menu_pages.end(),
                                          [&mod_id, &removed_pages](const MenuPageEntry& p) {
                                              if (p.owner_mod_id == mod_id) {
                                                  removed_pages.push_back(p.handle);
                                                  return true;
                                              }
                                              return false;
                                          }),
                           s_menu_pages.end());
        if (s_capture_entry.owner_mod_id == mod_id) s_capture_entry = {};
        sync_ext_flags_locked();
    }
    for (uint64_t h : removed_pages) {
        uint64_t expected = h;
        s_open_menu_page_req.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
    }
}

} // namespace

namespace detail {

void dispatch_vpad_hooks(uint8_t* mem) {
    std::vector<VpadEntry> copy;
    {
        std::lock_guard lock(s_ext_mutex);
        copy = s_vpad_entries;
    }
    for (const auto& e : copy)
        if (e.cb) e.cb(mem, e.user);
}

bool dispatch_kpad_provider(uint32_t chan, SplatModProPad* out_pad) {
    if (!out_pad) return false;
    std::vector<KpadEntry> copy;
    {
        std::lock_guard lock(s_ext_mutex);
        copy = s_kpad_entries;
    }
    for (const auto& e : copy) {
        if (e.cb && e.cb(chan, out_pad, e.user)) return true;
    }
    return false;
}

void dispatch_audio_taps(const float* stereo_frames, uint32_t frame_count) {
    if (!stereo_frames || frame_count == 0) return;
    thread_local std::vector<AudioEntry> tl_copy;
    {
        std::lock_guard lock(s_ext_mutex);
        tl_copy = s_audio_entries;
    }
    for (const auto& e : tl_copy)
        if (e.cb) e.cb(stereo_frames, frame_count, e.user);
}

bool copy_window_title_override_locked(std::string& out_title) {
    std::lock_guard lock(s_ext_mutex);
    if (s_window_title_override.empty()) return false;
    out_title = s_window_title_override;
    return true;
}

} // namespace detail

int load_and_early_init(int argc, char** argv, uint32_t game_revision) {
    if (const char* no_mods = std::getenv("RBREW_NO_MODS"); no_mods && std::strcmp(no_mods, "1") == 0) {
        RBREW_LOG_INFO("mods", "Mod loading disabled via RBREW_NO_MODS=1\n");
        return -1;
    }

    std::vector<std::filesystem::path> candidate_files;
    {
        std::lock_guard lock(s_mod_mutex);
        ensure_mods_dir_initialized();
        load_disabled_mods_locked();
        s_early_host.game_revision = game_revision;
        s_host.game_revision = game_revision;

        const std::filesystem::path mods_dir = path_from_utf8(s_mods_dir_utf8.c_str());
        std::error_code ec;
        if (std::filesystem::exists(mods_dir, ec) && std::filesystem::is_directory(mods_dir, ec)) {
#if defined(_WIN32)
            constexpr const char* kExt = ".dll";
#else
            constexpr const char* kExt = ".so";
#endif
            for (const auto& entry : std::filesystem::directory_iterator(mods_dir, ec)) {
                if (!entry.is_regular_file(ec)) continue;
                const auto ext = entry.path().extension();
                if (ext == kExt) candidate_files.push_back(entry.path());
            }
            std::sort(candidate_files.begin(), candidate_files.end(),
                      [](const auto& a, const auto& b) { return a.filename() < b.filename(); });
        }
    }

    std::vector<std::pair<std::string, SplatModEarlyInitFn>> early_hooks;
    {
        std::lock_guard lock(s_mod_mutex);
        for (const auto& file_path : candidate_files) {
            LoadedMod* mod = nullptr;
            if (try_load_mod_locked(file_path, mod) && mod && mod->enabled && mod->early_init) {
                early_hooks.emplace_back(mod->id, mod->early_init);
            } else if (mod && !mod->enabled) {
                RBREW_LOG_INFO("mods", "Mod '%s' (%s) is disabled in mods_state.ini\n", mod->name.c_str(),
                               mod->id.c_str());
            }
        }
    }

    for (const auto& [id, fn] : early_hooks) {
        ActiveModScope scope(id);
        const int exit_code = fn(&s_early_host, argc, argv);
        if (exit_code >= 0) {
            RBREW_LOG_INFO("mods", "Mod '%s' requested early exit with code %d\n", id.c_str(), exit_code);
            return exit_code;
        }
    }
    return -1;
}

void init_mods(CPUState* cpu) {
    {
        std::lock_guard lock(s_hook_mutex);
        s_dispatch_cpu = cpu;
    }
    {
        std::lock_guard lock(s_mod_mutex);
        s_runtime_initialized = true;
    }

    struct PendingInit {
        size_t index;
        std::string id;
        SplatModInitFn fn;
    };
    std::vector<PendingInit> pending;
    {
        std::lock_guard lock(s_mod_mutex);
        for (size_t i = 0; i < s_mods.size(); ++i) {
            if (s_mods[i].enabled && !s_mods[i].initialized && s_mods[i].init) {
                pending.push_back({i, s_mods[i].id, s_mods[i].init});
            }
        }
    }

    for (const auto& item : pending) {
        bool ok = false;
        {
            ActiveModScope scope(item.id);
            ok = item.fn(&s_host);
        }
        if (ok) {
            std::lock_guard lock(s_mod_mutex);
            if (item.index < s_mods.size() && s_mods[item.index].id == item.id) s_mods[item.index].initialized = true;
        } else {
            cleanup_mod_hooks_and_extensions(item.id);
            RBREW_LOG_WARN("mods", "Mod '%s' returned false from splatmod_init\n", item.id.c_str());
        }
    }
}

void shutdown_mods() {
    {
        std::lock_guard lock(s_mod_mutex);
        s_runtime_initialized = false;
    }
    std::vector<std::pair<std::string, SplatModShutdownFn>> to_shutdown;
    {
        std::lock_guard lock(s_mod_mutex);
        for (size_t i = s_mods.size(); i > 0; --i) {
            auto& m = s_mods[i - 1];
            if (m.initialized && m.shutdown) to_shutdown.emplace_back(m.id, m.shutdown);
            m.initialized = false;
        }
    }

    for (const auto& [id, fn] : to_shutdown) {
        fn();
    }

    reset_all_hooks_and_extensions();

    std::vector<void*> libs_to_close;
    {
        std::lock_guard lock(s_mod_mutex);
        for (size_t i = s_mods.size(); i > 0; --i) libs_to_close.push_back(s_mods[i - 1].lib_handle);
        s_mods.clear();
    }
    for (void* lib : libs_to_close) close_shared_lib(lib);

    {
        std::lock_guard lock(s_hook_mutex);
        s_dispatch_cpu = nullptr;
    }
}

bool load_mod_from_path(const char* path, uint32_t game_revision) {
    if (!path || !*path) return false;
    SplatModInitFn init_fn = nullptr;
    std::string id;
    bool enabled = true;
    {
        std::lock_guard lock(s_mod_mutex);
        ensure_mods_dir_initialized();
        load_disabled_mods_locked();
        s_early_host.game_revision = game_revision;
        s_host.game_revision = game_revision;
        s_runtime_initialized = true;
        LoadedMod* mod = nullptr;
        if (!try_load_mod_locked(path_from_utf8(path), mod) || !mod) return false;
        init_fn = mod->init;
        id = mod->id;
        enabled = mod->enabled;
    }
    if (!enabled) return true;
    if (!init_fn) return false;
    bool ok = false;
    {
        ActiveModScope scope(id);
        ok = init_fn(&s_host);
    }
    if (ok) {
        std::lock_guard lock(s_mod_mutex);
        for (auto& m : s_mods) {
            if (m.id == id) {
                m.initialized = true;
                break;
            }
        }
    } else {
        cleanup_mod_hooks_and_extensions(id);
    }
    return ok;
}

void set_mods_dir_for_test(const char* dir) {
    std::lock_guard lock(s_mod_mutex);
    s_mods_dir_utf8 = dir ? dir : "";
    s_disabled_mod_ids.clear();
}

void set_dispatch_cpu_for_test(CPUState* cpu) {
    std::lock_guard lock(s_hook_mutex);
    s_dispatch_cpu = cpu;
}

void unload_mods_for_test() {
    shutdown_mods();
    std::lock_guard lock(s_mod_mutex);
    s_disabled_mod_ids.clear();
    s_mods_dir_utf8.clear();
}

const SplatModHost* host_api() {
    return &s_host;
}

bool has_mods() {
    std::lock_guard lock(s_mod_mutex);
    return !s_mods.empty();
}

std::vector<ModSummary> list_mods() {
    std::lock_guard lock(s_mod_mutex);
    std::vector<ModSummary> out;
    out.reserve(s_mods.size());
    for (const auto& m : s_mods) {
        const std::string fname = path_to_utf8(path_from_utf8(m.path.c_str()).filename());
        out.push_back({m.id, m.name, m.version, m.author, fname, m.enabled, m.initialized});
    }
    return out;
}

std::vector<ModMenuPageView> list_menu_pages() {
    std::lock_guard lock(s_ext_mutex);
    std::vector<ModMenuPageView> out;
    out.reserve(s_menu_pages.size());
    for (const auto& p : s_menu_pages) {
        out.push_back({p.handle, p.owner_mod_id, p.title, p.draw, p.status, p.user});
    }
    return out;
}

bool is_mod_enabled(const std::string& mod_id) {
    std::lock_guard lock(s_mod_mutex);
    for (const auto& m : s_mods) {
        if (m.id == mod_id) return m.enabled;
    }
    return false;
}

bool set_mod_enabled(const std::string& mod_id, bool enabled, std::string* out_error) {
    if (out_error) out_error->clear();
    std::lock_guard toggle_lock(s_toggle_mutex);

    if (!enabled) {
        bool found = false;
        bool was_init = false;
        SplatModShutdownFn shutdown_fn = nullptr;
        {
            std::lock_guard lock(s_mod_mutex);
            for (auto& m : s_mods) {
                if (m.id == mod_id) {
                    found = true;
                    m.enabled = false;
                    was_init = m.initialized;
                    m.initialized = false;
                    shutdown_fn = m.shutdown;
                    s_disabled_mod_ids.insert(mod_id);
                    save_disabled_mods_locked();
                    break;
                }
            }
        }
        if (!found) {
            if (out_error) *out_error = "Mod not found";
            return false;
        }
        if (was_init) {
            if (shutdown_fn) shutdown_fn();
            cleanup_mod_hooks_and_extensions(mod_id);
        }
        RBREW_LOG_INFO("mods", "Disabled mod '%s'\n", mod_id.c_str());
        return true;
    }

    bool found = false;
    bool runtime_ready = false;
    bool already_init = false;
    SplatModInitFn init_fn = nullptr;
    {
        std::lock_guard lock(s_mod_mutex);
        runtime_ready = s_runtime_initialized;
        for (auto& m : s_mods) {
            if (m.id == mod_id) {
                found = true;
                already_init = m.initialized;
                init_fn = m.init;
                if (!runtime_ready || already_init) {
                    m.enabled = true;
                    s_disabled_mod_ids.erase(mod_id);
                    save_disabled_mods_locked();
                }
                break;
            }
        }
    }
    if (!found) {
        if (out_error) *out_error = "Mod not found";
        return false;
    }
    if (!runtime_ready || already_init) return true;
    if (!init_fn) {
        if (out_error) *out_error = "Mod has no init entry point";
        return false;
    }

    bool ok = false;
    {
        ActiveModScope scope(mod_id);
        ok = init_fn(&s_host);
    }
    if (!ok) {
        cleanup_mod_hooks_and_extensions(mod_id);
        if (out_error) *out_error = "Mod failed to initialize";
        return false;
    }

    {
        std::lock_guard lock(s_mod_mutex);
        for (auto& m : s_mods) {
            if (m.id == mod_id) {
                m.enabled = true;
                m.initialized = true;
                s_disabled_mod_ids.erase(mod_id);
                save_disabled_mods_locked();
                break;
            }
        }
    }
    RBREW_LOG_INFO("mods", "Enabled mod '%s'\n", mod_id.c_str());
    return true;
}

bool delete_mod(const std::string& mod_id, std::string* out_error) {
    if (out_error) out_error->clear();
    std::lock_guard toggle_lock(s_toggle_mutex);

    bool found = false;
    bool was_init = false;
    SplatModShutdownFn shutdown_fn = nullptr;
    void* lib_handle = nullptr;
    std::string mod_path;
    {
        std::lock_guard lock(s_mod_mutex);
        for (size_t i = 0; i < s_mods.size(); ++i) {
            if (s_mods[i].id == mod_id) {
                found = true;
                was_init = s_mods[i].initialized;
                shutdown_fn = s_mods[i].shutdown;
                lib_handle = s_mods[i].lib_handle;
                mod_path = s_mods[i].path;
                s_mods.erase(s_mods.begin() + ptrdiff_t(i));
                s_disabled_mod_ids.erase(mod_id);
                save_disabled_mods_locked();
                break;
            }
        }
    }
    if (!found) {
        if (out_error) *out_error = "Mod not found";
        return false;
    }

    if (was_init) {
        if (shutdown_fn) shutdown_fn();
        cleanup_mod_hooks_and_extensions(mod_id);
    }
    close_shared_lib(lib_handle);

    std::error_code ec;
    const std::filesystem::path file_path = path_from_utf8(mod_path.c_str());
    if (!std::filesystem::remove(file_path, ec) && std::filesystem::exists(file_path, ec)) {
        if (out_error) {
            *out_error = ec ? ec.message() : "Failed to delete mod file";
        }
        RBREW_LOG_WARN("mods", "Unloaded mod '%s' but could not delete '%s': %s\n", mod_id.c_str(), mod_path.c_str(),
                       ec.message().c_str());
        return false;
    }
    RBREW_LOG_INFO("mods", "Deleted mod '%s' (%s)\n", mod_id.c_str(), mod_path.c_str());
    return true;
}

size_t rescan_mods() {
    std::lock_guard toggle_lock(s_toggle_mutex);

    struct PendingRescan {
        std::string id;
        SplatModInitFn init;
    };
    std::vector<PendingRescan> pending_init;
    bool runtime_ready = false;
    size_t added_count = 0;

    {
        std::lock_guard lock(s_mod_mutex);
        ensure_mods_dir_initialized();
        load_disabled_mods_locked();
        runtime_ready = s_runtime_initialized;

        const std::filesystem::path mods_dir = path_from_utf8(s_mods_dir_utf8.c_str());
        std::error_code ec;
        if (!std::filesystem::exists(mods_dir, ec) || !std::filesystem::is_directory(mods_dir, ec)) {
            return 0;
        }

        std::vector<std::filesystem::path> candidate_files;
#if defined(_WIN32)
        constexpr const char* kExt = ".dll";
#else
        constexpr const char* kExt = ".so";
#endif
        for (const auto& entry : std::filesystem::directory_iterator(mods_dir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            if (entry.path().extension() == kExt) candidate_files.push_back(entry.path());
        }
        std::sort(candidate_files.begin(), candidate_files.end(),
                  [](const auto& a, const auto& b) { return a.filename() < b.filename(); });

        for (const auto& file_path : candidate_files) {
            bool already_loaded = false;
            for (const auto& existing : s_mods) {
                std::error_code eq_ec;
                if (existing.path == path_to_utf8(file_path) ||
                    std::filesystem::equivalent(path_from_utf8(existing.path.c_str()), file_path, eq_ec)) {
                    already_loaded = true;
                    break;
                }
            }
            if (already_loaded) continue;

            LoadedMod* mod = nullptr;
            if (try_load_mod_locked(file_path, mod) && mod) {
                ++added_count;
                if (runtime_ready && mod->enabled && mod->init) {
                    pending_init.push_back({mod->id, mod->init});
                }
            }
        }
    }

    for (const auto& item : pending_init) {
        bool ok = false;
        {
            ActiveModScope scope(item.id);
            ok = item.init(&s_host);
        }
        if (ok) {
            std::lock_guard lock(s_mod_mutex);
            for (auto& m : s_mods) {
                if (m.id == item.id) {
                    m.initialized = true;
                    break;
                }
            }
        } else {
            cleanup_mod_hooks_and_extensions(item.id);
            RBREW_LOG_WARN("mods", "Newly rescanned mod '%s' failed to initialize\n", item.id.c_str());
        }
    }
    return added_count;
}

bool query_frame_capture(uint32_t* out_view, uint32_t* out_width, uint32_t* out_height) {
    if (!out_view || !out_width || !out_height) return false;
    CaptureEntry entry;
    {
        std::lock_guard lock(s_ext_mutex);
        entry = s_capture_entry;
    }
    if (!entry.query) return false;
    return entry.query(out_view, out_width, out_height, entry.user);
}

void submit_captured_frame(const uint8_t* rgba, uint32_t width, uint32_t height) {
    if (!rgba || width == 0 || height == 0) return;
    CaptureEntry entry;
    {
        std::lock_guard lock(s_ext_mutex);
        entry = s_capture_entry;
    }
    if (entry.submit) entry.submit(rgba, width, height, entry.user);
}

bool consume_relative_mouse_changed() {
    return s_relative_mouse_changed.exchange(false, std::memory_order_acq_rel);
}

bool consume_window_title_changed() {
    return s_window_title_changed.exchange(false, std::memory_order_acq_rel);
}

bool consume_screen_mode_request(int* out_mode) {
    if (!s_screen_mode_requested.exchange(false, std::memory_order_acq_rel)) return false;
    if (out_mode) *out_mode = s_requested_screen_mode.load(std::memory_order_relaxed);
    return true;
}

void note_screen_mode(int mode) {
    s_current_screen_mode.store(mode, std::memory_order_relaxed);
}

int current_screen_mode() {
    return s_current_screen_mode.load(std::memory_order_relaxed);
}

bool consume_quit_request() {
    return s_quit_requested.exchange(false, std::memory_order_acq_rel);
}

bool consume_open_menu_page_request(uint64_t* out_handle) {
    const uint64_t h = s_open_menu_page_req.exchange(0, std::memory_order_acq_rel);
    if (h == 0) return false;
    if (out_handle) *out_handle = h;
    return true;
}

bool consume_close_menu_request() {
    return s_close_menu_req.exchange(false, std::memory_order_acq_rel);
}

} // namespace rbrew_mod
