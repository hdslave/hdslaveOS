// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <cstdint>

// A loaded plug-in as the audio thread sees it. The rack (plugin/plugin_rack.h)
// makes them on its own thread and hands them to the engine host, which only
// ever holds this interface: nothing of JUCE's plug-in hosting reaches the
// engine host or its tests. Every call below is on the audio thread.
namespace melodee {

class PluginRunner {
public:
    virtual ~PluginRunner() = default;
    // A MIDI message for the next render() (a note, a controller, the bend),
    // heard at that render's first frame.
    virtual void midi(uint8_t status, uint8_t data1, uint8_t data2) = 0;
    virtual void silence() = 0; // every note off and the sound cut, as on Stop
    // An instrument adds its output into left/right; an effect works on them
    // in place. `frames` is at most the engine host's block.
    virtual void render(float* left, float* right, unsigned frames) = 0;
    // Roadmap P4, plug-in delay compensation: how many device frames its output lags what goes in (an effect) or
    // the notes it is sent (an instrument): the latency the plug-in reports, at the device's rate, and its rate
    // converter's. The engine host delays the other paths to match.
    virtual unsigned latency() const { return 0; }
    // A side-chain key (melodee-hdslave): an instrument with a second, non-main audio output (a drum machine's "SC
    // Key", the kick alone) offers it here. has_key(): it has one the engine host can hear. key(), right after a
    // render(): adds what that port played in it, the same `frames`, into left/right (a mono port on both sides);
    // silence when the render played nothing. Its port is as late as the main output (latency()).
    virtual bool has_key() const { return false; }
    virtual void key(float* /*left*/, float* /*right*/, unsigned /*frames*/) {}
};

} // namespace melodee
