#!/usr/bin/env bash
# Check upstream xiaozhi-esp32 for commits touching brand-sensitive files.
#
# Usage: check_upstream.sh [upstream-url] [branch]
# Exit codes: 0 = no brand-sensitive upstream changes, 1 = review required,
#             2 = git failure.
#
# Read-only: fetches into FETCH_HEAD only; never creates branches or touches
# the working tree.

set -u

UPSTREAM_URL="${1:-https://github.com/78/xiaozhi-esp32.git}"
BRANCH="${2:-main}"

BRAND_SENSITIVE_PATHS=(
    "CMakeLists.txt"
    "main/Kconfig"
    "main/Kconfig.projbuild"
    "main/boards/common/wifi_board.cc"
    "main/boards/common/blufi.cpp"
    "scripts/release.py"
    "scripts/build.py"
    "scripts/versions.py"
    "main/assets/"
)

cd "$(git rev-parse --show-toplevel)" || exit 2

echo "Fetching ${UPSTREAM_URL} (${BRANCH})..."
if ! git fetch --no-tags "$UPSTREAM_URL" "$BRANCH"; then
    echo "ERROR: git fetch failed." >&2
    exit 2
fi

BASE=$(git merge-base HEAD FETCH_HEAD)
if [ -z "$BASE" ]; then
    echo "ERROR: cannot determine merge base with FETCH_HEAD." >&2
    exit 2
fi

echo "Comparing ${BASE}..FETCH_HEAD for brand-sensitive paths:"
printf '  %s\n' "${BRAND_SENSITIVE_PATHS[@]}"
echo

COMMITS=$(git log --oneline "${BASE}..FETCH_HEAD" -- "${BRAND_SENSITIVE_PATHS[@]}")
if [ -n "$COMMITS" ]; then
    echo "BRAND-SENSITIVE UPSTREAM CHANGES FOUND:"
    echo "$COMMITS"
    echo
    echo "Review brand patches and re-run: python3 scripts/product/brand_audit.py"
    exit 1
fi

echo "No brand-sensitive upstream changes."
exit 0
