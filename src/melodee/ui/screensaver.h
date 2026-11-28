// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <juce_graphics/juce_graphics.h>
#include <cstdint>

namespace melodee::ui {

// Settings > SCREEN's Screensaver (shell/screensaver.h), screen thread only: the name hdslaveOS (hdslaveOS: not
// Melodee's logo, ui/screensaver.cpp), on the palette's background, bouncing off the screen's edges in a track colour that changes at each
// bounce. Each start places it somewhere new.
class Screensaver {
public:
    // Whether it covers the screen at `now` (shell/screensaver.h due()); it starts over from a new place each time.
    bool shows(uint8_t choice, uint32_t now, uint32_t touched, bool busy);
    void draw(juce::Graphics& g, uint32_t now);

private:
    bool on = false, loaded = false;
    uint32_t began = 0;
    int x0 = 0, y0 = 0;
    juce::Image logo, tinted; // the logo scaled, and in the colour it shows
    uint32_t tinted_argb = 0, ground_argb = 0;
    float darkest = 0, lightest = 255; // the logo's own range of brightness

    void load();
    void tint(juce::Colour colour, juce::Colour ground, bool light_ground);
};

} // namespace melodee::ui
