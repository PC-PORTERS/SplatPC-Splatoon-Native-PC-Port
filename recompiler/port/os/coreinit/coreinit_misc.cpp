#include "../../runtime/rbrew_diagnostics.h"
// coreinit miscellaneous: time, I/O, system info, debug, mem helpers
#include "../os_common.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <stdexcept>
#include "coreinit_alarm.h"
#include "coreinit_sync_helpers.h"
#include "coreinit_dynload.h"
#include <thread>
#include "coreinit_system_settings.h"
#include "coreinit_user_config.h"

using namespace std::chrono;

static constexpr uint64_t WIIU_TIMER_FREQ = RBREW_TIMER_CLOCK;
// OSGetTime counts timer ticks since 2000-01-01 00:00:00; OSGetSystemTime
// counts since boot. Their difference is OSSystemInfo.baseTime.
static const uint64_t s_base_time = [] {
    const auto since_unix = duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
    const int64_t since_2000 = since_unix - int64_t(946684800) * 1000000;
    return since_2000 > 0 ? uint64_t(since_2000) / 1000000u * WIIU_TIMER_FREQ +
                                uint64_t(since_2000) % 1000000u * WIIU_TIMER_FREQ / 1000000u
                          : 0u;
}();

static void require_guest_range(CPUState* cpu, uint32_t address, uint64_t size) {
    if (uint64_t(address) + size > cpu->guest_memory_size || !rbrew_memory_accessible(MEM, address, size))
        throw std::runtime_error("Coreinit guest buffer is outside accessible memory");
}

static uint64_t get_wiiu_ticks() {
    return rbrew_system_ticks();
}

// --- Time ---
static void OSGetSystemTime(CPUState* cpu) {
    uint64_t t = get_wiiu_ticks();
    cpu->r[3] = (uint32_t)(t >> 32);
    cpu->r[4] = (uint32_t)(t);
}
static void OSGetTime(CPUState* cpu) {
    const uint64_t t = s_base_time + get_wiiu_ticks();
    cpu->r[3] = (uint32_t)(t >> 32);
    cpu->r[4] = (uint32_t)(t);
}

// OSGetSystemInfo returns a pointer to coreinit's static OSSystemInfo
// (coreinit.rpl: "return &systemInfo"). Layout and boot values per decaf
// coreinit_systeminfo: busSpeed, coreSpeed, baseTime, l2CacheSize[3], cpuRatio.
uint32_t coreinit_alloc_system_block(uint32_t size);
void coreinit_alarm_shutdown(); // coreinit_memory.cpp
static void OSGetSystemInfo(CPUState* cpu) {
    static uint32_t info = 0;
    static std::once_flag once;
    std::call_once(once, [&] {
        info = coreinit_alloc_system_block(0x20);
        if (!info) throw std::runtime_error("OSSystemInfo storage could not be allocated");
        rbrew_write32(MEM, info + 0x00, RBREW_BUS_CLOCK);
        rbrew_write32(MEM, info + 0x04, RBREW_CORE_CLOCK);
        rbrew_write32(MEM, info + 0x08, uint32_t(s_base_time >> 32));
        rbrew_write32(MEM, info + 0x0C, uint32_t(s_base_time));
        rbrew_write32(MEM, info + 0x10, 512u * 1024u);
        rbrew_write32(MEM, info + 0x14, 2u * 1024u * 1024u);
        rbrew_write32(MEM, info + 0x18, 512u * 1024u);
        rbrew_write32(MEM, info + 0x1C, RBREW_CORE_CLOCK / RBREW_BUS_CLOCK); // 5
    });
    RET = info;
}
static void OSGetTick(CPUState* cpu) {
    RET = (uint32_t)get_wiiu_ticks();
}

