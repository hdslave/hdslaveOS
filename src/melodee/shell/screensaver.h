// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

// Settings > SCREEN's Screensaver: once the controls have rested for the minutes picked while the sequencer is stopped
// and nothing records or renders, the name hdslaveOS (Melodee's logo upstream) drifts over the screen and bounces off its edges, a new colour at
// each bounce, as DVD players did, and the backlight dims. Any control brings the page back and still does its job.
// The name is drawn by ui/screensaver.cpp; where it is and when it shows is worked out here, without JUCE, so the host
// tests can check it.
namespace melodee::screensaver {

constexpr std::array<uint8_t, 6> minutes {{0, 1, 2, 5, 10, 30}}; // Settings' choices, 0 Off
constexpr int64_t speed_x = 34, speed_y = 26;                     // px a second, so milli-px a millisecond

// Whether it shows: a choice of minutes (not Off) since the last control or MIDI note, and nothing busy (the
// sequencer playing, a sample take armed or recording, a render running). `touched` is stamped on the control and
// MIDI threads, so it can be a little after the frame's `now`: that wraps to a huge wait, and counts as no wait at
// all (as does anything past 24 days).
inline bool due(uint8_t choice, uint32_t now, uint32_t touched, bool busy) {
    uint32_t wait = minutes[choice % minutes.size()], rested = now - touched;
    return wait && !busy && rested >= wait * 60000u && rested < 0x80000000u;
}

// While it shows, the backlight goes down to its second step (of the MPC's six, 1 the lowest), unless it is lower
// already; `level` is the step it is at otherwise, -1 when not known.
constexpr long dim_step = 2;
inline long backlight(long level, long maximum, bool shows) {
    const long dim = std::min(dim_step, maximum);
    return shows && level > dim ? dim : level;
}

struct Place { int x = 0, y = 0; unsigned bounces = 0; };

// One axis: `start` px into a `room` px run (screen less logo), `speed` px a second, `ms` in; back and forth.
inline int bounce(int start, int room, int64_t speed, uint32_t ms, unsigned& bounces) {
    if (room <= 0) return 0;
    const int64_t span = int64_t(room) * 1000, travel = int64_t(start % room) * 1000 + speed * int64_t(ms);
    const int64_t laps = travel / span, along = travel % span;
    bounces += unsigned(laps);
    return int((laps & 1 ? span - along : along) / 1000);
}

// Where a w x h logo sits on a screen_w x screen_h screen `ms` after the saver began at (x0, y0), going right and
// down at first, and how many edges it has hit on the way (the colour it shows).
inline Place place(uint32_t ms, int w, int h, int screen_w, int screen_h, int x0, int y0) {
    Place p;
    p.x = bounce(x0, screen_w - w, speed_x, ms, p.bounces);
    p.y = bounce(y0, screen_h - h, speed_y, ms, p.bounces);
    return p;
}

} // namespace melodee::screensaver
