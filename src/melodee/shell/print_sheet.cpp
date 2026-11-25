// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Freeze plan F2c: the PRINT sheet, Seq held + Sample Record, and the
// question an edit of a frozen track asks first ("Unfreeze track N?").
#include "melodee/engine/engine_host.h"
#include "melodee/shell/ask.h"
#include "melodee/shell/context.h"
#include "melodee/shell/print_engine.h"
#include <cstring>
#include <string>

namespace melodee {

namespace {
std::string track_word(unsigned t) { return "Track " + std::to_string(t % track_count + 1); }
std::string pattern_word(unsigned p) { return "P" + std::to_string(p % pattern_count + 1); } // as the Seq layer names them

// Every pattern of track `t` holding notes, a bit each.
std::array<uint64_t, (pattern_count + 63) / 64> with_notes(const PatternBank& bank, unsigned t) {
    std::array<uint64_t, (pattern_count + 63) / 64> bits {};
    for (unsigned p = 0; p < pattern_count; ++p)
        if (!bank.at[t % track_count][p].empty()) bits[p / 64] |= uint64_t(1) << (p % 64);
    return bits;
}
bool none_of(const std::array<uint64_t, (pattern_count + 63) / 64>& bits) {
    for (auto b : bits) if (b) return false;
    return true;
}
} // namespace

// ---- the question before an edit of a frozen track

bool Context::print_covers(const UndoScope& s) const {
    return melodee::print_covers(prints.freeze, s) || melodee::print_covers(freeze_requested, s)
        || melodee::print_covers(freeze_submitted, s);
}

void Context::ask_unfreeze(unsigned track) {
    ask(*this, UiState::Ask::Unfreeze, int(track % track_count));
}

bool Context::frozen_edit(const UndoScope& scope) {
    if (!print_covers(scope)) return false;
    ask_unfreeze(scope.track);
    return true;
}

bool Context::frozen_edit(unsigned track) {
    if (!freeze_protects_track(track)) return false;
    ask_unfreeze(track);
    return true;
}

bool Context::edit_covers(const Edit& e) const {
    if (e.swap_a >= 0 || e.track_before) return false; // a move or a copy: the tracks are printed again (freeze plan F2b)
    const unsigned t = e.track % track_count;
    auto pattern_frozen = [&](unsigned track, unsigned p) {
        track %= track_count;
        return prints.freeze[track].plays(p) || freeze_requested[track].plays(p) || freeze_submitted[track].plays(p);
    };
    if (freeze_protects_track(t)) {
        auto differ = [](const auto& a, const auto& b) { return std::memcmp(&a, &b, sizeof(a)) != 0; }; // copies of model data, as undo compares them
        if (e.pad >= 0 && (differ(e.pad_after.sound, e.pad_before.sound) || differ(e.pad_after.chain, e.pad_before.chain) || !same_trims(e.pad_after, e.pad_before)))
            return true;
        if (!e.pads_before.empty() || e.sound_before || e.motion_before) return true;
    }
    if (e.pattern >= 0 && e.pattern_before && pattern_frozen(t, unsigned(e.pattern))) return true;
    if (e.chain >= 0 && e.chain_before) {
        UndoScope chain {UndoScope::Kind::Chain, uint8_t(t), uint16_t(e.chain)};
        if (print_covers(chain)) return true;
    }
    if (e.column >= 0)
        for (const auto& [track, value] : e.column_before) if (pattern_frozen(track, unsigned(e.column))) return true;
    for (const auto& p : e.patterns_before) if (pattern_frozen(p.track, p.pattern)) return true;
    return false;
}

unsigned Context::edit_track(const Edit& edit) const {
    unsigned t = edit.track % track_count;
    if (!freeze_protects_track(t) && edit.chain >= int(pad_chain_base)) t = unsigned(edit.chain - int(pad_chain_base)) / sampler_pads % track_count;
    if (!freeze_protects_track(t) && edit.chain >= 0 && edit.chain < int(track_count)) t = unsigned(edit.chain);
    if (!freeze_protects_track(t))
        for (const auto& p : edit.patterns_before) if (freeze_protects_track(p.track)) { t = p.track; break; }
    if (!freeze_protects_track(t) && edit.column >= 0)
        for (const auto& [track, value] : edit.column_before) if (freeze_protects_track(track)) { t = track; break; }
    return t;
}

bool Context::edit_gate(void* owner, Edit& edit) {
    auto& c = *static_cast<Context*>(owner);
    if (!c.edit_covers(edit)) return true;
    // What the edit changed goes back as it was, and nothing is kept: unfreeze first, then edit.
    c.apply_edit(edit, false);
    c.ask_unfreeze(c.edit_track(edit));
    return false;
}

// ---- the PRINT sheet

namespace {
auto eligible_patterns(const Context& c, unsigned t, bool clocked_anvil) {
    auto eligible = with_notes(c.patterns, t);
    if (clocked_anvil) {
        // The playing pattern can carry Anvil's sequence with zero MPC notes. Retain previously
        // frozen empty patterns so the sheet can show and unpick them after a launch.
        const unsigned p = (c.host.playing() ? c.host.pattern_of(t) : c.project.tracks[t].pattern) % pattern_count;
        eligible[p / 64] |= uint64_t(1) << (p % 64);
        for (size_t i = 0; i < eligible.size(); ++i) eligible[i] |= c.prints.freeze[t].patterns[i];
    }
    return eligible;
}
} // namespace

std::array<uint64_t, (pattern_count + 63) / 64> Context::freeze_patterns(unsigned track, bool deep) const {
    const unsigned t = track % track_count;
    return eligible_patterns(*this, t, !deep && anvil_sequencing(t));
}

void Context::open_print_sheet() {
    const unsigned t = ui.track % track_count;
    const auto& tr = project.tracks[t];
    if (!tr.used()) { notice("Empty track"); return; }
    auto& sheet = ui.print_sheet;
    if (!(sheet.track == t && sheet.printing)) { // a sheet under way comes back as it is
        sheet = UiState::PrintSheet {};
        sheet.track = uint8_t(t);
        const auto& f = frozen(t) ? prints.freeze[t] : freeze_last[t];
        sheet.clocked_anvil = anvil_sequencing(t);
        sheet.tap = sheet.clocked_anvil ? PrintTap::PostFx : f.tap;
        sheet.tail = f.tail;
        sheet.eligible = eligible_patterns(*this, t, sheet.clocked_anvil);
        sheet.picks = frozen(t) ? f.patterns : sheet.eligible;
        for (size_t n = 0; n < sheet.picks.size(); ++n) sheet.picks[n] &= sheet.eligible[n];
    }
    // The bank in view: the one the track plays.
    const unsigned shown = sheet.clocked_anvil && host.playing() ? host.pattern_of(t) : tr.pattern;
    ui.pattern_bank = uint8_t(shown % pattern_count / pattern_pads);
    if (ui.page != PageId::Print) open(PageId::Print);
}

void Context::refresh_print_sheet() {
    auto& sheet = ui.print_sheet;
    if (ui.page != PageId::Print || sheet.printing) return;
    const unsigned t = sheet.track % track_count;
    const bool clocked = anvil_sequencing(t);
    if (clocked == sheet.clocked_anvil) return;
    sheet.clocked_anvil = clocked;
    if (clocked) sheet.tap = PrintTap::PostFx;
    sheet.eligible = eligible_patterns(*this, t, clocked);
    if (!sheet.picks_touched) sheet.picks = frozen(t) ? prints.freeze[t].patterns : sheet.eligible;
    for (size_t n = 0; n < sheet.picks.size(); ++n) sheet.picks[n] &= sheet.eligible[n];
}

void Context::print_sheet_pad(unsigned pad) {
    refresh_print_sheet();
    auto& sheet = ui.print_sheet;
    if (sheet.printing) return;
    unsigned p = ui.pad_pattern(pad);
    if (!sheet.can_pick(p)) { notice(pattern_word(p) + " is empty"); return; }
    sheet.pick(p, !sheet.picked(p));
    sheet.picks_touched = true;
}

void Context::print_sheet_all() {
    refresh_print_sheet();
    auto& sheet = ui.print_sheet;
    if (sheet.printing) return;
    if (none_of(sheet.eligible)) return;
    sheet.picks = sheet.picks == sheet.eligible ? decltype(sheet.picks) {} : sheet.eligible;
    sheet.picks_touched = true;
}

void Context::print_sheet_go(bool deep_freeze) {
    refresh_print_sheet();
    auto& sheet = ui.print_sheet;
    const unsigned t = sheet.track % track_count;
    if (sheet.printing) return;
    if (project.tracks[t].engine == EngineKind::Midi) { notice("MIDI tracks: later"); return; }
    if (project_path.empty()) { notice("Save the project first"); return; }
    if (!printer) return;
    if (deep_freeze && deep(t)) { notice(track_word(t) + " is deep frozen"); return; }
    // Nothing lit on a frozen track: it plays live again, as Seq held + Sample Record does with no pattern left.
    if (!deep_freeze && freeze_protects_track(t) && none_of(sheet.picks)) { print_sheet_unfreeze(); return; }
    // Freeze plan F2d: a deep freeze takes every pattern with notes, lit or not, at Post FX.
    if (none_of(deep_freeze ? with_notes(patterns, t) : sheet.picks)) { notice(deep_freeze ? "No pattern with notes" : "No pattern lit"); return; }
    PrintAsk ask;
    ask.track = uint8_t(t);
    ask.patterns = sheet.picks;
    ask.tap = sheet.tap;
    ask.tail = sheet.tail;
    ask.mode = PrintAsk::Freeze;
    ask.deep = deep_freeze;
    if (deep_freeze) { sheet.picks = sheet.eligible = with_notes(patterns, t); sheet.tap = PrintTap::PostFx; }
    if (!frozen(t)) live_load[t] = host.track_load(t); // what it takes live now: what freezing saves
    if (!freeze(ask)) return;
    sheet.printing = true;
    sheet.serial = freeze_serial[t];
}

void Context::print_sheet_cancel() {
    auto& sheet = ui.print_sheet;
    const unsigned t = sheet.track % track_count;
    if (!sheet.printing) { back(); return; }
    if (printer && printer->busy()) {
        const auto job = printer->progress();
        if (job.track == t && job.freeze && job.serial == sheet.serial) printer->cancel();
    }
    const bool restoring = (freeze_restoring >> t) & 1;
    cancel_freeze_request(t);
    if (restoring) request_freeze(t, prints.freeze[t]); // the project's previously saved selection still needs its pack
    notice("Print called off");
}

// The track live again from the sheet, without asking: the key says what it does. Its prints stay for the next freeze.
void Context::print_sheet_unfreeze() {
    auto& sheet = ui.print_sheet;
    const unsigned t = sheet.track % track_count;
    if (sheet.printing) return;
    if (!freeze_protects_track(t)) { notice(track_word(t) + " is live"); return; }
    unfreeze(t);
    back();
}

// ---- Seq held + Sample Record

void Context::seq_print_start() {
    const unsigned t = ui.track % track_count;
    const auto& tr = project.tracks[t];
    ui.seq_used = true;
    if (!tr.used() || tr.engine == EngineKind::Midi) { notice(tr.used() ? "MIDI tracks: later" : "Empty track"); return; }
    if (project_path.empty()) { notice("Save the project first"); return; }
    if (deep(t)) { notice(track_word(t) + " is deep frozen: whole track"); return; } // freeze plan F2d: no pattern alone

    ui.seq_print = true;
    ui.seq_frozen = frozen(t) ? prints.freeze[t].patterns : decltype(ui.seq_frozen) {};
}

void Context::seq_print_pad(unsigned pad) {
    const unsigned t = ui.track % track_count, p = ui.pad_pattern(pad);
    ui.seq_used = true;
    uint64_t bit = uint64_t(1) << (p % 64);
    if (!(freeze_patterns(t)[p / 64] & bit)) { notice(pattern_word(p) + " is empty"); return; }
    ui.seq_frozen[p / 64] ^= bit;
}

void Context::seq_print_end() {
    if (!ui.seq_print) return;
    ui.seq_print = false;
    const unsigned t = ui.track % track_count;
    auto want = ui.seq_frozen;
    auto eligible = freeze_patterns(t);
    for (size_t n = 0; n < want.size(); ++n) want[n] &= eligible[n];
    if (frozen(t) && want == prints.freeze[t].patterns) return; // as it was
    if (none_of(want)) {
        unfreeze(t);
        return;
    }
    const auto& f = frozen(t) ? prints.freeze[t] : freeze_last[t];
    PrintAsk ask;
    ask.track = uint8_t(t);
    ask.patterns = want;
    ask.tap = f.tap;
    ask.tail = f.tail;
    ask.mode = PrintAsk::Freeze;
    if (!frozen(t)) live_load[t] = host.track_load(t);
    if (freeze(ask)) notice("Printing " + track_word(t));
}

// ---- freeze plan F2d: Shift + B3 DEEP FREEZE on a frozen track's Stage

void Context::deep_freeze(unsigned track) {
    const unsigned t = track % track_count;
    const auto& tr = project.tracks[t];
    if (!tr.used() || tr.engine == EngineKind::Midi) { notice(tr.used() ? "MIDI tracks: later" : "Empty track"); return; }
    if (project_path.empty()) { notice("Save the project first"); return; }
    if (deep(t)) { notice(track_word(t) + " is deep frozen"); return; }
    if (none_of(with_notes(patterns, t))) { notice("No pattern with notes"); return; }
    const auto& f = frozen(t) ? prints.freeze[t] : freeze_last[t];
    PrintAsk ask;
    ask.track = uint8_t(t);
    ask.all = true;
    ask.tail = f.tail;
    ask.mode = PrintAsk::Freeze;
    ask.deep = true;
    if (!frozen(t)) live_load[t] = host.track_load(t);
    if (freeze(ask)) notice("Deep freezing " + track_word(t));
}

} // namespace melodee
