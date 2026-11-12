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

// The change notifier on notifyEpoch. The cross-process cases spawn this
// binary again as a writer that maps the page read-write and bumps the word
// on command. The oracle is the epoch value: every epoch the waiter
// observes is one the writer published and reported back over a pipe, and
// a jump past an intermediate value happens only where the test made the
// writer coalesce several bumps into one wake. Each command is sent only
// when the waiter is about to wait (the ready handshake), and the writer
// pauses briefly before it bumps, so the waiter is usually parked.

#include <k3sm/budget.hpp>
#include <k3sm/change_notifier.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <os/clock.h>
#include <os/os_sync_wait_on_address.h>
#elif defined(__linux__)
#include <climits>
#include <linux/futex.h>
#include <sys/syscall.h>
#endif

#include "test_util.hpp"

extern char** environ;

namespace sdk = k3sm::sdk;
using namespace std::chrono_literals;

namespace k3sm::sdk {

// change_notifier_test_access breaks a notifier's doorbell queue from the
// outside. It does not close queue_fd_: the destructor closes that number,
// and a close here would let it close whatever fd reused the number. It
// puts another fd on the number instead, so the queue wait fails while the
// destructor's close stays the close of an fd the notifier owns. On Darwin
// that fd is /dev/null, which is no kqueue. On Linux it is the write end of
// a pipe whose read end is closed, which poll reports with POLLERR (poll on
// /dev/null would only report it readable).
struct change_notifier_test_access {
    static constexpr std::chrono::nanoseconds max_slice() { return change_notifier::max_slice; }
    static constexpr std::chrono::nanoseconds sleep_step() { return change_notifier::sleep_step; }
    static std::chrono::nanoseconds slice_for(std::chrono::nanoseconds left) {
        return change_notifier::slice_for(left);
    }
    // block_doorbell waits on n's doorbell queue alone, for up to left, with
    // no re-check of the word around it.
    static bool block_doorbell(change_notifier& n, std::chrono::nanoseconds left) {
        return n.block_doorbell(left);
    }
    // address_wait_ok classifies one address wait's return as normal or as
    // the primitive failing for good.
    static constexpr bool address_wait_ok(int rc, int err) {
        return change_notifier::address_wait_ok(rc, err);
    }
    // fall_back demotes n as a failed address wait does.
    static void fall_back(change_notifier& n) { n.fall_back(); }

    static bool break_queue(change_notifier& n) {
        if (n.queue_fd_ < 0) {
            return false;
        }
#if defined(__linux__)
        int ends[2] = {-1, -1};
        if (::pipe(ends) != 0) {
            return false;
        }
        ::close(ends[0]);
        const int null_fd = ends[1];
#else
        const int null_fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (null_fd < 0) {
            return false;
        }
#endif
        const bool ok = ::dup2(null_fd, n.queue_fd_) == n.queue_fd_;
        ::close(null_fd);
        if (ok) {
            (void)::fcntl(n.queue_fd_, F_SETFD, FD_CLOEXEC);
        }
        return ok;
    }
};

}  // namespace k3sm::sdk

namespace notifier_test {

const char* self_path = nullptr;

std::string tmp_path(const char* name) {
    return std::string(K3SM_SDK_TEST_TMPDIR) + "/notifier_test." + std::to_string(::getpid()) +
           "." + name;
}

// remove_on_exit removes each named file, FIFO or empty directory when the
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

// write_page copies the darwin golden page to path.
bool write_page(const std::string& path) {
    const std::string bytes = k3sm_test::read_file(k3sm_test::fixture("darwin.v1.page"));
    if (bytes.empty()) {
        return false;
    }
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        return false;
    }
    const bool ok = ::write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size());
    ::close(fd);
    return ok;
}

// rw_page is a read-write mapping of a page file, the writer's view.
struct rw_page {
    unsigned char* base = nullptr;
    std::size_t size = 0;

    explicit rw_page(const std::string& path) {
        const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            return;
        }
        const off_t end = ::lseek(fd, 0, SEEK_END);
        void* p = ::mmap(nullptr, static_cast<std::size_t>(end), PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
        ::close(fd);
        if (p != MAP_FAILED) {
            base = static_cast<unsigned char*>(p);
            size = static_cast<std::size_t>(end);
        }
    }
    ~rw_page() {
        if (base != nullptr) {
            ::munmap(base, size);
        }
    }
    rw_page(const rw_page&) = delete;
    rw_page& operator=(const rw_page&) = delete;

    std::uint64_t* epoch_word() const {
        return reinterpret_cast<std::uint64_t*>(base + sdk::page_offset::notify_epoch);
    }
    std::uint64_t epoch() const {
        return std::atomic_ref<std::uint64_t>(*epoch_word()).load(std::memory_order_acquire);
    }
    std::uint64_t bump() const {
        return std::atomic_ref<std::uint64_t>(*epoch_word()).fetch_add(1, std::memory_order_release) + 1;
    }
    // wake wakes every waiter on the word, as the producer does after a bump.
    void wake() const {
#if defined(__APPLE__)
        if (__builtin_available(macOS 14.4, iOS 17.4, tvOS 17.4, watchOS 10.4, *)) {
            (void)os_sync_wake_by_address_all(epoch_word(), sizeof(std::uint64_t),
                                              OS_SYNC_WAKE_BY_ADDRESS_SHARED);
        }
#elif defined(__linux__)
        (void)::syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(epoch_word()), FUTEX_WAKE,
                        INT_MAX, nullptr, nullptr, 0);
#endif
    }
};

bool read_all(int fd, void* buf, std::size_t n) {
    auto* p = static_cast<unsigned char*>(buf);
    while (n > 0) {
        const ssize_t r = ::read(fd, p, n);
        if (r <= 0) {
            return false;
        }
        p += r;
        n -= static_cast<std::size_t>(r);
    }
    return true;
}

bool write_all(int fd, const void* buf, std::size_t n) {
    const auto* p = static_cast<const unsigned char*>(buf);
    while (n > 0) {
        const ssize_t r = ::write(fd, p, n);
        if (r <= 0) {
            return false;
        }
        p += r;
        n -= static_cast<std::size_t>(r);
    }
    return true;
}

