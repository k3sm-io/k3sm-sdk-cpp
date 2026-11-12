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

// The page writer: the encoder against the golden pages the apis repository
// publishes (each decodable page decoded and encoded again, and the absent
// page built by hand, are byte-identical to the file), the notify vectors of
// the manifest, and the live writer on a page file: preallocation, the
// writer lock, the seqlock words, generation and seq across a restart, the
// in-place page a reader's open mapping keeps seeing, and the wake.

#include <k3sm/budget.hpp>
#include <k3sm/page_writer.hpp>

#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_util.hpp"

namespace sdk = k3sm::sdk;
using namespace std::chrono_literals;

namespace writer_test {

constexpr std::int64_t observed = 1792411200;

std::string base_dir() {
    return std::string(K3SM_SDK_TEST_TMPDIR) + "/writer_test." + std::to_string(::getpid());
}
std::string page_dir() { return base_dir() + "/page"; }
std::string lock_dir() { return base_dir() + "/state"; }
std::string page_path(const char* name) { return page_dir() + "/" + name; }
std::string lock_path(const char* name) { return lock_dir() + "/" + name; }

// Unlinks each path it holds when it leaves scope, so an early return cannot leak a test file.
struct unlinker {
    std::vector<std::string> paths;
    explicit unlinker(std::initializer_list<std::string> p) : paths(p) {}
    unlinker(const unlinker&) = delete;
    unlinker& operator=(const unlinker&) = delete;
    ~unlinker() {
        for (const std::string& p : paths) {
            ::unlink(p.c_str());
        }
    }
};

std::uint64_t get_u64(const std::string& page, std::size_t offset) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= std::uint64_t{static_cast<unsigned char>(page[offset + i])} << (8 * i);
    }
    return v;
}

void put_u64(std::string& page, std::size_t offset, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        page[offset + i] = static_cast<char>(v >> (8 * i));
    }
}

sdk::result<sdk::snapshot> read_path(const std::string& path) {
    auto page = sdk::budget_page::open(path.c_str());
    if (!page) {
        return page.error();
    }
    return page->read_at(observed);
}

// golden returns the decoded snapshot of a golden page.
sdk::snapshot golden(const char* name) {
    auto r = read_path(k3sm_test::fixture(name));
    if (!r) {
        std::fprintf(stderr, "cannot read the golden %s: %s\n", name, std::string(r.error().name()).c_str());
        std::exit(2);
    }
    return *r;
}

std::string encoded(const sdk::snapshot& s) {
    std::string out(sdk::page_size, '\x7f');
    const auto r = sdk::encode_page(s, reinterpret_cast<unsigned char*>(out.data()), out.size());
    K3SM_CHECK(r.has_value());
    if (!r) {
        return {};
    }
    K3SM_CHECK_EQ(*r, sdk::page_size);
    return out;
}

// first_difference names the first differing byte offset, or -1.
long first_difference(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return 0;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) {
            return static_cast<long>(i);
        }
    }
    return -1;
}

// Every decodable golden page of the manifest, decoded and encoded again,
// is the file byte for byte.
void test_golden_round_trip() {
    const std::string m = k3sm_test::read_file(k3sm_test::fixture("manifest.json"));
    int checked = 0;
    for (const std::string& f : k3sm_test::json_objects(m, "fixtures")) {
        if (k3sm_test::json_value(f, "decodes") != "true") {
            continue;
        }
        const std::string file = k3sm_test::json_value(f, "file");
        const std::string want = k3sm_test::read_file(k3sm_test::fixture(file));
        K3SM_CHECK_EQ(want.size(), sdk::page_size);
        const std::string got = encoded(golden(file.c_str()));
        const long at = first_difference(got, want);
        if (at >= 0) {
            k3sm_test::fail(__FILE__, __LINE__, file + " differs at byte " + std::to_string(at));
        }
        ++checked;
    }
    K3SM_CHECK_EQ(checked, 4);
}

// The absent page, built by hand from its description in the manifest and
// the values the reader test pins, without the reader's decode.
void test_golden_absent_by_hand() {
    sdk::snapshot s;
    s.generation = 4;
    s.writer_period_seconds = 1;
    s.observed_at = observed;
    s.platform = sdk::platform::darwin;
    const std::string_view name = "mac-worker-1";
    std::memcpy(s.node_name_raw.data(), name.data(), name.size());
    s.node_name_len = name.size();
    s.level = sdk::level::ok;
    s.darwin = sdk::darwin_signals{};
    s.admit = true;
    s.kill_mode.mode = sdk::kill_mode::auto_;
    s.kill_mode.arming_episodes = 2;
    s.kill_mode.reason = sdk::kill_mode_reason::observing;
    // Bytes past the name's length are not the writer's to copy.
    s.node_name_raw[200] = 'x';
    s.seq = 7;
    s.notify_epoch = 9;
    const std::string want = k3sm_test::read_file(k3sm_test::fixture("absent.v1.page"));
    const long at = first_difference(encoded(s), want);
    if (at >= 0) {
        k3sm_test::fail(__FILE__, __LINE__, "absent.v1.page differs at byte " + std::to_string(at));
    }
}

// The writer writes the minor 1 capacity fields at their offsets, and a
// snapshot read from a minor 0 page encodes as minor 1 with them absent.
void test_encode_minor1_fields() {
    namespace o = sdk::page_offset;
    sdk::snapshot s = golden("darwin.v1.page");
    s.capacity.cpu_performance_cores = 10;
    s.capacity.cpu_efficiency_cores = 4;
    s.capacity.gpu_present = false;
    s.capacity.gpu_count = 0;
    std::string page = encoded(s);
    K3SM_CHECK_EQ(get_u64(page, o::cpu_performance_cores), 10u);
    K3SM_CHECK_EQ(get_u64(page, o::cpu_efficiency_cores), 4u);
    K3SM_CHECK_EQ(get_u64(page, o::gpu_present), 0u);
    K3SM_CHECK_EQ(get_u64(page, o::gpu_count), 0u);
    K3SM_CHECK_EQ(get_u64(page, o::abi_major) >> 16 & 0xffff, sdk::page_abi_minor);
    K3SM_CHECK_EQ(get_u64(page, o::body_size) & 0xffffffff, sdk::page_body_size);

    s.capacity.gpu_present = true;
    page = encoded(s);
    K3SM_CHECK_EQ(get_u64(page, o::gpu_present), 1u);

    // A minor 0 page: the same golden with its minor and body size rolled back.
    std::string v0 = k3sm_test::read_file(k3sm_test::fixture("darwin.v1.page"));
    put_u64(v0, o::abi_major,
            std::uint64_t{sdk::page_abi_major} | std::uint64_t{sdk::page_header_size} << 32);
    put_u64(v0, o::body_size, sdk::page_body_size_minor0);
    const std::string path = page_path("minor0.page");
    {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        K3SM_CHECK(f != nullptr);
        if (f == nullptr) {
            return;
        }
        std::fwrite(v0.data(), 1, v0.size(), f);
        std::fclose(f);
    }
    const auto r = read_path(path);
    ::unlink(path.c_str());
    K3SM_CHECK(r.has_value());
    if (!r) {
        return;
    }
    K3SM_CHECK(!r->capacity.cpu_performance_cores && !r->capacity.cpu_efficiency_cores &&
               !r->capacity.gpu_present && !r->capacity.gpu_count);
    page = encoded(*r);
    const auto absent = static_cast<std::uint64_t>(sdk::page_absent);
    for (std::size_t at = o::cpu_performance_cores; at <= o::gpu_count; at += 8) {
        K3SM_CHECK_EQ(get_u64(page, at), absent);
    }
}

