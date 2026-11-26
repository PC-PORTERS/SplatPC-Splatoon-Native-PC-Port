#include "gx2_write_watch.h"
#include <atomic>

namespace {
std::atomic<uint64_t> s_watch_batch_id{0};
thread_local uint64_t t_watch_batch_id = 0;
} // namespace

uint64_t gx2_write_watch_batch_id() {
    return t_watch_batch_id;
}
void gx2_write_watch_end_batch() {
    t_watch_batch_id = 0;
}

#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#define RBREW_HAVE_WRITE_WATCH 1
#elif defined(__linux__) && defined(PAGEMAP_SCAN) && defined(UFFD_API)
#define RBREW_HAVE_WRITE_WATCH 1
#else
#define RBREW_HAVE_WRITE_WATCH 0
#endif

#if RBREW_HAVE_WRITE_WATCH
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "../../runtime/rebrewu_memory_observer.h"
#include "../../runtime/rbrew_diagnostics.h"
#include "gx2_frame_interp.h"

namespace {
constexpr uint64_t kPage = 4096;
constexpr size_t kFastMruSlots = 256;
std::mutex s_lock;
const uint8_t* s_mem = nullptr;
std::vector<uint32_t> s_page_generation; // guest page -> generation of its newest collected write
std::vector<uint64_t> s_page_collected;  // bit per guest page: s_page_generation != 0
uint32_t s_generation = 0;
uint64_t s_queries = 0, s_checks = 0, s_reused = 0;
std::vector<Gx2WriteRange*> s_ranges;
struct RangeGeneration {
    uint64_t revision = 0, generation = 0;
};
std::unordered_map<uint64_t, RangeGeneration> s_range_generations;
std::atomic<uint64_t> s_range_revision{1};
uint64_t s_range_hits = 0;

std::vector<uint32_t> s_page_epoch; // guest page -> epoch of its last collection
std::atomic<uint32_t> s_epoch{1};
thread_local bool t_epoch_reuse = false;
thread_local const uint8_t* t_observed_mem = nullptr;

struct FastRangeMru {
    const uint8_t* mem = nullptr;
    uint32_t addr = 0;
    uint32_t size = 0;
    uint64_t revision = ~0ull;
    uint32_t epoch = ~0u;
    uint64_t generation = 0;
};
thread_local FastRangeMru t_fast_mru[kFastMruSlots]{};

size_t fast_mru_slot(uint32_t addr, uint32_t size) noexcept {
    return size_t(((addr >> 4) ^ (size >> 2) ^ size) & (kFastMruSlots - 1));
}

bool tracking_disabled() {
    static const bool disabled = [] {
        const char* v = std::getenv("RBREW_WRITE_WATCH_DISABLE");
        return v && v[0] == '1';
    }();
    return disabled;
}
bool reuse_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("RBREW_WRITE_WATCH_REUSE");
        return !(value && value[0] == '0');
    }();
    return enabled;
}

#if defined(_WIN32)
std::vector<void*> s_written;

bool os_arena_tracked(const uint8_t*) noexcept {
    return true;
}

bool os_collect_pages(const uint8_t* mem, uint64_t first, uint64_t last, std::vector<uint64_t>& dirty_pages) {
    s_written.resize(size_t(last - first + 1));
    ULONG_PTR count = s_written.size();
    DWORD granularity = 0;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, const_cast<uint8_t*>(mem) + first * kPage,
                      SIZE_T((last - first + 1) * kPage), s_written.data(), &count, &granularity) != 0)
        return false;
    dirty_pages.clear();
    dirty_pages.reserve(count);
    for (ULONG_PTR i = 0; i < count; ++i)
        dirty_pages.push_back(uint64_t(static_cast<const uint8_t*>(s_written[i]) - mem) / kPage);
    return true;
}

bool os_watch_pending(const uint8_t* mem, uint32_t addr, uint32_t size) {
    const uint64_t first = addr / kPage, last = (uint64_t(addr) + size - 1) / kPage;
    std::vector<void*> pages(size_t(last - first + 1));
    ULONG_PTR count = pages.size();
    DWORD granularity = 0;
    if (GetWriteWatch(0, const_cast<uint8_t*>(mem) + first * kPage, SIZE_T((last - first + 1) * kPage), pages.data(),
                      &count, &granularity) != 0)
        return false;
    return count != 0;
}

