# Copyright The k3sm Authors.
# SPDX-License-Identifier: Apache-2.0
# shellcheck shell=bash
#
# nonroot.sh -- sourced by hack/ci.sh and hack/linux-test.sh. The
# permission-denied cases of the reader and writer tests skip when the
# effective uid is 0, because root can open any file, so a gate run as root
# would go green without them. Both scripts call require_non_root before any
# build step. There is no waiver.

# require_non_root [uid]: 0 (silently) when the effective uid is not 0;
# otherwise prints a FAIL message on stderr and returns 1. The uid comes from
# `id -u`; the optional argument stands in for it and exists only for
# hack/verify-non-root-selftest.sh. A uid that is empty or not a number fails
# too, so a broken `id` cannot pass the check.
require_non_root() {
	local uid
	uid="${1:-$(id -u 2>/dev/null || true)}"
	case "$uid" in
	"" | *[!0-9]*)
		echo "FAIL: cannot read the effective uid (id -u printed '$uid')." >&2
		return 1 ;;
	esac
	if [ "$uid" -ne 0 ]; then
		return 0
	fi
	echo "FAIL: refusing to run as root (effective uid 0). The permission-denied cases of the" >&2
	echo "      reader and writer tests skip as root, so a root run would pass without them." >&2
	echo "      Run as a non-root user." >&2
	return 1
}