static void OSTicksToCalendarTime(CPUState* cpu) {
    // r3:r4 = ticks (64-bit), r5 = OSCalendarTime* out
    // Fill in a plausible UTC structure (OSCalendarTime: sec,min,hour,mday,mon,year,wday,yday,msec,usec)
    uint32_t out = ARG2;
    if (out) {
        require_guest_range(cpu, out, 40);
        memset(MEM + out, 0, 40);
    }
    RET = 0;
}

// --- Core info ---
static void OSGetCoreId(CPUState* cpu) {
    RET = rbrew_current_core();
}
static void OSGetMainCoreId(CPUState* cpu) {
    RET = 1;
}

// --- Logging ---
// Diagnostic only: the format string and its first two raw arguments.
static void OSReport(CPUState* cpu) {
    if (!ARG0 || !rbrew_memory_accessible(cpu->mem, ARG0, 1)) return;
    const char* fmt = (const char*)(MEM + ARG0);
    size_t length = strnlen(fmt, size_t(std::min<uint64_t>(256, cpu->guest_memory_size - ARG0)));
    while (length && fmt[length - 1] == '\n') --length;
    RBREW_LOG_INFO("OSReport", "%.*s [r4=%08X r5=%08X]\n", int(length), fmt, ARG1, ARG2);
}
static void OSPanic(CPUState* cpu) {
    // OSPanic(const char* file, int line, const char* fmt, ...)
    // r3=file, r4=line, r5=fmt, r6..=varargs
    const char* file = ARG0 ? (const char*)(MEM + ARG0) : "?";
    int line = (int)ARG1;
    const char* fmt = ARG2 ? (const char*)(MEM + ARG2) : "";
    // Print with up to 3 integer varargs (r6/r7/r8)
    RBREW_LOG_FATAL_BEGIN("OSPanic", "%s:%d  ", file, line);
    RBREW_LOG_RAW(fmt, cpu->r[6], cpu->r[7], cpu->r[8]);
    RBREW_LOG_RAW("\n");
    // Read-only caller attribution. EABI frames hold the previous SP at +0 and
    // the callee-saved LR in the previous frame at +4. Stop at any unreadable,
    // misaligned or non-increasing link; never write guest state.
    RBREW_LOG_FATAL_BEGIN("OSPanic", "  LR=%08X SP=%08X chain:", cpu->lr, cpu->r[1]);
    uint32_t sp = cpu->r[1];
    for (int depth = 0; depth < 16; ++depth) {
        if ((sp & 3u) || !rbrew_memory_accessible(cpu->mem, sp, 4)) break;
        const uint32_t prev = rbrew_read32(cpu->mem, sp);
        if (!prev || (prev & 3u) || prev <= sp || !rbrew_memory_accessible(cpu->mem, prev, 8)) break;
        RBREW_LOG_RAW(" %08X", rbrew_read32(cpu->mem, prev + 4));
        sp = prev;
    }
    RBREW_LOG_RAW("\n");
    // Do NOT abort() — let the game continue.  A real Wii U OSPanic would
    // halt the system, but during bring-up we want to see what happens after
    // non-fatal panics (e.g. "unhandled signal 6" from the fiber scheduler).
    // If the game truly cannot continue it will loop or fault elsewhere, giving
    // us a clearer callchain to diagnose.
}
static void COSWarn(CPUState* cpu) {
    (void)cpu;
}
static void OSVReport(CPUState* cpu) {
    (void)cpu;
}
static void OSConsoleWrite(CPUState* cpu) {
    (void)cpu;
}

// --- Interrupt control (no-op on PC) ---
static void OSDisableInterrupts(CPUState* cpu) {
    RET = 1;
}
static void OSRestoreInterrupts(CPUState* cpu) {
    RET = ARG0;
}

// --- Process/thread exit ---
// Implemented via GuestThreadExit exception (see coreinit_threads.cpp).
// coreinit_exit_thread() is the real implementation; we just forward to it.
extern void coreinit_exit_thread(CPUState*);
static void OSExitThread(CPUState* cpu) {
    coreinit_exit_thread(cpu);
}
static void exit_fn(CPUState* cpu) {
    ::exit((int)ARG0);
}
static void _Exit_fn(CPUState* cpu) {
    ::_Exit((int)ARG0);
}
static void OSBlockThreadsOnExit(CPUState* cpu) {
    (void)cpu;
}

