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

// The seqlock retry loop under a scripted seq word: a source that serves
// the darwin golden's bytes but answers each load of seq from a script, so
// a write that is always in progress (odd), or always moving between the
// two loads, is torn after exactly reader_retries attempts, and one that
// settles within them is read.

#include <k3sm/budget.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

#include "test_util.hpp"

namespace sdk = k3sm::sdk;

namespace torn_test {

constexpr std::int64_t observed = 1792411200;

// scripted_source serves page bytes, except that every load of seq returns
// seq_at(n) for its n-th seq load (n from 0: the even attempt loads s1 at
// 2k and s2 at 2k+1 while it keeps reading even values).
struct scripted_source {
    const std::string* page;
    std::function<std::uint64_t(int)> seq_at;
    mutable int seq_loads = 0;
    mutable int fences = 0;

    std::size_t size() const noexcept { return page->size(); }

    std::uint64_t load(std::size_t offset, std::memory_order) const noexcept {
        if (offset == sdk::page_offset::seq) {
            return seq_at(seq_loads++);
        }
        std::uint64_t v = 0;
        std::memcpy(&v, page->data() + offset, sizeof v);
        return v;
    }

    void fence_acquire() const noexcept { ++fences; }
};

void test_always_odd() {
    const std::string page = k3sm_test::read_file(k3sm_test::fixture("darwin.v1.page"));
    scripted_source src{&page, [](int) { return std::uint64_t{7}; }};
    const auto r = sdk::detail::read_page(src, observed);
    K3SM_CHECK(!r && r.error() == sdk::error::torn);
    K3SM_CHECK_EQ(src.seq_loads, sdk::reader_retries);  // one s1 load per attempt
    K3SM_CHECK_EQ(src.fences, 0);                       // no copy was ever taken
}

void test_always_moving() {
    const std::string page = k3sm_test::read_file(k3sm_test::fixture("darwin.v1.page"));
    // Every load sees the writer two steps further on: s2 != s1 each time.
    scripted_source src{&page, [](int n) { return std::uint64_t(2 * n); }};
    const auto r = sdk::detail::read_page(src, observed);
    K3SM_CHECK(!r && r.error() == sdk::error::torn);
    K3SM_CHECK_EQ(src.seq_loads, 2 * sdk::reader_retries);
    K3SM_CHECK_EQ(src.fences, sdk::reader_retries);
}

void test_settles_on_last_attempt() {
    const std::string page = k3sm_test::read_file(k3sm_test::fixture("darwin.v1.page"));
    // Odd for every attempt but the last, then a stable 42.
    constexpr int odd = sdk::reader_retries - 1;
    scripted_source src{&page, [](int n) { return n < odd ? std::uint64_t{41} : std::uint64_t{42}; }};
    const auto r = sdk::detail::read_page(src, observed);
    K3SM_CHECK(r.has_value());
    if (r) {
        K3SM_CHECK_EQ(r->seq, 42u);
        K3SM_CHECK_EQ(r->node_name(), std::string_view("mac-worker-1"));
    }
    K3SM_CHECK_EQ(src.seq_loads, odd + 2);
}

void test_settles_after_moving() {
    const std::string page = k3sm_test::read_file(k3sm_test::fixture("darwin.v1.page"));
    // Ten attempts overlap a write, then the seq holds at 100.
    scripted_source src{&page, [](int n) { return n < 20 ? std::uint64_t(2 * n) : std::uint64_t{100}; }};
    const auto r = sdk::detail::read_page(src, observed);
    K3SM_CHECK(r.has_value());
    if (r) {
        K3SM_CHECK_EQ(r->seq, 100u);
        K3SM_CHECK_EQ(r->generation, 4);
    }
    K3SM_CHECK_EQ(src.fences, 11);
}

void test_odd_one_attempt_too_many() {
    const std::string page = k3sm_test::read_file(k3sm_test::fixture("darwin.v1.page"));
    // Odd for exactly reader_retries loads: the stable value comes too late.
    constexpr int odd = sdk::reader_retries;
    scripted_source src{&page, [](int n) { return n < odd ? std::uint64_t{41} : std::uint64_t{42}; }};
    const auto r = sdk::detail::read_page(src, observed);
    K3SM_CHECK(!r && r.error() == sdk::error::torn);
}

}  // namespace torn_test

int main() {
    torn_test::test_always_odd();
    torn_test::test_always_moving();
    torn_test::test_settles_on_last_attempt();
    torn_test::test_settles_after_moving();
    torn_test::test_odd_one_attempt_too_many();
    if (k3sm_test::failures() != 0) {
        std::fprintf(stderr, "torn_test: %d failure(s)\n", k3sm_test::failures());
        return 1;
    }
    std::puts("torn_test: ok");
    return 0;
}
