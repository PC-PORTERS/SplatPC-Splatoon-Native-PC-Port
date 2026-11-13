#include <utility>
#include "../../runtime/rbrew_diagnostics.h"
#include "../../runtime/texture_diagnostics.h"
#include "gx2_display_list.h"
#include "gx2_native_context.h"
#include "gx2_pm4_state.h"
#include "gx2_render.h"
#include "gx2_perf.h"
#include "gx2_addrs.h"
#include "../os_common.h"
#include "gx2_write_watch.h"
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "../../runtime/benchmark_input.h"

extern bool g_gx2_frame_trace_active;

namespace {
struct Command {
    Gx2CommandFn fn;
    std::array<uint32_t, 8> r; // r3..r10
    std::array<double, 8> f;   // f1..f8
    // fn == nullptr: shader constant snapshot (r[0] = is_pixel, r[1] = index).
    std::vector<uint32_t> words;
    Gx2DescriptorFn descriptor_fn = nullptr;
    std::vector<uint8_t> descriptor;
};
struct OpenList {
    bool active = false;
    uint32_t buffer = 0, capacity = 0;
    std::vector<Command> commands;
};
std::mutex s_mutex;
// Callers waiting for s_mutex on the game/render path. std::mutex is not fair:
// a sweep re-locking it between chunks would keep such a caller waiting for
// the whole pass, so the sweep steps aside while this is nonzero.
std::atomic<uint32_t> s_registry_waiters{0};
struct RegistryLock {
    std::unique_lock<std::mutex> lock;
    RegistryLock() {
        s_registry_waiters.fetch_add(1, std::memory_order_relaxed);
        lock = std::unique_lock(s_mutex);
        s_registry_waiters.fetch_sub(1, std::memory_order_relaxed);
    }
};
// One command writer per Espresso core, plus slot 3: the render thread's
// private builder for context-state snapshots (gx2_dl_create_snapshot), so a
// snapshot built while the game keeps posting never captures its commands.
std::array<OpenList, 4> s_open;
std::array<std::mutex, 4> s_open_mutex;
thread_local bool t_private_builder = false;
unsigned builder_slot() {
    return t_private_builder ? 3u : unsigned(rbrew_current_core() % 3u);
}
// Every GX2EndDisplayList gets a fresh id: the game records meshes one after
// another into one scratch buffer and copies the bytes out, and each copy
// must replay what was recorded then. Recordings not used (recorded/called)
// for longest leave the resident cache beyond kMaxRecordings. Their command
// payloads remain in a private process-lifetime archive: copied guest tags
// are still callable, including startup shader lists first used in gameplay.
// Shared: a replay holds its commands while the recording may be evicted.
using Commands = std::shared_ptr<const std::vector<Command>>;
// Archive I/O and command-vector destruction do not need the recording
// registry. Run one maintenance job at a time; retain resident commands
// until the archive has been flushed and its offsets published.
class CommandRetirement {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::function<size_t()>> pending;
    std::thread worker;
    bool stopping = false, busy = false;
    uint64_t retired = 0;
    void run() {
        std::unique_lock lock(mutex);
        for (;;) {
            changed.wait(lock, [&] { return stopping || !pending.empty(); });
            if (pending.empty()) return;
            auto work = std::move(pending.front());
            pending.pop_front();
            busy = true;
            changed.notify_all();
            lock.unlock();
            size_t count = 0;
            try {
                count = work();
            } catch (...) {
                RBREW_LOG_ERROR("gx2", "display-list maintenance failed; retaining resident commands\n");
            }
            work = {};
            lock.lock();
            retired += count;
            busy = false;
            changed.notify_all();
        }
    }

public:
    ~CommandRetirement() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        changed.notify_all();
        if (worker.joinable()) worker.join();
    }
    void retire(std::function<size_t()> work) {
        std::unique_lock lock(mutex);
        if (!worker.joinable()) worker = std::thread([this] { run(); });
        changed.wait(lock, [&] { return pending.size() < 2; });
        pending.push_back(std::move(work));
        changed.notify_all();
    }
#ifdef RBREW_GX2_QUEUE_TESTS
    std::pair<uint64_t, bool> drain_for_test() {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return pending.empty() && !busy; });
        return {retired, worker.joinable() && worker.get_id() != std::this_thread::get_id()};
    }
#endif
};
CommandRetirement& command_retirement() {
    static CommandRetirement worker;
    return worker;
}
constexpr uint64_t kNotArchived = ~uint64_t(0);
enum RecordingFlags : uint8_t {
    kTagged = 1, // its id was written into its buffer (capacity >= 12)
    kUsed = 2,   // called or republished since the last sweep
    kMissed = 4, // the last sweep found no reference to it
    kDead = 8,   // unreachable: commands released, never callable again
};
enum class BakedOpKind : uint8_t {
    DrawIndexedEx,
    DrawIndexedEx2,
    DrawEx,
    UniformWords,
    SetAttribBuffer,
    SetUniformBlock,
    SetShader,
    SetTexture,
    SetSampler,
    Descriptor,
    Generic,
};
struct BakedCall {
    Gx2CommandFn fn;
    Gx2DescriptorFn desc_fn;
    uint32_t desc_offset;
    std::array<uint32_t, 8> r;
    std::array<double, 8> f;
};
struct BakedOp {
    BakedOpKind kind;
    uint8_t is_pixel;
    uint16_t word_count;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t arg2;
    uint32_t arg3;
    uint32_t arg4;
    uint32_t arg5;
    uint32_t arg6;
};
static_assert(sizeof(BakedOp) == 32);
struct BakedList {
    std::vector<BakedOp> ops;
    std::vector<BakedCall> call_pool;
    std::vector<uint32_t> word_pool;
    std::vector<uint8_t> desc_pool;
};
struct Recording {
    Commands commands;
    uint64_t last_use = 0, archive_offset = kNotArchived;
    uint32_t buffer = 0, bytes = 0; // where it was recorded; resident size
    uint8_t flags = 0;
    std::shared_ptr<const BakedList> baked;
    bool archived() const { return archive_offset != kNotArchived; }
};
// Recordings by id. Ids are dense (s_next_id++). Fixed-size chunks keep
// entry addresses stable, which eviction relies on while its archive job
// runs; a chunk whose recordings are all dead is released.
class RecordingTable {
    static constexpr size_t kChunk = 4096;
    std::vector<std::unique_ptr<Recording[]>> chunks;
    std::vector<uint32_t> dead; // dead recordings per chunk
    size_t count = 0;

public:
    Recording* find(uint32_t id) {
        return id < count && chunks[id / kChunk] ? &chunks[id / kChunk][id % kChunk] : nullptr;
    }
    Recording& insert(uint32_t id, Recording recording) {
        while (count <= id) {
            if (count % kChunk == 0) {
                chunks.push_back(std::make_unique<Recording[]>(kChunk));
                dead.push_back(0);
            }
            ++count;
        }
        return chunks[id / kChunk][id % kChunk] = std::move(recording);
    }
    // Caller has released the recording's commands and set kDead.
    void note_dead(uint32_t id) { ++dead[id / kChunk]; }
    // Not while an eviction job holds pointers to entries.
    void release_dead_chunks() {
        for (size_t chunk = 0; chunk < chunks.size(); ++chunk)
            if (chunks[chunk] && dead[chunk] == kChunk) chunks[chunk].reset();
    }
};
RecordingTable s_recordings;
// Ids that are not dead (ascending): eviction and sweeps walk these instead
// of every id ever recorded.
std::vector<uint32_t> s_tracked;
uint32_t recording_bytes(const std::vector<Command>& commands) {
    uint64_t bytes = sizeof(std::vector<Command>) + 64 + commands.size() * sizeof(Command);
    for (const Command& c : commands) bytes += c.words.size() * sizeof(uint32_t) + c.descriptor.size();
    return uint32_t(std::min<uint64_t>(bytes, UINT32_MAX));
}
std::unordered_map<uint32_t, uint32_t> s_lists; // buffer address -> latest id
uint32_t s_next_id = 0;
uint64_t s_use_clock = 0;
// 2^18: at 2^16 the game's per-frame lists pushed reused lists out to the
// archive and the render thread read them back mid-frame (~9% of its time).
// Also bounded by bytes: game lists average 10-40 KB, so 2^18 of them could
// hold 4-10 GB. Dead lists are freed by sweeps first (see sweep_recordings).
constexpr size_t kMaxRecordings = size_t(1) << 18;
constexpr uint64_t kMaxResidentBytes = uint64_t(1536) << 20;
size_t s_resident_recordings = 0;
uint64_t s_resident_bytes = 0;
// Contains native call arguments/snapshots, never GPU command execution.
// tmpfile is private and removed on close. If it cannot be written, keep
// the resident payload rather than discard observable guest state.
struct FileDeleter {
    void operator()(std::FILE* f) const {
        if (f) std::fclose(f);
    }
};
std::unique_ptr<std::FILE, FileDeleter> s_archive{nullptr};
uint64_t s_archive_seeks = 0;
uint64_t s_archive_end = 0;
bool s_archive_at_end = false; // the file position is s_archive_end (no restore read since)
std::mutex s_archive_mutex;
bool s_eviction_inflight = false; // protected by s_mutex
std::array<char, 1u << 20> s_archive_buffer{};
struct ArchivedCommand {
    Gx2CommandFn fn;
    Gx2DescriptorFn descriptor_fn;
    std::array<uint32_t, 8> r;
    std::array<double, 8> f;
    uint32_t words, descriptor;
};
bool archive_seek(uint64_t offset, int origin) {
    ++s_archive_seeks;
#ifdef _WIN32
    return ::_fseeki64(s_archive.get(), int64_t(offset), origin) == 0;
#else
    return ::fseeko(s_archive.get(), off_t(offset), origin) == 0;
#endif
}
bool archive_commands(Recording& recording) {
    if (recording.archived()) return true; // Immutable payload already has backing.
    if (!s_archive) return false;
    const uint64_t offset = s_archive_end;
    uint64_t serialized = 0;
    auto write = [&](const void* p, size_t bytes) {
        if (bytes && std::fwrite(p, 1, bytes, s_archive.get()) != bytes) return false;
        serialized += bytes;
        return true;
    };
    const uint32_t count = uint32_t(recording.commands->size());
    if (!write(&count, sizeof(count))) return false;
    for (const auto& c : *recording.commands) {
        const ArchivedCommand a{c.fn, c.descriptor_fn,          c.r,
                                c.f,  uint32_t(c.words.size()), uint32_t(c.descriptor.size())};
        if (!write(&a, sizeof(a)) || !write(c.words.data(), c.words.size() * sizeof(uint32_t)) ||
            !write(c.descriptor.data(), c.descriptor.size()))
            return false;
    }
    recording.archive_offset = offset;
    s_archive_end += serialized;
    return true;
}
Commands restore_commands(uint64_t offset) {
    std::lock_guard archive_lock(s_archive_mutex);
    s_archive_at_end = false;
    if (!s_archive || !archive_seek(offset, SEEK_SET)) throw std::runtime_error("GX2 display-list archive seek failed");
    auto read = [&](void* p, size_t bytes) {
        if (bytes && std::fread(p, 1, bytes, s_archive.get()) != bytes)
            throw std::runtime_error("GX2 display-list archive read failed");
    };
    uint32_t count = 0;
    read(&count, sizeof(count));
    auto commands = std::make_shared<std::vector<Command>>(count);
    for (auto& c : *commands) {
        ArchivedCommand a{};
        read(&a, sizeof(a));
        c.fn = a.fn;
        c.descriptor_fn = a.descriptor_fn;
        c.r = a.r;
        c.f = a.f;
        c.words.resize(a.words);
        c.descriptor.resize(a.descriptor);
        read(c.words.data(), c.words.size() * sizeof(uint32_t));
        read(c.descriptor.data(), c.descriptor.size());
    }
    return commands;
}
// Maintenance releases freed recordings in slices with short pauses. A sweep
// may free tens of thousands of lists (hundreds of MB of small blocks) at
// once, and such a burst slows every thread allocating from the process heap
// meanwhile, the game and render threads included. Paced, the memory returns
// within about a second instead of ~0.1 s.
// Sweep pacing (tests compare settings with gx2_dl_test_sweep_pacing).
struct SweepPacing {
    // Measured with [.dl-hitch-bench]: 4 MiB pieces, below-normal priority and
    // 256-list free slices made the worst game-thread stall larger, not smaller.
    uint64_t piece_bytes = uint64_t(16) << 20;  // scan read between pauses
    bool pause_scan = true, background = false; // 1 ms pause per piece; below-normal priority while reading
    size_t free_slice = 1024;                   // freed lists per 1 ms pause (0: all at once)
};
SweepPacing s_pacing;
// A dropped recording's commands and baked form, freed after the registry lock.
struct Retired {
    Commands commands;
    std::shared_ptr<const BakedList> baked;
};
void release_paced(std::vector<Retired>& retired) {
    const size_t kSlice = s_pacing.free_slice ? s_pacing.free_slice : ~size_t(0);
#if defined(_WIN32)
    thread_local HANDLE timer =
        CreateWaitableTimerExW(nullptr, nullptr, 0x2 /*HIGH_RESOLUTION*/, TIMER_MODIFY_STATE | SYNCHRONIZE);
#endif
    while (retired.size() > kSlice) {
        retired.resize(retired.size() - kSlice);
#if defined(_WIN32)
        LARGE_INTEGER due{};
        due.QuadPart = -10000; // 1 ms
        if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject(timer, INFINITE);
        else
            Sleep(1);
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
    }
    retired.clear();
}
void evict_old_recordings();
#ifdef RBREW_GX2_QUEUE_TESTS
std::function<void()> s_test_archive_gate;
std::function<void()> s_test_end_list_gate; // GX2EndDisplayList, between reuse lookup and publication
#endif

