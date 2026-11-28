// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <juce_graphics/juce_graphics.h>
#include "melodee/ui/boot_film.h"
#include <cstdint>

namespace melodee::ui {

// The boot animation, screen thread only (melodee-hdslave: the boot film, ui/boot_film.h). The film's frames
// play by one clock, the time since the first draw: a frame comes from that time, never from a count of frames
// drawn, so a late frame is passed over and the film keeps its length. The last frame stays, then the whole
// screen fades out over the app. A touch (skip()) cuts it short: the frame as it stands fades out.
// Each frame is decoded when it is first due (one 320 x 240 JPEG, from memory: nothing is read from storage);
// only the one on screen is kept.
class BootAnimation {
public:
    // The app fades in beneath the film's final black frame.
    static constexpr uint32_t out_ms = 200;
    static constexpr uint32_t out_at = boot_film::length_ms + boot_film::hold_ms;

    // `now` is any millisecond clock; the first call starts the animation.
    // True while the animation covers the whole screen, so the app need not
    // draw underneath; false once it fades out over the app, or has ended.
    bool covers(uint32_t now);
    bool finished(uint32_t now);
    void skip(uint32_t now);
    // Draws the frame for `now` over whatever the screen holds.
    void draw(juce::Graphics& g, uint32_t now);

private:
    bool started = false, loaded = false, broken = false;
    uint32_t start = 0, fade_from = out_at; // the elapsed time the fade out starts at
    juce::Image frame;                      // the film's frame `shown`
    int shown = -1;

    uint32_t elapsed(uint32_t now);
    void load();
    bool show(unsigned index);
};

} // namespace melodee::ui
