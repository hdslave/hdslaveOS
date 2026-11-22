// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Plug-ins on the MIDI thread (docs/MELODEE.md, Plug-ins): the PLUG-INS picker
// opened for a PLUGIN track's instrument or for a chain's slot, Push on a
// plug-in there, PRESETS (its programs) and a pick there, the cards of the
// plug-in in view, and the rack kept in step with the project
// (plugin/plugin_rack.h does the loading on its own thread).
//
// A plug-in is an entry of Project::plugins: its path on the card. A PLUGIN
// track names its entry; a Plugin effect names it in its first parameter. A
// track or slot that already has one keeps its entry for the new plug-in; a
// new one takes an entry nothing uses. The rack loads what is used and lets go
// of the rest, so undo, a project switch or a removed effect need nothing more.
#include "melodee/shell/context.h"
#include "melodee/shell/ask.h"
#include "melodee/shell/delete_jobs.h"
#include "melodee/engine/effect_info.h"
#include "melodee/engine/engine_host.h"
#include "melodee/model/scales.h"
#include "melodee/plugin/plugin_files.h"
#include "melodee/plugin/plugin_rack.h"
#include "melodee/plugin/preset_rows.h"
#include "melodee/shell/sample_library.h"
#include "melodee/shell/features.h"
#include "melodee/shell/stage_map.h"
#include "melodee/shell/track_copy.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

