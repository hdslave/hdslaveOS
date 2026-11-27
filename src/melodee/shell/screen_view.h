// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Which plug-in's own screen (plugin/plugin_screen.h, docs/PLUGIN_SCREENS_PLAN.md) the page in view shows, and as
// what: its page under B1-B3 (Stage's PANEL on its PLUGIN track; an effect slot's full page, Push on it), or an
// effect's strip under its chain's slots. Settings > SCREEN > Plug-in
// screens off: none. Pure, for the shell (what the rack is told) and the pages (what is drawn), so both agree. PANEL
// is a PLUGIN track's first page on B1 (core engine_info.cpp), and a plug-in picked from Stage moves to it once it is
// open (plugin_screen_first()).
#pragma once
#include "melodee/shell/bindings.h"
#include "melodee/shell/features.h"
#include "melodee/shell/stage_map.h"
#include "melodee/shell/ui_state.h"

namespace melodee {

constexpr bool fx_chain_page(PageId page) { return page == PageId::Rack || page == PageId::Bus || page == PageId::Master; }

struct ScreenPlace {
    int entry = -1;   // Project::plugins, -1 none
    uint8_t area = 0; // 0 its page under B1-B3, 1 its strip under a chain's slots
};

inline ScreenPlace plugin_screen_place(const Project& project, const UiState& ui) {
    const auto& panel = ui.plugin_panel;
    if (!ui.settings.plugin_skins || !panel.has_screen || panel.status != PluginStatus::Ready) return {};
    const auto& t = project.tracks[ui.track % track_count];
    const bool own = t.engine == EngineKind::Plugin && t.plugin == panel.entry;
    if (ui.page == PageId::Stage)
        return own && stage_page_now(ui, t, stage_skip(project, ui, t)).kind == StageKind::PluginScreen ? ScreenPlace {t.plugin, 0} : ScreenPlace {};
    if (fx_chain_page(ui.page) && !ui.picker_open && !ui.sheet_open) {
        const unsigned n = page_chain(project, ui), k = ui.fx_cursor[UiState::cursor_of(n)];
        const auto& chain = project.chain(n);
        if (k < chain.count() && chain.slots[k].kind == plugin_effect_kind && !chain.slots[k].bypass
            && unsigned(chain.slots[k].params[0]) == panel.entry)
            return {int(panel.entry), uint8_t(ui.fx_panel ? 0 : 1)};
    }
    return {};
}

// A plug-in just picked onto the track from Stage (Context::choose_plugin sets ui.plugin_screen_due) opens on EDIT;
// once it is open, one that draws its own screen moves to PANEL, its first page, unless another page was chosen
// meanwhile. Awaited no longer once it is open or failed, or once the track or page changed. After each panel refresh.
inline void plugin_screen_first(const Project& project, UiState& ui) {
    const unsigned due = ui.plugin_screen_due;
    if (due == no_plugin) return;
    const auto& t = project.tracks[ui.track % track_count];
    if (ui.page != PageId::Stage || t.engine != EngineKind::Plugin || t.plugin != due) {
        ui.plugin_screen_due = no_plugin;
        return;
    }
    const auto& panel = ui.plugin_panel;
    if (panel.entry != due || panel.status == PluginStatus::Loading) return;
    ui.plugin_screen_due = no_plugin;
    if (plugin_screen_staged(ui, t) && stage_page_now(ui, t, stage_skip(project, ui, t)).kind == StageKind::PluginEdit)
        stage_go(ui, EngineKind::Plugin, StageKind::PluginScreen);
}

} // namespace melodee
