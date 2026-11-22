// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Melodee's VST2 host (plugin/vst2_instance.h) against tests/vst2_fixture.cpp: opening it, its parameters and output,
// and its own screen (sdk/melodee-screen/melodee_screen.h): the two-step handshake, drawing, inputs, redraws, the
// block it asks for, and K1-K3. Built and run by tests/test_plugin_hosts.py on aarch64 Linux, with Melodee's objects.
#include "melodee/plugin/vst2_instance.h"
#include <melodee_screen.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>

using namespace melodee;

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

namespace {
constexpr int block = 128;
constexpr double rate = 48000;

float run(juce::AudioPluginInstance& p) {
    juce::AudioBuffer<float> buffer(2, block);
    buffer.clear();
    juce::MidiBuffer midi;
    p.processBlock(buffer, midi);
    return buffer.getMagnitude(0, 0, block);
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: vst2_host Test.so\n"); return 2; }
    juce::File file {juce::String(argv[1])};
    void* fixture = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL); // held, so its answer can be read once the host lets go
    auto hostScreen = reinterpret_cast<int (*)()>(dlsym(fixture, "fixture_host_screen"));
    CHECK(fixture && hostScreen);
    if (!hostScreen) return 1;
    juce::String error;
    {
        auto tone = Vst2Instance::load(file, rate, block, error);
        CHECK(tone != nullptr);
        if (!tone) { std::fprintf(stderr, "%s\n", error.toRawUTF8()); return 1; }
        CHECK(tone->getName() == "Test Tone" && tone->getParameters().size() == 2);
        CHECK(hostScreen() == 1); // the host says it takes a screen
        tone->prepareToPlay(rate, block);
        CHECK(std::abs(run(*tone) - 0.5f) < 1e-6f); // its Level

        CHECK(tone->hasScreen());
        std::vector<uint32_t> pixels(320 * 218, 0x123456u);
        melodee_screen_frame_t frame {};
        frame.pixels = pixels.data();
        frame.width = 320; frame.height = 218; frame.stride = 320;
        frame.redraw_all = 1;
        frame.colour = 0x40c0ffu;
        bool changed = false, used = false;
        CHECK(tone->screenDraw(frame, changed) && changed);
        CHECK(pixels[0] == 0x40c0ffu && pixels[150] == 0x40c0ffu && pixels[170] == 0x101010u && pixels[320 * 100] == 0x101010u);
        frame.redraw_all = 0;
        CHECK(tone->screenDraw(frame, changed) && !changed);
        CHECK(!tone->takeRedraw() && !tone->wantsProcess());
        melodee_screen_input_t wheel {MELODEE_SCREEN_WHEEL, 10, 0};
        CHECK(tone->screenInput(wheel, used) && used);
        CHECK(tone->takeRedraw() && !tone->takeRedraw()); // asked once
        CHECK(tone->wantsProcess());                      // and for a block, though silent
        CHECK(std::abs(run(*tone) - 0.6f) < 1e-5f);       // the new Level, heard
        CHECK(!tone->wantsProcess());
        CHECK(std::abs(tone->getParameters()[0]->getValue() - 0.6f) < 1e-5f);
        CHECK(tone->screenDraw(frame, changed) && changed && pixels[190] == 0x40c0ffu);
        melodee_screen_input_t step {MELODEE_SCREEN_STEP, -1, 0};
        CHECK(tone->screenInput(step, used) && !used);
        std::array<int, 3> controls {};
        CHECK(tone->screenControls(controls) && controls[0] == 0 && controls[1] == 1 && controls[2] == -1);
        tone->releaseResources();
    }
    { // A plug-in that says it has no screen, though it answers the vendor call: none taken.
        setenv("FIXTURE_NO_SCREEN", "1", 1);
        auto tone = Vst2Instance::load(file, rate, block, error);
        CHECK(tone != nullptr && !tone->hasScreen());
        bool changed = true;
        melodee_screen_frame_t frame {};
        CHECK(tone && !tone->screenDraw(frame, changed));
        unsetenv("FIXTURE_NO_SCREEN");
    }
    if (failures) return 1;
    std::printf("vst2 ok\n");
    return 0;
}
