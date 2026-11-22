// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// A plug-in over the cards (docs/MELODEE.md, Plug-ins), for Edit on a PLUGIN
// track: its name, then while it opens the plug glyph, if it could not the
// broken plug and why in two words, if its 32-bit bridge died STOPPED; once
// open, its parameters as a list, each name on one full line with its
// number: the three on K1-K3 raised and ticked in their colours (their values
// on the cards), the row before (or after) with its values; the wheel or
// Minus / Plus move the rows. The share of each block it takes is the
// header's (pages/edit_page.cpp).
#include "melodee/pages/common.h"
#include "melodee/pages/sampler_common.h"
#include "melodee/plugin/plugin_pages.h"
#include "melodee/ui/knobs.h"
#include "melodee/ui/widgets.h"
#include <cmath>
#include <filesystem>

namespace melodee {
using namespace ui;

juce::String plugin_name(const Snapshot& s, unsigned entry) {
    if (entry >= max_plugins) return "Plug-in";
    const auto& panel = s.ui.plugin_panel;
    if (panel.entry == entry && panel.name[0]) return juce::String::fromUTF8(panel.name);
    const auto& ref = s.project.plugins[entry];
    if (!ref.used()) return "Plug-in";
    if (ref.name[0]) return juce::String::fromUTF8(ref.name); // as the picker named it
    return juce::String(std::filesystem::path(ref.path).stem().string());
}

void draw_plugin_view(Paint& p, const Snapshot& s, unsigned entry, float top) {
    const auto& panel = s.ui.plugin_panel;
    const float x = zone::pad, w = zone::width - 2 * zone::pad;
    bool ours = panel.entry == entry;
    if (!ours || panel.status == PluginStatus::None || panel.status == PluginStatus::Loading) { // the plug, waiting
        p.text(plugin_name(s, entry), x, top + 4, w, 22, p.fit(plugin_name(s, entry), plain(colour::text, type::title, Weight::SemiBold), w, type::floor));
        icon(p, Icon::Plug, x + w / 2 - 14, top + 40, 28, colour::dim, 1.4f);
        p.text("OPENING", x, top + 74, w, 14, caps_text(colour::dim), juce::Justification::centred);
        return;
    }
    if (panel.status == PluginStatus::Failed || panel.status == PluginStatus::Crashed) {
        // The broken plug, the cause in two words, and (dim, for whoever fixes it) what the loader named.
        p.text(plugin_name(s, entry), x, top + 4, w, 22, p.fit(plugin_name(s, entry), plain(colour::text, type::title, Weight::SemiBold), w, type::floor));
        bool crashed = panel.status == PluginStatus::Crashed;
        auto error = juce::String::fromUTF8(panel.error);
        bool library = error.contains("shared object") || error.contains(".so");
        juce::String cause = crashed ? "STOPPED" : library ? "MISSING LIBRARY" : "WON'T OPEN";
        icon(p, Icon::PlugBroken, x + 2, top + 40, 40, colour::rec, 1.3f);
        icon(p, Icon::Warn, x + 56, top + 44, 13, colour::rec, 1.6f);
        p.text(cause, x + 74, top + 42, w - 74, 16, caps_text(colour::rec, type::body));
        juce::String code = crashed ? juce::String() : library ? error.upToFirstOccurrenceOf(":", false, false).trim() : error;
        if (code.isNotEmpty()) p.text(code, x + 56, top + 62, w - 56, 14, p.fit(code, plain(colour::dim, type::small), w - 56, type::floor));
        return;
    }
    if (!panel.rows) {
        icon(p, Icon::Sliders, x + w / 2 - 14, top + 40, 28, colour::dim, 1.4f);
        p.text("NONE", x, top + 74, w, 14, caps_text(colour::dim), juce::Justification::centred);
        return;
    }
    // The window's rows (two of the page in hand): the three in hand raised on one panel, ticked in their knobs' colours.
    constexpr float row_h = 23;
    unsigned hand = panel.row * 3u;
    int first_hand = -1;
    for (unsigned k = 0; k < panel.window_rows * 3u; ++k) if (panel.window_first + k == hand) { first_hand = int(k); break; }
    if (first_hand >= 0) p.fill(x, top + 2 + first_hand * row_h, w, 3 * row_h, colour::panel, 3);
    for (unsigned k = 0; k < panel.window_rows * 3u; ++k) {
        const auto& param = panel.window[k];
        if (!param.used) continue;
        unsigned at = panel.window_first + k;
        bool held = at >= hand && at < hand + 3;
        float y = top + 2 + k * row_h;
        if (held) p.fill(x + 4, y + 5, 2.5f, row_h - 10, knob_shade(s.ui.track, at - hand), 1);
        p.text(juce::String(at + 1), x + 6, y, 22, row_h, plain(colour::dim, type::small, Weight::SemiBold), juce::Justification::centredRight);
        juce::String name = juce::String::fromUTF8(param.name), value = juce::String::fromUTF8(param.text);
        auto value_style = plain(colour::mid, type::body, Weight::SemiBold);
        float vw = held ? 0 : p.width(value, value_style) + 10;
        auto name_style = plain(held ? colour::text : colour::mid, type::body, held ? Weight::Medium : Weight::Regular);
        p.text(name, x + 34, y, w - 42 - vw, row_h, p.fit(name, name_style, w - 42 - vw, type::floor));
        if (!held) p.text(value, x + w - 8 - vw + 10, y, vw - 10, row_h, value_style, juce::Justification::centredRight);
    }
}

// --- The page under B1-B3 (docs/PLUGIN_PAGES_PLAN.md, picks 3 c and 4 a) ---

namespace {
constexpr float cols[3] {58, 160, 262}; // each column over its pot
constexpr float arc_r = 20, sweep = 0.75f * juce::MathConstants<float>::pi; // 270 degrees, the gap at the bottom

// The page line: the page's name (the plug-in's where its page has none), and where it is among the pages.
void page_line(Paint& p, const Snapshot& s, unsigned entry, float y) {
    const auto& panel = s.ui.plugin_panel;
    auto c = colour_of(s);
    juce::String name = panel.page_name[0] ? juce::String::fromUTF8(panel.page_name) : plugin_name(s, entry);
    p.text(name.toUpperCase(), zone::pad, y, 220, 14, p.fit(name.toUpperCase(), caps_text(c), 220, type::floor));
    if (panel.pages > 24) // too many for dots: the count
        p.text(juce::String(panel.page + 1) + " / " + juce::String(panel.pages), zone::width - zone::pad - 80, y, 80, 14,
               plain(colour::mid, type::small, Weight::SemiBold), juce::Justification::centredRight);
    else if (panel.pages > 1)
        page_dots(p, zone::width - zone::pad, y + 7, int(panel.pages), int(panel.page), c, true);
}

juce::Path arc_between(float cx, float cy, float from, float to) {
    juce::Path path;
    path.addCentredArc(cx, cy, arc_r, arc_r, 0, from, to, true);
    return path;
}

// One parameter over its pot: the arc (a choice of a few values: its values as dots), its value, its name. In hand,
// the knob's colour and what its card showed: faint with a white dot where the pot sits until the pot meets the value
// (takeover), the fader glyph, the Shift mark, the snowflake of a frozen track.
void arc_cell(Paint& p, float cx, float top, const PluginPanelParam& param, bool hand, const ui::KnobCard* card, juce::Colour shade) {
    if (!param.used) return;
    const float cy = top + 28;
    const bool pickup = hand && card && card->state == ui::KnobCard::State::Pickup;
    const bool turning = hand && card && card->state == ui::KnobCard::State::Turning;
    juce::Colour c = hand ? (pickup ? shade.withAlpha(0.45f) : shade) : colour::mid;
    auto stroke = [&](const juce::Path& path, juce::Colour colour, float w) {
        p.g.setColour(colour);
        p.g.strokePath(path, juce::PathStrokeType(w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    };
    stroke(arc_between(cx, cy, -sweep, sweep), colour::line, 3);
    float v = std::clamp(param.value / float(plugin_param_max), 0.0f, 1.0f);
    auto point = [&](float at, float r) { float a = -sweep + 2 * sweep * at; return juce::Point<float>(cx + r * std::sin(a), cy - r * std::cos(a)); };
    if (param.steps >= 2 && param.steps <= 12) { // its values as dots, the one it is on in the colour
        unsigned now = unsigned(std::lround(v * float(param.steps - 1)));
        for (unsigned n = 0; n < param.steps; ++n) {
            auto at = point(float(n) / float(param.steps - 1), arc_r);
            p.circle(at.x, at.y, n == now ? 3.2f : 2.0f, n == now ? c : colour::mid);
        }
    } else if (v > 0.002f) {
        stroke(arc_between(cx, cy, -sweep, -sweep + 2 * sweep * v), c, turning ? 4.0f : 3.0f);
    }
    if (pickup && card->pot >= 0) {
        auto at = point(std::clamp(card->pot, 0.0f, 1.0f), arc_r);
        p.circle(at.x, at.y, 3.2f, colour::text);
        p.circle(at.x, at.y, 1.9f, colour::bg);
    }
    if (hand && card) {
        if (card->fader) icon(p, Icon::Fader, cx + 22, top + 2, 10, pickup ? colour::mid : shade, 1.4f);
        if (card->shift) shift_mark(p, cx - 32, top + 4, card->shift == 2 ? shade : colour::mid, card->shift == 2);
        if (card->frozen) icon(p, Icon::Snow, cx + 22, top + 14, 10, colour::ice, 1.4f);
    }
    juce::String value = juce::String::fromUTF8(param.text);
    auto value_style = plain(hand ? (pickup ? colour::dim : colour::text) : colour::mid, type::list, Weight::Medium);
    p.text(value, cx - 50, top + 50, 100, 18, p.fit(value, value_style, 100, type::floor), juce::Justification::centred);
    juce::String name = juce::String::fromUTF8(param.name);
    auto name_style = plain(hand ? c : colour::dim, type::small, Weight::SemiBold);
    if (pickup) name_style = plain(shade, type::small, Weight::SemiBold);
    auto lines = p.wrap(name, name_style, 100, 2);
    if (lines.size() <= 1) p.text(name, cx - 50, top + 69, 100, 13, name_style, juce::Justification::centred);
    else for (int n = 0; n < 2; ++n) p.text(lines[n], cx - 50, top + 64 + 12 * n, 100, 12, name_style, juce::Justification::centred);
}

// A row of three (its slots row * 3 .. + 2) at `top`; the row in hand raised, with its cards' states.
void arc_row(Paint& p, const Snapshot& s, unsigned row, float top, const std::array<ui::KnobCard, 3>& cards) {
    const auto& panel = s.ui.plugin_panel;
    bool hand = row == panel.row;
    if (hand) p.fill(zone::pad, top, zone::width - 2 * zone::pad, 90, colour::panel, 4);
    for (unsigned k = 0; k < 3; ++k) {
        unsigned at = row * 3 + k;
        if (at < panel.window_first || at >= panel.window_first + panel.window_rows * 3u) continue;
        const auto& param = panel.window[at - panel.window_first];
        arc_cell(p, cols[k], top, param, hand, hand ? &cards[k] : nullptr, knob_shade(s.ui.track, k));
    }
}

// The colour a picture's part takes: its knob's while its row is in hand, else grey.
juce::Colour part_colour(const Snapshot& s, int slot) {
    const auto& panel = s.ui.plugin_panel;
    if (slot < 0 || unsigned(slot) / 3 != panel.row) return colour::mid;
    return knob_shade(s.ui.track, unsigned(slot) % 3);
}

// Times from the value's text where it names them ("12 ms"), else from where it sits (0..1, squared, up to 4 s).
double seconds_of(const PluginPanelParam& param) {
    double out = 0;
    if (value_in(param.text, false, out)) return std::clamp(out, 0.0, 30.0);
    double v = param.value / double(plugin_param_max);
    return v * v * 4.0;
}

void envelope_picture(Paint& p, const Snapshot& s, float x, float y, float w, float h) {
    const auto& panel = s.ui.plugin_panel;
    double a = seconds_of(panel.picture_params[0]), d = seconds_of(panel.picture_params[1]), r = seconds_of(panel.picture_params[3]);
    float sustain = std::clamp(panel.picture_params[2].value / float(plugin_param_max), 0.0f, 1.0f);
    double hold = std::max(0.2, 0.3 * (a + d + r)), length = std::max(a + d + hold + r, 0.01);
    auto x_of = [&](double t) { return x + float(t / length) * w; };
    float top = y + 2, base = y + h - 2;
    auto y_of = [&](float l) { return base - l * (base - top); };
    struct Part { double t0, t1; float l0, l1; int slot; };
    std::array<Part, 4> parts {Part {0, a, 0, 1, panel.picture_slots[0]}, Part {a, a + d, 1, sustain, panel.picture_slots[1]},
                               Part {a + d, a + d + hold, sustain, sustain, panel.picture_slots[2]},
                               Part {a + d + hold, length, sustain, 0, panel.picture_slots[3]}};
    juce::Path area;
    area.startNewSubPath(x_of(0), base);
    for (const auto& part : parts) area.lineTo(x_of(part.t1), y_of(part.l1));
    area.closeSubPath();
    p.g.setColour(colour_of(s).withAlpha(0.1f));
    p.g.fillPath(area);
    for (const auto& part : parts) {
        auto c = part_colour(s, part.slot);
        p.g.setColour(c);
        p.g.drawLine(x_of(part.t0), y_of(part.l0), x_of(part.t1), y_of(part.l1), 2);
        if (c != colour::mid) { p.circle(x_of(part.t1), y_of(part.l1), 3, c); p.circle(x_of(part.t1), y_of(part.l1), 1.6f, colour::bg); }
    }
}

void filter_picture(Paint& p, const Snapshot& s, float x, float y, float w, float h) {
    const auto& panel = s.ui.plugin_panel;
    double hz = 0;
    if (!value_in(panel.picture_params[0].text, true, hz))
        hz = 20.0 * std::pow(1000.0, panel.picture_params[0].value / double(plugin_param_max));
    float q = 0.7f + 5.0f * std::clamp(panel.picture_params[1].value / float(plugin_param_max), 0.0f, 1.0f);
    draw_filter_curve(p, x, y, w, h, 0, 2, float(std::clamp(hz, 20.0, 20000.0)), q, colour_of(s), part_colour(s, panel.picture_slots[0]), true);
}
} // namespace

bool plugin_page_shown(const Snapshot& s, unsigned entry) {
    const auto& panel = s.ui.plugin_panel;
    if (entry >= max_plugins || panel.entry != entry || panel.status != PluginStatus::Ready || !panel.rows) return false;
    auto cards = cards_of(s);
    return std::none_of(cards.begin(), cards.end(), [](const ui::KnobCard& c) { return c.knob_fx; });
}

void draw_plugin_page(Paint& p, const Snapshot& s, unsigned entry, const SoftKeys& keys) {
    frame_keys(p, s, keys);
    const auto& panel = s.ui.plugin_panel;
    auto cards = cards_of(s);
    if (auto* seen = controls_sink()) { // the preview's controls audit: the arcs are this page's pots
        seen->knobs_drawn = true;
        for (unsigned k = 0; k < 3; ++k) seen->knobs[k] = panel.params[k].used ? juce::String::fromUTF8(panel.params[k].name) : juce::String();
    }
    page_line(p, s, entry, 26);
    if (panel.picture == uint8_t(PagePicture::Envelope)) {
        envelope_picture(p, s, 14, 46, 292, 80);
        arc_row(p, s, panel.row, 132, cards);
    } else if (panel.picture == uint8_t(PagePicture::Filter)) {
        filter_picture(p, s, 14, 46, 292, 62);
        arc_row(p, s, panel.row, 132, cards);
    } else {
        for (unsigned r = 0; r < panel.window_rows; ++r) arc_row(p, s, panel.window_first / 3u + r, 44 + 96 * float(r), cards);
    }
}

// An effect in a chain: its page line, then its output as it plays (left over right, a block a bar) and its levels in
// and out.
void draw_plugin_sound(Paint& p, const Snapshot& s, float x, float y, float w, float h) {
    const auto& panel = s.ui.plugin_panel;
    auto c = colour_of(s);
    juce::String name = panel.page_name[0] ? juce::String::fromUTF8(panel.page_name).toUpperCase() : juce::String();
    if (name.isNotEmpty()) p.text(name, x, y, w - 90, 14, p.fit(name, caps_text(c), w - 90, type::floor));
    if (panel.pages > 1 && panel.pages <= 24) page_dots(p, x + w, y + 7, int(panel.pages), int(panel.page), c, true);
    float sy = y + 18, lane = std::max(6.0f, (h - 18 - 4) / 2), sw = w - 96;
    if (h < 30 || !panel.measured) return;
    for (int side = 0; side < 2; ++side) {
        float mid = sy + side * (lane + 4) + lane / 2;
        p.fill(x, mid - 0.5f, sw, 1, colour::line);
        float step = sw / float(panel.scope.size());
        for (size_t n = 0; n < panel.scope.size(); ++n) {
            uint32_t v = panel.scope[n] >> (side * 16);
            float lo = int8_t(v & 0xff) / 127.0f, hi = int8_t((v >> 8) & 0xff) / 127.0f;
            if (hi - lo < 0.01f) continue;
            p.fill(x + n * step + 0.4f, mid - hi * lane / 2, std::max(1.2f, step - 1.2f), (hi - lo) * lane / 2,
                   c.withAlpha(0.35f + 0.65f * float(n) / float(panel.scope.size())), 0.8f);
        }
    }
    float mx = x + sw + 14, mw = w - sw - 14;
    for (int n = 0; n < 2; ++n) {
        float my = sy + n * 20;
        p.text(n ? "OUT" : "IN", mx, my, 30, 14, caps_text(colour::dim));
        float level = (n ? panel.level_out : panel.level_in) / 1000.0f;
        p.fill(mx + 30, my + 5, mw - 30, 4, colour::line, 2);
        if (level > 0.001f) p.fill(mx + 30, my + 5, (mw - 30) * std::min(1.0f, level), 4, n ? c : colour::mid, 2);
    }
}

} // namespace melodee

