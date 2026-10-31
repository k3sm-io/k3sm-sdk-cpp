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
// settles within them is read. Then the header line under a scripted
// resume: a source whose first loads of the minor word see the header from
// before a writer's resume and every other load sees the header after it,
// so the reader pairs an old minor with a new body size, and the bounded
// re-read of the header reads the page; a header that stays invalid is
// reported after exactly header_retries attempts.

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

// rewritten_source serves page bytes, except that the first stale_loads
// loads of the word holding the minor return stale_versions: the header
// word as it was before a resume the reader's later loads see the end of.
struct rewritten_source {
    const std::string* page;
    std::uint64_t stale_versions;
    int stale_loads;
    mutable int versions_loads = 0;
    mutable int magic_loads = 0;

    std::size_t size() const noexcept { return page->size(); }

    std::uint64_t load(std::size_t offset, std::memory_order) const noexcept {
        if (offset == sdk::page_offset::magic) {
            ++magic_loads;
        }
        if (offset == sdk::page_offset::abi_major && versions_loads++ < stale_loads) {
            return stale_versions;
        }
        std::uint64_t v = 0;
        std::memcpy(&v, page->data() + offset, sizeof v);
        return v;
    }

    void fence_acquire() const noexcept {}
};

void put_u64(std::string& page, std::size_t offset, std::uint64_t v) {
    std::memcpy(page.data() + offset, &v, sizeof v);
}

std::uint64_t get_u64(const std::string& page, std::size_t offset) {
    std::uint64_t v = 0;
    std::memcpy(&v, page.data() + offset, sizeof v);
    return v;
}

// versions_word is the word holding the major, the minor and the header size.
std::uint64_t versions_word(std::uint16_t minor) {
    return std::uint64_t{sdk::page_abi_major} | std::uint64_t{minor} << 16 |
           std::uint64_t{sdk::page_header_size} << 32;
}

// A rollback (a minor-0 writer resuming a minor-1 page) lands between the
// reader's load of the minor and its load of the body size: the first
// attempt sees minor 1 with the minor-0 body, which is too short for it.
// The reader loads the header again and reads the minor-0 page.
void test_rollback_between_header_loads() {
    std::string page = k3sm_test::read_file(k3sm_test::fixture("darwin.v1.page"));
    put_u64(page, sdk::page_offset::abi_major, versions_word(0));
    put_u64(page, sdk::page_offset::body_size, sdk::page_body_size_minor0);
    rewritten_source src{&page, versions_word(1), 1};
    const auto r = sdk::detail::read_page(src, observed);
    K3SM_CHECK(r.has_value());
    if (r) {
        K3SM_CHECK_EQ(r->abi_minor, 0u);
        K3SM_CHECK_EQ(r->body_size, sdk::page_body_size_minor0);
        K3SM_CHECK(!r->capacity.cpu_performance_cores.has_value());
        K3SM_CHECK_EQ(r->node_name(), std::string_view("mac-worker-1"));
    }
    K3SM_CHECK_EQ(src.versions_loads, 2);
}

// The stale pairing is seen on every attempt but the last: still read.
void test_rollback_settles_on_last_header_attempt() {
    std::string page = k3sm_test::read_file(k3sm_test::fixture("darwin.v1.page"));
    put_u64(page, sdk::page_offset::abi_major, versions_word(0));
    put_u64(page, sdk::page_offset::body_size, sdk::page_body_size_minor0);
    rewritten_source src{&page, versions_word(1), sdk::detail::header_retries - 1};
    const auto r = sdk::detail::read_page(src, observed);
    K3SM_CHECK(r.has_value());
    K3SM_CHECK_EQ(src.versions_loads, sdk::detail::header_retries);
}

// A header that is invalid on every load is reported, after the bound.
void test_invalid_header_after_bound() {
    std::string page = k3sm_test::read_file(k3sm_test::fixture("darwin.v1.page"));
    put_u64(page, sdk::page_offset::body_size, sdk::page_body_size_minor0);
    rewritten_source short_body{&page, 0, 0};
    const auto r = sdk::detail::read_page(short_body, observed);
    K3SM_CHECK(!r && r.error() == sdk::error::truncated);
    K3SM_CHECK_EQ(short_body.versions_loads, sdk::detail::header_retries);

    std::string other = k3sm_test::read_file(k3sm_test::fixture("darwin.v1.page"));
    put_u64(other, sdk::page_offset::magic, get_u64(other, sdk::page_offset::magic) ^ 1);
    rewritten_source bad_magic{&other, 0, 0};
    const auto m = sdk::detail::read_page(bad_magic, observed);
    K3SM_CHECK(!m && m.error() == sdk::error::abi_mismatch);
    K3SM_CHECK_EQ(bad_magic.magic_loads, sdk::detail::header_retries);
    K3SM_CHECK_EQ(bad_magic.versions_loads, 0);
}

}  // namespace torn_test

int main() {
    torn_test::test_always_odd();
    torn_test::test_always_moving();
    torn_test::test_settles_on_last_attempt();
    torn_test::test_settles_after_moving();
    torn_test::test_odd_one_attempt_too_many();
    torn_test::test_rollback_between_header_loads();
    torn_test::test_rollback_settles_on_last_header_attempt();
    torn_test::test_invalid_header_after_bound();
    if (k3sm_test::failures() != 0) {
        std::fprintf(stderr, "torn_test: %d failure(s)\n", k3sm_test::failures());
        return 1;
    }
    std::puts("torn_test: ok");
    return 0;
}
