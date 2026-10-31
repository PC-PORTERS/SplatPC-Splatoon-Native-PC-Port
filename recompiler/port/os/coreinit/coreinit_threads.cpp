#include "../../runtime/rbrew_diagnostics.h"
// coreinit thread management
//
// Design:
//   Each OSCreateThread call allocates a GuestThread on the heap.  The struct
//   holds an independent CPUState (with shared mem / dispatch_table pointers)
//   plus a std::thread that is started by OSResumeThread.
//
//   Thread-local state:
//     t_osthread_addr   — guest OSThread* for the calling host thread
//     t_thread_specific — 16-slot per-thread storage (OSGetThreadSpecific)
//     t_in_guest_thread — true only for worker threads (enables OSExitThread throw)
//
//   OSExitThread is implemented by throwing GuestThreadExit{}; the thread
//   lambda catches this and lets the thread function cleanly return.
//
//   GPU safety: the main thread creates the SDL GPU context.  Game rendering
//   typically runs on the same thread that calls GX2Init.  If a worker thread
//   calls GX2, it will fail silently (no MakeCurrent here).

#include "../os_common.h"
#include "../../runtime/host_sleep.h"
#include <array>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <memory>
#include <stdexcept>
#if defined(_WIN32)
extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
static unsigned long host_thread_id() {
    return GetCurrentThreadId();
}
#else
static unsigned long host_thread_id() {
    return 0;
}
#endif

// ---------------------------------------------------------------------------
// Thread-local identity
// ---------------------------------------------------------------------------
static constexpr uint32_t MAIN_THREAD_ADDR = 0x50000010u;

static thread_local uint32_t t_osthread_addr = 0; // 0 → main thread
static thread_local uint32_t t_thread_specific[16] = {};
static thread_local bool t_in_guest_thread = false;

// Exception used to cleanly exit from a guest thread (OSExitThread).
struct GuestThreadExit { uint32_t value; };

// ---------------------------------------------------------------------------
// GuestThread — one per OSCreateThread call
// ---------------------------------------------------------------------------
struct GuestThread {
    uint32_t osthread_addr = 0;
    uint32_t entry_addr = 0;
    CPUState cpu; // own register file; shares mem/dispatch
    std::thread host_thread;
    std::atomic<bool> started{false}, finished{false};
    std::mutex done_mutex;
    std::condition_variable done;
    uint32_t exit_value=0;
    std::atomic<uint32_t> affinity{2}; // OSThread attr bits 0-2 (core 0/1/2)
    std::atomic<uint32_t> core{1};     // core this thread runs on (OSGetCoreId)

    GuestThread() { memset(&cpu, 0, sizeof(cpu)); }
    GuestThread(const GuestThread&) = delete;
    GuestThread& operator=(const GuestThread&) = delete;
};

static std::mutex                              s_threads_mutex;
static std::unordered_map<uint32_t, std::shared_ptr<GuestThread>> s_threads; // osthread_addr → GuestThread*
static std::atomic<uint32_t> s_main_affinity{2}; // main thread: core 1
static std::unordered_map<uint32_t, uint32_t> s_pending_affinity; // set before OSCreateThread (s_threads_mutex)
static constexpr size_t kMaxPendingAffinity=1024; // reject new requests once full; never claim a discarded affinity succeeded

// Pick the core for an affinity set: stay on the preferred core when the set
// allows it, otherwise use the lowest allowed core; an empty set keeps it.
static uint32_t pick_core(uint32_t affinity, uint32_t preferred) {
    affinity &= 7u;
    if (!affinity || (affinity & (1u << preferred))) return preferred;
    return affinity & 1u ? 0u : affinity & 2u ? 1u : 2u;
}
static thread_local GuestThread* t_guest_thread = nullptr;
static uint32_t current_affinity() {
    return t_guest_thread ? t_guest_thread->affinity.load() : rbrew_system_thread ? (1u<<rbrew_current_core()) : s_main_affinity.load();
}

