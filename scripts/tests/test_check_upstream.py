import os
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/product/check_upstream.sh"


class CheckUpstreamTests(unittest.TestCase):
    """Exercise check_upstream.sh against small local Git histories.

    A throwaway working repo shares its baseline with a bare "upstream"
    repository. The upstream clone appends commits; running the script from
    the working repo (cwd) fetches the upstream over file:// and must only
    flag commits touching brand-sensitive paths.
    """

    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.gitconfig = self.root / "gitconfig"
        self.gitconfig.write_text("[safe]\n\tdirectory = *\n")
        self.work = self.root / "work"
        self.upstream = self.root / "upstream.git"
        self.git(self.root, "init", "--quiet", "-b", "main", "work")
        self.git(self.root, "init", "--quiet", "--bare", "-b", "main",
                 "upstream.git")
        (self.work / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.16)\nproject(renamed_firmware)\n")
        (self.work / "main").mkdir()
        (self.work / "main" / "app.c").write_text("int app_main(void) { return 0; }\n")
        (self.work / "README.md").write_text("renamed firmware\n")
        self.git(self.work, "add", ".")
        self.git(self.work, "commit", "--quiet", "-m", "baseline")
        self.git(self.work, "push", "--quiet", str(self.upstream), "main")
        self.git(self.root, "clone", "--quiet", str(self.upstream), "upstream-wt")
        self.wt = self.root / "upstream-wt"

    def git(self, repo, *args):
        env = dict(os.environ)
        env.update({
            "GIT_CONFIG_GLOBAL": str(self.gitconfig),
            "GIT_AUTHOR_NAME": "brand test",
            "GIT_AUTHOR_EMAIL": "brand-test@example.invalid",
            "GIT_COMMITTER_NAME": "brand test",
            "GIT_COMMITTER_EMAIL": "brand-test@example.invalid",
        })
        result = subprocess.run(
            ["git", "-c", "user.name=brand test", "-c",
             "user.email=brand-test@example.invalid", "-C", str(repo), *args],
            env=env, capture_output=True, text=True, timeout=120)
        if result.returncode != 0:
            self.fail(f"git {' '.join(args)} failed:\n{result.stdout}\n{result.stderr}")
        return result.stdout.strip()

    def run_check(self):
        env = dict(os.environ)
        env["GIT_CONFIG_GLOBAL"] = str(self.gitconfig)
        return subprocess.run(
            ["bash", str(SCRIPT), "file://" + str(self.upstream), "main"],
            cwd=str(self.work), env=env, capture_output=True, text=True, timeout=120)

    def test_unrelated_upstream_change_exits_zero(self):
        (self.wt / "README.md").write_text("upstream docs update\n")
        self.git(self.wt, "add", ".")
        self.git(self.wt, "commit", "--quiet", "-m", "docs only")
        self.git(self.wt, "push", "--quiet", "origin", "main")
        result = self.run_check()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("No brand-sensitive upstream changes", result.stdout)

    def test_upstream_cmake_change_exits_one_and_names_only_that_commit(self):
        (self.wt / "README.md").write_text("upstream docs update\n")
        self.git(self.wt, "add", ".")
        self.git(self.wt, "commit", "--quiet", "-m", "docs only")
        (self.wt / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.16)\nproject(upstream_xiaozhi)\n")
        self.git(self.wt, "add", ".")
        self.git(self.wt, "commit", "--quiet", "-m", "upstream cmake change")
        self.git(self.wt, "push", "--quiet", "origin", "main")
        result = self.run_check()
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("BRAND-SENSITIVE UPSTREAM CHANGES FOUND", result.stdout)
        self.assertIn("upstream cmake change", result.stdout)
        self.assertNotIn("docs only", result.stdout)
        self.assertIn("brand_audit.py", result.stdout)


if __name__ == "__main__":
    unittest.main()
