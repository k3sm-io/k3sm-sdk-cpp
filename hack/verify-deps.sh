#!/usr/bin/env bash
# Copyright The k3sm Authors.
# SPDX-License-Identifier: Apache-2.0
#
# verify-deps.sh [--self-test] [root] -- the dependency supply-chain check.
#
#   1. every FetchContent_Declare( and ExternalProject_Add( in a CMake file
#      (CMakeLists.txt, *.cmake, *.cmake.in) names a dependency listed in
#      hack/deps.allow (format: <name> <git url> <40-hex commit sha> <reason>)
#   2. an allowed dependency is fetched by GIT_REPOSITORY equal to the listed
#      url and GIT_TAG equal to the listed sha, never a branch, tag or URL
#      archive
#   3. no CPM and no git submodules (.gitmodules); a vendored copy of another
#      project's tree is not detected here and is caught in review
#
# With no dependencies the allow file holds only comments and every
# FetchContent_Declare or ExternalProject_Add fails. Comments are stripped
# before parsing.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"

# calls <file>: print "<command> <args on one line>" for each dependency call.
calls() {
	sed 's/#.*$//' "$1" | tr '\n' ' ' | awk '
	{
		t = $0
		while (match(t, /(FetchContent_Declare|ExternalProject_Add|CPMAddPackage|CPMFindPackage)[ \t]*\(/)) {
			cmd = substr(t, RSTART, RLENGTH); sub(/[ \t]*\($/, "", cmd)
			rest = substr(t, RSTART + RLENGTH)
			end = index(rest, ")")
			if (end == 0) { print cmd " " rest; break }
			print cmd " " substr(rest, 1, end - 1)
			t = substr(rest, end + 1)
		}
	}'
}

# check <root> <allow file>: exit 1 on any violation, printing each.
check() {
	local root="$1" allow="$2" bad=0 f line cmd name url tag want_url want_sha
	if [ -e "$root/.gitmodules" ]; then echo "    git submodules are not allowed: .gitmodules"; bad=1; fi
	while IFS= read -r -d '' f; do
		while IFS= read -r line; do
			[ -n "$line" ] || continue
			# shellcheck disable=SC2086  # the call text is split into words on purpose
			set -- $line
			cmd="$1"; name="${2:-}"
			case "$cmd" in CPM*) echo "    ${f#"$root"/}: $cmd is not allowed"; bad=1; continue ;; esac
			url="$(printf '%s\n' "$line" | awk '{for (i = 1; i < NF; i++) if ($i == "GIT_REPOSITORY") print $(i + 1)}')"
			tag="$(printf '%s\n' "$line" | awk '{for (i = 1; i < NF; i++) if ($i == "GIT_TAG") print $(i + 1)}')"
			read -r want_url want_sha < <(awk -v n="$name" '$1 == n {print $2, $3; exit}' <(grep -v '^[[:space:]]*#' "$allow") || true) || true
			if [ -z "${want_sha:-}" ]; then
				echo "    ${f#"$root"/}: $cmd($name) is not listed in hack/deps.allow"; bad=1; continue
			fi
			if ! printf '%s' "$want_sha" | grep -Eqx '[0-9a-f]{40}'; then
				echo "    hack/deps.allow: $name is not pinned to a 40-hex commit sha"; bad=1; continue
			fi
			[ "$url" = "$want_url" ] || { echo "    ${f#"$root"/}: $name GIT_REPOSITORY '$url' is not the allowed '$want_url'"; bad=1; }
			[ "$tag" = "$want_sha" ] || { echo "    ${f#"$root"/}: $name GIT_TAG '$tag' is not the allowed sha"; bad=1; }
			want_url=""; want_sha=""
		done < <(calls "$f")
	done < <(find "$root" \( -name .git -o -name build -o -name out -o -name testdata \) -prune -o \
		-type f \( -name CMakeLists.txt -o -name '*.cmake' -o -name '*.cmake.in' \) -print0)
	return "$bad"
}

if [ "${1:-}" = --self-test ]; then
	tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
	sha=0123456789abcdef0123456789abcdef01234567
	mk() { rm -rf "$tmp/r"; mkdir -p "$tmp/r"; printf '%s\n' "$1" > "$tmp/r/CMakeLists.txt"; printf '%s\n' "$2" > "$tmp/allow"; }
	expect() {
		local want="$1" what="$2" got=0
		check "$tmp/r" "$tmp/allow" > "$tmp/out" || got=1
		[ "$got" = "$want" ] || { echo "FAIL: self-test: $what (exit $got, want $want)"; cat "$tmp/out"; exit 1; }
	}
	mk 'project(x) # FetchContent_Declare(commented GIT_TAG main)' '# empty'
	expect 0 "no dependencies passes"
	mk 'FetchContent_Declare(fmt GIT_REPOSITORY https://example.com/fmt.git GIT_TAG main)' '# empty'
	expect 1 "an unlisted FetchContent_Declare fails"
	mk "FetchContent_Declare(fmt
  GIT_REPOSITORY https://example.com/fmt.git
  GIT_TAG $sha)" "fmt https://example.com/fmt.git $sha reviewed"
	expect 0 "a listed dependency at its sha passes"
	mk 'FetchContent_Declare(fmt GIT_REPOSITORY https://example.com/fmt.git GIT_TAG v1.0)' "fmt https://example.com/fmt.git $sha reviewed"
	expect 1 "a listed dependency at a tag fails"
	mk "FetchContent_Declare(fmt GIT_REPOSITORY https://example.com/other.git GIT_TAG $sha)" "fmt https://example.com/fmt.git $sha reviewed"
	expect 1 "a listed dependency from another url fails"
	mk 'ExternalProject_Add(zlib URL https://example.com/z.tgz)' '# empty'
	expect 1 "an ExternalProject_Add fails"
	mk 'CPMAddPackage("gh:a/b@1.0")' '# empty'
	expect 1 "CPM fails"
	mk 'project(x)' '# empty'; : > "$tmp/r/.gitmodules"
	expect 1 "a .gitmodules fails"
	echo "    self-test: eight cases"
	exit 0
fi

root="${1:-$here/..}"
if ! check "$root" "$here/deps.allow"; then
	echo "FAIL: dependencies must be listed in hack/deps.allow and pinned to a commit sha"; exit 1
fi
echo "    ok (no unlisted dependencies)"
