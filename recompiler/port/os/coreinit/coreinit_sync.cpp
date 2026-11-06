#include "coreinit_sync_helpers.h"
// coreinit synchronization primitives
//
// Each guest sync object is backed by a host struct kept in a hash-map keyed
// by its guest address.  Using host condition variables means worker threads
// block without busy-spinning.
//
// OSEvent  — signaled/non-signaled flag with blocking wait
// OSSemaphore — counting semaphore with blocking wait
// OSCond   — host std::condition_variable (per guest address)
// OSMutex  — host std::mutex (per guest address, recursive-safe wrapper)
// OSFastMutex — mapped to the same OSMutex table for simplicity

#include "../os_common.h"
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <array>
#include <memory>

// Objects have process lifetime and are never replaced or erased. Cache their
// stable addresses per host thread; each object's mutex/CV still enforces guest
// synchronization. Misses retain the registry lock for concurrent initialization.
template <class T>
static T* sync_object(uint32_t address, std::mutex& registry_lock, std::unordered_map<uint32_t, T*>& registry) {
    struct Entry {
        uint32_t address = 0;
        T* object = nullptr;
    };
    thread_local std::array<Entry, 64> recent{};
    auto& entry = recent[((address >> 2) ^ (address >> 10)) & (recent.size() - 1)];
    if (entry.object && entry.address == address) return entry.object;
    std::lock_guard lock(registry_lock);
    auto it = registry.find(address);
    T* object;
    if (it != registry.end())
        object = it->second;
    else {
        auto owned = std::make_unique<T>();
        registry.emplace(address, owned.get());
        object = owned.release();
    }
    entry = {address, object};
    return object;
}

// ---------------------------------------------------------------------------
// OSMutex — per-address recursive mutex
// (Wii U OSMutex is non-recursive but games sometimes lock the same mutex
//  from the same thread; use a recursive_mutex to avoid deadlocks.)
// ---------------------------------------------------------------------------
struct MutexImpl {
    std::recursive_mutex m;
    WatchedOwnerStack owner_stack; // RBREW_MUTEX_WATCH: guest LRs of the last locker
};
static std::mutex s_mutex_map_lock;
static std::unordered_map<uint32_t, MutexImpl*> s_mutexes;

static MutexImpl* get_mutex(uint32_t ptr) {
    return sync_object(ptr, s_mutex_map_lock, s_mutexes);
}

// RBREW_MUTEX_WATCH=1 (diagnostic): a lock still waiting after 3 s prints the
// mutex, the waiter's and the last locker's guest return addresses to stderr.
static const bool s_mutex_watch = [] {
    const char* v = std::getenv("RBREW_MUTEX_WATCH");
    return v && *v == '1';
}();
static void guest_stack(CPUState* cpu, uint32_t* out, int n) {
    out[0] = cpu->lr;
    uint32_t frame = cpu->r[1];
    for (int i = 1; i < n; ++i) {
        out[i] = 0;
        if (!frame || !rbrew_memory_accessible(MEM, frame, 4u)) continue;
        frame = rbrew_read32(MEM, frame);
        if (frame && rbrew_memory_accessible(MEM, frame + 4, 4u)) out[i] = rbrew_read32(MEM, frame + 4);
    }
}
static void lock_watched(CPUState* cpu, MutexImpl* md, uint32_t address) {
    if (!md->m.try_lock()) {
        const auto start = std::chrono::steady_clock::now();
        bool reported = false;
        while (!md->m.try_lock()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (!reported && std::chrono::steady_clock::now() - start > std::chrono::seconds(3)) {
                reported = true;
                uint32_t mine[6];
                guest_stack(cpu, mine, 6);
                const auto owner = md->owner_stack.snapshot();
                RBREW_LOG_WARN("mutex",
                               "%08X blocked 3s; waiter %08X %08X %08X %08X %08X %08X; last locker %08X %08X %08X %08X "
                               "%08X %08X\n",
                               address, mine[0], mine[1], mine[2], mine[3], mine[4], mine[5], owner[0], owner[1],
                               owner[2], owner[3], owner[4], owner[5]);
            }
        }
    }
    uint32_t owner[6];
    guest_stack(cpu, owner, 6);
    md->owner_stack.store(owner);
}
static void lock_mutex(CPUState* cpu, uint32_t address) {
    MutexImpl* md = get_mutex(address);
    if (s_mutex_watch)
        lock_watched(cpu, md, address);
    else
        md->m.lock();
}

