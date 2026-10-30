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

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
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

// A writer that died mid-write left seq odd: the next writer moves past it.
void test_resume_odd_seq() {
    const std::string page = page_path("odd.page");
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

// A short file that is not a page grows in place to page_size and starts over.
void test_short_file_grows_in_place() {
    const std::string page = page_path("short.page");
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

void cleanup() {
    for (const char* f : {"vectors.lock", "live.lock", "odd.lock", "short.lock", "wake.lock"}) {
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
    writer_test::test_encode_refusals();
    writer_test::test_notify_vectors();
    writer_test::test_live_page();
    writer_test::test_resume_odd_seq();
    writer_test::test_short_file_grows_in_place();
    writer_test::test_wake_and_doorbell();
    writer_test::cleanup();
    if (k3sm_test::failures() != 0) {
        std::fprintf(stderr, "writer_test: %d failure(s)\n", k3sm_test::failures());
        return 1;
    }
    std::puts("writer_test: ok");
    return 0;
}