// writer_main is the child process. Commands, one byte each, answered with
// the epoch the command left on the page (8 bytes):
//   b  bump once, then wake the word
//   c  bump five times, then wake once (a coalesced change)
//   s  wake without a bump (a wake the waiter must not report)
//   d  bump once, then write the doorbell file; no wake on the word
//   q  exit
int writer_main(const char* page_path, int cmd_fd, int reply_fd, const char* bell_path) {
    rw_page page(page_path);
    if (page.base == nullptr) {
        return 3;
    }
    char cmd = 0;
    while (read_all(cmd_fd, &cmd, 1) && cmd != 'q') {
        std::this_thread::sleep_for(2ms);  // let the waiter park
        switch (cmd) {
        case 'b':
            page.bump();
            page.wake();
            break;
        case 'c':
            for (int i = 0; i < 5; ++i) {
                page.bump();
            }
            page.wake();
            break;
        case 's':
            page.wake();
            break;
        case 'd': {
            page.bump();
            const int fd = ::open(bell_path, O_WRONLY | O_CLOEXEC);
            if (fd < 0) {
                return 4;
            }
            const char ring = '1';
            const bool ok = ::pwrite(fd, &ring, 1, 0) == 1;
            ::close(fd);
            if (!ok) {
                return 5;
            }
            break;
        }
        default:
            return 6;
        }
        const std::uint64_t e = page.epoch();
        if (!write_all(reply_fd, &e, sizeof e)) {
            return 7;
        }
    }
    return 0;
}

// writer is the parent's handle on a spawned writer process.
struct writer {
    pid_t pid = -1;
    int cmd = -1;
    int reply = -1;

    writer(const std::string& page_path, const std::string& bell_path) {
        int c[2];
        int r[2];
        if (::pipe(c) != 0 || ::pipe(r) != 0) {
            return;
        }
        const std::string cfd = std::to_string(c[0]);
        const std::string rfd = std::to_string(r[1]);
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addclose(&fa, c[1]);
        posix_spawn_file_actions_addclose(&fa, r[0]);
        char* argv[] = {const_cast<char*>(self_path), const_cast<char*>("writer"),
                        const_cast<char*>(page_path.c_str()), const_cast<char*>(cfd.c_str()),
                        const_cast<char*>(rfd.c_str()), const_cast<char*>(bell_path.c_str()),
                        nullptr};
        if (posix_spawn(&pid, self_path, &fa, nullptr, argv, environ) != 0) {
            pid = -1;
        }
        posix_spawn_file_actions_destroy(&fa);
        ::close(c[0]);
        ::close(r[1]);
        cmd = c[1];
        reply = r[0];
    }

    // send asks the writer for one command; result collects its answer.
    bool send(char c) const { return write_all(cmd, &c, 1); }
    std::uint64_t result() const {
        std::uint64_t e = 0;
        if (!read_all(reply, &e, sizeof e)) {
            k3sm_test::fail(__FILE__, __LINE__, "the writer did not answer");
        }
        return e;
    }

