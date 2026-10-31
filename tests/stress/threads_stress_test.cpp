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

// The torn-read stress: one writer thread publishing at full speed against
// N reader threads, meant to run under ThreadSanitizer (hack/ci.sh
// --stress builds it with -fsanitize=thread). Every published word is a
// function of the publication's number k (observedAt), so the oracle is
// that every snapshot a reader accepts is exactly publication(observedAt).
//
// It runs two phases:
//   1. shared memory: the writer and the readers use one buffer at one
//      address, so ThreadSanitizer sees every store the writer makes and
//      every load a reader makes. The writer here is a test-only copy of
//      page_writer::publish's store sequence.
//   2. page file: the SDK's own page_writer publishes into a page file and
//      every reader maps it through budget_page. The two mappings are at
//      different addresses, so ThreadSanitizer cannot pair them; this phase
//      checks the real writer against the oracle.
// Each reader also copies two body words without the seqlock and counts the
// mixed copies, a control that shows the threads really race. In the first
// phase one more reader runs with the seqlock defeated (its source reports
// the same even seq on every load), the negative control that shows the
// oracle catches a mixed snapshot when the reader lets one through.
//
// An optional argument sets each phase's length in milliseconds.

#include <k3sm/budget.hpp>
#include <k3sm/page_writer.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "../publication.hpp"
#include "../test_util.hpp"

namespace sdk = k3sm::sdk;

namespace threads_stress {

// tally is one reader's counts.
struct tally {
    long accepted = 0;
    long torn = 0;
    long mixed = 0;     // accepted snapshots that break the oracle
    long other = 0;     // any error but torn
    long distinct = 0;  // accepted snapshots of a new publication
    long raw = 0;
    long raw_mixed = 0;
};

// word_load reads one page word with a relaxed atomic load.
inline std::int64_t word_load(const unsigned char* base, std::size_t off) {
    auto* p = reinterpret_cast<std::int64_t*>(const_cast<unsigned char*>(base) + off);
    return std::atomic_ref<std::int64_t>(*p).load(std::memory_order_relaxed);
}

// seqless_source is a mapped page whose seq word always reads as 2, so the
// reader's seqlock never sees a write in progress.
struct seqless_source {
    sdk::detail::mapped_source page;

    std::size_t size() const noexcept { return page.size(); }
    std::uint64_t load(std::size_t offset, std::memory_order order) const noexcept {
        return offset == sdk::page_offset::seq ? 2 : page.load(offset, order);
    }
    void fence_acquire() const noexcept { page.fence_acquire(); }
};

template <class Source>
Source make_source(const unsigned char* base) {
    if constexpr (std::is_same_v<Source, seqless_source>) {
        return seqless_source{{base, sdk::page_size}};
    } else {
        return Source{base, sdk::page_size};
    }
}

// read_loop reads base until stop is set, checking each accepted snapshot
// against the oracle and the seqlock's own monotonic words.
template <class Source>
void read_loop(const unsigned char* base, std::int64_t generation, const std::atomic<bool>& stop,
               tally& t) {
    const Source src = make_source<Source>(base);
    std::int64_t last_k = -1;
    std::uint64_t last_seq = 0;
    std::uint64_t last_epoch = 0;
    while (!stop.load(std::memory_order_relaxed)) {
        const auto r = sdk::detail::read_page(src, 0);
        if (r) {
            ++t.accepted;
            if (r->generation != generation || !k3sm_test::consistent(*r, generation) || r->seq % 2 != 0 ||
                r->seq < last_seq || r->notify_epoch < last_epoch || r->observed_at < last_k) {
                ++t.mixed;
            }
            if (r->observed_at != last_k) {
                ++t.distinct;
                last_k = r->observed_at;
            }
            last_seq = r->seq;
            last_epoch = r->notify_epoch;
        } else if (r.error() == sdk::error::torn) {
            ++t.torn;
        } else {
            ++t.other;
        }
        const std::int64_t k = word_load(base, sdk::page_offset::observed_at);
        const std::int64_t last = word_load(base, sdk::page_offset::counter_slot(sdk::max_counters - 1) +
                                                       sdk::page_offset::counter_field::tier1_refused_total);
        ++t.raw;
        if (last != k3sm_test::word(k, k3sm_test::last_word)) {
            ++t.raw_mixed;
        }
    }
}

// test_writer publishes into a buffer with page_writer::publish's store
// sequence: seq odd (relaxed), a release fence, the body word by word
// (relaxed), then seq even (release).
class test_writer {
public:
    test_writer(unsigned char* base, std::int64_t generation) : base_(base), generation_(generation) {}

