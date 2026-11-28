// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <juce_graphics/juce_graphics.h>
#include "melodee/ui/palettes.h"
#include "melodee/ui/shades.h"
#include <array>
#include <cstdint>

// Melodee's design tokens at device size (the boards are drawn at 2x: halve
// them). The palette in use (HDSLAVE unless Settings > SCREEN > Palette picks
// another, ui/palettes.h), one colour per track, the corners it draws, and
// the four fixed zones of every screen.
namespace melodee::ui {

inline juce::Colour hex(uint32_t rgb, float alpha = 1) { return juce::Colour(0xff000000u | rgb).withAlpha(alpha); }

// The palette in use: use_palette() sets these (the screen thread, before each
// frame); they start as the default palette's (HDSLAVE).
namespace colour {
inline juce::Colour bg, panel, surf, raised, quiet, line, text, sec, mid, dim,
    rec, amber,
    play, // the transport playing (the place bar), apart from any track's colour
    ice;  // freeze plan F2c: a print, a frozen track or pattern
}
inline unsigned palette_in_use = ~0u;
inline void use_palette(unsigned n) {
    n %= unsigned(palettes.size());
    if (n == palette_in_use) return;
    palette_in_use = n;
    const auto& p = palettes[n];
    colour::bg = hex(p.bg); colour::panel = hex(p.panel); colour::surf = hex(p.surf); colour::raised = hex(p.raised);
    colour::quiet = hex(p.quiet); colour::line = hex(p.line); colour::text = hex(p.text); colour::sec = hex(p.sec);
    colour::mid = hex(p.mid); colour::dim = hex(p.dim); colour::rec = hex(p.rec); colour::amber = hex(p.amber);
    colour::play = hex(p.play); colour::ice = hex(p.ice);
}
namespace detail { inline const bool default_first = (use_palette(default_palette), true); }
inline const PaletteSpec& palette() { return palettes[palette_in_use % palettes.size()]; }

// Track colours 1-16 with their knob shades, and the Looper's L1-L4, as the palette in use has them (the pads
// keep ui/shades.h's own). slot_colour: the nth of the sixteen itself (a sequence's colour, a palette's swatch);
// track_colour: the one track n shows (issue 69: its place's, or the one picked for it, ui/shades.h track_slot).
inline juce::Colour slot_colour(unsigned slot) { return hex(palette().tracks[slot % 16][0]); }
inline juce::Colour slot_shade(unsigned slot, unsigned knob) { return hex(palette().tracks[slot % 16][knob % 3]); }
inline juce::Colour track_colour(unsigned track) { return slot_colour(track_slot(track)); }
inline juce::Colour knob_shade(unsigned track, unsigned knob) { return slot_shade(track_slot(track), knob); }
inline juce::Colour loop_colour(unsigned loop) { return hex(palette().loops[loop % 4][0]); }
inline juce::Colour loop_shade(unsigned loop, unsigned knob) { return hex(palette().loops[loop % 4][knob % 3]); }
inline juce::Colour sub_colour(unsigned sub) { return hex(palette().subs[sub % 8][0]); } // the mixer's submixes S1-S8
inline juce::Colour sub_shade(unsigned sub, unsigned knob) { return hex(palette().subs[sub % 8][knob % 3]); }
// A colour of the pads' own (ui/shades.h, a kit pad's COLOUR) as this palette shows it: the track colour it is, else as it is
// (a palette with no colour, LCD or MONO, shows any other in its ink).
inline juce::Colour screen_colour(uint32_t rgb) {
    for (unsigned t = 0; t < 16; ++t) if (track_shades[t][0] == rgb) return slot_colour(t);
    return palette().mono ? slot_colour(0) : hex(rgb);
}
// A corner's radius as the palette in use draws it: none on a square palette, which also drops soft shadows.
inline float corner(float radius) { return palette().square ? 0.0f : radius; }
inline bool soft_shadows() { return !palette().square; }

// Sequencer inks. The accented grid gives each group of four a stronger first column,
// and keeps notes on other lanes visible behind the edited lane.
struct SequenceGrid {
    juce::Colour step, group_start, past_lane, ghost, division, caret;
};
inline SequenceGrid sequence_grid() {
    if (palette().accented_grid)
        return {colour::raised, colour::raised.interpolatedWith(colour::line, 0.16f), colour::quiet,
                colour::mid, colour::line, colour::bg.interpolatedWith(colour::amber, 0.25f)};
    return {colour::quiet, colour::quiet, colour::bg, colour::line, colour::bg, colour::amber.withAlpha(0.22f)};
}

// Occupied sequence pads have an opaque base on an accented grid so the flare
// cannot make an empty pad appear brighter than a pattern that holds notes.
inline juce::Colour sequence_pad(juce::Colour ink, float amount) {
    return palette().accented_grid ? colour::surf.interpolatedWith(ink, amount) : ink.withAlpha(amount);
}

// Zones, top to bottom, at device size.
namespace zone {
constexpr int width = 320, height = 240, pad = 6;
constexpr int soft_y = 0, soft_h = 22;
constexpr int header_y = 22, header_h = 22;
constexpr int picture_y = 44, picture_h = 144;
constexpr int knobs_y = 188, knobs_h = 52;
}

// Type sizes in device pixels. Readable at arm's length on the 320 x 240
// panel: nothing is drawn under `floor`, and Paint::fit never shrinks below
// it. Where a page runs out of room it shows fewer things at a time instead.
namespace type {
constexpr float floor = 11;
constexpr float caps = 11, soft = 11, small = 11, body = 13, value = 22, title = 20, list = 15, bpm = 15;
constexpr float caps_tracking = 0.06f; // letter spacing of capitals, in em
}

// How strongly something sits behind an overlay: CSS opacity .35 over BG.
constexpr float behind_overlay = 0.65f;

} // namespace melodee::ui
