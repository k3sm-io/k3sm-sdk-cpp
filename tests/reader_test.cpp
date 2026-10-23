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

// The v1 page reader against the vendored apis fixtures: the manifest's
// constants and code tables, the layout descriptor, the pattern fixture's
// bytes, every golden page decoded to the values its source sample holds,
// every negative page to its manifest error, and the minor, code, staleness
// and helper rules.

#include <k3sm/budget.hpp>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include <unistd.h>

#include "test_util.hpp"

#ifndef K3SM_SDK_TEST_TMPDIR
#error "K3SM_SDK_TEST_TMPDIR must name a writable directory"
#endif

namespace sdk = k3sm::sdk;
using k3sm_test::fixture;
using k3sm_test::read_file;

namespace reader_test {

constexpr std::int64_t mib = std::int64_t{1} << 20;
constexpr std::int64_t gib = std::int64_t{1} << 30;
// observed is the instant every golden is stamped with, 2026-10-19T12:00:00Z.
constexpr std::int64_t observed = 1792411200;
constexpr std::optional<std::int64_t> none = std::nullopt;

static_assert(!std::is_copy_constructible_v<sdk::budget_page>, "budget_page is move-only");
static_assert(std::is_nothrow_move_constructible_v<sdk::budget_page>);
#if defined(__APPLE__)
static_assert(std::string_view(sdk::default_page_path()) == sdk::darwin_page_path);
#else
static_assert(std::string_view(sdk::default_page_path()) == sdk::linux_page_path);
#endif

// open_read opens path and reads it with the staleness clock at now.
sdk::result<sdk::snapshot> open_read(const std::string& path, std::int64_t now = observed) {
    auto page = sdk::budget_page::open(path.c_str());
    if (!page) {
        return page.error();
    }
    return page->read_at(now);
}

// write_temp writes bytes to a fresh file under the test directory and
// returns its path.
std::string write_temp(const std::string& bytes) {
    std::string path = std::string(K3SM_SDK_TEST_TMPDIR) + "/page-XXXXXX";
    const int fd = ::mkstemp(path.data());
    if (fd < 0 || ::write(fd, bytes.data(), bytes.size()) != static_cast<ssize_t>(bytes.size())) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        std::exit(2);
    }
    ::close(fd);
    return path;
}

// read_bytes writes bytes to a temporary page file, opens it, removes the
// file (the mapping keeps it) and reads it with the staleness clock at now.
sdk::result<sdk::snapshot> read_bytes(const std::string& bytes, std::int64_t now = observed) {
    const std::string path = write_temp(bytes);
    auto page = sdk::budget_page::open(path.c_str());
    ::unlink(path.c_str());
    if (!page) {
        return page.error();
    }
    return page->read_at(now);
}

void put_u64(std::string& page, std::size_t offset, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        page[offset + i] = static_cast<char>(v >> (8 * i));
    }
}

std::uint64_t get_u64(const std::string& page, std::size_t offset) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= std::uint64_t{static_cast<unsigned char>(page[offset + i])} << (8 * i);
    }
    return v;
}

void test_manifest_constants() {
    const std::string m = read_file(fixture("manifest.json"));
    K3SM_CHECK(!m.empty());
    auto num = [&](const char* key) { return std::stoll(k3sm_test::json_value(m, key)); };
    K3SM_CHECK_EQ(num("pageABIMajor"), sdk::page_abi_major);
    K3SM_CHECK_EQ(num("pageABIMinor"), sdk::page_abi_minor);
    K3SM_CHECK_EQ(k3sm_test::json_value(m, "pageFileName"), std::string(sdk::page_file_name));
    K3SM_CHECK_EQ(k3sm_test::json_value(m, "defaultPagePath"), std::string(sdk::darwin_page_path));
    K3SM_CHECK_EQ(k3sm_test::json_value(m, "linuxPagePath"), std::string(sdk::linux_page_path));
    K3SM_CHECK_EQ(num("pageSize"), static_cast<long long>(sdk::page_size));
    K3SM_CHECK_EQ(num("headerSize"), sdk::page_header_size);
    K3SM_CHECK_EQ(num("bodySize"), sdk::page_body_size);
    K3SM_CHECK_EQ(k3sm_test::json_value(m, "magic"), std::string(sdk::page_magic));
    K3SM_CHECK_EQ(num("absent"), sdk::page_absent);
    K3SM_CHECK_EQ(num("seqOffset"), static_cast<long long>(sdk::page_offset::seq));
    K3SM_CHECK_EQ(num("notifyEpochOffset"), static_cast<long long>(sdk::page_offset::notify_epoch));
    K3SM_CHECK_EQ(num("maxNodeNameBytes"), static_cast<long long>(sdk::max_node_name_bytes));
    K3SM_CHECK_EQ(num("staleFactor"), sdk::stale_factor);
    K3SM_CHECK_EQ(k3sm_test::json_value(m, "observedAtResolution"), std::string("1s"));
    K3SM_CHECK_EQ(num("readerRetries"), sdk::reader_retries);
    K3SM_CHECK_EQ(k3sm_test::json_value(m, "layout"), std::string("layout.v1.0.txt"));
}