static void OSInitMutex(CPUState* cpu) {
    get_mutex(ARG0);
}
static void OSLockMutex(CPUState* cpu) {
    lock_mutex(cpu, ARG0);
}
static void OSUnlockMutex(CPUState* cpu) {
    get_mutex(ARG0)->m.unlock();
}
static void OSTryLockMutex(CPUState* cpu) {
    RET = get_mutex(ARG0)->m.try_lock() ? 1u : 0u;
}

// OSFastMutex — same backing as OSMutex
static void OSFastMutex_Init(CPUState* cpu) {
    get_mutex(ARG0);
    if (ARG0 && rbrew_memory_accessible(MEM, ARG0, 0x20u)) memset(MEM + ARG0, 0, 0x20);
}
static void OSFastMutex_Lock(CPUState* cpu) {
    lock_mutex(cpu, ARG0);
}
static void OSFastMutex_Unlock(CPUState* cpu) {
    get_mutex(ARG0)->m.unlock();
}
static void OSFastMutex_TryLock(CPUState* cpu) {
    RET = get_mutex(ARG0)->m.try_lock() ? 1u : 0u;
}

// ---------------------------------------------------------------------------
// OSCond — per-address condition variable, paired with a recursive_mutex
// ---------------------------------------------------------------------------
struct CondImpl {
    std::mutex mtx; // separate lock for the CV
    std::condition_variable cv;
};
static std::mutex s_cond_map_lock;
static std::unordered_map<uint32_t, CondImpl*> s_condvars;

static CondImpl* get_cond(uint32_t ptr) {
    return sync_object(ptr, s_cond_map_lock, s_condvars);
}

static void OSInitCond(CPUState* cpu) {
    get_cond(ARG0);
}
// Taking the CV lock orders a signal after a waiter that already released the
// guest mutex has started waiting; without it that wakeup could be lost.
static void OSSignalCond(CPUState* cpu) {
    CondImpl* cd = get_cond(ARG0);
    { std::lock_guard<std::mutex> lk(cd->mtx); }
    cd->cv.notify_one();
}
static void OSBroadcastCond(CPUState* cpu) {
    CondImpl* cd = get_cond(ARG0);
    { std::lock_guard<std::mutex> lk(cd->mtx); }
    cd->cv.notify_all();
}

// OSWaitCond(cond, mutex)
// The game calls this while holding OSMutex (ARG1).  We must unlock it,
// wait, then relock — matching POSIX pthread_cond_wait semantics.
static void OSWaitCond(CPUState* cpu) {
    uint32_t cond_ptr = ARG0;
    uint32_t mutex_ptr = ARG1;

    CondImpl* cd = get_cond(cond_ptr);
    MutexImpl* md = get_mutex(mutex_ptr);

    // Unlock the game mutex, wait on our CV, then relock.
    std::unique_lock<std::mutex> lk(cd->mtx);
    md->m.unlock();
    cd->cv.wait(lk);
    lk.unlock(); // a signaler may hold the guest mutex while taking cd->mtx
    md->m.lock();
}

// ---------------------------------------------------------------------------
// OSSemaphore — counting semaphore with blocking wait
// ---------------------------------------------------------------------------
struct SemImpl {
    std::mutex mtx;
    std::condition_variable cv;
    int32_t count = 0;
};
static std::mutex s_sem_map_lock;
static std::unordered_map<uint32_t, SemImpl*> s_semaphores;

static SemImpl* get_sem(uint32_t ptr) {
    return sync_object(ptr, s_sem_map_lock, s_semaphores);
}

// OSInitSemaphore(sem, count)
static void OSInitSemaphore(CPUState* cpu) {
    SemImpl* s = get_sem(ARG0);
    std::lock_guard<std::mutex> lk(s->mtx);
    s->count = (int32_t)ARG1;
}

// OSWaitSemaphore — block until count > 0, then decrement
static void OSWaitSemaphore(CPUState* cpu) {
    SemImpl* s = get_sem(ARG0);
    std::unique_lock<std::mutex> lk(s->mtx);
    s->cv.wait(lk, [s] { return s->count > 0; });
    s->count--;
}

