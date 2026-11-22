# SPDX-License-Identifier: AGPL-3.0-only
"""Melodee's plug-in hosts against fixture plug-ins, linked with the objects
make built: the CLAP host (tests/clap_host.cpp) and JUCE's LV2 host as the
rack uses it (tests/lv2_host.cpp). Runs where those objects are: aarch64
Linux (./build.sh)."""
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
OBJ = ROOT / "build/obj"
OBJECTS = ["melodee/plugin/plugin_files.o", "juce_core.o", "juce_core_CompilationTime.o", "juce_core_zlib.o",
           "juce_events.o", "juce_audio_basics.o", "juce_audio_processors_headless.o",
           "juce_audio_processors_headless_lv2_libs.o"]
LV2_SDK = "build/deps/JUCE/modules/juce_audio_processors_headless/format_types/LV2_SDK"
INCLUDES = ["src", "core/src", "base/src", "build/deps/JUCE/modules", "build/deps/clap/include"]
DEFINES = ["-DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1", "-DJUCE_STANDALONE_APPLICATION=1", "-DJUCE_USE_CURL=0",
           "-DJUCE_WEB_BROWSER=0", "-DNDEBUG", "-DJUCE_PLUGINHOST_VST3=1", "-DJUCE_PLUGINHOST_LV2=1"]
FLAGS = ["-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", *["-I" + str(ROOT / i) for i in INCLUDES],
         "-isystem", str(ROOT / LV2_SDK / "lv2"), *DEFINES]  # the LV2 headers as JUCE ships them: their warnings are not ours


def compile_and_run(test, temp, plugin, host_source, extra_objects, expected):
    cxx = os.environ.get("CXX", "g++")
    host = Path(temp) / "host"
    subprocess.run([cxx, *FLAGS, str(ROOT / host_source), *[str(OBJ / o) for o in extra_objects + OBJECTS],
                    "-ldl", "-pthread", "-lrt", "-o", str(host)], check=True)
    result = subprocess.run([str(host), str(plugin)], capture_output=True, text=True, timeout=60,
                            env={**os.environ, "HOME": temp})
    test.assertEqual(result.returncode, 0, result.stderr)
    test.assertIn(expected, result.stdout)


@unittest.skipUnless(platform.system() == "Linux" and platform.machine() == "aarch64"
                     and all((OBJ / o).is_file() for o in OBJECTS + ["melodee/plugin/clap_instance.o"]),
                     "needs make's aarch64 objects")
