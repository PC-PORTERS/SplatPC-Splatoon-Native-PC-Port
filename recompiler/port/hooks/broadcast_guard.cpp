// broadcast_guard.cpp - message broadcast with a loop guard (Splatoon v288).
//
// Touching the GamePad screen froze the game: its main thread never left
// 0285C3D0, the game's message broadcast (a group list of receivers, each with
// a list of handlers called through a pointer-to-member). A memory snapshot showed sane
// lists at that moment, so the walk does not end because the lists change while it runs.
//
// The recompiler wraps 0285C3D0 (`[hooks]` of the manifest) and this hook runs
// the same walk natively, step for step with the guest code:
//   0285BD54(obj, -1); group = [key]; head H = obj+0x34+16*group, node offset [H+0xC];
//   for each receiver e: [obj+0x218] = 0; its handler list H2 = [[[obj+0x30]+4*[e+8]]+8];
//   for each handler x: [obj+0x218] = ++n; if [key] == 5 or [x+0x1C] == 2, call
//   the member (msg: s16 this delta, s16 vtable index (<0: plain function), u32 function/offset);
//   then [obj+0x218] = -1; 0285BD54(obj, -1).
// It adds a step limit: past it the walk stops, the details go to stderr once
// per call site, and the game goes on (one message not delivered to everyone).
// RBREW_BROADCAST_GUARD=0 runs the original code instead.
#include "../runtime/rebrewu_runtime.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if RBREW_GAME_REVISION == 288
namespace {
constexpr uint32_t kBroadcast = RBREW_GAME_BROADCAST_DISPATCH;
constexpr uint32_t kSetCurrent = RBREW_GAME_BROADCAST_SET_CURRENT; // called with (obj, -1) before and after the walk
constexpr uint32_t kMaxSteps = 50000; // receivers + handler calls; real broadcasts are far below

uint32_t rd(CPUState* cpu, uint32_t a) {
    return rbrew_read32(cpu->mem, a);
}
void wr(CPUState* cpu, uint32_t a, uint32_t v) {
    rbrew_write32(cpu->mem, a, v);
}
int32_t rh(CPUState* cpu, uint32_t a) {
    return int16_t(rbrew_read16(cpu->mem, a));
}
bool ok(CPUState* cpu, uint32_t a, uint32_t n) {
    return a && uint64_t(a) + n <= cpu->guest_memory_size && rbrew_memory_accessible(cpu->mem, a, n);
}
void call(CPUState* cpu, uint32_t fn) {
    rbrew_call(cpu, fn);
}

void report(CPUState* cpu, uint32_t caller, uint32_t obj, uint32_t key, uint32_t msg, uint32_t head, uint32_t inner,
            uint32_t offset, uint32_t x) {
    static uint32_t s_reported = 0;
    if (s_reported >= 8) return;
    ++s_reported;
    std::string handlers;
    uint32_t node = rd(cpu, inner + 4);
    for (int i = 0; i < 16 && node && node != inner && ok(cpu, node, 8); ++i) {
        const uint32_t h = node - offset;
        char item[64];
        std::snprintf(item, sizeof item, " %08X(vt %08X flag %u)", h, ok(cpu, h, 0x20) ? rd(cpu, h + 4) : 0,
                      ok(cpu, h, 0x20) ? rd(cpu, h + 0x1C) : 0);
        handlers += item;
        node = rd(cpu, node + 4);
    }
    RBREW_LOG_WARN("broadcast",
                   "loop stopped after %u steps: caller lr=%08X obj=%08X key=%u msg=%08X/%08X/%08X group head=%08X "
                   "handler head=%08X offset=%X at=%08X handlers:%s%s\n",
                   kMaxSteps, caller, obj, rd(cpu, key), rd(cpu, msg), rd(cpu, msg + 4), rd(cpu, msg + 8), head, inner,
                   offset, x, handlers.c_str(), node == inner ? " (end)" : " ...");
}

void broadcast(CPUState* cpu, uint32_t, HostFunc original) {
    const uint32_t obj = cpu->r[3], key = cpu->r[4], msg = cpu->r[5], caller = cpu->lr;
    if (!ok(cpu, obj, 0x21C) || !ok(cpu, key, 4) || !ok(cpu, msg, 8) || rd(cpu, key) > 0x1000) {
        original(cpu);
        return;
    }
    // A frame of our own, like the guest's (stwu r1,-0x40): callees save LR at 4(r1).
    const uint32_t sp = cpu->r[1], lr = cpu->lr;
    cpu->r[1] = sp - 0x40;
    wr(cpu, cpu->r[1], sp);
    auto set_current = [&] {
        cpu->r[3] = obj;
        cpu->r[4] = 0xFFFFFFFFu;
        call(cpu, kSetCurrent);
    };
    set_current();
    const uint32_t head = obj + 0x34 + rd(cpu, key) * 16;
    const uint32_t offset = rd(cpu, head + 0xC);
    uint32_t e = rd(cpu, head + 4) - offset, steps = 0;
    while (e != head - rd(cpu, head + 0xC)) {
        wr(cpu, obj + 0x218, 0);
        const uint32_t table = rd(cpu, obj + 0x30);
        const uint32_t list = rd(cpu, table + 4 * rd(cpu, e + 8));
        const uint32_t inner = rd(cpu, list + 8), o2 = rd(cpu, inner + 0xC), end2 = inner - o2;
        uint32_t x = rd(cpu, inner + 4) - o2;
        if (x != end2) {
            const int32_t delta = rh(cpu, msg), index = rh(cpu, msg + 2);
            const uint32_t function = rd(cpu, msg + 4);
            uint32_t n = 0;
            for (;;) {
                ++n;
                wr(cpu, obj + 0x218, n);
                if (rd(cpu, key) == 5 || rd(cpu, x + 0x1C) == 2) {
                    const uint32_t self = x + uint32_t(delta);
                    uint32_t target, r5;
                    if (index < 0) {
                        target = function;
                        r5 = n - 1;
                    } else {
                        r5 = rd(cpu, self + uint32_t(int32_t(int16_t(function)))) + uint32_t(index) * 8;
                        target = rd(cpu, r5 + 4);
                    }
                    cpu->r[3] = self;
                    cpu->r[4] = n;
                    cpu->r[5] = r5;
                    call(cpu, target);
                }
                if (++steps > kMaxSteps) {
                    report(cpu, caller, obj, key, msg, head, inner, o2, x);
                    goto done;
                }
                x = rd(cpu, x + o2 + 4) - o2;
                if (x == end2) break;
                n = rd(cpu, obj + 0x218);
            }
        }
        if (++steps > kMaxSteps) {
            report(cpu, caller, obj, key, msg, head, 0, 0, e);
            break;
        }
        e = rd(cpu, e + offset + 4) - offset;
    }
done:
    wr(cpu, obj + 0x218, 0xFFFFFFFFu);
    set_current();
    cpu->r[1] = sp;
    cpu->lr = lr;
}
} // namespace

void broadcast_guard_install() {
    if (const char* v = std::getenv("RBREW_BROADCAST_GUARD"); v && std::strcmp(v, "0") == 0) return;
    if (!rbrew_set_guest_hook(kBroadcast, broadcast)) RBREW_LOG_WARN("broadcast", "could not register the hook\n");
}
#else
void broadcast_guard_install() {}
#endif
