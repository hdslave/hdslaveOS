// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Jam capture, after Melodee FM-1's jam to song (model/jam_log.h). From PLAY outside Song to STOP, each bar is
// logged as it plays: the pattern each used track plays (none while it is silent or off), which tracks are not heard
// as the bar begins (Mute, or another track's Solo), the tempo, and a sequence that took over on it. TAKE JAM
// (Song's Shift + B1) lays the log after the song's end; PLAY outside Song starts a new log.
#include "melodee/shell/context.h"
#include "melodee/engine/engine_host.h"
#include <algorithm>
#include <string>

namespace melodee {
namespace {

// The patterns the used tracks play now; read through the bar, so the bar keeps what played to its end (a launch
// lands on a bar line).
void read_patterns(const Project& project, const EngineHost& host, JamBar& bar) {
    for (unsigned t = 0; t < track_count; ++t)
        bar.pattern[t] = project.tracks[t].used() && host.position_of(t) >= 0 ? uint8_t(host.pattern_of(t) % pattern_count) : jam_silent;
}

// The tracks not heard and the tempo, as a bar begins.
void read_mix(const Project& project, unsigned tempo, JamBar& bar) {
    bool solo = false;
    for (const auto& t : project.tracks) solo = solo || (t.used() && t.mix.solo);
    bar.muted = 0;
    for (unsigned t = 0; t < track_count; ++t) {
        const auto& mix = project.tracks[t].mix;
        if (mix.mute || (solo && !mix.solo)) bar.muted = uint16_t(bar.muted | 1u << t);
    }
    bar.tempo = uint16_t(tempo);
}

} // namespace

void Context::jam_tick() {
    const bool running = host.playing() && !host.count_in() && !host.song_playing();
    if (!running) {
        if (jam_running && !jam.full()) jam.bars.push_back(jam_now); // the bar it stopped in
        jam_running = false;
        if (host.song_playing()) jam_scene = -1; // launched while the song plays: no jam's
        ui.jam_logging = false;
        ui.jam_bars = uint16_t(jam.bars.size());
        return;
    }
    const uint32_t bar = host.ticks() / std::max(project.bar_ticks(), 1u);
    auto begin_bar = [&] {
        jam_now.scene = int16_t(jam_scene);
        jam_scene = -1;
        read_mix(project, tempo_now(), jam_now);
    };
    if (!jam_running) { // PLAY outside Song: a new jam
        jam.bars.clear();
        jam_running = true;
        jam_bar_seen = bar;
        jam_now = JamBar {};
        begin_bar();
    } else if (bar != jam_bar_seen) {
        // The bar before is logged; bars passed between two looks played as it did.
        const uint32_t passed = bar > jam_bar_seen ? std::min<uint32_t>(bar - jam_bar_seen, 64) : 1;
        for (uint32_t n = 0; n < passed && !jam.full(); ++n) {
            jam.bars.push_back(jam_now);
            jam_now.scene = -1;
        }
        jam_bar_seen = bar;
        begin_bar();
    }
    read_patterns(project, host, jam_now);
    ui.jam_logging = !jam.full();
    ui.jam_bars = uint16_t(jam.bars.size());
}

void Context::take_jam() {
    if (jam.bars.empty()) { notice("No jam yet"); return; }
    const JamTake took = jam_to_song(project, patterns, jam);
    if (!took.bars) { notice("Song is full"); return; }
    ui.song_cursor = uint16_t(took.at);
    ui.song_view = uint16_t(took.at);
    sync_song();
    dirty = true;
    notice("Jam at bar " + std::to_string(took.at + 1) + (took.left_out ? ", some left out" : ""));
}

} // namespace melodee
