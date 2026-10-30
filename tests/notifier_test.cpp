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
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
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

namespace notifier_test {

const char* self_path = nullptr;

std::string tmp_path(const char* name) {
    return std::string(K3SM_SDK_TEST_TMPDIR) + "/notifier_test." + std::to_string(::getpid()) +
           "." + name;
}

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
    ::unlink(path.c_str());
}

void test_timeout() {
    const std::string path = tmp_path("timeout.page");
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
    K3SM_CHECK(waited < 2s);
    K3SM_CHECK(!n.wait(0ns));  // a zero timeout is a poll
    K3SM_CHECK(!n.wait(-5ms));
    ::unlink(path.c_str());
}

// test_address_cross_process waits on the word while another process bumps it.
void test_address_cross_process() {
    const std::string path = tmp_path("address.page");
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
    ::unlink(path.c_str());
}

// test_doorbell_cross_process waits on the producer's doorbell file: the
// writer bumps the word and rings the bell, and never wakes the word.
void test_doorbell_cross_process() {
    const std::string path = tmp_path("doorbell.page");
    const std::string bell = tmp_path("doorbell.bell");
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
    ::unlink(path.c_str());
    ::unlink(bell.c_str());
}

// test_doorbell_absent keeps the primary when the producer has no doorbell.
void test_doorbell_absent() {
    const std::string path = tmp_path("nobell.page");
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
    ::unlink(path.c_str());
}

// test_short_page: a mapping too short to hold notifyEpoch never reports a
// change and never touches memory past its end.
void test_short_page() {
    const std::string path = tmp_path("short.page");
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
    ::unlink(path.c_str());
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
    notifier_test::test_doorbell_absent();
    notifier_test::test_short_page();
    if (k3sm_test::failures() != 0) {
        std::fprintf(stderr, "notifier_test: %d failure(s)\n", k3sm_test::failures());
        return 1;
    }
    std::puts("notifier_test: ok");
    return 0;
}