namespace melodee {
using Type = Command::Type;

namespace {
// An entry for a new plug-in: one never set first, else one nothing uses now.
int free_entry(const Project& project) {
    for (unsigned e = 0; e < max_plugins; ++e) if (!project.plugins[e].used()) return int(e);
    for (unsigned e = 0; e < max_plugins; ++e) if (!plugin_used_by(project, e)) return int(e);
    return -1;
}

bool chain_page(PageId page) { return page == PageId::Rack || page == PageId::Bus || page == PageId::Master; }
} // namespace

// One instance plays one place: a track copy (or a copied chain) leaves two users on an entry, and the second and
// after each get an entry of their own with the same plug-in and its state as it is now.
void Context::split_shared_plugins() {
    std::array<bool, max_plugins> seen {};
    auto claim = [&](unsigned entry) -> int { // -1: keep it; else the entry to move to
        if (entry >= max_plugins) return -1;
        if (!seen[entry]) { seen[entry] = true; return -1; }
        int copy = free_entry(project);
        if (copy < 0 || seen[size_t(copy)]) return -1; // none free: it stays shared (and is heard twice)
        project.plugins[size_t(copy)] = project.plugins[entry];
        seen[size_t(copy)] = true;
        if (plugins) plugins->clone(entry, unsigned(copy));
        return copy;
    };
    for (unsigned t = 0; t < track_count; ++t) {
        auto& tr = project.tracks[t];
        if (tr.engine != EngineKind::Plugin) continue;
        int copy = claim(tr.plugin);
        if (copy < 0) continue;
        tr.plugin = uint8_t(copy);
        send({Type::TrackPlugin, uint8_t(t), 0, 0, int16_t(copy)});
    }
    for (unsigned n = 0; n < chain_count; ++n)
        for (unsigned k = 0; k < project.chain(n).size; ++k) {
            const auto& s = project.chain(n).slots[k];
            if (s.kind != plugin_effect_kind) continue;
            int copy = claim(unsigned(s.params[0]));
            if (copy >= 0) set_effect_param(n, k, 0, copy);
        }
}

void Context::open_plugin_picker(UiState::PluginTarget target, unsigned chain, unsigned slot) {
    if (!plugins) { notice("Plug-ins are not available"); return; }
    ui.plugin_target = target;
    ui.plugin_chain = uint8_t(chain);
    ui.plugin_slot = uint8_t(slot);
    ui.plugin_back = target == UiState::PluginTarget::Instrument ? PageId::Edit : ui.page;
    plugins->browse(plugins->view().folder); // where the picker was left, listed afresh, on the row it was on
    if (ui.page != PageId::Plugins) ui.plugin_from = ui.page;
    open(PageId::Plugins);
}

void Context::choose_plugin(const PluginFile& file) {
    if (!plugins) return;
    if (file.folder || file.several()) { plugins->browse(file.path); ui.plugins_cursor = 0; return; } // a folder, or a bundle's plug-ins
    if (!file.loadable()) { notice(file.why.empty() ? "Not a plug-in for this MPC" : file.why); return; }
    bool instrument = ui.plugin_target == UiState::PluginTarget::Instrument;
    if (instrument && file.instrument == 0) { notice(file.name + " is an effect"); return; }
    if (!instrument && file.instrument == 1) { notice(file.name + " is an instrument"); return; }
    if (file.path.size() + 1 > plugin_path_length) { notice("Its path is too long"); return; }

    int entry = -1;
    unsigned n = ui.plugin_chain, k = ui.plugin_slot;
    if (instrument) {
        const auto& t = track();
        if (t.engine == EngineKind::Plugin && t.plugin < max_plugins) entry = t.plugin;
    } else {
        if (n >= chain_count || k >= project.chain(n).size) return;
        const auto& s = project.chain(n).slots[k];
        if (s.kind == plugin_effect_kind && unsigned(s.params[0]) < max_plugins) entry = s.params[0];
    }
    bool replace_asked = ui.plugin_replace;
    ui.plugin_replace = false;
    if (entry >= 0 && project.plugins[size_t(entry)].used() && !replace_asked
        && !(file.path == project.plugins[size_t(entry)].path && file.id == project.plugins[size_t(entry)].id)) {
        // Another plug-in over one in use: its sound and settings go (undo brings them back); ask first
        std::snprintf(ui.ask_name.data(), ui.ask_name.size(), "%s", project.plugins[size_t(entry)].name);
        ask(*this, UiState::Ask::ReplacePlugin, int(ui.plugins_cursor));
        return;
    }
    if (entry < 0) entry = free_entry(project);
    if (entry < 0) { notice("Every plug-in entry is in use"); return; }

    close_gesture();
    // An empty track turned PLUGIN: one undo step, as the carousel's KIT and MIDI ("New track"; shell/new_track.h).
    std::unique_ptr<Edit> made;
    if (instrument && !track().used()) {
        made = std::make_unique<Edit>(begin_edit("New track", ui.track, -1, false));
        made->motion_before.reset();
        made->track_before = std::make_unique<TrackState>(track_state(*this, ui.track));
    }
    auto& ref = project.plugins[size_t(entry)];
    bool same = file.path == ref.path && file.id == ref.id;
    // Another plug-in over one in use: one undo step, the old plug-in back with its state (swap_plugin); on a
    // track, its name and pads too.
    if (!made && !same && ref.used()) {
        made = std::make_unique<Edit>(begin_edit((std::string("Replace ") + ref.name).c_str(), ui.track, -1, false));
        made->motion_before.reset();
        made->plugin = std::make_unique<PluginSwap>();
        made->plugin->entry = uint8_t(entry);
        made->plugin->before = ref;
        made->plugin->state_before = plugins->state(unsigned(entry));
        if (instrument) made->track_before = std::make_unique<TrackState>(track_state(*this, ui.track));
    }
    std::snprintf(ref.path, plugin_path_length, "%s", file.path.c_str());
    std::snprintf(ref.name, sizeof(ref.name), "%s", file.name.c_str());
    ref.id = file.id;
    if (!same) ref.preset[0] = 0; // another plug-in: none of its presets picked yet
    if (same) plugins->reload(unsigned(entry)); // picked again: opened afresh (after it failed or stopped working)
    if (instrument) {
        unsigned t = ui.track % track_count;
        auto& tr = project.tracks[t];
        tr.plugin = uint8_t(entry);
        send({Type::TrackPlugin, uint8_t(t), 0, 0, int16_t(entry)});
        if (tr.engine != EngineKind::Plugin) load_sound(t, EngineKind::Plugin, 0);
        if (!file.drums.empty()) use_drum_pads(tr); // a drum machine (8W8, 9W9...): its pads as a kit's, 36 up
        else if (tr.layout == PadLayout::Drums) tr.layout = PadLayout::Scale; // a drum machine's pads, gone with it
        std::string name = file.name;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return char(std::toupper(ch)); });
        tr.name_from(name.c_str());
        if (made) {
            if (!made->plugin) stage_go(ui, 0, 0); // a new track: B1, its PRESETS (or its face), as a new kit's TRIM
            made->track_after = std::make_unique<TrackState>(track_state(*this, t));
        }
    } else {
        unsigned count = project.chain(n).count();
        if (k > count) k = count;
        if (project.chain(n).slots[k].kind != plugin_effect_kind) set_effect(n, k, EffectKind::Plugin);
        set_effect_param(n, k, 0, entry);
        ui.fx_cursor[UiState::cursor_of(n)] = uint8_t(k);
        ui.fx_row[UiState::cursor_of(n)] = 0;
    }
    if (made) {
        if (made->plugin) made->plugin->after = ref;
        edits.push(std::move(*made));
    }
    ui.plugin_row[size_t(entry)] = 0;
    dirty = true;
    plugins->sync(project, plugins_asleep());
    notice("Loading " + file.name);
    open(ui.plugin_back);
}

