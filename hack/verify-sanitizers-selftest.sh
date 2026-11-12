#!/usr/bin/env bash
# Copyright The k3sm Authors.
# SPDX-License-Identifier: Apache-2.0
#
# verify-sanitizers-selftest.sh -- check that require_sanitizers in
# hack/lib/sanitizers.sh can fail. Each case writes a fixture CMake cache
# under a temp dir and calls the helper on it, with the waiver unset, set to
# 1 and set to another value, asserting the return code and the output:
#   - a cache whose probe linked the sanitizers passes silently;
#   - a cache whose probe failed, and one with no probe entry, fail with the
#     FAIL text unless the waiver is exactly 1, which passes with a WAIVED
#     line.
# It also checks statically that no script under hack/ sets the waiver and
# that hack/ci.sh and hack/linux-test.sh each call require_sanitizers.
set -euo pipefail
cd "$(dirname "$0")/.."
# shellcheck source=hack/lib/sanitizers.sh
. hack/lib/sanitizers.sh

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

printf '%s\n' 'CMAKE_BUILD_TYPE:STRING=Debug' 'K3SM_SDK_HAVE_SANITIZERS:INTERNAL=1' > "$tmp/linked.txt"
printf '%s\n' 'CMAKE_BUILD_TYPE:STRING=Debug' 'K3SM_SDK_HAVE_SANITIZERS:UNINITIALIZED=0' > "$tmp/failed.txt"
printf '%s\n' 'CMAKE_BUILD_TYPE:STRING=Debug' > "$tmp/absent.txt"

fail_text="FAIL: this toolchain cannot link -fsanitize=address,undefined"
waived_text="WAIVED: K3SM_SDK_ALLOW_NO_SANITIZERS=1:"

# Each case calls the helper in a fresh shell, so the waiver's value is
# exactly what env gives it.
# shellcheck disable=SC2016 # $1 expands in the child shell
call='. hack/lib/sanitizers.sh; require_sanitizers "$1"'

# expect <fixture> <waiver: unset|value> <want-rc> <want-text: empty for silent>
expect() {
	local out rc=0
	if [ "$2" = unset ]; then
		out="$(env -u K3SM_SDK_ALLOW_NO_SANITIZERS bash -c "$call" _ "$tmp/$1.txt" 2>&1)" || rc=$?
	else
		out="$(env K3SM_SDK_ALLOW_NO_SANITIZERS="$2" bash -c "$call" _ "$tmp/$1.txt" 2>&1)" || rc=$?
	fi
	[ "$rc" = "$3" ] || { echo "FAIL: $1, waiver $2: exit $rc, want $3: $out"; exit 1; }
	if [ -z "$4" ]; then
		[ -z "$out" ] || { echo "FAIL: $1, waiver $2: want no output, got: $out"; exit 1; }
		return 0
	fi
	case "$out" in
	"$4"*) ;;
	*) echo "FAIL: $1, waiver $2: output does not start with '$4': $out"; exit 1 ;;
	esac
	case "$4" in
	FAIL:*)
		case "$out" in
		*K3SM_SDK_ALLOW_NO_SANITIZERS=1*) ;;
		*) echo "FAIL: $1, waiver $2: the FAIL text does not name the waiver: $out"; exit 1 ;;
		esac
		case "$out" in
		*"Only Clang and GNU compilers are probed"*) ;;
		*) echo "FAIL: $1, waiver $2: the FAIL text does not say which compilers are probed: $out"; exit 1 ;;
		esac ;;
	esac
}

for waiver in unset 1 yes; do
	expect linked "$waiver" 0 ""
done
for fixture in failed absent; do
	expect "$fixture" unset 1 "$fail_text"
	expect "$fixture" 1 0 "$waived_text"
	expect "$fixture" yes 1 "$fail_text"
	expect "$fixture" 0 1 "$fail_text"
done
sanitizers_linked "$tmp/linked.txt" || { echo "FAIL: sanitizers_linked missed a linked cache"; exit 1; }
! sanitizers_linked "$tmp/failed.txt" || { echo "FAIL: sanitizers_linked accepted a failed probe"; exit 1; }

# Static checks: no script sets the waiver itself, and both entry points that
# build the tests call require_sanitizers.
setters="$(grep -rnE '^[^#]*(export[[:space:]]+|^[[:space:]]*|[;&|][[:space:]]*)K3SM_SDK_ALLOW_NO_SANITIZERS=' \
	--include='*.sh' hack | grep -v '^hack/verify-sanitizers-selftest.sh:' || true)"
[ -z "$setters" ] || { echo "FAIL: a script under hack/ sets K3SM_SDK_ALLOW_NO_SANITIZERS; only the caller may: $setters"; exit 1; }
for script in hack/ci.sh hack/linux-test.sh; do
	grep -qE '^[^#]*require_sanitizers ' "$script" \
		|| { echo "FAIL: $script does not call require_sanitizers"; exit 1; }
done

echo "    self-test: linked passes silently; a failed or absent probe fails unless K3SM_SDK_ALLOW_NO_SANITIZERS is exactly 1, which prints WAIVED"