// OSSignalSemaphore — increment count + wake one waiter
static void OSSignalSemaphore(CPUState* cpu) {
    SemImpl* s = get_sem(ARG0);
    {
        std::lock_guard<std::mutex> lk(s->mtx);
        s->count++;
    }
    s->cv.notify_one();
}

// ---------------------------------------------------------------------------
// OSEvent — manual-reset event flag with blocking wait
// ---------------------------------------------------------------------------
struct EventImpl {
    std::mutex mtx;
    std::condition_variable cv;
    bool signaled = false;
    bool auto_reset = false; // mode 1 = autoreset
};
static std::mutex s_event_map_lock;
static std::unordered_map<uint32_t, EventImpl*> s_events;

static EventImpl* get_event(uint32_t ptr) {
    return sync_object(ptr, s_event_map_lock, s_events);
}

// OSInitEvent(event, initialState, mode)
// mode: 0 = manual-reset, 1 = auto-reset
static void OSInitEvent(CPUState* cpu) {
    EventImpl* e = get_event(ARG0);
    std::lock_guard<std::mutex> lk(e->mtx);
    e->signaled = (ARG1 != 0);
    e->auto_reset = (ARG2 != 0);
}

// OSSignalEvent — signal + wake all (or one for auto-reset)
static void OSSignalEvent(CPUState* cpu) {
    EventImpl* e = get_event(ARG0);
    {
        std::lock_guard<std::mutex> lk(e->mtx);
        e->signaled = true;
    }
    // Wake all waiters; auto-reset variant wakes one and clears in OSWaitEvent
    e->cv.notify_all();
}

// OSResetEvent — clear the signal
static void OSResetEvent(CPUState* cpu) {
    EventImpl* e = get_event(ARG0);
    std::lock_guard<std::mutex> lk(e->mtx);
    e->signaled = false;
}

// OSWaitEvent — block until signaled
static void OSWaitEvent(CPUState* cpu) {
    EventImpl* e = get_event(ARG0);
    std::unique_lock<std::mutex> lk(e->mtx);
    e->cv.wait(lk, [e] { return e->signaled; });
    if (e->auto_reset) e->signaled = false;
}

// OSWaitEventWithTimeout(event, OSTime timeout): r5:r6 ticks (a 64-bit
// argument starts on an odd register pair). TRUE if signaled, FALSE on
// timeout; 0 polls. The stub always answered FALSE, so a signaled event
// polled this way was never seen: a startup step (02BFC660) polls its
// event 0x10220BA4 and gave up after 30 s.
static void OSWaitEventWithTimeout(CPUState* cpu) {
    EventImpl* e = get_event(ARG0);
    const int64_t ticks = int64_t(uint64_t(cpu->r[5]) << 32 | cpu->r[6]);
    std::unique_lock<std::mutex> lk(e->mtx);
    bool signaled = e->signaled;
    if (!signaled && ticks > 0) {
        const auto wait = rbrew_timeout_deadline(uint64_t(ticks), RBREW_TIMER_CLOCK, std::chrono::steady_clock::now());
        signaled = e->cv.wait_until(lk, wait, [e] { return e->signaled; });
    }
    if (signaled && e->auto_reset) e->signaled = false;
    RET = signaled ? 1u : 0u;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
#include "coreinit_addrs.h"
void coreinit_sync_register(CPUState* cpu) {
#define REG(name) rbrew_register_func(cpu, ADDR_##name, name)
    REG(OSInitMutex);
    REG(OSLockMutex);
    REG(OSUnlockMutex);
    REG(OSTryLockMutex);
    REG(OSInitSemaphore);
    REG(OSWaitSemaphore);
    REG(OSSignalSemaphore);
    REG(OSInitEvent);
    REG(OSWaitEvent);
    REG(OSSignalEvent);
    REG(OSResetEvent);
    REG(OSWaitEventWithTimeout);
    REG(OSInitCond);
    REG(OSWaitCond);
    REG(OSSignalCond);
    rbrew_register_func(cpu, ADDR_OSFastMutex_Init, OSFastMutex_Init);
    rbrew_register_func(cpu, ADDR_OSFastMutex_Lock, OSFastMutex_Lock);
    rbrew_register_func(cpu, ADDR_OSFastMutex_Unlock, OSFastMutex_Unlock);
    rbrew_register_func(cpu, ADDR_OSFastMutex_TryLock, OSFastMutex_TryLock);
#undef REG
}