// ---------------------------------------------------------------------------
// OSCreateThread
// r3=OSThread*, r4=entry, r5=intArg(→r3), r6=ptrArg(→r4),
// r7=stackTop, r8=stackSize, r9=priority, r10=attr
// ---------------------------------------------------------------------------
static void OSCreateThread(CPUState* cpu) {
    uint32_t thread_addr = ARG0;
    uint32_t entry_addr = ARG1;
    uint32_t int_arg = ARG2;
    uint32_t ptr_arg = ARG3;
    uint32_t stack_top = ARG4; // r7 = high end of stack

    RBREW_LOG_INFO("coreinit", "OSCreateThread(thread=0x%08X entry=0x%08X)\n", thread_addr, entry_addr);

    if (!thread_addr || !entry_addr) {
        RET = 0;
        return;
    }

    auto gt = std::make_shared<GuestThread>();
    gt->osthread_addr = thread_addr;
    gt->entry_addr = entry_addr;

    // Share mem and dispatch table with the caller
    gt->cpu.mem = cpu->mem;
    gt->cpu.dispatch_table = cpu->dispatch_table;
    gt->cpu.dispatch_size = cpu->dispatch_size;
    gt->cpu.guest_memory_size = cpu->guest_memory_size;
#ifdef RBREW_ENABLE_PPC_FALLBACK
    gt->cpu.guest_code_ranges = cpu->guest_code_ranges;
    gt->cpu.guest_code_range_count = cpu->guest_code_range_count;
    gt->cpu.ppc_fallback_enabled = cpu->ppc_fallback_enabled;
    gt->cpu.ppc_instruction_budget = cpu->ppc_instruction_budget;
#endif

    // Initial register state
    gt->cpu.r[1] = stack_top ? (stack_top - 8u) : 0x1FBFFFE8u; // sp
    // Cafe OSInitContext supplies the application's SDA2/SDA bases. This
    // native process runs one generated guest module, so the creator carries
    // those module bases. Leaving them zero breaks SDA-relative guest loads.
    gt->cpu.r[2] = cpu->r[2];
    gt->cpu.r[13] = cpu->r[13];
    gt->cpu.r[3] = int_arg;
    gt->cpu.r[4] = ptr_arg;
    // Affinity: attribute bits 0-2; none given inherits the creator's (decaf).
    uint32_t affinity = ARG7 & 7u;
    if (!affinity) affinity = current_affinity();
    gt->affinity = affinity;
    gt->core = pick_core(affinity, rbrew_current_core());
    rbrew_init_thread_gqrs(&gt->cpu); // decaf OSCreateThread GQR2-5

    // Write READY state into guest OSThread struct (+0x10)
    if (rbrew_memory_accessible(cpu->mem, thread_addr, 0x20u))
        rbrew_write32(cpu->mem, thread_addr + 0x10u, 1u); // OS_THREAD_STATE_READY

    std::lock_guard<std::mutex> lk(s_threads_mutex);
    if (auto pending = s_pending_affinity.find(thread_addr); pending != s_pending_affinity.end()) {
        if (!(ARG7 & 7u) && pending->second) { gt->affinity = pending->second; gt->core = pick_core(pending->second, rbrew_current_core()); }
        s_pending_affinity.erase(pending);
    }
    // Running workers and joiners retain the old object; no raw retired list is needed.
    s_threads[thread_addr] = gt;
    RET = 1; // TRUE
}

// ---------------------------------------------------------------------------
// OSResumeThread — kick off the thread
// ---------------------------------------------------------------------------
static void OSResumeThread(CPUState* cpu) {
    uint32_t thread_addr = ARG0;

    std::lock_guard<std::mutex> lk(s_threads_mutex);
    auto it = s_threads.find(thread_addr);
    if (it == s_threads.end()) {
        RET = -1;
        return;
    }

    auto gt = it->second;
    if (gt->started.exchange(true)) { RET = 0; return; } // already running

    // Mark RUNNING in guest struct
    if (rbrew_memory_accessible(gt->cpu.mem, thread_addr, 0x20u))
        rbrew_write32(gt->cpu.mem, thread_addr + 0x10u, 2u); // OS_THREAD_STATE_RUNNING

    gt->host_thread = std::thread([gt]() {
        t_osthread_addr = gt->osthread_addr;
        t_in_guest_thread = true;
        t_guest_thread    = gt.get();
        rbrew_thread_core_slot = &gt->core;
        memset(t_thread_specific, 0, sizeof(t_thread_specific));

        RBREW_LOG_INFO("coreinit", "thread 0x%08X started (entry=0x%08X host_tid=%lu cpu=%p)\n", gt->osthread_addr,
                       gt->entry_addr, host_thread_id(), (void*)&gt->cpu);
        uint32_t exit_value=0;
        try { rbrew_dispatch(&gt->cpu,gt->entry_addr);exit_value=gt->cpu.r[3]; }
        catch(const GuestThreadExit& e){exit_value=e.value;}
        catch(const std::exception& e){RBREW_LOG_WARN("thread", "%08X failed: %s\n", gt->osthread_addr, e.what());exit_value=~0u;}
        catch(...){RBREW_LOG_WARN("thread", "%08X failed\n", gt->osthread_addr);exit_value=~0u;}
        {
            std::lock_guard registry(s_threads_mutex);
            auto current=s_threads.find(gt->osthread_addr);
            if(current!=s_threads.end() && current->second.get()==gt.get() &&
               rbrew_memory_accessible(gt->cpu.mem,gt->osthread_addr,0x20u))
                rbrew_write32(gt->cpu.mem,gt->osthread_addr+0x10u,8u);
        }
        { std::lock_guard done(gt->done_mutex);gt->exit_value=exit_value;gt->finished.store(true,std::memory_order_release); }
        gt->done.notify_all();
        rbrew_thread_core_slot=nullptr;t_guest_thread=nullptr;t_in_guest_thread=false;
    });
    gt->host_thread.detach(); // completion is joined through done, never racing std::thread::join/detach
    RET = 1; // previous suspend count
}