    // finish stops the writer and returns its exit status.
    int finish() {
        const char q = 'q';
        (void)write_all(cmd, &q, 1);
        ::close(cmd);
        ::close(reply);
        int status = 0;
        if (pid > 0 && ::waitpid(pid, &status, 0) == pid && WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
        return -1;
    }
};

void test_poll() {
    const std::string path = tmp_path("poll.page");
    const remove_on_exit cleanup{path};
    K3SM_CHECK(write_page(path));
    auto page = sdk::budget_page::open(path.c_str());
    K3SM_CHECK(page.has_value());
    if (!page) {
        return;
    }
    rw_page rw(path);
    K3SM_CHECK(rw.base != nullptr);
    sdk::change_notifier n(*page);
    K3SM_CHECK_EQ(n.epoch(), rw.epoch());
    K3SM_CHECK(!n.poll());
    const std::uint64_t e = rw.bump();
    K3SM_CHECK(n.poll());
    K3SM_CHECK_EQ(n.epoch(), e);
    K3SM_CHECK(!n.poll());  // the change is reported once

    // A moved-from notifier hands its state over.
    sdk::change_notifier m(std::move(n));
    K3SM_CHECK_EQ(m.epoch(), e);
    rw.bump();
    K3SM_CHECK(m.poll());

    // The snapshot's notify_epoch is the same word.
    const auto s = page->read_at(1792411200);
    K3SM_CHECK(s.has_value());
    if (s) {
        K3SM_CHECK_EQ(s->notify_epoch, m.epoch());
    }
}

void test_timeout() {
    const std::string path = tmp_path("timeout.page");
    const remove_on_exit cleanup{path};
    K3SM_CHECK(write_page(path));
    auto page = sdk::budget_page::open(path.c_str());
    if (!page) {
        k3sm_test::fail(__FILE__, __LINE__, "open");
        return;
    }
    sdk::change_notifier n(*page);
#if defined(__APPLE__) || defined(__linux__)
    K3SM_CHECK(n.source() == sdk::wake_source::address);
#endif
    const auto t0 = std::chrono::steady_clock::now();
    K3SM_CHECK(!n.wait(40ms));
    const auto waited = std::chrono::steady_clock::now() - t0;
    K3SM_CHECK(waited >= 39ms);
    // Tight enough that a timeout handed to the OS in the wrong unit (about
    // 40 times too long) fails.
    K3SM_CHECK(waited < 1s);
    K3SM_CHECK(!n.wait(0ns));  // a zero timeout is a poll
    K3SM_CHECK(!n.wait(-5ms));

    // Timeouts below a millisecond time out too, and promptly.
    for (const std::chrono::nanoseconds t :
         {std::chrono::nanoseconds(1ns), std::chrono::nanoseconds(999us)}) {
        const auto t1 = std::chrono::steady_clock::now();
        K3SM_CHECK(!n.wait(t));
        K3SM_CHECK(std::chrono::steady_clock::now() - t1 < 1s);
    }
}

// test_address_cross_process waits on the word while another process bumps it.
void test_address_cross_process() {
    const std::string path = tmp_path("address.page");
    const remove_on_exit cleanup{path};
    K3SM_CHECK(write_page(path));
    auto page = sdk::budget_page::open(path.c_str());
    if (!page) {
        k3sm_test::fail(__FILE__, __LINE__, "open");
        return;
    }
    sdk::change_notifier n(*page);
    K3SM_CHECK(n.source() == sdk::wake_source::address);
    writer w(path, "-");
    K3SM_CHECK(w.pid > 0);
    if (w.pid <= 0) {
        return;
    }

    // One bump per wake: every published epoch is observed, none skipped.
    std::uint64_t last = n.epoch();
    for (int i = 0; i < 25; ++i) {
        K3SM_CHECK(w.send('b'));
        const auto t0 = std::chrono::steady_clock::now();
        const bool woke = n.wait(5s);
        // A lost wake would still find the epoch after the 5 s timeout;
        // the bound proves the wake itself ended the wait.
        K3SM_CHECK(std::chrono::steady_clock::now() - t0 < 1s);
        const std::uint64_t published = w.result();
        K3SM_CHECK(woke);
        K3SM_CHECK_EQ(published, last + 1);
        K3SM_CHECK_EQ(n.epoch(), published);
        last = published;
    }

    // Five bumps, one wake: the waiter sees a published epoch past the last
    // one; anything short of the final value is the recorded coalesce, and
    // the next poll catches up.
    K3SM_CHECK(w.send('c'));
    K3SM_CHECK(n.wait(5s));
    const std::uint64_t coalesced = w.result();
    K3SM_CHECK_EQ(coalesced, last + 5);
    K3SM_CHECK(n.epoch() > last && n.epoch() <= coalesced);
    if (n.epoch() != coalesced) {
        K3SM_CHECK(n.poll());
    }
    K3SM_CHECK_EQ(n.epoch(), coalesced);
    last = coalesced;

    // A wake with no new epoch is not a change.
    K3SM_CHECK(w.send('s'));
    K3SM_CHECK(!n.wait(60ms));
    K3SM_CHECK_EQ(w.result(), last);
    K3SM_CHECK_EQ(n.epoch(), last);
    K3SM_CHECK(n.source() == sdk::wake_source::address);  // never degraded to sleeping

    K3SM_CHECK_EQ(w.finish(), 0);
}

// test_doorbell_cross_process waits on the producer's doorbell file: the
// writer bumps the word and rings the bell, and never wakes the word.
void test_doorbell_cross_process() {
    const std::string path = tmp_path("doorbell.page");
    const std::string bell = tmp_path("doorbell.bell");
    const remove_on_exit cleanup{path, bell};
    K3SM_CHECK(write_page(path));
    const int bfd = ::open(bell.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    K3SM_CHECK(bfd >= 0);
    if (bfd >= 0) {
        ::close(bfd);
    }
    auto page = sdk::budget_page::open(path.c_str());
    if (!page) {
        k3sm_test::fail(__FILE__, __LINE__, "open");
        return;
    }
    sdk::change_notifier n(*page, bell.c_str(), sdk::wake_source::doorbell);
#if defined(__APPLE__) || defined(__linux__)
    K3SM_CHECK(n.source() == sdk::wake_source::doorbell);
#endif
    writer w(path, bell);
    K3SM_CHECK(w.pid > 0);
    if (w.pid <= 0) {
        return;
    }
    std::uint64_t last = n.epoch();
    for (int i = 0; i < 10; ++i) {
        K3SM_CHECK(w.send('d'));
        const auto t0 = std::chrono::steady_clock::now();
        const bool woke = n.wait(5s);
        K3SM_CHECK(std::chrono::steady_clock::now() - t0 < 1s);
        const std::uint64_t published = w.result();
        K3SM_CHECK(woke);
        K3SM_CHECK_EQ(published, last + 1);
        K3SM_CHECK_EQ(n.epoch(), published);
        last = published;
    }
    // A ring with no new epoch (the test rings it here) is not a change.
    const int rfd = ::open(bell.c_str(), O_WRONLY | O_CLOEXEC);
    if (rfd >= 0) {
        const char ring = '1';
        K3SM_CHECK(::pwrite(rfd, &ring, 1, 0) == 1);
        ::close(rfd);
    }
    K3SM_CHECK(!n.wait(60ms));
#if defined(__APPLE__) || defined(__linux__)
    K3SM_CHECK(n.source() == sdk::wake_source::doorbell);
#endif
    K3SM_CHECK_EQ(w.finish(), 0);
}

// test_doorbell_ring_wakes_the_queue proves the doorbell registration itself
// wakes a waiter. It blocks on the doorbell queue directly, so the 100 ms
// re-check of the word inside wait is not in play, and the word is never
// bumped. block_doorbell returns true on a wake and on a timeout alike, so the
// proof is the time: the call returns after the ring and long before its
// several-second ceiling. The control blocks with no ring and returns only
// when its short ceiling runs out.
void test_doorbell_ring_wakes_the_queue() {
#if defined(__APPLE__) || defined(__linux__)
    const std::string path = tmp_path("ringbell.page");
    const std::string bell = tmp_path("ringbell.bell");
    const remove_on_exit cleanup{path, bell};
    K3SM_CHECK(write_page(path));
    const int bfd = ::open(bell.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    K3SM_CHECK(bfd >= 0);
    if (bfd >= 0) {
        ::close(bfd);
    }
    auto page = sdk::budget_page::open(path.c_str());
    if (!page) {
        k3sm_test::fail(__FILE__, __LINE__, "open");
        return;
    }
    sdk::change_notifier n(*page, bell.c_str(), sdk::wake_source::doorbell);
    K3SM_CHECK(n.source() == sdk::wake_source::doorbell);
    if (n.source() != sdk::wake_source::doorbell) {
        return;
    }
    const std::uint64_t before = n.epoch();

    // Control: no ring, so the wait runs to its ceiling.
    {
        const auto t0 = std::chrono::steady_clock::now();
        K3SM_CHECK(sdk::change_notifier_test_access::block_doorbell(n, 50ms));
        const auto waited = std::chrono::steady_clock::now() - t0;
        K3SM_CHECK(waited >= 40ms);
        K3SM_CHECK(waited < 2s);
    }

    // The ring alone ends a wait whose ceiling is 5 s.
    std::atomic<bool> rang{false};
    std::atomic<bool> ring_failed{false};
    std::thread ringer([&rang, &ring_failed, &bell] {
        std::this_thread::sleep_for(150ms);
        const int rfd = ::open(bell.c_str(), O_WRONLY | O_CLOEXEC);
        if (rfd < 0) {
            ring_failed.store(true, std::memory_order_release);
            return;
        }
        const char ring = '1';
        rang.store(true, std::memory_order_release);
        if (::pwrite(rfd, &ring, 1, 0) != 1) {
            ring_failed.store(true, std::memory_order_release);
        }
        ::close(rfd);
    });
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = sdk::change_notifier_test_access::block_doorbell(n, 5s);
    const auto waited = std::chrono::steady_clock::now() - t0;
    const bool after_ring = rang.load(std::memory_order_acquire);
    ringer.join();
    K3SM_CHECK(!ring_failed.load(std::memory_order_acquire));
    K3SM_CHECK(ok);
    K3SM_CHECK(after_ring);
    K3SM_CHECK(waited >= 100ms);
    K3SM_CHECK(waited < 2s);
    K3SM_CHECK_EQ(n.epoch(), before);
    K3SM_CHECK(n.source() == sdk::wake_source::doorbell);
#endif
}

// test_doorbell_absent keeps the primary when the producer has no doorbell.
void test_doorbell_absent() {
    const std::string path = tmp_path("nobell.page");
    const remove_on_exit cleanup{path};
    K3SM_CHECK(write_page(path));
    auto page = sdk::budget_page::open(path.c_str());
    if (!page) {
        k3sm_test::fail(__FILE__, __LINE__, "open");
        return;
    }
    const sdk::change_notifier a(*page, "/nonexistent/k3sm/budget.bell", sdk::wake_source::doorbell);
    const sdk::change_notifier b(*page, nullptr);
#if defined(__APPLE__) || defined(__linux__)
    K3SM_CHECK(a.source() == sdk::wake_source::address);
    K3SM_CHECK(b.source() == sdk::wake_source::address);
#endif
}

// test_short_page: a mapping too short to hold notifyEpoch never reports a
// change and never touches memory past its end.
void test_short_page() {
    const std::string path = tmp_path("short.page");
    const remove_on_exit cleanup{path};
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    K3SM_CHECK(fd >= 0);
    if (fd < 0) {
        return;
    }
    const char bytes[64] = {};
    K3SM_CHECK(::write(fd, bytes, sizeof bytes) == static_cast<ssize_t>(sizeof bytes));
    ::close(fd);
    auto page = sdk::budget_page::open(path.c_str());
    if (!page) {
        k3sm_test::fail(__FILE__, __LINE__, "open");
        return;
    }
    sdk::change_notifier n(*page);
    K3SM_CHECK(n.source() == sdk::wake_source::none);
    K3SM_CHECK_EQ(n.epoch(), std::uint64_t{0});
    K3SM_CHECK(!n.poll());
    const auto t0 = std::chrono::steady_clock::now();
    K3SM_CHECK(!n.wait(10ms));
    K3SM_CHECK(std::chrono::steady_clock::now() - t0 >= 9ms);
}

// test_notifier_seeded_from_snapshot_reports_intervening_bump: a consumer
// reads a snapshot, the producer bumps, then the consumer builds its
// notifier. Seeded from the snapshot's notify_epoch, the first wait reports
// the bump at once instead of waiting for the next one.
void test_notifier_seeded_from_snapshot_reports_intervening_bump() {
    const std::string path = tmp_path("seed.page");
    const remove_on_exit cleanup{path};
    K3SM_CHECK(write_page(path));
    auto page = sdk::budget_page::open(path.c_str());
    if (!page) {
        k3sm_test::fail(__FILE__, __LINE__, "open");
        return;
    }
    rw_page rw(path);
    K3SM_CHECK(rw.base != nullptr);
    const auto s = page->read_at(1792411200);
    K3SM_CHECK(s.has_value());
    if (!s || rw.base == nullptr) {
        return;
    }
    const std::uint64_t bumped = rw.bump();  // the intervening change
    K3SM_CHECK(bumped != s->notify_epoch);

    // Unseeded, the notifier starts at the live word and misses it.
    sdk::change_notifier late(*page);
    K3SM_CHECK_EQ(late.epoch(), bumped);
    K3SM_CHECK(!late.poll());

    sdk::change_notifier n(*page);
    n.seed(s->notify_epoch);
    K3SM_CHECK_EQ(n.epoch(), s->notify_epoch);
    const auto t0 = std::chrono::steady_clock::now();
    K3SM_CHECK(n.wait(5s));
    K3SM_CHECK(std::chrono::steady_clock::now() - t0 < 1s);
    K3SM_CHECK_EQ(n.epoch(), bumped);
    K3SM_CHECK(!n.poll());  // reported once

    // Seeding the doorbell form works the same way.
    sdk::change_notifier d(*page, nullptr);
    d.seed(s->notify_epoch);
    K3SM_CHECK(d.poll());
    K3SM_CHECK_EQ(d.epoch(), bumped);

    // Seeded with the live epoch, there is nothing to report.
    sdk::change_notifier same(*page);
    same.seed(bumped);
    K3SM_CHECK(!same.poll());

    // A moved-from notifier ignores seed.
    sdk::change_notifier moved(*page);
    sdk::change_notifier taker(std::move(moved));
    const std::uint64_t before = moved.epoch();  // deliberate use after move
    moved.seed(before + 7);
    K3SM_CHECK_EQ(moved.epoch(), before);
    K3SM_CHECK(!moved.poll());

    // A notifier over a page too short to hold notifyEpoch ignores seed.
    const std::string short_path = tmp_path("seed.short.page");
    const remove_on_exit short_cleanup{short_path};
    const int fd = ::open(short_path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    K3SM_CHECK(fd >= 0);
    if (fd < 0) {
        return;
    }
    const char bytes[64] = {};
    K3SM_CHECK(::write(fd, bytes, sizeof bytes) == static_cast<ssize_t>(sizeof bytes));
    ::close(fd);
    auto short_page = sdk::budget_page::open(short_path.c_str());
    if (!short_page) {
        k3sm_test::fail(__FILE__, __LINE__, "open short page");
        return;
    }
    sdk::change_notifier sn(*short_page);
    sn.seed(42);
    K3SM_CHECK_EQ(sn.epoch(), std::uint64_t{0});
    K3SM_CHECK(!sn.poll());
}

// guarded_wait runs n.wait(timeout) under a watchdog: if the wait has not
// returned after the bound, the watchdog bumps and wakes the word so the wait
// returns, and the lost wake is reported as a failure instead of a hang.
bool guarded_wait(sdk::change_notifier& n, const rw_page& rw, std::chrono::nanoseconds timeout,
                  int line) {
    constexpr auto bound = 5s;
    std::atomic<bool> done{false};
    std::atomic<bool> fired{false};
    std::thread watchdog([&] {
        const auto end = std::chrono::steady_clock::now() + bound;
        while (!done.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= end) {
                fired.store(true, std::memory_order_release);
                rw.bump();
                rw.wake();
                return;
            }
            std::this_thread::sleep_for(5ms);
        }
    });
    const bool woke = n.wait(timeout);
    done.store(true, std::memory_order_release);
    watchdog.join();
    if (fired.load(std::memory_order_acquire)) {
        k3sm_test::fail(__FILE__, line, "unbounded wait lost its wake (watchdog fired)");
    }
    return woke;
}

// test_wait_max_timeout_no_overflow: nanoseconds::max() is "wait forever".
// Adding it to now() overflows the clock's signed count, which the
// UndefinedBehaviorSanitizer build aborts on; the deadline must saturate.
// Each wait runs under guarded_wait, so a lost wake fails within about 5 s.
void test_wait_max_timeout_no_overflow() {
    const std::string path = tmp_path("maxwait.page");
    const remove_on_exit cleanup{path};
    K3SM_CHECK(write_page(path));
    auto page = sdk::budget_page::open(path.c_str());
    if (!page) {
        k3sm_test::fail(__FILE__, __LINE__, "open");
        return;
    }
    rw_page rw(path);
    K3SM_CHECK(rw.base != nullptr);
    if (rw.base == nullptr) {
        return;
    }
    constexpr auto forever = std::chrono::nanoseconds::max();

    // A change already pending: the wait reports it without blocking.
    sdk::change_notifier n(*page);
    const std::uint64_t first = rw.bump();
    K3SM_CHECK(guarded_wait(n, rw, forever, __LINE__));
    K3SM_CHECK_EQ(n.epoch(), first);

    // A change that arrives while the wait blocks with no deadline.
    std::thread bumper([&rw] {
        std::this_thread::sleep_for(20ms);
        rw.bump();
        rw.wake();
    });
    K3SM_CHECK(guarded_wait(n, rw, forever, __LINE__));
    bumper.join();
    K3SM_CHECK_EQ(n.epoch(), first + 1);

    // A timeout just short of the limit takes the same path.
    rw.bump();
    K3SM_CHECK(guarded_wait(n, rw, forever - 1ns, __LINE__));
    K3SM_CHECK_EQ(n.epoch(), first + 2);
}

// test_doorbell_fifo_does_not_block_construction: a doorbell path that names
// a FIFO with no writer is not a doorbell on Darwin. Opening a FIFO for reading blocks
// until a writer appears (O_EVTONLY included), so the constructor must not
// wait for one: it returns at once, ignores the FIFO and keeps the word. A
// watchdog opens the FIFO for writing after about 3 s, which releases an
// open that blocked, so a regression fails instead of hanging.
void test_doorbell_fifo_does_not_block_construction() {
    const std::string path = tmp_path("fifo.page");
    const std::string bell = tmp_path("fifo.bell");
    const remove_on_exit cleanup{path, bell};
    K3SM_CHECK(write_page(path));
    ::unlink(bell.c_str());
    if (::mkfifo(bell.c_str(), 0600) != 0) {
        k3sm_test::fail(__FILE__, __LINE__, "mkfifo");
        return;
    }
    auto page = sdk::budget_page::open(path.c_str());
    if (!page) {
        k3sm_test::fail(__FILE__, __LINE__, "open");
        return;
    }
    rw_page rw(path);
    K3SM_CHECK(rw.base != nullptr);
    if (rw.base == nullptr) {
        return;
    }

    std::atomic<bool> done{false};
    std::atomic<bool> fired{false};
    std::thread watchdog([&] {
        const auto end = std::chrono::steady_clock::now() + 3s;
        while (!done.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= end) {
                fired.store(true, std::memory_order_release);
                // O_RDWR never blocks on a FIFO and counts as a writer, so it
                // releases a reader blocked in open.
                const int wfd = ::open(bell.c_str(), O_RDWR | O_CLOEXEC);
                while (!done.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(5ms);
                }
                if (wfd >= 0) {
                    ::close(wfd);
                }
                return;
            }
            std::this_thread::sleep_for(5ms);
        }
    });
    sdk::change_notifier n(*page, bell.c_str(), sdk::wake_source::doorbell);
    done.store(true, std::memory_order_release);
    watchdog.join();
    if (fired.load(std::memory_order_acquire)) {
        k3sm_test::fail(__FILE__, __LINE__, "construction blocked on the FIFO doorbell");
    }
    // On Darwin a FIFO is not a doorbell; on Linux inotify still watches it.
#if defined(__APPLE__)
    K3SM_CHECK(n.source() == sdk::wake_source::address);
#endif

    // The notifier still reports a bump through its fallback source.
    std::thread bumper([&rw] {
        std::this_thread::sleep_for(20ms);
        rw.bump();
        rw.wake();
    });
    const std::uint64_t before = n.epoch();
    K3SM_CHECK(guarded_wait(n, rw, 2s, __LINE__));
    bumper.join();
    K3SM_CHECK_EQ(n.epoch(), before + 1);
}

// test_doorbell_directory_is_ignored: a doorbell path that names a
// directory is not a doorbell. The constructor returns at once, keeps the
// word on Darwin, and a bump with a wake is still reported by wait.
void test_doorbell_directory_is_ignored() {
    const std::string path = tmp_path("dirbell.page");
    const std::string bell = tmp_path("dirbell.bell");
    const remove_on_exit cleanup{path, bell};
    K3SM_CHECK(write_page(path));
    ::rmdir(bell.c_str());
    ::unlink(bell.c_str());
    if (::mkdir(bell.c_str(), 0700) != 0) {
        k3sm_test::fail(__FILE__, __LINE__, "mkdir");
        return;
    }
    auto page = sdk::budget_page::open(path.c_str());
    if (!page) {
        k3sm_test::fail(__FILE__, __LINE__, "open");
        return;
    }
    rw_page rw(path);
    K3SM_CHECK(rw.base != nullptr);
    if (rw.base == nullptr) {
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    sdk::change_notifier n(*page, bell.c_str(), sdk::wake_source::doorbell);
    K3SM_CHECK(std::chrono::steady_clock::now() - t0 < 2s);
#if defined(__APPLE__)
    K3SM_CHECK(n.source() == sdk::wake_source::address);
#endif
    std::thread bumper([&rw] {
        std::this_thread::sleep_for(20ms);
        rw.bump();
        rw.wake();
    });
    const std::uint64_t before = n.epoch();
    K3SM_CHECK(guarded_wait(n, rw, 2s, __LINE__));
    bumper.join();
    K3SM_CHECK_EQ(n.epoch(), before + 1);
}

// test_doorbell_wait_failure_falls_back_to_sleep: a doorbell wait that fails
// for good (on Darwin the queue is no longer a kqueue, on Linux poll reports
// the descriptor with POLLERR) makes the notifier sleep between polls, and a
// later bump is still reported.
void test_doorbell_wait_failure_falls_back_to_sleep() {
#if defined(__APPLE__) || defined(__linux__)
    const std::string path = tmp_path("badqueue.page");
    const std::string bell = tmp_path("badqueue.bell");
    const remove_on_exit cleanup{path, bell};
    K3SM_CHECK(write_page(path));
    const int bfd = ::open(bell.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    K3SM_CHECK(bfd >= 0);
    if (bfd >= 0) {
        ::close(bfd);
    }
    auto page = sdk::budget_page::open(path.c_str());
    if (!page) {
        k3sm_test::fail(__FILE__, __LINE__, "open");
        return;
    }
    rw_page rw(path);
    K3SM_CHECK(rw.base != nullptr);
    if (rw.base == nullptr) {
        return;
    }
    sdk::change_notifier n(*page, bell.c_str(), sdk::wake_source::doorbell);
    K3SM_CHECK(n.source() == sdk::wake_source::doorbell);
    if (n.source() != sdk::wake_source::doorbell) {
        return;
    }
    K3SM_CHECK(sdk::change_notifier_test_access::break_queue(n));

    const auto t0 = std::chrono::steady_clock::now();
    K3SM_CHECK(!n.wait(30ms));
    const auto waited = std::chrono::steady_clock::now() - t0;
    K3SM_CHECK(waited >= 29ms);
    K3SM_CHECK(waited < 2s);
    K3SM_CHECK(n.source() == sdk::wake_source::sleep);

    // Sleeping between polls, the notifier still reports a bump.
    std::thread bumper([&rw] {
        std::this_thread::sleep_for(20ms);
        rw.bump();
    });
    const std::uint64_t before = n.epoch();
    const auto t1 = std::chrono::steady_clock::now();
    K3SM_CHECK(n.wait(5s));
    K3SM_CHECK(std::chrono::steady_clock::now() - t1 < 1s);
    bumper.join();
    K3SM_CHECK_EQ(n.epoch(), before + 1);
    K3SM_CHECK(n.source() == sdk::wake_source::sleep);
#endif
}

// test_preferred_doorbell_still_sees_a_bump_without_a_ring: with the doorbell
// as the wake source, a bump of the epoch word that is never followed by a
// doorbell write or a wake on the word is still reported, well before the
// wait's timeout.
void test_preferred_doorbell_still_sees_a_bump_without_a_ring() {
    const std::string path = tmp_path("noring.page");
    const std::string bell = tmp_path("noring.bell");
    const remove_on_exit cleanup{path, bell};
    K3SM_CHECK(write_page(path));
    const int bfd = ::open(bell.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    K3SM_CHECK(bfd >= 0);
    if (bfd >= 0) {
        ::close(bfd);
    }
    auto page = sdk::budget_page::open(path.c_str());
    if (!page) {
        k3sm_test::fail(__FILE__, __LINE__, "open");
        return;
    }
    rw_page rw(path);
    K3SM_CHECK(rw.base != nullptr);
    if (rw.base == nullptr) {
        return;
    }
    sdk::change_notifier n(*page, bell.c_str(), sdk::wake_source::doorbell);
    K3SM_CHECK(n.source() == sdk::wake_source::doorbell);
    if (n.source() != sdk::wake_source::doorbell) {
        return;
    }

    // Bump the word only: no doorbell write, no wake.
    std::thread bumper([&rw] {
        std::this_thread::sleep_for(20ms);
        rw.bump();
    });
    const std::uint64_t before = n.epoch();
    const auto t0 = std::chrono::steady_clock::now();
    K3SM_CHECK(n.wait(3s));
    K3SM_CHECK(std::chrono::steady_clock::now() - t0 < 2s);
    K3SM_CHECK(n.source() == sdk::wake_source::doorbell);
    bumper.join();
    K3SM_CHECK_EQ(n.epoch(), before + 1);

    // A quiet doorbell slice does not demote the wake source.
    K3SM_CHECK(!n.wait(250ms));
    K3SM_CHECK(n.source() == sdk::wake_source::doorbell);
}

// test_wait_caps_are_pinned pins the two numbers a wait is built from: the
// longest single block (a day) and the sleep fallback's poll step (1 ms).
void test_wait_caps_are_pinned() {
    using access = sdk::change_notifier_test_access;
    K3SM_CHECK(access::max_slice() == std::chrono::hours(24));
    K3SM_CHECK(access::max_slice() == std::chrono::nanoseconds(86'400'000'000'000LL));
    K3SM_CHECK(access::sleep_step() == std::chrono::milliseconds(1));
    K3SM_CHECK(access::sleep_step() == std::chrono::nanoseconds(1'000'000));
}

// test_wait_slice_is_clamped pins one block of a wait: a timeout up to a day
// is blocked in one piece, and anything longer, up to nanoseconds::max(), is
// cut to exactly a day.
void test_wait_slice_is_clamped() {
    using access = sdk::change_notifier_test_access;
    using ns = std::chrono::nanoseconds;
    K3SM_CHECK(access::slice_for(ns(0)) == ns(0));
    K3SM_CHECK(access::slice_for(ns(1)) == ns(1));
    K3SM_CHECK(access::slice_for(ns(86'399'999'999'999LL)) == ns(86'399'999'999'999LL));
    K3SM_CHECK(access::slice_for(ns(86'400'000'000'000LL)) == ns(86'400'000'000'000LL));
    K3SM_CHECK(access::slice_for(ns(86'400'000'000'001LL)) == ns(86'400'000'000'000LL));
    K3SM_CHECK(access::slice_for(ns(9'223'372'036'854'775'807LL)) == ns(86'400'000'000'000LL));
}

// test_move_assign_and_failed_doorbells_leak_no_descriptor: move-assigning a
// notifier with a doorbell over another one closes the old doorbell, and a
// doorbell that cannot be opened (a missing path; on Darwin a directory)
// leaves the notifier on the address wait and keeps no descriptor, however
// often either is repeated.
void test_move_assign_and_failed_doorbells_leak_no_descriptor() {
    const std::string path = tmp_path("fdleak.page");
    const std::string bell = tmp_path("fdleak.bell");
    const std::string dirbell = tmp_path("fdleak.dirbell");
    const remove_on_exit cleanup{path, bell, dirbell};
    K3SM_CHECK(write_page(path));
    const int bfd = ::open(bell.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    K3SM_CHECK(bfd >= 0);
    if (bfd >= 0) {
        ::close(bfd);
    }
    ::rmdir(dirbell.c_str());
    ::unlink(dirbell.c_str());
    K3SM_CHECK_EQ(::mkdir(dirbell.c_str(), 0700), 0);
    auto page = sdk::budget_page::open(path.c_str());
    if (!page) {
        k3sm_test::fail(__FILE__, __LINE__, "open");
        return;
    }

    const auto assign_round = [&]() {
        sdk::change_notifier a(*page, bell.c_str(), sdk::wake_source::doorbell);
        sdk::change_notifier b(*page, bell.c_str(), sdk::wake_source::doorbell);
#if defined(__APPLE__) || defined(__linux__)
        K3SM_CHECK(a.source() == sdk::wake_source::doorbell);
        K3SM_CHECK(b.source() == sdk::wake_source::doorbell);
#endif
        b = std::move(a);
#if defined(__APPLE__) || defined(__linux__)
        K3SM_CHECK(b.source() == sdk::wake_source::doorbell);
#endif
        K3SM_CHECK(a.source() == sdk::wake_source::none);
    };
    const auto fail_round = [&]() {
        const sdk::change_notifier m(*page, "/nonexistent/k3sm/budget.bell", sdk::wake_source::doorbell);
#if defined(__APPLE__) || defined(__linux__)
        K3SM_CHECK(m.source() == sdk::wake_source::address);
#endif
#if defined(__APPLE__)
        const sdk::change_notifier d(*page, dirbell.c_str(), sdk::wake_source::doorbell);
        K3SM_CHECK(d.source() == sdk::wake_source::address);
#endif
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

// test_move_construct_keeps_the_doorbell: move-constructing a notifier that
// holds a doorbell hands the doorbell to the new notifier, leaves the old one
// with no source, keeps the new one waiting on the doorbell after the old one
// is destroyed, and keeps no descriptor once both are gone.
void test_move_construct_keeps_the_doorbell() {
#if defined(__APPLE__) || defined(__linux__)
    const std::string path = tmp_path("movector.page");
    const std::string bell = tmp_path("movector.bell");
    const remove_on_exit cleanup{path, bell};
    K3SM_CHECK(write_page(path));
    const int bfd = ::open(bell.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    K3SM_CHECK(bfd >= 0);
    if (bfd >= 0) {
        ::close(bfd);
    }
    auto page = sdk::budget_page::open(path.c_str());
    if (!page) {
        k3sm_test::fail(__FILE__, __LINE__, "open");
        return;
    }
    rw_page rw(path);
    K3SM_CHECK(rw.base != nullptr);
    if (rw.base == nullptr) {
        return;
    }
    const int base_fds = k3sm_test::open_fd_count();
    {
        auto a = std::make_unique<sdk::change_notifier>(*page, bell.c_str(), sdk::wake_source::doorbell);
        const sdk::wake_source original = a->source();
        K3SM_CHECK(original == sdk::wake_source::doorbell);
        sdk::change_notifier b(std::move(*a));
        K3SM_CHECK(b.source() == original);
        K3SM_CHECK(a->source() == sdk::wake_source::none);
        // Destroying the moved-from notifier must not close what b now holds.
        a.reset();

        const std::uint64_t before = b.epoch();
        std::thread bumper([&rw] {
            std::this_thread::sleep_for(20ms);
            rw.bump();
            rw.wake();
        });
        K3SM_CHECK(b.wait(2s));
        bumper.join();
        K3SM_CHECK_EQ(b.epoch(), before + 1);
        K3SM_CHECK(b.source() == original);
    }
    K3SM_CHECK_EQ(k3sm_test::open_fd_count(), base_fds);
#endif
}

// test_doorbell_character_device_is_ignored: a doorbell path that names a
// character device (/dev/null) is not a doorbell on Darwin, like a FIFO or a
// directory. The notifier keeps the word, still reports a bump, and building
// and destroying such a notifier keeps no descriptor.
void test_doorbell_character_device_is_ignored() {
#if defined(__APPLE__)
    const std::string path = tmp_path("chrbell.page");
    const remove_on_exit cleanup{path};
    K3SM_CHECK(write_page(path));
    auto page = sdk::budget_page::open(path.c_str());
    if (!page) {
        k3sm_test::fail(__FILE__, __LINE__, "open");
        return;
    }
    rw_page rw(path);
    K3SM_CHECK(rw.base != nullptr);
    if (rw.base == nullptr) {
        return;
    }
    const int base_fds = k3sm_test::open_fd_count();
    {
        const auto t0 = std::chrono::steady_clock::now();
        sdk::change_notifier n(*page, "/dev/null", sdk::wake_source::doorbell);
        K3SM_CHECK(std::chrono::steady_clock::now() - t0 < 2s);
        K3SM_CHECK(n.source() == sdk::wake_source::address);
        K3SM_CHECK_EQ(k3sm_test::open_fd_count(), base_fds);
        std::thread bumper([&rw] {
            std::this_thread::sleep_for(20ms);
            rw.bump();
            rw.wake();
        });
        const std::uint64_t before = n.epoch();
        K3SM_CHECK(guarded_wait(n, rw, 2s, __LINE__));
        bumper.join();
        K3SM_CHECK_EQ(n.epoch(), before + 1);
    }
    for (int i = 0; i < 100; ++i) {
        const sdk::change_notifier m(*page, "/dev/null", sdk::wake_source::doorbell);
        K3SM_CHECK(m.source() == sdk::wake_source::address);
    }
    K3SM_CHECK_EQ(k3sm_test::open_fd_count(), base_fds);
#endif
}

// test_kept_doorbell_with_the_word_as_primary: given a doorbell that opens
// but no preference for it (the default, or any value but doorbell), the
// notifier keeps the word as its wake source, a bump with a wake is reported,
// and building and destroying such a notifier keeps no descriptor.
void test_kept_doorbell_with_the_word_as_primary() {
#if defined(__APPLE__) || defined(__linux__)
    const std::string path = tmp_path("keptbell.page");
    const std::string bell = tmp_path("keptbell.bell");
    const remove_on_exit cleanup{path, bell};
    K3SM_CHECK(write_page(path));
    const int bfd = ::open(bell.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    K3SM_CHECK(bfd >= 0);
    if (bfd >= 0) {
        ::close(bfd);
    }
    auto page = sdk::budget_page::open(path.c_str());
    if (!page) {
        k3sm_test::fail(__FILE__, __LINE__, "open");
        return;
    }
    rw_page rw(path);
    K3SM_CHECK(rw.base != nullptr);
    if (rw.base == nullptr) {
        return;
    }
    const int base_fds = k3sm_test::open_fd_count();
    sdk::change_notifier n1(*page, bell.c_str());
    sdk::change_notifier n2(*page, bell.c_str(), sdk::wake_source::sleep);
    // The kept doorbell holds descriptors while the notifiers live.
    K3SM_CHECK(k3sm_test::open_fd_count() > base_fds);
    K3SM_CHECK(n1.source() == sdk::wake_source::address);
    K3SM_CHECK(n2.source() == sdk::wake_source::address);

    std::thread bumper([&rw] {
        std::this_thread::sleep_for(20ms);
        rw.bump();
        rw.wake();
    });
    const std::uint64_t before1 = n1.epoch();
    const std::uint64_t before2 = n2.epoch();
    K3SM_CHECK(guarded_wait(n1, rw, 2s, __LINE__));
    bumper.join();
    K3SM_CHECK(n2.wait(2s));
    K3SM_CHECK_EQ(n1.epoch(), before1 + 1);
    K3SM_CHECK_EQ(n2.epoch(), before2 + 1);
    K3SM_CHECK(n1.source() == sdk::wake_source::address);
    K3SM_CHECK(n2.source() == sdk::wake_source::address);

    const auto round = [&]() {
        const sdk::change_notifier a(*page, bell.c_str());
        const sdk::change_notifier b(*page, bell.c_str(), sdk::wake_source::sleep);
        K3SM_CHECK(a.source() == sdk::wake_source::address);
        K3SM_CHECK(b.source() == sdk::wake_source::address);
    };
    round();
    const int before = k3sm_test::open_fd_count();
    for (int i = 0; i < 300; ++i) {
        round();
    }
    K3SM_CHECK_EQ(k3sm_test::open_fd_count(), before);
#endif
}

// test_short_page_given_a_doorbell: a page too short to hold notifyEpoch,
// given a doorbell it would prefer, has nothing to wait on and never opens
// the doorbell, so it holds no descriptor while alive and leaks none.
void test_short_page_given_a_doorbell() {
    const std::string path = tmp_path("shortbell.page");
    const std::string bell = tmp_path("shortbell.bell");
    const remove_on_exit cleanup{path, bell};
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    K3SM_CHECK(fd >= 0);
    if (fd < 0) {
        return;
    }
    const char bytes[64] = {};
    K3SM_CHECK(::write(fd, bytes, sizeof bytes) == static_cast<ssize_t>(sizeof bytes));
    ::close(fd);
    const int bfd = ::open(bell.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    K3SM_CHECK(bfd >= 0);
    if (bfd >= 0) {
        ::close(bfd);
    }
    auto page = sdk::budget_page::open(path.c_str());
    if (!page) {
        k3sm_test::fail(__FILE__, __LINE__, "open");
        return;
    }

    const int before = k3sm_test::open_fd_count();
    {
        sdk::change_notifier n(*page, bell.c_str(), sdk::wake_source::doorbell);
        K3SM_CHECK(n.source() == sdk::wake_source::none);
        // The doorbell was never opened: the live notifier holds nothing.
        K3SM_CHECK_EQ(k3sm_test::open_fd_count(), before);
        K3SM_CHECK(!n.wait(10ms));
        K3SM_CHECK(n.source() == sdk::wake_source::none);
    }
    for (int i = 0; i < 300; ++i) {
        const sdk::change_notifier m(*page, bell.c_str(), sdk::wake_source::doorbell);
        K3SM_CHECK(m.source() == sdk::wake_source::none);
    }
    K3SM_CHECK_EQ(k3sm_test::open_fd_count(), before);
}

// test_address_wait_classifies_errno pins which address wait returns are
// normal (a wake, the timeout, a signal, and on Linux a futex whose word
// already moved) and which mean the primitive failed for good, the case that
// demotes the notifier.
void test_address_wait_classifies_errno() {
    using access = sdk::change_notifier_test_access;
    struct row {
        int rc;
        int err;
        bool ok;
    };
    const row rows[] = {
        {0, 0, true},
        {0, EINVAL, true},
        {-1, ETIMEDOUT, true},
        {-1, EINTR, true},
#if defined(__linux__)
        {-1, EAGAIN, true},
#else
        {-1, EAGAIN, false},
#endif
        {-1, EINVAL, false},
        {-1, EFAULT, false},
        {-1, ENOMEM, false},
        {-1, 0, false},
    };
    for (const row& r : rows) {
        if (access::address_wait_ok(r.rc, r.err) != r.ok) {
            std::fprintf(stderr, "address_wait_ok(%d, %d) != %d\n", r.rc, r.err, r.ok ? 1 : 0);
            k3sm_test::fail(__FILE__, __LINE__, "address_wait_ok");
        }
    }
}

// fall_back_and_see_a_bump demotes n, which must wait on the word, checks it
// moves to want, and checks a later bump of the word alone (no wake, no
// doorbell write) is still reported well before the wait's timeout.
void fall_back_and_see_a_bump(sdk::change_notifier& n, rw_page& rw, sdk::wake_source want) {
    K3SM_CHECK(n.source() == sdk::wake_source::address);
    if (n.source() != sdk::wake_source::address) {
        return;
    }
    sdk::change_notifier_test_access::fall_back(n);
    K3SM_CHECK(n.source() == want);

    std::thread bumper([&rw] {
        std::this_thread::sleep_for(20ms);
        rw.bump();
    });
    const std::uint64_t before = n.epoch();
    const auto t0 = std::chrono::steady_clock::now();
    K3SM_CHECK(n.wait(1s));
    K3SM_CHECK(std::chrono::steady_clock::now() - t0 < 500ms);
    bumper.join();
    K3SM_CHECK_EQ(n.epoch(), before + 1);
    K3SM_CHECK(n.source() == want);
}

// test_failed_address_wait_demotes_to_the_doorbell: a notifier that waits on
// the word and keeps a doorbell moves to the doorbell when the address wait
// fails for good, and still reports a bump.
void test_failed_address_wait_demotes_to_the_doorbell() {
#if defined(__APPLE__) || defined(__linux__)
    const std::string path = tmp_path("demotebell.page");
    const std::string bell = tmp_path("demotebell.bell");
    const remove_on_exit cleanup{path, bell};
    K3SM_CHECK(write_page(path));
    const int bfd = ::open(bell.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    K3SM_CHECK(bfd >= 0);
    if (bfd >= 0) {
        ::close(bfd);
    }
    auto page = sdk::budget_page::open(path.c_str());
    if (!page) {
        k3sm_test::fail(__FILE__, __LINE__, "open");
        return;
    }
    rw_page rw(path);
    K3SM_CHECK(rw.base != nullptr);
    if (rw.base == nullptr) {
        return;
    }
    sdk::change_notifier n(*page, bell.c_str());
    fall_back_and_see_a_bump(n, rw, sdk::wake_source::doorbell);
#endif
}

// test_failed_address_wait_demotes_to_sleep: a notifier that waits on the
// word with no doorbell moves to sleeping between polls when the address
// wait fails for good, and still reports a bump.
void test_failed_address_wait_demotes_to_sleep() {
#if defined(__APPLE__) || defined(__linux__)
    const std::string path = tmp_path("demotesleep.page");
    const remove_on_exit cleanup{path};
    K3SM_CHECK(write_page(path));
    auto page = sdk::budget_page::open(path.c_str());
    if (!page) {
        k3sm_test::fail(__FILE__, __LINE__, "open");
        return;
    }
    rw_page rw(path);
    K3SM_CHECK(rw.base != nullptr);
    if (rw.base == nullptr) {
        return;
    }
    sdk::change_notifier n(*page);
    fall_back_and_see_a_bump(n, rw, sdk::wake_source::sleep);
#endif
}

}  // namespace notifier_test

int main(int argc, char** argv) {
    if (argc == 6 && std::strcmp(argv[1], "writer") == 0) {
        return notifier_test::writer_main(argv[2], std::atoi(argv[3]), std::atoi(argv[4]), argv[5]);
    }
    ::signal(SIGPIPE, SIG_IGN);
    notifier_test::self_path = argv[0];
    notifier_test::test_poll();
    notifier_test::test_timeout();
    notifier_test::test_address_cross_process();
    notifier_test::test_doorbell_cross_process();
    notifier_test::test_doorbell_ring_wakes_the_queue();
    notifier_test::test_doorbell_absent();
    notifier_test::test_short_page();
    notifier_test::test_notifier_seeded_from_snapshot_reports_intervening_bump();
    notifier_test::test_wait_max_timeout_no_overflow();
    notifier_test::test_doorbell_fifo_does_not_block_construction();
    notifier_test::test_doorbell_directory_is_ignored();
    notifier_test::test_doorbell_wait_failure_falls_back_to_sleep();
    notifier_test::test_preferred_doorbell_still_sees_a_bump_without_a_ring();
    notifier_test::test_wait_caps_are_pinned();
    notifier_test::test_wait_slice_is_clamped();
    notifier_test::test_move_assign_and_failed_doorbells_leak_no_descriptor();
    notifier_test::test_move_construct_keeps_the_doorbell();
    notifier_test::test_doorbell_character_device_is_ignored();
    notifier_test::test_kept_doorbell_with_the_word_as_primary();
    notifier_test::test_short_page_given_a_doorbell();
    notifier_test::test_address_wait_classifies_errno();
    notifier_test::test_failed_address_wait_demotes_to_the_doorbell();
    notifier_test::test_failed_address_wait_demotes_to_sleep();
    if (k3sm_test::failures() != 0) {
        std::fprintf(stderr, "notifier_test: %d failure(s)\n", k3sm_test::failures());
        return 1;
    }
    std::puts("notifier_test: ok");
    return 0;
}
