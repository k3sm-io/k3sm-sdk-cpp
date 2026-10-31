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

// The reader benchmark: the latency of one uncontended read of a mapped
// page, and the latency from a writer's publish of a notifyEpoch change to
// a waiting reader's read of it. It prints p50 and p99 of each; it checks
// no threshold. Build it optimized and without sanitizers for real numbers.
//
// Usage: k3sm_sdk_page_bench [--quick] [--reads N] [--wakes N]
// --quick is one short iteration (the gate's run): it builds the harness
// and shows it runs, nothing more.

#include <k3sm/budget.hpp>
#include <k3sm/page_writer.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "../publication.hpp"

namespace sdk = k3sm::sdk;
using clock_type = std::chrono::steady_clock;

namespace page_bench {

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now().time_since_epoch()).count();
}

// percentile returns the p-th percentile (0..100) of v, sorting it.
std::int64_t percentile(std::vector<std::int64_t>& v, double p) {
    if (v.empty()) {
        return 0;
    }
    std::sort(v.begin(), v.end());
    const auto at = static_cast<std::size_t>(p / 100.0 * static_cast<double>(v.size() - 1));
    return v[at];
}

// bench_read times reads of an idle page, one at a time.
bool bench_read(const sdk::budget_page& page, long reads) {
    std::vector<std::int64_t> ns;
    ns.reserve(static_cast<std::size_t>(reads));
    for (long i = 0; i < reads; ++i) {
        const std::int64_t t0 = now_ns();
        const auto r = page.read_at(0);
        const std::int64_t t1 = now_ns();
        if (!r) {
            std::fprintf(stderr, "read: %s\n", std::string(r.error().name()).c_str());
            return false;
        }
        ns.push_back(t1 - t0);
    }
    const std::int64_t p50 = percentile(ns, 50);
    const std::int64_t p99 = percentile(ns, 99);
    std::printf("read: %ld reads, p50 %lld ns, p99 %lld ns (includes one clock read)\n", reads,
                static_cast<long long>(p50), static_cast<long long>(p99));
    return true;
}

// bench_wake times a change from just before the writer's publish to the
// waiting reader's completed read of it.
bool bench_wake(sdk::page_writer& w, const sdk::budget_page& page, long wakes) {
    std::atomic<std::int64_t> published_at{0};
    std::atomic<bool> ready{false};
    std::atomic<bool> failed{false};
    std::vector<std::int64_t> ns;
    ns.reserve(static_cast<std::size_t>(wakes));
    std::thread reader([&] {
        sdk::change_notifier n(page);
        for (long i = 0; i < wakes; ++i) {
            ready.store(true, std::memory_order_release);
            if (!n.wait(std::chrono::seconds(5))) {
                failed.store(true);
                return;
            }
            const auto r = page.read_at(0);
            const std::int64_t t1 = now_ns();
            if (!r) {
                failed.store(true);
                return;
            }
            ns.push_back(t1 - published_at.load(std::memory_order_acquire));
        }
    });
    std::int64_t k = 1000;
    for (long i = 0; i < wakes && !failed.load(); ++i) {
        while (!ready.exchange(false, std::memory_order_acq_rel)) {
            std::this_thread::yield();
        }
        // Give the reader time to block, so the wake is a real one.
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        // Moving to the next ladder step changes a threshold, so notifyEpoch
        // is bumped and the waiter woken.
        k += 64;
        published_at.store(now_ns(), std::memory_order_release);
        if (!w.publish(k3sm_test::publication(k))) {
            failed.store(true);
        }
    }
    reader.join();
    if (failed.load()) {
        std::fprintf(stderr, "wake: a wait timed out or a read failed\n");
        return false;
    }
    const std::int64_t p50 = percentile(ns, 50);
    const std::int64_t p99 = percentile(ns, 99);
    std::printf("wake-to-read: %ld wakes, p50 %lld ns, p99 %lld ns\n", wakes, static_cast<long long>(p50),
                static_cast<long long>(p99));
    return true;
}

}  // namespace page_bench

int main(int argc, char** argv) {
    long reads = 1000000;
    long wakes = 2000;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--quick") == 0) {
            reads = 1000;
            wakes = 10;
        } else if (std::strcmp(argv[i], "--reads") == 0 && i + 1 < argc) {
            reads = std::atol(argv[++i]);
        } else if (std::strcmp(argv[i], "--wakes") == 0 && i + 1 < argc) {
            wakes = std::atol(argv[++i]);
        } else {
            std::fprintf(stderr, "usage: %s [--quick] [--reads N] [--wakes N]\n", argv[0]);
            return 2;
        }
    }
    if (reads < 1 || wakes < 1) {
        std::fprintf(stderr, "reads and wakes must be positive\n");
        return 2;
    }
    const std::string dir = std::string(K3SM_SDK_TEST_TMPDIR) + "/page_bench." + std::to_string(::getpid());
    const std::string page_dir = dir + "/page";
    const std::string state_dir = dir + "/state";
    ::mkdir(dir.c_str(), 0755);
    ::mkdir(page_dir.c_str(), 0755);
    ::mkdir(state_dir.c_str(), 0700);
    const std::string page_path = page_dir + "/budget.v1.page";
    const std::string lock_path = state_dir + "/writer.lock";
    int rc = 1;
    {
        auto w = sdk::page_writer::open(page_path.c_str(), lock_path.c_str());
        auto page = w ? sdk::budget_page::open(page_path.c_str()) : sdk::result<sdk::budget_page>(sdk::error::io);
        if (w && page && w->publish(k3sm_test::publication(1000))) {
            const bool ok = page_bench::bench_read(*page, reads) && page_bench::bench_wake(*w, *page, wakes);
            rc = ok ? 0 : 1;
        } else {
            std::fprintf(stderr, "bench: could not open the page\n");
        }
    }
    ::unlink(page_path.c_str());
    ::unlink(lock_path.c_str());
    ::rmdir(page_dir.c_str());
    ::rmdir(state_dir.c_str());
    ::rmdir(dir.c_str());
    return rc;
}