void test_encode_refusals() {
    sdk::snapshot s = golden("darwin.v1.page");
    unsigned char buf[sdk::page_size];
    K3SM_CHECK(sdk::encode_page(s, buf, sizeof buf - 1).error() == sdk::error::invalid_argument);

    // The manifest's encoder error for a name past max_node_name_bytes.
    const std::string m = k3sm_test::read_file(k3sm_test::fixture("manifest.json"));
    K3SM_CHECK_EQ(k3sm_test::json_value(m, "encoderError"), std::string("nodeNameTooLong"));
    sdk::snapshot longer = s;
    longer.node_name_len = sdk::max_node_name_bytes + 1;
    const auto r = sdk::encode_page(longer, buf, sizeof buf);
    K3SM_CHECK(!r && r.error() == sdk::error::node_name_too_long);
    K3SM_CHECK(!r && r.error().name() == "nodeNameTooLong");
    longer.node_name_len = sdk::max_node_name_bytes;
    K3SM_CHECK(sdk::encode_page(longer, buf, sizeof buf).has_value());

    auto invalid = [&](auto mutate) {
        sdk::snapshot t = s;
        mutate(t);
        const auto e = sdk::encode_page(t, buf, sizeof buf);
        return !e && e.error() == sdk::error::invalid_snapshot;
    };
    K3SM_CHECK(invalid([](sdk::snapshot& t) { t.node_name_len = 0; }));
    K3SM_CHECK(invalid([](sdk::snapshot& t) { t.writer_period_seconds = 0; }));
    K3SM_CHECK(invalid([](sdk::snapshot& t) { t.platform = sdk::platform::unset; }));
    K3SM_CHECK(invalid([](sdk::snapshot& t) { t.level = sdk::level::unset; }));
    K3SM_CHECK(invalid([](sdk::snapshot& t) { t.level = static_cast<sdk::level>(5); }));
    K3SM_CHECK(invalid([](sdk::snapshot& t) { t.darwin.reset(); }));
    K3SM_CHECK(invalid([](sdk::snapshot& t) { t.platform = sdk::platform::linux_; }));
    K3SM_CHECK(invalid([](sdk::snapshot& t) { t.signal_count = sdk::max_signals + 1; }));
    K3SM_CHECK(invalid([](sdk::snapshot& t) { t.counter_count = sdk::max_counters + 1; }));
    K3SM_CHECK(invalid([](sdk::snapshot& t) { t.signals[0].id = sdk::signal_id::unset; }));
    K3SM_CHECK(invalid([](sdk::snapshot& t) { t.signals[0].unit = static_cast<sdk::unit>(4); }));
    K3SM_CHECK(invalid([](sdk::snapshot& t) { t.counters[0].band = sdk::band::unset; }));
}

// Each field check of the encoder, one mutated field per case, from the
// darwin golden or, for the linux cases, the linux producer golden: every
// case is refused as invalid_snapshot, and the unmutated baseline is not.
// Two of them are also refused by a live publish that leaves the page as it
// was.
struct field_case {
    const char* name;
    bool linux_golden;
    void (*mutate)(sdk::snapshot&);
};

void test_encode_field_checks() {
    const sdk::snapshot darwin = golden("darwin.v1.page");
    const sdk::snapshot linux_producer = golden("linux-producer.v1.page");
    K3SM_CHECK(darwin.signal_count > 1);
    K3SM_CHECK(darwin.counter_count > 1);
    unsigned char buf[sdk::page_size];
    for (const sdk::snapshot* base : {&darwin, &linux_producer}) {
        K3SM_CHECK(sdk::encode_page(*base, buf, sizeof buf).has_value());
    }

    const field_case cases[] = {
        {"linux block unset on a linux page", true, [](sdk::snapshot& t) { t.linux_.reset(); }},
        {"darwin block on a linux page", true, [](sdk::snapshot& t) { t.darwin = sdk::darwin_signals{}; }},
        {"admit reason signal out of range", false,
         [](sdk::snapshot& t) { t.admit_reason_signal = static_cast<sdk::signal_id>(sdk::detail::signal_codes + 1); }},
        {"kill mode out of range", false,
         [](sdk::snapshot& t) { t.kill_mode.mode = static_cast<sdk::kill_mode>(sdk::detail::kill_mode_codes + 1); }},
        {"kill mode reason out of range", false,
         [](sdk::snapshot& t) {
             t.kill_mode.reason = static_cast<sdk::kill_mode_reason>(sdk::detail::kill_mode_reason_codes + 1);
         }},
        {"recommendation basis out of range", false,
         [](sdk::snapshot& t) { t.recommendation.basis = static_cast<sdk::signal_id>(sdk::detail::signal_codes + 1); }},
        {"signal id out of range", false,
         [](sdk::snapshot& t) { t.signals[0].id = static_cast<sdk::signal_id>(sdk::detail::signal_codes + 1); }},
        {"trend out of range", false,
         [](sdk::snapshot& t) { t.signals[0].trend = static_cast<sdk::trend>(sdk::detail::trend_codes + 1); }},
        {"limiting resource out of range", false,
         [](sdk::snapshot& t) {
             t.signals[0].limiting_resource = static_cast<sdk::limiting_resource>(sdk::detail::limiting_resource_codes + 1);
         }},
        {"shared with out of range", false,
         [](sdk::snapshot& t) { t.signals[0].shared_with = static_cast<sdk::signal_id>(sdk::detail::signal_codes + 1); }},
        {"unit out of range on the last slot", false,
         [](sdk::snapshot& t) { t.signals[t.signal_count - 1].unit = static_cast<sdk::unit>(sdk::detail::unit_codes + 1); }},
        {"band out of range", false,
         [](sdk::snapshot& t) { t.counters[0].band = static_cast<sdk::band>(sdk::detail::band_codes + 1); }},
        {"band out of range on the last counter", false,
         [](sdk::snapshot& t) { t.counters[t.counter_count - 1].band = static_cast<sdk::band>(sdk::detail::band_codes + 1); }},
    };
    for (const field_case& c : cases) {
        sdk::snapshot t = c.linux_golden ? linux_producer : darwin;
        c.mutate(t);
        const auto e = sdk::encode_page(t, buf, sizeof buf);
        if (e || e.error() != sdk::error::invalid_snapshot) {
            k3sm_test::fail(__FILE__, __LINE__, c.name);
        }
    }

    const std::string page = page_path("fields.page");
    const std::string lock = lock_path("fields.lock");
    const unlinker guard{page, lock};
    auto w = sdk::page_writer::open(page.c_str(), lock.c_str());
    K3SM_CHECK(w.has_value());
    if (!w) {
        return;
    }
    K3SM_CHECK(w->publish(darwin).has_value());
    for (std::size_t i : {std::size_t{4}, std::size_t{10}}) {
        const std::string before = k3sm_test::read_file(page);
        sdk::snapshot bad = darwin;
        cases[i].mutate(bad);
        const auto refused = w->publish(bad);
        if (refused || refused.error() != sdk::error::invalid_snapshot) {
            k3sm_test::fail(__FILE__, __LINE__, cases[i].name);
        }
        K3SM_CHECK_EQ(first_difference(before, k3sm_test::read_file(page)), -1);
    }
}

// set_field applies one notify vector to s; false for a field it does not know.
bool set_field(sdk::snapshot& s, const std::string& field, std::int64_t v) {
    auto opt = [&](sdk::opt_i64& o) {
        o = v;
        if (v == sdk::page_absent) {
            o.reset();
        }
        return true;
    };
    if (field == "level") {
        s.level = static_cast<sdk::level>(v);
        return true;
    }
    if (field == "admit") {
        s.admit = v != 0;
        return true;
    }
    if (field == "generation") {
        s.generation = v;
        return true;
    }
    if (field == "observedAt") {
        s.observed_at = v;
        return true;
    }
    if (field == "capacity.swapUsedBytes") {
        return opt(s.capacity.swap_used_bytes);
    }
    if (field == "capacity.cpuPerformanceCores") {
        return opt(s.capacity.cpu_performance_cores);
    }
    if (field == "capacity.gpuPresent") {
        s.capacity.gpu_present = v == 0 ? std::optional<bool>(false) : std::optional<bool>(true);
        if (v == sdk::page_absent) {
            s.capacity.gpu_present.reset();
        }
        return true;
    }
    if (field == "darwin.vmPressureLevel") {
        return opt(s.darwin->vm_pressure_level);
    }
    if (field == "killMode.armingEpisodes") {
        s.kill_mode.arming_episodes = v;
        return true;
    }
    if (field == "recommendation.safeAdditionalBytes") {
        return opt(s.recommendation.safe_additional_bytes);
    }
    if (field.rfind("counters[", 0) == 0) {
        const std::size_t i = static_cast<std::size_t>(field[9] - '0');
        if (field.substr(11) == ".evictionsTotal") {
            s.counters[i].evictions_total = v;
            return true;
        }
        return false;
    }
    if (field.rfind("signals[", 0) == 0) {
        const std::size_t i = static_cast<std::size_t>(field[8] - '0');
        const std::string f = field.substr(11);
        sdk::signal_record& r = s.signals[i];
        if (f == "value") return opt(r.value);
        if (f == "trend") {
            r.trend = static_cast<sdk::trend>(v);
            return true;
        }
        if (f == "warnThreshold") return opt(r.warn_threshold);
        if (f == "admitStopThreshold") return opt(r.admit_stop_threshold);
        if (f == "criticalThreshold") return opt(r.critical_threshold);
        if (f == "evictSoftThreshold") return opt(r.evict_soft_threshold);
        if (f == "evictHardThreshold") return opt(r.evict_hard_threshold);
        if (f == "warnRelease") return opt(r.warn_release);
        if (f == "admitStopRelease") return opt(r.admit_stop_release);
        if (f == "criticalRelease") return opt(r.critical_release);
        if (f == "evictSoftRelease") return opt(r.evict_soft_release);
        if (f == "evictHardRelease") return opt(r.evict_hard_release);
    }
    return false;
}