bool os_written_pages_slice(const uint8_t* mem, uint64_t first, uint64_t slice, uint64_t page_count,
                            std::vector<uint64_t>& pages) {
    auto written = std::make_unique_for_overwrite<void*[]>(static_cast<size_t>(slice));
    ULONG_PTR count = static_cast<ULONG_PTR>(slice);
    DWORD granularity = 0;
    if (GetWriteWatch(0, const_cast<uint8_t*>(mem) + first * kPage, SIZE_T(slice * kPage), written.get(), &count,
                      &granularity) != 0)
        return false;
    for (ULONG_PTR i = 0; i < count; ++i) {
        const uint64_t page = uint64_t(static_cast<const uint8_t*>(written[i]) - mem) / kPage;
        if (page < page_count) pages[page / 64] |= 1ull << (page % 64);
    }
    return true;
}
#else
const uint8_t* s_linux_mem = nullptr;
uint64_t s_linux_size = 0;
int s_linux_uffd = -1;
int s_linux_pagemap = -1;
std::atomic<const uint8_t*> s_linux_mem_fast{nullptr};
std::atomic<uint64_t> s_linux_size_fast{0};
std::atomic<int> s_linux_pagemap_fast{-1};
std::vector<struct page_region> s_scan_regions;

int ioctl_retry(int fd, unsigned long req, void* arg) {
    int rc;
    do {
        rc = ::ioctl(fd, req, arg);
    } while (rc < 0 && errno == EINTR);
    return rc;
}

bool os_arena_tracked(const uint8_t* mem) noexcept {
    return mem && mem == s_linux_mem_fast.load(std::memory_order_acquire);
}

bool scan_linux_regions(int pagemap, uint64_t start, uint64_t end, bool reset_wp, std::vector<struct page_region>& out,
                        std::vector<uint64_t>* dirty_pages, std::vector<uint64_t>* bitset, const uint8_t* base,
                        uint64_t page_count, bool& any_found) {
    any_found = false;
    const uint64_t flags = (reset_wp ? PM_SCAN_WP_MATCHING : 0u) | PM_SCAN_CHECK_WPASYNC;
    while (start < end) {
        struct pm_scan_arg arg{};
        arg.size = sizeof(arg);
        arg.flags = flags;
        arg.start = start;
        arg.end = end;
        arg.vec = reinterpret_cast<uint64_t>(out.data());
        arg.vec_len = out.size();
        arg.max_pages = dirty_pages || bitset ? 0 : 1;
        arg.category_mask = PAGE_IS_WRITTEN;
        arg.return_mask = PAGE_IS_WRITTEN;
        const int rc = ioctl_retry(pagemap, PAGEMAP_SCAN, &arg);
        if (rc < 0) return false;
        if (rc > 0) {
            any_found = true;
            if (!dirty_pages && !bitset) return true;
            for (int i = 0; i < rc; ++i) {
                const uint64_t r0 = (out[size_t(i)].start - reinterpret_cast<uint64_t>(base)) / kPage;
                const uint64_t r1 = (out[size_t(i)].end - reinterpret_cast<uint64_t>(base)) / kPage;
                for (uint64_t p = r0; p < r1 && p < page_count; ++p) {
                    if (dirty_pages) dirty_pages->push_back(p);
                    if (bitset) (*bitset)[p / 64] |= 1ull << (p % 64);
                }
            }
        }
        if (arg.walk_end <= start) break;
        start = arg.walk_end;
    }
    return true;
}

bool os_collect_pages(const uint8_t* mem, uint64_t first, uint64_t last, std::vector<uint64_t>& dirty_pages) {
    if (!mem || mem != s_linux_mem || s_linux_pagemap < 0 || (last + 1) * kPage > s_linux_size) return false;
    dirty_pages.clear();
    const size_t wanted = std::min<size_t>(size_t(last - first + 1), 4096);
    if (s_scan_regions.size() < wanted) s_scan_regions.resize(wanted);
    bool any_found = false;
    const uint64_t base = reinterpret_cast<uint64_t>(mem);
    return scan_linux_regions(s_linux_pagemap, base + first * kPage, base + (last + 1) * kPage, true, s_scan_regions,
                              &dirty_pages, nullptr, mem, s_linux_size / kPage, any_found);
}

