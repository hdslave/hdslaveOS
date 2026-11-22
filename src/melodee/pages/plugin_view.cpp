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
#include "melodee/shell/features.h"
#include "melodee/shell/stage_map.h"
#include "melodee/ui/widgets.h"
#include <cstring>
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

bool plugin_screen_shown(const Snapshot& s, unsigned entry) {
    const auto& panel = s.ui.plugin_panel;
    return entry < max_plugins && panel.entry == entry && panel.status == PluginStatus::Ready && panel.screen;
}

bool plugin_screen_in_view(const Snapshot& s) {
    if (s.ui.fx_panel_entry != no_plugin) return plugin_screen_shown(s, s.ui.fx_panel_entry);
    const auto& t = s.track();
    if (t.engine != EngineKind::Plugin || !plugin_screen_shown(s, t.plugin)) return false;
    if (s.ui.page == PageId::Edit) return true;
    return s.ui.page == PageId::Stage && stage_page_now(s.ui, t, stage_skip(s.project, s.ui, t)).kind == StageKind::PluginEdit;
}

void draw_plugin_screen(Paint& p, const Snapshot& s, unsigned entry, const SoftKeys& keys) {
    static_assert(PluginScreenFrame::top == zone::soft_h && PluginScreenFrame::top + PluginScreenFrame::height == zone::height
                  && PluginScreenFrame::width == zone::width);
    constexpr int w = int(PluginScreenFrame::width), h = int(PluginScreenFrame::height), top = int(PluginScreenFrame::top);
    const auto& picture = s.plugin_screen;
    if (picture && picture->entry == entry && picture->pixels.size() >= size_t(w) * h) {
        // As an image, made again only when a new picture came. The screen thread's own (as Paint is), so static.
        static juce::Image image;
        static const PluginScreenFrame* from = nullptr;
        static uint32_t serial = 0;
        if (!image.isValid()) image = juce::Image(juce::Image::ARGB, w, h, false);
        if (from != picture.get() || serial != picture->serial) {
            juce::Image::BitmapData out(image, juce::Image::BitmapData::writeOnly); // 0xAARRGGBB, opaque: as the frame holds it
            for (int y = 0; y < h; ++y) std::memcpy(out.getLinePointer(y), picture->pixels.data() + size_t(y) * w, size_t(w) * 4);
            from = picture.get();
            serial = picture->serial;
        }
        p.g.drawImageAt(image, 0, top);
    } else {
        p.fill(0, float(top), float(w), float(h), colour::bg); // its first picture is on its way
    }
    soft_row(p, s, keys);
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
    // Six rows: the three in hand raised on one panel, ticked in their knobs' colours.
    constexpr float row_h = 23;
    unsigned hand = panel.row * 3u;
    int first_hand = -1;
    for (unsigned k = 0; k < panel.window.size(); ++k) if (panel.window_first + k == hand) { first_hand = int(k); break; }
    if (first_hand >= 0) p.fill(x, top + 2 + first_hand * row_h, w, 3 * row_h, colour::panel, 3);
    for (unsigned k = 0; k < panel.window.size(); ++k) {
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

} // namespace melodee
