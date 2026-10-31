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

#ifndef K3SM_BUDGET_PAGE_HPP
#define K3SM_BUDGET_PAGE_HPP

// budget_page maps a node budget page read-only and reads stable snapshots
// of it under the page's seqlock. The read path throws nothing, allocates
// nothing and never reads the environment.
//
// The seqlock (the writer stores seq odd, every body word, then seq even):
//   1. s1 = seq.load(acquire); an odd s1 is a write in progress, so retry.
//   2. Copy every body word with a relaxed std::atomic_ref load, never a
//      memcpy of racing memory.
//   3. std::atomic_thread_fence(acquire).
//   4. s2 = seq.load(relaxed); s1 != s2 means a write overlapped, so retry.
// After reader_retries attempts without a stable copy, read() returns
// error::torn.

#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <k3sm/page_layout.hpp>
#include <k3sm/result.hpp>
#include <k3sm/snapshot.hpp>

namespace k3sm::sdk {

static_assert(std::endian::native == std::endian::little,
              "the budget page is little-endian; k3sm runs on little-endian arm64 and x86_64");
static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free,
              "the seqlock needs lock-free 8-byte atomic loads");

namespace detail {

// page_magic_word is page_magic as the little-endian u64 at offset 0.
constexpr std::uint64_t page_magic_word() noexcept {
    std::uint64_t w = 0;
    for (std::size_t i = 0; i < page_magic.size(); ++i) {
        w |= std::uint64_t{static_cast<unsigned char>(page_magic[i])} << (8 * i);
    }
    return w;
}

// mapped_source reads page words from a mapping with atomic loads. A word
// is read through std::atomic_ref, which C++20 defines for non-const
// objects only; a load never writes, so it is safe on a read-only mapping.
struct mapped_source {
    const unsigned char* base;
    std::size_t bytes;

    std::size_t size() const noexcept { return bytes; }

    std::uint64_t load(std::size_t offset, std::memory_order order) const noexcept {
        auto* word = reinterpret_cast<std::uint64_t*>(const_cast<unsigned char*>(base + offset));
        return std::atomic_ref<std::uint64_t>(*word).load(order);
    }

    void fence_acquire() const noexcept { std::atomic_thread_fence(std::memory_order_acquire); }
};

// cpu_relax hints the core that the caller is spinning.
inline void cpu_relax() noexcept {
#if defined(__aarch64__) || defined(__arm64__)
    __asm__ __volatile__("yield");
#elif defined(__x86_64__)
    __builtin_ia32_pause();
#endif
}

// body_words is a stable copy of the body this reader knows. The words a
// page's minor does not carry stay page_absent.
struct body_words {
    std::uint64_t w[page_body_size / 8];