bool os_watch_pending(const uint8_t* mem, uint32_t addr, uint32_t size) {
    const int pagemap = s_linux_pagemap_fast.load(std::memory_order_acquire);
    const uint64_t reg_size = s_linux_size_fast.load(std::memory_order_acquire);
    if (!mem || mem != s_linux_mem_fast.load(std::memory_order_acquire) || pagemap < 0 || !size) return false;
    const uint64_t first = addr / kPage, last = (uint64_t(addr) + size - 1) / kPage;
    if ((last + 1) * kPage > reg_size) return false;
    std::vector<struct page_region> one(1);
    bool any_found = false;
    const uint64_t base = reinterpret_cast<uint64_t>(mem);
    if (!scan_linux_regions(pagemap, base + first * kPage, base + (last + 1) * kPage, false, one, nullptr, nullptr, mem,
                            reg_size / kPage, any_found))
        return false;
    return any_found;
}

bool os_written_pages_slice(const uint8_t* mem, uint64_t first, uint64_t slice, uint64_t page_count,
                            std::vector<uint64_t>& pages) {
    const int pagemap = s_linux_pagemap_fast.load(std::memory_order_acquire);
    const uint64_t reg_size = s_linux_size_fast.load(std::memory_order_acquire);
    if (!mem || mem != s_linux_mem_fast.load(std::memory_order_acquire) || pagemap < 0) return false;
    if ((first + slice) * kPage > reg_size) return false;
    std::vector<struct page_region> vec(4096);
    bool any_found = false;
    const uint64_t base = reinterpret_cast<uint64_t>(mem);
    return scan_linux_regions(pagemap, base + first * kPage, base + (first + slice) * kPage, false, vec, nullptr,
                              &pages, mem, page_count, any_found);
}
#endif

