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

#ifndef K3SM_PAGE_LAYOUT_HPP
#define K3SM_PAGE_LAYOUT_HPP

// The node budget page ABI, major 1: the constants and field offsets of the
// fixed 4096-byte little-endian page, as the k3sm apis repository publishes
// them (budget/v1alpha1/testdata/page: manifest.json, layout.v1.0.txt and
// layout.v1.1.txt). The tests check every value here against those vendored
// files.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace k3sm::sdk {

// page_abi_major is the one page ABI major this SDK reads. A page of any
// other major is refused (error::abi_mismatch); a new major is a new file
// name, so an SDK of an older major finds no page rather than misreading it.
inline constexpr std::uint16_t page_abi_major = 1;

// page_abi_minor is the newest page ABI minor this SDK knows. A page of a
// higher minor of the same major is read up to this minor's body size; a
// page of an older minor is read up to its own, and the fields a later
// minor appends read absent. Minor 1 appends the CPU core split and the GPU
// presence and count to the capacity, after the counters.
inline constexpr std::uint16_t page_abi_minor = 1;

// page_file_name is the page's file name, budget.v<page_abi_major>.page.
inline constexpr char page_file_name[] = "budget.v1.page";

// darwin_page_path is where a native Darwin pod finds the page.
inline constexpr char darwin_page_path[] = "/var/lib/k3sm/budget/budget.v1.page";

// linux_page_path is where a Linux reader (a vm guest, or a pod on a Linux
// producer's node) finds the page.
inline constexpr char linux_page_path[] = "/run/k3sm/budget/budget.v1.page";

// default_page_path returns the page path of the platform this translation
// unit is compiled for: darwin_page_path on Apple platforms, linux_page_path
// elsewhere. It is resolved at compile time and never reads the
// environment; an application that finds its page elsewhere passes that
// path to budget_page::open itself.
constexpr const char* default_page_path() noexcept {
#if defined(__APPLE__)
    return darwin_page_path;
#else
    return linux_page_path;
#endif
}

// page_size is the page file's fixed size in bytes.
inline constexpr std::size_t page_size = 4096;

// page_magic is the eight bytes at offset 0.
inline constexpr std::string_view page_magic = "K3SMBDGT";

// page_header_size is the offset of the first body field, and the only
// headerSize this major carries.
inline constexpr std::uint32_t page_header_size = 192;

// page_body_size is the body size of page_abi_minor: the bytes from
// page_header_size to the end of its last field.
inline constexpr std::uint32_t page_body_size = 2152;

// page_body_size_minor0 is the body size of minor 0, the smallest body a
// page of this major carries.
inline constexpr std::uint32_t page_body_size_minor0 = 2120;

// page_body_size_of returns the body size of a page of the given minor: a
// minor this SDK knows has its own size, and a newer one is read up to
// page_body_size.
constexpr std::uint32_t page_body_size_of(std::uint16_t minor) noexcept {
    return minor == 0 ? page_body_size_minor0 : page_body_size;
}

// page_absent is the value an optional i64 field holds when the producer
// could not sample it. 0 is never absent.
inline constexpr std::int64_t page_absent = std::numeric_limits<std::int64_t>::min();

// max_node_name_bytes is the longest node name a page carries.
inline constexpr std::uint64_t max_node_name_bytes = 253;

// node_name_bytes is the size of the inline nodeName field.
inline constexpr std::size_t node_name_bytes = 256;

// max_signals and max_counters are the slot counts of the signal and
// per-band counter arrays.
inline constexpr std::size_t max_signals = 8;
inline constexpr std::size_t max_counters = 3;

// stale_factor: a page is stale when now - observedAt exceeds this many
// writer periods. observedAt is in whole seconds, so the verdict may be up
// to one second early or late.
inline constexpr std::int64_t stale_factor = 3;

// reader_retries is how many seqlock read attempts read() makes before it
// returns error::torn.
inline constexpr int reader_retries = 64;