void make_dirs() {
    ::mkdir(base_dir().c_str(), 0755);
    ::mkdir(page_dir().c_str(), 0755);
    ::mkdir(lock_dir().c_str(), 0700);
}

// The manifest's notify vectors: notify_due agrees with each, and a live
// writer bumps notifyEpoch exactly for the due ones.
void test_notify_vectors() {
    const std::string m = k3sm_test::read_file(k3sm_test::fixture("manifest.json"));
    const auto vectors = k3sm_test::json_objects(m, "notifyVectors");
    K3SM_CHECK(vectors.size() >= 10);
    const std::string page = page_path("vectors.page");
    const unlinker guard{page};
    auto w = sdk::page_writer::open(page.c_str(), lock_path("vectors.lock").c_str());
    K3SM_CHECK(w.has_value());
    if (!w) {
        return;
    }
    for (const std::string& v : vectors) {
        const std::string base = k3sm_test::json_value(v, "base");
        const std::string field = k3sm_test::json_value(v, "field");
        const std::int64_t value = std::strtoll(k3sm_test::json_value(v, "value").c_str(), nullptr, 10);
        const bool due = k3sm_test::json_value(v, "due") == "true";
        const sdk::snapshot before = golden(base.c_str());
        sdk::snapshot after = before;
        if (!set_field(after, field, value)) {
            k3sm_test::fail(__FILE__, __LINE__, "notify vector field not known: " + field);
            continue;
        }
        if (sdk::notify_due(&before, after) != due) {
            k3sm_test::fail(__FILE__, __LINE__, "notify_due disagrees on " + field);
        }
        K3SM_CHECK(sdk::notify_due(nullptr, after));

        // The writer owns the generation: a generation change is a bump.
        K3SM_CHECK(w->publish(before).has_value());
        const std::uint64_t e0 = w->epoch();
        if (field == "generation") {
            w->bump_generation();
        }
        K3SM_CHECK(w->publish(after).has_value());
        if ((w->epoch() != e0) != due) {
            k3sm_test::fail(__FILE__, __LINE__, "the writer's epoch bump disagrees on " + field);
        }
    }
    ::unlink(page.c_str());
}

void test_live_page() {
    const std::string page = page_path("live.page");
    const unlinker guard{page};
    const std::string lock = lock_path("live.lock");
    const std::string want = k3sm_test::read_file(k3sm_test::fixture("darwin.v1.page"));
    const sdk::snapshot darwin = golden("darwin.v1.page");

    auto w = sdk::page_writer::open(page.c_str(), lock.c_str());
    K3SM_CHECK(w.has_value());
    if (!w) {
        return;
    }
    struct stat st {};
    K3SM_CHECK(::stat(page.c_str(), &st) == 0);
    K3SM_CHECK_EQ(static_cast<std::size_t>(st.st_size), sdk::page_size);
    const ino_t inode = st.st_ino;
    K3SM_CHECK_EQ(w->generation(), 1);
    K3SM_CHECK_EQ(w->seq(), 0u);
    K3SM_CHECK_EQ(w->epoch(), 0u);

    // One writer per page.
    auto second = sdk::page_writer::open(page.c_str(), lock.c_str());
    K3SM_CHECK(!second && second.error() == sdk::error::writer_busy);
    // The lock never lives in the page directory.
    auto inside = sdk::page_writer::open(page.c_str(), page_path("live.lock").c_str());
    K3SM_CHECK(!inside && inside.error() == sdk::error::invalid_argument);

    const auto s1 = w->publish(darwin);
    K3SM_CHECK(s1.has_value() && *s1 == 2u);
    K3SM_CHECK_EQ(w->epoch(), 1u);
    // The page is the golden but for seq, notifyEpoch and the writer's generation.
    std::string got = k3sm_test::read_file(page);
    K3SM_CHECK_EQ(get_u64(got, sdk::page_offset::seq), 2u);
    K3SM_CHECK_EQ(get_u64(got, sdk::page_offset::notify_epoch), 1u);
    K3SM_CHECK_EQ(get_u64(got, sdk::page_offset::generation), 1u);
    put_u64(got, sdk::page_offset::seq, 0);
    put_u64(got, sdk::page_offset::notify_epoch, 0);
    put_u64(got, sdk::page_offset::generation, 4);
    K3SM_CHECK_EQ(first_difference(got, want), -1);

    // A value change is published without a bump; an invalid snapshot is
    // refused before any store.
    sdk::snapshot next = darwin;
    next.signals[0].value = 1;
    K3SM_CHECK(w->publish(next).has_value() && w->seq() == 4u && w->epoch() == 1u);
    sdk::snapshot bad = darwin;
    bad.node_name_len = sdk::max_node_name_bytes + 1;
    const auto refused = w->publish(bad);
    K3SM_CHECK(!refused && refused.error() == sdk::error::node_name_too_long);
    K3SM_CHECK_EQ(w->seq(), 4u);
    // A policy change bumps the generation and the epoch.
    w->bump_generation();
    K3SM_CHECK(w->publish(next).has_value() && w->generation() == 2 && w->epoch() == 2u);

    // A reader that mapped the page before the writer restarts keeps
    // seeing it: the restart neither renames nor truncates the page.
    auto reader = sdk::budget_page::open(page.c_str());
    K3SM_CHECK(reader.has_value());
    {
        sdk::page_writer stopped = std::move(*w);  // released at the end of the scope
    }
    auto again = sdk::page_writer::open(page.c_str(), lock.c_str());
    K3SM_CHECK(again.has_value());
    if (!again || !reader) {
        return;
    }
    K3SM_CHECK_EQ(again->generation(), 3);  // the page's 2, plus the restart
    K3SM_CHECK_EQ(again->seq(), 6u);
    K3SM_CHECK_EQ(again->epoch(), 2u);
    K3SM_CHECK(again->publish(next).has_value());
    K3SM_CHECK_EQ(again->seq(), 8u);
    K3SM_CHECK_EQ(again->epoch(), 3u);  // the first publication is always due
    const auto seen = reader->read_at(observed);
    K3SM_CHECK(seen.has_value());
    if (seen) {
        K3SM_CHECK_EQ(seen->generation, 3);
        K3SM_CHECK_EQ(seen->seq, 8u);
        K3SM_CHECK_EQ(seen->notify_epoch, 3u);
        K3SM_CHECK_EQ(seen->node_name(), std::string_view("mac-worker-1"));
    }
    K3SM_CHECK(::stat(page.c_str(), &st) == 0);
    K3SM_CHECK_EQ(st.st_ino, inode);
    K3SM_CHECK_EQ(static_cast<std::size_t>(st.st_size), sdk::page_size);
    ::unlink(page.c_str());
}