// --- Dynamic loading ---
static void OSDynLoad_Acquire(CPUState* cpu) {
    rbrew_dynload_acquire(cpu);
}
static void OSDynLoad_FindExport(CPUState* cpu) {
    rbrew_dynload_find_export(cpu);
}

// --- Cache (no-op on cache-coherent host) ---
static void DCFlushRange(CPUState* cpu) {
    (void)cpu;
}
static void DCFlushRangeNoSync(CPUState* cpu) {
    (void)cpu;
}
static void DCInvalidateRange(CPUState* cpu) {
    (void)cpu;
}
static void DCStoreRange(CPUState* cpu) {
    (void)cpu;
}
static void DCZeroRange(CPUState* cpu) {
    uint32_t addr = ARG0, len = ARG1;
    if (addr && len) {
        require_guest_range(cpu, addr, len);
        memset(MEM + addr, 0, len);
    }
}
static void OSMemoryBarrier(CPUState* cpu) {
    (void)cpu;
}

// --- Memory block ops ---
static void OSBlockMove(CPUState* cpu) {
    uint32_t dst = ARG0, src = ARG1, len = ARG2;
    if (dst && src && len) {
        require_guest_range(cpu, dst, len);
        require_guest_range(cpu, src, len);
        memmove(MEM + dst, MEM + src, len);
    }
}
static void OSBlockSet(CPUState* cpu) {
    uint32_t dst = ARG0, val = ARG1, len = ARG2;
    if (dst && len) {
        require_guest_range(cpu, dst, len);
        memset(MEM + dst, (int)val, len);
    }
}

// --- Host memcpy/memmove/memset passed through ---
static void memcpy_fn(CPUState* cpu) {
    uint32_t dst = ARG0, src = ARG1, n = ARG2;
    if (dst && src && n && rbrew_memory_accessible(MEM, dst, n) && rbrew_memory_accessible(MEM, src, n))
        memcpy(MEM + dst, MEM + src, n);
    RET = dst;
}
static void memmove_fn(CPUState* cpu) {
    uint32_t dst = ARG0, src = ARG1, n = ARG2;
    if (dst && src && n && rbrew_memory_accessible(MEM, dst, n) && rbrew_memory_accessible(MEM, src, n))
        memmove(MEM + dst, MEM + src, n);
    RET = dst;
}
static void memset_fn(CPUState* cpu) {
    uint32_t dst = ARG0, val = ARG1, n = ARG2;
    if (dst && n) {
        // Retain the OOB guard for unmapped holes and a disabled overlay too.
        if (rbrew_memory_accessible(MEM, dst, n))
            memset(MEM + dst, (int)val, n);
        else
            RBREW_LOG_INFO("coreinit", "memset OOB: dst=0x%08X n=0x%X\n", dst, n);
    }
    RET = dst;
}

// --- Environment variable ---
static void ENVGetEnvironmentVariable(CPUState* cpu) {
    // r3 = name ptr, r4 = out buf, r5 = buf size
    if (ARG1 && ARG2) rbrew_write8(MEM, ARG1, 0); // empty string
    RET = 0;
}

// --- Debugger ---
static void OSIsDebuggerPresent(CPUState* cpu) {
    RET = 0;
}
static void OSIsDebuggerInitialized(CPUState* cpu) {
    RET = 0;
}

// --- Home menu ---
static void OSEnableHomeButtonMenu(CPUState* cpu) {
    (void)cpu;
}

// --- Saves done ---
static void OSSavesDone_ReadyToRelease(CPUState* cpu) {
    (void)cpu;
}

