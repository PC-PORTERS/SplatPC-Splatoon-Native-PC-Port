#include "mod_trampoline.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
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
#include <cstdio>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace rbrew_mod {

#if defined(__x86_64__) || defined(_M_X64)

namespace {

constexpr size_t kSlabSize = 64 * 1024;
constexpr size_t kPatchSize = 5; // E9 rel32
constexpr intptr_t kMaxNearDistance = 0x50000000;

struct ExecSlab {
    uint8_t* base = nullptr;
    size_t capacity = 0;
    size_t used = 0;
};

struct ActiveTrampoline {
    uint64_t id = 0;
    uint8_t* target = nullptr;
    uint8_t saved_bytes[32]{};
    size_t patch_len = 0;
    void* trampoline = nullptr;
};

std::mutex s_trampoline_mutex;
std::vector<ExecSlab> s_slabs;
std::vector<ActiveTrampoline> s_trampolines;
uint64_t s_next_trampoline_id = 1;

bool in_rel32_range(const void* from_end, const void* to) {
    const intptr_t delta = reinterpret_cast<intptr_t>(to) - reinterpret_cast<intptr_t>(from_end);
    return delta >= INT32_MIN && delta <= INT32_MAX;
}

bool within_search_window(const void* a, const void* b) {
    const intptr_t delta = reinterpret_cast<intptr_t>(a) - reinterpret_cast<intptr_t>(b);
    return delta >= -kMaxNearDistance && delta <= kMaxNearDistance;
}

uint8_t* alloc_near_slab(const void* anchor, size_t min_bytes) {
    const size_t size = std::max(kSlabSize, (min_bytes + 4095u) & ~size_t(4095u));
#if defined(_WIN32)
    if (!anchor) {
        return static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    }
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t gran = std::max<uintptr_t>(si.dwAllocationGranularity, 0x10000u);
    const uintptr_t center = reinterpret_cast<uintptr_t>(anchor);
    const uintptr_t low = center > uintptr_t(kMaxNearDistance) ? (center - uintptr_t(kMaxNearDistance)) : gran;
    const uintptr_t high = center + uintptr_t(kMaxNearDistance);

    auto try_region = [&](uintptr_t start, uintptr_t end) -> uint8_t* {
        for (uintptr_t addr = start; addr < end;) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<const void*>(addr), &mbi, sizeof(mbi)) == 0) break;
            const uintptr_t reg_base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            const uintptr_t reg_end = reg_base + mbi.RegionSize;
            if (mbi.State == MEM_FREE) {
                uintptr_t candidate = (std::max(addr, reg_base) + gran - 1u) & ~(gran - 1u);
                if (candidate >= gran && candidate + size <= reg_end && candidate + size <= high) {
                    if (void* mem = VirtualAlloc(reinterpret_cast<void*>(candidate), size, MEM_COMMIT | MEM_RESERVE,
                                                 PAGE_EXECUTE_READWRITE))
                        return static_cast<uint8_t*>(mem);
                }
            }
            if (reg_end <= addr) break;
            addr = reg_end;
        }
        return nullptr;
    };

    if (uint8_t* up = try_region(center, high)) return up;
    // Scan downward from center in granularity steps.
    for (uintptr_t probe = (center & ~(gran - 1u)); probe >= low + gran;) {
        probe -= gran;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(probe), &mbi, sizeof(mbi)) == 0) break;
        if (mbi.State == MEM_FREE) {
            const uintptr_t reg_base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            const uintptr_t reg_end = reg_base + mbi.RegionSize;
            uintptr_t candidate = (probe & ~(gran - 1u));
            if (candidate >= reg_base && candidate + size <= reg_end) {
                if (void* mem = VirtualAlloc(reinterpret_cast<void*>(candidate), size, MEM_COMMIT | MEM_RESERVE,
                                             PAGE_EXECUTE_READWRITE))
                    return static_cast<uint8_t*>(mem);
            }
        }
        const uintptr_t reg_base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        probe = std::min(probe, reg_base);
    }
    return nullptr;
