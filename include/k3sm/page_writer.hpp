/*
Copyright The k3sm Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#ifndef K3SM_PAGE_WRITER_HPP
#define K3SM_PAGE_WRITER_HPP

// The writer half of the node budget page, for a producer that publishes
// the page through this SDK. It encodes a snapshot with the same field
// offsets the reader decodes with (page_layout.hpp, checked by the tests
// against the layout descriptor and the golden pages the apis repository
// publishes); there is no second layout.
//
// The seqlock, as the reader expects it (budget_page.hpp):
//   1. seq = odd (relaxed), then std::atomic_thread_fence(release), so no
//      body store becomes visible before the odd seq;
//   2. every body word, headerSize to headerSize + bodySize, as a relaxed
//      std::atomic_ref store (never a memcpy into the mapping);
//   3. seq = the next even value (release);
//   4. only when the publication changes the level, admit, the generation or
//      a threshold (a rung or a release value of any signal slot):
//      notifyEpoch + 1 (release), then a wake of every waiter on the word
//      (os_sync_wake_by_address_all on Darwin, FUTEX_WAKE on Linux) and a
//      write of the doorbell file when the writer has one.
//
// The page file is preallocated at page_size, opened in place and mapped
// shared. It is never renamed, replaced or truncated, so a reader's open
// mapping keeps working across writer restarts. One writer per page: the
// writer holds an exclusive flock on a lock file that must live outside the
// page's directory (a pod can open that directory). The writer owns the
// page's generation: it is the previous page's generation plus one at every
// open (a writer restart), and bump_generation() adds one on a policy
// change.

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__APPLE__)
#if __has_include(<os/os_sync_wait_on_address.h>)
#include <os/os_sync_wait_on_address.h>
#define K3SM_SDK_HAVE_OS_SYNC_WAKE 1
#endif
#elif defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#endif

#include <k3sm/budget_page.hpp>
#include <k3sm/page_layout.hpp>
#include <k3sm/result.hpp>
#include <k3sm/snapshot.hpp>

namespace k3sm::sdk {

namespace detail {

// page_image is a page under construction: page_size bytes, 8-byte aligned,
// written word by word in little-endian order.
struct page_image {
    std::uint64_t w[page_size / 8];

    void put(std::size_t offset, std::uint64_t v) noexcept { w[offset / 8] = v; }
    void put_i64(std::size_t offset, std::int64_t v) noexcept {
        put(offset, static_cast<std::uint64_t>(v));
    }
    void put_opt(std::size_t offset, const opt_i64& v) noexcept {
        put_i64(offset, v ? *v : page_absent);
    }
    template <class E>
    void put_code(std::size_t offset, E e) noexcept {
        put(offset, static_cast<std::uint64_t>(e));
    }
};

template <class E>
constexpr bool code_in(E e, std::uint64_t count) noexcept {
    return static_cast<std::uint64_t>(e) <= count;
}

// check_snapshot holds s to what the layout can carry, as the apis encoder
// does: the node name, the writer period, the platform and level codes, the
// slot counts, the platform blocks and every code of every filled slot.
inline result<std::size_t> check_snapshot(const snapshot& s) noexcept {
    if (s.node_name_len > max_node_name_bytes) {
        return error::node_name_too_long;
    }
    const bool platform_ok = s.platform == platform::darwin || s.platform == platform::linux_;
    const bool level_ok = s.level != level::unset && code_in(s.level, level_codes);
    if (s.node_name_len == 0 || s.writer_period_seconds <= 0 || !platform_ok || !level_ok ||
        s.signal_count > max_signals || s.counter_count > max_counters) {
        return error::invalid_snapshot;
    }
    if (s.platform == platform::darwin && !s.darwin) {
        return error::invalid_snapshot;
    }
    if (s.platform == platform::linux_ && (!s.linux_ || s.darwin)) {
        return error::invalid_snapshot;
    }
    bool ok = code_in(s.admit_reason_signal, signal_codes) && code_in(s.kill_mode.mode, kill_mode_codes) &&
              code_in(s.kill_mode.reason, kill_mode_reason_codes) &&
              code_in(s.recommendation.basis, signal_codes);
    for (std::size_t i = 0; i < s.signal_count; ++i) {
        const signal_record& r = s.signals[i];
        ok = ok && r.id != signal_id::unset && code_in(r.id, signal_codes) && code_in(r.unit, unit_codes) &&
             code_in(r.trend, trend_codes) && code_in(r.limiting_resource, limiting_resource_codes) &&
             code_in(r.shared_with, signal_codes);
    }
    for (std::size_t i = 0; i < s.counter_count; ++i) {
        ok = ok && s.counters[i].band != band::unset && code_in(s.counters[i].band, band_codes);
    }
    if (!ok) {
        return error::invalid_snapshot;
    }
    return std::size_t{0};
}

inline void encode_psi_line(page_image& p, const psi_line& l, std::size_t resource,
                            std::size_t line) noexcept {
    using namespace page_offset;
    p.put_opt(linux_psi_field(resource, line, linux_psi_avg10_permille), l.avg10_permille);
    p.put_opt(linux_psi_field(resource, line, linux_psi_avg60_permille), l.avg60_permille);
    p.put_opt(linux_psi_field(resource, line, linux_psi_total_us), l.total_us);
}

// encode fills p (zeroed by the caller) from a checked snapshot: the header
// line with seq and notifyEpoch 0, then the body. It is decode's inverse,
// over the same page_offset table.
inline void encode(const snapshot& s, page_image& p) noexcept {
    namespace off = page_offset;
    p.put(off::magic, page_magic_word());
    p.put(off::abi_major, std::uint64_t{page_abi_major} | std::uint64_t{page_abi_minor} << 16 |
                              std::uint64_t{page_header_size} << 32);
    p.put(off::body_size, page_body_size);

    p.put_i64(off::generation, s.generation);
    p.put_i64(off::writer_period_seconds, s.writer_period_seconds);
    p.put_i64(off::observed_at, s.observed_at);
    p.put_code(off::platform, s.platform);
    p.put(off::node_name_len, s.node_name_len);
    auto* name = reinterpret_cast<unsigned char*>(p.w) + off::node_name;
    std::memcpy(name, s.node_name_raw.data(), static_cast<std::size_t>(s.node_name_len));

    const capacity& c = s.capacity;
    p.put_opt(off::cpu_capacity_milli, c.cpu_capacity_milli);
    p.put_opt(off::cpu_system_reserve_milli, c.cpu_system_reserve_milli);
    p.put_opt(off::mem_capacity_bytes, c.mem_capacity_bytes);
    p.put_opt(off::mem_system_reserve_bytes, c.mem_system_reserve_bytes);
    p.put_opt(off::mem_eviction_reserve_bytes, c.mem_eviction_reserve_bytes);
    p.put_opt(off::mem_allocatable_bytes, c.mem_allocatable_bytes);
    p.put_opt(off::swap_total_bytes, c.swap_total_bytes);
    p.put_opt(off::swap_used_bytes, c.swap_used_bytes);
    p.put_opt(off::cpu_performance_cores, c.cpu_performance_cores);
    p.put_opt(off::cpu_efficiency_cores, c.cpu_efficiency_cores);
    p.put_i64(off::gpu_present, c.gpu_present ? std::int64_t{*c.gpu_present} : page_absent);
    p.put_opt(off::gpu_count, c.gpu_count);
    p.put_code(off::level, s.level);

    const darwin_signals d = s.darwin.value_or(darwin_signals{});
    p.put_opt(off::darwin_vm_pressure_level, d.vm_pressure_level);
    p.put_opt(off::darwin_memorystatus_level_permille, d.memorystatus_level_permille);
    p.put_opt(off::darwin_compressor_bytes_used, d.compressor.bytes_used);
    p.put_opt(off::darwin_compressor_bytes_stored, d.compressor.bytes_stored);
    p.put_opt(off::darwin_compressor_segment_limit_bytes, d.compressor.segment_limit_bytes);
    p.put_opt(off::darwin_swapins_total, d.swapins_total);
    p.put_opt(off::darwin_swapouts_total, d.swapouts_total);

    const linux_signals l = s.linux_.value_or(linux_signals{});
    const psi_resource* res[3] = {&l.psi.cpu, &l.psi.memory, &l.psi.io};
    for (std::size_t r = 0; r < 3; ++r) {
        encode_psi_line(p, res[r]->some, r, 0);
        encode_psi_line(p, res[r]->full, r, 1);
    }
    p.put_opt(off::linux_memory_events_high, l.memory_events.high);
    p.put_opt(off::linux_memory_events_max, l.memory_events.max);
    p.put_opt(off::linux_memory_events_oom, l.memory_events.oom);
    p.put_opt(off::linux_memory_events_oom_kill, l.memory_events.oom_kill);

    // Filled slots in order; an empty slot stays all zero bytes.
    for (std::size_t i = 0; i < s.signal_count; ++i) {
        const std::size_t base = off::signal_slot(i);
        namespace f = off::signal_field;
        const signal_record& r = s.signals[i];
        p.put_code(base + f::id, r.id);
        p.put_code(base + f::unit, r.unit);
        p.put_opt(base + f::value, r.value);
        p.put_opt(base + f::warn_threshold, r.warn_threshold);
        p.put_opt(base + f::admit_stop_threshold, r.admit_stop_threshold);
        p.put_opt(base + f::critical_threshold, r.critical_threshold);
        p.put_opt(base + f::evict_soft_threshold, r.evict_soft_threshold);
        p.put_opt(base + f::evict_hard_threshold, r.evict_hard_threshold);
        p.put_opt(base + f::warn_release, r.warn_release);
        p.put_opt(base + f::admit_stop_release, r.admit_stop_release);
        p.put_opt(base + f::critical_release, r.critical_release);
        p.put_opt(base + f::evict_soft_release, r.evict_soft_release);
        p.put_opt(base + f::evict_hard_release, r.evict_hard_release);
        p.put_opt(base + f::grace_seconds, r.grace_seconds);
        p.put_opt(base + f::min_reclaim_bytes, r.min_reclaim_bytes);
        p.put_opt(base + f::window_seconds, r.window_seconds);
        p.put_opt(base + f::eta_to_next_rung_seconds, r.eta_to_next_rung_seconds);
        p.put_code(base + f::trend, r.trend);
        p.put_code(base + f::limiting_resource, r.limiting_resource);
        p.put_code(base + f::shared_with, r.shared_with);
    }

    p.put(off::admit, s.admit ? 1 : 0);
    p.put_code(off::admit_reason_signal, s.admit_reason_signal);
    p.put_opt(off::evict_deadline, s.evict_deadline);
    p.put_code(off::kill_mode_mode, s.kill_mode.mode);
    p.put(off::kill_mode_armed, s.kill_mode.armed ? 1 : 0);
    p.put_i64(off::kill_mode_arming_episodes, s.kill_mode.arming_episodes);
    p.put_code(off::kill_mode_reason, s.kill_mode.reason);
    p.put_opt(off::recommendation_safe_additional_bytes, s.recommendation.safe_additional_bytes);
    p.put_code(off::recommendation_basis, s.recommendation.basis);

    for (std::size_t i = 0; i < s.counter_count; ++i) {
        const std::size_t base = off::counter_slot(i);
        namespace f = off::counter_field;
        const band_counters& b = s.counters[i];
        p.put_code(base + f::band, b.band);
        p.put_i64(base + f::evictions_total, b.evictions_total);
        p.put_i64(base + f::oom_kills_total, b.oom_kills_total);
        p.put_i64(base + f::kernel_kills_total, b.kernel_kills_total);
        p.put_i64(base + f::restarts_total, b.restarts_total);
        p.put_i64(base + f::mem_high_events_total, b.mem_high_events_total);
        p.put_i64(base + f::tier1_refused_total, b.tier1_refused_total);
    }
}

// notify_key is what notify_due compares: the level, admit, the generation
// and every rung and release value of every signal slot (absent for an
// empty slot), as the apis writer compares them.
struct notify_key {
    level lvl = level::unset;
    bool admit = false;
    std::int64_t generation = 0;
    std::int64_t ladder[max_signals][10] = {};

    friend bool operator==(const notify_key&, const notify_key&) = default;
};

inline notify_key notify_key_of(const snapshot& s) noexcept {
    notify_key k;
    k.lvl = s.level;
    k.admit = s.admit;
    k.generation = s.generation;
    for (std::size_t i = 0; i < max_signals; ++i) {
        for (std::int64_t& v : k.ladder[i]) {
            v = page_absent;
        }
        if (i >= s.signal_count) {
            continue;
        }
        const signal_record& r = s.signals[i];
        const opt_i64* steps[10] = {
            &r.warn_threshold, &r.admit_stop_threshold, &r.critical_threshold,
            &r.evict_soft_threshold, &r.evict_hard_threshold, &r.warn_release,
            &r.admit_stop_release, &r.critical_release, &r.evict_soft_release,
            &r.evict_hard_release,
        };
        for (std::size_t j = 0; j < 10; ++j) {
            if (*steps[j]) {
                k.ladder[i][j] = **steps[j];
            }
        }
    }
    return k;
}

inline std::uint64_t* word_at(unsigned char* base, std::size_t offset) noexcept {
    return reinterpret_cast<std::uint64_t*>(base + offset);
}

inline void store(unsigned char* base, std::size_t offset, std::uint64_t v,
                  std::memory_order order) noexcept {
    std::atomic_ref<std::uint64_t>(*word_at(base, offset)).store(v, order);
}

inline std::uint64_t load(unsigned char* base, std::size_t offset) noexcept {
    return std::atomic_ref<std::uint64_t>(*word_at(base, offset)).load(std::memory_order_acquire);
}

// wake_all wakes every process waiting on the notifyEpoch word at word.
inline void wake_all(std::uint64_t* word) noexcept {
#if defined(K3SM_SDK_HAVE_OS_SYNC_WAKE)
    if (__builtin_available(macOS 14.4, iOS 17.4, tvOS 17.4, watchOS 10.4, *)) {
        // ENOENT (nobody waiting) is the common answer, not a failure.
        (void)os_sync_wake_by_address_all(word, sizeof(std::uint64_t), OS_SYNC_WAKE_BY_ADDRESS_SHARED);
    }
#elif defined(__linux__)
    // FUTEX_WAKE without FUTEX_PRIVATE_FLAG reaches waiters in other
    // processes; it wakes on the word's low 32 bits, where they wait.
    (void)::syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(word), FUTEX_WAKE, INT_MAX, nullptr,
                    nullptr, 0);
#else
    (void)word;
#endif
}

// parent_dir_stat stats the directory that holds path.
inline bool parent_dir_stat(const char* path, struct stat* out) noexcept {
    char dir[PATH_MAX];
    const std::size_t n = std::strlen(path);
    if (n == 0 || n >= sizeof dir) {
        return false;
    }
    std::memcpy(dir, path, n + 1);
    char* slash = std::strrchr(dir, '/');
    if (slash == nullptr) {
        dir[0] = '.';
        dir[1] = '\0';
    } else if (slash == dir) {
        dir[1] = '\0';
    } else {
        *slash = '\0';
    }
    return ::stat(dir, out) == 0;
}

// preallocate makes the file at fd at least page_size bytes long by writing
// zero bytes over the missing range, so the blocks exist before the page is
// mapped and a store through the mapping never meets an unbacked page. It
// only ever extends the file.
inline bool preallocate(int fd, std::size_t current) noexcept {
    const unsigned char zeros[512] = {};
    std::size_t at = current;
    while (at < page_size) {
        const std::size_t n = std::min(sizeof zeros, page_size - at);
        const ssize_t w = ::pwrite(fd, zeros, n, static_cast<off_t>(at));
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            return false;
        }
        at += static_cast<std::size_t>(w);
    }
    return ::fsync(fd) == 0 || errno == EINVAL;
}

}  // namespace detail

// encode_page writes the page of s into dst[0, page_size), deterministically,
// as the apis encoder does: the header, seq 0 and notifyEpoch 0, the body,
// and zero bytes everywhere else (the node name's unused bytes included).
// s's header fields, seq and notify_epoch are ignored. It returns page_size,
// error::invalid_argument for a dst shorter than page_size, or the
// check_snapshot error; dst is written only on success.
inline result<std::size_t> encode_page(const snapshot& s, unsigned char* dst,
                                       std::size_t dst_size) noexcept {
    if (dst == nullptr || dst_size < page_size) {
        return error::invalid_argument;
    }
    result<std::size_t> ok = detail::check_snapshot(s);
    if (!ok) {
        return ok;
    }
    detail::page_image p{};
    detail::encode(s, p);
    std::memcpy(dst, p.w, page_size);
    return page_size;
}

// notify_due reports whether publishing next after prev bumps notifyEpoch: a
// change of the level, of admit, of the generation, or of any threshold (a
// rung or release value of any signal slot, a value becoming absent or
// present included). A change of anything else, a signal's value or a
// counter included, is not due. A null prev (the first publication) is due.
inline bool notify_due(const snapshot* prev, const snapshot& next) noexcept {
    return prev == nullptr || !(detail::notify_key_of(*prev) == detail::notify_key_of(next));
}

// page_writer publishes snapshots into one page file under the seqlock. It
// is move-only and not safe for concurrent use; destroying it unmaps the
// page and releases the writer lock, leaving the page in place for the
// next writer.
class page_writer {
public:
    // open takes the writer lock at lock_path (created 0600 if missing; it
    // must not be in page_path's directory), opens or creates the page at
    // page_path (0644, preallocated to page_size; an existing file is never
    // truncated or replaced), maps it shared and takes it over: seq and
    // notifyEpoch continue from a page of this ABI major, the generation is
    // the page's previous generation plus one (1 on a new page), and the
    // header line is rewritten with the magic last. doorbell_path, when not
    // null, names a doorbell file (created 0644 if missing) the writer
    // writes after every notifyEpoch bump. Errors: writer_busy when another
    // writer holds the lock, invalid_argument for a lock file inside the
    // page directory, io (with errno) for a failing system call.
    static result<page_writer> open(const char* page_path, const char* lock_path,
                                    const char* doorbell_path = nullptr) noexcept {
        if (page_path == nullptr || lock_path == nullptr) {
            return error::invalid_argument;
        }
        struct stat page_dir {};
        struct stat lock_dir {};
        if (!detail::parent_dir_stat(page_path, &page_dir) || !detail::parent_dir_stat(lock_path, &lock_dir)) {
            return error(error::io, errno);
        }
        if (page_dir.st_dev == lock_dir.st_dev && page_dir.st_ino == lock_dir.st_ino) {
            return error::invalid_argument;
        }

        page_writer w;
        w.lock_fd_ = ::open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (w.lock_fd_ < 0) {
            return error(error::io, errno);
        }
        if (::flock(w.lock_fd_, LOCK_EX | LOCK_NB) != 0) {
            return errno == EWOULDBLOCK ? error(error::writer_busy) : error(error::io, errno);
        }

        const int fd = ::open(page_path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0) {
            return error(error::io, errno);
        }
        struct stat st {};
        if (::fstat(fd, &st) != 0 || !detail::preallocate(fd, static_cast<std::size_t>(st.st_size))) {
            const int e = errno;
            ::close(fd);
            return error(error::io, e);
        }
        void* base = ::mmap(nullptr, page_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        const int e = errno;
        ::close(fd);  // the mapping keeps the file
        if (base == MAP_FAILED) {
            return error(error::io, e);
        }
        w.base_ = static_cast<unsigned char*>(base);

        if (doorbell_path != nullptr) {
            w.bell_fd_ = ::open(doorbell_path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
            if (w.bell_fd_ < 0) {
                return error(error::io, errno);
            }
        }
        w.resume();
        return w;
    }

    page_writer(const page_writer&) = delete;
    page_writer& operator=(const page_writer&) = delete;
    page_writer(page_writer&& o) noexcept { take(o); }
    page_writer& operator=(page_writer&& o) noexcept {
        if (this != &o) {
            release();
            take(o);
        }
        return *this;
    }
    ~page_writer() { release(); }

    // publish writes s to the page under the seqlock with the writer's
    // generation in place of s.generation, and bumps notifyEpoch (with the
    // wake) when notify_due. An invalid snapshot is refused before any
    // store. It returns the new, even seq.
    result<std::uint64_t> publish(const snapshot& s) noexcept {
        snapshot next = s;
        next.generation = generation_;
        detail::page_image p{};
        {
            result<std::size_t> ok = detail::check_snapshot(next);
            if (!ok) {
                return ok.error();
            }
        }
        detail::encode(next, p);
        const detail::notify_key key = detail::notify_key_of(next);
        const bool due = !has_last_ || !(key == last_);

        namespace off = page_offset;
        detail::store(base_, off::seq, seq_ + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (std::size_t o = page_header_size; o < page_header_size + page_body_size; o += 8) {
            detail::store(base_, o, p.w[o / 8], std::memory_order_relaxed);
        }
        seq_ += 2;
        detail::store(base_, off::seq, seq_, std::memory_order_release);
        if (due) {
            ++epoch_;
            detail::store(base_, off::notify_epoch, epoch_, std::memory_order_release);
            detail::wake_all(detail::word_at(base_, off::notify_epoch));
            ring();
        }
        last_ = key;
        has_last_ = true;
        return seq_;
    }

    // bump_generation adds one to the generation, for a policy change. The
    // next publish carries it and so bumps notifyEpoch.
    void bump_generation() noexcept { ++generation_; }

    // generation is the generation the next publish writes.
    std::int64_t generation() const noexcept { return generation_; }
    // seq is the last seq the writer stored (even once it has published).
    std::uint64_t seq() const noexcept { return seq_; }
    // epoch is the last notifyEpoch the writer stored.
    std::uint64_t epoch() const noexcept { return epoch_; }

private:
    page_writer() noexcept = default;

    // resume takes the page over, as the apis writer does: it continues seq
    // and notifyEpoch from a page of this major (an odd seq left by a writer
    // that died mid-write is moved past), takes the next generation, and
    // writes this minor's header line, the magic last.
    void resume() noexcept {
        namespace off = page_offset;
        const std::uint64_t magic = detail::load(base_, off::magic);
        const std::uint64_t versions = detail::load(base_, off::abi_major);
        std::int64_t previous = 0;
        seq_ = 0;
        epoch_ = 0;
        if (magic == detail::page_magic_word() &&
            static_cast<std::uint16_t>(versions) == page_abi_major &&
            static_cast<std::uint32_t>(versions >> 32) == page_header_size) {
            seq_ = detail::load(base_, off::seq);
            epoch_ = detail::load(base_, off::notify_epoch);
            previous = static_cast<std::int64_t>(detail::load(base_, off::generation));
            if (seq_ % 2 == 1) {
                ++seq_;
            }
        }
        generation_ = previous < 0 ? 1 : previous + 1;

        detail::page_image header{};
        header.put(off::magic, detail::page_magic_word());
        header.put(off::abi_major, std::uint64_t{page_abi_major} | std::uint64_t{page_abi_minor} << 16 |
                                       std::uint64_t{page_header_size} << 32);
        header.put(off::body_size, page_body_size);
        for (std::size_t o = off::seq; o > 0;) {
            o -= 8;
            detail::store(base_, o, header.w[o / 8], std::memory_order_release);
        }
        has_last_ = false;
    }

    // ring writes the doorbell, for a reader that waits on the file.
    void ring() noexcept {
        if (bell_fd_ >= 0) {
            const char one = '1';
            (void)::pwrite(bell_fd_, &one, 1, 0);
        }
    }

    void take(page_writer& o) noexcept {
        base_ = std::exchange(o.base_, nullptr);
        lock_fd_ = std::exchange(o.lock_fd_, -1);
        bell_fd_ = std::exchange(o.bell_fd_, -1);
        seq_ = o.seq_;
        epoch_ = o.epoch_;
        generation_ = o.generation_;
        last_ = o.last_;
        has_last_ = o.has_last_;
    }

    void release() noexcept {
        if (base_ != nullptr) {
            ::munmap(base_, page_size);
            base_ = nullptr;
        }
        if (bell_fd_ >= 0) {
            ::close(bell_fd_);
            bell_fd_ = -1;
        }
        if (lock_fd_ >= 0) {
            ::close(lock_fd_);  // releases the flock
            lock_fd_ = -1;
        }
    }

    unsigned char* base_ = nullptr;
    int lock_fd_ = -1;
    int bell_fd_ = -1;
    std::uint64_t seq_ = 0;
    std::uint64_t epoch_ = 0;
    std::int64_t generation_ = 0;
    detail::notify_key last_{};
    bool has_last_ = false;
};

}  // namespace k3sm::sdk

#endif  // K3SM_PAGE_WRITER_HPP