// ---------------------------------------------------------------------------
// OSMessageQueue — bounded blocking message queue
//
// Layout per wut coreinit/messagequeue.h and decaf-emu (0x3C bytes):
//   +0x00 tag 'mSgQ'  +0x04 name  +0x0C sendQueue  +0x1C recvQueue
//   +0x2C OSMessage* messages  +0x30 size  +0x34 first  +0x38 used
// OSMessage is 16 bytes {message, args[3]}; send/receive copy whole
// entries. Flags: 1 = blocking, 2 = high priority (pushed to the front).
// Blocking uses host condition variables; the guest-visible fields are
// mirrored after every change.
// ---------------------------------------------------------------------------
constexpr uint32_t kMessageQueueTag = 0x6D536751u;
constexpr uint32_t kMessageSize = 0x10;
constexpr uint32_t kMessageFlagBlocking = 1, kMessageFlagHighPriority = 2;
struct MsgQueueImpl {
    std::mutex mtx;
    std::condition_variable not_empty;
    std::condition_variable not_full;
    uint32_t buf_addr = 0;
    uint32_t capacity = 0;
    uint32_t head = 0;
    uint32_t count = 0;
};

static std::mutex s_mq_map_lock;
static std::unordered_map<uint32_t, MsgQueueImpl*> s_msg_queues;

static MsgQueueImpl* get_mq(uint32_t ptr) {
    std::lock_guard<std::mutex> lk(s_mq_map_lock);
    auto it = s_msg_queues.find(ptr);
    if (it != s_msg_queues.end()) return it->second;
    auto* q = new MsgQueueImpl();
    s_msg_queues[ptr] = q;
    return q;
}
// Caller holds mq->mtx.
static void mirror_mq(CPUState* cpu, uint32_t q_addr, const MsgQueueImpl* mq) {
    rbrew_write32(MEM, q_addr + 0x34, mq->head);
    rbrew_write32(MEM, q_addr + 0x38, mq->count);
}
static void copy_message(CPUState* cpu, uint32_t to, uint32_t from) {
    require_guest_range(cpu, to, kMessageSize);
    require_guest_range(cpu, from, kMessageSize);
    for (uint32_t i = 0; i < kMessageSize; i += 4) rbrew_write32(MEM, to + i, rbrew_read32(MEM, from + i));
}

// OSInitMessageQueue(queue, messages, size)
static void OSInitMessageQueue(CPUState* cpu) {
    const uint32_t q_addr = ARG0, buf_ptr = ARG1, cap = ARG2;
    require_guest_range(cpu, q_addr, 0x3C);
    if (!cap || !buf_ptr) throw std::runtime_error("OSInitMessageQueue requires a message array");
    require_guest_range(cpu, buf_ptr, uint64_t(cap) * kMessageSize);
    MsgQueueImpl* mq = get_mq(q_addr);
    std::lock_guard<std::mutex> lk(mq->mtx);
    mq->buf_addr = buf_ptr;
    mq->capacity = cap;
    mq->head = 0;
    mq->count = 0;
    memset(MEM + q_addr, 0, 0x3C);
    rbrew_write32(MEM, q_addr, kMessageQueueTag);
    rbrew_write32(MEM, q_addr + 0x2C, buf_ptr);
    rbrew_write32(MEM, q_addr + 0x30, cap);
    mirror_mq(cpu, q_addr, mq);
}

// OSSendMessage(queue, const OSMessage* message, flags) -> BOOL
static void OSSendMessage(CPUState* cpu) {
    const uint32_t q_addr = ARG0, message = ARG1, flags = ARG2;
    MsgQueueImpl* mq = get_mq(q_addr);
    std::unique_lock<std::mutex> lk(mq->mtx);
    if (!mq->capacity) throw std::runtime_error("OSSendMessage on an uninitialized queue");
    if (flags & kMessageFlagBlocking)
        mq->not_full.wait(lk, [mq] { return mq->count < mq->capacity; });
    else if (mq->count >= mq->capacity) {
        RET = 0;
        return;
    }
    uint32_t index;
    if (flags & kMessageFlagHighPriority) {
        mq->head = mq->head ? mq->head - 1 : mq->capacity - 1;
        index = mq->head;
    } else {
        index = (mq->head + mq->count) % mq->capacity;
    }
    copy_message(cpu, mq->buf_addr + index * kMessageSize, message);
    mq->count++;
    mirror_mq(cpu, q_addr, mq);
    lk.unlock();
    mq->not_empty.notify_all();
    RET = 1;
}

