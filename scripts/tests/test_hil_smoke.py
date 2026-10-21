"""Offline tests for scripts/hil/hil_smoke.py (pure-logic core + replay CLI)."""

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts" / "hil"))

import hil_smoke  # noqa: E402


def make_runner(milestones=None):
    return hil_smoke.MilestoneRunner(milestones or hil_smoke.DEFAULT_MILESTONES)


class DefaultMilestonesTests(unittest.TestCase):
    def test_defaults_are_valid_regexes(self):
        for m in hil_smoke.DEFAULT_MILESTONES:
            Milestone = hil_smoke.Milestone.from_dict(m)
            self.assertTrue(Milestone.pattern.pattern)
            self.assertGreater(Milestone.timeout_s, 0)

    def test_list_milestones_cli_prints_json(self):
        out = subprocess.run(
            [sys.executable, str(REPO_ROOT / "scripts" / "hil" / "hil_smoke.py"),
             "--list-milestones"],
            capture_output=True, text=True)
        self.assertEqual(out.returncode, 0)
        data = json.loads(out.stdout)
        self.assertIsInstance(data, list)


class RunnerTests(unittest.TestCase):
    def test_boot_and_runtime_match_passes(self):
        r = make_runner()
        r.feed_line("I (30) boot: ESP-IDF v6.1-xxx 3rd stage bootloader", 1.0)
        r.feed_line("I (5000) main: min free heap 123456", 5.0)
        r.evaluate(5.5)
        res = r.result()
        self.assertTrue(res["passed"])
        self.assertEqual(res["missing"], [])
        self.assertEqual([m["name"] for m in res["matched"]], ["boot", "runtime"])

    def test_missing_required_milestone_fails_on_timeout(self):
        r = make_runner()
        r.feed_line("I (30) boot: ESP-IDF", 1.0)  # boot matched, runtime never
        r.evaluate(70.0)
        res = r.result()
        self.assertFalse(res["passed"])
        self.assertIn("runtime", res["missing"])

    def test_panic_line_fails_immediately(self):
        r = make_runner()
        r.feed_line("Guru Meditation Error: Core 0 panic'ed (StoreProhibited)", 2.0)
        res = r.result()
        self.assertFalse(res["passed"])
        self.assertIn("Guru Meditation", res["panic"])

    def test_optional_milestone_expiry_is_not_a_failure(self):
        ms = [
            {"name": "boot", "pattern": r"\bboot:", "timeout_s": 15, "required": True},
            {"name": "nice_to_have", "pattern": r"(?i)ota", "timeout_s": 5, "required": False},
        ]
        r = make_runner(ms)
        r.feed_line("I (30) boot: ESP-IDF", 1.0)
        r.evaluate(20.0)  # optional milestone window elapsed without a match
        self.assertTrue(r.result()["passed"])

    def test_deadline_anchors_at_first_line_not_wall_clock(self):
        r = make_runner()
        r.evaluate(999.0)  # nothing anchored yet: no-op
        r.feed_line("I (30) boot: ESP-IDF", 5000.0)  # start anchors here
        matched = {x["name"]: x["at_s"] for x in r.result()["matched"]}
        self.assertEqual(matched["boot"], 0.0)  # elapsed measured from first line

    def test_milestones_file_override(self):
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "ms.json"
            p.write_text(json.dumps(
                [{"name": "custom", "pattern": "HELLO", "timeout_s": 10}]), encoding="utf-8")
            r = make_runner(hil_smoke.load_milestones(str(p)))
            r.feed_line("xx HELLO yy", 1.0)
            self.assertTrue(r.result()["passed"])
            self.assertEqual(r.result()["matched"][0]["name"], "custom")


class ReplayCliTests(unittest.TestCase):
    def _run(self, capture_text, extra=None):
        with tempfile.TemporaryDirectory() as td:
            f = Path(td) / "capture.log"
            f.write_text(capture_text, encoding="utf-8")
            return subprocess.run(
                [sys.executable, str(REPO_ROOT / "scripts" / "hil" / "hil_smoke.py"),
                 "--from-file", str(f), "--json", *(extra or [])],
                capture_output=True, text=True)

    def test_passing_capture_exits_zero(self):
        capture = "[0.5] I (30) boot: ESP-IDF v6.1\n" \
                  "[3.0] I (3000) main: heap free=200000\n"
        out = self._run(capture)
        self.assertEqual(out.returncode, 0)
        self.assertTrue(json.loads(out.stdout)["passed"])

    def test_capture_missing_milestone_exits_one(self):
        capture = "[0.5] I (30) boot: ESP-IDF v6.1\n" \
                  "[10.0] nothing else happens\n"
        out = self._run(capture)
        self.assertEqual(out.returncode, 1)
        res = json.loads(out.stdout)
        self.assertFalse(res["passed"])
        self.assertIn("runtime", res["missing"])

    def test_panic_capture_exits_one_with_hint(self):
        capture = "[0.5] I (30) boot: ESP-IDF v6.1\n" \
                  "[2.0] Guru Meditation Error: Core 0 panic'ed (IllegalInstruction)\n"
        out = self._run(capture)
        self.assertEqual(out.returncode, 1)
        self.assertIn("espcoredump", out.stderr)

    def test_plain_lines_without_timestamp_are_spaced(self):
        capture = "I (30) boot: ok\nheap stats here\n"
        out = self._run(capture)
        self.assertEqual(out.returncode, 0)

    def test_usage_error_exits_two(self):
        out = subprocess.run(
            [sys.executable, str(REPO_ROOT / "scripts" / "hil" / "hil_smoke.py"), "--json"],
            capture_output=True, text=True)
        self.assertEqual(out.returncode, 2)


if __name__ == "__main__":
    unittest.main()