#else
    if (!anchor) {
        void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        return p == MAP_FAILED ? nullptr : static_cast<uint8_t*>(p);
    }
    const long page_sz = sysconf(_SC_PAGESIZE);
    const uintptr_t page = page_sz > 0 ? uintptr_t(page_sz) : 4096u;
    const uintptr_t center = reinterpret_cast<uintptr_t>(anchor) & ~(page - 1u);
    const uintptr_t low = center > uintptr_t(kMaxNearDistance) ? (center - uintptr_t(kMaxNearDistance)) : 0x10000u;
    const uintptr_t high = center + uintptr_t(kMaxNearDistance);

    // Inspect /proc/self/maps for unmapped gaps near `anchor`.
    struct Range {
        uintptr_t start, end;
    };
    std::vector<Range> mapped;
    if (FILE* f = std::fopen("/proc/self/maps", "r")) {
        char line[512];
        while (std::fgets(line, sizeof(line), f)) {
            unsigned long long a = 0, b = 0;
            if (std::sscanf(line, "%llx-%llx", &a, &b) == 2) mapped.push_back({uintptr_t(a), uintptr_t(b)});
        }
        std::fclose(f);
    }
    std::sort(mapped.begin(), mapped.end(), [](const Range& x, const Range& y) { return x.start < y.start; });

    auto try_map = [&](uintptr_t addr) -> uint8_t* {
        void* p = mmap(reinterpret_cast<void*>(addr), size, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return nullptr;
        if (!within_search_window(p, anchor)) {
            munmap(p, size);
            return nullptr;
        }
        return static_cast<uint8_t*>(p);
    };

    uintptr_t cursor = low;
    for (const auto& r : mapped) {
        if (r.end <= cursor) continue;
        if (r.start > cursor) {
            const uintptr_t gap_end = std::min(r.start, high);
            if (gap_end > cursor && gap_end - cursor >= size) {
                const uintptr_t candidate = (gap_end > center) ? cursor : (gap_end - size);
                if (uint8_t* p = try_map(candidate)) return p;
            }
        }
        cursor = std::max(cursor, r.end);
        if (cursor >= high) break;
    }
    if (cursor < high && high - cursor >= size) {
        if (uint8_t* p = try_map(cursor)) return p;
    }
    return nullptr;
#endif
}

uint8_t* alloc_exec_bytes(const void* near_target, size_t bytes) {
    const size_t aligned = (bytes + 15u) & ~size_t(15u);
    for (auto& slab : s_slabs) {
        if (slab.capacity - slab.used >= aligned &&
            (!near_target || (within_search_window(slab.base + slab.used, near_target) &&
                              within_search_window(slab.base + slab.used + aligned, near_target)))) {
            uint8_t* ptr = slab.base + slab.used;
            slab.used += aligned;
            return ptr;
        }
    }
    uint8_t* mem = alloc_near_slab(near_target, aligned);
    if (!mem) return nullptr;
    s_slabs.push_back({mem, std::max(kSlabSize, (aligned + 4095u) & ~size_t(4095u)), aligned});
    return mem;
}

