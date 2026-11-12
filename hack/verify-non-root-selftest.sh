#!/usr/bin/env bash
# Copyright The k3sm Authors.
# SPDX-License-Identifier: Apache-2.0
#
# verify-non-root-selftest.sh -- check that require_non_root in
# hack/lib/nonroot.sh can fail. Each case calls the helper in a fresh shell
# with a uid argument and asserts the return code and the output:
#   - uid 0 fails with the FAIL text, which names the reason;
#   - uid 501 passes silently;
#   - an empty-looking or non-numeric uid fails.
# It also checks statically that hack/ci.sh and hack/linux-test.sh each call
# require_non_root, with no argument, before their first build step (the
# first line that runs cmake, ctest or build_and_test). The static check is
# itself tested on fixture scripts: a script without the call, with the call
# after a build step, or passing a uid must each be found.
set -euo pipefail
cd "$(dirname "$0")/.."
# shellcheck source=hack/lib/nonroot.sh
. hack/lib/nonroot.sh

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

# shellcheck disable=SC2016 # $1 expands in the child shell
call='. hack/lib/nonroot.sh; require_non_root "$1"'

# expect <uid> <want-rc> <want-text: empty for silent>
expect() {
	local out rc=0
	out="$(bash -c "$call" _ "$1" 2>&1)" || rc=$?
	[ "$rc" = "$2" ] || { echo "FAIL: uid '$1': exit $rc, want $2: $out"; exit 1; }
	if [ -z "$3" ]; then
		[ -z "$out" ] || { echo "FAIL: uid '$1': want no output, got: $out"; exit 1; }
		return 0
	fi
	case "$out" in
	"$3"*) ;;
	*) echo "FAIL: uid '$1': output does not start with '$3': $out"; exit 1 ;;
	esac
}

expect 0 1 "FAIL: refusing to run as root"
out="$(bash -c "$call" _ 0 2>&1)" || true
for want in "permission-denied cases" "reader and writer tests skip as root" "Run as a non-root user"; do
	case "$out" in
	*"$want"*) ;;
	*) echo "FAIL: the root refusal does not say '$want': $out"; exit 1 ;;
	esac
done
expect 501 0 ""
expect 00 1 "FAIL: refusing to run as root"
expect abc 1 "FAIL: cannot read the effective uid"
expect -1 1 "FAIL: cannot read the effective uid"
# With no argument the helper reads id -u, so it agrees with id -u.
rc=0; bash -c '. hack/lib/nonroot.sh; require_non_root' > /dev/null 2>&1 || rc=$?
if [ "$(id -u)" = 0 ]; then want=1; else want=0; fi
[ "$rc" = "$want" ] || { echo "FAIL: require_non_root with no argument exited $rc as uid $(id -u), want $want"; exit 1; }

# gate_check <file>: 0 when <file> calls require_non_root with no argument
# (as `require_non_root`, optionally followed by || ...) on a non-comment
# line before its first non-comment line that runs cmake, ctest or
# build_and_test; otherwise prints why and returns 1.
gate_check() {
	# shellcheck disable=SC2016 # awk program, not shell
	awk -v f="$1" '
		/^[[:space:]]*#/ { next }
		call == 0 && /^[[:space:]]*require_non_root[[:space:]]*(\|\||;|$)/ { call = NR }
		build == 0 && /(^|[^A-Za-z0-9_.-])(cmake|ctest|build_and_test)([[:space:]]|$)/ { build = NR }
		END {
			if (call == 0) { print f ": no require_non_root call (with no argument)"; exit 1 }
			if (build != 0 && build < call) { print f ": first build step at line " build " comes before require_non_root at line " call; exit 1 }
		}' "$1"
}

fixture() {
	local name="$1"; shift
	{ echo '#!/usr/bin/env bash'; echo '# cmake in a comment is not a build step'; printf '%s\n' "$@"; } > "$tmp/$name.sh"
	echo "$tmp/$name.sh"
}
for bad in \
	"$(fixture missing 'cmake -S . -B b')" \
	"$(fixture after 'cmake -S . -B b' 'require_non_root || exit 1')" \
	"$(fixture after-ctest 'ctest --test-dir b' 'require_non_root || exit 1')" \
	"$(fixture after-build-fn 'build_and_test' 'require_non_root || exit 1')" \
	"$(fixture with-uid 'require_non_root 501 || exit 1' 'cmake -S . -B b')" \
	"$(fixture commented '# require_non_root || exit 1' 'cmake -S . -B b')"; do
	if gate_check "$bad" > /dev/null; then
		echo "FAIL: the gate check passed a bad script: $(cat "$bad")"; exit 1
	fi
done
good="$(fixture good 'require_non_root || exit 1' 'for tool in cmake ctest; do :; done' 'cmake -S . -B b')"
gate_check "$good" > /dev/null || { echo "FAIL: the gate check rejected a good script: $(cat "$good")"; exit 1; }

# Static check on the real entry points.
for script in hack/ci.sh hack/linux-test.sh; do
	gate_check "$script" || { echo "FAIL: $script does not call require_non_root before its first build step"; exit 1; }
done

echo "    self-test: uid 0 fails naming the skipped permission-denied cases; uid 501 passes silently; hack/ci.sh and hack/linux-test.sh check before any build step"
