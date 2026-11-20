// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Roadmap P5: scenes and follow actions. A scene is a column of patterns
// across the tracks (pattern n of each), with a name and a tempo of its own;
// on screen it is a sequence (shell/sequences.h). Launching it queues pattern
// n on every used track for the next bar, as a sequence on the stock app: a
// track with no notes there goes silent, its empty pattern as long as the
// sequence. Its tempo takes over on that bar. A pattern's follow
// action (model/follow.h) is the audio thread's (EngineHost::follow); here the
// shell tells it which tracks play a chain (their follow actions wait) and
// follows what it brought, so a track's pattern is the one playing.
#include "melodee/shell/context.h"
#include "melodee/shell/ask.h"
#include "melodee/shell/sequences.h"
#include "melodee/engine/engine_host.h"
#include <algorithm>
#include <string>

namespace melodee {
using Type = Command::Type;

void Context::launch_scene(unsigned scene, bool quiet) {
    scene %= pattern_count;
    unsigned count = 0, selected = ui.track % track_count;
    unsigned playing = host.playing() ? unsigned(host.pattern_of(selected)) % pattern_count : track().pattern % pattern_count;
    const auto& fallback = patterns.at[selected][playing];
    uint32_t length = sequence_length(project, patterns, selected, scene, fallback.length);
    bool free_now = !sequence_length(project, patterns, selected, scene, 0) && fallback.is_free(); // no notes there, none playing: Free stays
    for (unsigned t = 0; t < track_count; ++t) {
        if (!sequence_takes(project.tracks[t]) && t != selected) continue;
        auto& target = patterns.at[t][scene];
        if (target.empty() && length && (target.length != length || target.free != free_now)) { // silent, as long as the sequence
            target.length = length; target.free = free_now;
            sync_pattern(t, scene);
        }
        launch(t, scene);
        count += !target.empty();
    }
    const auto& s = project.scenes[scene];
    if (s.tempo) { // on the bar the patterns take over on (stopped, at once)
        project.tempo = s.tempo;
        send({Type::SceneTempo, 0, 0, 0, int16_t(s.tempo)});
        dirty = true;
    }
    ui.scene = uint8_t(scene);
    if (quiet) return;
    std::string text = "SEQ " + std::to_string(scene + 1);
    if (s.name[0]) text += std::string(" ") + s.name;
    text += ": " + std::to_string(count) + (count == 1 ? " track" : " tracks");
    if (s.tempo) text += ", " + std::to_string(s.tempo / 10) + (s.tempo % 10 ? "." + std::to_string(s.tempo % 10) : "") + " BPM";
    notice(text);
}

void Context::set_scene_tempo(unsigned scene, int tenths) {
    auto& s = project.scenes[scene % pattern_count];
    uint16_t value = uint16_t(tenths < 300 ? 0 : std::min(tenths, 3000)); // below 30 BPM: none of its own
    if (s.tempo == value) return;
    s.tempo = value;
    dirty = true;
}

void Context::name_scene(unsigned scene) {
    name_scene_at = uint8_t(scene % pattern_count);
    const auto& s = project.scenes[name_scene_at];
    open_name(NameTarget::Scene, s.name[0] ? std::string(s.name) : "SEQ " + std::to_string(name_scene_at + 1), sizeof(Scene::name) - 1, true);
}

void Context::name_track(unsigned t) {
    name_track_at = uint8_t(t % track_count);
    const auto& track = project.tracks[name_track_at];
    if (!track.used()) return;
    open_name(NameTarget::Track, track.name[0] ? std::string(track.name) : "TRACK " + std::to_string(name_track_at + 1), name_length - 1, true);
}

void Context::clear_scene(unsigned scene) {
    auto& s = project.scenes[scene % pattern_count];
    if (s.plain()) { notice("SEQ " + std::to_string(scene % pattern_count + 1) + " has no name or tempo"); return; }
    s = Scene {};
    dirty = true;
    notice("SEQ " + std::to_string(scene % pattern_count + 1) + " cleared");
}

// The selected track's pattern's follow action, from its cards on SCENES (one undo a turn: the pattern's).
void Context::set_follow(Binding::Kind kind, int value) {
    using Kind = Binding::Kind;
    auto& f = pattern().follow;
    PatternFollow was = f;
    switch (kind) {
    case Kind::FollowAction: f.action = uint8_t(std::clamp(value, 0, int(FollowAction::Count) - 1)); break;
    case Kind::FollowOther: f.other = uint8_t(std::clamp(value, 0, int(FollowAction::Count) - 1)); break;
    case Kind::FollowAfter: follow_after_of_card(f, std::clamp(value, 0, 127)); break;
    case Kind::FollowChance: f.chance = uint8_t(std::clamp(value, 0, 100)); break;
    default: return;
    }
    if (f != was) sync_pattern(ui.track, track().pattern);
}

// B3 ALL on SCENES: the pattern in hand's follow action onto every track's pattern with notes in its column; one undo.
void Context::follow_to_column() {
    unsigned column = track().pattern % pattern_count;
    PatternFollow f = pattern().follow;
    Edit edit;
    edit.what = "Follow to SEQ " + std::to_string(column + 1);
    edit.track = ui.track;
    edit.column = int16_t(column);
    for (unsigned t = 0; t < track_count; ++t) {
        const auto& p = patterns.at[t][column];
        if (t == ui.track % track_count || !project.tracks[t].used() || p.empty() || p.follow == f) continue;
        Pattern after = p;
        after.follow = f;
        edit.column_before.emplace_back(uint8_t(t), p);
        edit.column_after.emplace_back(uint8_t(t), std::move(after));
    }
    if (edit.column_after.empty()) { notice("SEQ " + std::to_string(column + 1) + ": nothing else to follow"); return; }
    for (const auto& [t, value] : edit.column_after) restore_pattern(t, column, value);
    unsigned count = unsigned(edit.column_after.size());
    edits.push(std::move(edit));
    dirty = true;
    notice("Follow to " + std::to_string(count) + (count == 1 ? " track" : " tracks"));
}

// CAPTURE on Scenes (Shift + B2, then a pad): what plays becomes scene n, each used track's playing pattern copied
// into its pattern n, one undo. Patterns with notes there would be replaced: asked first ("Replace 2 patterns in S5?").
void Context::capture_scene(unsigned column, bool asked) {
    column %= pattern_count;
    ui.scene_capture = false;
    Edit edit;
    edit.what = "Capture SEQ " + std::to_string(column + 1);
    edit.track = ui.track;
    edit.column = int16_t(column);
    unsigned replaced = 0;
    for (unsigned t = 0; t < track_count; ++t) {
        if (!project.tracks[t].used()) continue;
        unsigned from = host.pattern_of(t) % pattern_count;
        if (from == column) continue; // it plays this column already
        const auto& was = patterns.at[t][column];
        if (!was.empty()) ++replaced;
        edit.column_before.emplace_back(uint8_t(t), was);
        edit.column_after.emplace_back(uint8_t(t), patterns.at[t][from]);
    }
    if (edit.column_after.empty()) { notice("SEQ " + std::to_string(column + 1) + ": every track plays it already"); return; }
    if (replaced && !asked) { ask(*this, UiState::Ask::CaptureScene, int(column)); return; }
    for (const auto& [t, value] : edit.column_after) restore_pattern(t, column, value);
    unsigned count = unsigned(edit.column_after.size());
    edits.push(std::move(edit));
    dirty = true;
    ui.scene = uint8_t(column);
    notice("SEQ " + std::to_string(column + 1) + " captured: " + std::to_string(count) + (count == 1 ? " track" : " tracks"));
}

unsigned Context::tempo_now() const {
    if (!project.song.tempo.empty() && host.song_playing() && host.playing()) return host.tempo_now(); // the lane's, as it plays
    return project.tempo_of(ui.lead_pattern);
}

// Roadmap P5: the song's tempo lane, from Song's Shift + K1 (the tempo at the cursor's bar; under 30 BPM none there)
// and Shift + K2 (stepped to on its bar, or ramped to from the point before); one undo a turn, the song's.
void Context::set_song_tempo(unsigned bar, int tenths) {
    auto& lane = project.song.tempo;
    if (tenths <= 0) { if (!lane.erase(bar)) return; }
    else {
        int at = lane.at_bar(bar);
        bool ramp = at >= 0 && lane.points[size_t(at)].ramp;
        if (at >= 0 && lane.points[size_t(at)].tempo == unsigned(tenths)) return;
        if (!lane.set(bar, unsigned(tenths), ramp)) { notice("Tempo lane full"); return; }
    }
    sync_song();
    dirty = true;
}

void Context::set_song_tempo_shape(unsigned bar, bool ramp) {
    auto& lane = project.song.tempo;
    int at = lane.at_bar(bar);
    if (at < 0 || lane.points[size_t(at)].ramp == ramp) return;
    lane.points[size_t(at)].ramp = ramp;
    sync_song();
    dirty = true;
}

// MIDI thread, often: a chained track's follow actions wait (the chain is its order, docs/ROADMAP.md P5), and a
// follow action that came moves the track's pattern to the one it brought.
void Context::follow_tick() {
    bool patterns_playing = host.playing() && !host.song_playing();
    for (unsigned t = 0; t < track_count; ++t) {
        auto& tr = project.tracks[t];
        bool chained = tr.chained();
        if (chained != chained_sent[t]) {
            chained_sent[t] = chained;
            send({Type::Chained, uint8_t(t), 0, 0, int16_t(chained)});
        }
        uint32_t came = host.follows_of(t);
        if (came == follows_seen[t]) continue;
        follows_seen[t] = came;
        if (patterns_playing && !(host.stopped_tracks() >> t & 1)) {
            tr.pattern = uint8_t(host.pattern_of(t) % pattern_count);
            if (t == ui.track % track_count) ui.lead_pattern = tr.pattern;
        }
    }
    // The bank in view follows the selected track's pattern (playing, else the one that will) when that changes.
    unsigned shown = host.playing() ? unsigned(host.pattern_of(ui.track % track_count)) % pattern_count : track().pattern % pattern_count;
    if (int(shown) != ui.bank_seen) { ui.bank_seen = int16_t(shown); ui.pattern_bank = uint8_t(shown / pattern_pads); }
}

} // namespace melodee