void select_arena(const uint8_t* mem) {
    if (mem != s_mem) { // a new arena starts with an empty history
        s_mem = mem;
        s_page_generation.assign(size_t(1) << 20, 0); // 4 GiB guest space / 4 KiB pages
        s_page_collected.assign((size_t(1) << 20) / 64, 0);
        s_page_epoch.assign(size_t(1) << 20, 0);
        s_generation = 0;
        s_range_generations.clear();
    }
}
void forget_collected(uint32_t address, uint32_t size, void*) noexcept {
    if (!size) return;
    std::lock_guard lock(s_lock);
    // The observer cannot allocate. A revision invalidates all memoized
    // ranges conservatively, including overlaps and partial-page writes.
    ++s_range_revision;
    if (s_page_epoch.empty()) return;
    const uint64_t last = std::min<uint64_t>((uint64_t(address) + size - 1) / kPage, s_page_epoch.size() - 1);
    for (uint64_t page = address / kPage; page <= last; ++page) s_page_epoch[page] = 0;
}
// Caller holds s_lock with the arena selected.
bool collected_this_epoch(const uint8_t* mem, uint64_t first, uint64_t last) {
    if (!t_epoch_reuse || mem != t_observed_mem) return false;
    for (uint64_t page = first; page <= last; ++page)
        if (s_page_epoch[page] != s_epoch) return false;
    ++s_reused;
    return true;
}
void observe_render_writes(const uint8_t* mem) {
    if (!t_epoch_reuse || t_observed_mem == mem) return;
    // Installed outside s_lock: the observer callback takes it.
    rbrew_set_write_observer({const_cast<uint8_t*>(mem), forget_collected, nullptr});
    t_observed_mem = mem;
}
bool collect_pages(const uint8_t* mem, uint64_t first, uint64_t last) {
    ++s_queries;
    std::vector<uint64_t> dirty_pages;
    if (!os_collect_pages(mem, first, last, dirty_pages)) return false;
    if (!dirty_pages.empty()) {
        ++s_generation;
        ++s_range_revision;
        for (const uint64_t page : dirty_pages) {
            if (page >= s_page_generation.size()) continue;
            s_page_generation[page] = s_generation;
            s_page_collected[page / 64] |= 1ull << (page % 64);
        }
    }
    for (uint64_t page = first; page <= last; ++page) s_page_epoch[page] = s_epoch;
    return true;
}
uint64_t window_pages() {
    static const uint64_t pages = [] {
        const char* v = std::getenv("RBREW_WRITE_WATCH_WINDOW");
        const uint64_t n = v ? std::strtoull(v, nullptr, 10) : 16;
        return n && !(n & (n - 1)) ? n : 16;
    }();
    return pages;
}
bool collect_batch_pages(const uint8_t* mem, uint64_t first, uint64_t last) {
    const uint64_t kWindowPages = window_pages();
    if (t_epoch_reuse && mem == t_observed_mem) {
        const uint64_t window_first = first & ~(kWindowPages - 1);
        const uint64_t window_last = std::min<uint64_t>(last | (kWindowPages - 1), s_page_epoch.size() - 1);
        if ((window_first != first || window_last != last) && collect_pages(mem, window_first, window_last))
            return true;
    }
    return collect_pages(mem, first, last);
}
uint64_t newest(uint64_t first, uint64_t last) {
    uint32_t generation = 0;
    for (uint64_t page = first; page <= last; ++page) generation = std::max(generation, s_page_generation[page]);
    return generation;
}
bool memoizable(const uint8_t* mem, uint32_t addr, uint32_t size) {
    return t_epoch_reuse && mem == t_observed_mem && (uint64_t(addr) + size - 1) / kPage - addr / kPage >= 15;
}
bool memoized(const uint8_t* mem, uint32_t addr, uint32_t size, uint64_t& generation) {
    if (!memoizable(mem, addr, size)) return false;
    const auto it = s_range_generations.find(uint64_t(addr) << 32 | size);
    if (it == s_range_generations.end() || it->second.revision != s_range_revision) return false;
    generation = it->second.generation;
    ++s_range_hits;
    ++s_reused;
    return true;
}
void remember_range(const uint8_t* mem, uint32_t addr, uint32_t size, uint64_t generation) {
    if (generation != kGx2WriteUntracked && memoizable(mem, addr, size))
        s_range_generations[uint64_t(addr) << 32 | size] = {s_range_revision, generation};
}
} // namespace

bool gx2_write_watch_register_arena(const uint8_t* mem, uint64_t size) {
#if defined(_WIN32)
    (void)mem;
    (void)size;
    return true;
#else
    if (tracking_disabled() || !mem || !size || (reinterpret_cast<uintptr_t>(mem) & (kPage - 1)) ||
        (size & (kPage - 1)))
        return false;
    std::lock_guard lock(s_lock);
    if (s_linux_mem == mem && s_linux_size == size && s_linux_uffd >= 0 && s_linux_pagemap >= 0) return true;
    if (s_linux_uffd >= 0 && s_linux_mem) {
        struct uffdio_range unreg{reinterpret_cast<uint64_t>(s_linux_mem), s_linux_size};
        ioctl_retry(s_linux_uffd, UFFDIO_UNREGISTER, &unreg);
    }
    if (s_linux_uffd >= 0) ::close(s_linux_uffd);
    if (s_linux_pagemap >= 0) ::close(s_linux_pagemap);
    s_linux_mem = nullptr;
    s_linux_size = 0;
    s_linux_uffd = -1;
    s_linux_pagemap = -1;
    s_linux_mem_fast.store(nullptr, std::memory_order_release);
    s_linux_size_fast.store(0, std::memory_order_release);
    s_linux_pagemap_fast.store(-1, std::memory_order_release);

    const int pagemap = ::open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (pagemap < 0) return false;
#ifdef UFFD_USER_MODE_ONLY
    int uffd = int(::syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY));
    if (uffd < 0 && errno == EINVAL) uffd = int(::syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK));
#else
    int uffd = int(::syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK));
