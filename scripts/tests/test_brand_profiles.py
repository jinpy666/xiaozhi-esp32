"""OEM brand profile loading, validation, staging and sdkconfig generation."""

import gzip
import importlib.util
import json
import os
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("build_brand", ROOT / "scripts/build.py")
build = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(build)


class BrandProfileTests(unittest.TestCase):
    def test_example_profile_is_listed_and_valid(self):
        self.assertIn("example", build._collect_brand_profiles())
        profile = build._load_brand_profile("example")
        self.assertEqual(profile["brand_name"], "Example")
        self.assertEqual(profile["web"]["logo"], "profiles/example/logo.svg")

    def test_unknown_profile_lists_available(self):
        with self.assertRaises(ValueError) as caught:
            build._load_brand_profile("no-such-tenant")
        self.assertIn("example", str(caught.exception))

    def test_rejects_unsafe_profile_names(self):
        for name in ("", "../example", "a/b", ".hidden"):
            with self.assertRaises(ValueError, msg=name):
                build._load_brand_profile(name)

    def _load_in_temp_project(self, payload: dict) -> None:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        (root / "product/profiles").mkdir(parents=True)
        (root / "product/profiles/demo.json").write_text(
            json.dumps(payload), encoding="utf-8"
        )
        previous = os.getcwd()
        os.chdir(root)
        self.addCleanup(os.chdir, previous)
        build._load_brand_profile("demo")

    def test_rejects_invalid_fields(self):
        cases = [
            {"web": {"accent_color": "red"}},
            {"web": {"accent_color": "#12345"}},
            {"web": {"theme": "blue"}},
            {"web": {"logo": "profiles/demo/missing.svg"}},
            {"web": {"portal_pack": "profiles/demo/portal"}},
            {"web": {"brand_config": ["not", "an", "object"]}},
            {"sdkconfig": {"NOT_CONFIG": "x"}},
            {"sdkconfig": {"CONFIG_OK": 1.5}},
            {"project_name": "Bad Name"},
            {"project_ver": "1.0"},
            {"brand_name": 42},
        ]
        for payload in cases:
            with self.assertRaises(ValueError, msg=json.dumps(payload)):
                self._load_in_temp_project(payload)

    def test_rejects_paths_escaping_product_dir(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        (root / "product/profiles").mkdir(parents=True)
        (root / "product/profiles/demo.json").write_text(
            json.dumps({"web": {"logo": "../../outside.svg"}}), encoding="utf-8"
        )
        previous = os.getcwd()
        os.chdir(root)
        self.addCleanup(os.chdir, previous)
        with self.assertRaises(ValueError):
            build._load_brand_profile("demo")


class BrandSdkconfigTests(unittest.TestCase):
    def test_literal_serialization(self):
        self.assertEqual(build._sdkconfig_literal(True), "y")
        self.assertEqual(build._sdkconfig_literal(False), "n")
        self.assertEqual(build._sdkconfig_literal(8), "8")
        self.assertEqual(build._sdkconfig_literal("y"), "y")
        self.assertEqual(build._sdkconfig_literal('"quoted"'), '"quoted"')
        self.assertEqual(build._sdkconfig_literal("Acme"), '"Acme"')
        self.assertEqual(build._sdkconfig_literal('a"b\\c'), '"a\\"b\\\\c"')

    def test_options_cover_web_fields_and_profile_overrides(self):
        profile = {
            "brand_name": "Acme",
            "web": {
                "title": "Acme Setup",
                "accent_color": "#112233",
                "theme": "dark",
                "footer": "Powered by Acme",
                "support_url": "https://acme.example/help",
                "brand_config": {"notice": "hello"},
            },
            "sdkconfig": {
                "CONFIG_OTA_URL": "https://ota.acme.example/xiaozhi/ota/",
                "CONFIG_SOME_FLAG": False,
            },
        }
        staged = {
            "logo_format": "svg",
            "brand_config": "product/brand_assets/brand_config.json",
        }
        options = build._brand_sdkconfig_options(profile, staged)
        self.assertIn('CONFIG_PRODUCT_BRAND_NAME="Acme"', options)
        self.assertIn('CONFIG_PRODUCT_BRAND_PORTAL_TITLE="Acme Setup"', options)
        self.assertIn('CONFIG_PRODUCT_BRAND_PORTAL_ACCENT="#112233"', options)
        self.assertIn('CONFIG_PRODUCT_BRAND_PORTAL_THEME="dark"', options)
        self.assertIn('CONFIG_PRODUCT_BRAND_PORTAL_FOOTER="Powered by Acme"', options)
        self.assertIn(
            'CONFIG_PRODUCT_BRAND_PORTAL_SUPPORT_URL="https://acme.example/help"',
            options,
        )
        self.assertIn("CONFIG_PRODUCT_BRAND_LOGO_SVG=y", options)
        self.assertIn("CONFIG_PRODUCT_BRAND_PORTAL_CONFIG=y", options)
        self.assertIn('CONFIG_OTA_URL="https://ota.acme.example/xiaozhi/ota/"', options)
        self.assertIn("CONFIG_SOME_FLAG=n", options)

    def test_empty_profile_produces_no_options(self):
        options = build._brand_sdkconfig_options(
            {"web": {}}, {"logo_format": None, "brand_config": None}
        )
        self.assertEqual(options, [])

    def test_brand_merge_wins_over_board_values(self):
        merged = build._merge_sdkconfig_options(
            ['CONFIG_PRODUCT_BRAND_NAME="Board"', "CONFIG_KEEP=y"],
            ['CONFIG_PRODUCT_BRAND_NAME="Tenant"'],
        )
        self.assertEqual(
            merged, ['CONFIG_PRODUCT_BRAND_NAME="Tenant"', "CONFIG_KEEP=y"]
        )


class BrandStagingTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        previous = os.getcwd()
        os.chdir(self.root)
        self.addCleanup(os.chdir, previous)

        pack = self.root / "product/profiles/demo/portal"
        pack.mkdir(parents=True)
        (pack / "index.html").write_text("<html>tenant</html>", encoding="utf-8")
        (pack / "done.html").write_text("<html>done</html>", encoding="utf-8")
        (self.root / "product/profiles/demo/logo.svg").write_text(
            "<svg/>", encoding="utf-8"
        )
        self.profile = {
            "brand_name": "Demo",
            "project_name": "demo-voice",
            "project_ver": "9.9.9",
            "web": {
                "portal_pack": "profiles/demo/portal",
                "logo": "profiles/demo/logo.svg",
                "brand_config": {
                    "notice": "hello",
                    "sections": {"advanced_tab": False},
                },
            },
        }

    def test_stage_and_clean(self):
        staged = build._stage_brand_profile("demo", self.profile)
        assets = self.root / "product/brand_assets"

        self.assertEqual(staged["logo_format"], "svg")
        self.assertTrue((assets / "logo").is_file())
        self.assertTrue((assets / "portal/wifi_configuration.html").is_file())
        with gzip.open(assets / "portal/wifi_configuration.html.gz", "rb") as file:
            self.assertEqual(file.read(), b"<html>tenant</html>")
        with gzip.open(assets / "portal/wifi_configuration_done.html.gz", "rb") as file:
            self.assertEqual(file.read(), b"<html>done</html>")
        config = json.loads((assets / "brand_config.json").read_text(encoding="utf-8"))
        self.assertEqual(config["notice"], "hello")

        gen = (self.root / "product/brand.gen.cmake").read_text(encoding="utf-8")
        self.assertIn('set(BRAND_PROJECT_NAME "demo-voice")', gen)
        self.assertIn('set(BRAND_PROJECT_VER "9.9.9")', gen)

        build._clean_brand_staging()
        self.assertFalse(assets.exists())
        self.assertFalse((self.root / "product/brand.gen.cmake").exists())

    def test_done_page_is_optional(self):
        (self.root / "product/profiles/demo/portal/done.html").unlink()
        build._stage_brand_profile("demo", self.profile)
        assets = self.root / "product/brand_assets/portal"
        self.assertTrue((assets / "wifi_configuration.html.gz").is_file())
        self.assertFalse((assets / "wifi_configuration_done.html").exists())

    def test_oversized_page_is_rejected(self):
        page = self.root / "product/profiles/demo/portal/index.html"
        page.write_text("x" * (build._BRAND_PORTAL_MAX_RAW_BYTES + 1), encoding="utf-8")
        with self.assertRaises(ValueError):
            build._stage_brand_profile("demo", self.profile)

    def test_restaging_removes_previous_tenant_assets(self):
        build._stage_brand_profile("demo", self.profile)
        profile = {"brand_name": "Other", "web": {}}
        build._stage_brand_profile("other", profile)
        assets = self.root / "product/brand_assets"
        self.assertFalse((assets / "portal").exists())
        self.assertFalse((assets / "logo").exists())
        self.assertFalse((self.root / "product/brand.gen.cmake").exists())


class BrandPortalConfigTests(unittest.TestCase):
    def test_declarative_config_wins_and_kconfig_fields_fill_in(self):
        profile = {
            "brand_name": "Acme",
            "web": {
                "accent_color": "#112233",
                "footer": "F",
                "brand_config": {"notice": "n", "accent": "#ff0000"},
            },
        }
        config = build._brand_portal_config(
            profile, {"logo_format": "png", "brand_config": "staged"}
        )
        self.assertEqual(config["notice"], "n")
        self.assertEqual(config["accent"], "#ff0000")
        self.assertEqual(config["name"], "Acme")
        self.assertEqual(config["footer"], "F")
        self.assertEqual(config["logo"], "/brand-logo")

    def test_unbranded_profile_produces_empty_payload(self):
        self.assertEqual(
            build._brand_portal_config(
                {"web": {}}, {"logo_format": None, "brand_config": None}
            ),
            {},
        )


class BrandStagingCacheTests(unittest.TestCase):
    def test_write_if_changed_preserves_timestamps(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "nested" / "artifact.bin"
            self.assertTrue(build._write_if_changed(path, b"first"))
            mtime = path.stat().st_mtime_ns
            self.assertFalse(build._write_if_changed(path, b"first"))
            self.assertEqual(path.stat().st_mtime_ns, mtime)
            self.assertTrue(build._write_if_changed(path, b"second"))
            self.assertEqual(path.read_bytes(), b"second")


class BrandRestagingTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        previous = os.getcwd()
        os.chdir(self.root)
        self.addCleanup(os.chdir, previous)

        pack = self.root / "product/profiles/demo/portal"
        pack.mkdir(parents=True)
        (pack / "index.html").write_text("<html>tenant</html>", encoding="utf-8")
        (pack / "done.html").write_text("<html>done</html>", encoding="utf-8")
        (self.root / "product/profiles/demo/logo.svg").write_text(
            "<svg/>", encoding="utf-8"
        )
        self.profile = {
            "brand_name": "Demo",
            "project_name": "demo-voice",
            "project_ver": "9.9.9",
            "web": {
                "portal_pack": "profiles/demo/portal",
                "logo": "profiles/demo/logo.svg",
                "brand_config": {"notice": "hello"},
            },
        }
        self.assets = self.root / "product/brand_assets"

    def _snapshot(self):
        files = {
            path.relative_to(self.assets).as_posix(): (
                path.stat().st_mtime_ns,
                path.read_bytes(),
            )
            for path in self.assets.rglob("*")
            if path.is_file()
        }
        gen = self.root / "product/brand.gen.cmake"
        files["brand.gen.cmake"] = (gen.stat().st_mtime_ns, gen.read_bytes())
        return files

    def test_identical_restaging_keeps_timestamps_for_cache_reuse(self):
        build._stage_brand_profile("demo", self.profile)
        before = self._snapshot()
        build._stage_brand_profile("demo", self.profile)
        self.assertEqual(before, self._snapshot())

    def test_changed_parameter_rewrites_only_that_artifact(self):
        build._stage_brand_profile("demo", self.profile)
        before = self._snapshot()
        changed = {
            **self.profile,
            "project_ver": "9.9.10",
            "web": {**self.profile["web"], "logo": None},
        }
        build._stage_brand_profile("demo", changed)
        after = self._snapshot()
        # Portal pages and brand config are byte-identical, so they are reused.
        for key in (
            "portal/wifi_configuration.html",
            "portal/wifi_configuration.html.gz",
            "brand_config.json",
        ):
            self.assertEqual(before[key][0], after[key][0], key)
        # The version override changed and the dropped logo was pruned.
        self.assertNotEqual(before["brand.gen.cmake"][0], after["brand.gen.cmake"][0])
        self.assertNotIn("logo", after)

    def test_switching_to_profile_without_assets_prunes_stale_files(self):
        build._stage_brand_profile("demo", self.profile)
        build._stage_brand_profile("demo", {"brand_name": "Demo2", "web": {}})
        self.assertFalse((self.assets / "portal").exists())
        self.assertFalse((self.assets / "logo").exists())
        self.assertFalse((self.assets / "brand_config.json").exists())
        self.assertFalse((self.root / "product/brand.gen.cmake").exists())


class BrandCliWiringTests(unittest.TestCase):
    VARIANTS = [
        {
            "board": "bread-compact-wifi",
            "name": "bread-compact-wifi",
            "target": "esp32s3",
            "full_name": "bread-compact-wifi",
        }
    ]

    def test_brand_options_are_staged_and_forwarded_last(self):
        from unittest import mock

        with (
            mock.patch.object(build, "_detect_idf_version", return_value=(6, 0, 2)),
            mock.patch.object(build, "_board_type_exists", return_value=True),
            mock.patch.object(build, "_collect_variants", return_value=self.VARIANTS),
            mock.patch.object(
                build, "_load_brand_profile", return_value={"brand_name": "Acme"}
            ),
            mock.patch.object(
                build,
                "_stage_brand_profile",
                return_value={"logo_format": "svg", "brand_config": None},
            ) as stage,
            mock.patch.object(
                build,
                "_brand_sdkconfig_options",
                return_value=['CONFIG_PRODUCT_BRAND_NAME="Acme"'],
            ),
            mock.patch.object(build, "build_board") as build_board,
        ):
            build.main(["bread-compact-wifi", "--brand", "example"])

        stage.assert_called_once_with("example", {"brand_name": "Acme"})
        build_board.assert_called_once_with(
            "bread-compact-wifi",
            config_filename="config.json",
            name_filter="bread-compact-wifi",
            create_zip=False,
            language=None,
            wake_word=None,
            build_options=None,
            idf_version=(6, 0, 2),
            brand_options=['CONFIG_PRODUCT_BRAND_NAME="Acme"'],
            brand_label="example",
        )

    def test_unbranded_invocation_clears_stale_staging(self):
        from unittest import mock

        with (
            mock.patch.object(build, "_detect_idf_version", return_value=(6, 0, 2)),
            mock.patch.object(build, "_board_type_exists", return_value=True),
            mock.patch.object(build, "_collect_variants", return_value=self.VARIANTS),
            mock.patch.object(build, "_clean_brand_staging") as clean,
            mock.patch.object(build, "build_board") as build_board,
        ):
            build.main(["bread-compact-wifi"])

        clean.assert_called_once_with()
        self.assertEqual(build_board.call_args.kwargs["brand_options"], [])
        self.assertIsNone(build_board.call_args.kwargs["brand_label"])


if __name__ == "__main__":
    unittest.main()
