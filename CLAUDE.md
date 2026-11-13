# k3sm C++ SDK

A header-only C++20 client for the k3sm node pressure and headroom API, one CMake target
(`k3sm::sdk`). It is a leaf: it talks to a k3sm node over the API that node serves and imports no
code from the other k3sm repositories.

> User-facing behavior: `README.md`.

**The v1 page reader, the change notifier and the page writer are in place** (`k3sm/budget.hpp`: `budget_page`,
`snapshot`, the helpers, `change_notifier`, `page_writer`). The C ABI shim and the dump tool are later steps; their
`hack/ci.sh` stages are still placeholders. The notifier tests spawn a writer process, so they run in
`hack/ci.sh --notifier` (ctest label `notifier`), not in the bare gate's unit ctest pass. Build each one to the reviewed design, not ahead of it.

## Layout
- **`include/k3sm/`**: the public headers. Everything public is in namespace `k3sm::sdk`.
- **`tests/`**: ctest targets. They build with warnings as errors and under AddressSanitizer and
  UndefinedBehaviorSanitizer. A toolchain that cannot link those fails `hack/ci.sh` and
  `hack/linux-test.sh` (`hack/lib/sanitizers.sh`) unless `K3SM_SDK_ALLOW_NO_SANITIZERS=1` waives it
  for that run, which prints `WAIVED`; `--stress` takes no waiver, and no script sets it.
- **`cmake/`**: the package config template that `find_package(k3sm-sdk CONFIG)` reads.
- **`hack/`**: the gate and its checks.

## Build / test
```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
hack/ci.sh      # the full gate
```
- `hack/ci.sh` is the gate, and it refuses to run as root before any build step (`hack/lib/nonroot.sh`).
  Its stages: configure, build and ctest as separate sources and as a unity build; install and consume
  through `find_package(k3sm-sdk CONFIG)`; consume through FetchContent with no options; the version
  check and its self-test; the sanitizer self-test (the requirement can fail and honours only its
  waiver); the ctest self-test (every ctest call carries `--no-tests=error`); the non-root self-test;
  shellcheck over `hack/**/*.sh`, with `hack/linux-test.sh` refusing on any other host; license headers;
  dormant workflows; the dependency check and its self-test; the vendored page fixtures against
  `SHA256SUMS`; then `--notifier`; and `--compile-extras`, which builds (never runs) the stress test
  (RelWithDebInfo, ThreadSanitizer) and the benchmark (Release, no sanitizers), the configurations
  `--stress` runs them in. It ends with the `--cshim` and `--linux-vm` placeholders, which only print a
  notice. It does not run `--stress`.
- **No third-party dependencies, ever.** `hack/verify-deps.sh` (run by `ci.sh`) fails on any
  `FetchContent_Declare`, `ExternalProject_Add`, CPM call or git submodule not listed in
  `hack/deps.allow` with an exact commit sha, and the allow file is empty.

## Conventions
- **C++20, header-only.** The target is `INTERFACE`; it requires `cxx_std_20` of its consumers.
- **Unity-build safe.** No anonymous namespaces, no `static` free functions or variables at
  namespace scope in a header: functions are `inline`, constants `inline constexpr`, all in a named
  namespace (`k3sm::sdk`).
- **A consumer needs no options.** FetchContent with `GIT_REPOSITORY` and `GIT_TAG <full 40-hex
  commit sha>` (the release tag in a comment beside it, for example `# v1.0.0`), then
  `FetchContent_MakeAvailable`, gives `k3sm::sdk` and builds nothing else. Never `GIT_SHALLOW`: CMake
  shallow-fetches only a tag or a branch, not a sha.
- **One version source.** The `project()` VERSION in `CMakeLists.txt` generates
  `k3sm/version.hpp` from `include/k3sm/version.hpp.in`; never edit a copy by hand. The package
  version is `SameMajorVersion`; `ARCH_INDEPENDENT` stays only while the SDK is header-only.
  `hack/verify-version.sh` checks that the git tag, the CMake version and the generated header agree.
- Every C and C++ file starts with the Apache-2.0 header in `hack/boilerplate/boilerplate.cpp.txt`;
  `ci.sh` checks it. CMake and shell files carry the two-line `Copyright` and
  `SPDX-License-Identifier` comment.
- Workflows under `.github/workflows/` stay fully commented out; `ci.sh` checks it.

## Commits
Conventional Commits per `CONTRIBUTING.md`. Scopes are the top-level directory changed (`include`,
`tests`, `cmake`, `hack`) or `docs`. Sign off every commit (`git commit -s`).