// OSReceiveMessage(queue, OSMessage* message, flags) -> BOOL
static void OSReceiveMessage(CPUState* cpu) {
    const uint32_t q_addr = ARG0, out_ptr = ARG1, flags = ARG2;
    MsgQueueImpl* mq = get_mq(q_addr);
    std::unique_lock<std::mutex> lk(mq->mtx);
    if (flags & kMessageFlagBlocking)
        mq->not_empty.wait(lk, [mq] { return mq->count > 0; });
    else if (mq->count == 0) {
        RET = 0;
        return;
    } // message left untouched
    copy_message(cpu, out_ptr, mq->buf_addr + mq->head * kMessageSize);
    mq->head = (mq->head + 1) % mq->capacity;
    mq->count--;
    mirror_mq(cpu, q_addr, mq);
    lk.unlock();
    mq->not_full.notify_all();
    RET = 1;
}

// --- Alarms ---
// One host thread fires every alarm at its OS-tick deadline and calls the
// guest handler(alarm, context) like the audio callbacks do: own guest stack,
// a native-thread identity (rbrew_system_thread) and the setter's core.
// The no-op versions never fired: the 3 ms remote-speaker alarm (02CF5FB8)
// and system timers depend on them.
uint32_t coreinit_alloc_system_block(uint32_t size);
void coreinit_alarm_shutdown();
namespace {
struct PendingAlarm { uint64_t next=0,period=0,generation=0;uint32_t handler=0,core=1;bool firing=false; };
struct AlarmService {
    std::mutex mutex;std::condition_variable changed;
    std::unordered_map<uint32_t,PendingAlarm> pending;
    bool started=false,stopping=false;
    std::thread worker;
    uint64_t generation=0;
    CPUState cpu{};uint32_t storage=0;
    std::atomic<uint32_t> core{1};
};
AlarmService& alarm_service(){static auto* s=new AlarmService;return *s;}
constexpr uint32_t kAlarmStorage=0x8000,kAlarmContext=0x100;
uint64_t alarm_now(){return rbrew_alarm_add(s_base_time,rbrew_system_ticks());}
void fire_alarm(AlarmService& s,uint32_t alarm,const PendingAlarm& a) {
    CPUState call=s.cpu;call.r[1]=(s.storage+kAlarmStorage-16)&~15u;
    rbrew_write32(call.mem,call.r[1],0);
    std::memset(call.mem+s.storage+kAlarmContext,0,0x400);
    call.r[3]=alarm;call.r[4]=s.storage+kAlarmContext;call.lr=0;
    s.core.store(a.core);
    struct Scope {
        uint32_t thread=rbrew_system_thread;std::atomic<uint32_t>* core=rbrew_thread_core_slot;
        Scope(AlarmService& s){rbrew_system_thread=s.storage;rbrew_thread_core_slot=&s.core;}
        ~Scope(){rbrew_system_thread=thread;rbrew_thread_core_slot=core;}
    } scope(s);
    try{rbrew_dispatch(&call,a.handler);}
    catch(const std::exception& e){RBREW_LOGF(stderr,"[alarm] %08X: %s\n",a.handler,e.what());}
    catch(...){RBREW_LOGF(stderr,"[alarm] %08X: handler failed\n",a.handler);}
}
void alarm_thread() {
    auto& s=alarm_service();std::unique_lock lock(s.mutex);
    while(!s.stopping) {
        if(s.pending.empty()){s.changed.wait(lock);continue;}
        auto due=s.pending.begin();
        for(auto it=s.pending.begin();it!=s.pending.end();++it)if(it->second.next<due->second.next)due=it;
        const uint64_t now=alarm_now();
        if(due->second.next>now) {
            const auto deadline=rbrew_timeout_deadline(due->second.next-s_base_time,RBREW_TIMER_CLOCK,rbrew_boot_time);
            s.changed.wait_until(lock,deadline);continue;
        }
        const uint32_t address=due->first;const PendingAlarm a=due->second;
        due->second.firing=true;
        // Dispatch is claimed under the lock. Cancel may cancel a future fire
        // or the rearm of an in-flight handler; it never waits while holding a guest lock.
        lock.unlock();fire_alarm(s,address,a);lock.lock();
        auto current=s.pending.find(address);
        if(current!=s.pending.end() && current->second.generation==a.generation) {
            if(a.period) {
                current->second.next=rbrew_alarm_next_periodic(a.next,a.period,alarm_now());
                current->second.firing=false;
                rbrew_write64(s.cpu.mem,address+kOSAlarmNext,current->second.next);
            } else s.pending.erase(current);
        }
    }
}
void schedule_alarm(CPUState* cpu,uint32_t alarm,uint64_t next,uint64_t period,uint32_t handler) {
    if(!alarm || !handler || !rbrew_memory_accessible(MEM,alarm,kOSAlarmSize)){RET=0;return;}
    auto& s=alarm_service();
    {
        std::lock_guard lock(s.mutex);
        if(s.stopping){RET=0;return;}
        if(!s.started) {
            s.storage=coreinit_alloc_system_block(kAlarmStorage);
            if(!s.storage)throw std::runtime_error("alarm thread storage allocation failed");
            s.cpu=*cpu;s.stopping=false;
            s.worker=std::thread(alarm_thread);s.started=true;
            static const bool registered=[] {std::atexit(coreinit_alarm_shutdown);return true;}();(void)registered;
        }
        if(s.cpu.mem!=MEM)throw std::runtime_error("Alarm service belongs to another guest arena");
        rbrew_write32(MEM,alarm+kOSAlarmHandler,handler);
        rbrew_write64(MEM,alarm+kOSAlarmNext,next);rbrew_write64(MEM,alarm+kOSAlarmPeriod,period);
        s.pending[alarm]=PendingAlarm{next,period,++s.generation,handler,rbrew_current_core(),false};
    }
    s.changed.notify_all();RET=1;
}
}
void coreinit_alarm_shutdown() {
    auto& s=alarm_service();
    {std::lock_guard lock(s.mutex);s.stopping=true;s.pending.clear();}
    s.changed.notify_all();
    if(s.worker.joinable()) {
        if(s.worker.get_id()==std::this_thread::get_id())s.worker.detach();else s.worker.join();
    }
    {std::lock_guard lock(s.mutex);s.started=false;}
}