// The code tables: each enum's values in code order, by manifest name.
void test_code_tables() {
    const std::string m = read_file(fixture("manifest.json"));
    const std::size_t codes = m.find("\"codes\":");
    K3SM_CHECK(codes != std::string::npos);
    const std::map<std::string, std::vector<std::pair<std::uint64_t, std::string>>> want = {
        {"signal",
         {{std::uint64_t(sdk::signal_id::memory_available), "memory.available"},
          {std::uint64_t(sdk::signal_id::memory_compressor), "memory.compressor"},
          {std::uint64_t(sdk::signal_id::memory_swap), "memory.swap"},
          {std::uint64_t(sdk::signal_id::swapfs_available), "swapfs.available"},
          {std::uint64_t(sdk::signal_id::nodefs_available), "nodefs.available"},
          {std::uint64_t(sdk::signal_id::imagefs_available), "imagefs.available"},
          {std::uint64_t(sdk::signal_id::pid_available), "pid.available"}}},
        {"unit",
         {{std::uint64_t(sdk::unit::bytes), "bytes"},
          {std::uint64_t(sdk::unit::permille), "permille"},
          {std::uint64_t(sdk::unit::count), "count"}}},
        {"level",
         {{std::uint64_t(sdk::level::ok), "ok"},
          {std::uint64_t(sdk::level::warn), "warn"},
          {std::uint64_t(sdk::level::critical), "critical"},
          {std::uint64_t(sdk::level::evicting), "evicting"}}},
        {"band",
         {{std::uint64_t(sdk::band::critical), "critical"},
          {std::uint64_t(sdk::band::standard), "standard"},
          {std::uint64_t(sdk::band::batch), "batch"}}},
        {"platform",
         {{std::uint64_t(sdk::platform::darwin), "darwin"},
          {std::uint64_t(sdk::platform::linux_), "linux"}}},
        {"killMode",
         {{std::uint64_t(sdk::kill_mode::auto_), "auto"},
          {std::uint64_t(sdk::kill_mode::on), "on"},
          {std::uint64_t(sdk::kill_mode::off), "off"}}},
        {"trend",
         {{std::uint64_t(sdk::trend::falling), "falling"},
          {std::uint64_t(sdk::trend::steady), "steady"},
          {std::uint64_t(sdk::trend::rising), "rising"}}},
        {"limitingResource",
         {{std::uint64_t(sdk::limiting_resource::memory), "memory"},
          {std::uint64_t(sdk::limiting_resource::swapfs), "swapfs"},
          {std::uint64_t(sdk::limiting_resource::nodefs), "nodefs"},
          {std::uint64_t(sdk::limiting_resource::imagefs), "imagefs"},
          {std::uint64_t(sdk::limiting_resource::pid), "pid"},
          {std::uint64_t(sdk::limiting_resource::cpu), "cpu"}}},
        {"killModeReason",
         {{std::uint64_t(sdk::kill_mode_reason::cold_start), "cold-start"},
          {std::uint64_t(sdk::kill_mode_reason::observing), "observing"},
          {std::uint64_t(sdk::kill_mode_reason::armed), "armed"},
          {std::uint64_t(sdk::kill_mode_reason::policy_off), "policy-off"},
          {std::uint64_t(sdk::kill_mode_reason::disarm_file), "disarm-file"}}},
    };
    const std::map<std::string, std::uint64_t> counts = {
        {"signal", sdk::detail::signal_codes},
        {"unit", sdk::detail::unit_codes},
        {"level", sdk::detail::level_codes},
        {"band", sdk::detail::band_codes},
        {"platform", sdk::detail::platform_codes},
        {"killMode", sdk::detail::kill_mode_codes},
        {"trend", sdk::detail::trend_codes},
        {"limitingResource", sdk::detail::limiting_resource_codes},
        {"killModeReason", sdk::detail::kill_mode_reason_codes},
    };
    std::size_t tables = 0;
    for (std::size_t at = m.find('{', codes); at < m.find("\"fixtures\":"); ++at) {
        tables += m.compare(at, 4, "\": [") == 0 ? 1 : 0;
    }
    K3SM_CHECK_EQ(tables, want.size());
    for (const auto& [name, values] : want) {
        const std::vector<std::string> got = k3sm_test::json_strings(m, name, codes);
        K3SM_CHECK_EQ(got.size(), values.size());
        K3SM_CHECK_EQ(counts.at(name), got.size());
        for (std::size_t i = 0; i < values.size() && i < got.size(); ++i) {
            if (values[i].first != i + 1 || values[i].second != got[i]) {
                k3sm_test::fail(__FILE__, __LINE__, "code table " + name + " entry " + got[i]);
            }
        }
    }
}