#endif
    if (uffd < 0) {
        const int err = errno;
        ::close(pagemap);
        RBREW_LOG_INFO("mem", "Linux write watch disabled: userfaultfd errno=%d\n", err);
        return false;
    }
    struct uffdio_api api{};
    api.api = UFFD_API;
    api.features = UFFD_FEATURE_WP_ASYNC;
    if (ioctl_retry(uffd, UFFDIO_API, &api) < 0 || !(api.features & UFFD_FEATURE_WP_ASYNC)) {
        const int err = errno;
        ::close(uffd);
        ::close(pagemap);
        RBREW_LOG_INFO("mem", "Linux write watch disabled: UFFD_FEATURE_WP_ASYNC errno=%d\n", err);
        return false;
    }
    struct uffdio_register reg{};
    reg.range = {reinterpret_cast<uint64_t>(mem), size};
    reg.mode = UFFDIO_REGISTER_MODE_WP;
    if (ioctl_retry(uffd, UFFDIO_REGISTER, &reg) < 0) {
        const int err = errno;
        ::close(uffd);
        ::close(pagemap);
        RBREW_LOG_INFO("mem", "Linux write watch disabled: UFFDIO_REGISTER errno=%d\n", err);
        return false;
    }
    uint64_t scan_start = reinterpret_cast<uint64_t>(mem);
    const uint64_t scan_end = scan_start + size;
    while (scan_start < scan_end) {
        struct pm_scan_arg init_arg{};
        init_arg.size = sizeof(init_arg);
        init_arg.flags = PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC;
        init_arg.start = scan_start;
        init_arg.end = scan_end;
        init_arg.category_mask = PAGE_IS_WRITTEN;
        if (ioctl_retry(pagemap, PAGEMAP_SCAN, &init_arg) < 0) {
            const int err = errno;
            ::close(uffd);
            ::close(pagemap);
            RBREW_LOG_INFO("mem", "Linux write watch disabled: PAGEMAP_SCAN errno=%d\n", err);
            return false;
        }
        if (init_arg.walk_end <= scan_start) break;
        scan_start = init_arg.walk_end;
    }
    s_linux_mem = mem;
    s_linux_size = size;
    s_linux_uffd = uffd;
    s_linux_pagemap = pagemap;
    s_linux_mem_fast.store(mem, std::memory_order_release);
    s_linux_size_fast.store(size, std::memory_order_release);
    s_linux_pagemap_fast.store(pagemap, std::memory_order_release);
    s_mem = nullptr;
    ++s_range_revision;
    RBREW_LOG_INFO("mem", "Linux write watch active: userfaultfd WP_ASYNC + PAGEMAP_SCAN (%llu MB)\n",
                   (unsigned long long)(size >> 20));
    return true;
#endif
}

void gx2_write_watch_unregister_arena(const uint8_t* mem) {
#if defined(_WIN32)
    (void)mem;
#else
    if (!mem) return;
    std::lock_guard lock(s_lock);
    if (mem != s_linux_mem) return;
    if (s_linux_uffd >= 0) {
        struct uffdio_range unreg{reinterpret_cast<uint64_t>(s_linux_mem), s_linux_size};
        ioctl_retry(s_linux_uffd, UFFDIO_UNREGISTER, &unreg);
        ::close(s_linux_uffd);
    }
    if (s_linux_pagemap >= 0) ::close(s_linux_pagemap);
    s_linux_mem = nullptr;
    s_linux_size = 0;
    s_linux_uffd = -1;
    s_linux_pagemap = -1;
    s_linux_mem_fast.store(nullptr, std::memory_order_release);
    s_linux_size_fast.store(0, std::memory_order_release);
    s_linux_pagemap_fast.store(-1, std::memory_order_release);
    s_mem = nullptr;
    ++s_range_revision;
#endif
}

