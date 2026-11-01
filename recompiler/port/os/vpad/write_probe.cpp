// write_probe.cpp - diagnostic: which host instruction writes near a guest address.
//
// Armed only by the RBREW_CAM_OPS command "<frame> P <guest address>" (vpad.cpp). The page holding
// the address is made read-only; each write faults, is counted when it lands within 256 bytes of the
// address (with the faulting host instruction and the return addresses on the stack), then the page
// is opened for that one instruction (trap flag) and protected again. Host addresses map to
// recompiled guest functions through the link map (Gambit_fn_<guest>). "<frame> Q" prints the
// counts. Never active in normal play.
#include <cstdint>

#if !defined(_WIN32)
// The probe needs Windows page-protection faults: elsewhere it does nothing.
void rbrew_write_probe_arm(uint8_t*, uint32_t) {}
void rbrew_write_probe_report() {}
#else
#include <atomic>
#include <cstdint>
#include <cstdio>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {
struct Page { uint8_t* host; uint32_t lo, hi; };   // [lo, hi): guest window that is counted
Page g_pages[16];
std::atomic<int> g_page_count{0};
struct Seen { uintptr_t rip, ret1, ret2; uint32_t guest; uint32_t count; };
Seen g_seen[256];
std::atomic<int> g_seen_count{0};
uint8_t* g_mem = nullptr;
uintptr_t g_module = 0, g_code_lo = 0, g_code_hi = 0;
thread_local uint8_t* t_pending = nullptr;

uint64_t rva_or_zero(uintptr_t address) {
    return address >= g_module && address < g_module + 0x10000000u ? uint64_t(address - g_module) : 0;
}

bool is_code(uintptr_t address) { return address >= g_code_lo && address < g_code_hi; }
void find_code() {
    const auto* base = reinterpret_cast<const uint8_t*>(g_module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    g_code_lo = ~uintptr_t(0);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!(section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uintptr_t lo = g_module + section[i].VirtualAddress, hi = lo + section[i].Misc.VirtualSize;
        if (lo < g_code_lo) g_code_lo = lo;
        if (hi > g_code_hi) g_code_hi = hi;
    }
}

LONG CALLBACK handler(PEXCEPTION_POINTERS ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->ExceptionInformation[0] == 1) {
        uint8_t* const address = reinterpret_cast<uint8_t*>(ep->ExceptionRecord->ExceptionInformation[1]);
        const int pages = g_page_count.load();
        for (int i = 0; i < pages; ++i) {
            if (address < g_pages[i].host || address >= g_pages[i].host + 4096) continue;
            const uint32_t guest = uint32_t(address - g_mem);
            if (guest >= g_pages[i].lo && guest < g_pages[i].hi) {
                const uintptr_t rip = ep->ContextRecord->Rip;
                const uintptr_t* stack = reinterpret_cast<const uintptr_t*>(ep->ContextRecord->Rsp);
                uintptr_t r1 = 0, r2 = 0;
                for (int k = 0; k < 24 && !(r1 && r2); ++k) {
                    if (!is_code(stack[k])) continue;
                    (r1 ? r2 : r1) = stack[k];
                }
                int slot = -1;
                const int seen = g_seen_count.load();
                for (int s = 0; s < seen && s < 256; ++s) if (g_seen[s].rip == rip && g_seen[s].ret1 == r1) { slot = s; break; }
                if (slot < 0 && seen < 256) {
                    slot = g_seen_count.fetch_add(1);
                    if (slot < 256) g_seen[slot] = {rip, r1, r2, guest, 0};
                }
                if (slot >= 0 && slot < 256 && g_seen[slot].count++ < 1)
                    std::fprintf(stderr, "[probe] write guest=%08X rva=0x%llX ret1=0x%llX ret2=0x%llX\n", guest,
                                 static_cast<unsigned long long>(rva_or_zero(rip)),
                                 static_cast<unsigned long long>(rva_or_zero(r1)), static_cast<unsigned long long>(rva_or_zero(r2)));
            }
            DWORD old;
            VirtualProtect(g_pages[i].host, 4096, PAGE_READWRITE, &old);
            t_pending = g_pages[i].host;
            ep->ContextRecord->EFlags |= 0x100;   // trap flag: one instruction, then protect again
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    } else if (code == EXCEPTION_SINGLE_STEP && t_pending) {
        DWORD old;
        VirtualProtect(t_pending, 4096, PAGE_READONLY, &old);
        t_pending = nullptr;
        ep->ContextRecord->EFlags &= ~0x100u;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

void rbrew_write_probe_arm(uint8_t* mem, uint32_t guest) {
    static bool installed = false;
    if (!installed) {
        installed = true; g_mem = mem; g_module = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        find_code();
        AddVectoredExceptionHandler(1, handler);
    }
    uint8_t* const page = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(mem + guest) & ~uintptr_t(4095));
    const int count = g_page_count.load();
    if (count >= 16) return;
    // One entry per page; arming a page again widens its window.
    for (int i = 0; i < count; ++i) if (g_pages[i].host == page) {
        if (guest - 0x100 < g_pages[i].lo) g_pages[i].lo = guest - 0x100;
        if (guest + 0x100 > g_pages[i].hi) g_pages[i].hi = guest + 0x100;
        return;
    }
    g_pages[count] = {page, guest - 0x100, guest + 0x100};
    g_page_count.store(count + 1);
    DWORD old;
    VirtualProtect(page, 4096, PAGE_READONLY, &old);
    std::fprintf(stderr, "[probe] armed page of guest %08X\n", guest);
}

void rbrew_write_probe_report() {
    const int seen = g_seen_count.load() < 256 ? g_seen_count.load() : 256;
    for (int s = 0; s < seen; ++s)
        std::fprintf(stderr, "[probe] summary rva=0x%llX ret1=0x%llX ret2=0x%llX guest=%08X count=%u\n",
                     static_cast<unsigned long long>(rva_or_zero(g_seen[s].rip)),
                     static_cast<unsigned long long>(rva_or_zero(g_seen[s].ret1)),
                     static_cast<unsigned long long>(rva_or_zero(g_seen[s].ret2)), g_seen[s].guest, g_seen[s].count);
}
#endif // _WIN32
