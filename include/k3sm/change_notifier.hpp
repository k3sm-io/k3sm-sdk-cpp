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


#ifndef K3SM_CHANGE_NOTIFIER_HPP
#define K3SM_CHANGE_NOTIFIER_HPP

// change_notifier waits for the producer to bump the page's notifyEpoch
// word, which it does only when the level, admit, a threshold or the
// generation changes. A reader that wants every sample polls read()
// instead.
//
// The wait blocks on the word itself in the shared mapping:
// os_sync_wait_on_address with OS_SYNC_WAIT_ON_ADDRESS_SHARED on Darwin,
// FUTEX_WAIT on Linux (on the word's low 32 bits; the page is
// little-endian). A producer may also provide a doorbell file it writes
// after each bump. The notifier uses it only when it is given one and the
// word cannot be waited on (or the caller asks for it), through kqueue
// EVFILT_VNODE on Darwin and inotify on Linux. With neither, the wait
// polls the word in short sleeps. No socket is used. Nothing here throws,
// allocates or reads the environment.

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/event.h>
#if __has_include(<os/os_sync_wait_on_address.h>)
#include <os/clock.h>
#include <os/os_sync_wait_on_address.h>
#define K3SM_SDK_HAVE_OS_SYNC 1
#endif
#elif defined(__linux__)
#include <linux/futex.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/syscall.h>
#endif

#include <k3sm/budget_page.hpp>
#include <k3sm/page_layout.hpp>

namespace k3sm::sdk {

// wake_source is what change_notifier::wait blocks on.
enum class wake_source : std::uint8_t {
    // none: the mapping is too short to hold notifyEpoch; nothing changes.
    none = 0,
    // address: the notifyEpoch word (os_sync on Darwin, futex on Linux).
    address = 1,
    // doorbell: the producer's doorbell file (kqueue on Darwin, inotify on Linux).
    doorbell = 2,
    // sleep: neither is available, so the word is polled in short sleeps.
    sleep = 3,
};

// change_notifier reports changes of one page's notifyEpoch word. It
// remembers the last epoch it reported (at first the epoch at
// construction) and reports a change once. The page must outlive it. It is
// move-only; use one notifier per waiting thread.
class change_notifier {
public:
    // sleep_step is the longest sleep between two polls of the word when
    // the wait has no primitive to block on.
    static constexpr std::chrono::nanoseconds sleep_step = std::chrono::milliseconds(1);

    // change_notifier waits on page's notifyEpoch word.
    explicit change_notifier(const budget_page& page) noexcept
        : change_notifier(page, nullptr, wake_source::address) {}

    // change_notifier waits on page's notifyEpoch word and keeps the
    // producer's doorbell at doorbell_path (nullptr for none) as the
    // fallback. prefer is wake_source::doorbell to wait on the doorbell
    // from the start; any other value keeps the word as the primary. A
    // doorbell that cannot be opened is ignored.
    change_notifier(const budget_page& page, const char* doorbell_path,
                    wake_source prefer = wake_source::address) noexcept {
        if (page.data() == nullptr || page.size() < page_offset::notify_epoch + 8) {
            return;
        }
        word_ = reinterpret_cast<std::uint64_t*>(
            const_cast<unsigned char*>(static_cast<const unsigned char*>(page.data())) +
            page_offset::notify_epoch);
        last_ = load();
        if (doorbell_path != nullptr) {
            open_doorbell(doorbell_path);
        }
        if (prefer == wake_source::doorbell && have_doorbell()) {
            source_ = wake_source::doorbell;
        } else if (address_available()) {
            source_ = wake_source::address;
        } else {
            fall_back();
        }
    }

    change_notifier(const change_notifier&) = delete;
    change_notifier& operator=(const change_notifier&) = delete;
    change_notifier(change_notifier&& o) noexcept
        : word_(std::exchange(o.word_, nullptr)),
          last_(o.last_),
          source_(std::exchange(o.source_, wake_source::none)),
          bell_fd_(std::exchange(o.bell_fd_, -1)),
          queue_fd_(std::exchange(o.queue_fd_, -1)) {}
    change_notifier& operator=(change_notifier&& o) noexcept {
        if (this != &o) {
            close_doorbell();
            word_ = std::exchange(o.word_, nullptr);
            last_ = o.last_;
            source_ = std::exchange(o.source_, wake_source::none);
            bell_fd_ = std::exchange(o.bell_fd_, -1);
            queue_fd_ = std::exchange(o.queue_fd_, -1);
        }
        return *this;
    }
    ~change_notifier() { close_doorbell(); }

