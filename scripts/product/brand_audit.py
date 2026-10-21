#!/usr/bin/env python3
"""Brand audit: report residual upstream brand strings in the source tree.

Scans the sources that a re-branded firmware must keep clean (main/, root
CMakeLists.txt, release tooling) for "xiaozhi"/"tenclass" occurrences, filters
them through product/brand-audit-allowlist.txt, and prints what is left.

Exit codes: 0 = clean (or only allowlisted hits), 1 = residual brand strings.

Usage:
  python3 scripts/product/brand_audit.py               # scan sources
  python3 scripts/product/brand_audit.py --binary app.bin
  python3 scripts/product/brand_audit.py --json
"""

import argparse
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
ALLOWLIST_PATH = REPO_ROOT / "product/brand-audit-allowlist.txt"

BRAND_PATTERN = re.compile(r"xiaozhi|tenclass", re.IGNORECASE)

DEFAULT_ALLOWLIST = [
    r"CONFIG_XIAOZHI_",
    r"XIAOZHI_I2S_PORT",
    r"XIAOZHI_BUILD_STAGES",
    r'menu "Xiaozhi Assistant"',
    r"docs/",
    r"\.md$",
    r"LICENSE",
    r"78/",
    r"main/Kconfig\.projbuild",
]

# Always reported, even when the allowlist matches: re-branding must override
# the upstream OTA default (CONFIG_OTA_URL -> api.tenclass.net), so its line
# must survive the main/Kconfig.projbuild path allowlist entry.
NEVER_ALLOW = [r"tenclass"]

# Default scan set: everything a re-brand must touch.
DEFAULT_SOURCES = [
    REPO_ROOT / "CMakeLists.txt",
    REPO_ROOT / "scripts/release.py",
    REPO_ROOT / "scripts/versions.py",
    REPO_ROOT / "main",
]

SUMMARY_MAX = 100


def load_allowlist(path=ALLOWLIST_PATH):
    """Read one regex per line; '#' comments and blank lines are ignored."""
    if not path.is_file():
        print(f"[brand-audit] allowlist {path} not found, using built-in defaults",
              file=sys.stderr)
        return list(DEFAULT_ALLOWLIST)
    entries = []
    for line in path.read_text(encoding="utf-8").splitlines():
        entry = line.strip()
        if entry and not entry.startswith("#"):
            entries.append(entry)
    return entries


def is_allowed(text, rel_path, allowlist_patterns):
    """A hit is ignored when any allowlist regex matches the line or the path.

    Lines matching NEVER_ALLOW are reported regardless of the allowlist.
    """
    for pattern in NEVER_ALLOW:
        if re.search(pattern, text):
            return False
    for pattern in allowlist_patterns:
        try:
            if re.search(pattern, text) or re.search(pattern, rel_path):
                return True
        except re.error as exc:
            print(f"[brand-audit] bad allowlist regex {pattern!r}: {exc}",
                  file=sys.stderr)
    return False


def iter_text_files(roots):
    for root in roots:
        if root.is_file():
            yield root
        elif root.is_dir():
            for path in sorted(root.rglob("*")):
                if path.is_file():
                    yield path


def scan_sources(roots, allowlist_patterns):
    matches = []
    for path in iter_text_files(roots):
        rel = path.relative_to(REPO_ROOT).as_posix()
        try:
            data = path.read_bytes()
        except OSError as exc:
            print(f"[brand-audit] cannot read {rel}: {exc}", file=sys.stderr)
            continue
        if b"\x00" in data:  # Skip binary blobs inside the scan set (assets, fonts).
            continue
        try:
            lines = data.decode("utf-8").splitlines()
        except UnicodeDecodeError:
            continue
        for number, line in enumerate(lines, start=1):
            if BRAND_PATTERN.search(line) and not is_allowed(line, rel, allowlist_patterns):
                matches.append({
                    "file": rel,
                    "line": number,
                    "text": line.strip()[:SUMMARY_MAX],
                })
    return matches


def scan_binary(path, allowlist_patterns):
    matches = []
    try:
        data = path.read_bytes()
    except OSError as exc:
        print(f"[brand-audit] cannot read {path}: {exc}", file=sys.stderr)
        return matches
    rel = path.name
    for run in re.finditer(rb"[\x20-\x7e]{4,}", data):
        text = run.group(0).decode("ascii", errors="replace")
        if BRAND_PATTERN.search(text) and not is_allowed(text, rel, allowlist_patterns):
            matches.append({
                "file": rel,
                "line": run.start(),
                "text": text[:SUMMARY_MAX],
            })
    return matches


def print_table(matches):
    if not matches:
        print("No residual brand strings found.")
        return
    width = max(len(f"{m['file']}:{m['line']}") for m in matches)
    for m in matches:
        location = f"{m['file']}:{m['line']}"
        print(f"{location:<{width}}  {m['text']}")
    print(f"\n{len(matches)} residual brand string(s) found.")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", type=Path, default=None,
                        help="scan a firmware binary with strings-equivalent extraction")
    parser.add_argument("--json", action="store_true",
                        help="emit machine-readable JSON instead of a table")
    args = parser.parse_args()

    allowlist_patterns = load_allowlist()
    if args.binary:
        matches = scan_binary(args.binary, allowlist_patterns)
    else:
        matches = scan_sources(DEFAULT_SOURCES, allowlist_patterns)

    if args.json:
        print(json.dumps({
            "count": len(matches),
            "clean": not matches,
            "allowlist": allowlist_patterns,
            "matches": matches,
        }, ensure_ascii=False, indent=2))
    else:
        print_table(matches)

    return 1 if matches else 0


if __name__ == "__main__":
    sys.exit(main())
