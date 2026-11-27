// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// TIME CORRECT (Shift + Pad 14, as printed; round 2, time-correct C
// "strength, heard live"): no commit until APPLY. The selected track's pattern plays as
// close to its Q as STRENGTH says (K2), the second of each pair late by SWING
// (K3), as they turn: the right amount is found by ear and taken back by
// turning down (the pattern's feel, model/pattern.h: the notes stay as
// written). K1 Q (the pattern's, as on Patterns). Shift + K1 HUMANIZE (each
// note early or late), Shift + K2 its velocity, Shift + K3 the groove's
// amount; the encoder takes the groove from a pattern of the track that holds
// notes (none first); the pads play the track's sounds. B3 QUANTIZE, lit while the feel is heard, takes it off to compare by
// ear and puts it back; B2 ALL lays this feel on the track's other patterns;
// B1 APPLY (issue 55), while a feel is heard, writes it into the notes
// (model/pattern.h apply_feel: they move to where they play, the feel back
// to plain, one undo step), so Roll shows them on the grid. Picture: two beats of the pattern over the grid of Q (beats
// taller, the swung steps in a shade where they land), each note where it is
// written (a ring) and where it plays (a dot), joined; under it the track's
// patterns, the groove's lit.
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/model/print.h"
#include "melodee/shell/groove_pick.h"
#include <algorithm>
#include <cmath>

namespace melodee {
namespace {
using namespace ui;

void draw(Paint& p, const Snapshot& s) {
    auto c = colour_of(s);
    const auto& pattern = s.pattern;
    const auto& f = pattern.feel;
    bool heard = !f.plain();
    Header h = header_of(s);
    h.left.push_back(pill("TIME CORRECT"));
    h.left.push_back(pill("P" + juce::String(s.track().pattern + 1), c));
    h.right.push_back(tag_of(s, s.ui.track));
    SoftKeys keys {{heard ? "APPLY" : "", "ALL", "QUANTIZE"}, -1, c}; // Shift + Pad 14 again closes (T-a)
    keys.toggles = 4;
    keys.lit = heard ? 4 : 0;
    frame(p, s, keys, h);

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
    for (const auto& e : pattern.events) {
        if (e.tick >= span) break;
        double t = e.tick, step = std::floor(t / q + 0.5), target = step * q + (std::fmod(step, 2.0) == 1.0 ? late : 0.0);
        double played = t + (target - t) * f.strength / 100.0;
        float a = x_of(t), b = x_of(played);
        if (std::fabs(b - a) > 1) p.fill(std::min(a, b), line_y - 1, std::fabs(b - a), 2, c.withAlpha(0.35f), 1);
        p.ring(a - 3, line_y - 3, 6, 6, c.withAlpha(0.6f), 3, 1.2f);
        p.circle(b, line_y, 5, c);
    }
    p.text("2 BEATS", x0, line_y + 26, 80, 13, caps_text(colour::mid));

    // The encoder takes the groove from a pattern of the track (none first): the bank of the groove's pattern (else
    // of the one in hand) as a row, the groove's lit, the one in hand ringed; its name beside GROOVE.
    float gy = zone::knobs_y - 32, cw = (w - 15 * 3) / 16;
    int groove = f.groove_from && groove_track(f.groove_from) == s.ui.track % track_count ? int(groove_pattern(f.groove_from)) : -1;
    const unsigned row_bank = (groove >= 0 ? unsigned(groove) : s.track().pattern % pattern_count) / pattern_pads;
    icon(p, Icon::Bars, x0, gy - 18, 12, colour::mid, 1.4f);
    const juce::String named = !f.groove_from ? juce::String("NONE")
        : groove >= 0 ? juce::String(print_pattern_name(unsigned(groove))) : "T" + juce::String(groove_track(f.groove_from) + 1) + " " + juce::String(print_pattern_name(groove_pattern(f.groove_from)));
    p.text("GROOVE  " + named, x0 + 16, gy - 18, 160, 12, caps_text(f.groove_from ? c : colour::mid));
    for (unsigned k = 0; k < pattern_pads; ++k) {
        unsigned n = row_bank * pattern_pads + k;
        float x = x0 + k * (cw + 3);
        bool lit = int(n) == groove, here = n == s.track().pattern, notes = s.sketches[s.ui.track][n].notes;
        p.fill(x, gy, cw, 22, lit ? c : notes ? colour::surf : colour::quiet, 3);
        if (here) p.ring(x, gy, cw, 22, colour::text.withAlpha(0.6f), 3, 1);
        p.text(juce::String(k + 1), x, gy, cw, 22, plain(lit ? colour::bg : notes ? colour::sec : colour::dim, type::small, Weight::SemiBold),
               juce::Justification::centred);
    }
}

// The encoder: the groove from the next or previous pattern of the track that holds notes, none first (shell/groove_pick.h).
void wheel(Context& c, int delta) {
    if (!delta) return;
    const unsigned t = c.ui.track % track_count, here = c.track().pattern % pattern_count;
    std::array<uint64_t, 2> used {};
    for (unsigned n = 0; n < pattern_count; ++n) if (!c.patterns.at[t][n].empty()) used[n / 64] |= uint64_t(1) << (n % 64);
    const auto& f = c.pattern().feel;
    const uint16_t to = groove_step(used, t, here, f.groove_from, delta);
    if (to == f.groove_from) return;
    c.apply(Binding {Binding::Kind::GrooveFrom, 0, 0, int(track_count * pattern_count), f.groove_from}, to);
    c.notice(to ? "Groove from " + print_pattern_name(groove_pattern(to)) : std::string("No groove"));
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
    if (key == 2) { // QUANTIZE: the feel off to compare by ear, and back
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
        return;
    }
    // ALL: this feel on the track's other patterns, each its own undo.
    Feel feel = c.pattern().feel;
    unsigned t = ui.track % track_count, laid = 0;
    for (unsigned n = 0; n < pattern_count; ++n) {
        if (n == c.track().pattern || c.patterns.at[t][n].feel == feel) continue;
        c.edit_pattern(t, n, "Feel", [&] { c.patterns.at[t][n].feel = feel; c.sync_pattern(t, n); });
        ++laid;
    }
    c.notice(laid ? "Feel on all patterns" : "Every pattern has this feel");
}
} // namespace

const PageSpec time_correct_page {draw, play_pad, wheel, no_push, soft, [](Context&, int) {}}; // the pads play the track

} // namespace melodee