    // poll reports whether the epoch moved since the last reported change,
    // and if so records the new epoch. It is one atomic load.
    bool poll() noexcept {
        if (word_ == nullptr) {
            return false;
        }
        const std::uint64_t e = load();
        if (e == last_) {
            return false;
        }
        last_ = e;
        return true;
    }

    // wait blocks until the epoch moves (true, with the new epoch recorded)
    // or timeout passes (false). A timeout of zero or less is poll(). A wake
    // that leaves the epoch where it was is not reported; the wait goes on
    // until the deadline. The new epoch may be several bumps past the last
    // one when the producer changed more than once before the wait woke.
    bool wait(std::chrono::nanoseconds timeout) noexcept {
        if (timeout <= std::chrono::nanoseconds::zero()) {
            return poll();
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (true) {
            if (poll()) {
                return true;
            }
            const auto left = deadline - std::chrono::steady_clock::now();
            if (left <= std::chrono::nanoseconds::zero()) {
                return false;
            }
            block(std::chrono::duration_cast<std::chrono::nanoseconds>(left));
        }
    }

    // epoch returns the last epoch the notifier recorded.
    std::uint64_t epoch() const noexcept { return last_; }

    // source returns what wait blocks on now. It moves from address to the
    // doorbell, or to sleep, when the address wait fails for good.
    wake_source source() const noexcept { return source_; }

private:
    std::uint64_t load() const noexcept {
        return std::atomic_ref<std::uint64_t>(*word_).load(std::memory_order_acquire);
    }

    static bool address_available() noexcept {
#if defined(K3SM_SDK_HAVE_OS_SYNC)
        if (__builtin_available(macOS 14.4, iOS 17.4, tvOS 17.4, watchOS 10.4, *)) {
            return true;
        }
        return false;
#elif defined(__linux__)
        return true;
#else
        return false;
#endif
    }

    bool have_doorbell() const noexcept { return queue_fd_ >= 0; }

    // fall_back leaves the address wait: to the doorbell when there is one,
    // else to sleeping between polls.
    void fall_back() noexcept {
        source_ = have_doorbell() ? wake_source::doorbell : wake_source::sleep;
    }

    // block waits up to left for a wake from the current source. It returns
    // early on a wake, a signal or a failure; the caller re-checks the word.
    void block(std::chrono::nanoseconds left) noexcept {
        switch (source_) {
        case wake_source::address:
            if (!block_address(left)) {
                fall_back();
            }
            return;
        case wake_source::doorbell:
            if (!block_doorbell(left)) {
                source_ = wake_source::sleep;
            }
            return;
        case wake_source::none:
        case wake_source::sleep:
            sleep_for(std::min(left, sleep_step));
            return;
        }
    }

    // block_address waits on the word. It returns false only when the
    // primitive cannot be used at all.
    bool block_address(std::chrono::nanoseconds left) noexcept {
#if defined(K3SM_SDK_HAVE_OS_SYNC)
        if (__builtin_available(macOS 14.4, iOS 17.4, tvOS 17.4, watchOS 10.4, *)) {
            const int rc = os_sync_wait_on_address_with_timeout(
                word_, last_, sizeof(std::uint64_t), OS_SYNC_WAIT_ON_ADDRESS_SHARED,
                OS_CLOCK_MACH_ABSOLUTE_TIME, static_cast<std::uint64_t>(left.count()));
            return rc >= 0 || errno == ETIMEDOUT || errno == EINTR;
        }
        return false;
#elif defined(__linux__)
        // FUTEX_WAIT without FUTEX_PRIVATE_FLAG is the shared futex. It
        // compares 32 bits: the low half of the little-endian word.
        struct timespec ts {};
        ts.tv_sec = static_cast<std::time_t>(left.count() / 1000000000);
        ts.tv_nsec = static_cast<long>(left.count() % 1000000000);
        const long rc = ::syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(word_), FUTEX_WAIT,
                                  static_cast<std::uint32_t>(last_), &ts, nullptr, 0);
        return rc == 0 || errno == ETIMEDOUT || errno == EAGAIN || errno == EINTR;
#else
        (void)left;
        return false;
#endif
    }

