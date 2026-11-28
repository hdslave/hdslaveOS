// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// TIME CORRECT (Shift + Pad 14, as printed; round 2, time-correct C "strength, heard live"): no commit until APPLY.
// The selected track's pattern plays as close to its Q as STRENGTH says (K2), the second of each pair late by SWING
// (K3), as they turn: the right amount is found by ear and taken back by turning down (the pattern's feel,
// model/pattern.h: the notes stay as written). K1 Q (the pattern's, as on Patterns). Shift + K1 HUMANIZE (each note
// early or late), Shift + K2 its velocity, Shift + K3 the groove's amount.
// Keys: B1 APPLY (issue 55), while a feel is heard, writes it into the notes (model/pattern.h apply_feel: they move
// to where they play, the feel back to plain, one undo step), so Roll shows them on the grid. B2 GROOVE held: the
// track's patterns on the pads and the screen, a bank at a time (Pad Bank steps it, Minus / Plus the track they come
// from); a pad takes the groove from that pattern, again takes it away (shell/time_correct.h). Shift + B2 ALL lays
// this feel on the track's other patterns. B3 QUANTIZE, lit while the feel is heard, takes it off to compare by ear
// and puts it back. The pads play the track's sounds; Minus / Plus the previous / next pattern (the one corrected).
// Picture: two beats of the pattern over the grid of Q (beats taller, the swung steps in a shade where they land),
// each note where it is written (a ring) and where it plays (a dot, feel_pattern's), joined; under it the groove's
// pattern. B2 held: the patterns of the bank shown, the groove's lit, the one in hand ringed.
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/model/print.h"
#include "melodee/shell/seq_layer.h"
#include "melodee/shell/time_correct.h"
#include <algorithm>
#include <cmath>

