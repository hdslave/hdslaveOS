# SPDX-License-Identifier: AGPL-3.0-only
"""hdslaveOS: find moved files through the whole shell (tests/find_moved.cpp): projects opened from a card made in a
temporary folder, with the question, its answers and the sheets; Settings' Always and Off; a plug-in moved into
Melodee/Plugins/hdslave/ and one gone. Linked with the objects make built: runs where they are (./build.sh)."""
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from test_plugin_hosts import build_clap  # noqa: E402

BINARY = ROOT / "build/test-find-moved"


@unittest.skipUnless(platform.system() == "Linux" and platform.machine() == "aarch64"
                     and (ROOT / "build/obj/melodee/shell/find_moved.o").is_file(), "needs make's aarch64 objects")
class FindMovedTests(unittest.TestCase):
    def run_mode(self, mode, plugin=None):
        subprocess.run(["make", "-s", str(BINARY.relative_to(ROOT))], cwd=ROOT, check=True)
        with tempfile.TemporaryDirectory() as temp:
            args = [str(BINARY), temp, mode]
            if plugin:
                built = Path(temp) / "built" / "Test.clap"
                built.parent.mkdir()
                build_clap(built)
                args.append(str(built))
            result = subprocess.run(args, capture_output=True, text=True, timeout=300,
                                    env={**os.environ, "HOME": temp, "MELODEE_PLUGIN_STAGE": str(Path(temp) / "stage")})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("find moved ok", result.stdout)

    def test_question_answers_sheets_index_and_load_kit(self):
        self.run_mode("session")

    def test_always_searches_without_asking(self):
        self.run_mode("always")

    def test_off_asks_nothing_and_says_what_is_missing(self):
        self.run_mode("off")

    def test_plugin_moved_into_a_folder_and_one_gone(self):
        self.run_mode("plugin", plugin=True)


if __name__ == "__main__":
    unittest.main()
