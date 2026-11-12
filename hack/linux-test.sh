#!/usr/bin/env bash
# Copyright The k3sm Authors.
# SPDX-License-Identifier: Apache-2.0
#
# linux-test.sh -- configure, build and run the tests on a Linux host. Run
# from anywhere; it works on a lone clone.
#
# It builds the tests twice, as hack/ci.sh's first stage does on macOS: once
# as separate sources and once as a unity build (CMAKE_UNITY_BUILD), under
# AddressSanitizer and UndefinedBehaviorSanitizer, and runs every ctest test in
# both builds. The compiler is $CXX (default c++). The build directories live
# under a temporary directory that is removed on exit.
#
# A compiler that cannot link -fsanitize=address,undefined fails the run
# (hack/lib/sanitizers.sh; only Clang and GNU compilers are probed), unless
# K3SM_SDK_ALLOW_NO_SANITIZERS=1 waives it: the tests then build and run
# without sanitizers and each build prints a WAIVED line.
#
# On any other system it refuses with exit status 2 before doing anything,
# so it never reports a Linux result it did not produce.
set -euo pipefail

uname_s="$(uname -s)"
if [ "$uname_s" != Linux ]; then
	echo "linux-test: refused: not a Linux host ($uname_s)" >&2
	exit 2
fi

cd "$(dirname "$0")/.."
# shellcheck source=hack/lib/sanitizers.sh
. hack/lib/sanitizers.sh

for tool in cmake ctest; do
	command -v "$tool" >/dev/null || { echo "FAIL: $tool is not installed"; exit 1; }
done
cxx="${CXX:-c++}"
cxx_path="$(command -v "$cxx")" || { echo "FAIL: the C++ compiler '$cxx' is not installed"; exit 1; }
echo "==> compiler: $cxx_path ($("$cxx_path" --version | head -n 1))"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
jobs="$(nproc 2>/dev/null || echo 2)"

for unity in OFF ON; do
	echo "==> cmake configure, build, ctest (unity $unity)"
	b="$tmp/build-unity-$unity"
	cmake -S . -B "$b" -DCMAKE_CXX_COMPILER="$cxx_path" -DCMAKE_BUILD_TYPE=Debug \
		-DCMAKE_UNITY_BUILD="$unity" > "$tmp/configure.log" \
		|| { cat "$tmp/configure.log"; echo "FAIL: cmake configure (unity $unity)"; exit 1; }
	require_sanitizers "$b/CMakeCache.txt" || exit 1
	if sanitizers_linked "$b/CMakeCache.txt"; then
		san="with sanitizers"
	else
		san="WITHOUT sanitizers (waived)"
	fi
	cmake --build "$b" --parallel "$jobs"
	ctest --test-dir "$b" --output-on-failure --no-tests=error
	echo "    ok (unity $unity, $san)"
done

echo "linux-test: ok ($cxx)"