void Context::swap_plugin(Edit& e, bool after) {
    if (!e.plugin) return;
    auto& swap = *e.plugin;
    const unsigned entry = swap.entry % max_plugins;
    if (plugins) (after ? swap.state_before : swap.state_after) = plugins->state(entry); // the side left, as heard now
    project.plugins[entry] = after ? swap.after : swap.before;
    ui.plugin_row[entry] = 0;
    dirty = true;
    if (!plugins) return;
    plugins->restore(entry, after ? swap.state_after : swap.state_before);
    plugins->sync(project, plugins_asleep());
}

void Context::set_plugin_param(int32_t id, int value) {
    if (!plugins) return;
    unsigned entry = plugin_binding_entry(id), shown = plugin_binding_shown(id);
    plugins->set_param(entry, shown, value);
    auto& panel = ui.plugin_panel;
    if (panel.entry == entry)
        for (unsigned k = 0; k < 3; ++k)
            if (panel.knob[k] >= 0 && unsigned(panel.knob[k]) == shown) panel.params[k].value = int16_t(std::clamp(value, 0, plugin_param_max));
    dirty = true;
}

namespace {
std::string presets_folder(const UiState& ui) { return std::string(ui.presets_folder.data()); }
void set_presets_folder(UiState& ui, const std::string& folder) {
    ui.presets_folder.fill(0);
    folder.copy(ui.presets_folder.data(), ui.presets_folder.size() - 1);
}
} // namespace

// PRESETS opens where the preset picked last is: its folder, its row (plugin/preset_rows.h).
void Context::toggle_plugin_transport(unsigned entry) {
    if (entry >= max_plugins || !project.plugins[entry].used()) return;
    auto& ref = project.plugins[entry];
    ref.transport = !ref.transport;
    dirty = true;
    if (plugins) plugins->sync(project, plugins_asleep()); // heard from the next block
}

void Context::name_plugin_preset() {
    name_preset_entry = track().plugin;
    name_preset_generation = plugins ? plugins->preset_generation(name_preset_entry) : 0;
    if (!name_preset_generation) { notice("Synth is not ready"); return; }
    if (ui.page == PageId::PluginPresets && ui.presets_entry != name_preset_entry) {
        notice("Open the synth to save its preset");
        return;
    }
    open_name(NameTarget::PluginPreset, "My Patch", NameEntry::capacity - 1, false);
}

void Context::open_plugin_presets(unsigned entry) {
    if (!plugins || entry >= max_plugins) return;
    if (ui.preset_due && ui.presets_entry == entry) { // Stage's turn still to load: now, as the cursor means a row here
        ui.preset_due = 0;
        choose_plugin_program(ui.presets_cursor);
    }
    auto programs = plugins->programs(entry);
    if (programs.list.empty()) { notice(programs.finding ? "Looking for presets" : "No presets in this plug-in"); return; }
    auto place = preset_place(programs.list, programs.current);
    ui.presets_entry = uint8_t(entry);
    ui.presets_back = ui.page;
    set_presets_folder(ui, place.folder);
    ui.presets_cursor = uint16_t(place.row);
    open(PageId::PluginPresets);
}

