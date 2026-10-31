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

#ifndef K3SM_TEST_PUBLICATION_HPP
#define K3SM_TEST_PUBLICATION_HPP

// The writer-chosen invariant the stress tests check a snapshot against:
// publication(k) is a full Linux producer's snapshot whose every word is a
// function of k (its observedAt), so a snapshot that mixes two publications
// is caught by comparing it with the publication its own observedAt names.

#include <k3sm/budget.hpp>
#include <k3sm/page_writer.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace k3sm_test {

namespace sdk = k3sm::sdk;

// last_word is the index publication() gives the body's last field.
inline constexpr std::int64_t last_word = 90;

// word returns the value field i of publication k holds.
constexpr std::int64_t word(std::int64_t k, std::int64_t i) { return k * 1000003 + i; }

// ladder returns the threshold value field i holds for publication k: it
// moves every 64 publications, so some publications are due and most not.
constexpr std::int64_t ladder(std::int64_t k, std::int64_t i) { return (k / 64) * 1009 + i; }

inline void fill_psi(sdk::psi_line& l, std::int64_t k, std::int64_t& i) {
    l.avg10_permille = word(k, i++);
    l.avg60_permille = word(k, i++);
    l.total_us = word(k, i++);
}

// publication returns snapshot k: a Linux producer's page with every slot
// filled and every field derived from k.
inline sdk::snapshot publication(std::int64_t k) {
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
inline bool consistent(const sdk::snapshot& s, std::int64_t generation) {
    sdk::snapshot want = publication(s.observed_at);
    want.generation = generation;
    unsigned char a[sdk::page_size];
    unsigned char b[sdk::page_size];
    if (!sdk::encode_page(s, a, sizeof a) || !sdk::encode_page(want, b, sizeof b)) {
        return false;
    }
    return std::memcmp(a, b, sizeof a) == 0;
}
}  // namespace k3sm_test

#endif  // K3SM_TEST_PUBLICATION_HPP