// Identical lists share one recording. The game re-records many lists every
// frame with unchanged contents (~1,200/s on a static loading screen, owner
// run 2026-10-17); each copy cost ~10-15 KB resident and was later written to
// the archive. A list whose commands equal a resident recording replays the
// same calls with the same arguments and payloads, so GX2EndDisplayList
// republishes that recording's id (tag, s_lists) instead of storing a copy.
// Direct-mapped by content hash: fixed size, a collision only loses a reuse.
struct ContentSlot {
    uint64_t hash = 0;
    uint32_t id = 0;
    bool used = false;
};
constexpr size_t kContentSlots = size_t(1) << 16;
std::array<ContentSlot, kContentSlots> s_content{}; // guarded by s_mutex
std::atomic<uint64_t> s_content_reuses{0};
uint64_t commands_hash(const std::vector<Command>& commands) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ commands.size();
    auto mix = [&](uint64_t v) {
        h = (h ^ v) * 0x100000001B3ull;
        h ^= h >> 29;
    };
    auto bytes = [&](const void* data, size_t size) {
        const auto* p = static_cast<const uint8_t*>(data);
        mix(size);
        size_t i = 0;
        for (; i + 8 <= size; i += 8) {
            uint64_t w;
            std::memcpy(&w, p + i, 8);
            mix(w);
        }
        if (i < size) {
            uint64_t w = 0;
            std::memcpy(&w, p + i, size - i);
            mix(w);
        }
    };
    for (const Command& c : commands) {
        mix(reinterpret_cast<uintptr_t>(c.fn));
        mix(reinterpret_cast<uintptr_t>(c.descriptor_fn));
        bytes(c.r.data(), sizeof(c.r));
        bytes(c.f.data(), sizeof(c.f));
        bytes(c.words.data(), c.words.size() * sizeof(uint32_t));
        bytes(c.descriptor.data(), c.descriptor.size());
    }
    return h;
}
// Bitwise: floating-point registers compare as their exact bits.
bool same_commands(const std::vector<Command>& a, const std::vector<Command>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const Command& x = a[i];
        const Command& y = b[i];
        if (x.fn != y.fn || x.descriptor_fn != y.descriptor_fn || x.r != y.r ||
            std::memcmp(x.f.data(), y.f.data(), sizeof(x.f)) != 0 || x.words != y.words || x.descriptor != y.descriptor)
            return false;
    }
    return true;
}

// RBREW_PERF [perf-mem] counters (cumulative; read without the registry lock).
std::atomic<uint64_t> s_eviction_count{0}, s_restore_count{0}, s_eviction_ns{0};
std::shared_ptr<const BakedList> bake_commands(const std::vector<Command>& commands);

bool use_recording(uint32_t id, Commands& out, std::shared_ptr<const BakedList>& baked_out) {
    Recording* recording = s_recordings.find(id);
    if (!recording || (recording->flags & kDead) || (!recording->commands && !recording->archived())) return false;
    if (!recording->commands) {
        recording->commands = restore_commands(recording->archive_offset);
        recording->baked.reset();
        ++s_resident_recordings;
        s_resident_bytes += recording->bytes;
        s_restore_count.fetch_add(1, std::memory_order_relaxed);
    }
    if (!recording->baked && recording->commands) {
        recording->baked = bake_commands(*recording->commands);
    }
    recording->last_use = ++s_use_clock;
    recording->flags |= kUsed;
    out = recording->commands;
    baked_out = recording->baked;
    if (s_resident_recordings > kMaxRecordings || s_resident_bytes > kMaxResidentBytes) evict_old_recordings();
    return true;
}
bool use_recording(uint32_t id, Commands& out) {
    std::shared_ptr<const BakedList> dummy;
    return use_recording(id, out, dummy);
}

void evict_old_recordings() {
    if ((s_resident_recordings <= kMaxRecordings && s_resident_bytes <= kMaxResidentBytes) || s_eviction_inflight)
        return;
    struct Timed {
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        ~Timed() {
            s_eviction_ns.fetch_add(
                uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
                             .count()),
                std::memory_order_relaxed);
            s_eviction_count.fetch_add(1, std::memory_order_relaxed);
        }
    } timed;
    std::vector<uint64_t> uses;
    uses.reserve(s_resident_recordings);
    for (const uint32_t id : s_tracked)
        if (const Recording* r = s_recordings.find(id); r && r->commands) uses.push_back(r->last_use);
    auto median = uses.begin() + uses.size() / 2;
    std::nth_element(uses.begin(), median, uses.end());
    const uint64_t cutoff = *median;
    struct Candidate {
        Recording* resident;
        Recording snapshot;
    };
    auto cold = std::make_shared<std::vector<Candidate>>();
    cold->reserve(uses.size() / 2);
    // Table chunks keep entry addresses (a chunk is only released once all of
    // its recordings are dead, and a candidate has resident commands). Cold
    // live recordings go to the archive: copied guest tags may name them.
    for (const uint32_t id : s_tracked)
        if (Recording* r = s_recordings.find(id); r && r->commands && r->last_use < cutoff) cold->push_back({r, *r});
#ifdef RBREW_GX2_QUEUE_TESTS
    const auto gate = s_test_archive_gate;
#endif
    command_retirement().retire([cold
#ifdef RBREW_GX2_QUEUE_TESTS
                                 ,
                                 gate
#endif
    ]() -> size_t {
        struct Completion {
            ~Completion() {
                std::lock_guard lock(s_mutex);
                s_eviction_inflight = false;
            }
        } completion;
#ifdef RBREW_GX2_QUEUE_TESTS
        if (gate) gate();
#endif
        std::vector<Retired> retired;
        retired.reserve(cold->size());
        // Never acquire the recording registry while holding the archive
        // mutex. A cold-list restore holds them in the opposite order.
        bool ok = false;
        uint64_t committed_end = 0;
        {
            std::lock_guard archive_lock(s_archive_mutex);
            if (!s_archive) {
                s_archive.reset(std::tmpfile());
                if (s_archive) std::setvbuf(s_archive.get(), s_archive_buffer.data(), _IOFBF, s_archive_buffer.size());
            }
            committed_end = s_archive_end;
            ok = s_archive != nullptr;
        }
        // Written in small locked chunks. A restore runs on the render thread
        // while it holds the registry, so it - and every guest thread waiting
        // on the registry - used to wait for the whole batch (~130k lists,
        // GBs: a 10 s stall in owner run 2026-10-17). Now it waits for one
        // chunk at most. Only this job appends; offsets are published below.
        constexpr size_t kChunkLists = 64;
        for (size_t next = 0; ok && next < cold->size();) {
            std::lock_guard archive_lock(s_archive_mutex);
            if (!s_archive_at_end) s_archive_at_end = ok = archive_seek(s_archive_end, SEEK_SET);
            for (size_t n = 0; ok && n < kChunkLists && next < cold->size(); ++n, ++next)
                ok = archive_commands((*cold)[next].snapshot);
        }
        {
            std::lock_guard archive_lock(s_archive_mutex);
            ok = ok && std::fflush(s_archive.get()) == 0;
            if (!ok) {
                s_archive_end = committed_end;
                s_archive_at_end = false;
                if (s_archive) std::clearerr(s_archive.get());
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    RBREW_LOG_ERROR("gx2", "cannot archive cold display lists; retaining their commands\n");
                }
            }
        }
        if (ok) {
            std::lock_guard lock(s_mutex);
            for (auto& candidate : *cold) {
                auto& resident = *candidate.resident;
                resident.archive_offset = candidate.snapshot.archive_offset;
                // A list used since selection stays hot. Its immutable backing
                // is still useful for the next eviction, without re-writing it.
                if (resident.last_use != candidate.snapshot.last_use ||
                    resident.commands != candidate.snapshot.commands)
                    continue;
                retired.push_back({std::move(resident.commands), std::move(resident.baked)});
                --s_resident_recordings;
                s_resident_bytes -= resident.bytes;
            }
        }
        const size_t count = retired.size();
        release_paced(retired);
        cold->clear(); // all final releases are outside s_mutex
        return count;
    });
    s_eviction_inflight = true;
}
// Tag written into the game buffer at GX2EndDisplayList: a type-3 NOP packet
// (valid command data the GPU would skip) carrying a magic and the id, so a
// list the game copies elsewhere (memcpy of the bytes) can still be replayed.
constexpr uint32_t kTagNop = 0xC0011000u, kTagMagic = 0x5242444Cu; // NOP, 2 words; "RBDL"
std::unordered_set<uint32_t> s_logged;