static void OSCreateAlarm(CPUState* cpu) {
    if (!ARG0 || !rbrew_memory_accessible(MEM, ARG0, kOSAlarmSize)) return;
    { std::lock_guard lock(alarm_service().mutex); alarm_service().pending.erase(ARG0); }
    memset(MEM + ARG0, 0, kOSAlarmSize);
    rbrew_write32(MEM, ARG0, kOSAlarmTag);
}
// OSSetAlarm(alarm, u64 delay, handler): r5:r6 delay ticks, r7 handler.
static void OSSetAlarm(CPUState* cpu) {
    const uint64_t delay = uint64_t(cpu->r[5]) << 32 | cpu->r[6];
    if(int64_t(delay)<=0){RET=1;return;}
    schedule_alarm(cpu,ARG0,rbrew_alarm_add(alarm_now(),delay),0,cpu->r[7]);
}
// OSSetPeriodicAlarm(alarm, u64 start, u64 period, handler): r5:r6, r7:r8, r9.
static void OSSetPeriodicAlarm(CPUState* cpu) {
    const uint64_t start = uint64_t(cpu->r[5]) << 32 | cpu->r[6], period = uint64_t(cpu->r[7]) << 32 | cpu->r[8];
    if(int64_t(period)<=0){RET=1;return;}
    schedule_alarm(cpu, ARG0, rbrew_alarm_first_periodic(start,period,alarm_now()), period, cpu->r[9]);
}
static void OSCancelAlarm(CPUState* cpu) {
    std::lock_guard lock(alarm_service().mutex);
    RET = alarm_service().pending.erase(ARG0) ? 1u : 0u;
}
// OSCancelAlarms(group): every pending alarm whose group (+0x10) matches.
static void OSCancelAlarms(CPUState* cpu) {
    if(ARG0==0 || ARG0==~0u)return;
    std::lock_guard lock(alarm_service().mutex);
    auto& pending = alarm_service().pending;
    for (auto it = pending.begin(); it != pending.end();)
        it = rbrew_read32(MEM, it->first + kOSAlarmGroup) == ARG0 ? pending.erase(it) : std::next(it);
}
static void OSSetAlarmUserData(CPUState* cpu) {
    if (ARG0 && rbrew_memory_accessible(MEM, ARG0, kOSAlarmSize)) rbrew_write32(MEM, ARG0 + kOSAlarmUserData, ARG1);
}
static void OSGetAlarmUserData(CPUState* cpu) {
    RET = ARG0 && rbrew_memory_accessible(MEM, ARG0, kOSAlarmSize) ? rbrew_read32(MEM, ARG0 + kOSAlarmUserData) : 0u;
}

