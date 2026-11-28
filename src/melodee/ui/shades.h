// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <array>
#include <atomic>
#include <cstdint>

// The colours Melodee gives its tracks and loops, as 0xRRGGBB: the screen's
// (ui/tokens.h) and the pads' (shell/pad_light.h). No JUCE here, so the
// pad lights build in the host tests too.
namespace melodee::ui {

// Track colours 1-16 with the three knob shades: knob 1 is the track's own
// colour, knobs 2 and 3 turn 28 degrees each way round the hue
// (.local/melodee-design/gen/lib16.py).
constexpr std::array<std::array<uint32_t, 3>, 16> track_shades {{
    {0xFF7A5C, 0xFF6F93, 0xFF9A3C}, {0xFFC145, 0xFF9F5A, 0xE2D94A}, {0x46D9B0, 0x4FC7E0, 0x8FDB6A},
    {0x9D8CFF, 0xC587F2, 0x6FA0FF}, {0x4FB8FF, 0x4FFFF4, 0x4F66FF}, {0xFF6FA8, 0xFF6FEB, 0xFF796F},
    {0xA6E04A, 0xE0D44A, 0x60E04A}, {0xD77BFF, 0x997BFF, 0xFF7BE9}, {0x2FD0D6, 0x2FD68E, 0x2F82D6},
    {0xE8D94A, 0xE88F4A, 0xADE84A}, {0xFF5C7A, 0xFF5CC6, 0xFF8A5C}, {0x5FD97A, 0x7DD95F, 0x5FD9B3},
    {0x6F8CFF, 0x6FCFFF, 0x956FFF}, {0xFF9A3C, 0xFF3F3C, 0xFFF53C}, {0xF06FD8, 0xCC6FF0, 0xF06F9C},
    {0xB8C2D6, 0xB8D0D6, 0xBCB8D6},
}};
// The sixteen by name, as Stage's COLOUR offers them to a kit pad (shell/pad_colour.h).
constexpr std::array<const char*, 16> track_colour_names {
    "Coral", "Amber", "Teal", "Lavender", "Sky", "Pink", "Lime", "Violet", "Cyan", "Yellow", "Rose", "Green", "Blue", "Orange", "Magenta", "Grey",
};
// The sixteen as the nearest of Ableton Live's clip and track colours (its palette's index), for the Live Set export
// (shell/live_export.h): Coral as Live's salmon, Amber its orange, and so on.
constexpr std::array<uint8_t, 16> track_live_colours {28, 1, 20, 10, 8, 12, 4, 11, 21, 17, 0, 18, 9, 15, 26, 37};
// Which of the sixteen each track shows (issue 69, model/project.h track_colour_slot): its place's (track n the nth)
// until one is picked for it on the mixer's MENU. The shell sets them from the project each frame
// (shell/track_colours.h); the screen, the pad lights and the exports read them through track_slot(). Any thread.
struct TrackSlots {
    std::array<std::atomic<uint8_t>, 16> at;
    TrackSlots() { for (unsigned n = 0; n < 16; ++n) at[n].store(uint8_t(n), std::memory_order_relaxed); }
};
inline TrackSlots& track_slot_table() { static TrackSlots table; return table; }
inline unsigned track_slot(unsigned track) { return track_slot_table().at[track % 16].load(std::memory_order_relaxed) % 16u; }
inline void set_track_slot(unsigned track, unsigned slot) { track_slot_table().at[track % 16].store(uint8_t(slot % 16), std::memory_order_relaxed); }

// The Looper's tracks L1-L4 (docs/LOOPER.md): colours of their own, apart from the sixteen, shaded the same way.
constexpr std::array<std::array<uint32_t, 3>, 4> loop_shades {{
    {0x6FD3FF, 0x6FA6FF, 0x6FFFE5}, {0xFFA36F, 0xFF7A6F, 0xFFD06F}, {0xC09CFF, 0x9C9EFF, 0xE69CFF}, {0x7DEBA6, 0x7DEBD9, 0xA4EB7D},
}};

// The mixer's submixes S1-S8 (docs/MIXER_PLAN.md, decision 6): colours of their own, softer than the sixteen and
// the loops' (OKLCH lightness 0.80, chroma 0.085, hues 45 degrees apart from 20), shaded the same way.
constexpr std::array<std::array<uint32_t, 3>, 8> sub_shades {{
    {0xF0A8A7, 0xECAE8E, 0xE9A7C3}, {0xE4B383, 0xD0BD7E, 0xEFAB97}, {0xC1C382, 0xA3CB96, 0xD9B97E}, {0x91CEA6, 0x7BD0C3, 0xAFC88D},
    {0x77CFD3, 0x81C9E9, 0x82D0B7}, {0x8FC4F1, 0xABBBF5, 0x7BCCE1}, {0xBCB5F1, 0xD5ADE2, 0xA0BEF5}, {0xE1A9D4, 0xEDA7B8, 0xCCB0E9},
}};

} // namespace melodee::ui
