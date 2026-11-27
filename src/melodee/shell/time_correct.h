// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/pattern.h"
#include <algorithm>
#include <array>
#include <cstdint>

// Time Correct (pages/time_correct_page.cpp): the rules of its B2 GROOVE layer and of its picture, pure, for the
// page, the shell, the pad lights and the tests. B2 held puts one track's patterns on the pads and the screen, a
// bank at a time; a pad takes the groove from that pattern, the same pad again takes it away.
namespace melodee {

// B2 pressed: the track and bank shown. The groove's own pattern when there is one, else the pattern in hand's bank
// of the selected track.
struct GrooveView {
    uint8_t track = 0, bank = 0;
};
inline GrooveView groove_view(uint16_t from, unsigned track, unsigned here) {
    if (from) return {uint8_t(groove_track(from) % track_count), uint8_t(groove_pattern(from) % pattern_count / pattern_pads)};
    return {uint8_t(track % track_count), uint8_t(here % pattern_count / pattern_pads)};
}

// A pad with B2 held, on `pattern` of `view` (the track shown): Take its groove, Clear the groove (it was this
// pattern's), or nothing: Here (the pattern in hand itself) or Empty (no notes to take a groove from).
enum class GroovePick : uint8_t { Take, Clear, Here, Empty };
inline GroovePick groove_pick(uint16_t from, unsigned track, unsigned here, unsigned view, unsigned pattern, bool notes) {
    if (from && from == groove_source(view % track_count, pattern % pattern_count)) return GroovePick::Clear;
    if (view % track_count == track % track_count && pattern % pattern_count == here % pattern_count) return GroovePick::Here;
    return notes ? GroovePick::Take : GroovePick::Empty;
}

// Minus / Plus with B2 held: the next track that holds notes (or the selected one), stopping at the ends.
inline unsigned groove_track_step(const std::array<bool, track_count>& notes, unsigned track, unsigned at, int way) {
    if (!way) return at % track_count;
    for (int t = int(at % track_count) + (way > 0 ? 1 : -1); t >= 0 && t < int(track_count); t += way > 0 ? 1 : -1)
        if (notes[unsigned(t)] || unsigned(t) == track % track_count) return unsigned(t);
    return at % track_count;
}

// Where each note of a pattern plays with its feel (feel_pattern: strength, swing, the groove, humanize), as the
// page draws it: in ticks from the start, not wrapped round the loop (a note pulled early off the first step stays
// before it). Built once a frame; each note is played through feel_pattern alone, so the picture is what is heard.
class FeltTicks {
public:
    explicit FeltTicks(const Pattern& p) : probe(p) { probe.events.clear(); probe.motion.clear(); probe.punches.clear(); }
    double at(const Event& e) {
        probe.events.assign(1, e);
        const Pattern heard = feel_pattern(probe);
        if (heard.events.empty()) return e.tick; // hidden past the end: it does not play
        const double loop = std::max<uint32_t>(probe.lane_length(e.key), 1);
        double t = heard.events.front().tick;
        if (t - e.tick > loop / 2) t -= loop;
        else if (double(e.tick) - t > loop / 2) t += loop;
        return t;
    }

private:
    Pattern probe;
};

} // namespace melodee