// page_offset holds the byte offset of every field of the page.
namespace page_offset {

// The header line, the seqlock word and the notification word.
inline constexpr std::size_t magic = 0;
inline constexpr std::size_t abi_major = 8;     // u16
inline constexpr std::size_t abi_minor = 10;    // u16
inline constexpr std::size_t header_size = 12;  // u32
inline constexpr std::size_t body_size = 16;    // u32
inline constexpr std::size_t seq = 64;
inline constexpr std::size_t notify_epoch = 128;

// Identity.
inline constexpr std::size_t generation = 192;
inline constexpr std::size_t writer_period_seconds = 200;
inline constexpr std::size_t observed_at = 208;
inline constexpr std::size_t platform = 216;
inline constexpr std::size_t node_name_len = 224;
inline constexpr std::size_t node_name = 232;

// Capacity.
inline constexpr std::size_t cpu_capacity_milli = 488;
inline constexpr std::size_t cpu_system_reserve_milli = 496;
inline constexpr std::size_t mem_capacity_bytes = 504;
inline constexpr std::size_t mem_system_reserve_bytes = 512;
inline constexpr std::size_t mem_eviction_reserve_bytes = 520;
inline constexpr std::size_t mem_allocatable_bytes = 528;
inline constexpr std::size_t swap_total_bytes = 536;
inline constexpr std::size_t swap_used_bytes = 544;

// Pressure.
inline constexpr std::size_t level = 552;

// The Darwin block.
inline constexpr std::size_t darwin_vm_pressure_level = 560;
inline constexpr std::size_t darwin_memorystatus_level_permille = 568;
inline constexpr std::size_t darwin_compressor_bytes_used = 576;
inline constexpr std::size_t darwin_compressor_bytes_stored = 584;
inline constexpr std::size_t darwin_compressor_segment_limit_bytes = 592;
inline constexpr std::size_t darwin_swapins_total = 600;
inline constexpr std::size_t darwin_swapouts_total = 608;

// The Linux block: PSI as resource (cpu, memory, io) x line (some, full) x
// value (avg10Permille, avg60Permille, totalUs), then memory.events.
inline constexpr std::size_t linux_psi = 616;
inline constexpr std::size_t linux_psi_resource_stride = 48;
inline constexpr std::size_t linux_psi_line_stride = 24;
inline constexpr std::size_t linux_psi_avg10_permille = 0;
inline constexpr std::size_t linux_psi_avg60_permille = 8;
inline constexpr std::size_t linux_psi_total_us = 16;
inline constexpr std::size_t linux_memory_events_high = 760;
inline constexpr std::size_t linux_memory_events_max = 768;
inline constexpr std::size_t linux_memory_events_oom = 776;
inline constexpr std::size_t linux_memory_events_oom_kill = 784;

// linux_psi_field returns the offset of one PSI value: resource 0 cpu, 1
// memory, 2 io; line 0 some, 1 full; value one of the linux_psi_* offsets.
constexpr std::size_t linux_psi_field(std::size_t resource, std::size_t line,
                                      std::size_t value) noexcept {
    return linux_psi + resource * linux_psi_resource_stride + line * linux_psi_line_stride + value;
}

// The signal slots: max_signals records of signal_stride bytes.
inline constexpr std::size_t signals = 792;
inline constexpr std::size_t signal_stride = 160;
namespace signal_field {
inline constexpr std::size_t id = 0;
inline constexpr std::size_t unit = 8;
inline constexpr std::size_t value = 16;
inline constexpr std::size_t warn_threshold = 24;
inline constexpr std::size_t admit_stop_threshold = 32;
inline constexpr std::size_t critical_threshold = 40;
inline constexpr std::size_t evict_soft_threshold = 48;
inline constexpr std::size_t evict_hard_threshold = 56;
inline constexpr std::size_t warn_release = 64;
inline constexpr std::size_t admit_stop_release = 72;
inline constexpr std::size_t critical_release = 80;
inline constexpr std::size_t evict_soft_release = 88;
inline constexpr std::size_t evict_hard_release = 96;
inline constexpr std::size_t grace_seconds = 104;
inline constexpr std::size_t min_reclaim_bytes = 112;
inline constexpr std::size_t window_seconds = 120;
inline constexpr std::size_t eta_to_next_rung_seconds = 128;
inline constexpr std::size_t trend = 136;
inline constexpr std::size_t limiting_resource = 144;
inline constexpr std::size_t shared_with = 152;
}  // namespace signal_field

// signal_slot returns the offset of signal slot i.
constexpr std::size_t signal_slot(std::size_t i) noexcept { return signals + i * signal_stride; }

// Action, kill mode and recommendation.
inline constexpr std::size_t admit = 2072;
inline constexpr std::size_t admit_reason_signal = 2080;
inline constexpr std::size_t evict_deadline = 2088;
inline constexpr std::size_t kill_mode_mode = 2096;
inline constexpr std::size_t kill_mode_armed = 2104;
inline constexpr std::size_t kill_mode_arming_episodes = 2112;
inline constexpr std::size_t kill_mode_reason = 2120;
inline constexpr std::size_t recommendation_safe_additional_bytes = 2128;
inline constexpr std::size_t recommendation_basis = 2136;

// The per-band counter slots: max_counters records of counter_stride bytes.
inline constexpr std::size_t counters = 2144;
inline constexpr std::size_t counter_stride = 56;
namespace counter_field {
inline constexpr std::size_t band = 0;
inline constexpr std::size_t evictions_total = 8;
inline constexpr std::size_t oom_kills_total = 16;
inline constexpr std::size_t kernel_kills_total = 24;
inline constexpr std::size_t restarts_total = 32;
inline constexpr std::size_t mem_high_events_total = 40;
inline constexpr std::size_t tier1_refused_total = 48;
}  // namespace counter_field

// counter_slot returns the offset of counter slot i.
constexpr std::size_t counter_slot(std::size_t i) noexcept { return counters + i * counter_stride; }

// Minor 1: the capacity's CPU core split and GPU presence and count,
// appended after the counters. gpu_present is an i64 holding 0, 1 or
// page_absent.
inline constexpr std::size_t cpu_performance_cores = 2312;
inline constexpr std::size_t cpu_efficiency_cores = 2320;
inline constexpr std::size_t gpu_present = 2328;
inline constexpr std::size_t gpu_count = 2336;

}  // namespace page_offset

