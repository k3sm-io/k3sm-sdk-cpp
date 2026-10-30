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

// The writer and the reader in two processes on one shared page. This
// binary spawns itself as the writer, which maps the page through
// page_writer and publishes as fast as it can for a fixed time; the parent
// maps the same file through budget_page and reads in a tight loop. Every
// published word is a function of the publication's number k (observedAt),
// so a snapshot mixing two publications is caught by checking every word
// against the one its observedAt names. The oracle: no accepted snapshot is
// ever mixed. As a control, the parent also copies the body without the
// seqlock and counts the mixed copies it sees, which shows the two
// processes really do race.

#include <k3sm/budget.hpp>
#include <k3sm/page_writer.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_util.hpp"

extern char** environ;

namespace sdk = k3sm::sdk;
using namespace std::chrono_literals;

namespace stress_test {

constexpr auto writer_runtime = 1500ms;

// last_word is the index publication() gives the body's last field.
constexpr std::int64_t last_word = 90;

// word returns the value field i of publication k holds.
constexpr std::int64_t word(std::int64_t k, std::int64_t i) { return k * 1000003 + i; }

// ladder returns the threshold value field i holds for publication k: it
// moves every 64 publications, so some publications are due and most not.
constexpr std::int64_t ladder(std::int64_t k, std::int64_t i) { return (k / 64) * 1009 + i; }

void fill_psi(sdk::psi_line& l, std::int64_t k, std::int64_t& i) {
    l.avg10_permille = word(k, i++);
    l.avg60_permille = word(k, i++);
    l.total_us = word(k, i++);
}

// publication returns snapshot k: a Linux producer's page with every slot
// filled and every field derived from k.
sdk::snapshot publication(std::int64_t k) {
    sdk::snapshot s;
    s.writer_period_seconds = std::int64_t{1} << 40;  // never stale
    s.observed_at = k;
    s.platform = sdk::platform::linux_;
    const char name[] = "stress-node";
    std::memcpy(s.node_name_raw.data(), name, sizeof name - 1);
    s.node_name_len = sizeof name - 1;
    s.level = static_cast<sdk::level>(1 + (k / 64) % 4);
    s.admit = (k / 64) % 2 == 0;
    std::int64_t i = 0;
    sdk::capacity& c = s.capacity;
    for (sdk::opt_i64* f : {&c.cpu_capacity_milli, &c.cpu_system_reserve_milli, &c.mem_capacity_bytes,
                            &c.mem_system_reserve_bytes, &c.mem_eviction_reserve_bytes,
                            &c.mem_allocatable_bytes, &c.swap_total_bytes, &c.swap_used_bytes}) {
        *f = word(k, i++);
    }
    sdk::linux_signals l;
    for (sdk::psi_resource* r : {&l.psi.cpu, &l.psi.memory, &l.psi.io}) {
        fill_psi(r->some, k, i);
        fill_psi(r->full, k, i);
    }
    l.memory_events = {word(k, i), word(k, i + 1), word(k, i + 2), word(k, i + 3)};
    i += 4;
    s.linux_ = l;
    for (std::size_t n = 0; n < sdk::max_signals; ++n) {
        sdk::signal_record& r = s.signals[n];
        r.id = static_cast<sdk::signal_id>(1 + n % 7);
        r.unit = sdk::unit::bytes;
        r.value = word(k, i++);
        std::int64_t j = static_cast<std::int64_t>(n) * 10;
        for (sdk::opt_i64* f : {&r.warn_threshold, &r.admit_stop_threshold, &r.critical_threshold,
                                &r.evict_soft_threshold, &r.evict_hard_threshold, &r.warn_release,
                                &r.admit_stop_release, &r.critical_release, &r.evict_soft_release,
                                &r.evict_hard_release}) {
            *f = ladder(k, j++);
        }
        r.grace_seconds = word(k, i++);
        r.min_reclaim_bytes = word(k, i++);
        r.window_seconds = word(k, i++);
        r.eta_to_next_rung_seconds = word(k, i++);
    }
    s.signal_count = sdk::max_signals;
    s.evict_deadline = word(k, i++);
    s.kill_mode.mode = sdk::kill_mode::on;
    s.kill_mode.arming_episodes = word(k, i++);
    s.recommendation.safe_additional_bytes = word(k, i++);
    for (std::size_t n = 0; n < sdk::max_counters; ++n) {
        sdk::band_counters& b = s.counters[n];
        b.band = static_cast<sdk::band>(1 + n);
        for (std::int64_t* f : {&b.evictions_total, &b.oom_kills_total, &b.kernel_kills_total,
                                &b.restarts_total, &b.mem_high_events_total, &b.tier1_refused_total}) {
            *f = word(k, i++);
        }
    }
    s.counter_count = sdk::max_counters;
    if (i - 1 != last_word) {
        std::abort();  // publication() and last_word disagree
    }
    return s;
}

// consistent reports whether s is exactly publication(s.observed_at), with
// the generation the writer stamped.
bool consistent(const sdk::snapshot& s, std::int64_t generation) {
    sdk::snapshot want = publication(s.observed_at);
    want.generation = generation;
    unsigned char a[sdk::page_size];
    unsigned char b[sdk::page_size];
    if (!sdk::encode_page(s, a, sizeof a) || !sdk::encode_page(want, b, sizeof b)) {
        return false;
    }
    return std::memcmp(a, b, sizeof a) == 0;
}

int writer_main(const char* page, const char* lock, int ready_fd) {
    auto w = sdk::page_writer::open(page, lock);
    if (!w) {
        return 3;
    }
    std::int64_t k = 1;
    if (!w->publish(publication(k))) {
        return 4;
    }
    const std::int64_t generation = w->generation();
    if (::write(ready_fd, &generation, sizeof generation) != sizeof generation) {
        return 5;
    }
    ::close(ready_fd);
    const auto deadline = std::chrono::steady_clock::now() + writer_runtime;
    while (std::chrono::steady_clock::now() < deadline) {
        for (int burst = 0; burst < 64; ++burst) {
            if (!w->publish(publication(++k))) {
                return 6;
            }
        }
    }
    std::printf("writer: %lld publications, %llu epoch bumps\n", static_cast<long long>(k),
                static_cast<unsigned long long>(w->epoch()));
    return 0;
}

int run(const char* self) {
    const std::string base = std::string(K3SM_SDK_TEST_TMPDIR) + "/writer_stress." + std::to_string(::getpid());
    const std::string page_dir = base + "/page";
    const std::string state_dir = base + "/state";
    ::mkdir(base.c_str(), 0755);
    ::mkdir(page_dir.c_str(), 0755);
    ::mkdir(state_dir.c_str(), 0700);
    const std::string page = page_dir + "/budget.v1.page";
    const std::string lock = state_dir + "/writer.lock";

    int ready[2];
    if (::pipe(ready) != 0) {
        return 2;
    }
    const std::string ready_fd = std::to_string(ready[1]);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addclose(&fa, ready[0]);
    char* argv[] = {const_cast<char*>(self), const_cast<char*>("writer"), const_cast<char*>(page.c_str()),
                    const_cast<char*>(lock.c_str()), const_cast<char*>(ready_fd.c_str()), nullptr};
    pid_t pid = -1;
    const int spawned = posix_spawn(&pid, self, &fa, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(ready[1]);
    K3SM_CHECK_EQ(spawned, 0);
    std::int64_t generation = 0;
    K3SM_CHECK(::read(ready[0], &generation, sizeof generation) == sizeof generation);
    ::close(ready[0]);

    auto reader = sdk::budget_page::open(page.c_str());
    K3SM_CHECK(reader.has_value());
    long accepted = 0, torn = 0, mixed_accepted = 0, other = 0, raw = 0, raw_mixed = 0, distinct = 0;
    std::int64_t last_k = -1;
    std::uint64_t last_seq = 0, last_epoch = 0;
    int status = 0;
    bool done = false;
    while (reader && !done) {
        for (int n = 0; n < 256; ++n) {
            const auto r = reader->read_at(0);
            if (r) {
                ++accepted;
                if (r->generation != generation || !consistent(*r, generation)) {
                    ++mixed_accepted;
                }
                if (r->seq % 2 != 0 || r->seq < last_seq || r->notify_epoch < last_epoch) {
                    ++mixed_accepted;
                }
                if (r->observed_at != last_k) {
                    ++distinct;
                    last_k = r->observed_at;
                }
                last_seq = r->seq;
                last_epoch = r->notify_epoch;
            } else if (r.error() == sdk::error::torn) {
                ++torn;
            } else {
                ++other;
            }
            // The control: the first and last body words without the seqlock.
            const auto* bytes = static_cast<const unsigned char*>(reader->data());
            auto load = [&](std::size_t off) {
                auto* p = reinterpret_cast<std::int64_t*>(const_cast<unsigned char*>(bytes) + off);
                return std::atomic_ref<std::int64_t>(*p).load(std::memory_order_relaxed);
            };
            const std::int64_t k = load(sdk::page_offset::observed_at);
            const std::int64_t last = load(sdk::page_offset::counter_slot(sdk::max_counters - 1) +
                                           sdk::page_offset::counter_field::tier1_refused_total);
            ++raw;
            if (last != word(k, last_word)) {
                ++raw_mixed;
            }
        }
        done = ::waitpid(pid, &status, WNOHANG) == pid;
    }
    K3SM_CHECK(done && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    std::printf("reader: %ld accepted (%ld distinct publications), %ld torn, %ld other; "
                "control: %ld of %ld unsynchronized copies mixed\n",
                accepted, distinct, torn, other, raw_mixed, raw);
    K3SM_CHECK_EQ(mixed_accepted, 0L);
    K3SM_CHECK_EQ(other, 0L);
    K3SM_CHECK(accepted >= 1000);
    K3SM_CHECK(distinct >= 100);
    K3SM_CHECK(raw_mixed > 0);  // the processes raced, so the oracle had work
    ::unlink(page.c_str());
    ::unlink(lock.c_str());
    ::rmdir(page_dir.c_str());
    ::rmdir(state_dir.c_str());
    ::rmdir(base.c_str());
    return 0;
}

}  // namespace stress_test

int main(int argc, char** argv) {
    if (argc == 5 && std::strcmp(argv[1], "writer") == 0) {
        return stress_test::writer_main(argv[2], argv[3], std::atoi(argv[4]));
    }
    ::signal(SIGPIPE, SIG_IGN);
    stress_test::run(argv[0]);
    if (k3sm_test::failures() != 0) {
        std::fprintf(stderr, "writer_stress_test: %d failure(s)\n", k3sm_test::failures());
        return 1;
    }
    std::puts("writer_stress_test: ok");
    return 0;
}
