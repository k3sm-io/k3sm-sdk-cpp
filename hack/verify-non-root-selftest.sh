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

# gate_check <file>: 0 when the first non-comment line of <file> that names
# require_non_root is its call, before the first build step, as a line that
# is exactly `require_non_root || exit <1-255>` or a bare `require_non_root`
# under an earlier top-level `set -e` (a trailing comment is allowed), and
# that line runs unconditionally: at the top level (outside any function
# body, if/case/loop, brace group, subshell, substitution, quoted string or
# here-document), not continuing a list (`a &&`, `a |`, `a \`), and with no
# top-level statement before it starting with exit, return or exec (an exec
# with only redirections does not count). An alias, a function definition or
# an unset naming require_non_root above the call is that first mention, so
# it fails. A file the scanner cannot balance before the call fails, and so
# does a file with no build step. Otherwise prints the cause with its line
# number and returns 1. The scanner reads a bash subset, not the full
# grammar: what it cannot read, it rejects by name.
gate_check() {
	# shellcheck disable=SC2016 # awk program, not shell
	awk -v f="$1" -v steps="$build_steps" -v sq="'" '
		function push(t) { st[++sp] = t; stl[sp] = NR }
		function pop(want) {
			if (sp == 0 || st[sp] != want) {
				if (bad == "") bad = "line " NR ": unsupported or unbalanced construct: a closer for " kind(want) (sp ? " while " kind(st[sp]) " from line " stl[sp] " is open" : " with nothing open")
				return
			}
			sp--
		}
		function top() { return sp ? st[sp] : "" }
		function kind(t) {
			if (t == "if") return "an if block"
			if (t == "loop") return "a loop"
			if (t == "case") return "a case block"
			if (t == "{") return "a brace group or function body"
			if (t == "(") return "a subshell"
			if (t == "$(") return "a command substitution"
			if (t == "`") return "a backtick substitution"
			if (t == "${") return "a parameter expansion"
			if (t == sq || t == "$" sq) return "a single-quoted string"
			if (t == "\"") return "a double-quoted string"
			return t
		}
		function endword(   kw) {
			if (!wany) { atredir = 0; return }
			kw = wq ? "" : w
			if (inexec) {
				if (atredir && w ~ /^[0-9]+$/) { }
				else if (redirnext) { redirnext = 0 }
				else if (cmdpos == 0) { if (early == 0) { early = execline; earlyw = "exec" }; inexec = 0 }
			} else if (cmdpos) {
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
					if (sp == 0 && stmt && (kw == "exit" || kw == "return") && early == 0) { early = NR; earlyw = kw }
					if (sp == 0 && stmt && kw == "exec") { inexec = 1; execline = NR; redirnext = 0 }
					if (sp == 0 && kw == "set") inset = 1
					cmdpos = 0
				}
			} else if (fname) {
				fname = 0; cmdpos = 1
			} else if (redirnext) {
				redirnext = 0
			} else if (inset) {
				if (w == "--" || w == "-") inset = 0
				else if (w ~ /^-[A-Za-z]*e/) errexit = 1
				else if (w ~ /^[+][A-Za-z]*e/) errexit = 0
				else if (w == "-o" || w == "+o") setop = w
				else if (w == "errexit" && setop != "") errexit = (setop == "-o")
			} else if (top() == "case" && kw == "in") {
				cmdpos = 1
			}
			w = ""; wany = 0; wq = 0; atredir = 0
		}
		function sep(s) { endword(); cmdpos = 1; stmt = s; inset = 0; setop = ""; inexec = 0; redirnext = 0 }
		function redir() { atredir = 1; endword(); redirnext = 1 }
		# scan one line of shell, carrying the block and quote stack across lines.
		function scan(line,   i, n, c, d, t, delim) {
			n = length(line); cont = 0; opend = 0
			for (i = 1; i <= n; i++) {
				c = substr(line, i, 1); d = substr(line, i + 1, 1); t = top()
				if (t == sq) { if (c == sq) sp--; continue }
				if (t == "$" sq) { if (c == "\\") i++; else if (c == sq) sp--; continue }
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
					else if (c == "\"") push("\"")
					else if (c == sq && st[sp - 1] != "\"") push(sq)
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
				else if (c == "$" && d == sq) { push("$" sq); wany = 1; wq = 1; i++ }
				else if (c == "#" && !wany) { break }
				else if (c == " " || c == "\t") { endword() }
				else if (c == ";") { sep(1); if (d == ";" || d == "&") i++ }
				else if (c == "&" && d == "&") { sep(0); opend = 1; i++ }
				else if (c == "&" && d == ">") { redir(); i++ }
				else if (c == "&") { sep(1) }
				else if (c == "|" && d == "|") { sep(0); opend = 1; i++ }
				else if (c == "|") { sep(0); opend = 1; if (d == "&") i++ }
				else if ((c == "<" || c == ">") && d == "&") { redir(); i++ }
				else if (c == "<" && substr(line, i, 3) == "<<<") { redir(); i += 2 }
				else if (c == "<" && d == "<") {
					endword(); i += 2; tab = 0
					if (substr(line, i, 1) == "-") { tab = 1; i++ }
					while (substr(line, i, 1) ~ /[ \t]/) i++
					delim = ""
					while (i <= n && substr(line, i, 1) !~ /[ \t;&|<>()]/) { delim = delim substr(line, i, 1); i++ }
					i--
					gsub(/["\\]/, "", delim); gsub(sq, "", delim)
					if (delim == "") { if (bad == "") bad = "line " NR ": unsupported here-document delimiter" }
					else { hd[++nhd] = delim; hdtab[nhd] = tab; hdl[nhd] = NR }
				}
				else if (c == "<" || c == ">") { redir() }
				else if (c == "(" && d == ")") { endword(); cmdpos = 1; i++ }
				else if (c == "(") { endword(); push("("); cmdpos = 1; stmt = 1 }
				else if (c == ")") {
					endword(); t = top()
					if (t == "case") { cmdpos = 1; stmt = 1 }
					else if (t == "(") { sp--; cmdpos = 0 }
					else if (t == "$(") { sp--; cmdpos = 0; wany = 1; wq = 1 }
					else if (bad == "") bad = "line " NR ": unsupported or unbalanced construct: a ) with " (sp ? kind(t) " from line " stl[sp] " open" : "nothing open")
				}
				else { w = w c; wany = 1 }
			}
			t = top()
			if (t == sq || t == "$" sq || t == "\"" || t == "`" || t == "${") { w = w "\n"; return }
			if (cont) return
			if (opend) { endword(); cmdpos = 1; inset = 0; return }
			sep(1)
			if (nhd > hdone) inhd = 1
		}
		# why <line>: the reason the first line naming require_non_root is not an accepted call.
		function why(   s) {
			if (inhd) return "it is inside the here-document opened at line " hdl[hdone + 1]
			if (sp) return "it is inside " kind(st[sp]) " opened at line " stl[sp]
			if (cont) return "it continues the line before (a trailing backslash)"
			if (!stmt) return "it continues a && / || / | list from the line before"
			s = $0; sub(/^[[:space:]]+/, "", s)
			if (s ~ /^alias[[:space:]]/) return "it is an alias for require_non_root"
			if (s ~ /^unset[[:space:]]/) return "it unsets require_non_root"
			if (s ~ /^(function[[:space:]]+)?require_non_root[[:space:]]*[({]/ || s ~ /^function[[:space:]]+require_non_root/) return "it defines a function named require_non_root"
			if (s !~ /^require_non_root([[:space:];|&]|$)/) return "it names require_non_root other than as the first word of the line"
			if (!accepted) return "the call form is not accepted (want exactly require_non_root || exit <1-255>, or a bare call under set -e): " s
			if (early) return "the call is unreachable after the top-level " earlyw " at line " early
			return "a bare call with errexit off (no earlier top-level set -e, or a later set +e) ignores its failure"
		}
		BEGIN {
			sp = 0; cmdpos = 1; stmt = 1
			ns = split(steps, names, " ")
			re = ""
			for (k = 1; k <= ns; k++) { s = names[k]; gsub(/[+$.]/, "[&]", s); re = re (re == "" ? "" : "|") s }
			re = "(^|[^A-Za-z0-9_.+$-])(" re ")([^A-Za-z0-9_.+-]|$)"
			form = "^[[:space:]]*require_non_root([[:space:]]*[|][|][[:space:]]*exit[[:space:]]+([1-9]|[1-9][0-9]|1[0-9][0-9]|2[0-4][0-9]|25[0-5]))?[[:space:]]*([[:space:]]#.*)?$"
		}
		build == 0 && $0 !~ /^[[:space:]]*#/ && $0 ~ re { build = NR }
		call != 0 || bad != "" || reject != "" { next }
		$0 !~ /^[[:space:]]*#/ && $0 ~ /require_non_root/ {
			fresh = !inhd && sp == 0 && !cont && stmt && cmdpos
			accepted = $0 ~ form
			if (fresh && accepted && early == 0 && ($0 ~ /[|][|]/ || errexit)) { call = NR; next }
			reject = "line " NR ": " why()
			next
		}
		inhd {
			l = $0
			if (hdtab[hdone + 1]) sub(/^\t+/, "", l)
			if (l == hd[hdone + 1]) hdone++
			if (hdone == nhd) inhd = 0
			next
		}
		{ scan($0) }
		END {
			if (build == 0) { print f ": no build step in " NR " lines (none of: " steps ")"; exit 1 }
			if (call == 0 && bad != "") { print f ": cannot read the script before the call: " bad; exit 1 }
			if (call == 0 && reject != "") { print f ": " reject; exit 1 }
			if (call == 0) { print f ": no require_non_root call in " NR " lines"; exit 1 }
			if (build < call) { print f ": first build step at line " build " comes before require_non_root at line " call; exit 1 }
		}' "$1"
}

fixture() {
	local name="$1"; shift
	{ echo '#!/usr/bin/env bash'; echo '# cmake in a comment is not a build step'; printf '%s\n' "$@"; } > "$tmp/$name.sh"
	echo "$tmp/$name.sh"
}
# bad <name> <want: a substring of the rejection> <line>...: the gate check
# rejects the fixture with a message containing <want>. Line numbers count
# the fixture's two header lines.
bad() {
	local name="$1" want="$2" file out
	shift 2
	file="$(fixture "$name" "$@")"
	if out="$(gate_check "$file")"; then
		echo "FAIL: the gate check passed bad fixture $name: $(cat "$file")"; exit 1
	fi
	case "$out" in
	*"$want"*) ;;
	*) echo "FAIL: bad fixture $name: want a rejection containing '$want', got: $out"; exit 1 ;;
	esac
}
# good <name> <line>...: the gate check accepts the fixture.
good() {
	local name="$1" file out
	shift
	file="$(fixture "$name" "$@")"
	out="$(gate_check "$file")" || { echo "FAIL: the gate check rejected good fixture $name: $out"; exit 1; }
}
form='the call form is not accepted'
# shellcheck disable=SC1003,SC2016 # fixture lines are literal shell text
{
	bad missing 'no require_non_root call in 3 lines' 'cmake -S . -B b'
	bad after 'first build step at line 3 comes before require_non_root at line 4' 'cmake -S . -B b' 'require_non_root || exit 1'
	bad after-ctest 'first build step at line 3' 'ctest --test-dir b' 'require_non_root || exit 1'
	bad after-build-fn 'first build step at line 3' 'build_and_test' 'require_non_root || exit 1'
	bad after-extras 'first build step at line 3' 'extras_build b t' 'require_non_root || exit 1'
	bad after-compiler 'first build step at line 3' '"$cxx" -o p p.cpp' 'require_non_root || exit 1'
	bad no-build-step 'no build step in 4 lines' 'require_non_root || exit 1' 'echo done'
	bad commented 'no require_non_root call in 4 lines' '# require_non_root || exit 1' 'cmake -S . -B b'
	bad with-uid "line 3: $form" 'require_non_root 501 || exit 1' 'cmake -S . -B b'
	bad or-true "line 3: $form" 'require_non_root || true' 'cmake -S . -B b'
	bad or-colon "line 3: $form" 'require_non_root || :' 'cmake -S . -B b'
	bad or-exit-0 "line 3: $form" 'require_non_root || exit 0' 'cmake -S . -B b'
	bad or-exit-256 "line 3: $form" 'require_non_root || exit 256' 'cmake -S . -B b'
	bad then-exit-0 "line 3: $form" 'require_non_root; exit 0' 'cmake -S . -B b'
	bad trailing "line 3: $form" 'require_non_root || exit 1; true' 'cmake -S . -B b'
	bad bare-no-errexit 'line 3: a bare call with errexit off' 'require_non_root' 'cmake -S . -B b'
	bad bare-errexit-off 'line 5: a bare call with errexit off' 'set -e' 'set +e' 'require_non_root' 'cmake -S . -B b'
	bad bare-set-args 'line 4: a bare call with errexit off' 'set -- -e' 'require_non_root' 'cmake -S . -B b'
	bad alias 'line 3: it is an alias' 'alias require_non_root=true' 'require_non_root || exit 1' 'cmake -S . -B b'
	bad redefine 'line 3: it defines a function named require_non_root' 'require_non_root() { :; }' 'require_non_root || exit 1' 'cmake -S . -B b'
	bad redefine-kw 'line 3: it defines a function named require_non_root' 'function require_non_root {' ':' '}' 'require_non_root || exit 1' 'cmake -S . -B b'
	bad unset 'line 3: it unsets require_non_root' 'unset -f require_non_root' 'require_non_root || exit 1' 'cmake -S . -B b'
	bad nested 'line 3: it names require_non_root other than as the first word' 'false && require_non_root || exit 1' 'cmake -S . -B b'
	bad in-function 'line 4: it is inside a brace group or function body opened at line 3' 'check() {' 'require_non_root || exit 1' '}' 'cmake -S . -B b'
	bad in-function-kw 'line 4: it is inside a brace group or function body opened at line 3' 'function check {' 'require_non_root || exit 1' '}' 'cmake -S . -B b'
	bad in-if-false 'line 4: it is inside an if block opened at line 3' 'if false; then' 'require_non_root || exit 1' 'fi' 'cmake -S . -B b'
	bad in-loop 'line 4: it is inside a loop opened at line 3' 'while false; do' 'require_non_root || exit 1' 'done' 'cmake -S . -B b'
	bad in-case 'line 5: it is inside a case block opened at line 3' 'case x in' 'y)' 'require_non_root || exit 1' ';;' 'esac' 'cmake -S . -B b'
	bad in-group 'line 4: it is inside a brace group or function body opened at line 3' 'false && {' 'require_non_root || exit 1' '}' 'cmake -S . -B b'
	bad in-subshell 'line 4: it is inside a subshell opened at line 3' '(' 'require_non_root || exit 1' ')' 'cmake -S . -B b'
	bad in-heredoc 'line 4: it is inside the here-document opened at line 3' 'cat <<EOF' 'require_non_root || exit 1' 'EOF' 'cmake -S . -B b'
	bad in-string 'line 4: it is inside a double-quoted string opened at line 3' 'x="' 'require_non_root || exit 1' '"' 'cmake -S . -B b'
	bad after-and 'line 4: it continues a && / || / | list' 'false &&' 'require_non_root || exit 1' 'cmake -S . -B b'
	bad after-backslash 'line 4: it continues the line before' 'true \' 'require_non_root || exit 1' 'cmake -S . -B b'
	bad after-exit 'line 4: the call is unreachable after the top-level exit at line 3' 'exit 0' 'require_non_root || exit 1' 'cmake -S . -B b'
	bad after-exec 'line 4: the call is unreachable after the top-level exec at line 3' 'exec >log bash' 'require_non_root || exit 1' 'cmake -S . -B b'
	bad unbalanced 'line 3: unsupported or unbalanced construct' 'fi' 'require_non_root || exit 1' 'cmake -S . -B b'
	good good 'require_non_root || exit 1' 'for tool in cmake ctest; do :; done' 'cmake -S . -B b'
	good good-bare 'set -euo pipefail' 'require_non_root' 'cmake -S . -B b'
	good good-comment 'require_non_root || exit 255 # refuse root' 'cmake -S . -B b'
	good good-exec-redirect 'exec >log 2>&1' 'exec 3>&1' 'exec 4<in 5>>out' 'require_non_root || exit 1' 'cmake -S . -B b'
	good good-quoting 'x=$'"'"'a\'"'"'b'"'" 'y=${x:-"}"}' 'z=$(case a in a) echo 1;; esac)' 'v=$(' 'if true; then echo 1' 'fi' ')' 'require_non_root || exit 1' 'cmake -S . -B b'
	good good-after-blocks 'if [ "$(uname -s)" != Linux ]; then' '	exit 2' 'fi' 'f() { cd "$(dirname "$0")/.." || exit 1; }' 'cat <<EOF' 'fi' 'EOF' 'require_non_root || exit 1' 'cmake -S . -B b'
}

# Static check on the real entry points.
for script in hack/ci.sh hack/linux-test.sh; do
	gate_check "$script" || { echo "FAIL: $script does not call require_non_root before its first build step"; exit 1; }
done

echo "    self-test: uid 0 fails naming the skipped permission-denied cases; uid 501 passes silently; hack/ci.sh and hack/linux-test.sh check before any build step"