void Context::choose_plugin_program(int index) {
    if (!plugins) return;
    auto programs = plugins->programs(ui.presets_entry);
    if (index < 0 || size_t(index) >= programs.list.size()) return;
    const auto& picked = programs.list[size_t(index)];
    if (picked.user()) {
        plugins->load_user_preset(ui.presets_entry, picked.name);
        return;
    }
    plugins->select_program(ui.presets_entry, picked.index);
    if (ui.presets_entry < max_plugins) // its name kept with the project, shown again when it is opened
        std::snprintf(project.plugins[ui.presets_entry].preset, sizeof(project.plugins[0].preset), "%s", program_key(picked).c_str());
    dirty = true; // the plug-in's state, saved with the project, now holds it
}

void Context::push_plugin_preset() {
    if (!plugins) return;
    auto programs = plugins->programs(ui.presets_entry);
    auto inside = presets_folder(ui);
    auto rows = preset_rows(programs.list, inside, programs.current);
    if (rows.empty()) return;
    const auto& row = rows[std::min<size_t>(ui.presets_cursor, rows.size() - 1)];
    if (!row.folder) { choose_plugin_program(int(row.at)); return; }
    std::string into = inside.empty() ? row.name : inside + "/" + row.name;
    if (into.size() >= ui.presets_folder.size()) { notice("Folder name too long"); return; }
    set_presets_folder(ui, into);
    ui.presets_cursor = 0;
    if (!row.current) return;
    auto in = preset_rows(programs.list, into, programs.current); // the preset picked last is in here: open on it
    for (size_t n = 0; n < in.size(); ++n)
        if (in[n].current) { ui.presets_cursor = uint16_t(n); break; }
}

void Context::back_plugin_presets() {
    auto inside = presets_folder(ui);
    if (inside.empty() || !plugins) { open(ui.presets_back); return; }
    auto programs = plugins->programs(ui.presets_entry);
    ui.presets_cursor = uint16_t(preset_row_of_folder(programs.list, inside, programs.current));
    set_presets_folder(ui, preset_folder_up(inside));
}

int Context::plugin_in_focus() const {
    const auto& t = project.tracks[ui.track % track_count];
    if (ui.page == PageId::PluginPresets && ui.presets_entry < max_plugins) return ui.presets_entry;
    if (ui.page == PageId::Edit && t.engine == EngineKind::Plugin && t.plugin < max_plugins) return t.plugin;
    if (ui.page == PageId::Stage && t.engine == EngineKind::Plugin && t.plugin < max_plugins) return t.plugin; // B1 PRESETS, EDIT
    if (chain_page(ui.page) && !ui.picker_open) {
        unsigned n = page_chain(project, ui), k = ui.fx_cursor[UiState::cursor_of(n)];
        const auto& chain = project.chain(n);
        if (k < chain.count() && chain.slots[k].kind == plugin_effect_kind && unsigned(chain.slots[k].params[0]) < max_plugins)
            return chain.slots[k].params[0];
    }
    return -1;
}

int Context::plugin_screen_in_view() const {
    if (ui.fx_panel_entry != no_plugin && chain_page(ui.page) && plugin_in_focus() == ui.fx_panel_entry) return ui.fx_panel_entry;
    const auto& t = project.tracks[ui.track % track_count];
    if (t.engine != EngineKind::Plugin || t.plugin >= max_plugins) return -1;
    if (ui.page == PageId::Edit) return t.plugin;
    if (ui.page == PageId::Stage && stage_page_now(ui, t, stage_skip(project, ui, t)).kind == StageKind::PluginEdit) return t.plugin;
    return -1;
}

