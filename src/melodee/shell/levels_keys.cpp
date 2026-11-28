// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// 16 Levels held's keys on the MIDI thread (shell/levels_keys.h has their rules): SCALE and CHORDS, the type in one
// press; on Chords FILE, the card's progression files, and SET, a chord of your own on a pad.
#include "melodee/shell/levels_keys.h"
#include "melodee/shell/context.h"
#include "melodee/shell/progression_jobs.h"
#include <algorithm>
#include <cstdio>
#include <string>

namespace melodee {
namespace {

// The type in use, 16 Levels on: a kit's last pad on all sixteen, a synth's notes.
void type_on(Context& c, LevelsKind kind) {
    auto& t = c.track();
    c.ui.levels_hit = -1;
    if (t.engine == EngineKind::Sampler && !t.levels) t.levels_pad = t.last_pad;
    t.levels_kind = uint8_t(kind);
    t.levels = true;
    c.dirty = true;
}

// FILE's rows around the row in hand.
void show_rows(Context& c) {
    auto& ui = c.ui;
    const unsigned count = ui.prog_view_count;
    ui.prog_cursor = uint16_t(count ? std::min<unsigned>(ui.prog_cursor, count - 1) : 0);
    ui.prog_rows_first = uint16_t(prog_rows_first(ui.prog_cursor, count));
    ui.prog_rows_folders = 0;
    ui.prog_cursor_folder = ui.prog_cursor < c.prog_view.size() && c.prog_view[ui.prog_cursor].file < 0;
    ui.prog_rows_count = uint8_t(std::min<unsigned>(count - ui.prog_rows_first, prog_rows_shown));
    for (unsigned k = 0; k < ui.prog_rows.size(); ++k) {
        auto& row = ui.prog_rows[k];
        row[0] = 0;
        if (k >= ui.prog_rows_count) continue;
        const auto& entry = c.prog_view[ui.prog_rows_first + k];
        std::snprintf(row.data(), row.size(), "%s", entry.name.c_str());
        if (entry.file < 0) ui.prog_rows_folders = uint8_t(ui.prog_rows_folders | 1u << k);
    }
}

// The chord on the cards into the progression, in the key the file has it (K2 KEY moves it as it is heard).
void write_chord(Context& c) {
    auto& ui = c.ui;
    auto& t = c.track();
    const unsigned index = ui.chord_index % prog_chords;
    const unsigned root = unsigned(((int(ui.chord_root) - t.prog_shift) % 12 + 12) % 12);
    ProgChord chord = build_prog_chord(root, ui.chord_type, ui.chord_invert);
    chord.root = t.prog.chords[index].root; // the file's Root role stays with the pad
    t.prog.chords[index] = chord;
    t.prog.count = uint8_t(std::max<unsigned>(t.prog.count, index + 1));
    c.dirty = true;
}

// The chord's place for a pad: a kit plays the first sixteen (16 Levels' Chords), a synth the bank in view.
unsigned index_of(const Track& t, unsigned pad) { return t.engine == EngineKind::Sampler ? pad % 16 : prog_index(t, pad); }

} // namespace

void levels_chords_on_now(Context& c) {
    auto& t = c.track();
    if (!t.used() || t.is_audio()) return;
    if (drum_pads(t)) { c.notice("Chords: not on Drums pads"); return; }
    type_on(c, LevelsKind::Chords);
    prog_ready(c);
    c.notice(t.prog.file[0] ? "Chords: " + std::string(t.prog.file).substr(0, 24) : t.prog.empty() ? std::string("Chords: no file, SET makes one") : std::string("Chords"));
}

void levels_key(Context& c, unsigned key) {
    auto& ui = c.ui;
    const auto keys = levels_keys(c.track());
    switch (keys.key[key % 3]) {
    case LevelsKey::Scale:
        type_on(c, LevelsKind::Scale);
        c.notice("16 Levels: Scale");
        break;
    case LevelsKey::Chords: levels_chords_on_now(c); break;
    case LevelsKey::File:
        if (!prog_files_open(c)) { c.notice("No progression files on the card"); break; }
        show_rows(c);
        ui.levels_sheet = levels_sheet_files;
        ui.levels_sheet_track = ui.track;
        break;
    case LevelsKey::Set:
        if (!c.track().levels) type_on(c, LevelsKind::Chords); // the pads play the chords being made
        ui.levels_sheet = levels_sheet_pad;
        ui.levels_sheet_track = ui.track;
        break;
    default: break;
    }
}

bool levels_sheet_button(Context& c, SurfaceButton button) {
    using B = SurfaceButton;
    auto& ui = c.ui;
    if (!ui.levels_sheet) return false;
    if (button == B::Play || button == B::Stop || button == B::Shift) return false; // the transport still answers
    if (button == B::PadBank && ui.levels_sheet != levels_sheet_files) return false;  // SET: Pad Bank steps to a synth's banks B and C
    auto& t = c.track();
    switch (ui.levels_sheet) {
    case levels_sheet_files: // issue 65: a folder opens, BACK goes up one; at the top BACK closes, a file is picked
        if (button == B::TopLeft) {
            if (prog_view_up(c)) show_rows(c);
            else ui.levels_sheet = levels_sheet_none;
        } else if (button == B::TopRight || button == B::WheelPush) {
            if (prog_view_enter(c)) show_rows(c);
            else ui.levels_sheet = levels_sheet_none;
        }
        break;
    case levels_sheet_pad:
        if (button == B::TopLeft) ui.levels_sheet = levels_sheet_none;
        break;
    case levels_sheet_chord:
        if (button == B::TopLeft) { // BACK: the chord as it was
            t.prog.chords[ui.chord_index % prog_chords] = ui.chord_before;
            ui.levels_sheet = levels_sheet_pad;
        } else if (button == B::TopRight || button == B::WheelPush) { // DONE: saved, then the next pad
            const unsigned index = ui.chord_index % prog_chords;
            if (!t.prog.chords[index].used()) write_chord(c); // an empty pad with no card turned: the chord the cards show
            prog_save(c, index % 16, index);
            ui.levels_sheet = levels_sheet_pad;
        }
        break;
    default: ui.levels_sheet = levels_sheet_none; break;
    }
    return true; // any other button waits
}

void levels_sheet_wheel(Context& c, int delta) {
    auto& ui = c.ui;
    if (ui.levels_sheet != levels_sheet_files || !ui.prog_view_count) return;
    ui.prog_cursor = uint16_t(std::clamp(int(ui.prog_cursor) + delta, 0, int(ui.prog_view_count) - 1));
    show_rows(c);
}

void levels_set_pad(Context& c, unsigned pad) {
    auto& ui = c.ui;
    if (ui.levels_sheet != levels_sheet_pad) return;
    const auto& t = c.track();
    const unsigned index = index_of(t, pad);
    ui.chord_index = uint8_t(index);
    ui.chord_before = t.prog.chords[index];
    BuiltPick pick;
    if (find_built_chord(ui.chord_before, pick)) { // the cards start on what the pad plays, as it is heard
        ui.chord_root = uint8_t(((int(pick.root) + t.prog_shift) % 12 + 12) % 12);
        ui.chord_type = pick.type; ui.chord_invert = pick.inversion;
    } else { // an empty pad, or a chord the table has not: the project's root, a seventh chord of its scale
        std::array<uint8_t, 12> degrees {};
        unsigned count = scale_degrees(c.project.scale, degrees);
        bool minor = false, major = false;
        for (unsigned d = 0; d < count; ++d) { minor |= degrees[d] == 3; major |= degrees[d] == 4; }
        ui.chord_root = ui.chord_before.used() ? uint8_t(((int(ui.chord_before.notes[0]) + t.prog_shift) % 12 + 12) % 12) : uint8_t(c.project.root % 12);
        ui.chord_type = minor && !major ? 4 : 3; // min7, else Maj7
        ui.chord_invert = 0;
    }
    ui.levels_sheet = levels_sheet_chord;
}

void levels_chord_card(Context& c, unsigned card, int value) {
    auto& ui = c.ui;
    if (ui.levels_sheet != levels_sheet_chord) return;
    switch (card % 3) {
    case 0: ui.chord_root = uint8_t(std::clamp(value, 0, 11)); break;
    case 1: ui.chord_type = uint8_t(std::clamp(value, 0, int(built_chord_count) - 1)); break;
    default: ui.chord_invert = uint8_t(std::clamp(value, 0, int(built_inversions) - 1)); break;
    }
    write_chord(c);
}

} // namespace melodee