// ---------------------------------------------------------------------------
// OSJoinThread — block until the thread finishes
// r3=OSThread*, r4=optional exit value out ptr
// ---------------------------------------------------------------------------
static void OSJoinThread(CPUState* cpu) {
    const uint32_t address=ARG0,out=ARG1;
    std::shared_ptr<GuestThread> gt;
    { std::lock_guard lock(s_threads_mutex);auto it=s_threads.find(address);if(it!=s_threads.end())gt=it->second; }
    if(!gt || gt.get()==t_guest_thread || !gt->started || (out && !rbrew_memory_accessible(MEM,out,4))) {RET=0;return;}
    std::unique_lock done(gt->done_mutex);
    gt->done.wait(done,[&]{return gt->finished.load(std::memory_order_acquire);});
    if(out)rbrew_write32(MEM,out,gt->exit_value);
    done.unlock();
    { std::lock_guard registry(s_threads_mutex);auto it=s_threads.find(address);if(it!=s_threads.end() && it->second==gt)s_threads.erase(it); }
    RET=1;
}
// ---------------------------------------------------------------------------
// OSExitThread — gracefully terminate the current thread
// Implemented as a C++ throw so the stack unwinds cleanly back to the
// thread lambda in OSResumeThread, which catches GuestThreadExit.
// For the main thread (t_in_guest_thread == false) this is a no-op.
// ---------------------------------------------------------------------------
void coreinit_exit_thread(CPUState* cpu) {
    if (t_in_guest_thread) throw GuestThreadExit{cpu->r[3]};
}

// ---------------------------------------------------------------------------
// OSGetCurrentThread
// ---------------------------------------------------------------------------
static void OSGetCurrentThread(CPUState* cpu) {
    RET = rbrew_system_thread ? rbrew_system_thread : (t_osthread_addr ? t_osthread_addr : MAIN_THREAD_ADDR);
}

// ---------------------------------------------------------------------------
// Thread-specific storage (16 slots; thread_local)
// ---------------------------------------------------------------------------
static void OSGetThreadSpecific(CPUState* cpu) {
    uint32_t slot = ARG0;
    RET = (slot < 16) ? t_thread_specific[slot] : 0u;
}
static void OSSetThreadSpecific(CPUState* cpu) {
    uint32_t slot = ARG0;
    if (slot < 16) t_thread_specific[slot] = ARG1;
}

