import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "brand_audit", ROOT / "scripts/product/brand_audit.py")
brand = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(brand)

ALLOWLIST = list(brand.DEFAULT_ALLOWLIST)


class AllowlistTests(unittest.TestCase):
    def test_allowlist_regex_matches_line_text_or_path(self):
        for text, rel_path, expected in (
            ("CONFIG_XIAOZHI_SERVER_WSS_URL", "main/Kconfig.projbuild", True),
            ('menu "Xiaozhi Assistant"', "main/CMakeLists.txt", True),
            ('const char *kName = "xiaozhi";', "docs/notes.md", True),
            ('static const char *kName = "xiaozhi";', "main/application.cc", False),
        ):
            with self.subTest(text=text, rel_path=rel_path):
                self.assertEqual(brand.is_allowed(text, rel_path, ALLOWLIST), expected)

    def test_tenclass_is_never_allowlisted_even_on_matching_paths(self):
        # The main/Kconfig.projbuild path hits the allowlist, but NEVER_ALLOW
        # keeps the upstream OTA default visible for re-branding review.
        line = '    default "https://api.tenclass.net"'
        self.assertFalse(brand.is_allowed(line, "main/Kconfig.projbuild", ALLOWLIST))
        # Without tenclass the same allowlisted path would be exempted.
        self.assertTrue(brand.is_allowed(
            '    default "https://api.example.net"', "main/Kconfig.projbuild", ALLOWLIST))

    def test_load_allowlist_skips_comments_and_blank_lines(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "allowlist.txt"
            path.write_text("# header comment\n\nCONFIG_XIAOZHI_  \n\\.md$\n",
                            encoding="utf-8")
            self.assertEqual(brand.load_allowlist(path), ["CONFIG_XIAOZHI_", r"\.md$"])

    def test_missing_allowlist_file_falls_back_to_built_in_defaults(self):
        with tempfile.TemporaryDirectory() as folder:
            entries = brand.load_allowlist(Path(folder) / "absent.txt")
        self.assertEqual(entries, brand.DEFAULT_ALLOWLIST)
        self.assertIsNot(entries, brand.DEFAULT_ALLOWLIST)


class SourceScanTests(unittest.TestCase):
    def scan_tree(self, folder):
        # scan_sources derives relative paths from the module-level REPO_ROOT;
        # repoint it at the throwaway tree.
        with patch.object(brand, "REPO_ROOT", Path(folder)):
            return brand.scan_sources([Path(folder) / "src"], ALLOWLIST)

    def test_planted_files_report_only_residual_brand_lines(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "src" / "main").mkdir(parents=True)
            (root / "src" / "clean.c").write_text("int main(void) { return 0; }\n")
            (root / "src" / "dirty.c").write_text(
                'static const char *kName = "xiaozhi-esp32";\n')
            (root / "src" / "allowed.md").write_text("# xiaozhi release notes\n")
            (root / "src" / "main" / "Kconfig.projbuild").write_text(
                "config OTA_URL\n    default \"https://api.tenclass.net\"\n")
            matches = self.scan_tree(root)
        by_file = {m["file"]: m for m in matches}
        self.assertEqual(
            set(by_file), {"src/dirty.c", "src/main/Kconfig.projbuild"})
        self.assertEqual(by_file["src/dirty.c"]["line"], 1)
        self.assertIn("xiaozhi-esp32", by_file["src/dirty.c"]["text"])
        # Reported despite matching the path allowlist, because of tenclass.
        self.assertEqual(by_file["src/main/Kconfig.projbuild"]["line"], 2)
        self.assertIn("tenclass", by_file["src/main/Kconfig.projbuild"]["text"])

    def test_tree_without_brand_strings_is_clean(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "src").mkdir()
            (root / "src" / "clean.c").write_text("int main(void) { return 0; }\n")
            (root / "src" / "README.md").write_text("brand-free documentation\n")
            self.assertEqual(self.scan_tree(root), [])


class BinaryScanTests(unittest.TestCase):
    def test_embedded_brand_strings_are_reported(self):
        with tempfile.TemporaryDirectory() as folder:
            dirty = Path(folder) / "merged-binary.bin"
            dirty.write_bytes(
                b"\x00\x01\x02"
                + b"bootloader stage ok"
                + b"\x00\x7f"
                + b"embedded xiaozhi payload"
                + b"\x00"
                + b"ota=https://api.tenclass.net"
                + b"\xff"
            )
            matches = brand.scan_binary(dirty, ALLOWLIST)
        self.assertEqual(len(matches), 2)
        self.assertTrue(all(m["file"] == "merged-binary.bin" for m in matches))
        texts = " | ".join(m["text"] for m in matches)
        self.assertIn("xiaozhi", texts)
        self.assertIn("tenclass", texts)

    def test_normal_payload_without_brand_strings_is_clean(self):
        with tempfile.TemporaryDirectory() as folder:
            clean = Path(folder) / "merged-binary.bin"
            clean.write_bytes(b"\x00" * 16 + b"ordinary firmware strings" + b"\xff\xff")
            self.assertEqual(brand.scan_binary(clean, ALLOWLIST), [])


class CommandLineTests(unittest.TestCase):
    SCRIPT = ROOT / "scripts/product/brand_audit.py"

    def run_cli(self, *args):
        return subprocess.run([sys.executable, str(self.SCRIPT), *args],
                              capture_output=True, text=True, timeout=120)

    def test_json_output_is_valid_and_reports_residual_matches(self):
        with tempfile.TemporaryDirectory() as folder:
            dirty = Path(folder) / "firmware.bin"
            dirty.write_bytes(b"\x00" * 8 + b"hidden xiaozhi marker" + b"\x00")
            result = self.run_cli("--binary", str(dirty), "--json")
        self.assertEqual(result.returncode, 1, result.stderr)
        payload = json.loads(result.stdout)
        self.assertEqual(set(payload), {"count", "clean", "allowlist", "matches"})
        self.assertFalse(payload["clean"])
        self.assertEqual(payload["count"], len(payload["matches"]))
        self.assertEqual([m["file"] for m in payload["matches"]], ["firmware.bin"])
        self.assertIn("xiaozhi", payload["matches"][0]["text"])

    def test_json_clean_binary_exits_zero(self):
        with tempfile.TemporaryDirectory() as folder:
            clean = Path(folder) / "firmware.bin"
            clean.write_bytes(b"\x00" * 8 + b"no brand strings here" + b"\x00")
            result = self.run_cli("--binary", str(clean), "--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        payload = json.loads(result.stdout)
        self.assertTrue(payload["clean"])
        self.assertEqual(payload["count"], 0)
        self.assertEqual(payload["matches"], [])

    def test_table_output_reports_residual_and_clean_results(self):
        with tempfile.TemporaryDirectory() as folder:
            dirty = Path(folder) / "firmware.bin"
            dirty.write_bytes(b"\x00" * 8 + b"hidden xiaozhi marker" + b"\x00")
            dirty_result = self.run_cli("--binary", str(dirty))
            clean = Path(folder) / "firmware.bin"
            clean.write_bytes(b"\x00" * 8 + b"no brand strings here" + b"\x00")
            clean_result = self.run_cli("--binary", str(clean))
        self.assertEqual(dirty_result.returncode, 1)
        self.assertIn("firmware.bin", dirty_result.stdout)
        self.assertIn("1 residual brand string(s) found.", dirty_result.stdout)
        self.assertEqual(clean_result.returncode, 0)
        self.assertIn("No residual brand strings found.", clean_result.stdout)


if __name__ == "__main__":
    unittest.main()
