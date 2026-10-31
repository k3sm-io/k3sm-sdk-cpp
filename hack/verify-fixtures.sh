#!/usr/bin/env bash
# Copyright The k3sm Authors.
# SPDX-License-Identifier: Apache-2.0
#
# verify-fixtures.sh -- the vendored page fixtures are the apis bytes.
#
# tests/fixtures/page is a byte-for-byte copy of budget/v1alpha1/testdata/page
# in github.com/k3sm-io/apis at commit
# 115e311a5bc5e7f62c1a6c541d15d3f661c9c59b. Its SHA256SUMS comes with it.
# This check fails when any file differs from its digest, when a listed file
# is missing, or when the directory holds a file SHA256SUMS does not list.
# To move to a newer apis commit, copy the whole directory again (never edit
# a fixture here) and update the commit above.
#
#   hack/verify-fixtures.sh              check tests/fixtures/page
#   hack/verify-fixtures.sh --self-test  check that the check can fail
set -euo pipefail
cd "$(dirname "$0")/.."

sha256() {
	if command -v sha256sum >/dev/null; then
		sha256sum "$@"
	else
		shasum -a 256 "$@"
	fi
}

# check <dir>: every file of SHA256SUMS matches, and nothing is unlisted.
check() {
	local dir="$1" bad=0 f want got
	[ -s "$dir/SHA256SUMS" ] || { echo "    no SHA256SUMS in $dir"; return 1; }
	while read -r want f; do
		[ -n "$f" ] || continue
		if [ ! -f "$dir/$f" ]; then
			echo "    missing: $f"; bad=1; continue
		fi
		got="$(cd "$dir" && sha256 "$f" | cut -d' ' -f1)"
		if [ "$got" != "$want" ]; then
			echo "    digest mismatch: $f"; bad=1
		fi
	done < "$dir/SHA256SUMS"
	while IFS= read -r f; do
		f="${f#./}"
		[ "$f" = SHA256SUMS ] && continue
		if ! cut -d' ' -f3- "$dir/SHA256SUMS" | grep -qxF "$f"; then
			echo "    not in SHA256SUMS: $f"; bad=1
		fi
	done < <(cd "$dir" && find . -type f | sort)
	return "$bad"
}

if [ "${1:-}" = --self-test ]; then
	tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
	cp -R tests/fixtures/page "$tmp/ok"
	check "$tmp/ok" > /dev/null || { echo "FAIL: an unchanged copy was flagged"; exit 1; }
	cp -R tests/fixtures/page "$tmp/flip"
	printf 'X' | dd of="$tmp/flip/darwin.v1.page" bs=1 seek=300 conv=notrunc 2>/dev/null
	if check "$tmp/flip" > /dev/null; then echo "FAIL: a changed byte passed"; exit 1; fi
	cp -R tests/fixtures/page "$tmp/extra"
	printf 'x' > "$tmp/extra/extra.page"
	if check "$tmp/extra" > /dev/null; then echo "FAIL: an unlisted file passed"; exit 1; fi
	cp -R tests/fixtures/page "$tmp/gone"
	rm "$tmp/gone/negative/short.v1.page"
	if check "$tmp/gone" > /dev/null; then echo "FAIL: a missing file passed"; exit 1; fi
	echo "    self-test: a copy passes; a changed byte, an unlisted file and a missing file fail"
	exit 0
fi

if ! check tests/fixtures/page; then
	echo "FAIL: tests/fixtures/page differs from its SHA256SUMS; re-copy it from apis"; exit 1
fi
echo "    ok ($(grep -c . tests/fixtures/page/SHA256SUMS) files)"
