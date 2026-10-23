#!/usr/bin/env bash
# Copyright The k3sm Authors.
# SPDX-License-Identifier: Apache-2.0
#
# ci.sh -- the k3sm C++ SDK's gate. Run from anywhere; it works on a lone clone.
#
#   1. cmake configure, build and ctest (C++20, warnings as errors, the tests
#      under AddressSanitizer and UndefinedBehaviorSanitizer when the toolchain
#      supports them), once as separate sources and once as a unity build
#      (CMAKE_UNITY_BUILD), which merges the two test sources into one
#   2. install into a temp prefix, then build a consumer that uses
#      find_package(k3sm-sdk CONFIG) and links k3sm::sdk
#   3. build a consumer that uses FetchContent with no options from a git
#      clone of this checkout and links k3sm::sdk
#   4. the version check: tag, CMake version and generated header agree
#   5. shellcheck over every hack/**/*.sh
#   6. every C and C++ file carries the Apache-2.0 header
#   7. every file under .github/workflows/ is dormant (fully commented out)
#   8. the dependency supply-chain check: its self-test, then the live tree
#   9. the vendored page fixtures match their SHA256SUMS
#      (hack/verify-fixtures.sh, after its self-test)
#
# A stage name as the first argument runs that stage alone:
#   --reader    the page reader: the fixture check, then the reader tests
#               (golden and negative fixtures, layout, torn reads) built and
#               run as separate sources and as a unity build, under the
#               sanitizers where the toolchain supports them
#   --notifier, --cshim, --stress, --linux-vm
#               placeholders until their parts of the SDK land
set -euo pipefail
cd "$(dirname "$0")/.."
stage="${1:-}"
case "$stage" in
	--notifier|--cshim|--stress|--linux-vm)
		echo "==> ${stage#--}"
		echo "    not yet implemented"
		exit 0 ;;
	""|--reader) ;;
	*) echo "usage: hack/ci.sh [--reader|--notifier|--cshim|--stress|--linux-vm]"; exit 2 ;;
esac
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
jobs="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 2)"

for tool in cmake ctest git; do
	command -v "$tool" >/dev/null || { echo "FAIL: $tool is not installed"; exit 1; }
done

# build_and_test [ctest args...]: configure, build and ctest once as
# separate sources and once as a unity build.
build_and_test() {
	local unity b san
	for unity in OFF ON; do
		b="$tmp/build-unity-$unity"
		cmake -S . -B "$b" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_UNITY_BUILD="$unity" > "$tmp/configure.log" \
			|| { cat "$tmp/configure.log"; echo "FAIL: cmake configure (unity $unity)"; exit 1; }
		grep -q 'K3SM_SDK_HAVE_SANITIZERS:INTERNAL=1' "$b/CMakeCache.txt" \
			&& san="with sanitizers" || san="without sanitizers (unsupported here)"
		cmake --build "$b" --parallel "$jobs"
		ctest --test-dir "$b" --output-on-failure "$@"
		echo "    ok (unity $unity, $san)"
	done
}

verify_fixtures() {
	echo "==> page fixtures (hack/verify-fixtures.sh)"
	bash hack/verify-fixtures.sh --self-test
	bash hack/verify-fixtures.sh
}

if [ "$stage" = --reader ]; then
	verify_fixtures
	echo "==> reader tests (ctest -L reader)"
	build_and_test -L reader
	echo "k3sm-sdk-cpp ci --reader: green"
	exit 0
fi

echo "==> cmake configure, build, ctest"
build_and_test

echo "==> install and find_package(k3sm-sdk CONFIG)"
cmake --install "$tmp/build-unity-OFF" --prefix "$tmp/prefix" > /dev/null
for h in version.hpp budget.hpp budget_page.hpp helpers.hpp page_layout.hpp result.hpp snapshot.hpp; do
	[ -f "$tmp/prefix/include/k3sm/$h" ] || { echo "FAIL: k3sm/$h was not installed"; exit 1; }
done
consumer() {
	mkdir -p "$1"
	cat > "$1/main.cpp" <<'CPP'
#include <k3sm/budget.hpp>
int main() {
    static_assert(k3sm::sdk::page_abi_major == 1);
    auto page = k3sm::sdk::budget_page::open("/nonexistent/budget.v1.page");
    if (page || page.error() != k3sm::sdk::error::not_found) return 1;
    return k3sm::sdk::version.major == K3SM_SDK_VERSION_MAJOR ? 0 : 1;
}
CPP
	printf '%s\n' "$2" > "$1/CMakeLists.txt"
	cat >> "$1/CMakeLists.txt" <<'CMAKE'
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE k3sm::sdk)
get_target_property(features k3sm::sdk INTERFACE_COMPILE_FEATURES)
if(NOT features MATCHES "cxx_std_20")
  message(FATAL_ERROR "k3sm::sdk does not require C++20: ${features}")