uint64_t gx2_write_generation(const uint8_t* mem, uint32_t addr, uint32_t size) {
    if (!mem || !size) return 0;
    if (uint64_t(addr) + size > (1ull << 32) || tracking_disabled() || !os_arena_tracked(mem))
        return kGx2WriteUntracked;

    const uint64_t cur_rev = s_range_revision.load(std::memory_order_relaxed);
    const uint32_t cur_epoch = s_epoch.load(std::memory_order_relaxed);
    const size_t slot = fast_mru_slot(addr, size);
    if (t_epoch_reuse && t_fast_mru[slot].mem == mem && t_fast_mru[slot].addr == addr &&
        t_fast_mru[slot].size == size && t_fast_mru[slot].revision == cur_rev && t_fast_mru[slot].epoch == cur_epoch) {
        return t_fast_mru[slot].generation;
    }

    const uint64_t first = addr / kPage, last = (uint64_t(addr) + size - 1) / kPage;
    observe_render_writes(mem);
    std::lock_guard lock(s_lock);
    select_arena(mem);
    ++s_checks;
    if (gx2interp::replaying()) return newest(first, last);
    uint64_t generation;
    if (memoized(mem, addr, size, generation)) {
        if (t_epoch_reuse) t_fast_mru[slot] = {mem, addr, size, cur_rev, cur_epoch, generation};
        return generation;
    }
    generation = (collected_this_epoch(mem, first, last) || collect_batch_pages(mem, first, last)) ? newest(first, last)
                                                                                                   : kGx2WriteUntracked;
    remember_range(mem, addr, size, generation);
    if (t_epoch_reuse) t_fast_mru[slot] = {mem, addr, size, cur_rev, cur_epoch, generation};
    return generation;
}

void gx2_write_generations(const uint8_t* mem, Gx2WriteRange* ranges, uint32_t count) {
    if (!ranges || !count) return;
    if (!mem || tracking_disabled() || !os_arena_tracked(mem)) {
        for (uint32_t i = 0; i < count; ++i) ranges[i].generation = !ranges[i].size ? 0 : kGx2WriteUntracked;
        return;
    }
    const uint64_t cur_rev = s_range_revision.load(std::memory_order_relaxed);
    const uint32_t cur_epoch = s_epoch.load(std::memory_order_relaxed);
    if (t_epoch_reuse) {
        uint32_t hits = 0;
        for (uint32_t i = 0; i < count; ++i) {
            auto& range = ranges[i];
            if (!range.size) {
                range.generation = 0;
                ++hits;
                continue;
            }
            const size_t slot = fast_mru_slot(range.address, range.size);
            if (t_fast_mru[slot].mem == mem && t_fast_mru[slot].addr == range.address &&
                t_fast_mru[slot].size == range.size && t_fast_mru[slot].revision == cur_rev &&
                t_fast_mru[slot].epoch == cur_epoch) {
                range.generation = t_fast_mru[slot].generation;
                ++hits;
            }
        }
        if (hits == count) return;
    }
    observe_render_writes(mem);
    std::lock_guard lock(s_lock);
    select_arena(mem);
    if (gx2interp::replaying()) {
        for (uint32_t i = 0; i < count; ++i) {
            auto& range = ranges[i];
            range.generation = !range.size ? 0
                               : uint64_t(range.address) + range.size > (1ull << 32)
                                   ? kGx2WriteUntracked
                                   : newest(range.address / kPage, (uint64_t(range.address) + range.size - 1) / kPage);
        }
        return;
    }
    s_ranges.clear();
    for (uint32_t i = 0; i < count; ++i) {
        auto& range = ranges[i];
        range.generation = 0;
        if (!range.size) continue;
        if (uint64_t(range.address) + range.size > (1ull << 32)) {
            range.generation = kGx2WriteUntracked;
            continue;
        }
        ++s_checks;
        if (!memoized(mem, range.address, range.size, range.generation)) s_ranges.push_back(&range);
    }
    if (s_ranges.empty()) return;
    std::sort(s_ranges.begin(), s_ranges.end(), [](const auto* a, const auto* b) { return a->address < b->address; });
    for (size_t first_range = 0; first_range < s_ranges.size();) {
        const auto* first = s_ranges[first_range];
        const uint64_t first_page = first->address / kPage;
        uint64_t last_page = (uint64_t(first->address) + first->size - 1) / kPage;
        size_t end = first_range + 1;
        while (end < s_ranges.size() && s_ranges[end]->address / kPage <= last_page + 1) {
            last_page = std::max(last_page, (uint64_t(s_ranges[end]->address) + s_ranges[end]->size - 1) / kPage);
            ++end;
        }
        const bool tracked =
            collected_this_epoch(mem, first_page, last_page) || collect_batch_pages(mem, first_page, last_page);
        for (size_t i = first_range; i < end; ++i) {
            auto& range = *s_ranges[i];
            range.generation = tracked
                                   ? newest(range.address / kPage, (uint64_t(range.address) + range.size - 1) / kPage)
                                   : kGx2WriteUntracked;
            remember_range(mem, range.address, range.size, range.generation);
            if (t_epoch_reuse) {
                const size_t slot = fast_mru_slot(range.address, range.size);
                t_fast_mru[slot] = {mem, range.address, range.size, cur_rev, cur_epoch, range.generation};
            }
        }
        first_range = end;
    }
}

