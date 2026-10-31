#!/usr/bin/env bash
# Copyright The k3sm Authors.
# SPDX-License-Identifier: Apache-2.0
#
# ci.sh -- the k3sm C++ SDK's gate. Run from anywhere; it works on a lone clone.
#
#   1. cmake configure, build and ctest (C++20, warnings as errors, the tests
#      under AddressSanitizer and UndefinedBehaviorSanitizer when the toolchain
#      supports them), once as separate sources and once as a unity build
#      (CMAKE_UNITY_BUILD), which merges each test target's sources into one
#      translation unit (the version test's two sources, version_test.cpp and
#      second_tu.cpp, check the headers survive that)
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
#   --notifier  the change notifier: the notifier tests (poll, timeout, and
#               a spawned writer process waking the waiter through the
#               notifyEpoch word and through a doorbell file) built and run
#               as separate sources and as a unity build, under the
#               sanitizers where the toolchain supports them
#   --stress    the quality bar, in four parts:
#               (i)   the torn-read stress (a writer thread at full speed
#                     against reader threads) built and run under
#                     ThreadSanitizer
#               (ii)  the libFuzzer harness over arbitrary page bytes, a
#                     bounded smoke (K3SM_SDK_FUZZ_SECONDS, default 30)
#                     seeded from tests/fuzz/corpus and the page fixtures
#               (iii) the whole test suite under AddressSanitizer and
#                     UndefinedBehaviorSanitizer, as separate sources and as
#                     a unity build
#               (iv)  the benchmark, built optimized and run for one short
#                     iteration (it prints the read and wake-to-read
#                     latencies; it checks no threshold)
#               libFuzzer is required. Apple clang ships no libFuzzer
#               runtime, so the stage looks for an LLVM clang++
#               (K3SM_SDK_FUZZ_CXX, then $CXX, then Homebrew's llvm) and
#               fails, naming how to get one, when none links
#               -fsanitize=fuzzer.
#   --cshim, --linux-vm
#               placeholders until their parts of the SDK land
#
# The bare gate runs the unit tests in stage 1 and the notifier tests, which
# spawn a process, through the --notifier stage after stage 9. It does not
# run --stress.
set -euo pipefail
cd "$(dirname "$0")/.."
stage="${1:-}"
case "$stage" in
	--cshim|--linux-vm)
		echo "==> ${stage#--}"
		echo "    not yet implemented"
		exit 0 ;;
	""|--reader|--notifier|--stress) ;;
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

if [ "$stage" = --notifier ]; then
	echo "==> notifier tests (ctest -L notifier)"
	build_and_test -L notifier
	echo "k3sm-sdk-cpp ci --notifier: green"
	exit 0
fi

# fuzz_cxx prints the first C++ compiler that links -fsanitize=fuzzer, or
# nothing.
fuzz_cxx() {
	local c probe="$tmp/fuzz-probe"
	mkdir -p "$probe"
	cat > "$probe/probe.cpp" <<'CPP'
#include <cstddef>
#include <cstdint>
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t*, std::size_t) { return 0; }
CPP
	for c in "${K3SM_SDK_FUZZ_CXX:-}" "${CXX:-c++}" /opt/homebrew/opt/llvm/bin/clang++ /usr/local/opt/llvm/bin/clang++; do
		if [ -z "$c" ] || ! command -v "$c" >/dev/null; then
			continue
		fi
		if "$c" -std=c++20 -fsanitize=fuzzer,address,undefined "$probe/probe.cpp" -o "$probe/probe" >/dev/null 2>&1; then
			command -v "$c"
			return 0
		fi
	done
}