// --- GHS C library locks ---
// __ghsLock guards the C library's shared state (atexit table, stdio...);
// __ghs_mtx_* are its per-object mutexes, keyed by the guest address the
// library passes (Cemu: an OSMutex behind that pointer). They were no-ops.
namespace {
GuestRecursiveLock& ghs_global_lock() { static auto* l = new GuestRecursiveLock; return *l; }
std::mutex s_ghs_mutexes_lock;
std::unordered_map<uint32_t, GuestRecursiveLock*> s_ghs_mutexes;   // never freed: a late unlock stays valid
GuestRecursiveLock& ghs_mutex(uint32_t address) {
    std::lock_guard lock(s_ghs_mutexes_lock);
    auto& m = s_ghs_mutexes[address];
    if (!m) m = new GuestRecursiveLock;
    return *m;
}
void ghs_unlock_checked(GuestRecursiveLock& l, uint32_t address) {
    if (!l.unlock()) RBREW_LOGF(stderr, "[ghs] unlock of %08X by a thread that does not hold it\n", address);
}
}
static void __ghs_mtx_init(CPUState* cpu) {
    ghs_mutex(ARG0);
    if (ARG0 && rbrew_memory_accessible(MEM, ARG0, 4)) rbrew_write32(MEM, ARG0, ARG0);   // non-null handle
}
static void __ghs_mtx_dst(CPUState* cpu)    { (void)cpu; }
static void __ghs_mtx_lock(CPUState* cpu)   { ghs_mutex(ARG0).lock(); }
static void __ghs_mtx_unlock(CPUState* cpu) { ghs_unlock_checked(ghs_mutex(ARG0), ARG0); }
static void __ghs_flock_file(CPUState* cpu)   { (void)cpu; }
static void __ghs_flock_ptr(CPUState* cpu)    { (void)cpu; }
static void __ghs_funlock_file(CPUState* cpu) { (void)cpu; }
static void __ghsLock(CPUState* cpu)   { (void)cpu; ghs_global_lock().lock(); }
static void __ghsUnlock(CPUState* cpu) { (void)cpu; ghs_unlock_checked(ghs_global_lock(), 0); }
static void __gh_set_errno(CPUState* cpu) { (void)cpu; }

