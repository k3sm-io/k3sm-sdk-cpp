# k3sm C++ SDK

A header-only C++20 client for the k3sm node pressure and headroom API, one CMake target
(`k3sm::sdk`). It is a leaf: it talks to a k3sm node over the API that node serves and imports no
code from the other k3sm repositories.

> User-facing behavior: `README.md`.

**The client API is under design review.** Until that design lands, the repository carries only the
build, packaging and test scaffolding and `include/k3sm/version.hpp`. Do not add client code ahead
of the reviewed design.

## Layout
- **`include/k3sm/`**: the public headers. Everything public is in namespace `k3sm::sdk`.
- **`tests/`**: ctest targets. They build with warnings as errors and, where the toolchain
  supports it, AddressSanitizer and UndefinedBehaviorSanitizer.
- **`cmake/`**: the package config template that `find_package(k3sm-sdk CONFIG)` reads.
- **`hack/`**: the gate and its checks.

## Build / test
```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
hack/ci.sh      # the full gate
```
- `hack/ci.sh` is the gate: configure, build and ctest as separate sources and as a unity build;
  install and consume through `find_package(k3sm-sdk CONFIG)`; consume through FetchContent with no
  options; shellcheck; license headers; dormant workflows; the dependency check.
- **No third-party dependencies, ever.** `hack/verify-deps.sh` (run by `ci.sh`) fails on any
  `FetchContent_Declare`, `ExternalProject_Add`, CPM call or git submodule not listed in
  `hack/deps.allow` with an exact commit sha, and the allow file is empty.

## Conventions
- **C++20, header-only.** The target is `INTERFACE`; it requires `cxx_std_20` of its consumers.
- **Unity-build safe.** No anonymous namespaces, no `static` free functions or variables at
  namespace scope in a header: functions are `inline`, constants `inline constexpr`, all in a named
  namespace (`k3sm::sdk`).
- **A consumer needs no options.** FetchContent with `GIT_REPOSITORY`, `GIT_TAG` and
  `GIT_SHALLOW TRUE`, then `FetchContent_MakeAvailable`, gives `k3sm::sdk` and builds nothing else.
- Every C and C++ file starts with the Apache-2.0 header in `hack/boilerplate/boilerplate.cpp.txt`;
  `ci.sh` checks it. CMake and shell files carry the two-line `Copyright` and
  `SPDX-License-Identifier` comment.
- Workflows under `.github/workflows/` stay fully commented out; `ci.sh` checks it.

## Commits
Conventional Commits per `CONTRIBUTING.md`. Scopes are the top-level directory changed (`include`,
`tests`, `cmake`, `hack`) or `docs`. Sign off every commit (`git commit -s`).
