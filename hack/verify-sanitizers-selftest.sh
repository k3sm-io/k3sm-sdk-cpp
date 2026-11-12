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
# that hack/ci.sh and hack/linux-test.sh each call require_sanitizers. The
# setter scan is itself tested on fixture trees: every way of assigning the
# waiver must be found, and a comment-only line, a file that is not a script
# and the helper's own echo lines must not be.
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

# waiver_setters <root>: print <path>:<line>: <text> for every line of a script
# under <root>/hack that assigns K3SM_SDK_ALLOW_NO_SANITIZERS. A script is a
# *.sh file or any file starting with #!. The name followed by = counts
# anywhere on a line, so a prefix assignment after env, another assignment,
# then, do, a bracket or a #, and an export, declare, local or readonly
# operand are all found. Left out: comment-only lines, this file, and in
# hack/lib/sanitizers.sh a line that is one echo of a plain double-quoted
# string (the two lines that print the waiver's name).
waiver_setters() {
	local root="$1" f rel helper
	while IFS= read -r -d '' f; do
		rel="${f#"$root"/}"
		[ "$rel" != hack/verify-sanitizers-selftest.sh ] || continue
		case "$f" in
		*.sh) ;;
		*) [ "$(head -c 2 "$f")" = '#!' ] || continue ;;
		esac
		helper=0
		[ "$rel" != hack/lib/sanitizers.sh ] || helper=1
		# shellcheck disable=SC2016 # awk program, not shell
		awk -v rel="$rel" -v helper="$helper" '
			/^[[:space:]]*#/ { next }
			!/(^|[^A-Za-z0-9_])K3SM_SDK_ALLOW_NO_SANITIZERS=/ { next }
			helper == 1 && /^[[:space:]]*echo "[^"`$;&|]*"[[:space:]]*$/ { next }
			{ print rel ":" NR ": " $0 }' "$f"
	done < <(find "$root/hack" -type f -print0 | sort -z)
}

# Each fixture tree holds one script under hack/; form <name> <file> <line>...
# writes it, with the lines given after a shebang and a header comment.
form() {
	local d="$tmp/forms/$1/hack"
	mkdir -p "$(dirname "$d/$2")"
	{ echo '#!/usr/bin/env bash'; echo '# fixture'; printf '%s\n' "${@:3}"; } > "$d/$2"
	echo "$tmp/forms/$1"
}

# Forms that set the waiver: each must be found.
# shellcheck disable=SC2016 # the fixture line is written as is
bracket='[ -z "$(K3SM_SDK_ALLOW_NO_SANITIZERS=1 hack/ci.sh)" ]'
found=(
	"$(form env x.sh 'env K3SM_SDK_ALLOW_NO_SANITIZERS=1 hack/ci.sh')"
	"$(form second-prefix x.sh 'CC=clang K3SM_SDK_ALLOW_NO_SANITIZERS=1 hack/ci.sh')"
	"$(form second-export x.sh 'export CC=clang K3SM_SDK_ALLOW_NO_SANITIZERS=1')"
	"$(form after-then x.sh 'if true; then K3SM_SDK_ALLOW_NO_SANITIZERS=1 hack/ci.sh; fi')"
	"$(form after-do x.sh 'for i in 1; do K3SM_SDK_ALLOW_NO_SANITIZERS=1 hack/ci.sh; done')"
	"$(form paren x.sh '( K3SM_SDK_ALLOW_NO_SANITIZERS=1 hack/ci.sh )')"
	"$(form brace x.sh '{ K3SM_SDK_ALLOW_NO_SANITIZERS=1 hack/ci.sh; }')"
	"$(form bracket x.sh "$bracket")"
	"$(form declare x.sh 'declare -x K3SM_SDK_ALLOW_NO_SANITIZERS=1')"
	"$(form readonly x.sh 'readonly K3SM_SDK_ALLOW_NO_SANITIZERS=1')"
	"$(form hash-before x.sh 'echo "#"; K3SM_SDK_ALLOW_NO_SANITIZERS=1 hack/ci.sh')"
	"$(form no-suffix run-gate 'K3SM_SDK_ALLOW_NO_SANITIZERS=1 hack/ci.sh')"
	"$(form helper-chained lib/sanitizers.sh 'echo "x"; K3SM_SDK_ALLOW_NO_SANITIZERS=1 hack/ci.sh')"
)
for root in "${found[@]}"; do
	[ -n "$(waiver_setters "$root")" ] \
		|| { echo "FAIL: the setter scan missed the ${root##*/} form: $(grep -rhv '^#' "$root/hack")"; exit 1; }
done

# Lines that do not set it: none may be found.
quiet="$(form quiet ci.sh \
	'# K3SM_SDK_ALLOW_NO_SANITIZERS=1 waives the check' \
	'	# K3SM_SDK_ALLOW_NO_SANITIZERS=1, indented' \
	'FOO_K3SM_SDK_ALLOW_NO_SANITIZERS=1 hack/ci.sh')"
form quiet verify-sanitizers-selftest.sh 'K3SM_SDK_ALLOW_NO_SANITIZERS=1 hack/ci.sh' > /dev/null
mkdir -p "$quiet/hack/lib"; cp hack/lib/sanitizers.sh "$quiet/hack/lib/sanitizers.sh"
printf '%s\n' 'K3SM_SDK_ALLOW_NO_SANITIZERS=1 hack/ci.sh' > "$quiet/hack/notes.txt"
out="$(waiver_setters "$quiet")"
[ -z "$out" ] || { echo "FAIL: the setter scan flagged a line that does not set the waiver: $out"; exit 1; }

# Static checks: no script sets the waiver itself, and both entry points that
# build the tests call require_sanitizers.
setters="$(waiver_setters .)"
[ -z "$setters" ] || { echo "FAIL: a script under hack/ sets K3SM_SDK_ALLOW_NO_SANITIZERS; only the caller may: $setters"; exit 1; }
for script in hack/ci.sh hack/linux-test.sh; do
	grep -qE '^[^#]*require_sanitizers ' "$script" \
		|| { echo "FAIL: $script does not call require_sanitizers"; exit 1; }
done

echo "    self-test: linked passes silently; a failed or absent probe fails unless K3SM_SDK_ALLOW_NO_SANITIZERS is exactly 1, which prints WAIVED"