// Sweeps: freeing lists nothing can call any more.
//
// A recording is reachable only through a tag naming its id in guest memory
// (its own buffer or a copy the game made of the bytes), through s_lists (the
// latest list of a buffer), or through a call already queued (which holds its
// commands). The game records thousands of lists per second, calls them once
// and overwrites their buffers (owner run 2026-10-17: ~4,600/s in the hub,
// 10-40 KB each); keeping them all filled RAM and wrote GBs to the archive.
//
// A sweep scans every guest page written since the arena was reserved (pages
// never written cannot hold a tag; reading them would make Windows back them
// with RAM) for tags, without the registry lock. A tag cannot appear from
// nothing: a copy needs a source that still holds it. So a recording that no
// scan finds, that is not the latest of its buffer, and that was not called,
// is unreachable. A copy moved during the scan from an unscanned to a scanned
// address while its source was overwritten could hide once; requiring two
// consecutive sweeps to miss it (and no call in between) covers that.
// Unreadable written pages (an unmapped overlay) make a sweep inconclusive:
// it frees nothing.
constexpr uint32_t kSweepLists = 65536;               // new lists between sweeps
constexpr uint64_t kSweepBytes = uint64_t(256) << 20; // or growth of resident bytes
bool s_sweep_inflight = false;                        // guarded by s_mutex
uint32_t s_sweep_last_id = 0;
uint64_t s_sweep_last_bytes = 0;
const uint8_t* s_guest_mem = nullptr; // arena of the recorded lists
uint64_t s_scan_size = WIIU_MEM_SIZE;
bool s_sweeps_enabled = true;
std::atomic<uint64_t> s_sweep_count{0}, s_swept_lists{0}, s_sweep_ns{0}, s_sweep_inconclusive{0};

void mark_tags(const uint8_t* begin, size_t bytes, uint32_t limit, uint64_t* live) {
    const uint32_t nop = rbrew_bswap32(kTagNop), magic = rbrew_bswap32(kTagMagic);
    const auto* words = reinterpret_cast<const uint32_t*>(begin);
    const size_t count = bytes / 4;
    auto check = [&](size_t i) {
        if (i + 2 >= count || words[i + 1] != magic) return;
        const uint32_t id = rbrew_bswap32(words[i + 2]);
        if (id < limit) live[id / 64] |= 1ull << (id % 64);
    };
    size_t i = 0;
#if defined(_M_X64) || defined(__x86_64__)
    const __m128i wanted = _mm_set1_epi32(int(nop));
    for (; i + 4 <= count; i += 4) {
        int mask = _mm_movemask_ps(
            _mm_castsi128_ps(_mm_cmpeq_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(words + i)), wanted)));
        while (mask) {
            check(i + unsigned(std::countr_zero(unsigned(mask))));
            mask &= mask - 1;
        }
    }