// layout_offsets maps every layout descriptor name to this SDK's offset.
std::map<std::string, std::size_t> layout_offsets() {
    namespace o = sdk::page_offset;
    std::map<std::string, std::size_t> m = {
        {"magic", o::magic},
        {"abiMajor", o::abi_major},
        {"abiMinor", o::abi_minor},
        {"headerSize", o::header_size},
        {"bodySize", o::body_size},
        {"seq", o::seq},
        {"notifyEpoch", o::notify_epoch},
        {"generation", o::generation},
        {"writerPeriodSeconds", o::writer_period_seconds},
        {"observedAt", o::observed_at},
        {"platform", o::platform},
        {"nodeNameLen", o::node_name_len},
        {"nodeName", o::node_name},
        {"capacity.cpuCapacityMilli", o::cpu_capacity_milli},
        {"capacity.cpuSystemReserveMilli", o::cpu_system_reserve_milli},
        {"capacity.memCapacityBytes", o::mem_capacity_bytes},
        {"capacity.memSystemReserveBytes", o::mem_system_reserve_bytes},
        {"capacity.memEvictionReserveBytes", o::mem_eviction_reserve_bytes},
        {"capacity.memAllocatableBytes", o::mem_allocatable_bytes},
        {"capacity.swapTotalBytes", o::swap_total_bytes},
        {"capacity.swapUsedBytes", o::swap_used_bytes},
        {"level", o::level},
        {"darwin.vmPressureLevel", o::darwin_vm_pressure_level},
        {"darwin.memorystatusLevelPermille", o::darwin_memorystatus_level_permille},
        {"darwin.compressor.bytesUsed", o::darwin_compressor_bytes_used},
        {"darwin.compressor.bytesStored", o::darwin_compressor_bytes_stored},
        {"darwin.compressor.segmentLimitBytes", o::darwin_compressor_segment_limit_bytes},
        {"darwin.swapinsTotal", o::darwin_swapins_total},
        {"darwin.swapoutsTotal", o::darwin_swapouts_total},
        {"linux.memoryEvents.high", o::linux_memory_events_high},
        {"linux.memoryEvents.max", o::linux_memory_events_max},
        {"linux.memoryEvents.oom", o::linux_memory_events_oom},
        {"linux.memoryEvents.oomKill", o::linux_memory_events_oom_kill},
        {"admit", o::admit},
        {"admitReasonSignal", o::admit_reason_signal},
        {"evictDeadline", o::evict_deadline},
        {"killMode.mode", o::kill_mode_mode},
        {"killMode.armed", o::kill_mode_armed},
        {"killMode.armingEpisodes", o::kill_mode_arming_episodes},
        {"killMode.reason", o::kill_mode_reason},
        {"recommendation.safeAdditionalBytes", o::recommendation_safe_additional_bytes},
        {"recommendation.basis", o::recommendation_basis},
    };
    const char* resources[] = {"cpu", "memory", "io"};
    const char* lines[] = {"some", "full"};
    const std::pair<const char*, std::size_t> values[] = {
        {"avg10Permille", o::linux_psi_avg10_permille},
        {"avg60Permille", o::linux_psi_avg60_permille},
        {"totalUs", o::linux_psi_total_us},
    };
    for (std::size_t r = 0; r < 3; ++r) {
        for (std::size_t l = 0; l < 2; ++l) {
            for (const auto& [name, v] : values) {
                m[std::string("linux.psi.") + resources[r] + "." + lines[l] + "." + name] =
                    o::linux_psi_field(r, l, v);
            }
        }
    }
    namespace sf = o::signal_field;
    const std::pair<const char*, std::size_t> signal_fields[] = {
        {"id", sf::id},
        {"unit", sf::unit},
        {"value", sf::value},
        {"warnThreshold", sf::warn_threshold},
        {"admitStopThreshold", sf::admit_stop_threshold},
        {"criticalThreshold", sf::critical_threshold},
        {"evictSoftThreshold", sf::evict_soft_threshold},
        {"evictHardThreshold", sf::evict_hard_threshold},
        {"warnRelease", sf::warn_release},
        {"admitStopRelease", sf::admit_stop_release},
        {"criticalRelease", sf::critical_release},
        {"evictSoftRelease", sf::evict_soft_release},
        {"evictHardRelease", sf::evict_hard_release},
        {"graceSeconds", sf::grace_seconds},
        {"minReclaimBytes", sf::min_reclaim_bytes},
        {"windowSeconds", sf::window_seconds},
        {"etaToNextRungSeconds", sf::eta_to_next_rung_seconds},
        {"trend", sf::trend},
        {"limitingResource", sf::limiting_resource},
        {"sharedWith", sf::shared_with},
    };
    for (std::size_t i = 0; i < sdk::max_signals; ++i) {
        for (const auto& [name, f] : signal_fields) {
            m["signals[" + std::to_string(i) + "]." + name] = o::signal_slot(i) + f;
        }
    }
    namespace cf = o::counter_field;
    const std::pair<const char*, std::size_t> counter_fields[] = {
        {"band", cf::band},
        {"evictionsTotal", cf::evictions_total},
        {"oomKillsTotal", cf::oom_kills_total},
        {"kernelKillsTotal", cf::kernel_kills_total},
        {"restartsTotal", cf::restarts_total},
        {"memHighEventsTotal", cf::mem_high_events_total},
        {"tier1RefusedTotal", cf::tier1_refused_total},
    };
    for (std::size_t i = 0; i < sdk::max_counters; ++i) {
        for (const auto& [name, f] : counter_fields) {
            m["counters[" + std::to_string(i) + "]." + name] = o::counter_slot(i) + f;
        }
    }
    return m;
}

struct layout_line {
    std::size_t offset;
    std::size_t size;
    std::string kind;
    std::string name;
};

std::vector<layout_line> read_layout() {
    std::vector<layout_line> out;
    std::istringstream in(read_file(fixture("layout.v1.0.txt")));
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream f(line);
        layout_line l;
        std::string absent, enumname;
        f >> l.offset >> l.size >> l.kind >> absent >> enumname >> l.name;
        out.push_back(l);
    }
    return out;
}

// Every descriptor line names a field this SDK places at the same offset,
// and this SDK places no field the descriptor lacks.
void test_layout_descriptor() {
    const auto offsets = layout_offsets();
    const auto layout = read_layout();
    K3SM_CHECK_EQ(layout.size(), offsets.size());
    std::size_t end = 0;
    for (const auto& l : layout) {
        const auto it = offsets.find(l.name);
        if (it == offsets.end() || it->second != l.offset) {
            k3sm_test::fail(__FILE__, __LINE__, "layout field " + l.name + " at " + std::to_string(l.offset));
        }
        if (l.offset >= sdk::page_header_size) {
            end = std::max(end, l.offset + l.size);
        }
        if (l.name == "nodeName") {
            K3SM_CHECK_EQ(l.size, sdk::node_name_bytes);
        }
    }
    K3SM_CHECK_EQ(end, std::size_t{sdk::page_header_size} + sdk::page_body_size);
}

