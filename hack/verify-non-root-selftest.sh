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
# require_non_root, with no argument and with its failure ending the script,
# before their first build step (the first line that names a driver in
# build_steps below); a script with no build step at all fails. The static
# check is itself tested on fixture scripts: a script without the call, with
# the call after a build step, passing a uid, ignoring the failure, or with the
# call where it may never run must each be found.
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

# build_steps: every build driver hack/ci.sh and hack/linux-test.sh run: the
# CMake tools, the functions that wrap them, and the compiler, which the
# scripts run by name or through $c, $cxx and $cxx_path. A line naming one,
# outside a comment line, is a build step, wherever it is (a function body
# included); naming one too many only moves the first build step earlier.
# shellcheck disable=SC2016 # literal variable names, matched as text
build_steps='cmake ctest build_and_test extras_build fuzz_cxx c++ clang++ g++ $c $cxx $cxx_path'

# gate_check <file>: 0 when <file> calls require_non_root before its first
# build step, as a line that is exactly `require_non_root` (under an earlier
# top-level `set -e`) or `require_non_root || exit <nonzero>`, and that line
# runs unconditionally: at the top level (outside any function body,
# if/case/loop, brace group, subshell, quoted string or here-document), not
# continuing a list (`a &&`, `a |`, `a \`), and with no top-level statement
# starting with exit, return or exec before it. A file the scanner cannot
# balance before the call fails, and so does a file with no build step.
# Otherwise prints why and returns 1. The scanner is a bash subset, not a
# parser: it errs toward rejecting.
gate_check() {
	# shellcheck disable=SC2016 # awk program, not shell
	awk -v f="$1" -v steps="$build_steps" -v sq="'" '
		function push(t) { st[++sp] = t }
		function pop(want) {
			if (sp == 0 || st[sp] != want) {
				if (bad == "") bad = "unbalanced " want " at line " NR
				return
			}
			sp--
		}
		function top() { return sp ? st[sp] : "" }
		function endword(   kw) {
			if (!wany) return
			kw = wq ? "" : w
			if (cmdpos) {
				if (kw == "if") { push("if") }
				else if (kw == "while" || kw == "until") { push("loop") }
				else if (kw == "for" || kw == "select") { push("loop"); cmdpos = 0 }
				else if (kw == "case") { push("case"); cmdpos = 0 }
				else if (kw == "{") { push("{") }
				else if (kw == "}") { pop("{"); cmdpos = 0 }
				else if (kw == "fi") { pop("if"); cmdpos = 0 }
				else if (kw == "done") { pop("loop"); cmdpos = 0 }
				else if (kw == "esac") { pop("case"); cmdpos = 0 }
				else if (kw == "then" || kw == "do" || kw == "else" || kw == "elif" || kw == "!" || kw == "time") { }
				else if (kw == "function") { fname = 1; cmdpos = 0 }
				else if (w ~ /^[A-Za-z_][A-Za-z0-9_]*(\[.*\])?[+]?=/) { }
				else {
					if (sp == 0 && stmt && (kw == "exit" || kw == "return" || kw == "exec") && early == 0) early = NR
					if (sp == 0 && kw == "set") inset = 1
					cmdpos = 0
				}
			} else if (fname) {
				fname = 0; cmdpos = 1
			} else if (inset) {
				if (w ~ /^-[A-Za-z]*e/) errexit = 1
				else if (w ~ /^[+][A-Za-z]*e/) errexit = 0
				else if (w == "-o" || w == "+o") setop = w
				else if (w == "errexit" && setop != "") errexit = (setop == "-o")
			} else if (top() == "case" && kw == "in") {
				cmdpos = 1
			}
			w = ""; wany = 0; wq = 0
		}
		function sep(s) { endword(); cmdpos = 1; stmt = s; inset = 0; setop = "" }
		# scan one line of shell, carrying the block and quote stack across lines.
		function scan(line,   i, n, c, d, t, delim) {
			n = length(line); cont = 0; opend = 0
			for (i = 1; i <= n; i++) {
				c = substr(line, i, 1); d = substr(line, i + 1, 1); t = top()
				if (t == sq) { if (c == sq) sp--; continue }
				if (t == "\"") {
					if (c == "\\") i++
					else if (c == "\"") sp--
					else if (c == "$" && d == "(") { push("$("); w = ""; wany = 0; wq = 0; cmdpos = 1; i++ }
					else if (c == "$" && d == "{") { push("${"); i++ }
					else if (c == "`") push("`")
					continue
				}
				if (t == "`") { if (c == "\\") i++; else if (c == "`") sp--; continue }
				if (t == "${") {
					if (c == "\\") i++
					else if (c == "}") sp--
					else if (c == "$" && d == "(") { push("$("); cmdpos = 1; i++ }
					else if (c == "$" && d == "{") { push("${"); i++ }
					continue
				}
				opend = 0
				if (c == "\\") { if (i == n) { cont = 1; break } ; w = w substr(line, i, 2); wany = 1; wq = 1; i++ }
				else if (c == sq) { push(sq); wany = 1; wq = 1 }
				else if (c == "\"") { push("\""); wany = 1; wq = 1 }
				else if (c == "`") { push("`"); wany = 1; wq = 1 }
				else if (c == "$" && d == "(") { push("$("); w = ""; wany = 0; wq = 0; cmdpos = 1; i++ }
				else if (c == "$" && d == "{") { push("${"); wany = 1; wq = 1; i++ }
				else if (c == "$" && d == sq) { push(sq); wany = 1; wq = 1; i++ }
				else if (c == "#" && !wany) { break }
				else if (c == " " || c == "\t") { endword() }
				else if (c == ";") { sep(1); if (d == ";" || d == "&") i++ }
				else if (c == "&" && d == "&") { sep(0); opend = 1; i++ }
				else if (c == "&" && d == ">") { endword(); i++ }
				else if (c == "&") { sep(1) }
				else if (c == "|" && d == "|") { sep(0); opend = 1; i++ }
				else if (c == "|") { sep(0); opend = 1; if (d == "&") i++ }
				else if ((c == "<" || c == ">") && d == "&") { endword(); i++ }
				else if (c == "<" && substr(line, i, 3) == "<<<") { endword(); i += 2 }
				else if (c == "<" && d == "<") {
					endword(); i += 2; tab = 0
					if (substr(line, i, 1) == "-") { tab = 1; i++ }
					while (substr(line, i, 1) ~ /[ \t]/) i++
					delim = ""
					while (i <= n && substr(line, i, 1) !~ /[ \t;&|<>()]/) { delim = delim substr(line, i, 1); i++ }
					i--
					gsub(/["\\]/, "", delim); gsub(sq, "", delim)
					if (delim == "") { if (bad == "") bad = "unreadable here-document at line " NR }
					else { hd[++nhd] = delim; hdtab[nhd] = tab }
				}
				else if (c == "(" && d == ")") { endword(); cmdpos = 1; i++ }
				else if (c == "(") { endword(); push("("); cmdpos = 1; stmt = 1 }
				else if (c == ")") {
					endword()
					if (t == "case") { cmdpos = 1; stmt = 1 }
					else if (t == "(") { sp--; cmdpos = 0 }
					else if (t == "$(") { sp--; cmdpos = 0; wany = 1; wq = 1 }
					else if (bad == "") bad = "unbalanced ) at line " NR
				}
				else { w = w c; wany = 1 }
			}
			t = top()
			if (t == sq || t == "\"" || t == "`" || t == "${") { w = w "\n"; return }
			if (cont) return
			if (opend) { endword(); cmdpos = 1; inset = 0; return }
			sep(1)
			if (nhd > hdone) inhd = 1
		}
		BEGIN {
			sp = 0; cmdpos = 1; stmt = 1
			ns = split(steps, names, " ")
			re = ""
			for (k = 1; k <= ns; k++) { s = names[k]; gsub(/[+$.]/, "[&]", s); re = re (re == "" ? "" : "|") s }
			re = "(^|[^A-Za-z0-9_.+$-])(" re ")([^A-Za-z0-9_.+-]|$)"
		}
		build == 0 && $0 !~ /^[[:space:]]*#/ && $0 ~ re { build = NR }
		call != 0 || bad != "" { next }
		inhd {
			l = $0
			if (hdtab[hdone + 1]) sub(/^\t+/, "", l)
			if (l == hd[hdone + 1]) hdone++
			if (hdone == nhd) inhd = 0
			next
		}
		{
			fresh = sp == 0 && !cont && stmt && cmdpos
			if (fresh && $0 ~ /^[[:space:]]*require_non_root([[:space:]]*[|][|][[:space:]]*exit[[:space:]]+[1-9][0-9]*)?[[:space:]]*$/) {
				if (early != 0) { why = why "\n  line " NR ": the call is unreachable after the exit at line " early }
				else if ($0 !~ /[|][|]/ && !errexit) { why = why "\n  line " NR ": a bare call under no top-level set -e ignores its failure" }
				else { call = NR; next }
			}
			scan($0)
		}
		END {
			if (build == 0) { print f ": no build step (none of: " steps ")"; exit 1 }
			if (call == 0 && bad != "") { print f ": cannot read the script before the call: " bad; exit 1 }
			if (call == 0) { print f ": no top-level require_non_root || exit <nonzero> (or bare require_non_root under set -e) that always runs" why; exit 1 }
			if (build < call) { print f ": first build step at line " build " comes before require_non_root at line " call; exit 1 }
		}' "$1"
}

fixture() {
	local name="$1"; shift
	{ echo '#!/usr/bin/env bash'; echo '# cmake in a comment is not a build step'; printf '%s\n' "$@"; } > "$tmp/$name.sh"
	echo "$tmp/$name.sh"
}
# shellcheck disable=SC1003,SC2016 # fixture lines are literal shell text
for bad in \
	"$(fixture missing 'cmake -S . -B b')" \
	"$(fixture after 'cmake -S . -B b' 'require_non_root || exit 1')" \
	"$(fixture after-ctest 'ctest --test-dir b' 'require_non_root || exit 1')" \
	"$(fixture after-build-fn 'build_and_test' 'require_non_root || exit 1')" \
	"$(fixture with-uid 'require_non_root 501 || exit 1' 'cmake -S . -B b')" \
	"$(fixture commented '# require_non_root || exit 1' 'cmake -S . -B b')" \
	"$(fixture or-true 'require_non_root || true' 'cmake -S . -B b')" \
	"$(fixture or-colon 'require_non_root || :' 'cmake -S . -B b')" \
	"$(fixture or-exit-0 'require_non_root || exit 0' 'cmake -S . -B b')" \
	"$(fixture then-exit-0 'require_non_root; exit 0' 'cmake -S . -B b')" \
	"$(fixture trailing 'require_non_root || exit 1; true' 'cmake -S . -B b')" \
	"$(fixture bare-no-errexit 'require_non_root' 'cmake -S . -B b')" \
	"$(fixture bare-errexit-off 'set -e' 'set +e' 'require_non_root' 'cmake -S . -B b')" \
	"$(fixture in-function 'check() {' 'require_non_root || exit 1' '}' 'cmake -S . -B b')" \
	"$(fixture in-function-kw 'function check {' 'require_non_root || exit 1' '}' 'cmake -S . -B b')" \
	"$(fixture in-if-false 'if false; then' 'require_non_root || exit 1' 'fi' 'cmake -S . -B b')" \
	"$(fixture in-loop 'while false; do' 'require_non_root || exit 1' 'done' 'cmake -S . -B b')" \
	"$(fixture in-case 'case x in' 'y)' 'require_non_root || exit 1' ';;' 'esac' 'cmake -S . -B b')" \
	"$(fixture in-group 'false && {' 'require_non_root || exit 1' '}' 'cmake -S . -B b')" \
	"$(fixture in-subshell '(' 'require_non_root || exit 1' ')' 'cmake -S . -B b')" \
	"$(fixture after-and 'false &&' 'require_non_root || exit 1' 'cmake -S . -B b')" \
	"$(fixture after-backslash 'true \' 'require_non_root || exit 1' 'cmake -S . -B b')" \
	"$(fixture in-heredoc 'cat <<EOF' 'require_non_root || exit 1' 'EOF' 'cmake -S . -B b')" \
	"$(fixture in-string 'x="' 'require_non_root || exit 1' '"' 'cmake -S . -B b')" \
	"$(fixture after-exit 'exit 0' 'require_non_root || exit 1' 'cmake -S . -B b')" \
	"$(fixture unbalanced 'fi' 'require_non_root || exit 1' 'cmake -S . -B b')" \
	"$(fixture after-extras 'extras_build b t' 'require_non_root || exit 1' 'cmake -S . -B b')" \
	"$(fixture after-compiler '"$cxx" -o p p.cpp' 'require_non_root || exit 1' 'cmake -S . -B b')" \
	"$(fixture no-build-step 'require_non_root || exit 1' 'echo done')"; do
	if gate_check "$bad" > /dev/null; then
		echo "FAIL: the gate check passed a bad script: $(cat "$bad")"; exit 1
	fi
done
# shellcheck disable=SC2016 # fixture lines are literal shell text
for good in \
	"$(fixture good 'require_non_root || exit 1' 'for tool in cmake ctest; do :; done' 'cmake -S . -B b')" \
	"$(fixture good-bare 'set -euo pipefail' 'require_non_root' 'cmake -S . -B b')" \
	"$(fixture good-after-blocks 'if [ "$(uname -s)" != Linux ]; then' '	exit 2' 'fi' 'f() { cd "$(dirname "$0")/.." || exit 1; }' 'cat <<EOF' 'fi' 'EOF' 'require_non_root || exit 1' 'cmake -S . -B b')"; do
	gate_check "$good" > /dev/null || { echo "FAIL: the gate check rejected a good script: $(cat "$good")"; gate_check "$good"; exit 1; }
done

# Static check on the real entry points.
for script in hack/ci.sh hack/linux-test.sh; do
	gate_check "$script" || { echo "FAIL: $script does not call require_non_root before its first build step"; exit 1; }
done

echo "    self-test: uid 0 fails naming the skipped permission-denied cases; uid 501 passes silently; hack/ci.sh and hack/linux-test.sh check before any build step"
