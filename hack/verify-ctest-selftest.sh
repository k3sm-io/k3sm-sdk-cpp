#!/usr/bin/env bash
# Copyright The k3sm Authors.
# SPDX-License-Identifier: Apache-2.0
#
# verify-ctest-selftest.sh -- check that every ctest call in hack/ci.sh and
# hack/linux-test.sh passes --no-tests=error. Without it ctest prints "No
# tests were found!!!" and exits 0 when a label filter matches nothing, so a
# renamed or dropped test label would leave a stage that prints ok having run
# nothing.
#
# A ctest call is a line, after joining backslash-continued lines, that is
# not a comment and not an echo or printf, where the word ctest stands at a
# command position (line start, after ; & | ! ( or after a VAR=value prefix)
# and is followed by an option. The scan is tested on fixture lines first: a
# call without the flag must be found, a call with it (on the same or a
# continued line) must pass, and the lines that only mention ctest must not
# count as calls.
set -euo pipefail
cd "$(dirname "$0")/.."

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

# ctest_calls <file>: print <file>:<line>: <text> for every ctest call, one
# per logical line, with a leading OK or MISSING for whether it carries
# --no-tests=error.
ctest_calls() {
	# shellcheck disable=SC2016 # awk program, not shell
	awk -v f="$1" '
		{
			if (buf == "") start = NR
			line = $0
			if (sub(/\\$/, "", line)) { buf = buf line " "; next }
			buf = buf line
			text = buf; buf = ""
			if (text ~ /^[[:space:]]*#/) next
			if (text ~ /^[[:space:]]*(echo|printf)[[:space:]]/) next
			if (text !~ /(^|[;&|!(][[:space:]]*|[[:space:]][A-Za-z_][A-Za-z0-9_]*=("[^"]*"|[^[:space:]]*)[[:space:]]+|^[[:space:]]*)ctest[[:space:]]+-/) next
			state = (text ~ /(^|[[:space:]])--no-tests=error([[:space:]]|$)/) ? "OK" : "MISSING"
			print state " " f ":" start ": " text
		}' "$1"
}

# expect_scan <name> <want: OK|MISSING|none> <line>...
expect_scan() {
	local name="$1" want="$2" out
	shift 2
	printf '%s\n' "$@" > "$tmp/$name.sh"
	out="$(ctest_calls "$tmp/$name.sh")"
	case "$want" in
	none) [ -z "$out" ] || { echo "FAIL: the scan counted a non-call ($name) as a ctest call: $out"; exit 1; } ;;
	*)
		[ "$(printf '%s\n' "$out" | grep -c .)" = 1 ] \
			|| { echo "FAIL: the scan did not find exactly one call in $name: $out"; exit 1; }
		case "$out" in
		"$want "*) ;;
		*) echo "FAIL: the scan read $name as '${out%% *}', want $want: $out"; exit 1 ;;
		esac ;;
	esac
}

# Calls without the flag: each must be found as MISSING.
# shellcheck disable=SC2016,SC1003 # fixture lines are written as is; a trailing \ is a continuation
{
	expect_scan bare MISSING 'ctest --test-dir "$b" --output-on-failure "$@"'
	expect_scan indented MISSING '		ctest --test-dir "$b" -L reader'
	expect_scan prefixed MISSING 'if ! TSAN_OPTIONS="halt_on_error=1 x" ctest --test-dir "$b" -V -L stress \' '	> "$tmp/stress.log" 2>&1; then'
	expect_scan chained MISSING 'cd x && ctest -L notifier'
	expect_scan other-flag MISSING 'ctest --test-dir b --no-tests=ignore'
	expect_scan continued MISSING 'ctest --test-dir "$b" \' '	--output-on-failure'
}

# Calls with the flag: each must be found as OK.
# shellcheck disable=SC2016,SC1003 # fixture lines are written as is; a trailing \ is a continuation
{
	expect_scan with-flag OK 'ctest --test-dir "$b" --output-on-failure --no-tests=error "$@"'
	expect_scan prefixed-flag OK 'if ! TSAN_OPTIONS="a=1" ctest --test-dir "$b" --no-tests=error -L stress \' '	> "$tmp/stress.log" 2>&1; then'
	expect_scan continued-flag OK 'ctest --test-dir "$b" \' '	--no-tests=error -L reader'
}

# Lines that only mention ctest: none may count as a call.
expect_scan comment none '# ctest --test-dir build'
expect_scan indented-comment none '	# 1. cmake configure, build and ctest -L reader'
expect_scan echo none '	echo "==> reader tests (ctest -L reader)"'
expect_scan tool-list none 'for tool in cmake ctest git; do'
expect_scan word-suffix none 'myctest -L reader'

# The real scripts: each has at least one call, and every call has the flag.
for script in hack/ci.sh hack/linux-test.sh; do
	calls="$(ctest_calls "$script")"
	[ -n "$calls" ] || { echo "FAIL: found no ctest call in $script; the scan or the script changed"; exit 1; }
	missing="$(printf '%s\n' "$calls" | grep '^MISSING ' || true)"
	[ -z "$missing" ] || { echo "FAIL: a ctest call runs without --no-tests=error, so a label that matches nothing passes:"; printf '%s\n' "$missing" | sed 's/^MISSING /    /'; exit 1; }
done

echo "    self-test: a ctest call without --no-tests=error is found, one with it passes; every call in hack/ci.sh and hack/linux-test.sh has it"
