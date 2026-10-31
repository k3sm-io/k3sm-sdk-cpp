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

#ifndef K3SM_SNAPSHOT_HPP
#define K3SM_SNAPSHOT_HPP

// snapshot is one stable copy of a budget page: every field of the layout,
// under the same names as the NodeBudget status (in snake_case) and in the
// same units (seconds, bytes, permille, counts). An optional i64 the
// producer could not sample is std::nullopt; codes are typed enums.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include <k3sm/page_layout.hpp>

namespace k3sm::sdk {

// Every code below is the page's u64 code: a table value's index plus one,
// and 0 for unset (or, for the producer-vocabulary codes trend,
// limiting_resource and kill_mode_reason, unknown). A code a structural
// table does not hold makes read() return error::abi_mismatch; an unknown
// producer-vocabulary code reads as unknown.

// signal_id names a budget signal.
enum class signal_id : std::uint64_t {
    unset = 0,
    memory_available = 1,
    memory_compressor = 2,
    memory_swap = 3,
    swapfs_available = 4,
    nodefs_available = 5,
    imagefs_available = 6,
    pid_available = 7,
};

// unit is the unit of a signal's value and rungs.
enum class unit : std::uint64_t { unset = 0, bytes = 1, permille = 2, count = 3 };

// level is the node's derived pressure level, ordered by severity.
enum class level : std::uint64_t { unset = 0, ok = 1, warn = 2, critical = 3, evicting = 4 };

// band is a pod priority band.
enum class band : std::uint64_t { unset = 0, critical = 1, standard = 2, batch = 3 };

// platform names the producer's platform. linux_ carries a trailing
// underscore because GNU compilers define a `linux` macro.
enum class platform : std::uint64_t { unset = 0, darwin = 1, linux_ = 2 };

// kill_mode is the published tier-1 kill mode. auto_ carries a trailing
// underscore because `auto` is a keyword.
enum class kill_mode : std::uint64_t { unset = 0, auto_ = 1, on = 2, off = 3 };

// trend is a signal's direction over its fit window.
enum class trend : std::uint64_t { unknown = 0, falling = 1, steady = 2, rising = 3 };

// limiting_resource is the resource a signal limits.
enum class limiting_resource : std::uint64_t {
    unknown = 0,
    memory = 1,
    swapfs = 2,
    nodefs = 3,
    imagefs = 4,
    pid = 5,
    cpu = 6,
};

// kill_mode_reason is why the kill mode is in its state.
enum class kill_mode_reason : std::uint64_t {
    unknown = 0,
    cold_start = 1,
    observing = 2,
    armed = 3,
    policy_off = 4,
    disarm_file = 5,
};

// opt_i64 is an i64 page field that may be absent.
using opt_i64 = std::optional<std::int64_t>;

// capacity is the node's capacity and reserves. The last four fields are
// page minor 1: each reads absent on a minor 0 page.
struct capacity {
    opt_i64 cpu_capacity_milli;
    opt_i64 cpu_system_reserve_milli;
    opt_i64 mem_capacity_bytes;
    opt_i64 mem_system_reserve_bytes;
    opt_i64 mem_eviction_reserve_bytes;
    opt_i64 mem_allocatable_bytes;
    opt_i64 swap_total_bytes;
    opt_i64 swap_used_bytes;
    // cpu_performance_cores and cpu_efficiency_cores are the logical CPUs in
    // the performance and the efficiency level; absent where the host has no
    // performance level split.
    opt_i64 cpu_performance_cores;
    opt_i64 cpu_efficiency_cores;
    // gpu_present is whether the node advertises a GPU to pods (the
    // mlx.k3sm.io/gpu extended resource); absent when the producer does not
    // say.
    std::optional<bool> gpu_present;
    // gpu_count is the count of that extended resource.
    opt_i64 gpu_count;
};

// darwin_compressor is the macOS compressor detail.
struct darwin_compressor {
    opt_i64 bytes_used;
    opt_i64 bytes_stored;
    opt_i64 segment_limit_bytes;
};

// darwin_signals is the macOS platform block.
struct darwin_signals {
    opt_i64 vm_pressure_level;
    opt_i64 memorystatus_level_permille;
    darwin_compressor compressor;
    opt_i64 swapins_total;
    opt_i64 swapouts_total;
};

// psi_line is one PSI line (some or full) of one resource.
struct psi_line {
    opt_i64 avg10_permille;
    opt_i64 avg60_permille;
    opt_i64 total_us;
};

// psi_resource is the some and full lines of one resource.
struct psi_resource {
    psi_line some;
    psi_line full;
};

// linux_psi is pressure stall information for cpu, memory and io.
struct linux_psi {
    psi_resource cpu;
    psi_resource memory;
    psi_resource io;
};

// linux_memory_events is the memory.events totals.
struct linux_memory_events {
    opt_i64 high;
    opt_i64 max;
    opt_i64 oom;
    opt_i64 oom_kill;
};

// linux_signals is the Linux platform block; on a darwin page it is a vm
// guest's section.
struct linux_signals {
    linux_psi psi;
    linux_memory_events memory_events;
};

// signal_record is one signal slot: the value and the five rungs, each with
// its release value.
struct signal_record {
    signal_id id = signal_id::unset;
    sdk::unit unit = sdk::unit::unset;
    opt_i64 value;
    opt_i64 warn_threshold;
    opt_i64 admit_stop_threshold;
    opt_i64 critical_threshold;
    opt_i64 evict_soft_threshold;
    opt_i64 evict_hard_threshold;
    opt_i64 warn_release;
    opt_i64 admit_stop_release;
    opt_i64 critical_release;
    opt_i64 evict_soft_release;
    opt_i64 evict_hard_release;
    opt_i64 grace_seconds;
    opt_i64 min_reclaim_bytes;
    opt_i64 window_seconds;
    opt_i64 eta_to_next_rung_seconds;
    sdk::trend trend = sdk::trend::unknown;
    sdk::limiting_resource limiting_resource = sdk::limiting_resource::unknown;
    signal_id shared_with = signal_id::unset;
};

// kill_mode_state is the published tier-1 kill mode.
struct kill_mode_state {
    kill_mode mode = kill_mode::unset;
    bool armed = false;
    std::int64_t arming_episodes = 0;
    kill_mode_reason reason = kill_mode_reason::unknown;
};

// recommendation is how much more the node can safely take.
struct recommendation {
    opt_i64 safe_additional_bytes;
    signal_id basis = signal_id::unset;
};

// band_counters is one band's monotonic running totals.
struct band_counters {
    sdk::band band = sdk::band::unset;
    std::int64_t evictions_total = 0;
    std::int64_t oom_kills_total = 0;
    std::int64_t kernel_kills_total = 0;
    std::int64_t restarts_total = 0;
    std::int64_t mem_high_events_total = 0;
    std::int64_t tier1_refused_total = 0;
};

// snapshot is one stable read of the page.
struct snapshot {
    // The header as read: abi_minor may be above page_abi_minor (the fields
    // of the newer minor are not read), body_size above page_body_size.
    std::uint16_t abi_major = 0;
    std::uint16_t abi_minor = 0;
    std::uint32_t header_size = 0;
    std::uint32_t body_size = 0;
    // seq is the even seqlock value the copy was taken under.
    std::uint64_t seq = 0;
    // notify_epoch is the notification word, read after the copy. It is
    // outside the seqlock: it can be newer than the copy, never older.
    std::uint64_t notify_epoch = 0;

