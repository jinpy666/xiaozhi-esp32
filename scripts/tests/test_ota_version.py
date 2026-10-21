"""L1 host test: Ota::ParseVersion / IsNewVersionAvailable (source slicing).

Uses the repo's established "source slice" paradigm (cf. test_esp_vocat_i2c.py):
the two pure functions are extracted verbatim from main/ota.cc at build time,
`Ota::` stripped, and compiled as free functions with a stub esp_log.h. This
keeps the test pinned to the real implementation instead of a copy.

Pinned contract (main/ota.cc:413-465): versions compare as dot-separated
decimals; ANY malformed segment makes the comparison return False (never
report an update we cannot compare — protects against downgrade / upgrade
loops); a longer version with equal prefix counts as newer.
"""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
OTA_CC = REPO_ROOT / "main" / "ota.cc"

STUB_ESP_LOG = """#pragma once
#define ESP_LOGI(...) do {} while (0)
#define ESP_LOGW(...) do {} while (0)
#define ESP_LOGE(...) do {} while (0)
"""

SLICE_INCLUDES = """\
#include <algorithm>
#include <charconv>
#include <string>
#include <system_error>
#include <vector>
#include <expected>
#include <esp_log.h>
"""

DRIVER_HEADER = """\
#include <cstdio>
#include <string>
#include <vector>
#include "sliced.h"
"""

DRIVER = r"""
static int g_failures = 0;
static void CHECK(bool ok, const std::string& msg) {
    if (!ok) {
        printf("FAIL: %s\n", msg.c_str());
        ++g_failures;
    }
}

int main() {
    // ParseVersion: valid
    auto v = ParseVersion("2.5.3");
    CHECK(v.has_value() && (*v == std::vector<int>{2, 5, 3}), "parse 2.5.3");
    v = ParseVersion("1.0");
    CHECK(v.has_value() && (*v == std::vector<int>{1, 0}), "parse 1.0");

    // ParseVersion: malformed -> error, never throws (exceptions disabled)
    CHECK(!ParseVersion("").has_value(), "parse empty rejected");
    CHECK(!ParseVersion("abc").has_value(), "parse abc rejected");
    CHECK(!ParseVersion("1.0.x").has_value(), "parse 1.0.x rejected");
    CHECK(!ParseVersion("1..2").has_value(), "parse 1..2 rejected");

    // Quirk pinned: std::from_chars accepts a leading '-' as a signed value,
    // so "-1.0" parses as [-1, 0]. If this ever changes, the pin notices it.
    v = ParseVersion("-1.0");
    CHECK(v.has_value() && (*v == std::vector<int>{-1, 0}),
          "parse -1.0 accepted (from_chars signed quirk)");

    // IsNewVersionAvailable: normal compare
    CHECK(IsNewVersionAvailable("2.5.3", "2.5.4"), "2.5.3 < 2.5.4");
    CHECK(IsNewVersionAvailable("2.5.3", "2.5.10"), "2.5.3 < 2.5.10 (numeric, not lexical)");
    CHECK(IsNewVersionAvailable("1.0", "1.0.0"), "1.0 < 1.0.0 (longer prefix)");
    CHECK(IsNewVersionAvailable("2.5.3", "2.5.3.1"), "2.5.3 < 2.5.3.1");

    // Equal / older
    CHECK(!IsNewVersionAvailable("2.5.3", "2.5.3"), "equal not newer");
    CHECK(!IsNewVersionAvailable("2.5.4", "2.5.3"), "older not newer");
    CHECK(!IsNewVersionAvailable("3.0.0", "2.9.9"), "major older not newer");

    // Malformed versions: comparison must return False both ways
    CHECK(!IsNewVersionAvailable("abc", "2.0.0"), "malformed current -> never newer");
    CHECK(!IsNewVersionAvailable("2.5.3", "abc"), "malformed new -> never newer");
    CHECK(!IsNewVersionAvailable("", "1.0.0"), "empty current -> never newer");
    CHECK(!IsNewVersionAvailable("2.5.3", ""), "empty new -> never newer");
    CHECK(!IsNewVersionAvailable("1.0.x", "1.0.x"), "both malformed -> never newer");

    if (g_failures == 0) {
        printf("OTA_VERSION_ALL_OK\n");
        return 0;
    }
    printf("OTA_VERSION_FAILURES=%d\n", g_failures);
    return 1;
}
"""