void gx2_write_watch_begin_batch() {
    if (!reuse_enabled()) return;
    t_watch_batch_id = s_watch_batch_id.fetch_add(1, std::memory_order_relaxed) + 1;
    t_epoch_reuse = true;
    std::lock_guard lock(s_lock);
    s_range_generations.clear();
    if (++s_epoch == 0) s_epoch = 1; // 0 means "never collected"
}
uint64_t gx2_write_query_count() {
    std::lock_guard lock(s_lock);
    return s_queries;
}
Gx2WriteWatchStats gx2_write_watch_stats() {
    std::lock_guard lock(s_lock);
    return {s_checks, s_queries, s_reused, s_range_hits};
}
bool gx2_write_watch_pending(const uint8_t* mem, uint32_t addr, uint32_t size) {
    if (!mem || !size || tracking_disabled() || !os_arena_tracked(mem)) return false;
    return os_watch_pending(mem, addr, size);
}
bool gx2_write_watch_written_pages(const uint8_t* mem, uint64_t size, std::vector<uint64_t>& pages) {
    const uint64_t page_count = (size + kPage - 1) / kPage;
    pages.assign(size_t((page_count + 63) / 64), 0);
    if (!mem || !size || tracking_disabled() || !os_arena_tracked(mem)) return false;
    constexpr uint64_t kSlicePages = uint64_t(1) << 16; // 256 MiB
    for (uint64_t first = 0; first < page_count; first += kSlicePages) {
        const uint64_t slice = std::min(kSlicePages, page_count - first);
        if (!os_written_pages_slice(mem, first, slice, page_count, pages)) return false;
    }
    std::lock_guard lock(s_lock);
    if (mem == s_mem) {
        const size_t words = std::min(pages.size(), s_page_collected.size());
        for (size_t w = 0; w < words; ++w) pages[w] |= s_page_collected[w];
        if (page_count % 64 && words == pages.size()) pages.back() &= (1ull << (page_count % 64)) - 1;
    }
    return true;
}
#else
#include <cstdlib>
bool gx2_write_watch_register_arena(const uint8_t*, uint64_t) {
    return false;
}
void gx2_write_watch_unregister_arena(const uint8_t*) {}
bool gx2_write_watch_written_pages(const uint8_t*, uint64_t size, std::vector<uint64_t>& pages) {
    pages.assign(size_t((size / 4096 + 64) / 64), 0);
    return false;
}
void gx2_write_watch_begin_batch() {
    static const bool enabled = [] {
        const char* value = std::getenv("RBREW_WRITE_WATCH_REUSE");
        return !(value && value[0] == '0');
    }();
    if (!enabled) return;
    t_watch_batch_id = s_watch_batch_id.fetch_add(1, std::memory_order_relaxed) + 1;
}
uint64_t gx2_write_generation(const uint8_t*, uint32_t, uint32_t) {
    return kGx2WriteUntracked;
}
bool gx2_write_watch_pending(const uint8_t*, uint32_t, uint32_t) {
    return false;
}
uint64_t gx2_write_query_count() {
    return 0;
}
Gx2WriteWatchStats gx2_write_watch_stats() {
    return {};
}
void gx2_write_generations(const uint8_t* mem, Gx2WriteRange* ranges, uint32_t count) {
    if (!ranges) return;
    for (uint32_t i = 0; i < count; ++i)
        ranges[i].generation = gx2_write_generation(mem, ranges[i].address, ranges[i].size);
}
#endif