// The layout's own invariants. The tests check each offset against the
// vendored layout descriptor and the pattern fixture's bytes.
static_assert(page_offset::seq % 64 == 0 && page_offset::notify_epoch % 64 == 0,
              "seq and notifyEpoch each sit on their own 64-byte line");
static_assert(page_offset::notify_epoch < page_header_size, "notifyEpoch is outside the body");
static_assert(page_offset::generation == page_header_size, "the body starts at headerSize");
static_assert(page_offset::node_name + node_name_bytes == page_offset::cpu_capacity_milli);
static_assert(page_offset::linux_psi_field(2, 1, page_offset::linux_psi_total_us) + 8 ==
              page_offset::linux_memory_events_high);
static_assert(page_offset::signal_slot(max_signals) == page_offset::admit);
static_assert(page_offset::signal_field::shared_with + 8 == page_offset::signal_stride);
static_assert(page_offset::counter_field::tier1_refused_total + 8 == page_offset::counter_stride);
static_assert(page_offset::counter_slot(max_counters) == page_header_size + page_body_size_minor0,
              "the last counter slot ends the minor 0 body");
static_assert(page_offset::cpu_performance_cores == page_header_size + page_body_size_minor0,
              "minor 1 starts where minor 0 ends");
static_assert(page_offset::cpu_efficiency_cores == page_offset::cpu_performance_cores + 8 &&
              page_offset::gpu_present == page_offset::cpu_efficiency_cores + 8 &&
              page_offset::gpu_count == page_offset::gpu_present + 8);
static_assert(page_offset::gpu_count + 8 == page_header_size + page_body_size,
              "gpuCount ends the minor 1 body");
static_assert(page_header_size + page_body_size <= 3072, "a quarter of the page stays free");

}  // namespace k3sm::sdk

#endif  // K3SM_PAGE_LAYOUT_HPP
