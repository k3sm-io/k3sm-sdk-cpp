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

#include "publication.hpp"
#include "test_util.hpp"

extern char** environ;

namespace sdk = k3sm::sdk;
using namespace std::chrono_literals;

namespace stress_test {

constexpr auto writer_runtime = 1500ms;

using k3sm_test::consistent;
using k3sm_test::last_word;
using k3sm_test::publication;
using k3sm_test::word;

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