    std::int64_t generation = 0;
    std::int64_t writer_period_seconds = 0;
    // observed_at is when the producer took the observation, in Unix seconds.
    std::int64_t observed_at = 0;
    sdk::platform platform = sdk::platform::unset;
    std::uint64_t node_name_len = 0;
    // node_name_raw is the inline nodeName field; node_name() is its first
    // node_name_len bytes.
    std::array<char, node_name_bytes> node_name_raw{};

    sdk::capacity capacity;
    sdk::level level = sdk::level::unset;

    // darwin is present on a darwin page.
    std::optional<darwin_signals> darwin;
    // linux_ is present on a linux page, and on a darwin page that carries a
    // vm guest's section (any linux value present).
    std::optional<linux_signals> linux_;

    // signals holds signal_count records, the page's filled slots in order.
    std::array<signal_record, max_signals> signals{};
    std::size_t signal_count = 0;

    bool admit = false;
    signal_id admit_reason_signal = signal_id::unset;
    // evict_deadline is when the evict-soft grace runs out, in Unix seconds.
    opt_i64 evict_deadline;
    kill_mode_state kill_mode;
    sdk::recommendation recommendation;

    // counters holds counter_count records, the page's filled slots in order.
    std::array<band_counters, max_counters> counters{};
    std::size_t counter_count = 0;

    // node_name returns the node's name.
    constexpr std::string_view node_name() const noexcept {
        return {node_name_raw.data(), static_cast<std::size_t>(node_name_len)};
    }

    // find_signal returns the record of id, or nullptr when the page holds none.
    constexpr const signal_record* find_signal(signal_id id) const noexcept {
        for (std::size_t i = 0; i < signal_count; ++i) {
            if (signals[i].id == id) {
                return &signals[i];
            }
        }
        return nullptr;
    }
};

}  // namespace k3sm::sdk

#endif  // K3SM_SNAPSHOT_HPP
