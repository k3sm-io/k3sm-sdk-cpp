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

#ifndef K3SM_RESULT_HPP
#define K3SM_RESULT_HPP

// The reader's error type and a small result<T, E>, the std::expected shape
// for C++20 (std::expected is C++23). Nothing here throws or allocates.

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>

namespace k3sm::sdk {

// error is why a page operation failed: one of the reader contract's codes,
// and for not_found the errno of the failing system call.
class error {
public:
    // kind is the contract's error code. The values are stable.
    enum kind : std::uint8_t {
        // not_found: no page, or no access to it (sys_errno() says which).
        not_found = 1,
        // abi_mismatch: bad magic, an unknown ABI major, a header size this
        // reader does not know, or a code its tables do not hold.
        abi_mismatch = 2,
        // truncated: a file shorter than headerSize + bodySize, or a bodySize
        // below the size this reader knows.
        truncated = 3,
        // corrupt: a well-formed header, but a field outside its own domain
        // (a nodeNameLen above max_node_name_bytes).
        corrupt = 4,
        // torn: no stable read after reader_retries attempts.
        torn = 5,
        // stale: now - observedAt above stale_factor writer periods.
        stale = 6,
    };

    constexpr error(kind k, int sys_errno = 0) noexcept : kind_(k), errno_(sys_errno) {}

    // code returns the error's kind.
    constexpr kind code() const noexcept { return kind_; }

    // sys_errno returns the errno behind a not_found error, 0 otherwise.
    constexpr int sys_errno() const noexcept { return errno_; }

    // name returns the contract name of the error, as the apis manifest
    // spells it ("notFound", "abiMismatch", ...).
    constexpr std::string_view name() const noexcept { return name_of(kind_); }

    // name_of returns the contract name of k.
    static constexpr std::string_view name_of(kind k) noexcept {
        switch (k) {
        case not_found: return "notFound";
        case abi_mismatch: return "abiMismatch";
        case truncated: return "truncated";
        case corrupt: return "corrupt";
        case torn: return "torn";
        case stale: return "stale";
        }
        return "unknown";
    }

    friend constexpr bool operator==(const error& a, const error& b) noexcept {
        return a.kind_ == b.kind_;
    }
    friend constexpr bool operator==(const error& a, kind b) noexcept { return a.kind_ == b; }

private:
    kind kind_;
    int errno_;
};

// result holds either a T or an E. Accessing the side it does not hold
// aborts the process: check has_value() (or the bool conversion) first.
template <class T, class E = error>
class result {
    static_assert(std::is_nothrow_move_constructible_v<T>, "result<T> needs a nothrow move");
    static_assert(std::is_nothrow_move_constructible_v<E>, "result<E> needs a nothrow move");

public:
    result(const T& v) noexcept(std::is_nothrow_copy_constructible_v<T>) : ok_(true) {
        std::construct_at(&value_, v);
    }
    result(T&& v) noexcept : ok_(true) { std::construct_at(&value_, std::move(v)); }
    result(const E& e) noexcept(std::is_nothrow_copy_constructible_v<E>) : ok_(false) {
        std::construct_at(&error_, e);
    }
    result(typename E::kind k) noexcept
        requires std::is_constructible_v<E, typename E::kind>
        : ok_(false) {
        std::construct_at(&error_, E(k));
    }

    result(const result& o) noexcept(std::is_nothrow_copy_constructible_v<T>) : ok_(o.ok_) {
        if (ok_) {
            std::construct_at(&value_, o.value_);
        } else {
            std::construct_at(&error_, o.error_);
        }
    }
    result(result&& o) noexcept : ok_(o.ok_) {
        if (ok_) {
            std::construct_at(&value_, std::move(o.value_));
        } else {
            std::construct_at(&error_, std::move(o.error_));
        }
    }
    result& operator=(result o) noexcept {
        destroy();
        ok_ = o.ok_;
        if (ok_) {
            std::construct_at(&value_, std::move(o.value_));
        } else {
            std::construct_at(&error_, std::move(o.error_));
        }
        return *this;
    }
    ~result() { destroy(); }

    // has_value reports whether the result holds a T.
    bool has_value() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }

    // value returns the T; it aborts when the result holds an error.
    T& value() & noexcept { return ok_ ? value_ : fail<T&>(); }
    const T& value() const& noexcept { return ok_ ? value_ : fail<const T&>(); }
    T&& value() && noexcept { return ok_ ? std::move(value_) : fail<T&&>(); }

    T& operator*() & noexcept { return value(); }
    const T& operator*() const& noexcept { return value(); }
    T&& operator*() && noexcept { return std::move(*this).value(); }
    T* operator->() noexcept { return &value(); }
    const T* operator->() const noexcept { return &value(); }

    // error returns the E; it aborts when the result holds a value.
    const E& error() const noexcept { return ok_ ? fail<const E&>() : error_; }

private:
    template <class R>
    [[noreturn]] static R fail() noexcept {
        std::abort();
    }

    void destroy() noexcept {
        if (ok_) {
            std::destroy_at(&value_);
        } else {
            std::destroy_at(&error_);
        }
    }

    bool ok_;
    union {
        T value_;
        E error_;
    };
};

}  // namespace k3sm::sdk

#endif  // K3SM_RESULT_HPP
