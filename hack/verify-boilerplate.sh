#!/usr/bin/env bash
# Copyright The k3sm Authors.
# SPDX-License-Identifier: Apache-2.0
#
# verify-boilerplate.sh [--self-test] -- every C and C++ source and header
# (.h .hh .hpp .hxx .c .cc .cpp .cxx .ipp .inl) starts with the Apache-2.0
# header in hack/boilerplate/boilerplate.cpp.txt, byte for byte.
set -euo pipefail
cd "$(dirname "$0")/.."
header="hack/boilerplate/boilerplate.cpp.txt"

# check <root>: print each file under <root> missing the header; exit 1 if any.
check() {
	local root="$1" n missing=0 f
	n="$(wc -l < "$header" | tr -d ' ')"
	while IFS= read -r -d '' f; do
		# sed reads the whole file, so nothing dies of SIGPIPE under pipefail.
		if ! sed -n "1,${n}p" "$f" | cmp -s - "$header"; then
			echo "    missing header: ${f#"$root"/}"; missing=1
		fi
	done < <(find "$root" \( -name .git -o -name build -o -name out -o -name .cache \) -prune -o \
		-type f \( -name '*.h' -o -name '*.hh' -o -name '*.hpp' -o -name '*.hxx' -o -name '*.c' \
		-o -name '*.cc' -o -name '*.cpp' -o -name '*.cxx' -o -name '*.ipp' -o -name '*.inl' \) -print0)
	return "$missing"
}

if [ "${1:-}" = --self-test ]; then
	tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
	{ cat "$header"; printf '\nint a;\n'; } > "$tmp/good.hpp"
	check "$tmp" > /dev/null || { echo "FAIL: a file with the header was flagged"; exit 1; }
	printf 'int b;\n' > "$tmp/bad.cpp"
	if check "$tmp" > /dev/null; then echo "FAIL: a file without the header passed"; exit 1; fi
	echo "    self-test: a headed file passes, a bare one fails"
	exit 0
fi

if ! check "$PWD"; then
	echo "FAIL: add the header in $header to the files above"; exit 1
fi
echo "    ok"