    std::uint64_t u64(std::size_t offset) const noexcept {
        return w[(offset - page_header_size) / 8];
    }
    std::int64_t i64(std::size_t offset) const noexcept {
        return static_cast<std::int64_t>(u64(offset));
    }
    opt_i64 opt(std::size_t offset) const noexcept {
        const std::int64_t v = i64(offset);
        if (v == page_absent) {
            return std::nullopt;
        }
        return v;
    }
    bool flag(std::size_t offset) const noexcept { return u64(offset) == 1; }
};

// structural_code reads a code of a structural table of `count` values: an
// unknown code sets *bad and reads as unset.
template <class E>
E structural_code(const body_words& b, std::size_t offset, std::uint64_t count,
                  bool* bad) noexcept {
    const std::uint64_t c = b.u64(offset);
    if (c > count) {
        *bad = true;
        return E{};
    }
    return static_cast<E>(c);
}

// vocabulary_code reads a code of a producer-vocabulary table of `count`
// values: an unknown code reads as unknown (0).
template <class E>
E vocabulary_code(const body_words& b, std::size_t offset, std::uint64_t count) noexcept {
    const std::uint64_t c = b.u64(offset);
    return static_cast<E>(c > count ? 0 : c);
}

// The value counts of the manifest's code tables.
inline constexpr std::uint64_t signal_codes = 7;
inline constexpr std::uint64_t unit_codes = 3;
inline constexpr std::uint64_t level_codes = 4;
inline constexpr std::uint64_t band_codes = 3;
inline constexpr std::uint64_t platform_codes = 2;
inline constexpr std::uint64_t kill_mode_codes = 3;
inline constexpr std::uint64_t trend_codes = 3;
inline constexpr std::uint64_t limiting_resource_codes = 6;
inline constexpr std::uint64_t kill_mode_reason_codes = 5;

inline psi_line decode_psi_line(const body_words& b, std::size_t resource, std::size_t line) noexcept {
    using namespace page_offset;
    return psi_line{
        b.opt(linux_psi_field(resource, line, linux_psi_avg10_permille)),
        b.opt(linux_psi_field(resource, line, linux_psi_avg60_permille)),
        b.opt(linux_psi_field(resource, line, linux_psi_total_us)),
    };
}

inline psi_resource decode_psi_resource(const body_words& b, std::size_t resource) noexcept {
    return psi_resource{decode_psi_line(b, resource, 0), decode_psi_line(b, resource, 1)};
}

// decode fills s from a stable body copy. It returns error::corrupt for a
// field outside its domain, error::abi_mismatch for a structural code the
// tables do not hold, and otherwise success.
inline result<snapshot> decode(const body_words& b, snapshot& s) noexcept {
    namespace off = page_offset;
    s.node_name_len = b.u64(off::node_name_len);
    if (s.node_name_len > max_node_name_bytes) {
        return error::corrupt;
    }
    for (std::size_t i = 0; i < node_name_bytes; ++i) {
        const std::size_t at = off::node_name + i;
        s.node_name_raw[i] = static_cast<char>(b.u64(at & ~std::size_t{7}) >> (8 * (at & 7)));
    }

    bool bad = false;
    s.generation = b.i64(off::generation);
    s.writer_period_seconds = b.i64(off::writer_period_seconds);
    s.observed_at = b.i64(off::observed_at);
    s.platform = structural_code<platform>(b, off::platform, platform_codes, &bad);

    s.capacity = capacity{
        b.opt(off::cpu_capacity_milli),       b.opt(off::cpu_system_reserve_milli),
        b.opt(off::mem_capacity_bytes),       b.opt(off::mem_system_reserve_bytes),
        b.opt(off::mem_eviction_reserve_bytes), b.opt(off::mem_allocatable_bytes),
        b.opt(off::swap_total_bytes),         b.opt(off::swap_used_bytes),
        b.opt(off::cpu_performance_cores),    b.opt(off::cpu_efficiency_cores),
        std::nullopt,                         b.opt(off::gpu_count),
    };
    // gpuPresent is an i64 holding 0, 1 or page_absent.
    if (const opt_i64 gpu = b.opt(off::gpu_present)) {
        if (*gpu != 0 && *gpu != 1) {
            return error::corrupt;
        }
        s.capacity.gpu_present = *gpu == 1;
    }
    s.level = structural_code<level>(b, off::level, level_codes, &bad);

    if (s.platform == platform::darwin) {
        s.darwin = darwin_signals{
            b.opt(off::darwin_vm_pressure_level),
            b.opt(off::darwin_memorystatus_level_permille),
            darwin_compressor{b.opt(off::darwin_compressor_bytes_used),
                              b.opt(off::darwin_compressor_bytes_stored),
                              b.opt(off::darwin_compressor_segment_limit_bytes)},
            b.opt(off::darwin_swapins_total),
            b.opt(off::darwin_swapouts_total),
        };
    }
    bool any_linux = false;
    for (std::size_t o = off::linux_psi; o <= off::linux_memory_events_oom_kill; o += 8) {
        any_linux = any_linux || b.i64(o) != page_absent;
    }
    if (s.platform == platform::linux_ || any_linux) {
        s.linux_ = linux_signals{
            linux_psi{decode_psi_resource(b, 0), decode_psi_resource(b, 1), decode_psi_resource(b, 2)},
            linux_memory_events{b.opt(off::linux_memory_events_high), b.opt(off::linux_memory_events_max),
                                b.opt(off::linux_memory_events_oom),
                                b.opt(off::linux_memory_events_oom_kill)},
        };
    }

    for (std::size_t i = 0; i < max_signals; ++i) {
        const std::size_t base = off::signal_slot(i);
        namespace f = off::signal_field;
        if (b.u64(base + f::id) == 0) {
            continue;
        }
        signal_record& r = s.signals[s.signal_count++];
        r.id = structural_code<signal_id>(b, base + f::id, signal_codes, &bad);
        r.unit = structural_code<unit>(b, base + f::unit, unit_codes, &bad);
        r.value = b.opt(base + f::value);
        r.warn_threshold = b.opt(base + f::warn_threshold);
        r.admit_stop_threshold = b.opt(base + f::admit_stop_threshold);
        r.critical_threshold = b.opt(base + f::critical_threshold);
        r.evict_soft_threshold = b.opt(base + f::evict_soft_threshold);
        r.evict_hard_threshold = b.opt(base + f::evict_hard_threshold);
        r.warn_release = b.opt(base + f::warn_release);
        r.admit_stop_release = b.opt(base + f::admit_stop_release);
        r.critical_release = b.opt(base + f::critical_release);
        r.evict_soft_release = b.opt(base + f::evict_soft_release);
        r.evict_hard_release = b.opt(base + f::evict_hard_release);
        r.grace_seconds = b.opt(base + f::grace_seconds);
        r.min_reclaim_bytes = b.opt(base + f::min_reclaim_bytes);
        r.window_seconds = b.opt(base + f::window_seconds);
        r.eta_to_next_rung_seconds = b.opt(base + f::eta_to_next_rung_seconds);
        r.trend = vocabulary_code<trend>(b, base + f::trend, trend_codes);
        r.limiting_resource =
            vocabulary_code<limiting_resource>(b, base + f::limiting_resource, limiting_resource_codes);
        r.shared_with = structural_code<signal_id>(b, base + f::shared_with, signal_codes, &bad);
    }

    s.admit = b.flag(off::admit);
    s.admit_reason_signal = structural_code<signal_id>(b, off::admit_reason_signal, signal_codes, &bad);
    s.evict_deadline = b.opt(off::evict_deadline);
    s.kill_mode.mode = structural_code<kill_mode>(b, off::kill_mode_mode, kill_mode_codes, &bad);
    s.kill_mode.armed = b.flag(off::kill_mode_armed);
    s.kill_mode.arming_episodes = b.i64(off::kill_mode_arming_episodes);
    s.kill_mode.reason =
        vocabulary_code<kill_mode_reason>(b, off::kill_mode_reason, kill_mode_reason_codes);
    s.recommendation.safe_additional_bytes = b.opt(off::recommendation_safe_additional_bytes);
    s.recommendation.basis = structural_code<signal_id>(b, off::recommendation_basis, signal_codes, &bad);

    for (std::size_t i = 0; i < max_counters; ++i) {
        const std::size_t base = off::counter_slot(i);
        namespace f = off::counter_field;
        if (b.u64(base + f::band) == 0) {
            continue;
        }
        band_counters& c = s.counters[s.counter_count++];
        c.band = structural_code<band>(b, base + f::band, band_codes, &bad);
        c.evictions_total = b.i64(base + f::evictions_total);
        c.oom_kills_total = b.i64(base + f::oom_kills_total);
        c.kernel_kills_total = b.i64(base + f::kernel_kills_total);
        c.restarts_total = b.i64(base + f::restarts_total);
        c.mem_high_events_total = b.i64(base + f::mem_high_events_total);
        c.tier1_refused_total = b.i64(base + f::tier1_refused_total);
    }
    if (bad) {
        return error::abi_mismatch;
    }
    return s;
}

// is_stale reports whether now - observed_at exceeds stale_factor writer
// periods, without overflowing on any field values.
inline bool is_stale(std::int64_t observed_at, std::int64_t writer_period_seconds,
                     std::int64_t now) noexcept {
    constexpr std::int64_t max = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t min = std::numeric_limits<std::int64_t>::min();
    std::int64_t bound = writer_period_seconds;
    if (bound > max / stale_factor) {
        bound = max;
    } else if (bound < min / stale_factor) {
        bound = min;
    } else {
        bound *= stale_factor;
    }
    std::int64_t age = 0;
    if (__builtin_sub_overflow(now, observed_at, &age)) {
        return now > observed_at;  // the age is beyond any bound, or below any
    }
    return age > bound;
}

// read_page reads one snapshot from src (a mapped_source, or a test's
// scripted source with the same three members) and checks it against now
// (Unix seconds). The checks run in the contract's order: truncated header
// line, magic, major, header size, the body size of the page's minor (a
// newer minor's is read as this SDK's newest), file size, then the
// seqlock copy (torn), the field domains (corrupt), the codes
// (abi_mismatch) and staleness.
template <class Source>
result<snapshot> read_page(const Source& src, std::int64_t now) noexcept {
    namespace off = page_offset;
    if (src.size() < off::seq) {
        return error::truncated;
    }
    if (src.load(off::magic, std::memory_order_relaxed) != page_magic_word()) {
        return error::abi_mismatch;
    }
    const std::uint64_t versions = src.load(off::abi_major, std::memory_order_relaxed);
    const std::uint64_t sizes = src.load(off::body_size, std::memory_order_relaxed);
    snapshot s;
    s.abi_major = static_cast<std::uint16_t>(versions);
    s.abi_minor = static_cast<std::uint16_t>(versions >> 16);
    s.header_size = static_cast<std::uint32_t>(versions >> 32);
    s.body_size = static_cast<std::uint32_t>(sizes);
    if (s.abi_major != page_abi_major || s.header_size != page_header_size) {
        return error::abi_mismatch;
    }
    const std::uint32_t known = page_body_size_of(s.abi_minor);
    if (s.body_size < known ||
        src.size() < std::uint64_t{s.header_size} + std::uint64_t{s.body_size}) {
        return error::truncated;
    }

    body_words b;
    for (std::size_t i = known / 8; i < page_body_size / 8; ++i) {
        b.w[i] = static_cast<std::uint64_t>(page_absent);
    }
    bool stable = false;
    for (int attempt = 0; attempt < reader_retries && !stable; ++attempt) {
        const std::uint64_t s1 = src.load(off::seq, std::memory_order_acquire);
        if (s1 % 2 == 1) {
            cpu_relax();
            continue;
        }
        for (std::size_t i = 0; i < known / 8; ++i) {
            b.w[i] = src.load(page_header_size + 8 * i, std::memory_order_relaxed);
        }
        src.fence_acquire();
        const std::uint64_t s2 = src.load(off::seq, std::memory_order_relaxed);
        if (s1 == s2) {
            s.seq = s1;
            stable = true;
        } else {
            cpu_relax();
        }
    }
    if (!stable) {
        return error::torn;
    }
    s.notify_epoch = src.load(off::notify_epoch, std::memory_order_relaxed);

    result<snapshot> r = decode(b, s);
    if (r && is_stale(r->observed_at, r->writer_period_seconds, now)) {
        return error::stale;
    }
    return r;
}

// now_unix_seconds returns CLOCK_REALTIME in whole seconds.
inline std::int64_t now_unix_seconds() noexcept {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace detail

// budget_page is a read-only mapping of one budget page. It is move-only;
// destroying it unmaps the page. Reading is safe from any number of threads
// at once.
class budget_page {
public:
    // open maps the page at path read-only. It fails with error::not_found
    // (carrying errno) when the file cannot be opened, stat'ed or mapped, and
    // with error::truncated when it is empty. Every other check runs on each
    // read, because the writer may take the page over between reads.
    static result<budget_page> open(const char* path) noexcept {
        const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return error(error::not_found, errno);
        }
        struct stat st {};
        if (::fstat(fd, &st) != 0) {
            const int e = errno;
            ::close(fd);
            return error(error::not_found, e);
        }
        if (st.st_size <= 0) {
            ::close(fd);
            return error::truncated;
        }
        const auto bytes = static_cast<std::size_t>(st.st_size);
        void* base = ::mmap(nullptr, bytes, PROT_READ, MAP_SHARED, fd, 0);
        const int e = errno;
        ::close(fd);  // the mapping keeps the file
        if (base == MAP_FAILED) {
            return error(error::not_found, e);
        }
        return budget_page(static_cast<const unsigned char*>(base), bytes);
    }

    budget_page(const budget_page&) = delete;
    budget_page& operator=(const budget_page&) = delete;
    budget_page(budget_page&& o) noexcept
        : base_(std::exchange(o.base_, nullptr)), size_(std::exchange(o.size_, 0)) {}
    budget_page& operator=(budget_page&& o) noexcept {
        if (this != &o) {
            unmap();
            base_ = std::exchange(o.base_, nullptr);
            size_ = std::exchange(o.size_, 0);
        }
        return *this;
    }
    ~budget_page() { unmap(); }

    // read returns a stable snapshot of the page, checked against the
    // current CLOCK_REALTIME for staleness.
    result<snapshot> read() const noexcept { return read_at(detail::now_unix_seconds()); }

    // read_at is read with the staleness check made against now_unix_seconds.
    result<snapshot> read_at(std::int64_t now_unix_seconds) const noexcept {
        return detail::read_page(detail::mapped_source{base_, size_}, now_unix_seconds);
    }

    // data and size are the mapping, for a notifier waiting on notifyEpoch.
    const void* data() const noexcept { return base_; }
    std::size_t size() const noexcept { return size_; }

private:
    budget_page(const unsigned char* base, std::size_t size) noexcept : base_(base), size_(size) {}

    void unmap() noexcept {
        if (base_ != nullptr) {
            ::munmap(const_cast<unsigned char*>(base_), size_);
            base_ = nullptr;
            size_ = 0;
        }
    }

    const unsigned char* base_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace k3sm::sdk

#endif  // K3SM_BUDGET_PAGE_HPP
