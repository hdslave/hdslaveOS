# SPDX-License-Identifier: AGPL-3.0-only
"""Melodee's plug-in hosts against fixture plug-ins, linked with the objects
make built: the CLAP host (tests/clap_host.cpp), the VST2 host
(tests/vst2_host.cpp) and JUCE's LV2 host as the rack uses it
(tests/lv2_host.cpp). Runs where those objects are: aarch64
Linux (./build.sh)."""
import itertools
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
INCLUDES = ["src", "core/src", "base/src", "build/deps/JUCE/modules", "build/deps/clap/include", "sdk/melodee-screen"]
DEFINES = ["-DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1", "-DJUCE_STANDALONE_APPLICATION=1", "-DJUCE_USE_CURL=0",
           "-DJUCE_WEB_BROWSER=0", "-DNDEBUG", "-DJUCE_PLUGINHOST_VST3=1", "-DJUCE_PLUGINHOST_LV2=1"]
FLAGS = ["-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", *["-I" + str(ROOT / i) for i in INCLUDES],
         "-isystem", str(ROOT / LV2_SDK / "lv2"), *DEFINES]  # the LV2 headers as JUCE ships them: their warnings are not ours


def build_clap(path):
    subprocess.run([os.environ.get("CXX", "g++"), *FLAGS, "-shared", "-fPIC",
                    str(ROOT / "tests/clap_fixture.cpp"), "-o", str(path)], check=True)


def build_lv2(bundle):
    bundle.mkdir()
    for name in ("manifest.ttl", "tone.ttl", "presets.ttl"):
        shutil.copy(ROOT / "tests/lv2_fixture" / name, bundle / name)
    # The LV2 atom macros, expanded in the fixture's own code, trigger -Wparentheses.
    subprocess.run([os.environ.get("CXX", "g++"), *FLAGS, "-Wno-parentheses", "-shared", "-fPIC",
                    str(ROOT / "tests/lv2_fixture/tone.cpp"), "-o", str(bundle / "tone.so")], check=True)


def compile_and_run(test, temp, plugin, host_source, extra_objects, expected):
    cxx = os.environ.get("CXX", "g++")
    host = Path(temp) / "host"
    subprocess.run([cxx, *FLAGS, str(ROOT / host_source), *[str(OBJ / o) for o in extra_objects + OBJECTS],
                    "-ldl", "-pthread", "-lrt", "-o", str(host)], check=True)
    result = subprocess.run([str(host), str(plugin)], capture_output=True, text=True, timeout=60,
                            env={**os.environ, "HOME": temp})
    test.assertEqual(result.returncode, 0, result.stderr)
    test.assertIn(expected, result.stdout)


class IdleGateTests(unittest.TestCase):
    def test_idle_gate(self):
        with tempfile.TemporaryDirectory() as temp:
            exe = Path(temp) / "idle_gate"
            subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(ROOT / "src"), str(ROOT / "tests/idle_gate.cpp"), "-o", str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("idle ok", result.stdout)


@unittest.skipUnless(platform.system() == "Linux" and platform.machine() == "aarch64"
                     and all((OBJ / o).is_file() for o in OBJECTS + ["melodee/plugin/clap_instance.o"]),
                     "needs make's aarch64 objects")