if [ "$stage" = --stress ]; then
	echo "==> libFuzzer toolchain"
	cxx="$(fuzz_cxx)"
	if [ -z "$cxx" ]; then
		echo "FAIL: no C++ compiler here links -fsanitize=fuzzer (libFuzzer)."
		echo "      Apple clang ships no libFuzzer runtime. Install an LLVM clang (on macOS:"
		echo "      'brew install llvm', found at \$(brew --prefix llvm)/bin/clang++; on Linux: the"
		echo "      distribution's clang package) or point K3SM_SDK_FUZZ_CXX at one, then re-run."
		exit 1
	fi
	echo "    $cxx ($("$cxx" --version | head -n 1))"

	echo "==> (i) torn-read stress under ThreadSanitizer (ctest -L stress)"
	b="$tmp/build-tsan"
	cmake -S . -B "$b" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DK3SM_SDK_SANITIZER=thread -DK3SM_SDK_STRESS=ON \
		> "$tmp/configure.log" || { cat "$tmp/configure.log"; echo "FAIL: cmake configure (ThreadSanitizer)"; exit 1; }
	cmake --build "$b" --parallel "$jobs" --target k3sm_sdk_threads_stress_test
	if ! TSAN_OPTIONS="halt_on_error=1 ${TSAN_OPTIONS:-}" ctest --test-dir "$b" --output-on-failure -V -L stress \
		> "$tmp/stress.log" 2>&1; then
		tail -n 60 "$tmp/stress.log"
		echo "FAIL: the torn-read stress"
		exit 1
	fi
	grep -E '^[0-9]+: (shared memory|negative control|page file)' "$tmp/stress.log" | sed 's/^[0-9]*: /    /' || true
	echo "    ok"

	echo "==> (ii) libFuzzer smoke over the page reader (${K3SM_SDK_FUZZ_SECONDS:-30} s)"
	b="$tmp/build-fuzz"
	cmake -S . -B "$b" -DCMAKE_CXX_COMPILER="$cxx" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DK3SM_SDK_FUZZ=ON \
		> "$tmp/configure.log" || { cat "$tmp/configure.log"; echo "FAIL: cmake configure (libFuzzer)"; exit 1; }
	cmake --build "$b" --parallel "$jobs" --target k3sm_sdk_page_fuzz
	mkdir -p "$tmp/fuzz-corpus" "$tmp/fuzz-artifacts"
	if ! "$b/tests/k3sm_sdk_page_fuzz" -max_total_time="${K3SM_SDK_FUZZ_SECONDS:-30}" -max_len=8192 -timeout=10 \
		-print_final_stats=1 -artifact_prefix="$tmp/fuzz-artifacts/" \
		"$tmp/fuzz-corpus" tests/fuzz/corpus tests/fixtures/page > "$tmp/fuzz.log" 2>&1; then
		tail -n 40 "$tmp/fuzz.log"
		mkdir -p build/fuzz-artifacts
		cp "$tmp"/fuzz-artifacts/* build/fuzz-artifacts/ 2>/dev/null || true
		echo "FAIL: the page fuzzer found a failing input (kept under build/fuzz-artifacts)"
		exit 1
	fi
	grep -E '^(Done|stat::number_of_executed_units|stat::new_units_added)' "$tmp/fuzz.log" | sed 's/^/    /'
	echo "    ok"

	echo "==> (iii) the test suite under AddressSanitizer and UndefinedBehaviorSanitizer"
	build_and_test
	for unity in OFF ON; do
		grep -q 'K3SM_SDK_HAVE_SANITIZERS:INTERNAL=1' "$tmp/build-unity-$unity/CMakeCache.txt" \
			|| { echo "FAIL: this toolchain cannot build the tests with -fsanitize=address,undefined"; exit 1; }
	done

	echo "==> (iv) benchmark, one short iteration (optimized, no sanitizers)"
	b="$tmp/build-bench"
	cmake -S . -B "$b" -DCMAKE_BUILD_TYPE=Release -DK3SM_SDK_SANITIZER=none -DK3SM_SDK_STRESS=ON \
		> "$tmp/configure.log" || { cat "$tmp/configure.log"; echo "FAIL: cmake configure (benchmark)"; exit 1; }
	cmake --build "$b" --parallel "$jobs" --target k3sm_sdk_page_bench
	"$b/tests/k3sm_sdk_page_bench" --quick > "$tmp/bench.log" 2>&1 \
		|| { cat "$tmp/bench.log"; echo "FAIL: the benchmark"; exit 1; }
	sed 's/^/    /' "$tmp/bench.log"
	echo "    ok (a smoke run; real numbers need a full run on idle hardware)"
	echo "k3sm-sdk-cpp ci --stress: green"
	exit 0
fi

echo "==> cmake configure, build, ctest (unit; the notifier runs below)"
build_and_test -LE notifier

echo "==> install and find_package(k3sm-sdk CONFIG)"
cmake --install "$tmp/build-unity-OFF" --prefix "$tmp/prefix" > /dev/null
for h in version.hpp budget.hpp budget_page.hpp change_notifier.hpp helpers.hpp page_layout.hpp page_writer.hpp result.hpp snapshot.hpp; do
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

echo "==> notifier (hack/ci.sh --notifier)"
bash hack/ci.sh --notifier | sed 's/^/    /'

echo "==> placeholder stages (--cshim --linux-vm)"
for st in --cshim --linux-vm; do
	bash hack/ci.sh "$st" | sed 's/^/    /'
done

echo "k3sm-sdk-cpp ci: green"
