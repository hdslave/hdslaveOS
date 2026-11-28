# SPDX-License-Identifier: AGPL-3.0-only
"""hdslaveOS: find moved files. The finder (src/melodee/shell/file_finder.cpp) and its thread (moved_files.cpp) on a
card made in a temporary folder, built for the host (tests/file_finder.cpp)."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class FileFinderTests(unittest.TestCase):
    def test_steps_index_plugins_and_thread(self):
        with tempfile.TemporaryDirectory() as temp:
            binary = Path(temp) / "file_finder"
            subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
                            "-I" + str(ROOT / "src"), "-I" + str(ROOT / "core/src"), str(ROOT / "tests/file_finder.cpp"),
                            str(ROOT / "src/melodee/shell/file_finder.cpp"), str(ROOT / "src/melodee/shell/moved_files.cpp"),
                            "-o", str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=120, env={**os.environ, "TMPDIR": temp})
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("file finder ok", result.stdout)


if __name__ == "__main__":
    unittest.main()