#endif
    for (; i < count; ++i)
        if (words[i] == nop) check(i);
}
// False if the memory faulted (a protection change during the scan).
bool mark_tags_guarded(const uint8_t* begin, size_t bytes, uint32_t limit, uint64_t* live) {
#if defined(_WIN32)
    __try {
        mark_tags(begin, bytes, limit, live);
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                 : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
    return true;
#else
    mark_tags(begin, bytes, limit, live);
    return true;
#endif
}
bool readable(const uint8_t* p, size_t bytes) {
#if defined(_WIN32)
    for (const uint8_t* end = p + bytes; p < end;) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(p, &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
            !(info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
            return false;
        p = static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize;
    }
#endif
    (void)p;
    (void)bytes;
    return true;
}
// Ids < limit named by a tag anywhere in written guest memory. False when the
// scan is inconclusive. The OS is only asked which pages were ever written
// (never reset: resetting write watch over the arena each sweep took 20-25 ms
// with the renderer's lock held per slice and caused a stutter per sweep,
// 2026-10-17). The read itself (~3 GB) is paced (s_pacing: 16 MiB pieces, 1 ms
// pauses): read at full speed it slowed memory-bound game and render threads
// by 10-30% for its ~170 ms (scratch bench 2026-10-17). No lock is held while
// reading.
#ifdef RBREW_GX2_QUEUE_TESTS
std::vector<std::pair<const char*, std::chrono::steady_clock::time_point>> s_test_sweep_phases;
#define SWEEP_PHASE(name) s_test_sweep_phases.emplace_back(name, std::chrono::steady_clock::now())
#else
#define SWEEP_PHASE(name) ((void)0)
#endif
void pause_scan() {
#if defined(_WIN32)
    thread_local HANDLE timer =
        CreateWaitableTimerExW(nullptr, nullptr, 0x2 /*HIGH_RESOLUTION*/, TIMER_MODIFY_STATE | SYNCHRONIZE);
    LARGE_INTEGER due{};
    due.QuadPart = -10000; // 1 ms
    if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
        WaitForSingleObject(timer, INFINITE);
    else
        Sleep(1);
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
}
bool scan_live_tags(const uint8_t* mem, uint64_t size, uint32_t limit, std::vector<uint64_t>& live) {
    live.assign(size_t(limit / 64 + 1), 0);
    std::vector<uint64_t> pages;
    if (!gx2_write_watch_written_pages(mem, size, pages)) {
        // No write watch (test arenas): small enough to read entirely.
        if (size > (uint64_t(64) << 20) || !readable(mem, size_t(size))) return false;
        return mark_tags_guarded(mem, size_t(size), limit, live.data());
    }
    SWEEP_PHASE("ww-query");
    constexpr uint64_t kPage = 4096;
    const uint64_t kPiecePages = std::max<uint64_t>(1, s_pacing.piece_bytes / kPage);
    const uint64_t page_count = (size + kPage - 1) / kPage;
    uint64_t since_pause = 0;
#if defined(_WIN32)
    struct Background {
        const bool on = s_pacing.background;
        Background() {
            if (on) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        }
        ~Background() {
            if (on) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
        }
    } background; // restored before the sweep takes any lock again
#endif
    for (uint64_t page = 0; page < page_count;) {
        if (!(pages[page / 64] >> (page % 64) & 1)) {
            ++page;
            continue;
        }
        uint64_t end = page + 1;
        while (end < page_count && end - page < kPiecePages && (pages[end / 64] >> (end % 64) & 1)) ++end;
        // A tag may straddle into the next written page: read two more words
        // when the run was cut at the piece size.
        const uint64_t ahead = end < page_count && (pages[end / 64] >> (end % 64) & 1) ? 8 : 0;
        const uint8_t* begin = mem + page * kPage;
        const size_t bytes = size_t(std::min(end * kPage + ahead, size) - page * kPage);
        // No VirtualQuery per piece: it walks the page tables of the whole
        // remaining region under the process memory lock, and the renderer's
        // write-watch queries waited 8-13 ms behind it (dl-hitch-bench
        // 2026-10-17). Guest pages are read-write or no-access; a no-access or
        // decommitted page faults inside the guard and makes the sweep
        // inconclusive, as readable() did.
        if (!mark_tags_guarded(begin, bytes, limit, live.data())) return false;
        since_pause += end - page;
        if (since_pause >= kPiecePages) {
            since_pause = 0;
            if (s_pacing.pause_scan) pause_scan();
        }
        page = end;
    }
    return true;
}
void sweep_recordings(uint32_t limit) {
    TXDIAG(txdiag::event("display_list_sweep_begin", "\"limit\":%u", limit));
    SWEEP_PHASE("start");
    const auto start = std::chrono::steady_clock::now();
    struct Completion {
        ~Completion() {
            std::lock_guard lock(s_mutex);
            s_sweep_inflight = false;
            s_sweep_last_bytes = s_resident_bytes;
        }
    } completion;
    const uint8_t* mem;
    uint64_t size;
    {
        std::lock_guard lock(s_mutex);
        mem = s_guest_mem;
        size = s_scan_size;
    }
    std::vector<uint64_t> live;
    if (!mem || !scan_live_tags(mem, size, limit, live)) {
        s_sweep_inconclusive.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    SWEEP_PHASE("scanned");
    std::vector<Retired> retired; // released outside the registry lock
    size_t scanned = 0;           // s_tracked entries present when the pass starts
    // Both passes run in short locked chunks and let a waiting game/render
    // caller take the registry between chunks (std::mutex is not fair).
    auto yield_to_waiters = [] {
        // Bounded: a waiter that is not running cannot hold up the sweep.
        const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(200);
        for (unsigned n = 1; s_registry_waiters.load(std::memory_order_relaxed); ++n) {
            if (n % 64 == 0 && std::chrono::steady_clock::now() >= until) break;
#if defined(_M_X64) || defined(__x86_64__)
            _mm_pause();
#endif
        }
    };
    std::vector<std::pair<uint32_t, uint32_t>> latest; // s_lists when the pass starts
    {
        std::lock_guard lock(s_mutex);
        latest.assign(s_lists.begin(), s_lists.end());
        scanned = s_tracked.size();
    }
    SWEEP_PHASE("snapshot");
    // The latest list of each buffer stays callable by address. A buffer
    // whose tag header was overwritten no longer holds that list (a patched
    // list keeps its header: GX2PatchDisplayList writes at +8). An entry that
    // changed since the snapshot names a new list (>= limit) or a republished
    // one (kUsed): either stays alive without this pass.
    constexpr size_t kChunkLists = 2048;
    for (size_t next = 0; next < latest.size();) {
        if (next) yield_to_waiters();
        std::lock_guard lock(s_mutex);
        for (const size_t end = std::min(latest.size(), next + kChunkLists); next < end; ++next) {
            const auto [buffer, id] = latest[next];
            const auto it = s_lists.find(buffer);
            if (it == s_lists.end() || it->second != id) continue;
            Recording* r = s_recordings.find(id);
            if (!r || (r->flags & kDead)) {
                s_lists.erase(it);
                continue;
            }
            if (id < limit && (r->flags & kTagged) &&
                !(rbrew_memory_accessible(const_cast<uint8_t*>(mem), buffer, 8) &&
                  rbrew_read32(const_cast<uint8_t*>(mem), buffer) == kTagNop &&
                  rbrew_read32(const_cast<uint8_t*>(mem), buffer + 4) == kTagMagic)) {
                s_lists.erase(it);
                continue;
            }
            if (id < limit) live[id / 64] |= 1ull << (id % 64);
        }
    }
    latest = {};
    SWEEP_PHASE("lists");
    // The tracked recordings are visited in short locked chunks: the game
    // thread records and calls lists through this registry every frame and
    // waited for the whole pass (3.4 ms for 200k lists). Between chunks other
    // threads only append new ids (>= limit) to s_tracked, and a recording
    // used meanwhile is referenced (kUsed) as if used before the pass.
    // Visited entries stay in s_tracked until the survivors replace it.
    std::vector<uint32_t> kept;
    kept.reserve(scanned);
    retired.reserve(scanned); // no reallocation of moved commands under the lock
    constexpr size_t kChunkIds = 2048;
    for (size_t next = 0; next < scanned;) {
        yield_to_waiters();
        std::lock_guard lock(s_mutex);
        for (const size_t end = std::min(scanned, next + kChunkIds); next < end; ++next) {
            const uint32_t id = s_tracked[next];
            Recording* r = s_recordings.find(id);
            if (!r) continue;
            if (id >= limit) {
                kept.push_back(id);
                continue;
            }
            const bool referenced = (live[id / 64] >> (id % 64) & 1) || (r->flags & kUsed);
            r->flags &= uint8_t(~kUsed);
            if (referenced) {
                r->flags &= uint8_t(~kMissed);
                kept.push_back(id);
                continue;
            }
            if (!(r->flags & kMissed)) {
                r->flags |= kMissed;
                kept.push_back(id);
                continue;
            }
            if (r->commands) {
                retired.push_back({std::move(r->commands), std::move(r->baked)});
                --s_resident_recordings;
                s_resident_bytes -= r->bytes;
            }
            r->flags = kDead;
            r->archive_offset = kNotArchived;
            s_recordings.note_dead(id);
            s_swept_lists.fetch_add(1, std::memory_order_relaxed);
        }
    }
    {
        std::lock_guard lock(s_mutex);
        kept.insert(kept.end(), s_tracked.begin() + std::ptrdiff_t(scanned), s_tracked.end());
        SWEEP_PHASE("tracked");
        s_tracked.swap(kept);
        if (!s_eviction_inflight) s_recordings.release_dead_chunks();
    }
    release_paced(retired);
    SWEEP_PHASE("freed");
    s_sweep_count.fetch_add(1, std::memory_order_relaxed);
    TXDIAG(txdiag::event("display_list_sweep_end", "\"limit\":%u,\"swept_total\":%llu", limit, s_swept_lists.load()));
    s_sweep_ns.fetch_add(
        uint64_t(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count()),
        std::memory_order_relaxed);
}
// Caller holds s_mutex.
void maybe_sweep() {
    if (!s_sweeps_enabled || s_sweep_inflight || !s_guest_mem) return;
    if (s_next_id - s_sweep_last_id < kSweepLists && s_resident_bytes < s_sweep_last_bytes + kSweepBytes) return;
    s_sweep_inflight = true;
    s_sweep_last_id = s_next_id;
    const uint32_t limit = s_next_id;
    command_retirement().retire([limit]() -> size_t {
        sweep_recordings(limit);
        return 0;
    });
}

// Mirrors the core's builder state so the per-call recording check needs no lock.
// Only the core that owns a list (or the render thread while that core waits
// on it) opens or closes it.
std::array<std::atomic<bool>, 4> s_open_active{};
std::atomic<bool>& open_active() {
    return s_open_active[builder_slot()];
}

// Size reported by GX2EndDisplayList: the game only hands it back to
// GX2CallDisplayList, so it stands for the recorded commands (32 bytes each,
// like a small packet), never more than the buffer the game gave us.
uint32_t recorded_size(const OpenList& list) {
    const uint64_t bytes = uint64_t(list.commands.size() ? list.commands.size() : 1) * 32u;
    return uint32_t(bytes < list.capacity ? bytes : list.capacity);
}

Gx2CommandFn s_draw_indexed_ex_fn = nullptr;
Gx2CommandFn s_draw_indexed_ex2_fn = nullptr;
Gx2CommandFn s_draw_ex_fn = nullptr;
Gx2BakedHotCommands s_hot_cmds{};

uint32_t be_word(const uint8_t* d) {
    return (uint32_t(d[0]) << 24) | (uint32_t(d[1]) << 16) | (uint32_t(d[2]) << 8) | uint32_t(d[3]);
}

thread_local std::array<CPUState, 20> t_replay_cpus{};
thread_local unsigned t_replay_slot = 0;
struct ReplayCpuGuard {
    CPUState& state;
    explicit ReplayCpuGuard(const CPUState* src) noexcept : state(t_replay_cpus[t_replay_slot++]) {
        state.r[1] = src->r[1];
        state.lr = src->lr;
        state.mem = src->mem;
        state.guest_memory_size = src->guest_memory_size;
        state.dispatch_table = src->dispatch_table;
        state.dispatch_size = src->dispatch_size;
    }
    ~ReplayCpuGuard() noexcept { --t_replay_slot; }
};

std::shared_ptr<const BakedList> bake_commands(const std::vector<Command>& commands) {
    auto baked = std::make_shared<BakedList>();
    baked->ops.reserve(commands.size());
    for (const Command& c : commands) {
        if (!c.fn && !c.descriptor_fn) {
            BakedOp op{};
            op.kind = BakedOpKind::UniformWords;
            op.is_pixel = (c.r[0] != 0);
            op.arg0 = c.r[1];
            op.word_count = uint16_t(c.words.size());
            op.arg6 = uint32_t(baked->word_pool.size());
            baked->word_pool.insert(baked->word_pool.end(), c.words.begin(), c.words.end());
            baked->ops.push_back(op);
        } else if (c.fn && c.fn == s_draw_indexed_ex_fn) {
            BakedOp op{};
            op.kind = BakedOpKind::DrawIndexedEx;
            op.arg0 = c.r[0];
            op.arg1 = c.r[1];
            op.arg2 = c.r[2];
            op.arg3 = c.r[3];
            op.arg4 = c.r[4];
            op.arg5 = c.r[5];
            baked->ops.push_back(op);
        } else if (c.fn && c.fn == s_draw_indexed_ex2_fn) {
            BakedOp op{};
            op.kind = BakedOpKind::DrawIndexedEx2;
            op.arg0 = c.r[0];
            op.arg1 = c.r[1];
            op.arg2 = c.r[2];
            op.arg3 = c.r[3];
            op.arg4 = c.r[4];
            op.arg5 = c.r[5];
            op.arg6 = c.r[6];
            baked->ops.push_back(op);
        } else if (c.fn && c.fn == s_draw_ex_fn) {
            BakedOp op{};
            op.kind = BakedOpKind::DrawEx;
            op.arg0 = c.r[0];
            op.arg1 = c.r[1];
            op.arg2 = c.r[2];
            op.arg3 = c.r[3];
            baked->ops.push_back(op);
        } else if (c.fn && c.fn == s_hot_cmds.set_attrib_buffer) {
            BakedOp op{};
            op.kind = BakedOpKind::SetAttribBuffer;
            op.arg0 = c.r[0];
            op.arg1 = c.r[1];
            op.arg2 = c.r[2];
            op.arg3 = c.r[3];
            baked->ops.push_back(op);
        } else if (c.fn && (c.fn == s_hot_cmds.set_vs_ubo || c.fn == s_hot_cmds.set_ps_ubo)) {
            BakedOp op{};
            op.kind = BakedOpKind::SetUniformBlock;
            op.is_pixel = (c.fn == s_hot_cmds.set_ps_ubo) ? 1 : 0;
            op.arg0 = c.r[0];
            op.arg1 = c.r[1];
            op.arg2 = c.r[2];
            baked->ops.push_back(op);
        } else if (c.fn && (c.fn == s_hot_cmds.set_vs || c.fn == s_hot_cmds.set_ps)) {
            BakedOp op{};
            op.kind = BakedOpKind::SetShader;
            op.is_pixel = (c.fn == s_hot_cmds.set_ps) ? 1 : 0;
            op.arg0 = c.r[0];
            baked->ops.push_back(op);
        } else if (c.descriptor_fn &&
                   (c.descriptor_fn == s_hot_cmds.set_ps_sampler || c.descriptor_fn == s_hot_cmds.set_vs_sampler) &&
                   c.r[1] < 18 && c.descriptor.size() >= 12) {
            const bool is_ps = (c.descriptor_fn == s_hot_cmds.set_ps_sampler);
            const uint8_t* d = c.descriptor.data();
            BakedOp op{};
            op.kind = BakedOpKind::SetSampler;
            op.is_pixel = is_ps ? 1 : 0;
            op.arg0 = 0xF000u + ((is_ps ? 0u : 1u) * 18u + c.r[1]) * 3u;
            op.arg1 = be_word(d);
            op.arg2 = be_word(d + 4);
            op.arg3 = be_word(d + 8);
            baked->ops.push_back(op);
        } else if (c.descriptor_fn &&
                   (c.descriptor_fn == s_hot_cmds.set_ps_tex || c.descriptor_fn == s_hot_cmds.set_vs_tex)) {
            BakedOp op{};
            op.kind = BakedOpKind::SetTexture;
            op.is_pixel = (c.descriptor_fn == s_hot_cmds.set_ps_tex) ? 1 : 0;
            op.arg0 = c.r[0];
            op.arg1 = c.r[1];
            op.arg6 = uint32_t(baked->desc_pool.size());
            baked->desc_pool.insert(baked->desc_pool.end(), c.descriptor.begin(), c.descriptor.end());
            baked->ops.push_back(op);
        } else if (c.descriptor_fn) {
            BakedOp op{};
            op.kind = BakedOpKind::Descriptor;
            op.arg0 = uint32_t(baked->call_pool.size());
            const uint32_t desc_off = uint32_t(baked->desc_pool.size());
            baked->desc_pool.insert(baked->desc_pool.end(), c.descriptor.begin(), c.descriptor.end());
            baked->call_pool.push_back({nullptr, c.descriptor_fn, desc_off, c.r, c.f});
            baked->ops.push_back(op);
        } else {
            BakedOp op{};
            op.kind = BakedOpKind::Generic;
            op.arg0 = uint32_t(baked->call_pool.size());
            baked->call_pool.push_back({c.fn, nullptr, 0, c.r, c.f});
            baked->ops.push_back(op);
        }
    }
    return baked;
}

void replay_baked(CPUState* cpu, const BakedList& baked, unsigned depth) {
    if (depth > 16 || t_replay_slot >= t_replay_cpus.size()) return;
    ReplayCpuGuard guard(cpu);
    CPUState& call = guard.state;
    const bool perf = gx2_perf_enabled();
    static const bool poll = [] {
        const char* v = std::getenv("RBREW_LIST_QUERY_POLL");
        return !(v && v[0] == '0');
    }();
    unsigned since_poll = 0;
    const auto* ops = baked.ops.data();
    const size_t n_ops = baked.ops.size();
    const auto* calls = baked.call_pool.data();
    const auto* words = baked.word_pool.data();
    const auto* descs = baked.desc_pool.data();
    uint32_t draws_done = 0;

    for (size_t i = 0; i < n_ops; ++i) {
        const auto& c = ops[i];
        if (poll && ++since_poll >= 128) {
            since_poll = 0;
            gx2_render_query_poll_completed();
        }
        switch (c.kind) {
        case BakedOpKind::DrawIndexedEx: {
            const uint64_t start = perf ? gx2_perf_now() : 0;
            ++draws_done;
            gx2_render_draw_indexed(c.arg0, c.arg1, c.arg2, c.arg3, c.arg4, call.mem, c.arg5);
            if (perf) gx2_perf_function(0, reinterpret_cast<const void*>(s_draw_indexed_ex_fn), gx2_perf_now() - start);
            break;
        }
        case BakedOpKind::DrawIndexedEx2: {
            const uint64_t start = perf ? gx2_perf_now() : 0;
            ++draws_done;
            gx2_render_draw_indexed(c.arg0, c.arg1, c.arg2, c.arg3, c.arg4, call.mem, c.arg5, c.arg6);
            if (perf)
                gx2_perf_function(0, reinterpret_cast<const void*>(s_draw_indexed_ex2_fn), gx2_perf_now() - start);
            break;
        }
        case BakedOpKind::DrawEx: {
            const uint64_t start = perf ? gx2_perf_now() : 0;
            ++draws_done;
            gx2_render_draw(c.arg0, c.arg1, c.arg2, call.mem, c.arg3);
            if (perf) gx2_perf_function(0, reinterpret_cast<const void*>(s_draw_ex_fn), gx2_perf_now() - start);
            break;
        }
        case BakedOpKind::UniformWords: {
            gx2_render_set_uniform_words(c.is_pixel != 0, c.arg0, words + c.arg6, c.word_count);
            break;
        }
        case BakedOpKind::SetAttribBuffer: {
            const uint64_t start = perf ? gx2_perf_now() : 0;
            gx2_render_set_attrib_buffer(c.arg0, c.arg3, c.arg1, c.arg2);
            if (perf)
                gx2_perf_function(0, reinterpret_cast<const void*>(s_hot_cmds.set_attrib_buffer),
                                  gx2_perf_now() - start);
            break;
        }
        case BakedOpKind::SetUniformBlock: {
            const uint64_t start = perf ? gx2_perf_now() : 0;
            gx2_render_set_uniform_block(c.is_pixel != 0, c.arg0, c.arg2, c.arg1, call.mem);
            if (perf)
                gx2_perf_function(
                    0, reinterpret_cast<const void*>(c.is_pixel ? s_hot_cmds.set_ps_ubo : s_hot_cmds.set_vs_ubo),
                    gx2_perf_now() - start);
            break;
        }
        case BakedOpKind::SetShader: {
            const uint64_t start = perf ? gx2_perf_now() : 0;
            if (c.is_pixel)
                gx2_render_set_pixel_shader(c.arg0, call.mem);
            else
                gx2_render_set_vertex_shader(c.arg0, call.mem);
            if (perf)
                gx2_perf_function(0, reinterpret_cast<const void*>(c.is_pixel ? s_hot_cmds.set_ps : s_hot_cmds.set_vs),
                                  gx2_perf_now() - start);
            break;
        }
        case BakedOpKind::SetSampler: {
            const uint64_t start = perf ? gx2_perf_now() : 0;
            const Gx2RegisterWrite reg_writes[3] = {{c.arg0, c.arg1}, {c.arg0 + 1, c.arg2}, {c.arg0 + 2, c.arg3}};
            gx2_render_apply_registers(reg_writes, 3);
            if (perf)
                gx2_perf_function(
                    0,
                    reinterpret_cast<const void*>(c.is_pixel ? s_hot_cmds.set_ps_sampler : s_hot_cmds.set_vs_sampler),
                    gx2_perf_now() - start);
            break;
        }
        case BakedOpKind::SetTexture: {
            const uint64_t start = perf ? gx2_perf_now() : 0;
            if (c.is_pixel) {
                if (g_gx2_frame_trace_active) {
                    call.r[3] = c.arg0;
                    call.r[4] = c.arg1;
                    s_hot_cmds.set_ps_tex(&call, descs + c.arg6);
                } else {
                    gx2_render_upload_texture(c.arg0, c.arg1, call.mem, descs + c.arg6);
                }
            } else {
                gx2_render_bind_texture(c.arg0, c.arg1 + 32, call.mem, descs + c.arg6);
            }
            if (perf)
                gx2_perf_function(
                    0, reinterpret_cast<const void*>(c.is_pixel ? s_hot_cmds.set_ps_tex : s_hot_cmds.set_vs_tex),
                    gx2_perf_now() - start);
            break;
        }
        case BakedOpKind::Descriptor: {
            const auto& bc = calls[c.arg0];
            std::memcpy(&call.r[3], bc.r.data(), sizeof(bc.r));
            std::memcpy(&call.f[1], bc.f.data(), sizeof(bc.f));
            const uint64_t start = perf ? gx2_perf_now() : 0;
            bc.desc_fn(&call, descs + bc.desc_offset);
            if (perf) gx2_perf_function(0, reinterpret_cast<const void*>(bc.desc_fn), gx2_perf_now() - start);
            break;
        }
        case BakedOpKind::Generic: {
            const auto& bc = calls[c.arg0];
            std::memcpy(&call.r[3], bc.r.data(), sizeof(bc.r));
            std::memcpy(&call.f[1], bc.f.data(), sizeof(bc.f));
            const uint64_t start = perf ? gx2_perf_now() : 0;
            bc.fn(&call);
            if (perf) gx2_perf_function(0, reinterpret_cast<const void*>(bc.fn), gx2_perf_now() - start);
            break;
        }
        }
    }
    if (draws_done) rbrew_benchmark::guest_draws.fetch_add(draws_done, std::memory_order_relaxed);
}

void replay(CPUState* cpu, const std::vector<Command>& commands, unsigned depth) {
    if (depth > 16 || t_replay_slot >= t_replay_cpus.size()) return;
    ReplayCpuGuard guard(cpu);
    CPUState& call = guard.state;
    const bool perf = gx2_perf_enabled();
    // The plaza's scene list replays for ~11.6 ms in one piece while the game
    // polls occlusion results of the previous frame: the GPU had them, but
    // they were published only between render items, so the read drained the
    // whole list (10 ms of the game thread per frame). Publish finished
    // results while a long list replays. RBREW_LIST_QUERY_POLL=0 disables it.
    static const bool poll = [] {
        const char* v = std::getenv("RBREW_LIST_QUERY_POLL");
        return !(v && v[0] == '0');
    }();
    unsigned since_poll = 0;
    uint32_t draws_done = 0;
    for (const Command& c : commands) {
        if (poll && ++since_poll >= 128) {
            since_poll = 0;
            gx2_render_query_poll_completed();
        }
        if (!c.fn && !c.descriptor_fn) {
            gx2_render_set_uniform_words(c.r[0] != 0, c.r[1], c.words.data(), uint32_t(c.words.size()));
            continue;
        }
        if (c.fn == s_draw_indexed_ex_fn) {
            const uint64_t start = perf ? gx2_perf_now() : 0;
            ++draws_done;
            gx2_render_draw_indexed(c.r[0], c.r[1], c.r[2], c.r[3], c.r[4], call.mem, c.r[5]);
            if (perf) gx2_perf_function(0, reinterpret_cast<const void*>(c.fn), gx2_perf_now() - start);
            continue;
        }
        if (c.fn == s_draw_indexed_ex2_fn) {
            const uint64_t start = perf ? gx2_perf_now() : 0;
            ++draws_done;
            gx2_render_draw_indexed(c.r[0], c.r[1], c.r[2], c.r[3], c.r[4], call.mem, c.r[5], c.r[6]);
            if (perf) gx2_perf_function(0, reinterpret_cast<const void*>(c.fn), gx2_perf_now() - start);
            continue;
        }
        if (c.fn == s_draw_ex_fn) {
            const uint64_t start = perf ? gx2_perf_now() : 0;
            ++draws_done;
            gx2_render_draw(c.r[0], c.r[1], c.r[2], call.mem, c.r[3]);
            if (perf) gx2_perf_function(0, reinterpret_cast<const void*>(c.fn), gx2_perf_now() - start);
            continue;
        }
        for (unsigned i = 0; i < 8; ++i) {
            call.r[3 + i] = c.r[i];
            call.f[1 + i] = c.f[i];
        }
        const uint64_t start = perf ? gx2_perf_now() : 0;
        if (c.descriptor_fn)
            c.descriptor_fn(&call, c.descriptor.data());
        else
            c.fn(&call);
        if (perf)
            gx2_perf_function(0,
                              c.descriptor_fn ? reinterpret_cast<const void*>(c.descriptor_fn)
                                              : (c.fn ? reinterpret_cast<const void*>(c.fn)
                                                      : reinterpret_cast<const void*>(uintptr_t(1))),
                              gx2_perf_now() - start);
    }
    if (draws_done) rbrew_benchmark::guest_draws.fetch_add(draws_done, std::memory_order_relaxed);
}

// Commands issued outside a display list while the render thread runs: the
// same recorded form, posted to the render thread instead of waiting for it.
// GX2 command functions only use r3..r10, f1..f8, lr and the arena pointer.
thread_local bool t_stream_replay = false;
struct StreamCommand final : Gx2AsyncWork {
    Command command;
    uint8_t* mem;
    uint32_t lr;
    StreamCommand(Command&& c, uint8_t* m, uint32_t l) : command(std::move(c)), mem(m), lr(l) {}
    void run() override {
        thread_local CPUState call{};
        call.mem = mem;
        call.lr = lr;
        t_stream_replay = true;
        const bool perf = gx2_perf_enabled();
        try {
            const Command& c = command;
            if (!c.fn && !c.descriptor_fn) {
                gx2_render_set_uniform_words(c.r[0] != 0, c.r[1], c.words.data(), uint32_t(c.words.size()));
            } else if (c.fn == s_draw_indexed_ex_fn) {
                const uint64_t start = perf ? gx2_perf_now() : 0;
                rbrew_benchmark::guest_draws.fetch_add(1, std::memory_order_relaxed);
                gx2_render_draw_indexed(c.r[0], c.r[1], c.r[2], c.r[3], c.r[4], call.mem, c.r[5]);
                if (perf) gx2_perf_function(0, reinterpret_cast<const void*>(c.fn), gx2_perf_now() - start);
            } else if (c.fn == s_draw_indexed_ex2_fn) {
                const uint64_t start = perf ? gx2_perf_now() : 0;
                rbrew_benchmark::guest_draws.fetch_add(1, std::memory_order_relaxed);
                gx2_render_draw_indexed(c.r[0], c.r[1], c.r[2], c.r[3], c.r[4], call.mem, c.r[5], c.r[6]);
                if (perf) gx2_perf_function(0, reinterpret_cast<const void*>(c.fn), gx2_perf_now() - start);
            } else if (c.fn == s_draw_ex_fn) {
                const uint64_t start = perf ? gx2_perf_now() : 0;
                rbrew_benchmark::guest_draws.fetch_add(1, std::memory_order_relaxed);
                gx2_render_draw(c.r[0], c.r[1], c.r[2], call.mem, c.r[3]);
                if (perf) gx2_perf_function(0, reinterpret_cast<const void*>(c.fn), gx2_perf_now() - start);
            } else {
                for (unsigned i = 0; i < 8; ++i) {
                    call.r[3 + i] = command.r[i];
                    call.f[1 + i] = command.f[i];
                }
                const uint64_t start = perf ? gx2_perf_now() : 0;
                if (c.descriptor_fn)
                    c.descriptor_fn(&call, c.descriptor.data());
                else
                    c.fn(&call);
                if (perf)
                    gx2_perf_function(0,
                                      c.descriptor_fn ? reinterpret_cast<const void*>(c.descriptor_fn)
                                                      : reinterpret_cast<const void*>(c.fn),
                                      gx2_perf_now() - start);
            }
        } catch (...) {
            t_stream_replay = false;
            throw;
        }
        t_stream_replay = false;
    }
};

// A display list called outside an open list: resolved on the caller (the
// game may overwrite or free the list buffer once the call returns), then
// replayed on the render thread.
void replay(CPUState* cpu, const std::vector<Command>& commands, unsigned depth);
struct ListCall final : Gx2AsyncWork {
    Commands commands;
    std::shared_ptr<const BakedList> baked;
    uint8_t* mem;
    uint32_t lr;
    ListCall(Commands c, std::shared_ptr<const BakedList> b, uint8_t* m, uint32_t l)
        : commands(std::move(c)), baked(std::move(b)), mem(m), lr(l) {}
    void run() override {
        Gx2PerfScope perf_scope(kPerfDisplayList);
        thread_local CPUState call{};
        call.mem = mem;
        call.lr = lr;
        t_stream_replay = true;
        try {
            if (baked)
                replay_baked(&call, *baked, 1);
            else if (commands)
                replay(&call, *commands, 1);
        } catch (...) {
            t_stream_replay = false;
            throw;
        }
        t_stream_replay = false;
    }
};
bool find_recording(uint8_t* mem, uint32_t list, uint32_t size, Commands& out,
                    std::shared_ptr<const BakedList>& baked_out);
bool find_recording(uint8_t* mem, uint32_t list, uint32_t size, Commands& out);
// GX2CallDisplayList / GX2DirectCallDisplayList, set at registration.
Gx2CommandFn s_list_call_fns[2] = {};

// Append to this core's open display list, or post to the render thread.
void push_command(Command&& c, uint8_t* mem, uint32_t lr) {
    const auto core = builder_slot();
    // A core's private builder has no dependency on published recordings or
    // their archive. Renderer lookups and eviction must not block each append.
    if (s_open_active[core].load(std::memory_order_acquire) || !gx2_native_async_active()) {
        std::lock_guard lock(s_open_mutex[core]);
        s_open[core].commands.push_back(std::move(c));
        return;
    }
    if (c.fn && (c.fn == s_list_call_fns[0] || c.fn == s_list_call_fns[1])) {
        Commands recorded;
        std::shared_ptr<const BakedList> baked;
        if (find_recording(mem, c.r[0], c.r[1], recorded, baked)) {
            gx2_native_post(new ListCall(std::move(recorded), std::move(baked), mem, lr));
            return;
        }
    }
    gx2_native_post(new StreamCommand(std::move(c), mem, lr));
}

thread_local unsigned t_depth = 0;

bool find_recording(uint8_t* mem, uint32_t list, uint32_t size, Commands& out,
                    std::shared_ptr<const BakedList>& baked_out) {
    if (size >= 12 && rbrew_memory_accessible(mem, list, 12) && rbrew_read32(mem, list) == kTagNop &&
        rbrew_read32(mem, list + 4) == kTagMagic) {
        const uint32_t id = rbrew_read32(mem, list + 8);
        struct DlMru {
            uint32_t id = UINT32_MAX;
            Commands commands;
            std::shared_ptr<const BakedList> baked;
        };
        thread_local DlMru t_find_mru[16]{};
        const size_t slot = id & 15;
        if (t_find_mru[slot].id == id && (t_find_mru[slot].commands || t_find_mru[slot].baked)) {
            out = t_find_mru[slot].commands;
            baked_out = t_find_mru[slot].baked;
            return true;
        }
        RegistryLock lock;
        if (use_recording(id, out, baked_out)) {
            t_find_mru[slot] = {id, out, baked_out};
            return true;
        }
        return false;
    }
    RegistryLock lock;
    if (auto it = s_lists.find(list); it != s_lists.end()) return use_recording(it->second, out, baked_out);
    return false;
}
bool find_recording(uint8_t* mem, uint32_t list, uint32_t size, Commands& out) {
    std::shared_ptr<const BakedList> dummy;
    return find_recording(mem, list, size, out, dummy);
}

void call_display_list(CPUState* cpu, const char* api) {
    Gx2PerfScope perf_scope(kPerfDisplayList);
    const uint32_t list = ARG0, size = ARG1;
    Commands recorded;
    std::shared_ptr<const BakedList> baked;
    bool found = false;
    if (size >= 12 && rbrew_memory_accessible(MEM, list, 12) && rbrew_read32(MEM, list) == kTagNop &&
        rbrew_read32(MEM, list + 4) == kTagMagic) {
        const uint32_t id = rbrew_read32(MEM, list + 8);
        struct DlMru {
            uint32_t id = UINT32_MAX;
            Commands commands;
            std::shared_ptr<const BakedList> baked;
        };
        thread_local DlMru t_call_mru[16]{};
        const size_t slot = id & 15;
        if (t_call_mru[slot].id == id && (t_call_mru[slot].commands || t_call_mru[slot].baked)) {
            recorded = t_call_mru[slot].commands;
            baked = t_call_mru[slot].baked;
            found = true;
        } else {
            RegistryLock lock;
            if (use_recording(id, recorded, baked)) {
                t_call_mru[slot] = {id, recorded, baked};
                found = true;
            }
        }
    } else {
        RegistryLock lock;
        if (auto it = s_lists.find(list); it != s_lists.end()) found = use_recording(it->second, recorded, baked);
    }
    if (found) {
        ++t_depth;
        if (baked)
            replay_baked(cpu, *baked, t_depth);
        else if (recorded)
            replay(cpu, *recorded, t_depth);
        --t_depth;
        return;
    }
    if (!list || !size || !rbrew_memory_accessible(MEM, list, size)) return;
    struct StaticListCache {
        uint32_t list = 0;
        uint32_t size = 0;
        uint32_t w0 = 0;
        uint32_t w1 = 0;
        std::vector<Gx2RegisterWrite> writes;
        bool ok = false;
    };
    static thread_local StaticListCache t_static_mru[4]{};
    const uint32_t w0 = rbrew_read32(MEM, list);
    const uint32_t w1 = size >= 8 ? rbrew_read32(MEM, list + 4) : 0;
    const size_t slot = ((list >> 4) ^ size) & 3;
    if (t_static_mru[slot].list == list && t_static_mru[slot].size == size && t_static_mru[slot].w0 == w0 &&
        t_static_mru[slot].w1 == w1 && t_static_mru[slot].ok) {
        gx2_render_apply_registers(t_static_mru[slot].writes.data(), t_static_mru[slot].writes.size());
        return;
    }
    const auto state = gx2_decode_static_display_list(MEM + list, size);
    TXDIAG(if (txdiag::sample("unrecorded_list", uint64_t(list) << 32 | size)) txdiag::event(
        "unrecorded_display_list",
        "\"address\":%u,\"size\":%u,\"decoded\":%s,\"register_writes\":%zu,\"other_packets\":%zu,\"lr\":%u", list, size,
        state.ok ? "true" : "false", state.writes.size(), state.other_opcodes.size(), cpu->lr));
    {
        std::lock_guard lock(s_mutex);
        if (s_logged.insert(list).second)
            RBREW_LOG_INFO_BEGIN("gx2",
                                 "%s unrecorded list=%08X size=%u decoded=%d writes=%zu other_packets=%zu "
                                 "words=%08X %08X %08X %08X lr=%08X\n",
                                 api, list, size, int(state.ok), state.writes.size(), state.other_opcodes.size(),
                                 rbrew_read32(MEM, list), rbrew_read32(MEM, list + 4), rbrew_read32(MEM, list + 8),
                                 rbrew_read32(MEM, list + 12), cpu->lr);
    }
    if (state.ok) {
        t_static_mru[slot] = {list, size, w0, w1, state.writes, true};
        gx2_render_apply_registers(state.writes.data(), state.writes.size());
    }
}
} // namespace

#ifdef RBREW_GX2_QUEUE_TESTS
void gx2_dl_test_with_recordings_locked(const std::function<void()>& action) {
    std::lock_guard lock(s_mutex);
    action();
}
std::pair<uint64_t, bool> gx2_dl_test_drain_retired_commands() {
    return command_retirement().drain_for_test();
}
void gx2_dl_test_archive_gate(std::function<void()> gate) {
    std::lock_guard lock(s_mutex);
    s_test_archive_gate = std::move(gate);
}
void gx2_dl_test_end_list_gate(std::function<void()> gate) {
    s_test_end_list_gate = std::move(gate);
}
std::vector<std::pair<const char*, std::chrono::steady_clock::time_point>> gx2_dl_test_take_sweep_phases() {
    return std::exchange(s_test_sweep_phases, {});
}
void gx2_dl_test_sweep_pacing(uint64_t piece_bytes, bool pause_scan, bool background, size_t free_slice) {
    s_pacing = SweepPacing{piece_bytes, pause_scan, background, free_slice};
}
// Sweeps for tests: enable/disable automatic ones, the guest extent to scan
// (test arenas have no write watch), and one synchronous sweep.
void gx2_dl_test_configure_sweeps(bool automatic, uint64_t scan_size) {
    std::lock_guard lock(s_mutex);
    s_sweeps_enabled = automatic;
    s_scan_size = scan_size ? scan_size : WIIU_MEM_SIZE;
}
void gx2_dl_test_sweep_now() {
    command_retirement().drain_for_test();
    uint32_t limit;
    {
        std::lock_guard lock(s_mutex);
        if (s_sweep_inflight) return;
        s_sweep_inflight = true;
        limit = s_next_id;
    }
    sweep_recordings(limit);
}
// Whether a recording can still be called (resident or archived, not freed).
bool gx2_dl_test_recording_alive(uint32_t id) {
    std::lock_guard lock(s_mutex);
    const Recording* r = s_recordings.find(id);
    return r && !(r->flags & kDead) && (r->commands || r->archived());
}
#endif

// Query begins per recording: the addresses its query-begin commands use and
// the lists it calls. Posting a call to such a list counts those begins as
// issued, so GX2QueryGetOcclusionResult knows a begin is still queued.
// Ids are never reused, so entries of freed recordings are just unused.
struct ListQueries {
    std::vector<uint32_t> begins;
    std::vector<std::pair<uint32_t, uint32_t>> calls; // list, size
};
std::mutex s_list_queries_mutex;
std::unordered_map<uint32_t, std::shared_ptr<const ListQueries>> s_list_queries;
std::atomic<bool> s_any_list_queries{false};
static bool is_list_call(Gx2CommandFn fn) {
    return fn == s_list_call_fns[0] || fn == s_list_call_fns[1];
}
static std::shared_ptr<const ListQueries> scan_list_queries(const std::vector<Command>& commands) {
    const Gx2CommandFn begin = gx2_query_begin_command();
    ListQueries found;
    for (const Command& c : commands) {
        if (!c.fn) continue;
        if (c.fn == begin)
            found.begins.push_back(c.r[1]);
        else if (is_list_call(c.fn))
            found.calls.emplace_back(c.r[0], c.r[1]);
    }
    if (found.begins.empty() && found.calls.empty()) return nullptr;
    return std::make_shared<const ListQueries>(std::move(found));
}
static void count_list_queries(uint8_t* mem, uint32_t list, uint32_t size, unsigned depth) {
    if (depth > 8 || !list) return;
    uint32_t id = UINT32_MAX;
    if (size >= 12 && rbrew_memory_accessible(mem, list, 12) && rbrew_read32(mem, list) == kTagNop &&
        rbrew_read32(mem, list + 4) == kTagMagic)
        id = rbrew_read32(mem, list + 8);
    else {
        RegistryLock lock;
        if (auto it = s_lists.find(list); it != s_lists.end()) id = it->second;
    }
    std::shared_ptr<const ListQueries> queries;
    {
        std::lock_guard lock(s_list_queries_mutex);
        if (auto it = s_list_queries.find(id); it != s_list_queries.end()) queries = it->second;
    }
    if (!queries) return;
    for (uint32_t address : queries->begins) gx2_query_list_issued(address);
    for (const auto& [inner, inner_size] : queries->calls) count_list_queries(mem, inner, inner_size, depth + 1);
}

bool gx2_dl_recording() {
    // Posted commands run on the render thread: never record them again.
    if (t_stream_replay) return false;
    if (open_active().load(std::memory_order_acquire)) return true;
    return gx2_native_async_active();
}
bool gx2_dl_list_open() {
    if (t_stream_replay) return false;
    return open_active().load(std::memory_order_acquire);
}
void gx2_dl_call(CPUState* cpu, uint32_t list, uint32_t size) {
    const uint32_t saved_list = cpu->r[3], saved_size = cpu->r[4];
    cpu->r[3] = list;
    cpu->r[4] = size;
    call_display_list(cpu, "GX2SetContextState");
    cpu->r[3] = saved_list;
    cpu->r[4] = saved_size;
}
uint64_t gx2_dl_archive_seek_count() {
    std::lock_guard lock(s_archive_mutex);
    return s_archive_seeks;
}

void gx2_dl_record(Gx2CommandFn fn, const CPUState* cpu) {
    // A list call posted to the render stream (not into another list).
    if (is_list_call(fn) && s_any_list_queries.load(std::memory_order_relaxed) && gx2_query_list_counting() &&
        !gx2_dl_list_open())
        count_list_queries(cpu->mem, cpu->r[3], cpu->r[4], 0);
    Command c{fn, {}, {}, {}};
    for (unsigned i = 0; i < 8; ++i) {
        c.r[i] = cpu->r[3 + i];
        c.f[i] = cpu->f[1 + i];
    }
    push_command(std::move(c), cpu->mem, cpu->lr);
}

void gx2_dl_record_uniform_words(bool is_pixel, uint32_t index, std::vector<uint32_t> words) {
    Command c{nullptr, {}, {}, std::move(words)};
    c.r[0] = is_pixel;
    c.r[1] = index;
    push_command(std::move(c), nullptr, 0);
}

void gx2_dl_record_descriptor(Gx2DescriptorFn fn, const CPUState* cpu, uint32_t bytes) {
    const uint32_t address = cpu->r[3];
    if (!address || !rbrew_memory_accessible(cpu->mem, address, bytes)) return;
    Command c{};
    c.descriptor_fn = fn;
    for (unsigned i = 0; i < 8; ++i) {
        c.r[i] = cpu->r[3 + i];
        c.f[i] = cpu->f[1 + i];
    }
    c.descriptor.assign(cpu->mem + address, cpu->mem + address + bytes);
    push_command(std::move(c), cpu->mem, cpu->lr);
}

void gx2_dl_record_snapshot(Gx2DescriptorFn fn, const CPUState* cpu, std::span<const uint8_t> bytes) {
    Command c{nullptr, {}, {}, {}};
    c.descriptor_fn = fn;
    c.descriptor.assign(bytes.begin(), bytes.end());
    for (unsigned i = 0; i < 8; ++i) {
        c.r[i] = cpu->r[3 + i];
        c.f[i] = cpu->f[1 + i];
    }
    push_command(std::move(c), cpu->mem, cpu->lr);
}

// GX2BeginDisplayListEx(void* buffer, u32 bytes, BOOL profiling) - gx2.rpl 02003d6c
static void GX2BeginDisplayListEx(CPUState* cpu) {
    const auto core = builder_slot();
    std::lock_guard lock(s_open_mutex[core]);
    OpenList& list = s_open[core];
    // The builder's command vector is reused (cleared, capacity kept).
    list.active = true;
    list.buffer = ARG0;
    list.capacity = ARG1 & ~3u;
    list.commands.clear();
    s_open_active[core].store(true, std::memory_order_release);
}

// u32 GX2EndDisplayList(void* buffer) - gx2.rpl 02003e1c: bytes written.
static void GX2EndDisplayList(CPUState* cpu) {
    const auto core = builder_slot();
    std::lock_guard builder_lock(s_open_mutex[core]);
    OpenList& list = s_open[core];
    if (!list.active) {
        RET = 0;
        return;
    }
    RET = recorded_size(list);
    // An identical resident recording is reused (see s_content). Hash and
    // compare outside the registry lock: the candidate's commands are held
    // by reference and immutable, and republishing a recording evicted in
    // the meantime is still correct (it is restored from the archive).
    // Large lists are rebuilt every frame (the plaza: 94% of ~13,300 commands
    // per frame are new) and were never reused; hashing them for reuse cost
    // ~2% of the game thread. RBREW_DL_HASH_ALL=1 hashes every list again.
    static const bool hash_all = [] {
        const char* v = std::getenv("RBREW_DL_HASH_ALL");
        return v && v[0] == '1';
    }();
    const bool hashable = hash_all || list.commands.size() <= 512;
    const uint64_t hash = hashable ? commands_hash(list.commands) : 0;
    Commands candidate;
    uint32_t candidate_id = 0;
    if (hashable) {
        RegistryLock lock;
        const auto& slot = s_content[hash & (kContentSlots - 1)];
        if (slot.used && slot.hash == hash)
            if (const Recording* r = s_recordings.find(slot.id); r && r->commands) {
                candidate = r->commands;
                candidate_id = slot.id;
            }
    }
    const bool reuse = candidate && same_commands(*candidate, list.commands);
#ifdef RBREW_GX2_QUEUE_TESTS
    if (reuse && s_test_end_list_gate) s_test_end_list_gate();
#endif
    // Otherwise published at its exact size: a vector grown by appends
    // carries up to half its capacity unused for as long as the recording
    // stays resident (up to kMaxRecordings of them). One allocation replaces
    // the growth reallocations; the builder keeps its capacity.
    std::shared_ptr<std::vector<Command>> commands;
    std::shared_ptr<const BakedList> prebaked;
    auto queries = reuse ? nullptr : scan_list_queries(list.commands);
    if (!reuse) {
        commands = std::make_shared<std::vector<Command>>();
        commands->reserve(list.commands.size());
        commands->insert(commands->end(), std::make_move_iterator(list.commands.begin()),
                         std::make_move_iterator(list.commands.end()));
        prebaked = bake_commands(*commands);
    }
    const uint32_t buffer = list.buffer, capacity = list.capacity;
    list.active = false;
    list.buffer = list.capacity = 0;
    list.commands.clear();
    if (list.commands.capacity() > 4096) std::vector<Command>().swap(list.commands); // an unusually long list
    const bool tagged = capacity >= 12 && rbrew_memory_accessible(MEM, buffer, 12);
    RegistryLock lock;
    s_guest_mem = MEM;
    uint32_t id = 0;
    // A sweep between the candidate lookup and here may have freed it (and
    // even its table chunk). Its commands are immutable and still held: they
    // are then published under a new id instead of republishing a dead one.
    Recording* reused = reuse ? s_recordings.find(candidate_id) : nullptr;
    if (reused && !(reused->flags & kDead)) {
        id = candidate_id;
        reused->last_use = ++s_use_clock;
        reused->flags |= kUsed | (tagged ? kTagged : 0);
        s_content_reuses.fetch_add(1, std::memory_order_relaxed);
    } else {
        id = s_next_id++;
        Recording recording{commands ? Commands(std::move(commands)) : std::move(candidate), ++s_use_clock};
        if (recording.commands) recording.baked = prebaked ? std::move(prebaked) : bake_commands(*recording.commands);
        recording.buffer = buffer;
        recording.bytes = recording_bytes(*recording.commands);
        recording.flags = tagged ? kTagged : 0;
        s_resident_bytes += recording.bytes;
        s_recordings.insert(id, std::move(recording));
        s_tracked.push_back(id);
        ++s_resident_recordings;
        if (hashable) s_content[hash & (kContentSlots - 1)] = {hash, id, true};
        if (queries) {
            std::lock_guard queries_lock(s_list_queries_mutex);
            s_list_queries[id] = std::move(queries);
            s_any_list_queries.store(true, std::memory_order_relaxed);
        }
    }
    s_lists[buffer] = id;
    // RBREW_DL_STATS=1 (diagnostics): new versus reused recordings and their
    // commands, every 300 frames.
    static const bool stats = [] {
        const char* v = std::getenv("RBREW_DL_STATS");
        return v && v[0] == '1';
    }();
    if (stats) {
        static uint64_t ended = 0, reused_lists = 0, new_commands = 0, reused_commands = 0, largest = 0;
        static uint32_t last_frame = 0;
        const Recording* r = s_recordings.find(id);
        const uint64_t n = r && r->commands ? r->commands->size() : 0;
        ++ended;
        largest = std::max(largest, n);
        if (reused && id == candidate_id) {
            ++reused_lists;
            reused_commands += n;
        } else
            new_commands += n;
        const uint32_t frame = rbrew_benchmark::frame.load(std::memory_order_relaxed);
        if (frame / 300 != last_frame / 300) {
            RBREW_LOG_INFO("dlstats", "frame %u lists %llu reused %llu new_cmds %llu reused_cmds %llu largest %llu\n",
                           frame, (unsigned long long)ended, (unsigned long long)reused_lists,
                           (unsigned long long)new_commands, (unsigned long long)reused_commands,
                           (unsigned long long)largest);
            ended = reused_lists = new_commands = reused_commands = largest = 0;
        }
        last_frame = frame;
    }
    maybe_sweep();
    evict_old_recordings();
    if (tagged) {
        rbrew_write32(MEM, buffer, kTagNop);
        rbrew_write32(MEM, buffer + 4, kTagMagic);
        rbrew_write32(MEM, buffer + 8, id);
    }
    s_open_active[core].store(false, std::memory_order_release);
}

namespace {
// Context shadow lists are re-snapshotted on every GX2SetContextState switch
// (~0x9800 bytes each). Recordings are never erased, so a fresh recording per
// switch grew the registry and its archive without bound although a context
// mostly cycles through the same few states. A snapshot whose function and
// bytes equal a resident earlier snapshot of the same buffer reuses that
// recording: replaying it runs the same function on the same bytes. Snapshot
// functions read only those bytes, never the recorded argument registers.
struct SnapshotSlot {
    uint64_t hash = 0;
    uint32_t id = 0;
    Gx2DescriptorFn fn = nullptr;
};
constexpr size_t kSnapshotSlots = 16;
struct SnapshotSlots {
    std::array<SnapshotSlot, kSnapshotSlots> slots{};
    size_t next = 0;
};
std::unordered_map<uint32_t, SnapshotSlots> s_snapshots; // by list buffer, guarded by s_mutex
uint64_t s_snapshot_reuses = 0;

uint64_t snapshot_hash(std::span<const uint8_t> bytes) {
    uint64_t h[4] = {0x9E3779B97F4A7C15ull, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull};
    size_t i = 0;
    for (; i + 32 <= bytes.size(); i += 32)
        for (unsigned lane = 0; lane < 4; ++lane) {
            uint64_t w;
            std::memcpy(&w, bytes.data() + i + lane * 8, 8);
            h[lane] = (h[lane] ^ w) * 0x100000001B3ull;
            h[lane] ^= h[lane] >> 31;
        }
    uint64_t r = bytes.size();
    for (unsigned lane = 0; lane < 4; ++lane) r = (r ^ h[lane]) * 0x9E3779B97F4A7C15ull;
    for (; i < bytes.size(); ++i) r = (r ^ bytes[i]) * 0x100000001B3ull;
    return r ^ (r >> 29);
}

// Caller holds s_mutex. A hit republishes the recording for `buffer` exactly
// as GX2EndDisplayList would have published a new one.
bool reuse_snapshot(uint8_t* mem, uint32_t buffer, uint32_t capacity, Gx2DescriptorFn fn,
                    std::span<const uint8_t> snapshot, uint64_t hash, uint32_t& size) {
    auto slots = s_snapshots.find(buffer);
    if (slots == s_snapshots.end()) return false;
    for (const auto& slot : slots->second.slots) {
        if (slot.fn != fn || slot.hash != hash) continue;
        Recording* recording = s_recordings.find(slot.id);
        if (!recording || !recording->commands) continue; // archived: record afresh
        const auto& commands = *recording->commands;
        if (commands.size() != 1 || commands[0].descriptor_fn != fn || commands[0].fn ||
            commands[0].descriptor.size() != snapshot.size() ||
            std::memcmp(commands[0].descriptor.data(), snapshot.data(), snapshot.size()) != 0)
            continue;
        recording->last_use = ++s_use_clock;
        recording->flags |= kUsed;
        s_lists[buffer] = slot.id;
        if (capacity >= 12 && rbrew_memory_accessible(mem, buffer, 12)) {
            rbrew_write32(mem, buffer, kTagNop);
            rbrew_write32(mem, buffer + 4, kTagMagic);
            rbrew_write32(mem, buffer + 8, slot.id);
        }
        size = std::min<uint32_t>(32u, capacity & ~3u); // recorded_size() of one command
        ++s_snapshot_reuses;
        return true;
    }
    return false;
}
void remember_snapshot(uint32_t buffer, Gx2DescriptorFn fn, uint64_t hash, uint32_t id) {
    auto& slots = s_snapshots[buffer];
    slots.slots[slots.next] = {hash, id, fn};
    slots.next = (slots.next + 1) % kSnapshotSlots;
}
} // namespace

// [mem] diagnostics: lists recorded so far and those with resident commands.
std::pair<uint64_t, uint64_t> gx2_dl_recording_counts() {
    std::lock_guard lock(s_mutex);
    return {s_next_id, s_resident_recordings};
}
// [perf-mem] (cumulative): evictions started, cold lists restored from the
// archive, time spent selecting eviction candidates, lists that reused an
// identical recording, sweeps finished, lists they freed, sweep time,
// inconclusive sweeps; then the current resident bytes.
std::array<uint64_t, 9> gx2_dl_maintenance_counts() {
    uint64_t resident_bytes;
    {
        std::lock_guard lock(s_mutex);
        resident_bytes = s_resident_bytes;
    }
    const auto get = [](const std::atomic<uint64_t>& v) { return v.load(std::memory_order_relaxed); };
    return {get(s_eviction_count), get(s_restore_count),      get(s_eviction_ns),
            get(s_content_reuses), get(s_sweep_count),        get(s_swept_lists),
            get(s_sweep_ns),       get(s_sweep_inconclusive), resident_bytes};
}

uint64_t gx2_dl_snapshot_reuse_count() {
    std::lock_guard lock(s_mutex);
    return s_snapshot_reuses;
}

uint32_t gx2_dl_create_snapshot(CPUState* cpu, uint32_t buffer, uint32_t capacity, Gx2DescriptorFn fn,
                                std::span<const uint8_t> snapshot) {
    if (!cpu || !fn || !buffer || capacity < 12 || snapshot.empty() || gx2_dl_list_open()) return 0;

    const uint64_t hash = snapshot_hash(snapshot);
    {
        RegistryLock lock;
        uint32_t size = 0;
        if (reuse_snapshot(cpu->mem, buffer, capacity, fn, snapshot, hash, size)) return size;
    }
    const uint32_t saved_r3 = cpu->r[3], saved_r4 = cpu->r[4];
    cpu->r[3] = buffer;
    cpu->r[4] = capacity;
    const bool private_builder = std::exchange(t_private_builder, true);
    GX2BeginDisplayListEx(cpu);
    gx2_dl_record_snapshot(fn, cpu, snapshot);
    cpu->r[3] = buffer;
    GX2EndDisplayList(cpu);
    t_private_builder = private_builder;
    const uint32_t size = cpu->r[3];
    cpu->r[3] = saved_r3;
    cpu->r[4] = saved_r4;
    if (size) {
        RegistryLock lock;
        if (const auto it = s_lists.find(buffer); it != s_lists.end()) remember_snapshot(buffer, fn, hash, it->second);
    }
    return size;
}

// BOOL GX2GetCurrentDisplayList(void** buffer, u32* bytes) - gx2.rpl 02004160
static void GX2GetCurrentDisplayList(CPUState* cpu) {
    const auto core = builder_slot();
    bool active;
    uint32_t buffer, capacity; // not the recorded commands
    {
        std::lock_guard lock(s_open_mutex[core]);
        const OpenList& list = s_open[core];
        active = list.active;
        buffer = list.buffer;
        capacity = list.capacity;
    }
    if (!active) {
        RET = 0;
        return;
    }
    if (ARG0) rbrew_write32(MEM, ARG0, buffer);
    if (ARG1) rbrew_write32(MEM, ARG1, capacity);
    RET = 1;
}

// GX2PatchDisplayList(list, type, byteOffset, shader) - gx2.rpl 02009eb4:
// stores the shader program address >> 8 in the register packet at
// byteOffset (+8 skips header and register index) of a pre-built list.
// The console stores the physical address; the native renderer resolves
// guest effective addresses, so the effective address is stored.
static void GX2PatchDisplayList(CPUState* cpu) {
    const uint32_t list = ARG0, type = ARG1, offset = ARG2 & ~3u, shader = ARG3;
    // Per type: field tested for zero, program field, fallback (GX2R buffer).
    struct Field {
        uint32_t test, program, fallback;
    };
    static constexpr Field fields[7] = {{},
                                        {0x0C, 0x0C, 0x0C},
                                        {0xD4, 0xD4, 0x130},
                                        {0x50, 0x58, 0xBC},
                                        {0x50, 0x50, 0xAC},
                                        {0xA8, 0xA8, 0xE4},
                                        {0x34, 0x34, 0x80}};
    if (type == 0 || type > 6 || !shader || !rbrew_memory_accessible(MEM, shader, fields[type].fallback + 4) ||
        !rbrew_memory_accessible(MEM, list + offset + 8, 4))
        return;
    const Field& f = fields[type];
    const uint32_t program = rbrew_read32(MEM, shader + (rbrew_read32(MEM, shader + f.test) ? f.program : f.fallback));
    rbrew_write32(MEM, list + offset + 8, program >> 8);
}

static void GX2CallDisplayList(CPUState* cpu) {
    rbrew_benchmark::guest_draws.fetch_add(1, std::memory_order_relaxed);
    call_display_list(cpu, "GX2CallDisplayList");
}
static void GX2DirectCallDisplayList(CPUState* cpu) {
    rbrew_benchmark::guest_draws.fetch_add(1, std::memory_order_relaxed);
    call_display_list(cpu, "GX2DirectCallDisplayList");
}

void gx2_display_list_register(CPUState* cpu) {
#if defined(HAVE_SDL2)
#define REG(name)                                                                                                      \
    do {                                                                                                               \
        rbrew_register_func(cpu, ADDR_##name, GX2_NATIVE_CALL(name));                                                  \
        gx2_perf_register_name(reinterpret_cast<const void*>(name), #name);                                            \
    } while (0)
#else
#define REG(name)                                                                                                      \
    do {                                                                                                               \
        rbrew_register_func(cpu, ADDR_##name, gx2_command_call<name, gx2_is_command(#name)>);                          \
        gx2_perf_register_name(reinterpret_cast<const void*>(name), #name);                                            \
    } while (0)
#endif
    s_list_call_fns[0] = GX2CallDisplayList;
    s_list_call_fns[1] = GX2DirectCallDisplayList;
    REG(GX2BeginDisplayListEx);
    REG(GX2EndDisplayList);
    REG(GX2GetCurrentDisplayList);
    REG(GX2CallDisplayList);
    REG(GX2DirectCallDisplayList);
    REG(GX2PatchDisplayList);
#undef REG
}

void gx2_register_draw_commands(Gx2CommandFn draw_indexed, Gx2CommandFn draw_indexed2, Gx2CommandFn draw_ex) {
    s_draw_indexed_ex_fn = draw_indexed;
    s_draw_indexed_ex2_fn = draw_indexed2;
    s_draw_ex_fn = draw_ex;
}

void gx2_register_hot_commands(const Gx2BakedHotCommands& hot) {
    s_hot_cmds = hot;
}
