// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

// Song (issue 60): the loop range as a section to copy, paste and take out, on the selected track's lane or, in the
// all-tracks view (PAD BANK tap), on every lane with the markers and the tempo lane. A clip the range's edge falls in
// is cut there, as Chop cuts it (the part after the edge starts its pattern again). Pure, for the page and the tests.
namespace melodee {

constexpr uint16_t song_all_lanes = 0xffff;
inline uint16_t song_lanes(bool all, unsigned track) { return all ? song_all_lanes : uint16_t(1u << (track % track_count)); }
inline bool song_lane_in(uint16_t lanes, unsigned track) { return (lanes >> (track % track_count)) & 1; }

// A copied section: its clips, each bar from the section's start, and how many bars it is.
struct SongSection {
    std::vector<Clip> clips;
    uint16_t bars = 0;
    uint16_t lanes = 0;
    bool empty() const { return !bars; }
};

// The clips of [from, to) on `lanes`, cut to it.
inline SongSection song_copy_range(const Song& song, unsigned from, unsigned to, uint16_t lanes) {
    SongSection out;
    if (to <= from) return out;
    out.bars = uint16_t(to - from);
    out.lanes = lanes;
    for (unsigned n = 0; n < song.count; ++n) {
        Clip c = song.clips[n];
        if (!song_lane_in(lanes, c.track) || c.end() <= from || c.bar >= to) continue;
        const unsigned start = std::max<unsigned>(c.bar, from), end = std::min<unsigned>(c.end(), to);
        c.bar = uint16_t(start - from);
        c.bars = uint16_t(end - start);
        out.clips.push_back(c);
    }
    return out;
}

// How many more clips an edit at `at` would need: each clip on `lanes` that `at` falls inside is cut in two.
inline unsigned song_splits_at(const Song& song, unsigned at, uint16_t lanes) {
    unsigned splits = 0;
    for (unsigned n = 0; n < song.count; ++n) {
        const auto& c = song.clips[n];
        splits += song_lane_in(lanes, c.track) && c.bar < at && c.end() > at;
    }
    return splits;
}

// `bars` of silence at `at` on `lanes`: what starts there or after moves on, a clip across `at` is cut and its tail
// moves on. In every lane the markers and the tempo lane's points at or after it move too. False (nothing done) when
// the song would run out of clips or past its last bar.
inline bool song_insert_bars(Song& song, unsigned at, unsigned bars, uint16_t lanes) {
    if (!bars) return true;
    if (song.count + song_splits_at(song, at, lanes) > song.clips.size()) return false;
    for (unsigned n = 0; n < song.count; ++n) {
        const auto& c = song.clips[n];
        if (song_lane_in(lanes, c.track) && c.end() > at && c.end() + bars > song_bars) return false; // moved past the last bar
    }
    const unsigned count = song.count;
    for (unsigned n = 0; n < count; ++n) {
        auto& c = song.clips[n];
        if (!song_lane_in(lanes, c.track) || c.end() <= at) continue;
        if (c.bar >= at) { c.bar = uint16_t(c.bar + bars); continue; }
        Clip tail = c;
        tail.bar = uint16_t(at + bars);
        tail.bars = uint16_t(c.end() - at);
        c.bars = uint16_t(at - c.bar);
        song.clips[song.count++] = tail;
    }
    if (lanes == song_all_lanes) {
        for (unsigned m = 0; m < song.marker_count; ++m)
            if (song.markers[m].bar >= at) song.markers[m].bar = uint16_t(std::min<unsigned>(song.markers[m].bar + bars, song_bars - 1));
        for (unsigned k = 0; k < song.tempo.count; ++k)
            if (song.tempo.points[k].bar >= at) song.tempo.points[k].bar = uint16_t(std::min<unsigned>(song.tempo.points[k].bar + bars, song_bars - 1));
    }
    return true;
}

// [from, to) taken out of `lanes`: the clips in it go, a clip across an edge is cut there, and what comes after
// moves back to close the gap (in every lane the markers and tempo points too; those in it go). False when the
// song would run out of clips.
inline bool song_remove_bars(Song& song, unsigned from, unsigned to, uint16_t lanes) {
    if (to <= from) return true;
    const unsigned bars = to - from;
    for (unsigned n = 0; n < song.count;) {
        auto& c = song.clips[n];
        if (!song_lane_in(lanes, c.track) || c.end() <= from) { ++n; continue; }
        if (c.bar >= to) { c.bar = uint16_t(c.bar - bars); ++n; continue; }
        if (c.bar >= from && c.end() <= to) { song.erase(n); continue; } // inside: gone (the next slides into n)
        if (c.bar < from) { // starts before: keeps its start, loses what is in the range
            c.bars = uint16_t(c.bars - (std::min<unsigned>(c.end(), to) - from));
            ++n;
            continue;
        }
        // starts inside, ends after: what is after the range, at its start
        c.bars = uint16_t(c.end() - to);
        c.bar = uint16_t(from);
        ++n;
    }
    if (lanes == song_all_lanes) {
        for (unsigned m = 0; m < song.marker_count;) {
            auto& mark = song.markers[m];
            if (mark.bar >= from && mark.bar < to && mark.bar != from) { // inside: gone
                for (unsigned k = m + 1; k < song.marker_count; ++k) song.markers[k - 1] = song.markers[k];
                --song.marker_count;
                continue;
            }
            if (mark.bar >= to) mark.bar = uint16_t(mark.bar - bars);
            ++m;
        }
        auto& lane = song.tempo;
        unsigned kept = 0;
        for (unsigned k = 0; k < lane.count; ++k) {
            auto point = lane.points[k];
            if (point.bar > from && point.bar < to) continue; // inside: gone
            if (point.bar >= to) point.bar = uint16_t(point.bar - bars);
            lane.points[kept++] = point;
        }
        lane.count = uint8_t(kept);
    }
    return true;
}

// LOOP on Song (issue 60), on a clip or not: the first tap the loop's start at `cursor` (kept in `waiting`), the next
// its end (the cursor's bar the last in it, either way round); two taps on one bar of a clip (`clip`, its index; -1
// none), the cursor not moved, the clip's bars. With a loop on and none waiting, a tap turns it off. What it did.
enum class SongLoopTap : uint8_t { Start, Set, Off };
inline SongLoopTap song_loop_tap(Song& song, int16_t& waiting, unsigned cursor, int clip) {
    if (waiting >= 0) {
        const unsigned a = unsigned(waiting), b = cursor;
        if (a == b && clip >= 0 && unsigned(clip) < song.count) { song.loop_start = song.clips[size_t(clip)].bar; song.loop_end = song.clips[size_t(clip)].end(); }
        else { song.loop_start = uint16_t(std::min(a, b)); song.loop_end = uint16_t(std::max(a, b) + 1); }
        waiting = -1;
        return SongLoopTap::Set;
    }
    if (song.looping()) { song.loop_start = song.loop_end = 0; return SongLoopTap::Off; }
    waiting = int16_t(cursor);
    return SongLoopTap::Start;
}

// K2 PATTERN on Song (issue 60): the track's patterns that hold notes (`used`, a bit each), the first empty one after
// the last of them (else the first empty), and `current` whatever it is, in order.
inline std::vector<uint8_t> song_pattern_list(const std::array<uint64_t, 2>& used, unsigned current) {
    auto has = [&](unsigned n) { return (used[(n / 64) % 2] >> (n % 64)) & 1; };
    std::vector<uint8_t> out;
    int last = -1;
    for (unsigned n = 0; n < pattern_count; ++n) if (has(n)) { out.push_back(uint8_t(n)); last = int(n); }
    int empty = -1;
    for (unsigned n = unsigned(last + 1); n < pattern_count && empty < 0; ++n) if (!has(n)) empty = int(n);
    for (unsigned n = 0; n < pattern_count && empty < 0; ++n) if (!has(n)) empty = int(n);
    if (empty >= 0) out.push_back(uint8_t(empty));
    current %= pattern_count;
    if (std::find(out.begin(), out.end(), uint8_t(current)) == out.end()) out.push_back(uint8_t(current));
    std::sort(out.begin(), out.end());
    return out;
}
inline unsigned song_pattern_index(const std::vector<uint8_t>& list, unsigned pattern) {
    auto at = std::find(list.begin(), list.end(), uint8_t(pattern % pattern_count));
    return at == list.end() ? 0u : unsigned(at - list.begin());
}

// A section pasted at `at`, room made for it first (song_insert_bars on the section's lanes).
inline bool song_paste(Song& song, const SongSection& section, unsigned at) {
    if (section.empty()) return false;
    if (song.count + song_splits_at(song, at, section.lanes) + section.clips.size() > song.clips.size()) return false;
    if (at + section.bars > song_bars) return false;
    if (!song_insert_bars(song, at, section.bars, section.lanes)) return false;
    for (Clip c : section.clips) {
        c.bar = uint16_t(c.bar + at);
        if (!song.place(c)) return false;
    }
    return true;
}

} // namespace melodee