// ---------------------------------------------------------------------------
// Misc thread operations
// ---------------------------------------------------------------------------
static void OSSetThreadName(CPUState* cpu) {
    (void)cpu;
}
// OSSetThreadAffinity(thread, affinity): the thread moves to an allowed core
// (staying put when its current core is still allowed). Only the main
// thread's own OSThread moves the main thread. sead and the startup code set the affinity
// of OSThreads not created yet; that is kept for their OSCreateThread.
// Applied to the main thread instead, it moved it between cores while it held
// a per-core mutex (029BC018), and a render worker of the old core waited
// forever: the game froze during startup.
static void OSSetThreadAffinity(CPUState* cpu) {
    const uint32_t thread_addr=ARG0,affinity=ARG1&7u;
    if(!affinity){RET=0;return;}
    if(thread_addr==rbrew_system_thread && rbrew_thread_core_slot) {
        *rbrew_thread_core_slot=pick_core(affinity,rbrew_current_core());RET=1;return;
    }
    if(thread_addr==MAIN_THREAD_ADDR) {
        s_main_affinity=affinity;rbrew_main_thread_core=pick_core(affinity,rbrew_main_thread_core.load());RET=1;return;
    }
    if(!thread_addr || uint64_t(thread_addr)+0x20>cpu->guest_memory_size || !rbrew_memory_accessible(MEM,thread_addr,0x20)){RET=0;return;}
    std::lock_guard lock(s_threads_mutex);
    const auto it=s_threads.find(thread_addr);
    if(it!=s_threads.end() && !it->second->finished.load(std::memory_order_acquire)) {
        it->second->affinity=affinity;it->second->core=pick_core(affinity,it->second->core.load());
    } else {
        if(!s_pending_affinity.contains(thread_addr) && s_pending_affinity.size()>=kMaxPendingAffinity){RET=0;return;}
        s_pending_affinity[thread_addr]=affinity;
    }
    RET=1;
}
static void OSSetThreadPriority(CPUState* cpu) {
    RET = 1;
}
static void OSGetThreadPriority(CPUState* cpu) {
    RET = 16;
}
static void OSInitThreadQueue(CPUState* cpu) {
    if (ARG0 && rbrew_memory_accessible(MEM, ARG0, 8u)) memset(MEM + ARG0, 0, 8);
}

// RBREW_SLEEP_TRACE=<file>: total guest sleep time per guest call chain (the
// return addresses of the four innermost frames), rewritten every 2 s.
static void trace_sleep(CPUState* cpu, uint64_t us) {
    static const char* const path = std::getenv("RBREW_SLEEP_TRACE");
    if (!path || !*path) return;
    std::array<uint32_t, 4> chain{cpu->lr};
    for (uint32_t i = 1, sp = cpu->r[1]; i < chain.size(); ++i) {
        if (!rbrew_memory_accessible(MEM, sp, 4)) break;
        sp = rbrew_read32(MEM, sp); // back chain
        if (!sp || !rbrew_memory_accessible(MEM, sp + 4, 4)) break;
        chain[i] = rbrew_read32(MEM, sp + 4); // saved LR
    }
    struct Hash {
        size_t operator()(const std::array<uint32_t, 4>& c) const {
            return c[0] ^ c[1] * 31 ^ c[2] * 131 ^ c[3] * 1031;
        }
    };
    static std::mutex lock;
    struct Total {
        uint64_t calls = 0, us = 0;
        double first_s = 0, last_s = 0;
    };
    static std::unordered_map<std::array<uint32_t, 4>, Total, Hash> totals;
    static const auto start = std::chrono::steady_clock::now();
    static auto last_dump = start;
    std::lock_guard guard(lock);
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - start).count();
    auto& total = totals[chain];
    if (!total.calls++) total.first_s = seconds;
    total.us += us;
    total.last_s = seconds;
    if (now - last_dump < std::chrono::seconds(2)) return;
    last_dump = now;
    if (FILE* f = std::fopen(path, "w")) {
        std::fprintf(f, "calls total_ms first_s last_s lr0 lr1 lr2 lr3\n");
        for (const auto& [c, t] : totals)
            std::fprintf(f, "%llu %.1f %.1f %.1f %08X %08X %08X %08X\n", (unsigned long long)t.calls, t.us / 1000.0,
                         t.first_s, t.last_s, c[0], c[1], c[2], c[3]);
        std::fclose(f);
    }
}