class PluginHostTests(unittest.TestCase):
    def test_idle_gate(self):
        with tempfile.TemporaryDirectory() as temp:
            host = Path(temp) / "idle_gate"
            subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(ROOT / "src"), str(ROOT / "tests/idle_gate.cpp"), "-o", str(host)], check=True)
            result = subprocess.run([str(host)], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("idle ok", result.stdout)

    def test_clap_host_against_fixture(self):
        with tempfile.TemporaryDirectory() as temp:
            plugin = Path(temp) / "Test.clap"
            build_clap(plugin)
            compile_and_run(self, temp, plugin, "tests/clap_host.cpp", ["melodee/plugin/clap_instance.o"], "clap ok")

    def test_user_preset_across_projects_and_sampler_shortcuts(self):
        subprocess.run(["make", "-s", "build/test-user-presets"], cwd=ROOT, check=True)
        with tempfile.TemporaryDirectory() as temp:
            card = Path(temp) / "card"
            plugin = card / "Melodee/Plugins/Test.clap"
            plugin.parent.mkdir(parents=True)
            build_clap(plugin)
            for mode in ("save", "load", "fxsave", "fxload"):
                subprocess.run([str(ROOT / "build/test-user-presets"), str(card), str(plugin), mode],
                               check=True, timeout=90, env={**os.environ, "MELODEE_PLUGIN_STAGE": str(Path(temp) / "stage")})

    def test_rack_level_behaviours(self):
        subprocess.run(["make", "-s", "build/test-user-presets"], cwd=ROOT, check=True)
        with tempfile.TemporaryDirectory() as temp:
            card = Path(temp) / "card"
            plugins = card / "Melodee/Plugins"
            plugins.mkdir(parents=True)
            clap, lv2 = plugins / "Test.clap", plugins / "Tone.lv2"
            build_clap(clap)
            build_lv2(lv2)
            for plugin, mode in ((clap, "failsave"), (clap, "mono"), (clap, "ringtail"), (clap, "dynamic-ring"),
                                 (clap, "native-ring"), (lv2, "lv2wake")):
                with self.subTest(mode=mode):
                    subprocess.run([str(ROOT / "build/test-user-presets"), str(card), str(plugin), mode],
                                   check=True, timeout=90, env={**os.environ, "MELODEE_PLUGIN_STAGE": str(Path(temp) / "stage")})

    def test_freeze_playback_matches_wavs_or_reports_insufficient_memory(self):
        subprocess.run(["make", "-s", "build/test-user-presets"], cwd=ROOT, check=True)
        with tempfile.TemporaryDirectory() as temp:
            card = Path(temp) / "card"
            plugin = card / "Melodee/Plugins/Test.clap"
            plugin.parent.mkdir(parents=True)
            build_clap(plugin)
            env = {**os.environ, "MELODEE_PLUGIN_STAGE": str(Path(temp) / "stage")}
            for rate, count, mib in itertools.product((44100, 48000, 96000), (1, 4, 8, 12, 16), (4, 8, 64)):
                with self.subTest(rate=rate, patterns=count, locked_mib=mib):
                    subprocess.run([str(ROOT / "build/test-user-presets"), str(card), str(plugin), "ring-freeze",
                                    str(count), str(rate), str(mib)], check=True, timeout=90, env=env)

    def run_rack_fixture(self, mode):
        subprocess.run(["make", "-s", "build/test-user-presets"], cwd=ROOT, check=True)
        with tempfile.TemporaryDirectory() as temp:
            card = Path(temp) / "card"
            plugin = card / "Melodee/Plugins/Test.clap"
            plugin.parent.mkdir(parents=True)
            build_clap(plugin)
            subprocess.run([str(ROOT / "build/test-user-presets"), str(card), str(plugin), mode],
                           check=True, timeout=90, env={**os.environ, "MELODEE_PLUGIN_STAGE": str(Path(temp) / "stage")})

    def test_knob_takeover_on_plugin_screen_changes(self):
        for mode in ("knob-scaled", "knob-pickup", "knob-instant"):
            with self.subTest(mode=mode):
                self.run_rack_fixture(mode)

    def test_deep_freeze_waits_for_its_pack_to_take_over(self):
        for mode in ("deep-freeze-live", "deep-freeze-replace"):
            with self.subTest(mode=mode):
                self.run_rack_fixture(mode)

    def test_deep_freeze_keeps_playback_when_tempo_changes_before_the_bar(self):
        for mode in ("deep-freeze-tempo", "deep-freeze-tempo-replace"):
            with self.subTest(mode=mode):
                self.run_rack_fixture(mode)

    def test_deep_freeze_keeps_playback_when_tempo_changes_during_unload_grace(self):
        self.run_rack_fixture("deep-freeze-unload-grace")

    def test_deep_freeze_keeps_playback_when_tempo_changes_after_unload(self):
        for mode in ("deep-freeze-active-tempo", "deep-freeze-active-lead", "deep-freeze-active-reload-fails"):
            with self.subTest(mode=mode):
                self.run_rack_fixture(mode)

    def test_deep_freeze_keeps_playback_when_tempo_returns_during_thaw(self):
        self.run_rack_fixture("deep-freeze-active-clock-reverts")

    def test_deep_freeze_remains_audible_when_returning_from_an_empty_pattern(self):
        for mode in ("deep-freeze-empty-new-tempo", "deep-freeze-empty-steady", "deep-freeze-empty-pattern-tempo"):
            with self.subTest(mode=mode):
                self.run_rack_fixture(mode)

    def test_deep_freeze_remains_audible_when_another_track_leads_the_handoff(self):
        for mode in ("deep-freeze-lead-playing-stale", "deep-freeze-lead-off-stale",
                     "deep-freeze-lead-playing-clock-mismatch", "deep-freeze-lead-off-compatible"):
            with self.subTest(mode=mode):
                self.run_rack_fixture(mode)

    def test_recording_cannot_change_a_pending_freeze_snapshot(self):
        self.run_rack_fixture("freeze-recording")

    def test_freeze_replacement_reuses_pins_and_preserves_selection_on_failure(self):
        self.run_rack_fixture("freeze-replace")

    def test_freeze_and_unfreeze_retry_when_the_audio_queue_is_full(self):
        self.run_rack_fixture("freeze-queue")

    def test_vst2_host_against_fixture(self):
        with tempfile.TemporaryDirectory() as temp:
            plugin = Path(temp) / "Test.so"
            subprocess.run([os.environ.get("CXX", "g++"), *FLAGS, "-shared", "-fPIC",
                            str(ROOT / "tests/vst2_fixture.cpp"), "-o", str(plugin)], check=True)
            compile_and_run(self, temp, plugin, "tests/vst2_host.cpp",
                            ["melodee/plugin/vst2_instance.o", "melodee/plugin/vst2_bridge_instance.o"], "vst2 ok")

    def test_screen_example(self):
        """sdk/melodee-screen/example builds with both entry points, and Melodee draws its page as Edit shows it."""
        example = ROOT / "sdk/melodee-screen/example"
        melodee = ROOT / "build/melodee"
        with tempfile.TemporaryDirectory() as temp:
            library = Path(temp) / "screen-example.so"
            subprocess.run([os.environ.get("CXX", "g++"), "-std=c++17", "-O2", "-fPIC", "-shared", "-fvisibility=hidden",
                            "-Wall", "-Wextra", "-Werror", "-I" + str(example.parent), "-I" + str(ROOT / "build/deps/clap/include"),
                            str(example / "screen_example.cpp"), "-o", str(library)], check=True)
            symbols = subprocess.run(["nm", "-D", "--defined-only", str(library)], capture_output=True, text=True, check=True).stdout
            self.assertIn("clap_entry", symbols)
            self.assertIn("VSTPluginMain", symbols)
            if not melodee.is_file():
                return
            clap = Path(temp) / "screen-example.clap"
            shutil.copy(library, clap)
            for plugin in (clap, library):
                out = Path(temp) / ("png-" + plugin.suffix[1:])
                result = subprocess.run([str(melodee), "--melodee-plugin-screen", str(plugin), str(out),
                                         "wheel:20", "plus", "wheel:-10", "push", "k3:40"],
                                        capture_output=True, text=True, timeout=60, env={**os.environ, "HOME": temp})
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("written=6", result.stdout)
                self.assertEqual(len(list(out.glob("*.png"))), 6)

                result = subprocess.run([str(melodee), "--melodee-plugin-screen", str(plugin), str(out),
                                         "strip", "png:chain", "push", "png:panel"],
                                        capture_output=True, text=True, timeout=60, env={**os.environ, "HOME": temp})
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("chain screen=1 area=1", result.stdout)
                self.assertIn("panel screen=1 area=0", result.stdout)

    def test_lv2_host_against_fixture(self):
        with tempfile.TemporaryDirectory() as temp:
            bundle = Path(temp) / "Tone.lv2"
            build_lv2(bundle)
            compile_and_run(self, temp, bundle, "tests/lv2_host.cpp", [], "lv2 ok")


if __name__ == "__main__":
    unittest.main()