    bool publish(std::int64_t k) {
        sdk::snapshot s = k3sm_test::publication(k);
        s.generation = generation_;
        if (!sdk::encode_page(s, image_, sizeof image_)) {
            return false;
        }
        if (!started_) {
            // The first publication lays down the header too, before any
            // reader starts.
            std::memcpy(base_, image_, sdk::page_size);
            started_ = true;
            return true;
        }
        store(sdk::page_offset::seq, seq_ + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (std::size_t o = sdk::page_header_size; o < sdk::page_header_size + sdk::page_body_size; o += 8) {
            std::uint64_t v = 0;
            std::memcpy(&v, image_ + o, sizeof v);
            store(o, v, std::memory_order_relaxed);
        }
        seq_ += 2;
        store(sdk::page_offset::seq, seq_, std::memory_order_release);
        return true;
    }

private:
    void store(std::size_t off, std::uint64_t v, std::memory_order order) {
        std::atomic_ref<std::uint64_t>(*reinterpret_cast<std::uint64_t*>(base_ + off)).store(v, order);
    }

    unsigned char* base_;
    std::int64_t generation_;
    std::uint64_t seq_ = 0;
    bool started_ = false;
    alignas(8) unsigned char image_[sdk::page_size] = {};
};

unsigned reader_count() {
    const unsigned hw = std::thread::hardware_concurrency();
    return std::clamp(hw > 1 ? hw - 1 : 1u, 2u, 8u);
}

// check sums the tallies, prints them and checks the oracle held and the
// phase did real work.
void check(const char* phase, const std::vector<tally>& ts, std::int64_t publications) {
    tally sum;
    for (const tally& t : ts) {
        sum.accepted += t.accepted;
        sum.torn += t.torn;
        sum.mixed += t.mixed;
        sum.other += t.other;
        sum.distinct += t.distinct;
        sum.raw += t.raw;
        sum.raw_mixed += t.raw_mixed;
    }
    std::printf("%s: %zu readers, %lld publications; %ld accepted (%ld distinct), %ld torn, %ld other, "
                "%ld mixed; control: %ld of %ld unsynchronized copies mixed\n",
                phase, ts.size(), static_cast<long long>(publications), sum.accepted, sum.distinct, sum.torn,
                sum.other, sum.mixed, sum.raw_mixed, sum.raw);
    K3SM_CHECK_EQ(sum.mixed, 0L);
    K3SM_CHECK_EQ(sum.other, 0L);
    K3SM_CHECK(sum.accepted >= 100);
    K3SM_CHECK(sum.distinct >= 20);
    K3SM_CHECK(sum.raw_mixed > 0);  // the threads raced, so the oracle had work
}

void phase_shared(std::chrono::milliseconds length) {
    constexpr std::int64_t generation = 7;
    auto* base = static_cast<unsigned char*>(::operator new(sdk::page_size, std::align_val_t{64}));
    test_writer w(base, generation);
    K3SM_CHECK(w.publish(1));
    std::atomic<bool> stop{false};
    std::vector<tally> ts(reader_count());
    std::vector<std::thread> readers;
    for (tally& t : ts) {
        readers.emplace_back(read_loop<sdk::detail::mapped_source>, base, generation, std::cref(stop),
                             std::ref(t));
    }
    tally broken;
    readers.emplace_back(read_loop<seqless_source>, base, generation, std::cref(stop), std::ref(broken));
    std::int64_t k = 1;
    const auto deadline = std::chrono::steady_clock::now() + length;
    std::thread writer([&] {
        while (std::chrono::steady_clock::now() < deadline) {
            for (int burst = 0; burst < 64; ++burst) {
                if (!w.publish(++k)) {
                    std::abort();  // publication() is always a valid snapshot
                }
            }
        }
        stop.store(true, std::memory_order_relaxed);
    });
    writer.join();
    for (std::thread& r : readers) {
        r.join();
    }
    check("shared memory", ts, k);
    std::printf("negative control (seqlock defeated): %ld accepted, %ld mixed\n", broken.accepted, broken.mixed);
    K3SM_CHECK(broken.mixed > 0);  // the oracle catches what the seqlock would have refused
    ::operator delete(base, std::align_val_t{64});
}

void phase_file(std::chrono::milliseconds length) {
    const std::string dir = std::string(K3SM_SDK_TEST_TMPDIR) + "/threads_stress." + std::to_string(::getpid());
    const std::string page_dir = dir + "/page";
    const std::string state_dir = dir + "/state";
    ::mkdir(dir.c_str(), 0755);
    ::mkdir(page_dir.c_str(), 0755);
    ::mkdir(state_dir.c_str(), 0700);
    const std::string page = page_dir + "/budget.v1.page";
    const std::string lock = state_dir + "/writer.lock";
    {
        auto w = sdk::page_writer::open(page.c_str(), lock.c_str());
        K3SM_CHECK(w.has_value());
        if (!w) {
            return;
        }
        std::int64_t k = 1;
        K3SM_CHECK(w->publish(k3sm_test::publication(k)).has_value());
        const std::int64_t generation = w->generation();
        std::vector<sdk::budget_page> maps;
        for (unsigned i = 0; i < reader_count(); ++i) {
            auto m = sdk::budget_page::open(page.c_str());
            K3SM_CHECK(m.has_value());
            if (!m) {
                return;
            }
            maps.push_back(std::move(*m));
        }
        std::atomic<bool> stop{false};
        std::vector<tally> ts(maps.size());
        std::vector<std::thread> readers;
        for (std::size_t i = 0; i < maps.size(); ++i) {
            readers.emplace_back(read_loop<sdk::detail::mapped_source>, static_cast<const unsigned char*>(maps[i].data()), generation,
                                 std::cref(stop), std::ref(ts[i]));
        }
        const auto deadline = std::chrono::steady_clock::now() + length;
        while (std::chrono::steady_clock::now() < deadline) {
            for (int burst = 0; burst < 64; ++burst) {
                if (!w->publish(k3sm_test::publication(++k))) {
                    std::abort();
                }
            }
        }
        stop.store(true, std::memory_order_relaxed);
        for (std::thread& r : readers) {
            r.join();
        }
        check("page file", ts, k);
    }
    ::unlink(page.c_str());
    ::unlink(lock.c_str());
    ::rmdir(page_dir.c_str());
    ::rmdir(state_dir.c_str());
    ::rmdir(dir.c_str());
}

}  // namespace threads_stress

int main(int argc, char** argv) {
    const long ms = argc > 1 ? std::atol(argv[1]) : 1500;
    const std::chrono::milliseconds length(ms > 0 ? ms : 1500);
    threads_stress::phase_shared(length);
    threads_stress::phase_file(length);
    if (k3sm_test::failures() != 0) {
        std::fprintf(stderr, "threads_stress_test: %d failure(s)\n", k3sm_test::failures());
        return 1;
    }
    std::puts("threads_stress_test: ok");
    return 0;
}