// The pattern fixture holds 0x50415454<<32 | offset in every u64 and i64
// word, the low offset byte in each nodeName byte and zero elsewhere: every
// offset here must land on its word, and no word may lie outside a field.
void test_pattern_fixture() {
    const std::string page = read_file(fixture("pattern.v1.page"));
    K3SM_CHECK_EQ(page.size(), sdk::page_size);
    std::vector<bool> covered(sdk::page_size, false);
    for (const auto& l : read_layout()) {
        for (std::size_t i = 0; i < l.size; ++i) {
            covered[l.offset + i] = true;
        }
        if (l.kind == "u64" || l.kind == "i64") {
            const std::uint64_t want = (std::uint64_t{0x50415454} << 32) | l.offset;
            if (get_u64(page, l.offset) != want) {
                k3sm_test::fail(__FILE__, __LINE__, "pattern word of " + l.name);
            }
        }
    }
    for (std::size_t i = 0; i < sdk::node_name_bytes; ++i) {
        const std::size_t at = sdk::page_offset::node_name + i;
        K3SM_CHECK_EQ(static_cast<unsigned char>(page[at]), static_cast<unsigned char>(at & 0xff));
    }
    for (std::size_t i = 0; i < page.size(); ++i) {
        if (!covered[i] && page[i] != 0) {
            k3sm_test::fail(__FILE__, __LINE__, "non-zero byte outside every field at " + std::to_string(i));
        }
    }
    // The pattern's nodeNameLen word is far above 253: a reader refuses it.
    const auto r = open_read(fixture("pattern.v1.page"));
    K3SM_CHECK(!r && r.error() == sdk::error::corrupt);
}

void check_capacity_darwin(const sdk::capacity& c) {
    K3SM_CHECK_EQ(c.cpu_capacity_milli, 8000);
    K3SM_CHECK_EQ(c.cpu_system_reserve_milli, 500);
    K3SM_CHECK_EQ(c.mem_capacity_bytes, 16 * gib);
    K3SM_CHECK_EQ(c.mem_system_reserve_bytes, 2 * gib);
    K3SM_CHECK_EQ(c.mem_eviction_reserve_bytes, 512 * mib);
    K3SM_CHECK_EQ(c.mem_allocatable_bytes, 13 * gib + 512 * mib);
    K3SM_CHECK_EQ(c.swap_total_bytes, 4 * gib);
    K3SM_CHECK_EQ(c.swap_used_bytes, 1 * gib);
}

void check_darwin_block(const sdk::darwin_signals& d) {
    K3SM_CHECK_EQ(d.vm_pressure_level, 1);
    K3SM_CHECK_EQ(d.memorystatus_level_permille, 640);
    K3SM_CHECK_EQ(d.compressor.bytes_used, 1 * gib);
    K3SM_CHECK_EQ(d.compressor.bytes_stored, 3 * gib);
    K3SM_CHECK_EQ(d.compressor.segment_limit_bytes, 8 * gib);
    K3SM_CHECK_EQ(d.swapins_total, 1200);
    K3SM_CHECK_EQ(d.swapouts_total, 3400);
}

void check_psi_line(const sdk::psi_line& l, std::int64_t a10, std::int64_t a60, std::int64_t total) {
    K3SM_CHECK_EQ(l.avg10_permille, a10);
    K3SM_CHECK_EQ(l.avg60_permille, a60);
    K3SM_CHECK_EQ(l.total_us, total);
}