bool write_code_bytes(uint8_t* dst, const uint8_t* src, size_t len) {
#if defined(_WIN32)
    DWORD old_prot = 0;
    if (!VirtualProtect(dst, len, PAGE_EXECUTE_READWRITE, &old_prot)) return false;
    std::memcpy(dst, src, len);
    DWORD dummy = 0;
    VirtualProtect(dst, len, old_prot, &dummy);
    FlushInstructionCache(GetCurrentProcess(), dst, len);
    return true;
#else
    const long page_sz = sysconf(_SC_PAGESIZE);
    const uintptr_t page = page_sz > 0 ? uintptr_t(page_sz) : 4096u;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(dst) & ~(page - 1u);
    const uintptr_t end = (reinterpret_cast<uintptr_t>(dst) + len + page - 1u) & ~(page - 1u);
    if (mprotect(reinterpret_cast<void*>(begin), end - begin, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return false;
    std::memcpy(dst, src, len);
    __builtin___clear_cache(reinterpret_cast<char*>(dst), reinterpret_cast<char*>(dst + len));
    mprotect(reinterpret_cast<void*>(begin), end - begin, PROT_READ | PROT_EXEC);
    return true;
#endif
}

enum class RelocKind { None, RipDisp32, Rel32, Rel8Jmp, Rel8Jcc };

struct DecodedInstr {
    size_t len = 0;
    RelocKind kind = RelocKind::None;
    size_t disp_offset = 0; // offset of rel32/disp32/rel8 within instruction
    uint8_t jcc_cc = 0;
    bool terminator = false;
    bool valid = false;
};

DecodedInstr decode_x64_instr(const uint8_t* p, size_t max_len) {
    DecodedInstr out{};
    if (max_len == 0) return out;
    size_t i = 0;
    bool has_66 = false, has_67 = false, has_f2 = false, has_f3 = false;

    auto is_legacy_prefix = [](uint8_t b) {
        switch (b) {
        case 0xF0:
        case 0xF2:
        case 0xF3:
        case 0x2E:
        case 0x36:
        case 0x3E:
        case 0x26:
        case 0x64:
        case 0x65:
        case 0x66:
        case 0x67: return true;
        default: return false;
        }
    };
    while (i < max_len && i < 4 && is_legacy_prefix(p[i])) {
        if (p[i] == 0x66) has_66 = true;
        if (p[i] == 0x67) has_67 = true;
        if (p[i] == 0xF2) has_f2 = true;
        if (p[i] == 0xF3) has_f3 = true;
        ++i;
    }
    if (i >= max_len) return out;

    // Intel CET endbr64 / endbr32 (F3 0F 1E FA / FB)
    if (has_f3 && i + 3 <= max_len && p[i] == 0x0F && p[i + 1] == 0x1E && (p[i + 2] == 0xFA || p[i + 2] == 0xFB)) {
        out.len = i + 3;
        out.valid = true;
        return out;
    }
    (void)has_f2;

    uint8_t rex = 0;
    if ((p[i] & 0xF0u) == 0x40u) {
        rex = p[i++];
        if (i >= max_len) return out;
    }
    const bool rex_w = (rex & 0x08u) != 0;

    auto decode_modrm = [&](size_t imm_bytes) -> bool {
        if (i >= max_len) return false;
        const uint8_t modrm = p[i++];
        const uint8_t mod = (modrm >> 6) & 3u;
        const uint8_t rm = modrm & 7u;
        uint8_t sib_base = 0;
        bool has_sib = false;
        if (mod != 3u && rm == 4u) {
            if (i >= max_len) return false;
            sib_base = p[i++] & 7u;
            has_sib = true;
        }
        size_t disp = 0;
        if (mod == 0u && rm == 5u) {
            out.kind = RelocKind::RipDisp32;
            out.disp_offset = i;
            disp = 4;
        } else if (mod == 0u && has_sib && sib_base == 5u) {
            disp = 4;
        } else if (mod == 1u) {
            disp = 1;
        } else if (mod == 2u) {
            disp = 4;
        }
        if (i + disp + imm_bytes > max_len) return false;
        i += disp + imm_bytes;
        out.len = i;
        out.valid = true;
        return true;
    };

    const uint8_t op = p[i++];

    // VEX prefixes (2-byte 0xC5, 3-byte 0xC4)
    if (rex == 0 && (op == 0xC5 || op == 0xC4)) {
        uint8_t map = 1;
        if (op == 0xC5) {
            if (i >= max_len) return out;
            ++i;
        } else {
            if (i + 2 > max_len) return out;
            map = p[i] & 0x1Fu;
            i += 2;
        }
        if (i >= max_len) return out;
        const uint8_t vop = p[i++];
        if (map == 1 && vop == 0x77) { // vzeroupper / vzeroall
            out.len = i;
            out.valid = true;
            return out;
        }
        size_t imm = 0;
        if (map == 3 || (map == 1 && (vop == 0x70 || vop == 0x71 || vop == 0x72 || vop == 0x73 || vop == 0xC2 ||
                                      vop == 0xC4 || vop == 0xC5 || vop == 0xC6)))
            imm = 1;
        decode_modrm(imm);
        return out;
    }

    // Two-byte / three-byte opcodes
    if (op == 0x0F) {
        if (i >= max_len) return out;
        const uint8_t op2 = p[i++];
        if (op2 == 0x38) {
            if (i >= max_len) return out;
            ++i;
            decode_modrm(0);
            return out;
        }
        if (op2 == 0x3A) {
            if (i >= max_len) return out;
            ++i;
            decode_modrm(1);
            return out;
        }
        if (op2 >= 0x80 && op2 <= 0x8F) { // Jcc rel32
            if (i + 4 > max_len) return out;
            out.kind = RelocKind::Rel32;
            out.disp_offset = i;
            out.len = i + 4;
            out.valid = true;
            return out;
        }
        if (op2 == 0x05 || op2 == 0x07 || op2 == 0x0B || op2 == 0x31 || op2 == 0xA0 || op2 == 0xA1 || op2 == 0xA2 ||
            op2 == 0xA8 || op2 == 0xA9 || (op2 >= 0xC8 && op2 <= 0xCF)) {
            out.len = i;
            out.valid = true;
            return out;
        }
        size_t imm = 0;
        if (op2 == 0x70 || op2 == 0x71 || op2 == 0x72 || op2 == 0x73 || op2 == 0xA4 || op2 == 0xAC || op2 == 0xBA ||
            op2 == 0xC2 || op2 == 0xC4 || op2 == 0xC5 || op2 == 0xC6)
            imm = 1;
        decode_modrm(imm);
        return out;
    }

    // 1-byte opcodes without ModR/M
    if ((op >= 0x40 && op <= 0x5F) || op == 0x90 || op == 0x98 || op == 0x99 || op == 0x9C || op == 0x9D ||
        op == 0x9E || op == 0x9F || op == 0xC9 || op == 0xCC || op == 0xF4 || op == 0xF5 || op == 0xF8 || op == 0xF9 ||
        op == 0xFA || op == 0xFB || op == 0xFC || op == 0xFD || (op >= 0xA4 && op <= 0xA7) ||
        (op >= 0xAA && op <= 0xAF)) {
        out.len = i;
        out.valid = true;
        return out;
    }
    if (op == 0xC3 || op == 0xCB) { // ret / retf
        out.terminator = true;
        out.len = i;
        out.valid = true;
        return out;
    }
    if (op == 0xC2 || op == 0xCA) {
        if (i + 2 > max_len) return out;
        out.terminator = true;
        out.len = i + 2;
        out.valid = true;
        return out;
    }
    if (op == 0xE8 || op == 0xE9) { // call/jmp rel32
        if (i + 4 > max_len) return out;
        out.kind = RelocKind::Rel32;
        out.terminator = (op == 0xE9);
        out.disp_offset = i;
        out.len = i + 4;
        out.valid = true;
        return out;
    }
    if (op == 0xEB) { // jmp rel8
        if (i + 1 > max_len) return out;
        out.kind = RelocKind::Rel8Jmp;
        out.terminator = true;
        out.disp_offset = i;
        out.len = i + 1;
        out.valid = true;
        return out;
    }
    if (op >= 0x70 && op <= 0x7F) { // jcc rel8
        if (i + 1 > max_len) return out;
        out.kind = RelocKind::Rel8Jcc;
        out.disp_offset = i;
        out.jcc_cc = op & 0x0Fu;
        out.len = i + 1;
        out.valid = true;
        return out;
    }
    if (op == 0xE0 || op == 0xE1 || op == 0xE2 || op == 0xE3) {
        return out; // loop/jrcxz rel8 unsupported for relocation
    }
    if (op >= 0xB0 && op <= 0xB7) {
        if (i + 1 > max_len) return out;
        out.len = i + 1;
        out.valid = true;
        return out;
    }
    if (op >= 0xB8 && op <= 0xBF) {
        const size_t imm = rex_w ? 8 : (has_66 ? 2 : 4);
        if (i + imm > max_len) return out;
        out.len = i + imm;
        out.valid = true;
        return out;
    }
    if (op == 0xA0 || op == 0xA1 || op == 0xA2 || op == 0xA3) {
        const size_t moffs = has_67 ? 4 : 8;
        if (i + moffs > max_len) return out;
        out.len = i + moffs;
        out.valid = true;
        return out;
    }
    if (op == 0x6A || op == 0xA8 || op == 0xCD || (op >= 0xE4 && op <= 0xE7) ||
        (op & 0xC7u) == 0x04u) { // op al, imm8 (04,0C,14,1C,24,2C,34,3C)
        if (i + 1 > max_len) return out;
        out.len = i + 1;
        out.valid = true;
        return out;
    }
    if (op == 0x68 || op == 0xA9 || (op & 0xC7u) == 0x05u) { // op rax, imm32
        const size_t imm = has_66 ? 2 : 4;
        if (i + imm > max_len) return out;
        out.len = i + imm;
        out.valid = true;
        return out;
    }
    if (op == 0xC8) {
        if (i + 3 > max_len) return out;
        out.len = i + 3;
        out.valid = true;
        return out;
    }

    // 1-byte opcodes with ModR/M
    if (op == 0xF6 || op == 0xF7) {
        if (i >= max_len) return out;
        const uint8_t reg = (p[i] >> 3) & 7u;
        const size_t imm = (reg <= 1u) ? (op == 0xF6 ? 1u : (has_66 ? 2u : 4u)) : 0u;
        decode_modrm(imm);
        return out;
    }
    if (op == 0x6B || op == 0x80 || op == 0x82 || op == 0x83 || op == 0xC0 || op == 0xC1 || op == 0xC6) {
        decode_modrm(1);
        return out;
    }
    if (op == 0x69 || op == 0x81 || op == 0xC7) {
        decode_modrm(has_66 ? 2 : 4);
        return out;
    }
    // Note FF /4 (jmp indirect) / FE / FF / D0..D3 / 84..8F / 63 / 00..3B
    if (op == 0xFF && i < max_len && ((p[i] >> 3) & 7u) == 4u) {
        if (!decode_modrm(0)) return out;
        out.terminator = true;
        return out;
    }
    decode_modrm(0);
    return out;
}

void write_abs_jump(std::vector<uint8_t>& buf, const void* target) {
    buf.push_back(0xFF);
    buf.push_back(0x25);
    buf.push_back(0x00);
    buf.push_back(0x00);
    buf.push_back(0x00);
    buf.push_back(0x00);
    const uint64_t addr = reinterpret_cast<uintptr_t>(target);
    const size_t pos = buf.size();
    buf.resize(pos + 8);
    std::memcpy(buf.data() + pos, &addr, 8);
}

bool emit_trampoline_and_relay(uint8_t* src, size_t stolen_len, bool terminated_early, void* detour,
                               void** out_trampoline, uint8_t** out_relay) {
    struct Step {
        size_t src_off;
        DecodedInstr d;
    };
    std::vector<Step> steps;
    size_t offset = 0;
    while (offset < stolen_len) {
        DecodedInstr d = decode_x64_instr(src + offset, stolen_len - offset);
        if (!d.valid) return false;
        steps.push_back({offset, d});
        offset += d.len;
    }

    size_t est_size = terminated_early ? 0 : 14;
    for (const auto& s : steps) {
        if (s.d.kind == RelocKind::Rel8Jmp)
            est_size += 5;
        else if (s.d.kind == RelocKind::Rel8Jcc)
            est_size += 6;
        else
            est_size += s.d.len;
    }

    uint8_t* mem = alloc_exec_bytes(src, est_size);
    uint8_t* relay = alloc_exec_bytes(src, 14);
    if (!mem || !relay || !in_rel32_range(src + kPatchSize, relay)) return false;

    std::vector<uint8_t> code;
    code.reserve(est_size);
    for (const auto& s : steps) {
        const uint8_t* old_ins = src + s.src_off;
        const size_t dst_off = code.size();
        if (s.d.kind == RelocKind::RipDisp32 || s.d.kind == RelocKind::Rel32) {
            code.insert(code.end(), old_ins, old_ins + s.d.len);
            int32_t old_disp = 0;
            std::memcpy(&old_disp, old_ins + s.d.disp_offset, 4);
            const uint8_t* abs_target = old_ins + s.d.len + old_disp;
            const uint8_t* new_end = mem + dst_off + s.d.len;
            if (!in_rel32_range(new_end, abs_target)) return false;
            const int32_t new_disp =
                int32_t(reinterpret_cast<intptr_t>(abs_target) - reinterpret_cast<intptr_t>(new_end));
            std::memcpy(code.data() + dst_off + s.d.disp_offset, &new_disp, 4);
        } else if (s.d.kind == RelocKind::Rel8Jmp) {
            const int8_t old_disp = static_cast<int8_t>(old_ins[s.d.disp_offset]);
            const uint8_t* abs_target = old_ins + s.d.len + old_disp;
            const uint8_t* new_end = mem + dst_off + 5;
            if (!in_rel32_range(new_end, abs_target)) return false;
            const int32_t new_disp =
                int32_t(reinterpret_cast<intptr_t>(abs_target) - reinterpret_cast<intptr_t>(new_end));
            code.push_back(0xE9);
            const size_t p = code.size();
            code.resize(p + 4);
            std::memcpy(code.data() + p, &new_disp, 4);
        } else if (s.d.kind == RelocKind::Rel8Jcc) {
            const int8_t old_disp = static_cast<int8_t>(old_ins[s.d.disp_offset]);
            const uint8_t* abs_target = old_ins + s.d.len + old_disp;
            const uint8_t* new_end = mem + dst_off + 6;
            if (!in_rel32_range(new_end, abs_target)) return false;
            const int32_t new_disp =
                int32_t(reinterpret_cast<intptr_t>(abs_target) - reinterpret_cast<intptr_t>(new_end));
            code.push_back(0x0F);
            code.push_back(uint8_t(0x80u | s.d.jcc_cc));
            const size_t p = code.size();
            code.resize(p + 4);
            std::memcpy(code.data() + p, &new_disp, 4);
        } else {
            code.insert(code.end(), old_ins, old_ins + s.d.len);
        }
    }
    if (!terminated_early) write_abs_jump(code, src + stolen_len);
    std::memcpy(mem, code.data(), code.size());

    std::vector<uint8_t> relay_code;
    relay_code.reserve(14);
    write_abs_jump(relay_code, detour);
    std::memcpy(relay, relay_code.data(), relay_code.size());

#if !defined(_WIN32)
    __builtin___clear_cache(reinterpret_cast<char*>(mem), reinterpret_cast<char*>(mem + code.size()));
    __builtin___clear_cache(reinterpret_cast<char*>(relay), reinterpret_cast<char*>(relay + relay_code.size()));
#endif

    *out_trampoline = mem;
    *out_relay = relay;
    return true;
}

} // namespace

bool install_native_trampoline(void* target, void* detour, void** out_trampoline, uint64_t* out_id) {
    if (!target || !detour || !out_trampoline || !out_id) return false;
    std::lock_guard lock(s_trampoline_mutex);

    auto* src = static_cast<uint8_t*>(target);
    size_t stolen = 0;
    bool terminated_early = false;
    while (stolen < kPatchSize) {
        const DecodedInstr d = decode_x64_instr(src + stolen, 32 - stolen);
        if (!d.valid) return false;
        stolen += d.len;
        if (d.terminator && stolen < kPatchSize) {
            bool all_pad = true;
            for (size_t k = stolen; k < kPatchSize; ++k) {
                if (src[k] != 0xCCu && src[k] != 0x90u) {
                    all_pad = false;
                    break;
                }
            }
            if (!all_pad) return false;
            terminated_early = true;
            break;
        }
        if (stolen > sizeof(ActiveTrampoline::saved_bytes)) return false;
    }

    const size_t patch_len = std::max(stolen, kPatchSize);
    if (patch_len > sizeof(ActiveTrampoline::saved_bytes)) return false;

    void* trampoline = nullptr;
    uint8_t* relay = nullptr;
    if (!emit_trampoline_and_relay(src, stolen, terminated_early, detour, &trampoline, &relay)) return false;

    ActiveTrampoline record;
    record.id = s_next_trampoline_id++;
    record.target = src;
    record.patch_len = patch_len;
    record.trampoline = trampoline;
    std::memcpy(record.saved_bytes, src, patch_len);

    uint8_t patch[32];
    patch[0] = 0xE9;
    const int32_t rel = int32_t(reinterpret_cast<intptr_t>(relay) - reinterpret_cast<intptr_t>(src + kPatchSize));
    std::memcpy(patch + 1, &rel, 4);
    for (size_t i = kPatchSize; i < patch_len; ++i) patch[i] = 0x90u;

    if (!write_code_bytes(src, patch, patch_len)) return false;

    s_trampolines.push_back(record);
    *out_trampoline = trampoline;
    *out_id = record.id;
    return true;
}

bool remove_native_trampoline(uint64_t id) {
    if (!id) return false;
    std::lock_guard lock(s_trampoline_mutex);
    for (size_t i = 0; i < s_trampolines.size(); ++i) {
        if (s_trampolines[i].id == id) {
            const auto& r = s_trampolines[i];
            if (!write_code_bytes(r.target, r.saved_bytes, r.patch_len)) return false;
            s_trampolines.erase(s_trampolines.begin() + ptrdiff_t(i));
            return true;
        }
    }
    return false;
}

void* create_guest_body_bridge(uint32_t guest_addr, void* orig_trampoline, void* dispatcher) {
    if (!dispatcher) return nullptr;
    std::lock_guard lock(s_trampoline_mutex);
    std::vector<uint8_t> code;
#if defined(_WIN32)
    // Win64: rcx = cpu, rdx = mem -> r8d = guest_addr, r9 = orig_trampoline, jmp dispatcher
    code.push_back(0x41);
    code.push_back(0xB8);
    const size_t off_a = code.size();
    code.resize(off_a + 4);
    std::memcpy(code.data() + off_a, &guest_addr, 4);

    code.push_back(0x49);
    code.push_back(0xB9);
    const uint64_t orig_u64 = reinterpret_cast<uintptr_t>(orig_trampoline);
    const size_t off_o = code.size();
    code.resize(off_o + 8);
    std::memcpy(code.data() + off_o, &orig_u64, 8);
#else
    // SysV: endbr64; rdi = cpu, rsi = mem -> edx = guest_addr, rcx = orig_trampoline, jmp dispatcher
    code.insert(code.end(), {0xF3, 0x0F, 0x1E, 0xFA});
    code.push_back(0xBA);
    const size_t off_a = code.size();
    code.resize(off_a + 4);
    std::memcpy(code.data() + off_a, &guest_addr, 4);

    code.push_back(0x48);
    code.push_back(0xB9);
    const uint64_t orig_u64 = reinterpret_cast<uintptr_t>(orig_trampoline);
    const size_t off_o = code.size();
    code.resize(off_o + 8);
    std::memcpy(code.data() + off_o, &orig_u64, 8);
#endif
    write_abs_jump(code, dispatcher);
    uint8_t* mem = alloc_exec_bytes(nullptr, code.size());
    if (!mem) return nullptr;
    std::memcpy(mem, code.data(), code.size());
#if !defined(_WIN32)
    __builtin___clear_cache(reinterpret_cast<char*>(mem), reinterpret_cast<char*>(mem + code.size()));
#endif
    return mem;
}

void* create_dispatch_bridge(uint32_t guest_addr, void* prev_host_fn, void* dispatcher) {
    if (!dispatcher) return nullptr;
    std::lock_guard lock(s_trampoline_mutex);
    std::vector<uint8_t> code;
#if defined(_WIN32)
    // Win64: rcx = cpu -> edx = guest_addr, r8 = prev_host_fn, jmp dispatcher
    code.push_back(0xBA);
    const size_t off_a = code.size();
    code.resize(off_a + 4);
    std::memcpy(code.data() + off_a, &guest_addr, 4);

    code.push_back(0x49);
    code.push_back(0xB8);
    const uint64_t prev_u64 = reinterpret_cast<uintptr_t>(prev_host_fn);
    const size_t off_p = code.size();
    code.resize(off_p + 8);
    std::memcpy(code.data() + off_p, &prev_u64, 8);
#else
    // SysV: endbr64; rdi = cpu -> esi = guest_addr, rdx = prev_host_fn, jmp dispatcher
    code.insert(code.end(), {0xF3, 0x0F, 0x1E, 0xFA});
    code.push_back(0xBE);
    const size_t off_a = code.size();
    code.resize(off_a + 4);
    std::memcpy(code.data() + off_a, &guest_addr, 4);

    code.push_back(0x48);
    code.push_back(0xBA);
    const uint64_t prev_u64 = reinterpret_cast<uintptr_t>(prev_host_fn);
    const size_t off_p = code.size();
    code.resize(off_p + 8);
    std::memcpy(code.data() + off_p, &prev_u64, 8);
#endif
    write_abs_jump(code, dispatcher);
    uint8_t* mem = alloc_exec_bytes(nullptr, code.size());
    if (!mem) return nullptr;
    std::memcpy(mem, code.data(), code.size());
#if !defined(_WIN32)
    __builtin___clear_cache(reinterpret_cast<char*>(mem), reinterpret_cast<char*>(mem + code.size()));
#endif
    return mem;
}

void* create_guest_orig_stub(uintptr_t cookie, void* orig_dispatcher) {
    if (!orig_dispatcher) return nullptr;
    std::lock_guard lock(s_trampoline_mutex);
    std::vector<uint8_t> code;
#if defined(_WIN32)
    // Win64: rcx = cpu -> rdx = cookie, jmp orig_dispatcher
    code.push_back(0x48);
    code.push_back(0xBA);
#else
    // SysV: endbr64; rdi = cpu -> rsi = cookie, jmp orig_dispatcher
    code.insert(code.end(), {0xF3, 0x0F, 0x1E, 0xFA});
    code.push_back(0x48);
    code.push_back(0xBE);
#endif
    const uint64_t cookie_u64 = uint64_t(cookie);
    const size_t off_c = code.size();
    code.resize(off_c + 8);
    std::memcpy(code.data() + off_c, &cookie_u64, 8);
    write_abs_jump(code, orig_dispatcher);
    uint8_t* mem = alloc_exec_bytes(nullptr, code.size());
    if (!mem) return nullptr;
    std::memcpy(mem, code.data(), code.size());
#if !defined(_WIN32)
    __builtin___clear_cache(reinterpret_cast<char*>(mem), reinterpret_cast<char*>(mem + code.size()));
#endif
    return mem;
}

namespace {
#if defined(_WIN32)
constexpr size_t kJumpStubTargetOffset = 8; // 66 90 (2) + FF 25 00 00 00 00 (6)
#else
constexpr size_t kJumpStubTargetOffset = 16; // F3 0F 1E FA (4) + 66 0F 1F 44 00 00 (6) + FF 25 00 00 00 00 (6)
#endif
} // namespace

void* create_jump_stub(void* initial_target) {
    std::lock_guard lock(s_trampoline_mutex);
    std::vector<uint8_t> code;
#if defined(_WIN32)
    code.insert(code.end(), {0x66, 0x90});
#else
    code.insert(code.end(), {0xF3, 0x0F, 0x1E, 0xFA, 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00});
#endif
    write_abs_jump(code, initial_target);
    uint8_t* mem = alloc_exec_bytes(nullptr, code.size());
    if (!mem) return nullptr;
    std::memcpy(mem, code.data(), code.size());
#if !defined(_WIN32)
    __builtin___clear_cache(reinterpret_cast<char*>(mem), reinterpret_cast<char*>(mem + code.size()));
#endif
    return mem;
}

bool patch_jump_stub_target(void* stub, void* new_target) {
    if (!stub) return false;
    auto* bytes = static_cast<uint8_t*>(stub);
    if ((reinterpret_cast<uintptr_t>(bytes) & 15u) != 0) return false;
    if (bytes[kJumpStubTargetOffset - 6] != 0xFFu || bytes[kJumpStubTargetOffset - 5] != 0x25u) return false;
    std::lock_guard lock(s_trampoline_mutex);
    const uint64_t addr = reinterpret_cast<uintptr_t>(new_target);
    reinterpret_cast<std::atomic<uint64_t>*>(bytes + kJumpStubTargetOffset)->store(addr, std::memory_order_release);
    return true;
}

#else

bool install_native_trampoline(void*, void*, void**, uint64_t*) {
    return false;
}
bool remove_native_trampoline(uint64_t) {
    return false;
}
void* create_guest_body_bridge(uint32_t, void*, void*) {
    return nullptr;
}
void* create_dispatch_bridge(uint32_t, void*, void*) {
    return nullptr;
}
void* create_guest_orig_stub(uintptr_t, void*) {
    return nullptr;
}
void* create_jump_stub(void*) {
    return nullptr;
}
bool patch_jump_stub_target(void*, void*) {
    return false;
}

#endif

} // namespace rbrew_mod
