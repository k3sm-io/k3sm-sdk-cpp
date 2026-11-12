# Copyright The k3sm Authors.
# SPDX-License-Identifier: Apache-2.0
# shellcheck shell=bash
#
# sanitizers.sh -- sourced by hack/ci.sh and hack/linux-test.sh. The tests
# must build under AddressSanitizer and UndefinedBehaviorSanitizer; a
# toolchain that cannot link them fails the gate unless the caller sets
# K3SM_SDK_ALLOW_NO_SANITIZERS=1, a named waiver that is printed on every run.
# No script sets the waiver itself.

# sanitizers_linked <cache-file>: 0 when the CMake probe in tests/CMakeLists.txt
# linked -fsanitize=address,undefined for the build behind <cache-file>.
sanitizers_linked() {
	grep -q 'K3SM_SDK_HAVE_SANITIZERS:INTERNAL=1' "$1"
}

# require_sanitizers <cache-file>: 0 when the sanitizers linked (silently) or
# when the waiver is set to exactly 1 (with a WAIVED line); otherwise prints a
# FAIL message and returns 1.
require_sanitizers() {
	if sanitizers_linked "$1"; then
		return 0
	fi
	if [ "${K3SM_SDK_ALLOW_NO_SANITIZERS:-}" = 1 ]; then
		echo "WAIVED: K3SM_SDK_ALLOW_NO_SANITIZERS=1: the tests are built and run WITHOUT sanitizers"
		return 0
	fi
	echo "FAIL: this toolchain cannot link -fsanitize=address,undefined, so the tests would run without sanitizers."
	echo "      Only Clang and GNU compilers are probed. Use one that links them, or set"
	echo "      K3SM_SDK_ALLOW_NO_SANITIZERS=1 to run the tests without sanitizers (the run says so)."
	return 1
}