static void OSSleepTicks(CPUState* cpu) {
    // Ticks of the OS timer (bus clock / 4); the full duration is slept.
    uint64_t ticks = ((uint64_t)ARG0 << 32) | (uint64_t)ARG1;
    uint64_t us = ticks / RBREW_TIMER_CLOCK * 1000000ULL + ticks % RBREW_TIMER_CLOCK * 1000000ULL / RBREW_TIMER_CLOCK;
    trace_sleep(cpu, us);
    if (us > 0) rbrew_host_sleep_for(us);
}
static void OSYieldThread(CPUState* cpu) {
    (void)cpu;
    std::this_thread::yield();
}
// coreinit.rpl 0201FC1C / 0201FC38: three big-endian arrival words and a
// self pointer, exactly 0x10 bytes. Arrival stays published until reinit.
// The native condition variable supplies the original release/acquire barrier
// without consuming a host core while another guest core initializes a library.
struct RendezvousImpl {
    std::mutex mutex;
    std::condition_variable arrived;
};
static std::mutex s_rendezvous_map_mutex;
static std::unordered_map<const uint8_t*, std::unique_ptr<RendezvousImpl>> s_rendezvous;
static RendezvousImpl& get_rendezvous(CPUState* cpu, uint32_t address) {
    if (!address || uint64_t(address) + 0x10 > cpu->guest_memory_size || !rbrew_memory_accessible(MEM, address, 0x10))
        throw std::runtime_error("Cafe rendezvous lies outside accessible guest memory");
    std::lock_guard lock(s_rendezvous_map_mutex);
    auto& item = s_rendezvous[MEM + address];
    if (!item) item = std::make_unique<RendezvousImpl>();
    return *item;
}
static void OSInitRendezvous(CPUState* cpu) {
    const uint32_t address = ARG0;
    auto& rendezvous = get_rendezvous(cpu, address);
    {
        std::lock_guard lock(rendezvous.mutex);
        for (unsigned core = 0; core < 3; ++core) rbrew_write32(MEM, address + core * 4, 0);
        rbrew_write32(MEM, address + 12, address);
    }
    rendezvous.arrived.notify_all();
}
static void OSWaitRendezvous(CPUState* cpu) {
    const uint32_t address = ARG0, mask = ARG1 & 7, core = rbrew_current_core();
    if (core >= 3) throw std::runtime_error("Cafe rendezvous has an invalid caller core");
    auto& rendezvous = get_rendezvous(cpu, address);
    std::unique_lock lock(rendezvous.mutex);
    rbrew_write32(MEM, address + core * 4, 1);
    rendezvous.arrived.notify_all();
    rendezvous.arrived.wait(lock, [&] {
        for (unsigned participant = 0; participant < 3; ++participant)
            if ((mask & (1u << participant)) && rbrew_read32(MEM, address + participant * 4) != 1) return false;
        return true;
    });
    RET = 1;
}
static void OSWaitEventWithTimeout(CPUState* cpu) {
    (void)cpu;
    RET = 0;
}
static void OSDriver_Register(CPUState* cpu) {
    (void)cpu;
    RET = 0;
}
// OSWaitEventWithTimeout lives with the other events (coreinit_sync.cpp).

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
// Diagnostic for a frozen game (main.cpp, RBREW_HANG_SAMPLE=freeze): guest
// LR, SP, r3 and the saved-LR chain of every guest thread. PPC frames start
// with the back chain at [sp] and the caller saves LR at [back chain + 4].
// Read-only; values written by a running thread may be a little stale.
static void dump_guest_stack(FILE* out, const char* name, uint32_t id, const CPUState* cpu) {
    std::fprintf(out, "[stack] %s %08X lr=%08X sp=%08X r3=%08X r4=%08X r30=%08X r31=%08X chain:", name, id,
                 cpu->lr, cpu->r[1], cpu->r[3], cpu->r[4], cpu->r[30], cpu->r[31]);
    uint32_t sp = cpu->r[1];
    for (int depth = 0; depth < 16 && sp && rbrew_memory_accessible(cpu->mem, sp, 4); ++depth) {
        const uint32_t back = rbrew_read32(cpu->mem, sp);
        if (back <= sp || !rbrew_memory_accessible(cpu->mem, back + 4, 4)) break;
        std::fprintf(out, " %08X", rbrew_read32(cpu->mem, back + 4));
        sp = back;
    }
    std::fprintf(out, "\n");
}
void rbrew_dump_guest_stacks(FILE* out, const CPUState* main_cpu) {
    if (main_cpu) dump_guest_stack(out, "main", 0, main_cpu);
    std::lock_guard<std::mutex> lock(s_threads_mutex);
    for (const auto& [address, thread] : s_threads)
        if (thread && thread->started && !thread->finished) dump_guest_stack(out, "thread", address, &thread->cpu);
    std::fflush(out);
}

#include "coreinit_addrs.h"
void coreinit_threads_register(CPUState* cpu) {
#define REG(name) rbrew_register_func(cpu, ADDR_##name, name)
    REG(OSCreateThread);
    REG(OSJoinThread);
    REG(OSResumeThread);
    REG(OSYieldThread);
    REG(OSGetCurrentThread);
    REG(OSSetThreadName);
    REG(OSSetThreadAffinity);
    REG(OSSetThreadPriority);
    REG(OSGetThreadPriority);
    REG(OSGetThreadSpecific);
    REG(OSSetThreadSpecific);
    REG(OSInitThreadQueue);
    REG(OSSleepTicks);
    REG(OSWaitRendezvous);
    REG(OSInitRendezvous);
    REG(OSDriver_Register);
    // OSExitThread is registered in coreinit_misc.cpp via coreinit_exit_thread()
#undef REG
}