    void open_doorbell(const char* path) noexcept {
#if defined(__APPLE__)
        bell_fd_ = ::open(path, O_RDONLY | O_EVTONLY | O_CLOEXEC);
        if (bell_fd_ < 0) {
            return;
        }
        queue_fd_ = ::kqueue();
        if (queue_fd_ >= 0) {
            struct kevent ev {};
            EV_SET(&ev, static_cast<std::uintptr_t>(bell_fd_), EVFILT_VNODE, EV_ADD | EV_CLEAR,
                   NOTE_WRITE | NOTE_EXTEND | NOTE_ATTRIB, 0, nullptr);
            if (::kevent(queue_fd_, &ev, 1, nullptr, 0, nullptr) == 0) {
                return;
            }
        }
        close_doorbell();
#elif defined(__linux__)
        queue_fd_ = ::inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
        if (queue_fd_ < 0) {
            return;
        }
        bell_fd_ = ::inotify_add_watch(queue_fd_, path, IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE);
        if (bell_fd_ < 0) {
            close_doorbell();
        }
#else
        (void)path;
#endif
    }

    void close_doorbell() noexcept {
#if defined(__APPLE__)
        if (bell_fd_ >= 0) {
            ::close(bell_fd_);
        }
#endif
        // On Linux bell_fd_ is the inotify watch, closed with its queue.
        if (queue_fd_ >= 0) {
            ::close(queue_fd_);
        }
        bell_fd_ = -1;
        queue_fd_ = -1;
    }

    // block_doorbell waits for the doorbell. It returns false only when the
    // doorbell cannot be waited on any more.
    bool block_doorbell(std::chrono::nanoseconds left) noexcept {
#if defined(__APPLE__)
        struct timespec ts {};
        ts.tv_sec = static_cast<std::time_t>(left.count() / 1000000000);
        ts.tv_nsec = static_cast<long>(left.count() % 1000000000);
        struct kevent out {};
        const int rc = ::kevent(queue_fd_, nullptr, 0, &out, 1, &ts);
        if (rc < 0) {
            return errno == EINTR;
        }
        return rc == 0 || (out.flags & EV_ERROR) == 0;
#elif defined(__linux__)
        struct pollfd p {};
        p.fd = queue_fd_;
        p.events = POLLIN;
        const auto ms = std::chrono::ceil<std::chrono::milliseconds>(left).count();
        const int rc = ::poll(&p, 1, static_cast<int>(std::min<long long>(ms, 1 << 30)));
        if (rc < 0) {
            return errno == EINTR;
        }
        if (rc > 0) {
            // Drain the queued events; the word, not the event, says what changed.
            alignas(struct inotify_event) unsigned char buf[1024];
            while (::read(queue_fd_, buf, sizeof buf) > 0) {
            }
        }
        return true;
#else
        (void)left;
        return false;
#endif
    }

    static void sleep_for(std::chrono::nanoseconds d) noexcept {
        struct timespec ts {};
        ts.tv_sec = static_cast<std::time_t>(d.count() / 1000000000);
        ts.tv_nsec = static_cast<long>(d.count() % 1000000000);
        ::nanosleep(&ts, nullptr);
    }

    std::uint64_t* word_ = nullptr;
    std::uint64_t last_ = 0;
    wake_source source_ = wake_source::none;
    // bell_fd_ is the doorbell file (Darwin) or its inotify watch (Linux);
    // queue_fd_ is the kqueue or the inotify instance.
    int bell_fd_ = -1;
    int queue_fd_ = -1;
};

}  // namespace k3sm::sdk

#endif  // K3SM_CHANGE_NOTIFIER_HPP
