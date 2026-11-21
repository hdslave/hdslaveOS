// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <cstdint>

// Background save, as on the stock app. The project is saved to its file autosave_idle_ms after the last control
// was used, while nothing plays, records, prints or loads and no sheet is open. A project with no file yet gets
// one on the card. It never runs while the sequencer plays: a save reads each plug-in's state on the MIDI thread.
// Settings' Autosave Off (issue 18) turns it off: only SAVE writes the project then.
// Pure, for the shell and the tests.
namespace melodee {

constexpr uint32_t autosave_idle_ms = 3000;   // this long after the last control or note
constexpr uint32_t autosave_retry_ms = 10000; // after a save that found no card: not before this

struct AutosaveFacts {
    bool enabled = true;    // Settings > SEQ > Autosave
    bool dirty = false;     // changed since it was saved or loaded
    bool playing = false;   // the sequencer, the song or a loop take runs
    bool recording = false; // a pattern take, a sample take, a print, an auto sample or an export runs
    bool sheet = false;     // a question, the Name sheet, the Exit sheet, SD card access or an update has the controls
    bool loading = false;   // the project's samples are still being read
    bool editing = false;   // a Chop take playing or Record open (their work is not in the project yet), or an MPC project was
                            // opened and has no file of its own (opening one writes nothing; SAVE AS gives it a file)
    bool failed = false;    // the last save found no card
    uint32_t now = 0, last_input = 0, last_try = 0;
};

inline bool autosave_due(const AutosaveFacts& f) {
    if (!f.enabled || !f.dirty || f.playing || f.recording || f.sheet || f.loading || f.editing) return false;
    if (f.now - f.last_input < autosave_idle_ms) return false;
    return !f.failed || f.now - f.last_try >= autosave_retry_ms;
}

} // namespace melodee