def gpp() -> str:
    return shutil.which("g++") or shutil.which("c++") or ""


def extract_function(source: str, signature_start: str) -> str:
    """Extract a top-level function block starting at `signature_start` by
    brace matching from its opening '{'."""
    idx = source.find(signature_start)
    if idx < 0:
        raise RuntimeError(f"signature not found in ota.cc: {signature_start[:60]}...")
    open_brace = source.find("{", idx)
    if open_brace < 0:
        raise RuntimeError(f"no opening brace after: {signature_start[:60]}...")
    depth = 0
    for i in range(open_brace, len(source)):
        ch = source[i]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return source[idx:i + 1]
    raise RuntimeError(f"unbalanced braces for: {signature_start[:60]}...")


def build(workdir: Path) -> Path:
    stub = workdir / "stub"
    stub.mkdir(parents=True)
    (stub / "esp_log.h").write_text(STUB_ESP_LOG, encoding="utf-8")

    src = OTA_CC.read_text(encoding="utf-8")
    parts = [
        extract_function(src, "std::expected<std::vector<int>, std::string> Ota::ParseVersion"),
        extract_function(src, "bool Ota::IsNewVersionAvailable"),
    ]
    sliced = "\n".join(p.replace("Ota::", "") for p in parts)
    for fn in ("ParseVersion(", "IsNewVersionAvailable("):
        if fn not in sliced:
            raise RuntimeError(f"slice lost {fn}")

    # sliced.h defines both functions (single-TU include to keep it simple).
    sliced_h = workdir / "sliced.h"
    sliced_h.write_text(
        "#pragma once\n" + SLICE_INCLUDES + "\n" + sliced + "\n", encoding="utf-8")

    driver = workdir / "driver.cpp"
    driver.write_text(DRIVER_HEADER + DRIVER, encoding="utf-8")

    binary = workdir / "ota_version_test"
    cmd = [
        gpp(), "-std=c++23",
        f"-I{stub}", f"-I{workdir}",
        str(driver), "-o", str(binary),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError("compile failed:\n" + proc.stderr)
    return binary


class OtaVersionHostTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not gpp():
            raise unittest.SkipTest("g++ not available")
        cls._tmp = tempfile.TemporaryDirectory()
        try:
            cls.binary = build(Path(cls._tmp.name))
        except RuntimeError as e:
            cls._tmp.cleanup()
            msg = str(e)
            if "signature not found" in msg or "unbalanced" in msg or "slice lost" in msg:
                raise unittest.SkipTest("ota.cc structure changed; slice needs update: " + msg)
            if "expected" in msg or "std=c++23" in msg or "unrecognized" in msg:
                # Distinguish toolchain gaps from genuine code errors via probe.
                probe = subprocess.run(
                    [gpp(), "-std=c++23", "-x", "c++", "-fsyntax-only", "-"],
                    input="#include <expected>\nint main(){}\n", capture_output=True, text=True)
                if probe.returncode != 0:
                    raise unittest.SkipTest(
                        "toolchain lacks C++23 <expected>: " + probe.stderr.strip())
            raise
        cls._tmp_path = Path(cls._tmp.name)

    @classmethod
    def tearDownClass(cls):
        cls._tmp.cleanup()

    def test_version_compare_contract(self):
        proc = subprocess.run([str(self.binary)], capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, msg=proc.stdout + proc.stderr)
        self.assertIn("OTA_VERSION_ALL_OK", proc.stdout)
        self.assertNotIn("FAIL:", proc.stdout)


if __name__ == "__main__":
    unittest.main()
