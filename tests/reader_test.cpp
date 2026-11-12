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

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
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
    if (fd < 0) {
        std::fprintf(stderr, "cannot create %s\n", path.c_str());
        std::exit(2);
    }
    if (::write(fd, bytes.data(), bytes.size()) != static_cast<ssize_t>(bytes.size())) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        ::close(fd);
        ::unlink(path.c_str());
        std::exit(2);
    }
    if (::close(fd) != 0) {
        std::fprintf(stderr, "cannot close %s\n", path.c_str());
        ::unlink(path.c_str());
        std::exit(2);
    }
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
    K3SM_CHECK_EQ(k3sm_test::json_value(m, "layout"), std::string("layout.v1.1.txt"));
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
        {"capacity.cpuPerformanceCores", o::cpu_performance_cores},
        {"capacity.cpuEfficiencyCores", o::cpu_efficiency_cores},
        {"capacity.gpuPresent", o::gpu_present},
        {"capacity.gpuCount", o::gpu_count},
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

std::vector<layout_line> read_layout(const char* file = "layout.v1.1.txt") {
    std::vector<layout_line> out;
    std::istringstream in(read_file(fixture(file)));
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

    // The minor 0 descriptor is a line prefix of minor 1's and ends at the
    // minor 0 body size; minor 1 appends only the four capacity fields.
    const auto v0 = read_layout("layout.v1.0.txt");
    K3SM_CHECK_EQ(v0.size() + 4, layout.size());
    std::size_t end0 = 0;
    for (std::size_t i = 0; i < v0.size() && i < layout.size(); ++i) {
        K3SM_CHECK(v0[i].name == layout[i].name && v0[i].offset == layout[i].offset &&
                   v0[i].size == layout[i].size && v0[i].kind == layout[i].kind);
        if (v0[i].offset >= sdk::page_header_size) {
            end0 = std::max(end0, v0[i].offset + v0[i].size);
        }
    }
    K3SM_CHECK_EQ(end0, std::size_t{sdk::page_header_size} + sdk::page_body_size_minor0);
    K3SM_CHECK_EQ(sdk::page_body_size_of(0), sdk::page_body_size_minor0);
    K3SM_CHECK_EQ(sdk::page_body_size_of(1), sdk::page_body_size);
    K3SM_CHECK_EQ(sdk::page_body_size_of(7), sdk::page_body_size);
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

// check_capacity_minor1 checks the four capacity fields of minor 1.
void check_capacity_minor1(const sdk::capacity& c, std::optional<std::int64_t> perf,
                           std::optional<std::int64_t> eff, std::optional<bool> gpu,
                           std::optional<std::int64_t> gpus) {
    K3SM_CHECK_EQ(c.cpu_performance_cores, perf);
    K3SM_CHECK_EQ(c.cpu_efficiency_cores, eff);
    K3SM_CHECK_EQ(c.gpu_present, gpu);
    K3SM_CHECK_EQ(c.gpu_count, gpus);
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

void check_darwin_common(const sdk::snapshot& s, std::uint16_t minor = 1, std::uint32_t body = 2152) {
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

void check_darwin_sample(const sdk::snapshot& s, std::uint16_t minor = 1, std::uint32_t body = 2152) {
    using sdk::signal_id;
    using sdk::trend;
    using sdk::unit;
    using lr = sdk::limiting_resource;
    check_darwin_common(s, minor, body);
    check_capacity_darwin(s.capacity);
    if (minor == 0) {
        check_capacity_minor1(s.capacity, none, none, std::nullopt, none);
    } else {
        check_capacity_minor1(s.capacity, 4, 4, true, 1);
    }
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
    check_capacity_minor1(s.capacity, none, none, false, 0);
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
    check_capacity_minor1(c, none, none, std::nullopt, none);
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

// open takes only a regular file and maps at most one page: a FIFO fails at
// once instead of blocking until a writer appears, a directory fails, and a
// file longer than a page reads as its first page.
void test_open_rejects_fifo_and_caps_oversize_file() {
    const std::string dir_path = std::string(K3SM_SDK_TEST_TMPDIR) + "/open-XXXXXX";
    std::string dir = dir_path;
    if (::mkdtemp(dir.data()) == nullptr) {
        std::fprintf(stderr, "cannot create a directory under %s\n", K3SM_SDK_TEST_TMPDIR);
        std::exit(2);
    }

    // A FIFO. The open runs on a helper thread so a regression that blocks
    // fails the test after a bound instead of hanging the suite.
    const std::string fifo = dir + "/page.fifo";
    K3SM_CHECK_EQ(::mkfifo(fifo.c_str(), 0600), 0);
    std::atomic<bool> done{false};
    std::optional<sdk::result<sdk::budget_page>> fifo_page;
    std::thread opener([&] {
        fifo_page.emplace(sdk::budget_page::open(fifo.c_str()));
        done.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    int release_fd = -1;
    if (!done.load(std::memory_order_acquire)) {
        k3sm_test::fail(__FILE__, __LINE__, "budget_page::open blocked on a FIFO");
        release_fd = ::open(fifo.c_str(), O_RDWR | O_CLOEXEC);  // lets the blocked open return
    }
    opener.join();
    if (release_fd >= 0) {
        ::close(release_fd);
    }
    K3SM_CHECK(fifo_page.has_value() && !*fifo_page && fifo_page->error() == sdk::error::not_found);
    K3SM_CHECK(fifo_page.has_value() && !*fifo_page && fifo_page->error().sys_errno() == EINVAL);
    ::unlink(fifo.c_str());

    // A directory.
    const auto dir_page = sdk::budget_page::open(dir.c_str());
    K3SM_CHECK(!dir_page && dir_page.error() == sdk::error::not_found);
    K3SM_CHECK(!dir_page && dir_page.error().sys_errno() == EISDIR);
    ::rmdir(dir.c_str());

    // A valid page followed by padding maps one page.
    const std::string darwin = read_file(fixture("darwin.v1.page"));
    const std::string oversize_path = write_temp(darwin + std::string(3 * sdk::page_size, '\x5a'));
    auto big = sdk::budget_page::open(oversize_path.c_str());
    ::unlink(oversize_path.c_str());
    K3SM_CHECK(big.has_value());
    if (big) {
        K3SM_CHECK_EQ(big->size(), sdk::page_size);
        const auto got = big->read_at(observed);
        K3SM_CHECK(got.has_value());
        if (got) {
            check_darwin_sample(*got);
            K3SM_CHECK(!got->linux_.has_value());
        }
    }
}

// with_minor returns page with its abiMinor set to minor and its bodySize to
// body.
std::string with_minor(std::string page, std::uint16_t minor, std::uint32_t body) {
    namespace o = sdk::page_offset;
    put_u64(page, o::abi_major,
            std::uint64_t{sdk::page_abi_major} | std::uint64_t{minor} << 16 |
                std::uint64_t{sdk::page_header_size} << 32);
    put_u64(page, o::body_size, body);
    return page;
}

// A minor 0 page reads up to the minor 0 body, and the minor 1 fields read
// absent whatever bytes follow it; a page shorter than its own minor's body
// is truncated; gpuPresent holds 0, 1 or absent.
void test_minor0_page() {
    namespace o = sdk::page_offset;
    const std::string darwin = read_file(fixture("darwin.v1.page"));

    std::string v0 = with_minor(darwin, 0, sdk::page_body_size_minor0);
    for (std::size_t at = o::cpu_performance_cores; at <= o::gpu_count; at += 8) {
        put_u64(v0, at, 7);  // bytes past a minor 0 body are not fields
    }
    auto r = read_bytes(v0);
    K3SM_CHECK(r.has_value());
    if (r) {
        check_darwin_sample(*r, 0, sdk::page_body_size_minor0);
    }

    // A minor 0 page with a longer body still carries no minor 1 field.
    r = read_bytes(with_minor(darwin, 0, sdk::page_body_size));
    K3SM_CHECK(r.has_value());
    if (r) {
        check_capacity_minor1(r->capacity, none, none, std::nullopt, none);
    }

    r = read_bytes(with_minor(darwin, 0, sdk::page_body_size_minor0 - 8));
    K3SM_CHECK(!r && r.error() == sdk::error::truncated);
    r = read_bytes(with_minor(darwin, 1, sdk::page_body_size - 8));
    K3SM_CHECK(!r && r.error() == sdk::error::truncated);

    std::string gpu = darwin;
    put_u64(gpu, o::gpu_present, 0);
    put_u64(gpu, o::gpu_count, 0);
    r = read_bytes(gpu);
    K3SM_CHECK(r.has_value());
    if (r) {
        check_capacity_minor1(r->capacity, 4, 4, false, 0);
    }
    put_u64(gpu, o::gpu_present, 2);
    r = read_bytes(gpu);
    K3SM_CHECK(!r && r.error() == sdk::error::corrupt);
    put_u64(gpu, o::gpu_present, static_cast<std::uint64_t>(-1));
    r = read_bytes(gpu);
    K3SM_CHECK(!r && r.error() == sdk::error::corrupt);
}

// A higher minor of the known major reads up to the known size; a header
// size or a structural code this reader does not know is a mismatch; an
// unknown producer-vocabulary code reads as unknown.
void test_minor_and_codes() {
    const std::string darwin = read_file(fixture("darwin.v1.page"));
    namespace o = sdk::page_offset;

    std::string newer = with_minor(darwin, 2, sdk::page_body_size + 8);
    put_u64(newer, std::size_t{sdk::page_header_size} + sdk::page_body_size, 0x1234);
    auto r = read_bytes(newer);
    K3SM_CHECK(r.has_value());
    if (r) {
        check_darwin_sample(*r, 2, sdk::page_body_size + 8);
    }
    // A newer minor's body is at least this SDK's newest.
    r = read_bytes(with_minor(darwin, 2, sdk::page_body_size - 8));
    K3SM_CHECK(!r && r.error() == sdk::error::truncated);

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

// Every structural code site reads the first code past its table as a
// mismatch.
void test_structural_codes_out_of_range() {
    const std::string darwin = read_file(fixture("darwin.v1.page"));
    namespace o = sdk::page_offset;
    namespace d = sdk::detail;
    namespace sf = o::signal_field;
    struct site {
        const char* name;
        std::size_t offset;
        std::uint64_t count;
    };
    const site sites[] = {
        {"platform", o::platform, d::platform_codes},
        {"level", o::level, d::level_codes},
        {"signals[0].id", o::signal_slot(0) + sf::id, d::signal_codes},
        {"signals[0].unit", o::signal_slot(0) + sf::unit, d::unit_codes},
        {"signals[0].shared_with", o::signal_slot(0) + sf::shared_with, d::signal_codes},
        {"admit_reason_signal", o::admit_reason_signal, d::signal_codes},
        {"kill_mode.mode", o::kill_mode_mode, d::kill_mode_codes},
        {"recommendation.basis", o::recommendation_basis, d::signal_codes},
        {"counters[0].band", o::counter_slot(0) + o::counter_field::band, d::band_codes},
        {"signals[1].id", o::signal_slot(1) + sf::id, d::signal_codes},
        {"counters[1].band", o::counter_slot(1) + o::counter_field::band, d::band_codes},
    };
    for (const site& s : sites) {
        std::string last = darwin;
        put_u64(last, s.offset, s.count);
        const auto ok = read_bytes(last);
        if (!ok) {
            std::fprintf(stderr, "structural code site %s: last valid code %llu is rejected\n", s.name,
                         static_cast<unsigned long long>(s.count));
        }
        K3SM_CHECK(ok.has_value());
        std::string page = darwin;
        put_u64(page, s.offset, s.count + 1);
        const auto r = read_bytes(page);
        const bool mismatch = !r && r.error() == sdk::error::abi_mismatch;
        if (!mismatch) {
            std::fprintf(stderr, "structural code site %s: code %llu is not a mismatch\n", s.name,
                         static_cast<unsigned long long>(s.count + 1));
        }
        K3SM_CHECK(mismatch);
    }
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

// The stale rule's period and boundary edges, one row per case: the bound is
// stale_factor periods, clamped to the int64 range instead of overflowing.
void test_staleness_edges() {
    constexpr std::int64_t max = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t min = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t f = sdk::stale_factor;
    constexpr std::int64_t large = 1'000'000'000;
    struct row {
        const char* name;
        std::int64_t observed_at;
        std::int64_t period;
        std::int64_t now;
        bool want;
    };
    const row rows[] = {
        // Period 0 and a negative period pin today's behaviour: the bound is
        // zero or below, so every positive age is stale.
        {"period 0, age 0", observed, 0, observed, false},
        {"period 0, age 1", observed, 0, observed + 1, true},
        {"period -1, age 1", observed, -1, observed + 1, true},
        // A negative bound also makes an age of 0 stale, and an age down to
        // the bound itself fresh.
        {"period -1, age 0", observed, -1, observed, true},
        {"period -1, age -f", observed, -1, observed - f, false},
        {"period -1, age -f+1", observed, -1, observed - f + 1, true},
        {"period 1, age == bound", observed, 1, observed + f, false},
        {"period 1, age == bound+1", observed, 1, observed + f + 1, true},
        {"period 3, age == bound", observed, 3, observed + 3 * f, false},
        {"period 3, age == bound+1", observed, 3, observed + 3 * f + 1, true},
        {"large period, age == bound", 0, large, large * f, false},
        {"large period, age == bound+1", 0, large, large * f + 1, true},
        // max/f is not above max/f, so the bound is f*(max/f) == max-1 here.
        {"period max/f, age == bound", 0, max / f, f * (max / f), false},
        {"period max/f, age == max", 0, max / f, max, true},
        // Above max/f the bound clamps to max, which no age exceeds.
        {"period max/f+1, age == max", 0, max / f + 1, max, false},
        {"period max, age == max", 0, max, max, false},
        // min/f is not below min/f, so the bound is f*(min/f) == min+2 here.
        {"period min/f, age 0", 0, min / f, 0, true},
        {"period min/f, age == bound", 0, min / f, f * (min / f), false},
        {"period min/f, age == min", 0, min / f, min, false},
        // Below min/f the bound clamps to min: an age of min is fresh, any
        // other age stale.
        {"period min/f-1, age == min", 0, min / f - 1, min, false},
        {"period min/f-1, age == min+1", 0, min / f - 1, min + 1, true},
        {"period min, age 0", 0, min, 0, true},
        // A publication time in the future gives a negative age: fresh.
        {"future, period 1", observed + 10, 1, observed, false},
        {"future, period 0", observed + 10, 0, observed, false},
        {"future by max, period 1", max, 1, 0, false},
    };
    for (const auto& r : rows) {
        const bool got = sdk::detail::is_stale(r.observed_at, r.period, r.now);
        if (got != r.want) {
            std::fprintf(stderr, "staleness edge: %s\n", r.name);
        }
        K3SM_CHECK_EQ(got, r.want);
    }
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

// set_signal overwrites the value and warn threshold of id's record in s.
void set_signal(sdk::snapshot& s, sdk::signal_id id, std::int64_t value, std::int64_t warn) {
    for (std::size_t i = 0; i < s.signal_count; ++i) {
        if (s.signals[i].id == id) {
            s.signals[i].value = value;
            s.signals[i].warn_threshold = warn;
            return;
        }
    }
    k3sm_test::fail(__FILE__, __LINE__, "no record of the signal");
}

// test_bytes_until_clamps_negative_inputs: a negative value or threshold
// counts as 0 in either direction.
void test_bytes_until_clamps_negative_inputs() {
    using sdk::rung;
    using sdk::signal_id;
    const auto darwin = open_read(fixture("darwin.v1.page"));
    K3SM_CHECK(darwin.has_value());
    if (!darwin) {
        return;
    }
    sdk::snapshot s = *darwin;
    set_signal(s, signal_id::memory_available, -5, -10);
    K3SM_CHECK_EQ(sdk::bytes_until(s, signal_id::memory_available, rung::warn), 0);
    set_signal(s, signal_id::memory_available, 5, -10);
    K3SM_CHECK_EQ(sdk::bytes_until(s, signal_id::memory_available, rung::warn), 5);
    set_signal(s, signal_id::memory_swap, -10, -5);
    K3SM_CHECK_EQ(sdk::bytes_until(s, signal_id::memory_swap, rung::warn), 0);
    set_signal(s, signal_id::memory_swap, -10, 5);
    K3SM_CHECK_EQ(sdk::bytes_until(s, signal_id::memory_swap, rung::warn), 5);
}

// test_bytes_until_rungs_and_direction: each rung reads its own threshold,
// a falling signal measures value - threshold and a rising one threshold -
// value, and both clamp to 0 at or past the rung.
void test_bytes_until_rungs_and_direction() {
    using sdk::rung;
    using sdk::signal_id;
    const auto darwin = open_read(fixture("darwin.v1.page"));
    K3SM_CHECK(darwin.has_value());
    if (!darwin) {
        return;
    }
    sdk::snapshot s = *darwin;
    set_signal(s, signal_id::memory_available, 100 * mib, 50 * mib);
    for (std::size_t i = 0; i < s.signal_count; ++i) {
        if (s.signals[i].id == signal_id::memory_available) {
            s.signals[i].critical_threshold = 10 * mib;
            s.signals[i].evict_soft_threshold = 20 * mib;
        }
    }
    K3SM_CHECK_EQ(sdk::bytes_until(s, signal_id::memory_available, rung::critical), 90 * mib);
    K3SM_CHECK_EQ(sdk::bytes_until(s, signal_id::memory_available, rung::evict_soft), 80 * mib);
    set_signal(s, signal_id::memory_swap, 300 * mib, 200 * mib);
    K3SM_CHECK_EQ(sdk::bytes_until(s, signal_id::memory_swap, rung::warn), 0);  // past it
    set_signal(s, signal_id::memory_swap, 200 * mib, 200 * mib);
    K3SM_CHECK_EQ(sdk::bytes_until(s, signal_id::memory_swap, rung::warn), 0);  // at it
}

// test_bytes_until_nothing_to_measure: a present record whose value is
// absent, or whose unit is not bytes, gives no answer for any rung.
void test_bytes_until_nothing_to_measure() {
    using sdk::rung;
    using sdk::signal_id;
    const auto darwin = open_read(fixture("darwin.v1.page"));
    K3SM_CHECK(darwin.has_value());
    if (!darwin) {
        return;
    }
    const rung rungs[] = {rung::warn, rung::admit_stop, rung::critical, rung::evict_soft, rung::evict_hard};
    sdk::snapshot s = *darwin;
    sdk::signal_record* rec = nullptr;
    for (std::size_t i = 0; i < s.signal_count; ++i) {
        if (s.signals[i].id == signal_id::memory_available) {
            rec = &s.signals[i];
        }
    }
    K3SM_CHECK(rec != nullptr);
    if (rec == nullptr) {
        return;
    }
    rec->value = 100 * mib;
    for (rung r : rungs) {
        rec->warn_threshold = rec->admit_stop_threshold = rec->critical_threshold = 10 * mib;
        rec->evict_soft_threshold = rec->evict_hard_threshold = 10 * mib;
        K3SM_CHECK_EQ(sdk::bytes_until(s, signal_id::memory_available, r), 90 * mib);  // control
    }
    rec->value = std::nullopt;
    for (rung r : rungs) {
        K3SM_CHECK_EQ(sdk::bytes_until(s, signal_id::memory_available, r), none);
    }
    rec->value = 100 * mib;
    rec->unit = sdk::unit::permille;
    for (rung r : rungs) {
        K3SM_CHECK_EQ(sdk::bytes_until(s, signal_id::memory_available, r), none);
    }
}

// test_batch_slots_nonpositive_recommendation: a zero or negative
// safeAdditionalBytes gives no slot even while the node admits.
void test_batch_slots_nonpositive_recommendation() {
    const auto darwin = open_read(fixture("darwin.v1.page"));
    K3SM_CHECK(darwin.has_value());
    if (!darwin) {
        return;
    }
    sdk::snapshot s = *darwin;
    K3SM_CHECK(s.admit);
    s.recommendation.safe_additional_bytes = 0;
    K3SM_CHECK_EQ(sdk::batch_slots(s, 1), 0);
    s.recommendation.safe_additional_bytes = -1 * gib;
    K3SM_CHECK_EQ(sdk::batch_slots(s, 1), 0);
}

// test_open_short_file_is_truncated: a non-empty regular file of 64 bytes
// opens (open fails only on an empty file) and the error comes from read.
// 64 bytes hold the whole header line (offsets 0 to 63, up to seq), so read
// checks the header before the size: the darwin fixture's first 64 bytes
// pass magic, major and header size and fail the file size check with
// error::truncated, while 64 zero bytes fail the magic with abi_mismatch.
void test_open_short_file_is_truncated() {
    const std::string prefix = read_file(fixture("darwin.v1.page")).substr(0, 64);
    const struct {
        std::string bytes;
        sdk::error::kind want;
    } cases[] = {
        {prefix, sdk::error::truncated},
        {std::string(64, '\0'), sdk::error::abi_mismatch},
    };
    for (const auto& c : cases) {
        const std::string path = write_temp(c.bytes);
        auto page = sdk::budget_page::open(path.c_str());
        ::unlink(path.c_str());
        K3SM_CHECK(page.has_value());
        if (!page) {
            continue;
        }
        K3SM_CHECK_EQ(page->size(), std::size_t{64});
        const auto r = page->read_at(observed);
        K3SM_CHECK(!r.has_value());
        if (!r) {
            K3SM_CHECK(r.error() == c.want);
        }
    }
}

// test_page_shorter_than_header_is_truncated: a non-empty file shorter than
// the 64-byte header line opens (open fails only on an empty file) and reads
// as error::truncated. Each length is a prefix of the darwin golden, so the
// magic is intact; the 8-byte prefix has the magic and nothing after it, so
// only the size guard ahead of the header check can call it truncated.
void test_page_shorter_than_header_is_truncated() {
    const std::string golden = read_file(fixture("darwin.v1.page"));
    for (const std::size_t n : {std::size_t{8}, std::size_t{23}, std::size_t{63}}) {
        const std::string path = write_temp(golden.substr(0, n));
        auto page = sdk::budget_page::open(path.c_str());
        ::unlink(path.c_str());
        K3SM_CHECK(page.has_value());
        if (!page) {
            continue;
        }
        K3SM_CHECK_EQ(page->size(), n);
        const auto r = page->read_at(observed);
        if (r || r.error() != sdk::error::truncated) {
            k3sm_test::fail(__FILE__, __LINE__,
                            "a " + std::to_string(n) + "-byte page: got " +
                                (r ? std::string("ok") : std::string(r.error().name())) + ", want truncated");
        }
    }
}

// test_lone_linux_word_makes_linux_block: a darwin page carries the Linux
// block as soon as any one Linux word is present. The darwin golden holds
// none; setting only the first scanned word (the first PSI value) or only
// the last (memory.events oom_kill) gives a Linux block, which pins both
// ends of the scan.
void test_lone_linux_word_makes_linux_block() {
    namespace o = sdk::page_offset;
    const std::string golden = read_file(fixture("darwin.v1.page"));
    const auto plain = read_bytes(golden);
    K3SM_CHECK(plain.has_value());
    if (plain) {
        K3SM_CHECK(plain->platform == sdk::platform::darwin);
        K3SM_CHECK(!plain->linux_.has_value());
    }
    for (std::size_t offset = o::linux_psi; offset <= o::linux_memory_events_oom_kill; offset += 8) {
        K3SM_CHECK_EQ(static_cast<std::int64_t>(get_u64(golden, offset)), sdk::page_absent);
    }

    std::string first = golden;
    put_u64(first, o::linux_psi, 120);
    const auto f = read_bytes(first);
    K3SM_CHECK(f.has_value());
    if (f) {
        K3SM_CHECK(f->linux_.has_value());
        if (f->linux_) {
            K3SM_CHECK_EQ(f->linux_->psi.cpu.some.avg10_permille, 120);
        }
    }

    std::string last = golden;
    put_u64(last, o::linux_memory_events_oom_kill, 4);
    const auto l = read_bytes(last);
    K3SM_CHECK(l.has_value());
    if (l) {
        K3SM_CHECK(l->linux_.has_value());
        if (l->linux_) {
            K3SM_CHECK_EQ(l->linux_->memory_events.oom_kill, 4);
        }
    }
}

// test_find_signal: on the darwin golden, find_signal returns nullptr for
// the unset id and for an id the page does not hold, and the slot of an id
// it holds. The golden holds all seven ids, so the absent case clears the
// last slot (pid_available).
void test_find_signal() {
    namespace o = sdk::page_offset;
    using sdk::signal_id;
    const auto r = open_read(fixture("darwin.v1.page"));
    K3SM_CHECK(r.has_value());
    if (!r) {
        return;
    }
    K3SM_CHECK(r->find_signal(signal_id::unset) == nullptr);
    for (const signal_id id : {signal_id::memory_available, signal_id::nodefs_available, signal_id::pid_available}) {
        const sdk::signal_record* g = r->find_signal(id);
        K3SM_CHECK(g != nullptr);
        if (g != nullptr) {
            K3SM_CHECK(g->id == id);
            K3SM_CHECK(g >= r->signals.data() && g < r->signals.data() + r->signal_count);
        }
    }
    const sdk::signal_record* pid = r->find_signal(signal_id::pid_available);
    K3SM_CHECK(pid != nullptr && pid->value == 9000);

    std::string page = read_file(fixture("darwin.v1.page"));
    const std::size_t last = o::signal_slot(6);
    for (std::size_t i = 0; i < o::signal_stride; i += 8) {
        put_u64(page, last + i, 0);
    }
    const auto a = read_bytes(page);
    K3SM_CHECK(a.has_value());
    if (a) {
        K3SM_CHECK_EQ(a->signal_count, 6u);
        K3SM_CHECK(a->find_signal(signal_id::pid_available) == nullptr);
        K3SM_CHECK(a->find_signal(signal_id::unset) == nullptr);
        const sdk::signal_record* m = a->find_signal(signal_id::memory_available);
        K3SM_CHECK(m != nullptr && m->id == signal_id::memory_available);
    }
}

// remove_on_exit removes each named file, link or empty directory when the
// test returns on any path, including an early return after a failed check.
struct remove_on_exit {
    std::vector<std::string> paths;
    remove_on_exit(std::initializer_list<std::string> p) : paths(p) {}
    remove_on_exit(const remove_on_exit&) = delete;
    remove_on_exit& operator=(const remove_on_exit&) = delete;
    ~remove_on_exit() {
        for (const std::string& p : paths) {
            ::unlink(p.c_str());
            ::rmdir(p.c_str());
        }
    }
};

// make_temp_dir creates a fresh directory under the test directory.
std::string make_temp_dir() {
    std::string dir = std::string(K3SM_SDK_TEST_TMPDIR) + "/open-XXXXXX";
    if (::mkdtemp(dir.data()) == nullptr) {
        std::fprintf(stderr, "cannot create a directory under %s\n", K3SM_SDK_TEST_TMPDIR);
        std::exit(2);
    }
    return dir;
}

// write_page copies the darwin golden page to path.
bool write_page(const std::string& path) {
    const std::string bytes = read_file(fixture("darwin.v1.page"));
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        return false;
    }
    const bool ok = !bytes.empty() && ::write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size());
    ::close(fd);
    return ok;
}

// test_corrupt_beats_stale: a page with an out-of-domain field read with a
// clock far past the staleness bound reports corrupt, because decode runs
// before the staleness check.
void test_corrupt_beats_stale() {
    namespace o = sdk::page_offset;
    std::string page = read_file(fixture("darwin.v1.page"));
    put_u64(page, o::gpu_present, 2);
    const auto r = read_bytes(page, observed + 100 * sdk::stale_factor);
    K3SM_CHECK(!r && r.error() == sdk::error::corrupt);
    // The same clock on the intact page is stale, so the clock is far enough.
    const auto s = open_read(fixture("darwin.v1.page"), observed + 100 * sdk::stale_factor);
    K3SM_CHECK(!s && s.error() == sdk::error::stale);
}

// test_open_follows_symlink: a symlink to the page opens and reads the same
// snapshot, and a symlink to a directory fails like the directory itself.
void test_open_follows_symlink() {
    const std::string dir = make_temp_dir();
    const std::string link = dir + "/page.link";
    const std::string dirlink = dir + "/dir.link";
    const remove_on_exit cleanup{link, dirlink, dir};
    K3SM_CHECK_EQ(::symlink(fixture("darwin.v1.page").c_str(), link.c_str()), 0);
    K3SM_CHECK_EQ(::symlink(dir.c_str(), dirlink.c_str()), 0);

    const auto r = open_read(link);
    K3SM_CHECK(r.has_value());
    if (r) {
        check_darwin_sample(*r);
    }
    const auto direct = open_read(fixture("darwin.v1.page"));
    K3SM_CHECK(direct.has_value());
    if (r && direct) {
        K3SM_CHECK_EQ(r->observed_at, direct->observed_at);
        K3SM_CHECK_EQ(r->seq, direct->seq);
    }

    const auto d = sdk::budget_page::open(dirlink.c_str());
    K3SM_CHECK(!d && d.error() == sdk::error::not_found);
    K3SM_CHECK(!d && d.error().sys_errno() == EISDIR);
}

// test_open_carries_eacces_and_enotdir: open reports the errno of the failed
// system call, here an unreadable page and a path beneath a regular file.
void test_open_carries_eacces_and_enotdir() {
    const std::string dir = make_temp_dir();
    const std::string locked = dir + "/locked.page";
    const std::string plain = dir + "/plain.page";
    const remove_on_exit cleanup{locked, plain, dir};
    K3SM_CHECK(write_page(locked));
    K3SM_CHECK(write_page(plain));

    if (::geteuid() != 0) {
        K3SM_CHECK_EQ(::chmod(locked.c_str(), 0), 0);
        const auto p = sdk::budget_page::open(locked.c_str());
        K3SM_CHECK(!p && p.error() == sdk::error::not_found);
        K3SM_CHECK(!p && p.error().sys_errno() == EACCES);
    } else {
        std::fprintf(stderr, "skip: the EACCES half needs a non-root user\n");
    }

    const std::string below = plain + "/page";
    const auto q = sdk::budget_page::open(below.c_str());
    K3SM_CHECK(!q && q.error() == sdk::error::not_found);
    K3SM_CHECK(!q && q.error().sys_errno() == ENOTDIR);
}

// test_open_carries_eloop_and_enametoolong: open reports the errno of a
// symlink loop and of a path component longer than the system allows.
void test_open_carries_eloop_and_enametoolong() {
    const std::string dir = make_temp_dir();
    const std::string a = dir + "/a.link";
    const std::string b = dir + "/b.link";
    const remove_on_exit cleanup{a, b, dir};
    K3SM_CHECK_EQ(::symlink(b.c_str(), a.c_str()), 0);
    K3SM_CHECK_EQ(::symlink(a.c_str(), b.c_str()), 0);
    const auto loop = sdk::budget_page::open(a.c_str());
    K3SM_CHECK(!loop && loop.error() == sdk::error::not_found);
    K3SM_CHECK(!loop && loop.error().sys_errno() == ELOOP);

    const std::string longname(5000, 'x');
    const auto tooLong = sdk::budget_page::open(longname.c_str());
    K3SM_CHECK(!tooLong && tooLong.error() == sdk::error::not_found);
    K3SM_CHECK(!tooLong && tooLong.error().sys_errno() == ENAMETOOLONG);
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

// test_move_assign_and_failed_opens_leak_nothing: move-assigning a live page
// over another live page unmaps the old mapping and leaves the moved-from
// page empty, and neither that nor a failed open (an empty file, a missing
// file, a directory) or a page that opens but fails to read (a short page, a
// bad magic) keeps a descriptor open, however often it is repeated.
void test_move_assign_and_failed_opens_leak_nothing() {
    const std::string page_bytes = read_file(fixture("darwin.v1.page"));
    const std::string dir = make_temp_dir();
    const std::string good = dir + "/good.page";
    const std::string empty = dir + "/empty.page";
    const std::string short_page = dir + "/short.page";
    const std::string bad_magic = dir + "/magic.page";
    const std::string missing = dir + "/missing.page";
    const remove_on_exit cleanup{good, empty, short_page, bad_magic, dir};
    K3SM_CHECK(write_page(good));
    {
        const struct {
            const std::string* path;
            std::string bytes;
        } files[] = {{&empty, ""}, {&short_page, page_bytes.substr(0, 64)}, {&bad_magic, std::string(64, '\0')}};
        for (const auto& f : files) {
            const int fd = ::open(f.path->c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
            K3SM_CHECK(fd >= 0);
            if (fd >= 0) {
                K3SM_CHECK(::write(fd, f.bytes.data(), f.bytes.size()) == static_cast<ssize_t>(f.bytes.size()));
                ::close(fd);
            }
        }
    }

    const auto assign_round = [&]() {
        auto a = sdk::budget_page::open(good.c_str());
        auto b = sdk::budget_page::open(good.c_str());
        K3SM_CHECK(a.has_value() && b.has_value());
        if (!a || !b) {
            return;
        }
        const void* new_base = a->data();
        const void* old_base = b->data();
        K3SM_CHECK(new_base != old_base);
        *b = std::move(*a);
        K3SM_CHECK(b->data() == new_base && b->size() == sdk::page_size);
        K3SM_CHECK(a->data() == nullptr && a->size() == 0);
        const auto r = b->read_at(observed);
        K3SM_CHECK(r.has_value());
        if (r) {
            check_darwin_sample(*r);
        }
        // The old mapping is gone: msync on an unmapped range fails with ENOMEM.
        K3SM_CHECK(::msync(const_cast<void*>(old_base), sdk::page_size, MS_ASYNC) == -1 && errno == ENOMEM);
    };
    const auto fail_round = [&]() {
        const auto e = sdk::budget_page::open(empty.c_str());
        K3SM_CHECK(!e && e.error() == sdk::error::truncated);
        const auto m = sdk::budget_page::open(missing.c_str());
        K3SM_CHECK(!m && m.error() == sdk::error::not_found && m.error().sys_errno() == ENOENT);
        const auto d = sdk::budget_page::open(dir.c_str());
        K3SM_CHECK(!d && d.error() == sdk::error::not_found && d.error().sys_errno() == EISDIR);
        const auto s = open_read(short_page);
        K3SM_CHECK(!s && s.error() == sdk::error::truncated);
        const auto b = open_read(bad_magic);
        K3SM_CHECK(!b && b.error() == sdk::error::abi_mismatch);
    };

    assign_round();
    const int before_assign = k3sm_test::open_fd_count();
    for (int i = 0; i < 300; ++i) {
        assign_round();
    }
    K3SM_CHECK_EQ(k3sm_test::open_fd_count(), before_assign);

    fail_round();
    const int before_fail = k3sm_test::open_fd_count();
    for (int i = 0; i < 300; ++i) {
        fail_round();
    }
    K3SM_CHECK_EQ(k3sm_test::open_fd_count(), before_fail);
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
    reader_test::test_open_rejects_fifo_and_caps_oversize_file();
    reader_test::test_minor0_page();
    reader_test::test_minor_and_codes();
    reader_test::test_structural_codes_out_of_range();
    reader_test::test_staleness();
    reader_test::test_staleness_edges();
    reader_test::test_helpers();
    reader_test::test_bytes_until_clamps_negative_inputs();
    reader_test::test_bytes_until_rungs_and_direction();
    reader_test::test_bytes_until_nothing_to_measure();
    reader_test::test_batch_slots_nonpositive_recommendation();
    reader_test::test_open_short_file_is_truncated();
    reader_test::test_page_shorter_than_header_is_truncated();
    reader_test::test_lone_linux_word_makes_linux_block();
    reader_test::test_find_signal();
    reader_test::test_corrupt_beats_stale();
    reader_test::test_open_follows_symlink();
    reader_test::test_open_carries_eacces_and_enotdir();
    reader_test::test_open_carries_eloop_and_enametoolong();
    reader_test::test_result_and_move();
    reader_test::test_move_assign_and_failed_opens_leak_nothing();
    if (k3sm_test::failures() != 0) {
        std::fprintf(stderr, "reader_test: %d failure(s)\n", k3sm_test::failures());
        return 1;
    }
    std::puts("reader_test: ok");
    return 0;
}
