# k3sm C++ SDK

A low-latency C++ client for the node pressure and headroom API of
[k3sm](https://k3sm.io), the macOS-native Kubernetes distribution for Apple
Silicon. It is for programs that run on a k3sm node and need to react to that
node's resource pressure and headroom quickly.

**Status: the page reader and the change notifier are in.** The SDK reads the
node budget page, ABI major 1: open it, take a stable snapshot, decide from it,
and wait for the node to report a change. The C ABI shim and the dump tool come
next.

## Use it

The SDK is header-only C++20, one CMake target, `k3sm::sdk`.

With FetchContent:

```cmake
include(FetchContent)
FetchContent_Declare(k3sm_sdk
  GIT_REPOSITORY https://github.com/k3sm-io/k3sm-sdk-cpp.git
  GIT_TAG 0123456789abcdef0123456789abcdef01234567  # v1.0.0
  )
FetchContent_MakeAvailable(k3sm_sdk)
target_link_libraries(your_target PRIVATE k3sm::sdk)
```

Pin the full 40-character commit sha and keep the release tag in a comment
beside it. Do not add `GIT_SHALLOW`: CMake shallow-fetches only a tag or a
branch name, so it cannot fetch a sha. The sha and tag above are a placeholder
for the format; use the sha of the release you want.

From an installed copy:

```sh
cmake -S . -B build && cmake --install build --prefix /your/prefix
```

```cmake
find_package(k3sm-sdk CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE k3sm::sdk)
```

Pulled in by another project, the SDK builds no tests and adds no install
rules; set `K3SM_SDK_BUILD_TESTS` or `K3SM_SDK_INSTALL` to `ON` to change that.

## API

Include `<k3sm/budget.hpp>` for everything, or the headers one by one:

| header | what it holds |
|---|---|
| `k3sm/budget_page.hpp` | `budget_page`, the read-only mapping and its seqlock reader |
| `k3sm/change_notifier.hpp` | `change_notifier`, which waits for the page's change word to move |
| `k3sm/snapshot.hpp` | `snapshot`, one stable copy of every page field, and the code enums |
| `k3sm/helpers.hpp` | `admit_ok`, `bytes_until`, `batch_slots` |
| `k3sm/page_layout.hpp` | the page ABI: `page_abi_major`, the paths, sizes and field offsets |
| `k3sm/result.hpp` | `error` and `result<T, E>` |
| `k3sm/version.hpp` | the SDK version, `K3SM_SDK_VERSION_{MAJOR,MINOR,PATCH}` |

Everything is in namespace `k3sm::sdk`.

```cpp
#include <k3sm/budget.hpp>

namespace sdk = k3sm::sdk;

auto page = sdk::budget_page::open(sdk::default_page_path());
if (!page) {
    // page.error() is error::not_found (with sys_errno()) or error::truncated:
    // no page on this node, so fall back to your own sizing.
    return own_sizing();
}
auto s = page->read();
if (!s) {
    return own_sizing();  // abi_mismatch, truncated, corrupt, torn or stale
}
if (sdk::admit_ok(*s)) {
    cap = std::min(cap, sdk::batch_slots(*s, slot_bytes));
}
```

- `budget_page::open(const char* path)` maps the page read-only. The page
  stays mapped until the `budget_page` is destroyed; it is move-only.
- `page.read()` copies every field under the page's seqlock and returns
  `result<snapshot>`. It throws nothing, allocates nothing and never reads the
  environment. `page.read_at(now_unix_seconds)` is the same read with the
  staleness check made against a time you pass.
- Errors (`error::kind`): `not_found` (no page or no access; `sys_errno()` has
  the errno), `abi_mismatch` (bad magic, an unknown ABI major or header size, or
  a code this SDK's tables do not hold), `truncated` (a file shorter than the
  header says, or a body shorter than this SDK knows), `corrupt` (a field out
  of its range), `torn` (no stable copy after `reader_retries` attempts, 64),
  `stale` (the observation is older than `stale_factor`, 3, writer periods).
- A value the producer could not sample is `std::nullopt`. The `darwin` block
  is present on a macOS page, `linux_` on a Linux page and on a vm guest's
  page. Enumerators that would clash with a keyword or a compiler macro carry
  a trailing underscore (`platform::linux_`, `kill_mode::auto_`).
- A page of a newer minor of ABI major 1 reads up to the fields this SDK
  knows. A page of another major is `abi_mismatch`, and a new major also
  changes the file name, so an older SDK finds no page instead of misreading
  one.
- `change_notifier n(*page)` watches the page's `notifyEpoch` word, which the
  node bumps only when the level, `admit`, a threshold or the generation
  changes. `n.wait(timeout)` blocks until it moves (true) or the timeout
  passes (false); `n.poll()` checks it without blocking; `n.epoch()` is the
  last value seen. Several bumps before a wake are reported as one change. The
  wait blocks on the word in the shared mapping (`os_sync_wait_on_address` on
  macOS, `FUTEX_WAIT` on Linux). `change_notifier(page, doorbell_path)` also
  takes a doorbell file, if the node provides one, as the fallback (kqueue on
  macOS, inotify on Linux); `n.source()` says which one a wait uses. The page
  must outlive the notifier. To act on every sample instead, call `read()` on
  your own schedule.

  ```cpp
  sdk::change_notifier bell(*page);
  while (running) {
      if (bell.wait(std::chrono::seconds(1))) {
          auto s = page->read();
          if (s && s->level >= sdk::level::critical) shed_work();
      }
  }
  ```
- `default_page_path()` is `/var/lib/k3sm/budget/budget.v1.page` on macOS
  (`darwin_page_path`) and `/run/k3sm/budget/budget.v1.page` elsewhere
  (`linux_page_path`), fixed at compile time.
- `admit_ok(s)` is false while the node refuses new work.
  `bytes_until(s, signal, rung)` is how many bytes a byte-valued signal can
  move toward a rung (0 at or past it, `std::nullopt` when the page cannot
  say). `batch_slots(s, slot_bytes)` is how many more slots of that size fit
  in the node's safe headroom (0 while it does not admit). Use it only to
  lower your own limit.

The page layout and its golden fixtures come from the k3sm `apis` repository;
the copies under `tests/fixtures/page` are checked against their digests by
`hack/verify-fixtures.sh`.

## Build and test

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
hack/ci.sh          # the full gate
hack/ci.sh --reader # the page reader's tests alone
```

Requirements: CMake 3.24 or later and a C++20 compiler (Apple clang from the
Xcode Command Line Tools is enough). The SDK depends on nothing outside the
standard library.

## Contributing

See `CONTRIBUTING.md`. Every commit is signed off for the Developer
Certificate of Origin (`git commit -s`), and commit subjects follow
Conventional Commits.

## License

Apache-2.0, see `LICENSE`.