class PluginHostTests(unittest.TestCase):
    def test_clap_host_against_fixture(self):
        with tempfile.TemporaryDirectory() as temp:
            plugin = Path(temp) / "Test.clap"
            subprocess.run([os.environ.get("CXX", "g++"), *FLAGS, "-shared", "-fPIC",
                            str(ROOT / "tests/clap_fixture.cpp"), "-o", str(plugin)], check=True)
            compile_and_run(self, temp, plugin, "tests/clap_host.cpp", ["melodee/plugin/clap_instance.o"], "clap ok")

    @unittest.skipUnless((ROOT / "build/melodee").is_file(), "needs make's build/melodee")
    def test_plugin_screen_through_melodee(self):
        """Melodee's plug-in screen end to end: the fixture opened through the rack on a PLUGIN track, the hands'
        controls through the shell to it, its pictures through the snapshot onto the Edit and Stage pages."""
        with tempfile.TemporaryDirectory() as temp:
            plugin = Path(temp) / "Test.clap"
            subprocess.run([os.environ.get("CXX", "g++"), *FLAGS, "-shared", "-fPIC",
                            str(ROOT / "tests/clap_fixture.cpp"), "-o", str(plugin)], check=True)
            # Then, once it sleeps (silent, no key held, stopped: plugin/idle_gate.h; 1200 ms here is about 0.64 s of
            # audio, more than the half second it waits): a pot turned on its page, a Push (request_process) and a Plus
            # (request_flush) each reach its audio side, which wakes for them.
            steps = ["png:first", "wheel:1", "png:wheel", "push", "png:push", "plus", "png:plus", "k1:127", "png:k1",
                     "stage", "png:stage", "minus", "png:minus", "shift", "png:shift", "shift",
                     "push", "wait:1200", "png:asleep", "k1:0", "wait:100", "png:level",
                     "wait:1200", "png:asleep2", "push", "wait:100", "png:pushed",
                     "wait:1200", "png:asleep3", "plus", "wait:100", "png:flushed"]
            result = subprocess.run([str(ROOT / "build/melodee"), "--melodee-plugin-screen", str(plugin), str(Path(temp) / "out"),
                                     *steps], capture_output=True, text=True, timeout=120,
                                    env={**os.environ, "HOME": temp, "MELODEE_PLUGIN_STAGE": str(Path(temp) / "stage")})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            said = {line.split()[1]: dict(kv.split("=") for kv in line.split()[2:])
                    for line in result.stdout.splitlines() if line.startswith("MELODEE_PLUGIN_SCREEN ")}
            # Level 0.5: red 128; the cursor's row in green; the inputs taken in blue; K1-K3 the rows it names.
            # Level 0 (K1 at the bottom, once Push put Level on it): red 0.
            want = {"first": ("0,1,-1", "0xff800000"), "wheel": ("1,0,-1", "0xff80ff01"), "push": ("0,1,-1", "0xff800002"),
                    "plus": ("1,0,-1", "0xff80ff03"), "stage": ("1,0,-1", "0xff80ff03"), "minus": ("0,1,-1", "0xff800004"),
                "shift": ("0,1,-1", "0xff800004"), "asleep": ("0,1,-1", "0xff800005"), "level": ("0,1,-1", "0xff000005"),
                "pushed": ("0,1,-1", "0xff000006"), "flushed": ("1,0,-1", "0xff00ff07")}
            for name, (knobs, pixel) in want.items():
                self.assertEqual(said[name]["screen"], "1", name)
                self.assertEqual(said[name]["knobs"], knobs, name)
                self.assertEqual(said[name]["pixel"], pixel, name)
                self.assertEqual(said[name]["page"], pixel, name)  # the page shows the picture under the soft keys
                self.assertEqual(said[name]["low"], pixel, name)   # and nothing over its foot, Shift held or not
            self.assertEqual(said["first"]["bar"], "0")  # K1 not moved yet
            self.assertEqual(said["k1"]["bar"], "320")   # K1 at the top, as the plug-in was told
            self.assertGreater(int(said["minus"]["serial"]), int(said["first"]["serial"]))
            # Asleep before each, awake after; its third row 16 pixels for each Push, Minus and Plus its audio side took.
            for name in ("asleep", "asleep2", "asleep3"):
                self.assertEqual(said[name]["asleep"], "1", name)
            for name in ("level", "pushed", "flushed"):
                self.assertEqual(said[name]["asleep"], "0", name)
            self.assertEqual(said["asleep"]["bar2"], "64")   # push, plus, minus, push
            self.assertEqual(said["asleep2"]["bar2"], "64")
            self.assertEqual(said["pushed"]["bar2"], "80")
            self.assertEqual(said["flushed"]["bar2"], "96")
            for name in want:
                self.assertTrue((Path(temp) / "out" / (name + ".png")).is_file())

    def test_lv2_host_against_fixture(self):
        with tempfile.TemporaryDirectory() as temp:
            bundle = Path(temp) / "Tone.lv2"
            bundle.mkdir()
            for name in ("manifest.ttl", "tone.ttl", "presets.ttl"):
                shutil.copy(ROOT / "tests/lv2_fixture" / name, bundle / name)
            # -Wno-parentheses: LV2's atom macros, expanded in the fixture's own code.
            subprocess.run([os.environ.get("CXX", "g++"), *FLAGS, "-Wno-parentheses", "-shared", "-fPIC",
                            str(ROOT / "tests/lv2_fixture/tone.cpp"), "-o", str(bundle / "tone.so")], check=True)
            compile_and_run(self, temp, bundle, "tests/lv2_host.cpp", [], "lv2 ok")


if __name__ == "__main__":
    unittest.main()