namespace melodee {
namespace {
using namespace ui;

// A groove's source by name: the pattern's (A3), with its track when it is another's (T2 A3).
std::string groove_name(uint16_t from, unsigned track) {
    if (!from) return "NONE";
    const std::string name = print_pattern_name(groove_pattern(from));
    return groove_track(from) == track % track_count ? name : "T" + std::to_string(groove_track(from) + 1) + " " + name;
}

// B2 held: the sixteen patterns of the bank shown, as the pads sit (pad 1 bottom left): the groove's lit, the one
// in hand ringed, those with notes on a surface, empty ones dim.
void draw_patterns(Paint& p, const Snapshot& s, juce::Colour c) {
    const unsigned t = s.ui.track % track_count, view = s.ui.tc_groove_track % track_count, bank = s.ui.tc_groove_bank % pattern_banks;
    const uint16_t from = s.pattern.feel.groove_from;
    const float x0 = zone::pad, w = zone::width - 2 * zone::pad, gap = 4, cw = (w - 3 * gap) / 4, ch = 31, y0 = zone::picture_y + 4;
    for (unsigned k = 0; k < pattern_pads; ++k) {
        const unsigned n = bank * pattern_pads + k, row = 3 - k / 4, col = k % 4;
        const float x = x0 + col * (cw + gap), y = y0 + row * (ch + gap);
        const bool lit = from && from == groove_source(view, n), here = view == t && n == s.track().pattern % pattern_count;
        const bool notes = s.sketches[view][n].notes;
        p.fill(x, y, cw, ch, lit ? c : notes ? colour::surf : colour::quiet, 3);
        if (here) p.ring(x, y, cw, ch, colour::text.withAlpha(0.6f), 3, 1);
        p.text(juce::String(print_pattern_name(n)), x, y, cw, ch, plain(lit ? colour::bg : notes ? colour::sec : colour::dim, type::body, Weight::SemiBold),
               juce::Justification::centred);
    }
}

void draw(Paint& p, const Snapshot& s) {
    auto c = colour_of(s);
    const auto& pattern = s.pattern;
    const auto& f = pattern.feel;
    const unsigned t = s.ui.track % track_count;
    const bool heard = !f.plain(), held = s.ui.tc_groove_held;
    Header h = header_of(s);
    h.left.push_back(pill("TIME CORRECT"));
    if (held) {
        h.left.push_back(chip("GROOVE", colour::text));
        if (s.ui.tc_groove_track % track_count != t) h.left.push_back(pill("T" + juce::String(s.ui.tc_groove_track % track_count + 1)));
    } else {
        h.left.push_back(pill(juce::String(print_pattern_name(s.track().pattern)), c));
    }
    h.right.push_back(tag_of(s, s.ui.track));
    SoftKeys keys {{heard ? "APPLY" : "", "GROOVE", "QUANTIZE"}, held ? 1 : -1, held ? colour::text : c}; // Shift + Pad 14 again closes (T-a)
    keys.toggles = 4;
    keys.lit = heard ? 4 : 0;
    frame(p, s, keys, h);
    if (held) { draw_patterns(p, s, c); return; }

    // Two beats: the grid of Q, the swung steps where they land, each note written (ring) and played (dot).
    double q = pattern.q(), beat = s.project.beat_ticks(), span = 2 * beat;
    double late = division_triplet(pattern.division) ? 0 : q * (std::clamp<int>(f.swing, 50, 75) - 50) / 50.0;
    float x0 = zone::pad, w = zone::width - 2 * zone::pad, line_y = zone::picture_y + 46;
    auto x_of = [&](double tick) { return x0 + float(tick / span) * w; };
    p.fill(x0, line_y - 0.75f, w, 1.5f, colour::line);
    unsigned per_beat = std::max(1u, unsigned(std::lround(beat / q)));
    for (unsigned k = 0; k * q <= span + 0.5; ++k) {
        bool on_beat = k % per_beat == 0, swung = k % 2 == 1 && f.strength > 0;
        double at = k * q + (swung ? late : 0);
        if (at > span) continue;
        float gh = on_beat ? 40 : 24;
        p.fill(x_of(at) - 0.5f, line_y - gh / 2, 1, gh, swung ? knob_shade(s.ui.track, 2) : on_beat ? colour::sec : colour::mid);
    }
    FeltTicks felt(pattern);
    for (const auto& e : pattern.events) {
        if (e.tick >= span) break;
        float a = x_of(e.tick), b = x_of(felt.at(e));
        if (std::fabs(b - a) > 1) p.fill(std::min(a, b), line_y - 1, std::fabs(b - a), 2, c.withAlpha(0.35f), 1);
        p.ring(a - 3, line_y - 3, 6, 6, c.withAlpha(0.6f), 3, 1.2f);
        p.circle(b, line_y, 5, c);
    }
    p.text("2 BEATS", x0, line_y + 26, 80, 13, caps_text(colour::mid));

    // The groove: its pattern's name on a tile in the track's colour (B2 held picks it), or NONE.
    const float gy = zone::knobs_y - 36;
    icon(p, Icon::Bars, x0, gy + 5, 12, f.groove_from ? c : colour::mid, 1.4f);
    const auto cap_style = caps_text(colour::mid);
    float gx = x0 + 16 + p.text("GROOVE", x0 + 16, gy + 4, 80, 13, cap_style) + 8;
    const juce::String named = groove_name(f.groove_from, t);
    if (!f.groove_from) { p.text(named, gx, gy + 4, 80, 13, cap_style); return; }
    const auto tile = plain(colour::bg, type::body, Weight::SemiBold);
    const float tw = p.width(named, tile) + 14;
    p.fill(gx, gy, tw, 22, c, 3);
    p.text(named, gx, gy, tw, 22, tile, juce::Justification::centred);
}

// The pads play the track; B2 held, a pad takes the groove from that pattern of the track shown (shell/time_correct.h).
// A release always reaches the pad's note, whatever was held when it went down.
void pad(Context& c, unsigned pad, uint8_t velocity, bool down) {
    auto& ui = c.ui;
    if (!ui.tc_groove_held || !down) { c.play_pad(pad, velocity, down); return; }
    const unsigned t = ui.track % track_count, here = c.track().pattern % pattern_count, view = ui.tc_groove_track % track_count;
    const unsigned n = (ui.tc_groove_bank % pattern_banks) * pattern_pads + pad % pattern_pads;
    const uint16_t from = c.pattern().feel.groove_from, to = groove_source(view, n);
    const std::string name = groove_name(to, t);
    const Binding b {Binding::Kind::GrooveFrom, 0, 0, int(track_count * pattern_count), from};
    switch (groove_pick(from, t, here, view, n, !c.patterns.at[view][n].empty())) {
    case GroovePick::Take: c.apply(b, to); c.notice("Groove from " + name); return;
    case GroovePick::Clear: c.apply(b, 0); c.notice("No groove"); return;
    case GroovePick::Here: c.notice(name + " is this pattern"); return;
    case GroovePick::Empty: c.notice(name + " is empty"); return;
    }
}

// Minus / Plus: the previous / next pattern, the one corrected (as with Seq held). B2 held: the track whose patterns
// the pads show, the next that holds notes.
void nudge(Context& c, int way) {
    auto& ui = c.ui;
    if (!ui.tc_groove_held) { seq_step_pattern(c, way); return; }
    std::array<bool, track_count> notes {};
    for (unsigned t = 0; t < track_count; ++t)
        for (unsigned n = 0; n < pattern_count && !notes[t]; ++n) notes[t] = !c.patterns.at[t][n].empty();
    const unsigned to = groove_track_step(notes, ui.track, ui.tc_groove_track, way);
    if (to == ui.tc_groove_track % track_count) { c.notice(way > 0 ? "No later track with notes" : "No earlier track with notes"); return; }
    ui.tc_groove_track = uint8_t(to);
    c.notice("Track " + std::to_string(to + 1) + "'s patterns");
}

// Shift + B2 ALL: this feel on the track's other patterns, each its own undo.
void feel_on_all(Context& c) {
    Feel feel = c.pattern().feel;
    unsigned t = c.ui.track % track_count, laid = 0;
    for (unsigned n = 0; n < pattern_count; ++n) {
        if (n == c.track().pattern || c.patterns.at[t][n].feel == feel) continue;
        c.edit_pattern(t, n, "Feel", [&] { c.patterns.at[t][n].feel = feel; c.sync_pattern(t, n); });
        ++laid;
    }
    c.notice(laid ? "Feel on all patterns" : "Every pattern has this feel");
}

void soft(Context& c, int key) {
    using K = Binding::Kind;
    auto& ui = c.ui;
    int here = int(ui.track % track_count) * int(pattern_count) + c.track().pattern;
    if (key == 0) { // APPLY (issue 55): the feel written into the notes, one undo step
        if (c.pattern().feel.plain()) return;
        const unsigned t = ui.track % track_count, n = c.track().pattern % pattern_count;
        c.edit_pattern(t, n, "Apply feel", [&] { apply_feel(c.patterns.at[t][n]); c.sync_pattern(t, n); });
        if (ui.tc_stash_at == here) ui.tc_stash_at = -1; // nothing to put back
        c.notice("Feel in the notes");
        return;
    }
    if (key == 1) { // GROOVE: held, the patterns on the pads (its release: shell/shell.cpp); Shift + B2 ALL
        if (ui.shift) { feel_on_all(c); return; }
        const auto view = groove_view(c.pattern().feel.groove_from, ui.track, c.track().pattern);
        ui.tc_groove_held = true;
        ui.tc_groove_track = view.track;
        ui.tc_groove_bank = view.bank;
        return;
    }
    // QUANTIZE: the feel off to compare by ear, and back
    const auto& f = c.pattern().feel;
    if (!f.plain()) {
        ui.tc_stash = {f.strength, f.humanize, f.humanize_velocity, f.groove_amount};
        ui.tc_stash_at = int16_t(here);
        c.set_feel(K::FeelStrength, 0); c.set_feel(K::FeelHumanize, 0); c.set_feel(K::FeelVelocity, 0); c.set_feel(K::GrooveAmount, 0);
        c.notice("As written");
    } else if (ui.tc_stash_at == here) {
        c.set_feel(K::FeelStrength, ui.tc_stash[0]); c.set_feel(K::FeelHumanize, ui.tc_stash[1]);
        c.set_feel(K::FeelVelocity, ui.tc_stash[2]); c.set_feel(K::GrooveAmount, ui.tc_stash[3]);
        ui.tc_stash_at = -1;
        c.notice("Feel heard");
    }
}
} // namespace

const PageSpec time_correct_page {draw, pad, no_wheel, no_push, soft, nudge};

} // namespace melodee