void check_linux_block(const sdk::linux_signals& l) {
    check_psi_line(l.psi.cpu.some, 120, 80, 5'000'000);
    check_psi_line(l.psi.cpu.full, 0, 0, 0);
    check_psi_line(l.psi.memory.some, 610, 420, 9'000'000);
    check_psi_line(l.psi.memory.full, 300, 200, 4'000'000);
    check_psi_line(l.psi.io.some, 10, 5, 700'000);
    check_psi_line(l.psi.io.full, 4, 2, 300'000);
    K3SM_CHECK_EQ(l.memory_events.high, 11);
    K3SM_CHECK_EQ(l.memory_events.max, 2);
    K3SM_CHECK_EQ(l.memory_events.oom, 1);
    K3SM_CHECK_EQ(l.memory_events.oom_kill, 1);
}

// rungs is a signal's five thresholds then five releases, in rung order.
struct rungs {
    std::optional<std::int64_t> t[5];
    std::optional<std::int64_t> r[5];
};

void check_signal(const sdk::signal_record& g, sdk::signal_id id, sdk::unit u,
                  std::optional<std::int64_t> value, const rungs& want,
                  std::optional<std::int64_t> grace, std::optional<std::int64_t> min_reclaim,
                  std::optional<std::int64_t> window, std::optional<std::int64_t> eta, sdk::trend tr,
                  sdk::limiting_resource lr, sdk::signal_id shared = sdk::signal_id::unset) {
    K3SM_CHECK(g.id == id);
    K3SM_CHECK(g.unit == u);
    K3SM_CHECK_EQ(g.value, value);
    const std::optional<std::int64_t> got_t[5] = {g.warn_threshold, g.admit_stop_threshold, g.critical_threshold,
                                                  g.evict_soft_threshold, g.evict_hard_threshold};
    const std::optional<std::int64_t> got_r[5] = {g.warn_release, g.admit_stop_release, g.critical_release,
                                                  g.evict_soft_release, g.evict_hard_release};
    for (int i = 0; i < 5; ++i) {
        if (got_t[i] != want.t[i] || got_r[i] != want.r[i]) {
            k3sm_test::fail(__FILE__, __LINE__,
                            "signal " + std::to_string(std::uint64_t(id)) + " rung " + std::to_string(i));
        }
    }
    K3SM_CHECK_EQ(g.grace_seconds, grace);
    K3SM_CHECK_EQ(g.min_reclaim_bytes, min_reclaim);
    K3SM_CHECK_EQ(g.window_seconds, window);
    K3SM_CHECK_EQ(g.eta_to_next_rung_seconds, eta);
    K3SM_CHECK(g.trend == tr);
    K3SM_CHECK(g.limiting_resource == lr);
    K3SM_CHECK(g.shared_with == shared);
}

void check_darwin_common(const sdk::snapshot& s, std::uint16_t minor = 0, std::uint32_t body = 2120) {
    K3SM_CHECK_EQ(s.abi_major, 1);
    K3SM_CHECK_EQ(s.abi_minor, minor);
    K3SM_CHECK_EQ(s.header_size, 192u);
    K3SM_CHECK_EQ(s.body_size, body);
    K3SM_CHECK_EQ(s.seq, 0u);
    K3SM_CHECK_EQ(s.notify_epoch, 0u);
    K3SM_CHECK_EQ(s.generation, 4);
    K3SM_CHECK_EQ(s.writer_period_seconds, 1);
    K3SM_CHECK_EQ(s.observed_at, observed);
    K3SM_CHECK(s.platform == sdk::platform::darwin);
    K3SM_CHECK_EQ(s.node_name(), std::string_view("mac-worker-1"));
    K3SM_CHECK(s.level == sdk::level::ok);
    K3SM_CHECK(s.admit);
    K3SM_CHECK(s.kill_mode.mode == sdk::kill_mode::auto_);
    K3SM_CHECK(!s.kill_mode.armed);
    K3SM_CHECK_EQ(s.kill_mode.arming_episodes, 2);
    K3SM_CHECK(s.kill_mode.reason == sdk::kill_mode_reason::observing);
    K3SM_CHECK(s.admit_reason_signal == sdk::signal_id::unset);
    K3SM_CHECK_EQ(s.evict_deadline, none);
    K3SM_CHECK(s.darwin.has_value());
}

void check_darwin_sample(const sdk::snapshot& s, std::uint16_t minor = 0, std::uint32_t body = 2120) {
    using sdk::signal_id;
    using sdk::trend;
    using sdk::unit;
    using lr = sdk::limiting_resource;
    check_darwin_common(s, minor, body);
    check_capacity_darwin(s.capacity);
    if (s.darwin) {
        check_darwin_block(*s.darwin);
    }
    K3SM_CHECK_EQ(s.signal_count, 7u);
    if (s.signal_count == 7) {
        check_signal(s.signals[0], signal_id::memory_available, unit::bytes, 6 * gib,
                     rungs{{2 * gib, 1536 * mib, 1 * gib, 512 * mib, 100 * mib},
                           {2560 * mib, 2 * gib, 1536 * mib, 768 * mib, 256 * mib}},
                     90, 256 * mib, none, none, trend::steady, lr::memory);
        check_signal(s.signals[1], signal_id::memory_compressor, unit::permille, 210,
                     rungs{{500, 600, 700, 750, 800}, {450, 550, 650, 720, 780}}, 60, none, none, none,
                     trend::rising, lr::memory);
        check_signal(s.signals[2], signal_id::memory_swap, unit::bytes, 0,
                     rungs{{256 * mib, 512 * mib, 1 * gib, none, none}, {}}, none, none, 60, none,
                     trend::steady, lr::memory);
        check_signal(s.signals[3], signal_id::swapfs_available, unit::bytes, 120 * gib,
                     rungs{{24 * gib, 16 * gib, 12 * gib, none, 8 * gib}, {}}, none, none, none, none,
                     trend::unknown, lr::swapfs);
        check_signal(s.signals[4], signal_id::nodefs_available, unit::bytes, 120 * gib,
                     rungs{{20 * gib, 15 * gib, 10 * gib, none, 2 * gib}, {none, none, none, none, 10 * gib + 1}},
                     none, none, none, none, trend::unknown, lr::nodefs);
        check_signal(s.signals[5], signal_id::imagefs_available, unit::bytes, 120 * gib, rungs{}, none, none,
                     none, none, trend::unknown, lr::nodefs, signal_id::nodefs_available);
        check_signal(s.signals[6], signal_id::pid_available, unit::count, 9000,
                     rungs{{3000, 2500, 1500, none, 1000}, {}}, none, none, none, none, trend::unknown,
                     lr::pid);
    }
    K3SM_CHECK_EQ(s.recommendation.safe_additional_bytes, 3 * gib);
    K3SM_CHECK(s.recommendation.basis == signal_id::memory_available);
    K3SM_CHECK_EQ(s.counter_count, 3u);
    if (s.counter_count == 3) {
        const sdk::band_counters& c0 = s.counters[0];
        K3SM_CHECK(c0.band == sdk::band::critical);
        K3SM_CHECK(c0.evictions_total == 0 && c0.oom_kills_total == 0 && c0.kernel_kills_total == 0 &&
                   c0.restarts_total == 0 && c0.mem_high_events_total == 0 && c0.tier1_refused_total == 0);
        const sdk::band_counters& c1 = s.counters[1];
        K3SM_CHECK(c1.band == sdk::band::standard);
        K3SM_CHECK_EQ(c1.restarts_total, 3);
        K3SM_CHECK_EQ(c1.evictions_total, 0);
        const sdk::band_counters& c2 = s.counters[2];
        K3SM_CHECK(c2.band == sdk::band::batch);
        K3SM_CHECK_EQ(c2.evictions_total, 5);
        K3SM_CHECK_EQ(c2.oom_kills_total, 1);
        K3SM_CHECK_EQ(c2.kernel_kills_total, 1);
        K3SM_CHECK_EQ(c2.restarts_total, 7);
        K3SM_CHECK_EQ(c2.mem_high_events_total, 0);
        K3SM_CHECK_EQ(c2.tier1_refused_total, 0);
    }
}

void test_golden_darwin() {
    const auto r = open_read(fixture("darwin.v1.page"));
    K3SM_CHECK(r.has_value());
    if (r) {
        check_darwin_sample(*r);
        K3SM_CHECK(!r->linux_.has_value());
    }
}

void test_golden_linux_guest() {
    const auto r = open_read(fixture("linux-guest.v1.page"));
    K3SM_CHECK(r.has_value());
    if (r) {
        check_darwin_sample(*r);
        K3SM_CHECK(r->linux_.has_value());
        if (r->linux_) {
            check_linux_block(*r->linux_);
        }
    }
}

void test_golden_linux_producer() {
    const auto r = open_read(fixture("linux-producer.v1.page"));
    K3SM_CHECK(r.has_value());
    if (!r) {
        return;
    }
    const sdk::snapshot& s = *r;
    K3SM_CHECK_EQ(s.generation, 9);
    K3SM_CHECK_EQ(s.writer_period_seconds, 1);
    K3SM_CHECK_EQ(s.observed_at, observed);
    K3SM_CHECK(s.platform == sdk::platform::linux_);
    K3SM_CHECK_EQ(s.node_name(), std::string_view("linux-worker-1"));
    K3SM_CHECK(s.level == sdk::level::evicting);
    K3SM_CHECK(!s.admit);
    K3SM_CHECK(s.admit_reason_signal == sdk::signal_id::memory_available);
    K3SM_CHECK_EQ(s.evict_deadline, observed + 45);
    K3SM_CHECK_EQ(s.capacity.cpu_capacity_milli, 16000);
    K3SM_CHECK_EQ(s.capacity.cpu_system_reserve_milli, none);
    K3SM_CHECK_EQ(s.capacity.mem_capacity_bytes, 32 * gib);
    K3SM_CHECK_EQ(s.capacity.mem_system_reserve_bytes, none);
    K3SM_CHECK_EQ(s.capacity.mem_eviction_reserve_bytes, none);
    K3SM_CHECK_EQ(s.capacity.mem_allocatable_bytes, 30 * gib);
    K3SM_CHECK_EQ(s.capacity.swap_total_bytes, 0);
    K3SM_CHECK_EQ(s.capacity.swap_used_bytes, 0);
    K3SM_CHECK(!s.darwin.has_value());
    K3SM_CHECK(s.linux_.has_value());
    if (s.linux_) {
        check_linux_block(*s.linux_);
    }
    K3SM_CHECK_EQ(s.signal_count, 1u);
    if (s.signal_count == 1) {
        check_signal(s.signals[0], sdk::signal_id::memory_available, sdk::unit::bytes, 400 * mib,
                     rungs{{4 * gib, 3 * gib, 1600 * mib, 800 * mib, 100 * mib}, {}}, 90, 512 * mib, none, 30,
                     sdk::trend::falling, sdk::limiting_resource::memory);
    }
    K3SM_CHECK_EQ(s.recommendation.safe_additional_bytes, 0);
    K3SM_CHECK(s.recommendation.basis == sdk::signal_id::memory_available);
    K3SM_CHECK(s.kill_mode.mode == sdk::kill_mode::on);
    K3SM_CHECK(s.kill_mode.armed);
    K3SM_CHECK_EQ(s.kill_mode.arming_episodes, 3);
    K3SM_CHECK(s.kill_mode.reason == sdk::kill_mode_reason::armed);
    K3SM_CHECK_EQ(s.counter_count, 1u);
    if (s.counter_count == 1) {
        const sdk::band_counters& c = s.counters[0];
        K3SM_CHECK(c.band == sdk::band::batch);
        K3SM_CHECK_EQ(c.evictions_total, 2);
        K3SM_CHECK_EQ(c.oom_kills_total, 0);
        K3SM_CHECK_EQ(c.kernel_kills_total, 0);
        K3SM_CHECK_EQ(c.restarts_total, 0);
        K3SM_CHECK_EQ(c.mem_high_events_total, 11);
        K3SM_CHECK_EQ(c.tier1_refused_total, 1);
    }
}

void test_golden_absent() {
    const auto r = open_read(fixture("absent.v1.page"));
    K3SM_CHECK(r.has_value());
    if (!r) {
        return;
    }
    const sdk::snapshot& s = *r;
    check_darwin_common(s);
    const sdk::capacity& c = s.capacity;
    K3SM_CHECK(!c.cpu_capacity_milli && !c.cpu_system_reserve_milli && !c.mem_capacity_bytes &&
               !c.mem_system_reserve_bytes && !c.mem_eviction_reserve_bytes && !c.mem_allocatable_bytes &&
               !c.swap_total_bytes && !c.swap_used_bytes);
    if (s.darwin) {
        const sdk::darwin_signals& d = *s.darwin;
        K3SM_CHECK(!d.vm_pressure_level && !d.memorystatus_level_permille && !d.compressor.bytes_used &&
                   !d.compressor.bytes_stored && !d.compressor.segment_limit_bytes && !d.swapins_total &&
                   !d.swapouts_total);
    }
    K3SM_CHECK(!s.linux_.has_value());
    K3SM_CHECK_EQ(s.signal_count, 0u);
    K3SM_CHECK_EQ(s.counter_count, 0u);
    K3SM_CHECK_EQ(s.recommendation.safe_additional_bytes, none);
    K3SM_CHECK(s.recommendation.basis == sdk::signal_id::unset);
}

// Every decodable fixture of the manifest decodes, the raw one does not.
void test_manifest_fixture_list() {
    const std::string m = read_file(fixture("manifest.json"));
    const auto fixtures = k3sm_test::json_objects(m, "fixtures");
    K3SM_CHECK_EQ(fixtures.size(), 5u);
    for (const std::string& f : fixtures) {
        const std::string file = k3sm_test::json_value(f, "file");
        const bool decodes = k3sm_test::json_value(f, "decodes") == "true";
        const auto r = open_read(fixture(file));
        if (r.has_value() != decodes) {
            k3sm_test::fail(__FILE__, __LINE__, "fixture " + file + " decodes: " + (decodes ? "true" : "false"));
        }
    }
}

// Every negative page of the manifest reads as its manifest error.
void test_negative_cases() {
    const std::string m = read_file(fixture("manifest.json"));
    const auto cases = k3sm_test::json_objects(m, "negative");
    K3SM_CHECK_EQ(cases.size(), 6u);
    for (const std::string& c : cases) {
        const std::string file = k3sm_test::json_value(c, "file");
        const std::string want = k3sm_test::json_value(c, "error");
        const auto r = open_read(fixture(file));
        const std::string got = r ? std::string("ok") : std::string(r.error().name());
        if (got != want) {
            k3sm_test::fail(__FILE__, __LINE__, "negative " + file + ": got " + got + ", want " + want);
        }
    }
}

void test_not_found() {
    const auto page = sdk::budget_page::open(fixture("no-such.v1.page").c_str());
    K3SM_CHECK(!page);
    if (!page) {
        K3SM_CHECK(page.error() == sdk::error::not_found);
        K3SM_CHECK_EQ(page.error().sys_errno(), ENOENT);
        K3SM_CHECK_EQ(page.error().name(), std::string_view("notFound"));
    }
    const std::string empty_path = write_temp("");
    const auto empty = sdk::budget_page::open(empty_path.c_str());
    ::unlink(empty_path.c_str());
    K3SM_CHECK(!empty && empty.error() == sdk::error::truncated);
}

// A higher minor of the known major reads up to the known size; a header
// size or a structural code this reader does not know is a mismatch; an
// unknown producer-vocabulary code reads as unknown.
void test_minor_and_codes() {
    const std::string darwin = read_file(fixture("darwin.v1.page"));
    namespace o = sdk::page_offset;

    std::string newer = darwin;
    put_u64(newer, o::abi_major, get_u64(newer, o::abi_major) | (std::uint64_t{1} << 16));
    put_u64(newer, o::body_size, sdk::page_body_size + 8);
    put_u64(newer, std::size_t{sdk::page_header_size} + sdk::page_body_size, 0x1234);
    auto r = read_bytes(newer);
    K3SM_CHECK(r.has_value());
    if (r) {
        check_darwin_sample(*r, 1, sdk::page_body_size + 8);
    }

    std::string past_file = darwin;
    put_u64(past_file, o::body_size, sdk::page_size);  // headerSize + bodySize beyond the file
    r = read_bytes(past_file);
    K3SM_CHECK(!r && r.error() == sdk::error::truncated);

    std::string header = darwin;
    put_u64(header, o::abi_major, (get_u64(header, o::abi_major) & 0xffffffff) | (std::uint64_t{200} << 32));
    r = read_bytes(header);
    K3SM_CHECK(!r && r.error() == sdk::error::abi_mismatch);

    std::string level = darwin;
    put_u64(level, o::level, 5);
    r = read_bytes(level);
    K3SM_CHECK(!r && r.error() == sdk::error::abi_mismatch);

    std::string signal = darwin;
    put_u64(signal, o::signal_slot(0) + o::signal_field::id, 8);
    r = read_bytes(signal);
    K3SM_CHECK(!r && r.error() == sdk::error::abi_mismatch);

    std::string vocab = darwin;
    put_u64(vocab, o::signal_slot(0) + o::signal_field::trend, 99);
    put_u64(vocab, o::signal_slot(0) + o::signal_field::limiting_resource, 99);
    put_u64(vocab, o::kill_mode_reason, 99);
    r = read_bytes(vocab);
    K3SM_CHECK(r.has_value());
    if (r) {
        K3SM_CHECK(r->signals[0].trend == sdk::trend::unknown);
        K3SM_CHECK(r->signals[0].limiting_resource == sdk::limiting_resource::unknown);
        K3SM_CHECK(r->kill_mode.reason == sdk::kill_mode_reason::unknown);
    }

    // A slot left empty is skipped; the filled ones keep their order.
    std::string gap = darwin;
    for (std::size_t i = 0; i < o::signal_stride; i += 8) {
        put_u64(gap, o::signal_slot(1) + i, 0);
    }
    r = read_bytes(gap);
    K3SM_CHECK(r.has_value());
    if (r) {
        K3SM_CHECK_EQ(r->signal_count, 6u);
        K3SM_CHECK(r->signals[1].id == sdk::signal_id::memory_swap);
    }

    std::string name = darwin;
    put_u64(name, o::node_name_len, 253);
    r = read_bytes(name);
    K3SM_CHECK(r.has_value());
    put_u64(name, o::node_name_len, 254);
    r = read_bytes(name);
    K3SM_CHECK(!r && r.error() == sdk::error::corrupt);
}

// Stale when now - observedAt exceeds staleFactor writer periods.
void test_staleness() {
    const std::string path = fixture("darwin.v1.page");
    K3SM_CHECK(open_read(path, observed + sdk::stale_factor).has_value());
    const auto stale = open_read(path, observed + sdk::stale_factor + 1);
    K3SM_CHECK(!stale && stale.error() == sdk::error::stale);
    K3SM_CHECK(open_read(path, observed - 1000).has_value());  // a clock behind the writer's
    auto page = sdk::budget_page::open(path.c_str());
    K3SM_CHECK(page.has_value());
    if (page) {
        const auto now = page->read();  // the golden's 2026-10-19 stamp is long past
        K3SM_CHECK(!now && now.error() == sdk::error::stale);
    }
    K3SM_CHECK(!sdk::detail::is_stale(0, std::numeric_limits<std::int64_t>::max(), 1'000'000));
    K3SM_CHECK(sdk::detail::is_stale(std::numeric_limits<std::int64_t>::min(), 1,
                                     std::numeric_limits<std::int64_t>::max()));
    K3SM_CHECK(!sdk::detail::is_stale(std::numeric_limits<std::int64_t>::max(), 1,
                                      std::numeric_limits<std::int64_t>::min()));
}

void test_helpers() {
    using sdk::rung;
    using sdk::signal_id;
    const auto darwin = open_read(fixture("darwin.v1.page"));
    const auto linux_page = open_read(fixture("linux-producer.v1.page"));
    const auto absent = open_read(fixture("absent.v1.page"));
    K3SM_CHECK(darwin && linux_page && absent);
    if (!darwin || !linux_page || !absent) {
        return;
    }
    K3SM_CHECK(sdk::admit_ok(*darwin));
    K3SM_CHECK(!sdk::admit_ok(*linux_page));

    K3SM_CHECK_EQ(sdk::bytes_until(*darwin, signal_id::memory_available, rung::warn), 4 * gib);
    K3SM_CHECK_EQ(sdk::bytes_until(*darwin, signal_id::memory_available, rung::admit_stop), 6 * gib - 1536 * mib);
    K3SM_CHECK_EQ(sdk::bytes_until(*darwin, signal_id::memory_available, rung::evict_hard), 6 * gib - 100 * mib);
    K3SM_CHECK_EQ(sdk::bytes_until(*darwin, signal_id::memory_swap, rung::warn), 256 * mib);
    K3SM_CHECK_EQ(sdk::bytes_until(*darwin, signal_id::memory_swap, rung::evict_soft), none);
    K3SM_CHECK_EQ(sdk::bytes_until(*darwin, signal_id::swapfs_available, rung::evict_hard), 112 * gib);
    K3SM_CHECK_EQ(sdk::bytes_until(*darwin, signal_id::memory_compressor, rung::warn), none);  // permille
    K3SM_CHECK_EQ(sdk::bytes_until(*darwin, signal_id::pid_available, rung::warn), none);      // count
    K3SM_CHECK_EQ(sdk::bytes_until(*darwin, signal_id::imagefs_available, rung::warn), none);  // no rung
    K3SM_CHECK_EQ(sdk::bytes_until(*linux_page, signal_id::memory_available, rung::critical), 0);  // past it
    K3SM_CHECK_EQ(sdk::bytes_until(*linux_page, signal_id::memory_available, rung::evict_hard), 300 * mib);
    K3SM_CHECK_EQ(sdk::bytes_until(*linux_page, signal_id::memory_swap, rung::warn), none);  // no record
    K3SM_CHECK_EQ(sdk::bytes_until(*absent, signal_id::memory_available, rung::warn), none);

    K3SM_CHECK_EQ(sdk::batch_slots(*darwin, 1 * gib), 3);
    K3SM_CHECK_EQ(sdk::batch_slots(*darwin, 2 * gib), 1);
    K3SM_CHECK_EQ(sdk::batch_slots(*darwin, 4 * gib), 0);
    K3SM_CHECK_EQ(sdk::batch_slots(*darwin, 0), 0);
    K3SM_CHECK_EQ(sdk::batch_slots(*darwin, -1), 0);
    K3SM_CHECK_EQ(sdk::batch_slots(*linux_page, 1), 0);  // admit is false
    K3SM_CHECK_EQ(sdk::batch_slots(*absent, 1), 0);      // no recommendation
}

void test_result_and_move() {
    sdk::result<int> ok = 7;
    K3SM_CHECK(ok && *ok == 7);
    sdk::result<int> bad = sdk::error::torn;
    K3SM_CHECK(!bad && bad.error() == sdk::error::torn && bad.error().name() == "torn");
    bad = ok;
    K3SM_CHECK(bad && *bad == 7);

    auto a = sdk::budget_page::open(fixture("darwin.v1.page").c_str());
    K3SM_CHECK(a.has_value());
    if (a) {
        sdk::budget_page b = std::move(*a);
        K3SM_CHECK(a->data() == nullptr && a->size() == 0);
        K3SM_CHECK(b.size() == sdk::page_size && b.read_at(observed).has_value());
        sdk::budget_page c = std::move(b);
        b = std::move(c);
        K3SM_CHECK(b.read_at(observed).has_value());
    }
}

}  // namespace reader_test

int main() {
    reader_test::test_manifest_constants();
    reader_test::test_code_tables();
    reader_test::test_layout_descriptor();
    reader_test::test_pattern_fixture();
    reader_test::test_golden_darwin();
    reader_test::test_golden_linux_guest();
    reader_test::test_golden_linux_producer();
    reader_test::test_golden_absent();
    reader_test::test_manifest_fixture_list();
    reader_test::test_negative_cases();
    reader_test::test_not_found();
    reader_test::test_minor_and_codes();
    reader_test::test_staleness();
    reader_test::test_helpers();
    reader_test::test_result_and_move();
    if (k3sm_test::failures() != 0) {
        std::fprintf(stderr, "reader_test: %d failure(s)\n", k3sm_test::failures());
        return 1;
    }
    std::puts("reader_test: ok");
    return 0;
}