bool Context::plugin_screen_input(PluginScreenKey key, int value) {
    int entry = plugin_screen_in_view();
    if (entry < 0 || !plugins || ui.plugin_panel.entry != unsigned(entry) || !ui.plugin_panel.screen) return false;
    plugins->screen_input(unsigned(entry), {uint32_t(key), value, ui.shift});
    if (ui.shift) ui.shift_used = true;
    return true;
}

void Context::plugins_tick(uint32_t at) {
    if (!plugins) return;
    if (library) plugins->set_card(library->card()); // the card's top folder, as the sample library found it
    split_shared_plugins();
    plugins->sync(project, plugins_asleep());
    PluginRack::PresetResult result;
    while (plugins->take_preset_result(result)) {
        if (!result.error.empty()) { notice(result.error); continue; }
        if (result.saving) notice("Saved " + result.name);
        if (result.entry == no_plugin) continue;
        auto& ref = project.plugins[result.entry];
        std::snprintf(ref.preset, sizeof(ref.preset), "/user/%s", result.name.c_str());
        if (!result.saving) plugins->restore(result.entry, std::move(result.state));
        plugins->sync(project, plugins_asleep());
        if (result.saving && ui.page == PageId::PluginPresets && ui.presets_entry == result.entry) {
            auto programs = plugins->programs(result.entry);
            auto place = preset_place(programs.list, programs.current);
            set_presets_folder(ui, place.folder);
            ui.presets_cursor = uint16_t(place.row);
        }
        plugin_panel_at = at - 50; // enable PRESETS immediately, including a synth's very first patch
        dirty = true;
    }
    const int entry = plugin_in_focus();
    if (ui.fx_panel_entry != no_plugin && entry != ui.fx_panel_entry) ui.fx_panel_entry = no_plugin;
    const int screen = plugin_screen_in_view();
    { // the plug-in screen in view, and what it is told: each tick, so a pot's move reaches its picture at once
        PluginScreenContext told;
        for (unsigned k = 0; k < 3; ++k) { told.pots[k] = ui.pots[k].position; told.engaged[k] = ui.pots[k].engaged; }
        told.shift = ui.shift;
        told.playing = host.playing();
        told.accent = 0xff000000u | ui::palettes[ui.settings.palette % ui::palettes.size()].tracks[ui.track % 16][0];
        plugins->screen_focus(screen >= 0 ? unsigned(screen) : no_plugin, told);
    }
    delete_tick(*this); // the picker listed again once a plug-in is deleted (shell/delete_jobs.h)
    if (ui.page == PageId::Plugins && plugins->changes() != plugin_view_seen) { // the picker's rows, and where going up lands
        plugin_view_seen = plugins->changes();
        auto view = plugins->view();
        ui.plugins_count = uint16_t(view.files.size());
        if (view.came_from >= 0 && !plugin_came_back) { ui.plugins_cursor = uint16_t(view.came_from); plugin_came_back = true; }
        if (ui.plugins_cursor >= ui.plugins_count) ui.plugins_cursor = uint16_t(ui.plugins_count ? ui.plugins_count - 1 : 0);
    }
    if (at - plugin_panel_at < 50) return; // the cards' texts twenty times a second
    plugin_panel_at = at;
    { // the selected track's drum voices, for Keys and the pad lights (loaded after the pick, so asked each time)
        const auto& t = track();
        if (t.engine == EngineKind::Plugin && drum_pads(t)) plugins->drum_voices(t.plugin, ui.drum_voices);
        else if (ui.drum_voices.entry != no_plugin) ui.drum_voices = DrumVoices {};
    }
    if (entry < 0) { ui.plugin_panel = PluginPanel {}; return; }
    plugins->panel(unsigned(entry), ui.plugin_row[size_t(entry)], screen == entry, ui.plugin_panel);
    if (ui.fx_panel_entry != no_plugin && !ui.plugin_panel.has_screen) ui.fx_panel_entry = no_plugin;
    ui.plugin_row[size_t(entry)] = ui.plugin_panel.row; // clamped to its rows
}

} // namespace melodee
