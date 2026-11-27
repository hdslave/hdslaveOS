// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/shell/context.h"

// The Prog pad layout (model/progression.h) on Keys: the wheel picks the progression from the card's .progression
// files (shell/progression_file.h), and chords made on the MPC are stored on its pads: on Keys, hold the notes on
// another layout and press B2 STORE; the layout turns to Prog and the next pad hit takes the chord (B1 BACK: none).
// Each store saves the progression to <card>/Melodee/Progressions.
namespace melodee {

void prog_wheel(Context& c, int delta);  // the next or previous progression on the card, onto the track
void prog_layout_on(Context& c);         // K3 LAYOUT turned to Prog: an empty track gets the card's first
bool prog_can_store(const Context& c);   // Keys: pads held on a layout that plays notes (B2 then stores them)
void prog_store_begin(Context& c);       // B2 STORE
void prog_store_back(Context& c);        // B1 BACK: no store, the layout as it was
bool prog_store_pad(Context& c, unsigned pad); // a pad while storing: the chord goes on it (true: it took it)
// 16 Levels held's keys (shell/levels_keys.h):
void prog_ready(Context& c);              // the track's progression, or the card's first when it has none
unsigned prog_files(Context& c);          // the card listed again: how many, UiState::prog_at the track's own
std::string prog_file_name(const Context& c, unsigned at); // a file's name as the list shows it
// FILE's folders (issue 65, shell/progression_tree.h), over the list prog_files() made:
bool prog_files_open(Context& c);        // listed again, open on the folder of the track's own file (false: none on the card)
void prog_view_open(Context& c, const std::string& folder, const std::string& select = {}); // that folder's rows; the cursor
                                         // on the folder row `select`, else the track's own file, else the first
bool prog_view_enter(Context& c);        // Push or B3 on the row in hand: a folder opens (true), a file goes on the track
bool prog_view_up(Context& c);           // B1 BACK: the folder above (false at the top: the sheet closes)
bool prog_pick(Context& c, unsigned at);  // that file onto the track
void prog_save(Context& c, unsigned pad, unsigned index); // the track's progression to Melodee/Progressions (a new one: Mine 1 ...), saying the pad's chord

} // namespace melodee