// --- MCP (system settings) ---
static void MCP_Open(CPUState* cpu) {
    rbrew_mcp_open(cpu);
}
static void MCP_Close(CPUState* cpu) {
    rbrew_mcp_close(cpu);
}
static void MCP_GetSysProdSettings(CPUState* cpu) {
    rbrew_mcp_get_sys_prod_settings(cpu);
}

// --- UC (user config) ---
static void UCOpen(CPUState* cpu) {
    rbrew_uc_open(cpu);
}
static void UCClose(CPUState* cpu) {
    rbrew_uc_close(cpu);
}
static void UCReadSysConfig(CPUState* cpu) {
    rbrew_uc_read_sys_config(cpu);
}

// --- Misc ---
static void bspGetHardwareVersion(CPUState* cpu) {
    RET = 0x00000001u;
}
static void OSCoherencyBarrier(CPUState* cpu) {
    (void)cpu;
}
static void OSIsAddressRangeDCValid(CPUState* cpu) {
    RET = 1;
}

#include "coreinit_addrs.h"

// NOTE: Functions not called by this game are not registered here.
// gambit_plt_auto_register() pre-registers STUB for all 610 PLT stubs.
void coreinit_misc_register(CPUState* cpu) {
#define REG(name) rbrew_register_func(cpu, ADDR_##name, name)
    REG(OSGetSystemTime);
    REG(OSGetTime);
    REG(OSGetSystemInfo);
    REG(OSGetTick);
    REG(OSTicksToCalendarTime);
    REG(OSGetCoreId);
    REG(OSGetMainCoreId);
    REG(OSReport);
    REG(OSPanic);
    REG(COSWarn);
    REG(OSVReport);
    REG(OSConsoleWrite);
    REG(OSDisableInterrupts);
    REG(OSRestoreInterrupts);
    REG(OSExitThread);
    rbrew_register_func(cpu, ADDR_exit, exit_fn);
    rbrew_register_func(cpu, ADDR__Exit, _Exit_fn);
    REG(OSBlockThreadsOnExit);
    REG(OSDynLoad_Acquire);
    REG(OSDynLoad_FindExport);
    REG(DCFlushRange);
    REG(DCFlushRangeNoSync);
    REG(DCInvalidateRange);
    REG(DCStoreRange);
    REG(DCZeroRange);
    REG(OSMemoryBarrier);
    REG(OSBlockMove);
    REG(OSBlockSet);
    rbrew_register_func(cpu, ADDR_memcpy, memcpy_fn);
    rbrew_register_func(cpu, ADDR_memmove, memmove_fn);
    rbrew_register_func(cpu, ADDR_memset, memset_fn);
    REG(ENVGetEnvironmentVariable);
    REG(OSIsDebuggerPresent);
    REG(OSIsDebuggerInitialized);
    REG(OSEnableHomeButtonMenu);
    REG(OSSavesDone_ReadyToRelease);
    REG(OSInitMessageQueue);
    REG(OSSendMessage);
    REG(OSReceiveMessage);
    REG(OSCreateAlarm);
    REG(OSSetAlarm);
    REG(OSCancelAlarm);
    REG(OSCancelAlarms);
    REG(OSSetPeriodicAlarm);
    REG(OSSetAlarmUserData);
    REG(OSGetAlarmUserData);
    // OSFastMutex_* are registered in coreinit_sync.cpp
    REG(__ghs_mtx_init);
    REG(__ghs_mtx_dst);
    REG(__ghs_mtx_lock);
    REG(__ghs_mtx_unlock);
    REG(__ghsLock);
    REG(__ghsUnlock);
    REG(__gh_set_errno);
    REG(MCP_Open);
    REG(MCP_Close);
    REG(MCP_GetSysProdSettings);
    REG(UCOpen);
    REG(UCClose);
    REG(UCReadSysConfig);
    REG(bspGetHardwareVersion);
    REG(OSCoherencyBarrier);
    REG(OSIsAddressRangeDCValid);
#undef REG
}