// A writer process that died without closing left its lock file behind: the
// lock went with the process, so the next writer opens the page.
void test_lock_left_by_dead_writer() {
    const std::string page = page_path("dead.page");
    const std::string lock = lock_path("dead.lock");
    const unlinker guard{page, lock};
    int ready[2];
    int release[2];
    if (::pipe(ready) != 0) {
        k3sm_test::fail(__FILE__, __LINE__, "pipe");
        return;
    }
    if (::pipe(release) != 0) {
        k3sm_test::fail(__FILE__, __LINE__, "pipe");
        ::close(ready[0]);
        ::close(ready[1]);
        return;
    }
    const pid_t child = ::fork();
    if (child < 0) {
        k3sm_test::fail(__FILE__, __LINE__, "fork");
        for (const int fd : {ready[0], ready[1], release[0], release[1]}) {
            ::close(fd);
        }
        return;
    }
    if (child == 0) {
        ::close(ready[0]);
        ::close(release[1]);
        const sdk::snapshot darwin = golden("darwin.v1.page");
        auto w = sdk::page_writer::open(page.c_str(), lock.c_str());
        const bool held = w.has_value() && w->publish(darwin).has_value();
        const char ok = held ? '1' : '0';
        (void)!::write(ready[1], &ok, 1);
        char c = 0;
        (void)!::read(release[0], &c, 1);
        ::_exit(held ? 0 : 1);  // no destructor runs: the writer is never closed
    }
    ::close(ready[1]);
    ::close(release[0]);
    char ok = 0;
    K3SM_CHECK(::read(ready[0], &ok, 1) == 1 && ok == '1');
    ::close(ready[0]);
    // While the child lives, it holds the lock.
    auto busy = sdk::page_writer::open(page.c_str(), lock.c_str());
    K3SM_CHECK(!busy && busy.error() == sdk::error::writer_busy);
    ::close(release[1]);
    int status = 0;
    K3SM_CHECK(::waitpid(child, &status, 0) == child);
    K3SM_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    struct stat st {};
    K3SM_CHECK(::stat(lock.c_str(), &st) == 0);  // the lock file is still there
    auto w = sdk::page_writer::open(page.c_str(), lock.c_str());
    K3SM_CHECK(w.has_value());
    if (w) {
        K3SM_CHECK_EQ(w->generation(), 2);  // the dead writer's 1, plus the restart
        K3SM_CHECK_EQ(w->seq(), 2u);          // one publication by the dead writer
    }
}

// An open that fails on the doorbell after it took the lock and mapped the
// page releases both: the next open of the same page succeeds.
void test_open_fails_after_lock() {
    const std::string page = page_path("bellfail.page");
    const std::string lock = lock_path("bellfail.lock");
    const std::string bad_bell = page_path("missing-dir/budget.bell");
    const std::string bell = page_path("bellfail.bell");
    const unlinker guard{page, lock, bell};
    {
        auto failed = sdk::page_writer::open(page.c_str(), lock.c_str(), bad_bell.c_str());
        K3SM_CHECK(!failed && failed.error() == sdk::error::io);
        if (!failed) {
            K3SM_CHECK_EQ(failed.error().sys_errno(), ENOENT);
        }
    }
    // The page file the failed open created is there, preallocated.
    struct stat st {};
    K3SM_CHECK(::stat(page.c_str(), &st) == 0);
    K3SM_CHECK_EQ(static_cast<std::size_t>(st.st_size), sdk::page_size);

    auto w = sdk::page_writer::open(page.c_str(), lock.c_str(), bell.c_str());
    K3SM_CHECK(w.has_value());
    if (!w) {
        return;
    }
    K3SM_CHECK_EQ(w->generation(), 1);  // the failed open never took the page over
    K3SM_CHECK(w->publish(golden("darwin.v1.page")).has_value());
    K3SM_CHECK(::stat(bell.c_str(), &st) == 0 && st.st_size >= 1);
}

// open refuses a null page or lock path, and a page or lock path whose
// directory does not exist fails with io and ENOENT. None of the failing
// opens creates the page or the lock file.
void test_open_argument_and_path_errors() {
    const std::string page = page_path("openerr.page");
    const std::string lock = lock_path("openerr.lock");
    const unlinker guard{page, lock};
    struct stat st {};

    auto no_page = sdk::page_writer::open(nullptr, lock.c_str());
    K3SM_CHECK(!no_page && no_page.error() == sdk::error::invalid_argument);
    auto no_lock = sdk::page_writer::open(page.c_str(), nullptr);
    K3SM_CHECK(!no_lock && no_lock.error() == sdk::error::invalid_argument);

    const std::string missing_lock = lock_path("missing-dir/openerr.lock");
    auto lock_dir_missing = sdk::page_writer::open(page.c_str(), missing_lock.c_str());
    K3SM_CHECK(!lock_dir_missing && lock_dir_missing.error() == sdk::error::io);
    if (!lock_dir_missing) {
        K3SM_CHECK_EQ(lock_dir_missing.error().sys_errno(), ENOENT);
    }
    const std::string missing_page = page_path("missing-dir/openerr.page");
    auto page_dir_missing = sdk::page_writer::open(missing_page.c_str(), lock.c_str());
    K3SM_CHECK(!page_dir_missing && page_dir_missing.error() == sdk::error::io);
    if (!page_dir_missing) {
        K3SM_CHECK_EQ(page_dir_missing.error().sys_errno(), ENOENT);
    }

    K3SM_CHECK(::stat(page.c_str(), &st) != 0 && errno == ENOENT);
    K3SM_CHECK(::stat(lock.c_str(), &st) != 0 && errno == ENOENT);
}

// An empty or over-long page or lock path fails as io with its own errno
// (ENOENT or ENAMETOOLONG), never whatever an earlier call left in errno,
// and creates neither file.
void test_open_bad_path_errno() {
    const std::string page = page_path("badpath.page");
    const std::string lock = lock_path("badpath.lock");
    const unlinker guard{page, lock};
    const std::string long_page = page_dir() + "/" + std::string(PATH_MAX, 'p');
    const std::string long_lock = lock_dir() + "/" + std::string(PATH_MAX, 'l');
    constexpr int sentinel = 777;

    struct bad_case {
        const char* name;
        const char* page;
        const char* lock;
        int want;
    };
    const bad_case cases[] = {
        {"empty page path", "", lock.c_str(), ENOENT},
        {"over-long page path", long_page.c_str(), lock.c_str(), ENAMETOOLONG},
        {"empty lock path", page.c_str(), "", ENOENT},
        {"over-long lock path", page.c_str(), long_lock.c_str(), ENAMETOOLONG},
    };
    for (const bad_case& c : cases) {
        errno = sentinel;
        auto w = sdk::page_writer::open(c.page, c.lock);
        K3SM_CHECK(!w && w.error() == sdk::error::io);
        if (!w) {
            if (w.error().sys_errno() != c.want) {
                std::fprintf(stderr, "test_open_bad_path_errno: %s: sys_errno %d, want %d\n", c.name,
                             w.error().sys_errno(), c.want);
            }
            K3SM_CHECK_EQ(w.error().sys_errno(), c.want);
        }
        struct stat st {};
        K3SM_CHECK(::stat(page.c_str(), &st) != 0 && errno == ENOENT);
        K3SM_CHECK(::stat(lock.c_str(), &st) != 0 && errno == ENOENT);
        K3SM_CHECK(::stat(long_page.c_str(), &st) != 0);
        K3SM_CHECK(::stat(long_lock.c_str(), &st) != 0);
    }
}

