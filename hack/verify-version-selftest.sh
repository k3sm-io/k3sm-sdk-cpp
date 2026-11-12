#!/usr/bin/env bash
# Copyright The k3sm Authors.
# SPDX-License-Identifier: Apache-2.0
#
# verify-version-selftest.sh -- check that hack/verify-version.sh can fail.
# Each case builds a fake tree (CMakeLists.txt, README.md, CLAUDE.md, a
# generated k3sm/version.hpp and a throwaway git repo) under a temp dir, runs
# a copy of verify-version.sh against it and asserts the exit code and the
# FAIL text:
#   - a consistent tree passes;
#   - a header macro mismatch, a version_string mismatch, a wrong v* tag on
#     HEAD, a malformed "# vX.Y.Z" GIT_TAG comment and a missing generated
#     header each fail with their own message.
set -euo pipefail
cd "$(dirname "$0")/.."

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

# Isolate git from the caller: the fake trees must never act on the real
# repository or read the host's global or system config.
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE
export GIT_CEILING_DIRECTORIES="$tmp" GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1
sha=0123456789abcdef0123456789abcdef01234567

g() { git -C "$1" -c commit.gpgsign=false -c tag.gpgsign=false "${@:2}"; }

# mktree <name> <macro-version> <string-version>: a fake tree whose CMake
# version is 1.2.3 and whose generated header says the two given versions.
mktree() {
	local d="$tmp/$1" maj min pat
	IFS=. read -r maj min pat <<< "$2"
	mkdir -p "$d/hack" "$d/build/generated/k3sm"
	cp hack/verify-version.sh "$d/hack/"
	printf 'project(k3sm-sdk\n  VERSION 1.2.3\n  LANGUAGES CXX)\n' > "$d/CMakeLists.txt"
	printf '  GIT_TAG %s # v1.2.3\n' "$sha" > "$d/README.md"
	printf 'no pin here\n' > "$d/CLAUDE.md"
	cat > "$d/build/generated/k3sm/version.hpp" <<EOF
#define K3SM_SDK_VERSION_MAJOR $maj
#define K3SM_SDK_VERSION_MINOR $min
#define K3SM_SDK_VERSION_PATCH $pat
inline constexpr char version_string[] = "$3";
EOF
	git init -q "$d"
	g "$d" config user.name selftest
	g "$d" config user.email selftest@example.invalid
	g "$d" add -A
	g "$d" commit -q -m fixture
}

# expect <name> <want-exit> <want-text>
expect() {
	local out rc=0
	out="$(bash "$tmp/$1/hack/verify-version.sh" build 2>&1)" || rc=$?
	if [ "$2" = 0 ]; then
		[ "$rc" = 0 ] || { echo "FAIL: $1: exit $rc, want 0: $out"; exit 1; }
	else
		[ "$rc" != 0 ] || { echo "FAIL: $1: passed, want a failure: $out"; exit 1; }
	fi
	case "$out" in
	*"$3"*) ;;
	*) echo "FAIL: $1: output lacks '$3': $out"; exit 1 ;;
	esac
}

mktree ok 1.2.3 1.2.3
g "$tmp/ok" tag v1.2.3
expect ok 0 "ok (version 1.2.3)"

mktree macro 1.2.4 1.2.3
expect macro 1 "FAIL: header macros say 1.2.4, CMake says 1.2.3"

mktree string 1.2.3 1.3.0
expect string 1 "FAIL: header version_string is 1.3.0, CMake says 1.2.3"

mktree tag 1.2.3 1.2.3
g "$tmp/tag" tag v1.2.4
expect tag 1 "FAIL: tag v1.2.4 on HEAD, CMake version is 1.2.3"

mktree comment 1.2.3 1.2.3
printf '  GIT_TAG %s # v1.2\n' "$sha" > "$tmp/comment/CLAUDE.md"
expect comment 1 "FAIL: CLAUDE.md: malformed tag comment 'v1.2'"

mktree header 1.2.3 1.2.3
rm "$tmp/header/build/generated/k3sm/version.hpp"
expect header 1 "FAIL: build/generated/k3sm/version.hpp was not generated"

echo "    self-test: a consistent tree passes; a macro, version_string, tag, tag-comment or missing-header fault fails"
