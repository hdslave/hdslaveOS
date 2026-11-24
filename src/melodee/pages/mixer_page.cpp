// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The mixer (docs/MIXER_PLAN.md; Pad FX + Knob FX): the channels in a row, the selected one a wide card in the middle,
// its neighbours narrow strips either side (number, stereo meter, level). The card: name, mute and solo, a slim meter
// and its fader, and one block, the one B1-B3 light: SENDS (four sends and what each bus holds), EQ (nine bands as a
// curve, the three in hand lit; B2 again steps LOW, MID, HIGH) or COMP (its curve and what it took off of late).
// K1-K3 turn the lit block (Shift: its second row), the fader the level. A pad, the wheel or Minus / Plus pick the
// channel; Push turns the lit block's EQ or compressor on or off.
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/erase_copy.h"
#include "melodee/shell/features.h"
#include "melodee/shell/new_track.h"
#include <cmath>
#include <complex>

namespace melodee {
namespace {
using namespace ui;

// The row: the card in the middle, strips either side, the outer ones cut by the screen's edge.
constexpr float top = zone::picture_y + 3, bottom = zone::picture_y + zone::picture_h - 6;
constexpr float card_x = 75, card_w = 170, strip_w = 25, gap = 3;
constexpr float head_h = 18; // a strip's and the card's top
constexpr float block_x = card_x + 32, block_y = top + head_h + 5, block_w = card_x + card_w - 5 - block_x, block_h = bottom - block_y;

float meter_fraction(float linear) { return linear > 1e-5f ? std::clamp((20 * std::log10(linear) + 48) / 48, 0.0f, 1.0f) : 0.0f; }

// A meter of short segments, bottom up: lit to `fill`, the rest on the groove.
void segments(Paint& p, float x, float y, float w, float h, float fill, juce::Colour c) {
    constexpr float seg = 2, space = 1;
    const unsigned count = unsigned((h + space) / (seg + space));
    const unsigned lit = unsigned(std::lround(fill * float(count)));
    for (unsigned n = 0; n < count; ++n) {
        const float sy = y + h - float(n + 1) * (seg + space) + space;
        p.fill(x, sy, w, seg, n < lit ? c : colour::line);
    }
}

// A level's fader: the groove and the cap where the level sits (0..127, as the cards count it).
void fader(Paint& p, float x, float y, float h, int level, juce::Colour c) {
    p.fill(x, y, 2, h, colour::line, 1);
    const float at = y + h * (1 - std::clamp(level / 127.0f, 0.0f, 1.0f));
    p.fill(x, at, 2, y + h - at, c, 1);
    p.fill(x - 3, at - 2, 8, 4, colour::text, 1);
}

juce::String track_title(const Track& track) {
    if (track.name[0]) return name_case(track.name);
    return track.used() ? engine_word(track.engine) : juce::String("Empty");
}

// A neighbour: its number on its colour, its stereo meter and where its level sits.
void strip(Paint& p, const Snapshot& s, unsigned t, float x) {
    const auto& track = s.project.tracks[t];
    const auto c = track_colour(t);
    const bool used = track.used();
    p.fill(x, top, strip_w, bottom - top, colour::panel, 3);
    p.fill(x, top, strip_w, 2, used ? c : colour::line, 1);
    p.text(juce::String(t + 1), x, top + 2, strip_w, head_h - 2, plain(used ? c : colour::dim, type::body, Weight::Bold), juce::Justification::centred);
    if (!used) return;
    const auto& m = s.strips[t];
    const float my = top + head_h + 4, mh = bottom - my - 4;
    const bool muted = track.mix.mute;
    segments(p, x + 5, my, 4, mh, muted ? 0 : meter_fraction(m.left), c);
    segments(p, x + 10, my, 4, mh, muted ? 0 : meter_fraction(m.right), c);
    const float at = my + mh * (1 - std::clamp(track.mix.level / 127.0f, 0.0f, 1.0f));
    p.fill(x + 17, my, 1, mh, colour::line);
    p.fill(x + 15, at - 1.5f, 6, 3, colour::text, 1);
}

// The response of a strip's nine bands at `hz`, dB (model/strip.h; the engine's peaks, engine/rbj.h, at 48 kHz).
float eq_db(const Strip& strip, float hz) {
    if (!strip.eq_on) return 0;
    constexpr double fs = 48000, q = 1.4142;
    const std::complex<double> z1 = std::polar(1.0, -2 * juce::MathConstants<double>::pi * hz / fs), z2 = z1 * z1;
    std::complex<double> h = 1;
    for (unsigned n = 0; n < eq_bands; ++n) {
        if (!strip.eq[n]) continue;
        const double A = std::pow(10.0, strip.eq[n] / 10.0 / 40.0);
        const double w = 2 * juce::MathConstants<double>::pi * std::min(double(eq_band_hz[n]), 0.45 * fs) / fs, alpha = std::sin(w) / (2 * q), cw = std::cos(w);
        const double a0 = 1 + alpha / A;
        const double b0 = (1 + alpha * A) / a0, b1 = -2 * cw / a0, b2 = (1 - alpha * A) / a0, a1 = -2 * cw / a0, a2 = (1 - alpha / A) / a0;
        h *= (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2);
    }
    return float(20 * std::log10(std::max(std::abs(h), 1e-9)));
}

// SENDS: four columns, A-D: how much goes into each bus, and what the bus holds first.
void sends_block(Paint& p, const Snapshot& s, unsigned t) {
    const auto& track = s.project.tracks[t];
    const bool basic = !full_features(s.ui);
    const unsigned shown = basic ? 1 : bus_count;
    const float col = block_w / float(bus_count), bar_y = block_y + 22, bar_h = block_h - 56;
    for (unsigned b = 0; b < shown; ++b) {
        const float cx = block_x + col * (float(b) + 0.5f);
        const auto sc = b < 3 ? knob_shade(t, b) : colour::mid;
        const int amount = track.mix.sends[b];
        p.text(juce::String(amount), cx - col / 2, block_y + 4, col, 16, plain(colour::text, type::body, Weight::SemiBold), juce::Justification::centred);
        p.fill(cx - 4, bar_y, 8, bar_h, colour::line, 4);
        const float fill = bar_h * amount / 100.0f;
        if (amount > 0) p.fill(cx - 4, bar_y + bar_h - fill, 8, fill, sc, 4);
        p.text(juce::String::charToString(juce::juce_wchar('A' + b)), cx - col / 2, bar_y + bar_h + 2, col, 15, plain(sc, type::body, Weight::Bold), juce::Justification::centred);
        // What the bus holds first: its name where it fits the column, else its family's glyph (text is never cut).
        const auto& first = s.project.buses[b].fx.slots[0];
        const float ny = bar_y + bar_h + 17;
        auto style = plain(colour::mid, type::small, Weight::Medium);
        if (!first.used()) {
            p.text(juce::String::fromUTF8("\xe2\x80\x93"), cx - col / 2, ny, col, 13, style, juce::Justification::centred);
            continue;
        }
        const auto& info = effect_info(EffectKind(first.kind));
        const juce::String held = name_case(info.name);
        if (p.width(held, style) <= col - 2) p.text(held, cx - col / 2 + 1, ny, col - 2, 13, style, juce::Justification::centred);
        else icon(p, family_icon(info.family), cx - 6, ny, 12, colour::mid, 1.4f);
    }
}

// EQ: the nine bands as a curve over 40 Hz - 20 kHz, the group in hand shaded and its three dots in the knobs' shades.
void eq_block(Paint& p, const Snapshot& s, unsigned t) {
    const auto& strip = s.project.tracks[t].strip;
    const float x = block_x + 3, y = block_y + 4, w = block_w - 6, h = block_h - 20;
    const float lo = std::log10(40.0f), hi = std::log10(20000.0f);
    auto x_of = [&](float hz) { return x + w * (std::log10(hz) - lo) / (hi - lo); };
    auto y_of = [&](float db) { return y + h / 2 - std::clamp(db, -15.0f, 15.0f) / 15.0f * (h / 2); };
    const unsigned g = s.ui.mixer_eq_group % 3;
    const float gx0 = x_of(eq_band_hz[g * 3] / 1.414f), gx1 = x_of(eq_band_hz[g * 3 + 2] * 1.414f);
    p.fill(gx0, y, gx1 - gx0, h, knob_shade(t, 0).withAlpha(0.14f), 3);
    p.fill(x, y + h / 2, w, 1, colour::line);
    juce::Path curve;
    for (unsigned n = 0; n <= 96; ++n) {
        const float hz = std::pow(10.0f, lo + (hi - lo) * float(n) / 96);
        const float px = x_of(hz), py = y_of(eq_db(strip, hz));
        if (n == 0) curve.startNewSubPath(px, py); else curve.lineTo(px, py);
    }
    p.g.setColour(strip.eq_on ? colour::text : colour::dim);
    p.g.strokePath(curve, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    for (unsigned n = 0; n < eq_bands; ++n) {
        const float px = x_of(eq_band_hz[n]), py = y_of(eq_db(strip, eq_band_hz[n]));
        const bool hand = n / 3 == g;
        if (hand) {
            const auto c = knob_shade(t, n % 3);
            p.fill(px - 0.5f, std::min(py, y + h / 2), 1, std::fabs(py - (y + h / 2)), c);
            p.circle(px, py, 3.2f, c);
        } else {
            p.circle(px, py, 2.4f, colour::text);
            p.circle(px, py, 1.4f, colour::panel);
        }
    }
    auto label = plain(colour::mid, type::small, Weight::Medium);
    p.text("63", x, y + h + 2, 30, 13, label);
    p.text("1k", x_of(1000) - 15, y + h + 2, 30, 13, label, juce::Justification::centred);
    p.text("16k", x + w - 30, y + h + 2, 30, 13, label, juce::Justification::centredRight);
    if (!strip.eq_on) p.text("OFF", x + w - 40, y + 2, 38, 13, caps_text(colour::dim), juce::Justification::centredRight);
}

// COMP: its curve (in against out, the knee where AMOUNT sets it) and what it took off of late, bars hanging down.
void comp_block(Paint& p, const Snapshot& s, unsigned t) {
    const auto& strip = s.project.tracks[t].strip;
    const auto& trail = s.strip_trail;
    const float reduce = s.strips[t].reduction;
    const bool on = strip.comp_on && strip.comp_amount;
    p.text("GR", block_x + 4, block_y + 2, 30, 14, caps_text(colour::mid));
    const juce::String value = (reduce >= 0.05f ? juce::String::fromUTF8("\xe2\x88\x92") : juce::String()) + juce::String(reduce, 1);
    p.text(value + " dB", block_x + 30, block_y + 1, block_w - 34, 16, plain(on ? colour::text : colour::dim, type::body, Weight::SemiBold), juce::Justification::centredRight);
    // The curve: -48..0 dB in and out, the threshold at -AMOUNT, the ratio above it.
    const float cx = block_x + 4, cy = block_y + 20, cs = std::min(block_h - 26, 58.0f);
    p.fill(cx, cy, cs, cs, colour::panel, 2);
    const float threshold = -float(strip.comp_amount), slope = 1 - 10.0f / float(std::max<unsigned>(strip.comp_ratio, comp_ratio_min));
    auto out_of = [&](float in) {
        const float over = in - threshold;
        const float red = over <= -3 ? 0 : over >= 3 ? over * slope : slope * (over + 3) * (over + 3) / 12;
        return on ? in - red : in;
    };
    juce::Path curve;
    for (unsigned n = 0; n <= 48; ++n) {
        const float in = -48 + float(n), out = out_of(in);
        const float px = cx + cs * (in + 48) / 48, py = cy + cs * (1 - (out + 48) / 48);
        if (n == 0) curve.startNewSubPath(px, py); else curve.lineTo(px, py);
    }
    p.g.setColour(on ? knob_shade(t, 0) : colour::dim);
    p.g.strokePath(curve, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    const float heard = std::clamp(trail.back().level, -48.0f, 0.0f);
    if (on && trail.back().level > -90) p.circle(cx + cs * (heard + 48) / 48, cy + cs * (1 - (out_of(heard) + 48) / 48), 2.6f, colour::text);
    // Its reduction of late, newest on the right: a bar a column, 12 dB the bars' full depth.
    const float hx = cx + cs + 6, hw = block_x + block_w - 4 - hx, hh = cs;
    const unsigned shown = std::min<unsigned>(comp_columns, unsigned(hw / 2));
    for (unsigned n = 0; n < shown; ++n) {
        const float r = trail[comp_columns - shown + n].reduce;
        if (r < 0.05f) continue;
        p.fill(hx + float(n) * 2, cy, 1.4f, hh * std::min(r / 12.0f, 1.0f), colour::amber);
    }
    p.text(juce::String::fromUTF8("\xe2\x88\x92") + "12", hx, cy + hh + 1, 30, 13, plain(colour::mid, type::small, Weight::Medium));
    if (!on) p.text(strip.comp_on ? "AMOUNT 0" : "OFF", hx, cy + hh / 2 - 7, hw, 14, caps_text(colour::dim), juce::Justification::centred);
}

// The selected channel's card: its name on its colour with M and S, its meter and fader, its pan, the lit block.
void card(Paint& p, const Snapshot& s, unsigned t) {
    const auto& track = s.project.tracks[t];
    const auto c = track_colour(t);
    const bool used = track.used();
    p.fill(card_x, top, card_w, bottom - top + 5, colour::panel, 4);
    p.fill(card_x, top, card_w, head_h, used ? c : colour::raised, 4);
    p.fill(card_x, top + head_h - 4, card_w, 4, used ? c : colour::raised);
    const juce::String title = juce::String(t + 1) + "  " + track_title(track);
    auto head = plain(used ? colour::bg : colour::dim, type::body, Weight::Bold);
    const float pills = used ? 36 : 0;
    p.text(title, card_x + 6, top, card_w - 12 - pills, head_h, p.fit(title, head, card_w - 12 - pills, type::floor));
    if (!used) {
        p.text("Empty", card_x, top + head_h, card_w, bottom - top - head_h, plain(colour::dim, type::body, Weight::Medium), juce::Justification::centred);
        return;
    }
    // M and S: lit red and amber when on, as the MUTE key and Shift + pad set them.
    auto pill_at = [&](float x, const char* letter, bool lit, juce::Colour on) {
        p.fill(x, top + 3, 14, 12, lit ? on : colour::bg.withAlpha(0.25f), 2);
        p.text(letter, x, top + 3, 14, 12, plain(lit ? colour::bg : colour::bg.withAlpha(0.8f), type::small, Weight::Bold), juce::Justification::centred);
    };
    pill_at(card_x + card_w - 34, "M", track.mix.mute, colour::rec);
    pill_at(card_x + card_w - 18, "S", track.mix.solo, colour::amber);
    const auto& m = s.strips[t];
    const float my = top + head_h + 5, mh = bottom - my - 10;
    segments(p, card_x + 4, my, 4, mh, track.mix.mute ? 0 : meter_fraction(m.left), c);
    segments(p, card_x + 9, my, 4, mh, track.mix.mute ? 0 : meter_fraction(m.right), c);
    fader(p, card_x + 19, my, mh, track.mix.level, c);
    // Pan: a short line under them, the dot where it points.
    const float py = bottom - 3, px0 = card_x + 4, pw = 22;
    p.fill(px0, py - 0.5f, pw, 1, colour::line);
    p.circle(px0 + pw * (track.mix.pan + 50) / 100.0f, py, 2.2f, colour::text);
    p.fill(block_x, block_y, block_w, block_h, colour::surf, 3);
    if (!full_features(s.ui)) { sends_block(p, s, t); return; }
    switch (s.ui.mixer_block % 3) {
    case 1: eq_block(p, s, t); break;
    case 2: comp_block(p, s, t); break;
    default: sends_block(p, s, t); break;
    }
}

void draw(Paint& p, const Snapshot& s) {
    const unsigned t = s.ui.track % track_count;
    Header h = header_of(s);
    h.left.push_back(chip(Icon::Pads, "SELECT", colour_of(s)));
    h.right.push_back(label("TRACKS"));
    const bool basic = !full_features(s.ui);
    static const char* const groups[] {"EQ LOW", "EQ MID", "EQ HIGH"};
    const unsigned block = basic ? 0 : s.ui.mixer_block % 3;
    SoftKeys keys {{"SENDS", basic ? "" : block == 1 ? groups[s.ui.mixer_eq_group % 3] : "EQ", basic ? "" : "COMP"}, int(block), colour_of(s)};
    if (!basic) { keys.dots[1] = 3; keys.dot_at[1] = uint8_t(s.ui.mixer_eq_group % 3); }
    frame(p, s, keys, h);
    // The neighbours, three a side, nearest the card first; then the card over them.
    for (int k = 1; k <= 3; ++k) {
        if (int(t) - k >= 0) strip(p, s, unsigned(int(t) - k), card_x - float(k) * (strip_w + gap));
        if (t + unsigned(k) < track_count) strip(p, s, t + unsigned(k), card_x + card_w + gap + float(k - 1) * (strip_w + gap));
    }
    card(p, s, t);
}

void pad(Context& c, unsigned pad, uint8_t, bool down) {
    if (!down) return;
    c.select_track(pad % track_count);
    if (c.ui.erase) erase_tap(c); // Erase + pad: that track cleared, once B3 says so (as on Tracks)
    else offer_new_track(c);      // an empty one: its doors, then back here
}

void wheel(Context& c, int delta) { c.select_track(unsigned(std::clamp(int(c.ui.track) + delta, 0, int(track_count) - 1))); }

// Push: the lit block's on and off (EQ, the compressor); SENDS has none.
void push(Context& c) {
    if (!c.track().used()) { offer_new_track(c); return; }
    const unsigned t = c.ui.track % track_count;
    const unsigned block = c.ui.mixer_block % 3;
    if (block == 0 || !full_features(c.ui)) return;
    const auto part = block == 1 ? StripPart::EqOn : StripPart::CompOn;
    c.apply(strip_card(t, part, c.track().strip), strip_get(c.track().strip, part) ? 0 : 1);
}

// B1 SENDS, B2 EQ (again: the next three bands), B3 COMP.
void soft(Context& c, int key) {
    if (!full_features(c.ui)) { c.ui.mixer_block = 0; return; }
    const unsigned k = unsigned(key) % 3;
    if (k == 1 && c.ui.mixer_block == 1) c.ui.mixer_eq_group = uint8_t((c.ui.mixer_eq_group + 1) % 3);
    c.ui.mixer_block = uint8_t(k);
}
} // namespace

const PageSpec mixer_page {draw, pad, wheel, push, soft, wheel}; // Minus / Plus: the channel, as the wheel

} // namespace melodee
