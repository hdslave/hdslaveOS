// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// When a plug-in may sleep (plugin/idle_gate.h): held keys, the sustain pedal and the controllers that release
// them (a stranded pedal must not keep a silent plug-in awake for ever), and an effect's reported tail holding
// it awake past the plain quiet span. Built and run by tests/test_plugin_hosts.py; no JUCE, no objects.
#include "melodee/plugin/idle_gate.h"

#include <cstdio>

using namespace melodee;

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

namespace {
constexpr double rate = 48000;
float zeros[128] {};

// `seconds` of silence heard, in blocks.
void quiet(IdleGate& gate, double seconds) {
    for (double done = 0; done < seconds; done += 128.0 / rate) gate.heard(zeros, zeros, 128);
}
void cc(IdleGate& gate, uint8_t channel, uint8_t controller, uint8_t value) {
    gate.midi(uint8_t(0xb0 | channel), controller, value);
}
} // namespace

int main() {
    // The pedal down keeps an instrument awake; each of All Sound Off, Reset All Controllers and All Notes Off
    // lets it sleep once it has been quiet long enough.
    for (uint8_t release : {uint8_t(120), uint8_t(121), uint8_t(123)}) {
        IdleGate gate;
        gate.reset(true, rate);
        gate.midi(0x90, 60, 100);
        gate.midi(0x80, 60, 0);
        cc(gate, 0, 64, 127);
        quiet(gate, 1.0);
        CHECK(!gate.asleep(false, false, nullptr, nullptr, 0)); // the pedal is still down
        cc(gate, 0, release, 0);
        CHECK(gate.keys_held() == 0);
        CHECK(gate.asleep(false, false, nullptr, nullptr, 0));
    }
    { // The pedal on another channel is not this one's release.
        IdleGate gate;
        gate.reset(true, rate);
        cc(gate, 1, 64, 127);
        cc(gate, 0, 121, 0);
        quiet(gate, 1.0);
        CHECK(!gate.asleep(false, false, nullptr, nullptr, 0));
    }
    { // A key still held, the song playing, or work waiting: awake however quiet.
        IdleGate gate;
        gate.reset(true, rate);
        gate.midi(0x90, 60, 100);
        quiet(gate, 1.0);
        CHECK(!gate.asleep(false, false, nullptr, nullptr, 0));
        gate.midi(0x80, 60, 0);
        CHECK(gate.asleep(false, false, nullptr, nullptr, 0));
        CHECK(!gate.asleep(false, true, nullptr, nullptr, 0));
        CHECK(!gate.asleep(true, false, nullptr, nullptr, 0));
    }
    { // An effect with a two-second tail stays awake for it after its last sound, then sleeps.
        IdleGate gate;
        gate.reset(false, rate, 2.0);
        float loud[128];
        for (auto& s : loud) s = 0.5f;
        gate.heard(loud, loud, 128);
        quiet(gate, 1.0);
        CHECK(!gate.asleep(false, false, zeros, zeros, 128)); // past the plain half second, inside the tail
        quiet(gate, 1.5);
        CHECK(gate.asleep(false, false, zeros, zeros, 128));
        CHECK(!gate.asleep(false, false, loud, loud, 128)); // sound at its input wakes it
    }
    { // The same effect told no tail sleeps after the plain quiet span, as before.
        IdleGate gate;
        gate.reset(false, rate);
        quiet(gate, 1.0);
        CHECK(gate.asleep(false, false, zeros, zeros, 128));
    }
    { // A tail told again while it rings: the span follows it, longer or shorter, the quiet counted standing.
        IdleGate gate;
        gate.reset(false, rate);
        float loud[128];
        for (auto& s : loud) s = 0.5f;
        gate.heard(loud, loud, 128);
        quiet(gate, 0.4);
        gate.setTail(2.0);
        quiet(gate, 1.0);
        CHECK(!gate.asleep(false, false, zeros, zeros, 128)); // past the plain half second, inside the new tail
        quiet(gate, 1.5);
        CHECK(gate.asleep(false, false, zeros, zeros, 128));
        gate.heard(loud, loud, 128);
        quiet(gate, 1.0);
        gate.setTail(0); // told shorter again: the second already counted is past the plain span
        CHECK(gate.asleep(false, false, zeros, zeros, 128));
    }
    { // heardPeak(passing peak) decides as heard(samples) does, block by block, the -100 dB edge included.
        IdleGate bySamples, byPeak;
        bySamples.reset(false, rate, 1.0);
        byPeak.reset(false, rate, 1.0);
        const float peaks[] {0.5f, 0.0f, 1e-5f, -1e-5f, 2e-5f, 0.0f, -0.25f, 9e-6f, 0.0f};
        for (int round = 0; round < 80; ++round) {
            for (float peak : peaks) {
                float left[128] {}, right[128] {};
                (round % 2 ? right : left)[37] = peak;
                float taken = 0; // the rack's passing peak: the most either channel reaches, either way up
                for (int f = 0; f < 128; ++f) {
                    taken = std::max(taken, std::fabs(left[f]));
                    taken = std::max(taken, std::fabs(right[f]));
                }
                bySamples.heard(left, right, 128);
                byPeak.heardPeak(taken, 128);
                CHECK(bySamples.asleep(false, false, zeros, zeros, 128) == byPeak.asleep(false, false, zeros, zeros, 128));
                CHECK(bySamples.keys_held() == byPeak.keys_held());
            }
        }
        // Both do sleep once only the edge itself follows (at -100 dB exactly is quiet, never a wake), block by
        // block alike, and a later loud block is counted from there by both.
        float edge[128] {};
        edge[5] = IdleGate::quiet_level;
        for (int b = 0; b < 600; ++b) {
            bySamples.heard(edge, zeros, 128);
            byPeak.heardPeak(IdleGate::quiet_level, 128);
            CHECK(bySamples.asleep(false, false, zeros, zeros, 128) == byPeak.asleep(false, false, zeros, zeros, 128));
        }
        CHECK(bySamples.asleep(false, false, zeros, zeros, 128) && byPeak.asleep(false, false, zeros, zeros, 128));
        float loud[128];
        for (auto& s : loud) s = 0.5f;
        bySamples.heard(loud, loud, 128);
        byPeak.heardPeak(0.5f, 128);
        CHECK(!bySamples.asleep(false, false, zeros, zeros, 128) && !byPeak.asleep(false, false, zeros, zeros, 128));
    }
    if (failures) return 1;
    std::puts("idle ok");
    return 0;
}