// An open that fails at the lock open or at the page open, after the lock
// was taken, leaves no lock behind: a later open on the same lock path
// succeeds instead of failing with writer_busy. The lock path and the page
// path each name a directory in turn, and a page file without permissions
// fails with EACCES (skipped as root, which ignores the mode).
void test_open_failure_releases_the_lock() {
    const std::string page = page_path("relfail.page");
    const std::string lock = lock_path("relfail.lock");
    const std::string fresh_lock = lock_path("relfail-fresh.lock");
    const std::string lock_as_dir = lock_path("relfail-lockdir");
    const std::string page_as_dir = page_path("relfail-pagedir");
    const std::string closed_page = page_path("relfail-closed.page");
    const unlinker guard{page, lock, fresh_lock, closed_page};
    K3SM_CHECK(::mkdir(lock_as_dir.c_str(), 0700) == 0);
    K3SM_CHECK(::mkdir(page_as_dir.c_str(), 0755) == 0);

    // (a) The lock path is a directory: the lock open fails, before any lock.
    {
        auto failed = sdk::page_writer::open(page.c_str(), lock_as_dir.c_str());
        K3SM_CHECK(!failed && failed.error() == sdk::error::io);
        if (!failed) {
            K3SM_CHECK_EQ(failed.error().sys_errno(), EISDIR);
        }
        auto w = sdk::page_writer::open(page.c_str(), fresh_lock.c_str());
        K3SM_CHECK(w.has_value());
    }

    // (b) The page path is a directory: the page open fails after the lock
    // was taken, and the same lock opens again.
    {
        auto failed = sdk::page_writer::open(page_as_dir.c_str(), lock.c_str());
        K3SM_CHECK(!failed && failed.error() == sdk::error::io);
        if (!failed) {
            K3SM_CHECK_EQ(failed.error().sys_errno(), EISDIR);
        }
        auto w = sdk::page_writer::open(page.c_str(), lock.c_str());
        K3SM_CHECK(w.has_value());
        if (!w) {
            K3SM_CHECK(w.error() != sdk::error::writer_busy);
        }
    }

    // (c) A page file with mode 0000: the page open fails with EACCES after
    // the lock was taken, and the same lock opens again.
    if (::geteuid() != 0) {
        const int fd = ::open(closed_page.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        K3SM_CHECK(fd >= 0);
        if (fd >= 0) {
            ::close(fd);
            K3SM_CHECK(::chmod(closed_page.c_str(), 0000) == 0);
            {
                auto failed = sdk::page_writer::open(closed_page.c_str(), lock.c_str());
                K3SM_CHECK(!failed && failed.error() == sdk::error::io);
                if (!failed) {
                    K3SM_CHECK_EQ(failed.error().sys_errno(), EACCES);
                }
                auto w = sdk::page_writer::open(page.c_str(), lock.c_str());
                K3SM_CHECK(w.has_value());
                if (!w) {
                    K3SM_CHECK(w.error() != sdk::error::writer_busy);
                }
            }
            K3SM_CHECK(::chmod(closed_page.c_str(), 0600) == 0);
        }
    } else {
        std::fprintf(stderr, "skip: the mode-0000 page case needs a non-root user\n");
    }

    ::rmdir(lock_as_dir.c_str());
    ::rmdir(page_as_dir.c_str());
}

// Move assignment releases the target's own lock and mapping and takes the
// source's: the target's old page opens again, the source's page stays held,
// and the target carries the source's seq and generation. A self-move is a
// no-op.
void test_move_assignment() {
    const std::string page1 = page_path("move1.page");
    const std::string lock1 = lock_path("move1.lock");
    const std::string page2 = page_path("move2.page");
    const std::string lock2 = lock_path("move2.lock");
    const unlinker guard{page1, lock1, page2, lock2};
    const sdk::snapshot darwin = golden("darwin.v1.page");

    auto a = sdk::page_writer::open(page1.c_str(), lock1.c_str());
    auto b = sdk::page_writer::open(page2.c_str(), lock2.c_str());
    K3SM_CHECK(a.has_value() && b.has_value());
    if (!a || !b) {
        return;
    }
    K3SM_CHECK(a->publish(darwin).has_value());
    K3SM_CHECK(b->publish(darwin).has_value());
    b->bump_generation();
    K3SM_CHECK(b->publish(darwin).has_value());
    const std::uint64_t b_seq = b->seq();
    const std::int64_t b_generation = b->generation();
    K3SM_CHECK(b_seq != a->seq() && b_generation != a->generation());

    *a = std::move(*b);
    {
        auto reopened = sdk::page_writer::open(page1.c_str(), lock1.c_str());
        K3SM_CHECK(reopened.has_value());
    }
    auto busy = sdk::page_writer::open(page2.c_str(), lock2.c_str());
    K3SM_CHECK(!busy && busy.error() == sdk::error::writer_busy);
    K3SM_CHECK_EQ(a->seq(), b_seq);
    K3SM_CHECK_EQ(a->generation(), b_generation);

    // The moved-to writer publishes into the source's page.
    K3SM_CHECK(a->publish(darwin).has_value());
    K3SM_CHECK_EQ(get_u64(k3sm_test::read_file(page2), sdk::page_offset::seq), b_seq + 2);

    // A self-move keeps the lock, the mapping and the counters.
    sdk::page_writer& same = *a;
    *a = std::move(same);
    auto still_busy = sdk::page_writer::open(page2.c_str(), lock2.c_str());
    K3SM_CHECK(!still_busy && still_busy.error() == sdk::error::writer_busy);
    K3SM_CHECK_EQ(a->seq(), b_seq + 2);
    K3SM_CHECK(a->publish(darwin).has_value());
    K3SM_CHECK_EQ(get_u64(k3sm_test::read_file(page2), sdk::page_offset::seq), b_seq + 4);
}

// A publication that is not due leaves the doorbell untouched; a due one
// writes it.
void test_doorbell_silent_when_not_due() {
    const std::string page = page_path("quietbell.page");
    const std::string lock = lock_path("quietbell.lock");
    const std::string bell = page_path("quietbell.bell");
    const unlinker guard{page, lock, bell};
    auto w = sdk::page_writer::open(page.c_str(), lock.c_str(), bell.c_str());
    K3SM_CHECK(w.has_value());
    if (!w) {
        return;
    }
    const sdk::snapshot darwin = golden("darwin.v1.page");
    K3SM_CHECK(w->publish(darwin).has_value());
    struct stat st {};
    K3SM_CHECK(::stat(bell.c_str(), &st) == 0 && st.st_size >= 1);
    K3SM_CHECK(::truncate(bell.c_str(), 0) == 0);
    const std::uint64_t e0 = w->epoch();

    sdk::snapshot quiet = darwin;
    quiet.signals[0].value = 5;
    K3SM_CHECK(!sdk::notify_due(&darwin, quiet));
    K3SM_CHECK(w->publish(quiet).has_value());
    K3SM_CHECK(::stat(bell.c_str(), &st) == 0);
    K3SM_CHECK_EQ(static_cast<long long>(st.st_size), 0LL);
    K3SM_CHECK_EQ(w->epoch(), e0);

    sdk::snapshot loud = quiet;
    loud.level = sdk::level::warn;
    K3SM_CHECK(sdk::notify_due(&quiet, loud));
    K3SM_CHECK(w->publish(loud).has_value());
    K3SM_CHECK(::stat(bell.c_str(), &st) == 0 && st.st_size >= 1);
    K3SM_CHECK_EQ(w->epoch(), e0 + 1);
}

// A writer that died mid-write left seq odd: the next writer moves past it.
void test_resume_odd_seq() {
    const std::string page = page_path("odd.page");
    const unlinker guard{page};
    std::string bytes = k3sm_test::read_file(k3sm_test::fixture("darwin.v1.page"));
    put_u64(bytes, sdk::page_offset::seq, 41);
    put_u64(bytes, sdk::page_offset::notify_epoch, 7);
    {
        const int fd = ::open(page.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        K3SM_CHECK(fd >= 0 && ::write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
        ::close(fd);
    }
    auto w = sdk::page_writer::open(page.c_str(), lock_path("odd.lock").c_str());
    K3SM_CHECK(w.has_value());
    if (!w) {
        return;
    }
    K3SM_CHECK_EQ(w->seq(), 42u);
    K3SM_CHECK_EQ(w->epoch(), 7u);
    K3SM_CHECK_EQ(w->generation(), 5);
    K3SM_CHECK(w->publish(golden("darwin.v1.page")).has_value());
    K3SM_CHECK_EQ(w->seq(), 44u);
    K3SM_CHECK_EQ(w->epoch(), 8u);
    ::unlink(page.c_str());
}

// A page of this ABI whose generation word reads as negative (as a signed
// 64-bit value) cannot be continued: the next writer starts at generation 1.
void test_resume_negative_generation() {
    const std::string page = page_path("neggen.page");
    const unlinker guard{page};
    for (const std::int64_t stored : {std::int64_t{-1}, std::int64_t{-5}, INT64_MIN}) {
        std::string bytes = k3sm_test::read_file(k3sm_test::fixture("darwin.v1.page"));
        put_u64(bytes, sdk::page_offset::seq, 10);
        put_u64(bytes, sdk::page_offset::notify_epoch, 3);
        put_u64(bytes, sdk::page_offset::generation, static_cast<std::uint64_t>(stored));
        {
            const int fd = ::open(page.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            K3SM_CHECK(fd >= 0 && ::write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
            ::close(fd);
        }
        auto w = sdk::page_writer::open(page.c_str(), lock_path("neggen.lock").c_str());
        K3SM_CHECK(w.has_value());
        if (!w) {
            continue;
        }
        if (w->generation() != 1) {
            std::fprintf(stderr, "stored generation %lld: resumed at %lld, want 1\n",
                         static_cast<long long>(stored), static_cast<long long>(w->generation()));
            K3SM_CHECK(false);
        }
        // The page is of this ABI, so seq and notifyEpoch still carry over.
        K3SM_CHECK_EQ(w->seq(), 10u);
        K3SM_CHECK_EQ(w->epoch(), 3u);
        K3SM_CHECK(w->publish(golden("darwin.v1.page")).has_value());
        K3SM_CHECK_EQ(get_u64(k3sm_test::read_file(page), sdk::page_offset::generation), 1u);
    }
}

// A stored generation with no successor (INT64_MAX) restarts at 1 instead of
// overflowing; INT64_MAX-1 resumes at INT64_MAX, and one bump from there
// restarts at 1.
void test_resume_max_generation() {
    const std::string page = page_path("maxgen.page");
    const unlinker guard{page};
    constexpr std::int64_t max = INT64_MAX;
    const struct {
        std::int64_t stored;
        std::int64_t want;
    } cases[] = {{max, 1}, {max - 1, max}};
    for (const auto& c : cases) {
        std::string bytes = k3sm_test::read_file(k3sm_test::fixture("darwin.v1.page"));
        put_u64(bytes, sdk::page_offset::generation, static_cast<std::uint64_t>(c.stored));
        {
            const int fd = ::open(page.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            K3SM_CHECK(fd >= 0 && ::write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
            ::close(fd);
        }
        auto w = sdk::page_writer::open(page.c_str(), lock_path("maxgen.lock").c_str());
        K3SM_CHECK(w.has_value());
        if (!w) {
            continue;
        }
        if (w->generation() != c.want) {
            std::fprintf(stderr, "stored generation %lld: resumed at %lld, want %lld\n",
                         static_cast<long long>(c.stored), static_cast<long long>(w->generation()),
                         static_cast<long long>(c.want));
            K3SM_CHECK(false);
        }
        if (c.want == max) {
            w->bump_generation();
            if (w->generation() != 1) {
                std::fprintf(stderr, "bump from INT64_MAX gave %lld, want 1\n",
                             static_cast<long long>(w->generation()));
                K3SM_CHECK(false);
            }
        }
    }
}

constexpr std::int64_t gen_max = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t gen_min = std::numeric_limits<std::int64_t>::min();
static_assert(sdk::detail::next_generation(0) == 1);
static_assert(sdk::detail::next_generation(1) == 2);
static_assert(sdk::detail::next_generation(gen_max - 1) == gen_max);
static_assert(sdk::detail::next_generation(gen_max) == 1);
static_assert(sdk::detail::next_generation(-1) == 1);
static_assert(sdk::detail::next_generation(gen_min) == 1);

// One bump on a fresh page raises the generation by exactly one.
void test_bump_generation_by_one() {
    const std::string page = page_path("bump1.page");
    const std::string lock = lock_path("bump1.lock");
    const unlinker guard{page, lock};
    auto w = sdk::page_writer::open(page.c_str(), lock.c_str());
    K3SM_CHECK(w.has_value());
    if (!w) {
        return;
    }
    const std::int64_t before = w->generation();
    w->bump_generation();
    K3SM_CHECK_EQ(w->generation(), before + 1);
}

// Sets the process umask for its lifetime and restores the previous one when
// it leaves scope, an early return included.
struct umask_guard {
    mode_t previous;
    explicit umask_guard(mode_t m) : previous(::umask(m)) {}
    umask_guard(const umask_guard&) = delete;
    umask_guard& operator=(const umask_guard&) = delete;
    ~umask_guard() { ::umask(previous); }
};

// With no umask to narrow them, open creates the lock file 0600 and the page
// and doorbell files 0644.
void test_open_file_modes() {
    const std::string page = page_path("modes.page");
    const std::string lock = lock_path("modes.lock");
    const std::string bell = page_path("modes.bell");
    const unlinker guard{page, lock, bell};
    const umask_guard mask(0);
    auto w = sdk::page_writer::open(page.c_str(), lock.c_str(), bell.c_str());
    K3SM_CHECK(w.has_value());
    if (!w) {
        return;
    }
    struct mode_case {
        const std::string& path;
        mode_t want;
    };
    const mode_case cases[] = {{lock, 0600}, {page, 0644}, {bell, 0644}};
    for (const mode_case& c : cases) {
        struct stat st {};
        K3SM_CHECK(::stat(c.path.c_str(), &st) == 0);
        const mode_t got = st.st_mode & 0777;
        if (got != c.want) {
            std::fprintf(stderr, "%s: mode %03o, want %03o\n", c.path.c_str(), static_cast<unsigned>(got),
                         static_cast<unsigned>(c.want));
            K3SM_CHECK(false);
        }
    }
}

// versions_word is the word holding the major, the minor and the header size.
std::uint64_t versions_word(std::uint16_t minor) {
    return std::uint64_t{sdk::page_abi_major} | std::uint64_t{minor} << 16 |
           std::uint64_t{sdk::page_header_size} << 32;
}

// A reader that loads the header between any two of a resume's stores sees
// an abiMinor and a bodySize that agree: the body size is at least that of
// the minor. Readers check the header before the seqlock copy, so the
// guard is the store order, not seq. The reader here knows a hypothetical
// minor 2 that appends four words, as a newer SDK would; a rollback from it
// (and from a page whose body is larger than any known minor's) stores the
// minor word first, an upgrade the body size word.
void test_resume_header_store_order() {
    const std::uint32_t body_of[] = {sdk::page_body_size_minor0, sdk::page_body_size,
                                     sdk::page_body_size + 32};
    struct resume_case {
        const char* name;
        std::uint16_t minor;
        std::uint32_t body;
    };
    const resume_case cases[] = {
        {"rollback from a newer minor", 2, body_of[2]},
        {"rollback from a body larger than any known minor", 2, 3000},
        {"upgrade from minor 0", 0, body_of[0]},
        {"same minor", sdk::page_abi_minor, body_of[sdk::page_abi_minor]},
    };
    std::string fresh(sdk::page_size, '\0');
    put_u64(fresh, sdk::page_offset::magic, sdk::detail::page_magic_word());
    put_u64(fresh, sdk::page_offset::abi_major, versions_word(sdk::page_abi_minor));
    put_u64(fresh, sdk::page_offset::body_size, sdk::page_body_size);
    for (const resume_case& c : cases) {
        std::string page = k3sm_test::read_file(k3sm_test::fixture("darwin.v1.page"));
        page.resize(sdk::page_size, '\0');
        put_u64(page, sdk::page_offset::abi_major, versions_word(c.minor));
        put_u64(page, sdk::page_offset::body_size, c.body);

        // Apply the resume's stores one at a time, reading the header after each.
        const bool shrinks = sdk::detail::resume_shrinks(c.body);
        int stores = 0;
        for (const std::size_t o : sdk::detail::resume_store_order(shrinks)) {
            put_u64(page, o, get_u64(fresh, o));
            ++stores;
            const auto minor = static_cast<std::uint16_t>(get_u64(page, sdk::page_offset::abi_major) >> 16);
            const auto body = static_cast<std::uint32_t>(get_u64(page, sdk::page_offset::body_size));
            if (minor != c.minor && minor != sdk::page_abi_minor) {
                std::fprintf(stderr, "%s: store %d: abiMinor %u is neither the old nor the new minor\n",
                             c.name, stores, minor);
                K3SM_CHECK(false);
            } else if (body < body_of[minor]) {
                std::fprintf(stderr, "%s: store %d: abiMinor %u with bodySize %u, below its %u\n", c.name,
                             stores, minor, body, body_of[minor]);
                K3SM_CHECK(false);
            }
        }
        K3SM_CHECK_EQ(static_cast<std::size_t>(stores), sdk::detail::header_words);
        K3SM_CHECK(page.compare(0, sdk::page_offset::seq, fresh, 0, sdk::page_offset::seq) == 0);
    }
    // Every header word is stored once, and the magic last.
    for (const bool shrinks : {false, true}) {
        const auto order = sdk::detail::resume_store_order(shrinks);
        bool seen[sdk::detail::header_words] = {};
        for (const std::size_t o : order) {
            K3SM_CHECK(o % 8 == 0 && o < sdk::page_offset::seq && !seen[o / 8]);
            seen[o / 8] = true;
        }
        K3SM_CHECK_EQ(order.back(), sdk::page_offset::magic);
    }
    // The flag resume derives from the existing bodySize, and the order it picks:
    // a smaller or equal body stores the bodySize word first, a larger one the
    // minor word first. Only the low 32 bits of the word are the bodySize.
    constexpr std::size_t minor_word = sdk::page_offset::abi_minor & ~std::size_t{7};
    constexpr std::size_t size_word = sdk::page_offset::body_size & ~std::size_t{7};
    constexpr std::size_t first = sdk::detail::header_words - 3;
    struct shrink_case {
        const char* name;
        std::uint64_t word;
        bool shrinks;
    };
    const shrink_case shrink_cases[] = {
        {"smaller body", sdk::page_body_size - 8, false},
        {"equal body", sdk::page_body_size, false},
        {"larger body", sdk::page_body_size + 8, true},
        {"high bits ignored", std::uint64_t{1} << 32 | sdk::page_body_size, false},
        {"zero body", 0, false},
        {"top bit of the low word", 0xFFFFFFFFull, true},
    };
    for (const shrink_case& c : shrink_cases) {
        const bool shrinks = sdk::detail::resume_shrinks(c.word);
        if (shrinks != c.shrinks) {
            std::fprintf(stderr, "%s: resume_shrinks is %d, want %d\n", c.name, shrinks, c.shrinks);
            K3SM_CHECK(false);
        }
        const auto order = sdk::detail::resume_store_order(shrinks);
        K3SM_CHECK_EQ(order[first], c.shrinks ? minor_word : size_word);
        K3SM_CHECK_EQ(order[first + 1], c.shrinks ? size_word : minor_word);
    }
}

// A writer resuming a page of a newer minor with a larger body takes it
// over: the header is this minor's, and the page reads.
void test_resume_rollback_page() {
    const std::string page = page_path("rollback.page");
    const unlinker guard{page};
    std::string bytes = k3sm_test::read_file(k3sm_test::fixture("darwin.v1.page"));
    bytes.resize(sdk::page_size, '\0');
    put_u64(bytes, sdk::page_offset::abi_major, versions_word(2));
    put_u64(bytes, sdk::page_offset::body_size, sdk::page_body_size + 32);
    {
        const int fd = ::open(page.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        K3SM_CHECK(fd >= 0 && ::write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
        ::close(fd);
    }
    auto w = sdk::page_writer::open(page.c_str(), lock_path("rollback.lock").c_str());
    K3SM_CHECK(w.has_value());
    if (!w) {
        return;
    }
    const std::string after = k3sm_test::read_file(page);
    K3SM_CHECK_EQ(get_u64(after, sdk::page_offset::abi_major), versions_word(sdk::page_abi_minor));
    K3SM_CHECK_EQ(get_u64(after, sdk::page_offset::body_size), std::uint64_t{sdk::page_body_size});
    K3SM_CHECK(w->publish(golden("darwin.v1.page")).has_value());
    const auto r = read_path(page);
    K3SM_CHECK(r.has_value() && r->abi_minor == sdk::page_abi_minor);
    ::unlink(page.c_str());
}

// A page with an intact magic but a foreign major or header size is not a
// page of this ABI: a reader refuses it until the resume's word-8 store, and
// the full resume starts over (seq 0, epoch 0, and generation 1: the old page's
// generation is not read, so it does not carry over).
void test_resume_foreign_header_with_intact_magic() {
    struct foreign_case {
        const char* name;
        std::uint64_t versions;
    };
    const foreign_case cases[] = {
        {"foreign abiMajor", versions_word(sdk::page_abi_minor) + 1},
        {"foreign headerSize",
         versions_word(sdk::page_abi_minor) + (std::uint64_t{8} << 32)},
    };
    std::string fresh(sdk::page_size, '\0');
    put_u64(fresh, sdk::page_offset::magic, sdk::detail::page_magic_word());
    put_u64(fresh, sdk::page_offset::abi_major, versions_word(sdk::page_abi_minor));
    put_u64(fresh, sdk::page_offset::body_size, sdk::page_body_size);
    const std::size_t word8 = sdk::page_offset::abi_major;
    for (const foreign_case& c : cases) {
        std::string bytes = k3sm_test::read_file(k3sm_test::fixture("darwin.v1.page"));
        bytes.resize(sdk::page_size, '\0');
        put_u64(bytes, sdk::page_offset::abi_major, c.versions);
        put_u64(bytes, sdk::page_offset::seq, 40);
        put_u64(bytes, sdk::page_offset::notify_epoch, 7);
        put_u64(bytes, sdk::page_offset::generation, 9);

        // Stage by hand: each store of the resume order, the header read after each.
        const std::string page = page_path("foreign-staged.page");
        bool recognised = false;
        std::string staged = bytes;
        for (const std::size_t o : sdk::detail::resume_store_order(false)) {
            put_u64(staged, o, get_u64(fresh, o));
            recognised = recognised || o == word8;
            const int fd = ::open(page.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            K3SM_CHECK(fd >= 0 && ::write(fd, staged.data(), staged.size()) == static_cast<ssize_t>(staged.size()));
            ::close(fd);
            const auto r = read_path(page);
            if (recognised) {
                if (!r.has_value()) {
                    std::fprintf(stderr, "%s: store at %zu: not valid after the word-8 store\n", c.name, o);
                }
                K3SM_CHECK(r.has_value());
            } else {
                K3SM_CHECK(!r.has_value() && r.error() == sdk::error::abi_mismatch);
            }
        }
        K3SM_CHECK(recognised);
        ::unlink(page.c_str());

        // The real resume over the same page.
        const std::string live = page_path("foreign.page");
        {
            const int fd = ::open(live.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            K3SM_CHECK(fd >= 0 && ::write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()));
            ::close(fd);
        }
        auto w = sdk::page_writer::open(live.c_str(), lock_path("foreign.lock").c_str());
        K3SM_CHECK(w.has_value());
        if (!w) {
            ::unlink(live.c_str());
            continue;
        }
        K3SM_CHECK_EQ(w->seq(), 0u);
        K3SM_CHECK_EQ(w->epoch(), 0u);
        K3SM_CHECK_EQ(w->generation(), 1);
        K3SM_CHECK(w->publish(golden("darwin.v1.page")).has_value());
        K3SM_CHECK_EQ(get_u64(k3sm_test::read_file(live), sdk::page_offset::generation), 1u);
        const auto r = read_path(live);
        K3SM_CHECK(r.has_value() && r->abi_minor == sdk::page_abi_minor);
        ::unlink(live.c_str());
    }
}

// A short file that is not a page grows in place to page_size and starts over.
void test_short_file_grows_in_place() {
    const std::string page = page_path("short.page");
    const unlinker guard{page};
    {
        const int fd = ::open(page.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        const char junk[100] = {'j', 'u', 'n', 'k'};
        K3SM_CHECK(fd >= 0 && ::write(fd, junk, sizeof junk) == static_cast<ssize_t>(sizeof junk));
        ::close(fd);
    }
    struct stat before {};
    K3SM_CHECK(::stat(page.c_str(), &before) == 0);
    auto w = sdk::page_writer::open(page.c_str(), lock_path("short.lock").c_str());
    K3SM_CHECK(w.has_value());
    if (!w) {
        return;
    }
    struct stat after {};
    K3SM_CHECK(::stat(page.c_str(), &after) == 0);
    K3SM_CHECK_EQ(after.st_ino, before.st_ino);
    K3SM_CHECK_EQ(static_cast<std::size_t>(after.st_size), sdk::page_size);
    K3SM_CHECK_EQ(w->generation(), 1);
    K3SM_CHECK_EQ(w->seq(), 0u);
    K3SM_CHECK(w->publish(golden("linux-producer.v1.page")).has_value());
    const auto r = read_path(page);
    K3SM_CHECK(r.has_value() && r->platform == sdk::platform::linux_ && r->generation == 1);
    ::unlink(page.c_str());
}

// A due publication wakes a waiter on notifyEpoch and rings the doorbell; a
// publication that is not due does neither.
void test_wake_and_doorbell() {
    const std::string page = page_path("wake.page");
    const std::string bell = page_path("budget.bell");
    const unlinker guard{page, bell};
    auto w = sdk::page_writer::open(page.c_str(), lock_path("wake.lock").c_str(), bell.c_str());
    K3SM_CHECK(w.has_value());
    if (!w) {
        return;
    }
    const sdk::snapshot darwin = golden("darwin.v1.page");
    K3SM_CHECK(w->publish(darwin).has_value());
    auto reader = sdk::budget_page::open(page.c_str());
    K3SM_CHECK(reader.has_value());
    if (!reader) {
        return;
    }
    sdk::change_notifier n(*reader);
    K3SM_CHECK_EQ(n.epoch(), w->epoch());

    sdk::snapshot quiet = darwin;
    quiet.signals[0].value = 5;
    K3SM_CHECK(w->publish(quiet).has_value());
    K3SM_CHECK(!n.poll());

    bool woke = false;
    std::thread waiter([&] { woke = n.wait(5s); });
    std::this_thread::sleep_for(20ms);
    sdk::snapshot loud = darwin;
    loud.level = sdk::level::warn;
    const auto t0 = std::chrono::steady_clock::now();
    K3SM_CHECK(w->publish(loud).has_value());
    waiter.join();
    K3SM_CHECK(woke);
    K3SM_CHECK(std::chrono::steady_clock::now() - t0 < 2s);
    K3SM_CHECK_EQ(n.epoch(), w->epoch());

    struct stat st {};
    K3SM_CHECK(::stat(bell.c_str(), &st) == 0 && st.st_size >= 1);
    ::unlink(page.c_str());
    ::unlink(bell.c_str());
}

// A publish the writer refuses leaves every byte of the page as it was, the
// seq word included (still even), and the next valid publish succeeds.
void test_refused_publish_leaves_page() {
    const std::string page = page_path("refused.page");
    const std::string lock = lock_path("refused.lock");
    const unlinker guard{page, lock};
    const sdk::snapshot darwin = golden("darwin.v1.page");
    auto w = sdk::page_writer::open(page.c_str(), lock.c_str());
    K3SM_CHECK(w.has_value());
    if (!w) {
        return;
    }
    K3SM_CHECK(w->publish(darwin).has_value());
    const std::string before = k3sm_test::read_file(page);
    sdk::snapshot bad = darwin;
    bad.node_name_len = sdk::max_node_name_bytes + 1;
    const auto refused = w->publish(bad);
    K3SM_CHECK(!refused && refused.error() == sdk::error::node_name_too_long);
    const std::string after = k3sm_test::read_file(page);
    K3SM_CHECK_EQ(first_difference(before, after), -1);
    K3SM_CHECK_EQ(get_u64(after, sdk::page_offset::seq) % 2, 0u);
    const auto next = w->publish(darwin);
    K3SM_CHECK(next.has_value() && *next == 4u);
}

// The writer's descriptors are close-on-exec: a child spawned while the
// writer is open and outliving it does not keep the lock, so a new writer
// opens on the same page and lock.
void test_descriptors_close_on_exec() {
    const std::string page = page_path("cloexec.page");
    const std::string lock = lock_path("cloexec.lock");
    const unlinker guard{page, lock};
    pid_t child = -1;
    {
        auto w = sdk::page_writer::open(page.c_str(), lock.c_str());
        K3SM_CHECK(w.has_value());
        if (!w) {
            return;
        }
        char sleep_path[] = "/bin/sleep";
        char five[] = "5";
        char* argv[] = {sleep_path, five, nullptr};
        if (::posix_spawn(&child, sleep_path, nullptr, nullptr, argv, nullptr) != 0) {
            k3sm_test::fail(__FILE__, __LINE__, "posix_spawn");
            return;
        }
    }
    // The writer is gone; the child still runs.
    auto again = sdk::page_writer::open(page.c_str(), lock.c_str());
    K3SM_CHECK(again.has_value());
    if (!again) {
        K3SM_CHECK(again.error() != sdk::error::writer_busy);
    }
    ::kill(child, SIGKILL);
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
}

// A page path with no slash names a file in the working directory.
void test_bare_page_name() {
    const std::string dir = base_dir() + "/bare";
    if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        k3sm_test::fail(__FILE__, __LINE__, "mkdir");
        return;
    }
    const std::string lock = lock_path("bare.lock");
    const unlinker guard{dir + "/bare.page", lock};
    const int cwd = ::open(".", O_RDONLY | O_CLOEXEC);
    if (cwd < 0) {
        k3sm_test::fail(__FILE__, __LINE__, "open .");
        return;
    }
    if (::chdir(dir.c_str()) != 0) {
        k3sm_test::fail(__FILE__, __LINE__, "chdir");
        ::close(cwd);
        return;
    }
    {
        auto w = sdk::page_writer::open("bare.page", lock.c_str());
        K3SM_CHECK(w.has_value());
        if (w) {
            K3SM_CHECK(w->publish(golden("darwin.v1.page")).has_value());
        }
    }
    K3SM_CHECK(::fchdir(cwd) == 0);
    ::close(cwd);
    ::unlink((dir + "/bare.page").c_str());
    ::rmdir(dir.c_str());
}

void cleanup() {
    for (const char* f : {"vectors.lock", "live.lock", "odd.lock", "rollback.lock", "short.lock", "wake.lock", "foreign.lock", "neggen.lock"}) {
        ::unlink(lock_path(f).c_str());
    }
    ::rmdir(page_dir().c_str());
    ::rmdir(lock_dir().c_str());
    ::rmdir(base_dir().c_str());
}

}  // namespace writer_test

int main() {
    writer_test::make_dirs();
    writer_test::test_golden_round_trip();
    writer_test::test_golden_absent_by_hand();
    writer_test::test_encode_minor1_fields();
    writer_test::test_encode_refusals();
    writer_test::test_encode_field_checks();
    writer_test::test_notify_vectors();
    writer_test::test_live_page();
    writer_test::test_lock_left_by_dead_writer();
    writer_test::test_open_fails_after_lock();
    writer_test::test_open_argument_and_path_errors();
    writer_test::test_open_bad_path_errno();
    writer_test::test_open_failure_releases_the_lock();
    writer_test::test_move_assignment();
    writer_test::test_doorbell_silent_when_not_due();
    writer_test::test_resume_odd_seq();
    writer_test::test_resume_negative_generation();
    writer_test::test_resume_max_generation();
    writer_test::test_bump_generation_by_one();
    writer_test::test_open_file_modes();
    writer_test::test_resume_header_store_order();
    writer_test::test_resume_rollback_page();
    writer_test::test_resume_foreign_header_with_intact_magic();
    writer_test::test_short_file_grows_in_place();
    writer_test::test_wake_and_doorbell();
    writer_test::test_refused_publish_leaves_page();
    writer_test::test_descriptors_close_on_exec();
    writer_test::test_bare_page_name();
    writer_test::cleanup();
    if (k3sm_test::failures() != 0) {
        std::fprintf(stderr, "writer_test: %d failure(s)\n", k3sm_test::failures());
        return 1;
    }
    std::puts("writer_test: ok");
    return 0;
}
