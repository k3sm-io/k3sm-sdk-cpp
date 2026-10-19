# k3sm C++ SDK

A low-latency C++ client for the node pressure and headroom API of
[k3sm](https://k3sm.io), the macOS-native Kubernetes distribution for Apple
Silicon. It is for programs that run on a k3sm node and need to react to that
node's resource pressure and headroom quickly.

**Status: the client API is being designed.** This repository holds the build,
packaging and test scaffolding only. The one public header today is
`k3sm/version.hpp`; nothing here talks to a node yet.

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

## Build and test

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
hack/ci.sh          # the full gate
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