endif()
CMAKE
}
consumer "$tmp/find" 'cmake_minimum_required(VERSION 3.24)
project(consumer LANGUAGES CXX)
find_package(k3sm-sdk CONFIG QUIET)
if(NOT k3sm-sdk_FOUND)
  message(FATAL_ERROR "find_package(k3sm-sdk CONFIG) did not find the installed SDK")
endif()'
cmake -S "$tmp/find" -B "$tmp/find/build" -DCMAKE_PREFIX_PATH="$tmp/prefix" > "$tmp/find.log" \
	|| { cat "$tmp/find.log"; echo "FAIL: the find_package consumer did not configure"; exit 1; }
cmake --build "$tmp/find/build" > /dev/null
"$tmp/find/build/consumer" || { echo "FAIL: the find_package consumer exited non-zero"; exit 1; }
echo "    ok"

echo "==> FetchContent with no options"
# A git repository holding this checkout's files as they are (tracked and
# untracked, minus ignored ones), served over file://, stands in for the
# GitHub repository, so the gate checks the tree it runs on.
src="$tmp/src"
mkdir -p "$src"
git ls-files -co --exclude-standard -z | while IFS= read -r -d '' f; do
	[ -f "$f" ] || continue
	mkdir -p "$src/$(dirname "$f")"; cp -p "$f" "$src/$f"
done
git -C "$src" init -q
git -C "$src" add -A
git -C "$src" -c user.name=ci -c user.email=ci@invalid -c commit.gpgsign=false commit -q -m snapshot
rev="$(git -C "$src" rev-parse HEAD)"
consumer "$tmp/fetch" "cmake_minimum_required(VERSION 3.24)
project(consumer LANGUAGES CXX)
include(FetchContent)
FetchContent_Declare(k3sm_sdk
  GIT_REPOSITORY file://$src/.git
  GIT_TAG $rev)
FetchContent_MakeAvailable(k3sm_sdk)
if(TARGET k3sm_sdk_version_test)
  message(FATAL_ERROR \"the SDK built its tests inside a consumer\")
endif()"
cmake -S "$tmp/fetch" -B "$tmp/fetch/build" > "$tmp/fetch.log" \
	|| { cat "$tmp/fetch.log"; echo "FAIL: the FetchContent consumer did not configure"; exit 1; }
cmake --build "$tmp/fetch/build" > /dev/null
"$tmp/fetch/build/consumer" || { echo "FAIL: the FetchContent consumer exited non-zero"; exit 1; }
echo "    ok"

echo "==> version (tag, CMake, generated header)"
bash hack/verify-version.sh "$tmp/build-unity-OFF"

echo "==> shellcheck (hack/**/*.sh)"
command -v shellcheck >/dev/null || { echo "FAIL: shellcheck is not installed"; exit 1; }
find hack -name '*.sh' -print0 | xargs -0 shellcheck
echo "    ok"

echo "==> license headers (C and C++)"
bash hack/verify-boilerplate.sh --self-test
bash hack/verify-boilerplate.sh

echo "==> workflows dormant (.github/workflows)"
# A workflow file is dormant iff it has no line that is neither blank nor a
# comment. Every file counts, whatever its name or extension. Deliberately
# strict: a stray '---' or a byte-order mark makes a file active.
active=0
if [ -d .github/workflows ]; then
	while IFS= read -r -d '' f; do
		if grep -qEv '^[[:space:]]*(#|$)' "$f"; then
			echo "    ACTIVE: $f"; active=1
		fi
	done < <(find .github/workflows -type f -print0)
fi
[ "$active" = 0 ] || { echo "FAIL: workflows must stay fully commented out"; exit 1; }
# Red-path check, so the predicate above cannot pass vacuously.
printf '# off\non: push\n' > "$tmp/active.yml"
printf '# off\n\n  # still off\n' > "$tmp/dormant.yml"
grep -qEv '^[[:space:]]*(#|$)' "$tmp/active.yml" || { echo "FAIL: predicate missed an active file"; exit 1; }
! grep -qEv '^[[:space:]]*(#|$)' "$tmp/dormant.yml" || { echo "FAIL: predicate flagged a dormant file"; exit 1; }
echo "    ok"

echo "==> dependency supply-chain (hack/verify-deps.sh)"
bash hack/verify-deps.sh --self-test
bash hack/verify-deps.sh

verify_fixtures

echo "==> placeholder stages (--notifier --cshim --stress --linux-vm)"
for st in --notifier --cshim --stress --linux-vm; do
	bash hack/ci.sh "$st" | sed 's/^/    /'
done

echo "k3sm-sdk-cpp ci: green"
