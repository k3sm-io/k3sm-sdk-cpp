# k3sm C++ SDK

A low-latency C++ client for the node pressure and headroom API of
[k3sm](https://k3sm.io), the macOS-native Kubernetes distribution for Apple
Silicon. It is for programs that run on a k3sm node and need to react to that
node's resource pressure and headroom quickly.

**Status: the page reader, the change notifier and the page writer are in.** The
SDK reads the node budget page, ABI major 1: open it, take a stable snapshot,
decide from it, and wait for the node to report a change. A producer other than
k3sm can also publish the page with `page_writer`. The C ABI shim and the dump
tool come next.

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
| `k3sm/page_writer.hpp` | `page_writer`, `encode_page` and `notify_due`: the seqlock writer for producers other than k3sm |
| `k3sm/page_layout.hpp` | the page ABI: `page_abi_major`, the paths, sizes and field offsets |
| `k3sm/result.hpp` | `error` and `result<T, E>` |
| `k3sm/version.hpp` | the SDK version, `K3SM_SDK_VERSION_{MAJOR,MINOR,PATCH}` |

Everything is in namespace `k3sm::sdk`.

```cpp
#include <k3sm/budget.hpp>

#include <algorithm>
#include <cstdint>

namespace sdk = k3sm::sdk;

// cap is your own concurrency limit; slot_bytes is what one unit of work needs.
std::int64_t size_batch(std::int64_t cap, std::int64_t slot_bytes) {
    auto page = sdk::budget_page::open(sdk::default_page_path());
    if (!page) {
        // page.error() is error::not_found (with sys_errno()) or error::truncated:
        // no page on this node, so keep your own sizing.
        return cap;
    }
    auto s = page->read();
    if (!s) {
        return cap;  // abi_mismatch, truncated, corrupt, torn or stale
    }
    // batch_slots is 0 while the node does not admit, so this only lowers cap.
    return std::min(cap, sdk::batch_slots(*s, slot_bytes));
}
```

- `budget_page::open(const char* path)` maps the page read-only. The page
  stays mapped until the `budget_page` is destroyed; it is move-only. `open`
  never blocks, so a FIFO or device at the path fails at once. It accepts only
  a regular file, follows a symlink and checks its target, and maps at most
  one page of a longer file. `open` fails with `not_found` for a missing,
  inaccessible or non-regular path and with `truncated` for an empty file. The
  ABI, header size and body size are checked on every read, not at `open`.
- `page.read()` copies every field under the page's seqlock and returns
  `result<snapshot>`. It throws nothing, allocates nothing and never reads the
  environment. `page.read_at(now_unix_seconds)` is the same read with the
  staleness check made against a time you pass.
- Errors (`error::kind`): `not_found` (no page or no access, or a path that is
  not a regular file; `sys_errno()` has the errno, `EISDIR` for a directory and
  `EINVAL` for anything else that is not a regular file), `abi_mismatch` (bad
  magic, an unknown ABI major or header size, or a code this SDK's tables do not
  hold), `truncated` (a file shorter than the header says, or a body shorter
  than its minor's), `corrupt` (a field out of its range), `torn` (no stable
  copy after `reader_retries` attempts, 64), `stale` (the observation is older
  than `stale_factor`, 3, writer periods).
- A writer that takes the page over rewrites its header in place, so a read
  loads a header that fails the `abi_mismatch` or `truncated` checks again, up
  to 64 times, before it returns that error.
- A value the producer could not sample is `std::nullopt`. The `darwin` block
  is present on a macOS page, `linux_` on a Linux page and on a vm guest's
  page. Enumerators that would clash with a keyword or a compiler macro carry
  a trailing underscore (`platform::linux_`, `kill_mode::auto_`).
- A page of a newer minor of ABI major 1 reads up to the fields this SDK
  knows (minor 1). A minor 0 page reads too, and the fields minor 1 added
  read `std::nullopt`: the capacity's `cpu_performance_cores`,
  `cpu_efficiency_cores`, `gpu_present` (a `std::optional<bool>`) and
  `gpu_count`. A page of another major is `abi_mismatch`, and a new major also
  changes the file name, so an older SDK finds no page instead of misreading
  one.
- `change_notifier n(*page)` watches the page's `notifyEpoch` word, which the
  node bumps only when the level, `admit`, a threshold or the generation
  changes. `n.wait(timeout)` blocks until it moves (true) or the timeout passes
  (false); `n.poll()` checks it without blocking; `n.epoch()` is the last value
  seen. A notifier starts from the epoch on the page when it is built; if you
  read a snapshot first, call `n.seed(s->notify_epoch)` so a bump between the
  read and the construction is reported by the first wait.
  `n.wait(std::chrono::nanoseconds::max())` waits with no deadline. Several
  bumps before a wake are reported as one change.

  The wait blocks on the word in the shared mapping (`os_sync_wait_on_address`
  on macOS, `FUTEX_WAIT` on Linux). `change_notifier(page, doorbell_path)` also
  takes a doorbell file, if the node provides one, as the fallback (kqueue on
  macOS, inotify on Linux); `n.source()` says which one a wait uses: `address`
  (the word in the mapping), `doorbell` (the doorbell file), `sleep` (neither is
  available, so the word is polled) or `none` (the mapping is too short to hold
  the word, so nothing changes). One wait is cut into slices of at most 24
  hours. When the address wait fails for good, the notifier falls back to the
  doorbell if one is open, and only without a doorbell does it sleep and poll
  the word, in steps of at most 1 millisecond. On macOS the address wait needs
  macOS 14.4 or later at run time; on an older system the notifier starts on
  the doorbell if one is open, and otherwise polls.

  A third argument, `change_notifier(page, doorbell_path, prefer)`, takes a
  `wake_source`: `wake_source::doorbell` waits on the doorbell from the start
  when it opened, and any other value (the default is `wake_source::address`)
  keeps the word as the primary. While the doorbell is the wake source, the wait
  re-checks the word every 100 milliseconds at the longest, because a producer
  may bump the word without writing the doorbell. If the doorbell wait itself
  fails, the notifier polls the word. On macOS a doorbell path that is not a
  regular file (a FIFO, for example) is ignored like one that cannot be opened,
  and the notifier uses its other wake source.

  The page must outlive the notifier. To act on every sample instead, call
  `read()` on your own schedule.

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

## Writing the page

A producer other than k3sm publishes the page with `page_writer`. Open it with
the page path, a lock file path and, optionally, a doorbell path, then publish a
`snapshot`:

```cpp
auto w = sdk::page_writer::open("/run/app/budget.v1.page", "/run/app.lock",
                                "/run/app/budget.doorbell");
if (!w) return 1;                // w.error().name() says which error
auto seq = w->publish(snap);     // the new, even seq, or an error
```

- `open` takes an exclusive lock on the lock file, creates the page (mode 0644,
  sized to `page_size`) if it is missing, and takes over an existing one without
  truncating or replacing it. A reader's open mapping survives a writer restart.
  The writer's generation is the page's previous generation plus one. It
  restarts at 1 on a new page, on a page with a foreign header, and when the
  stored generation is negative or has no successor (the largest signed 64-bit
  value).
- The lock file must not be in the page's directory, since a pod can open that
  directory. There is one writer per page.
- `publish` refuses a snapshot the layout cannot carry before it stores
  anything. It bumps `notifyEpoch`, wakes the waiters and writes the doorbell
  on the first publish, and after that only when `notify_due` says the level,
  `admit`, the generation or a threshold changed. `publish` writes the
  writer's generation, not `snap.generation`. `bump_generation()` adds one on a
  policy change, and restarts at 1 from the largest signed 64-bit value.
- The writer-side error kinds are `writer_busy` (another writer holds the lock),
  `invalid_argument` (from `open`: a null path, or a lock file in the page's
  directory), `invalid_snapshot`, `node_name_too_long` and `io`, which
  carries the `errno` of the failing call in `sys_errno()`.
- A `page_writer` is move-only and not safe for concurrent use from several
  threads.

## Build and test

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
hack/ci.sh            # the full gate
hack/ci.sh --reader   # the page reader's tests alone
hack/ci.sh --notifier # the change notifier's tests alone
hack/ci.sh --compile-extras # build (not run) the stress test (RelWithDebInfo, TSan) and the bench (Release)
hack/ci.sh --stress   # ThreadSanitizer stress, fuzz smoke, ASan/UBSan, bench
```

Requirements: CMake 3.24 or later and a C++20 compiler (Apple clang from the
Xcode Command Line Tools is enough). The SDK depends on nothing outside the
standard library. `hack/ci.sh --stress` also needs a clang with libFuzzer, which
Apple clang does not ship: on macOS, `brew install llvm` provides one, and
`K3SM_SDK_FUZZ_CXX` can name any other.

The tests build and run under AddressSanitizer and UndefinedBehaviorSanitizer.
When the compiler cannot link `-fsanitize=address,undefined`, `hack/ci.sh` and
`hack/linux-test.sh` fail and say so. Setting `K3SM_SDK_ALLOW_NO_SANITIZERS=1`
runs the tests without the sanitizers instead, and the run prints a `WAIVED`
line; `hack/ci.sh --stress` takes no such waiver.

`hack/ci.sh` is the macOS gate and does not build the Linux tests. On a Linux
host, run `hack/linux-test.sh`: it builds and runs the tests with the compiler
named by `CXX`.

## Contributing

See `CONTRIBUTING.md`. Every commit is signed off for the Developer
Certificate of Origin (`git commit -s`), and commit subjects follow
Conventional Commits.

## License

Apache-2.0, see `LICENSE`.
