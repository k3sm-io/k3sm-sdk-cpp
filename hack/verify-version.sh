#!/usr/bin/env bash
# Copyright The k3sm Authors.
# SPDX-License-Identifier: Apache-2.0
#
# verify-version.sh <build-dir> -- the CMake project() VERSION, the generated
# k3sm/version.hpp in <build-dir> and the git tag all name one version.
#   - the generated header's macros and version_string equal the CMake version;
#   - a v* tag on HEAD equals the CMake version;
#   - a "# vX.Y.Z" tag comment beside a GIT_TAG in README.md or CLAUDE.md, when
#     one is present, is a well-formed release tag (the doc pins a placeholder
#     until a release exists, so it is checked for shape, not for equality).
set -euo pipefail
cd "$(dirname "$0")/.."
build="${1:?usage: verify-version.sh <build-dir>}"
hdr="$build/generated/k3sm/version.hpp"
[ -f "$hdr" ] || { echo "FAIL: $hdr was not generated"; exit 1; }

cmake_ver="$(sed -n 's/^[[:space:]]*VERSION[[:space:]]\{1,\}\([0-9][0-9.]*\).*/\1/p' CMakeLists.txt | head -n 1)"
[ -n "$cmake_ver" ] || { echo "FAIL: no project() VERSION in CMakeLists.txt"; exit 1; }

def() { sed -n "s/^#define $1 \\(.*\\)\$/\\1/p" "$hdr"; }
hdr_ver="$(def K3SM_SDK_VERSION_MAJOR).$(def K3SM_SDK_VERSION_MINOR).$(def K3SM_SDK_VERSION_PATCH)"
hdr_str="$(sed -n 's/.*version_string\[\] = "\(.*\)";/\1/p' "$hdr")"
[ "$hdr_ver" = "$cmake_ver" ] || { echo "FAIL: header macros say $hdr_ver, CMake says $cmake_ver"; exit 1; }
[ "$hdr_str" = "$cmake_ver" ] || { echo "FAIL: header version_string is $hdr_str, CMake says $cmake_ver"; exit 1; }

tags="$(git tag --points-at HEAD 'v*' 2>/dev/null || true)"
for t in $tags; do
	[ "$t" = "v$cmake_ver" ] || { echo "FAIL: tag $t on HEAD, CMake version is $cmake_ver"; exit 1; }
done

for f in README.md CLAUDE.md; do
	while IFS= read -r t; do
		[[ "$t" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "FAIL: $f: malformed tag comment '$t'"; exit 1; }
	done < <(sed -n 's/.*GIT_TAG[[:space:]]\{1,\}[0-9a-f]\{40\}[[:space:]]*#[[:space:]]*\([^[:space:]]*\).*/\1/p' "$f")
done
echo "    ok (version $cmake_ver)"
