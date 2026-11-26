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
// new one takes an entry nothing uses. An instrument loads onto the track in
// hand when Push comes: over another plug-in, or over a track's other sound,
// it asks first and is one undo step. The rack loads what is used and lets go
// of the rest, so undo, a project switch or a removed effect need nothing more.
#include "melodee/shell/mixer_channels.h"
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
#include "melodee/shell/screen_view.h"
#include "melodee/shell/stage_map.h"
#include "melodee/shell/track_copy.h"
#include "melodee/ui/tokens.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

namespace melodee {
using Type = Command::Type;

namespace {
// The entries a track names: its instrument's, and its chain's plug-in effects'.
void mark_track(const Track& t, std::array<bool, max_plugins>& out) {
    if (t.engine == EngineKind::Plugin && t.plugin < max_plugins) out[t.plugin] = true;
    for (const auto& s : t.fx.slots) if (s.kind == plugin_effect_kind && unsigned(s.params[0]) < max_plugins) out[size_t(s.params[0])] = true;
}
void mark_chain(const Chain& chain, std::array<bool, max_plugins>& out) {
    for (const auto& s : chain.slots) if (s.kind == plugin_effect_kind && unsigned(s.params[0]) < max_plugins) out[size_t(s.params[0])] = true;
}
// The entries an undo or a redo could point a track or a chain at again, or swap a plug-in in; `pending`: an edit
// still being made, not kept yet.
std::array<bool, max_plugins> named_by_history(const History& edits, const Edit* pending) {
    std::array<bool, max_plugins> named {};
    auto mark = [&](const Edit& e) {
        if (e.track_before) mark_track(e.track_before->track, named);
        if (e.track_after) mark_track(e.track_after->track, named);
        if (e.chain_before) mark_chain(*e.chain_before, named);
        if (e.chain_after) mark_chain(*e.chain_after, named);
        if (e.plugin) named[e.plugin->entry % max_plugins] = true;
    };
    edits.each(mark);
    if (pending) mark(*pending);
    return named;
}
} // namespace

// An entry for a new plug-in: one never set first, else one nothing uses now. Never one an undo or a redo still names
// (a cleared track's, a copy undone): its plug-in would come back as the new one. When every entry is used or named,
// the redo steps go, then the oldest undo steps, until one is free (-1: all 32 are in use now). The entry taken starts
// clean: its old name and path, and the rack's state kept for it, go.
int Context::take_plugin_entry(const Edit* pending) {
    for (;;) {
        const auto named = named_by_history(edits, pending);
        for (unsigned e = 0; e < max_plugins; ++e) if (!project.plugins[e].used() && !named[e]) return fresh_entry(e);
        for (unsigned e = 0; e < max_plugins; ++e) if (!plugin_used_by(project, e) && !named[e]) return fresh_entry(e);
        if (edits.can_redo()) { edits.clear_redo(); continue; }
        bool any = false;
        for (unsigned e = 0; e < max_plugins; ++e) any |= !plugin_used_by(project, e);
        if (!any || !edits.drop_oldest()) return -1;
    }
}

int Context::fresh_entry(unsigned e) {
    project.plugins[e] = PluginRef {};
    if (plugins) plugins->restore(e, {}); // nothing of what was let go here comes back
    return int(e);
}

// A track copied onto track `t` names the same entries as the track copied: `t` gets entries of its own, with the
// same plug-ins and their state as they are now, and the track copied keeps its own (and is not opened again).
void Context::own_plugins(unsigned t, const Edit* pending) {
    t %= track_count;
    auto shared = [&](unsigned entry) {
        unsigned users = 0;
        for (const auto& tr : project.tracks) users += tr.engine == EngineKind::Plugin && tr.plugin == entry;
        for (unsigned n = 0; n < mix_chain_count; ++n)
            for (const auto& s : project.chain(n).slots) users += s.kind == plugin_effect_kind && unsigned(s.params[0]) == entry;
        return users > 1;
    };
    auto copy_of = [&](unsigned entry) -> int {
        if (entry >= max_plugins || !shared(entry)) return -1;
        const PluginRef from = project.plugins[entry];
        const int copy = take_plugin_entry(pending);
        if (copy < 0) return -1; // none free: it stays shared until split_shared_plugins finds one
        project.plugins[size_t(copy)] = from;
        if (plugins) plugins->clone(entry, unsigned(copy));
        return copy;
    };
    auto& tr = project.tracks[t];
    if (tr.engine == EngineKind::Plugin)
        if (int copy = copy_of(tr.plugin); copy >= 0) {
            tr.plugin = uint8_t(copy);
            send({Type::TrackPlugin, uint8_t(t), 0, 0, int16_t(copy)});
        }
    for (unsigned k = 0; k < tr.fx.size; ++k) {
        const auto& s = tr.fx.slots[k];
        if (s.kind != plugin_effect_kind) continue;
        if (int copy = copy_of(unsigned(s.params[0])); copy >= 0) set_effect_param(t, k, 0, copy);
    }
}

// One instance plays one place: a track copy (or a copied chain) leaves two users on an entry, and the second and
// after each get an entry of their own with the same plug-in and its state as it is now.
void Context::split_shared_plugins() {
    std::array<bool, max_plugins> seen {};
    auto claim = [&](unsigned entry) -> int { // -1: keep it; else the entry to move to
        if (entry >= max_plugins) return -1;
        if (!seen[entry]) { seen[entry] = true; return -1; }
        const PluginRef from = project.plugins[entry];
        int copy = take_plugin_entry();
        if (copy < 0 || seen[size_t(copy)]) return -1; // none free: it stays shared (and is heard twice)
        project.plugins[size_t(copy)] = from;
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
    for (unsigned n = 0; n < mix_chain_count; ++n) // every chain but the kit pads' (no plug-ins there)
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
    ui.plugin_back = target == UiState::PluginTarget::Instrument ? PageId::Stage : ui.page; // an instrument: Stage's B1 EDIT once loaded
    take_plugin_view(); // the rows as they are listed now, until the new listing is in (never none: NO CARD)
    plugins->relist(); // where the picker was left, listed afresh, on the row it was on
    if (ui.page != PageId::Plugins) ui.plugin_from = ui.page;
    open(PageId::Plugins);
}

// The picker's rows as the rack listed them, taken here and in plugins_tick() only, and the cursor with them: onto the
// row asked for once its own listing is in (going up, a web page change), to the top of a folder newly opened, else
// on the row it was on.
void Context::take_plugin_view() {
    if (!plugins) return;
    plugin_view_seen = plugins->changes();
    auto view = plugins->view();
    const bool other = view.folder != plugin_view.folder;
    plugin_view = std::move(view);
    ui.plugins_count = uint16_t(plugin_view.files.size());
    if (plugin_landing && plugin_view.request == plugin_landing) {
        if (plugin_view.came_from >= 0) ui.plugins_cursor = uint16_t(plugin_view.came_from);
        else if (other) ui.plugins_cursor = 0;
        plugin_landing = 0;
    } else if (other) {
        ui.plugins_cursor = 0;
    }
    if (ui.plugins_cursor >= ui.plugins_count) ui.plugins_cursor = uint16_t(ui.plugins_count ? ui.plugins_count - 1 : 0);
}

void Context::choose_plugin(const PluginFile& file) {
    if (!plugins) return;
    // A folder, or a bundle's plug-ins: listed on the rack's thread; the cursor goes to its top once they show (plugins_tick)
    if (file.folder || file.several()) { plugins->browse(file.path); return; }
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
        if (n >= mix_chain_count || k >= project.chain(n).size) return;
        const auto& s = project.chain(n).slots[k];
        if (s.kind == plugin_effect_kind && unsigned(s.params[0]) < max_plugins) entry = s.params[0];
    }
    bool replace_asked = ui.plugin_replace;
    ui.plugin_replace = false;
    // The picker loads onto the track in hand, which may not be the one it was opened on (Pad Bank + pad, MIDI
    // Remote). A kit, a keygroup, a MIDI or an audio track there would turn PLUGIN: its sound and settings go too.
    const bool over_sound = instrument && track().used() && track().engine != EngineKind::Plugin;
    const std::string sound_name = track().name[0] ? std::string(track().name) : "Track " + std::to_string(ui.track % track_count + 1);
    const bool over_plugin = entry >= 0 && project.plugins[size_t(entry)].used()
        && !(file.path == project.plugins[size_t(entry)].path && file.id == project.plugins[size_t(entry)].id);
    if ((over_plugin || over_sound) && !replace_asked) {
        // Another plug-in over one in use, or over a track's other sound: its sound and settings go (undo brings them
        // back); ask first, and for this track only (shell/ask.cpp)
        std::snprintf(ui.ask_name.data(), ui.ask_name.size(), "%s", over_sound ? sound_name.c_str() : project.plugins[size_t(entry)].name);
        plugin_asked_track = instrument ? int(ui.track % track_count) : -1;
        plugin_asked = file; // REPLACE loads this one, wherever its row is by then
        ask(*this, UiState::Ask::ReplacePlugin, int(ui.plugins_cursor));
        return;
    }
    if (entry < 0) entry = take_plugin_entry();
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
    // Over a track's other sound: one undo step, the whole track back as it was (the plug-in takes an entry of its own).
    if (!made && over_sound) {
        made = std::make_unique<Edit>(begin_edit(("Replace " + sound_name).c_str(), ui.track, -1, false));
        made->motion_before.reset();
        made->track_before = std::make_unique<TrackState>(track_state(*this, ui.track));
    }
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
    if (instrument && ui.plugin_back == PageId::Stage) stage_go(ui, EngineKind::Plugin, StageKind::PluginEdit); // its parameters
    open(ui.plugin_back);
    if (!instrument) picker_done(); // from the mixer's plus: back there
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
    auto& panel = ui.plugin_panel;
    if (plugin_binding_indexed(id)) { // a companion screen's own edit (plugin/companion_screen.h), by its index
        plugins->set_param_index(entry, plugins->live_generation(entry), int(shown - plugin_index_slot), value);
        dirty = true; // K1-K3's cards read it back at the panel's next tick
        return;
    }
    if (shown >= plugin_screen_slot) { // one of the three its own screen names
        unsigned k = shown - plugin_screen_slot;
        const bool relative = !ui.erase && ui.settings.knob_takeover != Takeover::Instant;
        if (panel.entry != entry || !plugins->set_screen_param(panel, k, value, relative)) return;
        panel.params[k].value = int16_t(std::clamp(value, 0, plugin_param_max));
        dirty = true;
        return;
    }
    plugins->set_param(entry, shown, value);
    if (panel.entry == entry && !panel.screen)
        for (unsigned k = 0; k < 3; ++k)
            if (panel.row * 3u + k == shown) panel.params[k].value = int16_t(std::clamp(value, 0, plugin_param_max));
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

void Context::name_plugin_preset(unsigned entry) {
    name_preset_entry = entry;
    name_preset_generation = plugins ? plugins->preset_generation(entry) : 0;
    if (!name_preset_generation) { notice("Plug-in is not ready"); return; }
    open_name(NameTarget::PluginPreset, "My Patch", NameEntry::capacity - 1, false);
}

void Context::open_plugin_presets(unsigned entry) {
    if (!plugins || entry >= max_plugins) return;
    // A turn on Stage still to load (a plug-in's preset, or a synth's sound): now, onto the track it was turned on, as
    // Stage would have once the turning stopped, before this list takes the cursor and the entry over (it may be
    // an effect's). Left for later, it was dropped: the list's entry was no longer the track's.
    if (ui.preset_due) stage_tick(UINT32_MAX);
    auto programs = plugins->programs(entry);
    if (programs.list.empty()) { notice(programs.finding ? "Looking for presets" : "No presets in this plug-in"); return; }
    auto place = preset_place(programs.list, programs.current);
    ui.presets_entry = uint8_t(entry);
    ui.presets_back = ui.page;
    set_presets_folder(ui, place.folder);
    ui.presets_cursor = uint16_t(place.row);
    { // whose list it is: the track in hand's instrument, an effect on its chain, or no track's (presets_follow)
        const auto& t = track();
        presets_instrument = t.engine == EngineKind::Plugin && t.plugin == entry;
        bool effect = false;
        for (const auto& s : project.chain(ui.track % track_count).slots) effect |= s.kind == plugin_effect_kind && unsigned(s.params[0]) == entry;
        presets_track = presets_instrument || effect ? int8_t(ui.track % track_count) : int8_t(-1);
    }
    open(PageId::PluginPresets);
}

// PRESETS is open for a track's list (open_plugin_presets: presets_track), and that is no longer what it lists: another
// track was picked (select_track), or the track's plug-in moved to another entry (a Copy Track's copy gets its own at
// the next tick, split_shared_plugins). Left so, the list would go on loading into a plug-in the pads no longer play.
// An instrument's list turns to the plug-in of the track in hand, where the preset picked last is; with no presets
// there (another kind of track, a plug-in with none or still opening), and for an effect's list on a track's chain,
// the page goes back where it was opened from. An effect on a bus's, a submix's, the master's or a loop track's chain
// is no track's: its list stays.
void Context::presets_follow() {
    if (ui.page != PageId::PluginPresets || !plugins || presets_track < 0 || ui.presets_entry >= max_plugins) return;
    const unsigned owner = unsigned(presets_track) % track_count, here = ui.track % track_count;
    const auto& t = track();
    if (presets_instrument) {
        if (here == owner && t.engine == EngineKind::Plugin && t.plugin == ui.presets_entry) return; // still its own
        if (t.engine == EngineKind::Plugin && t.plugin < max_plugins) {
            auto programs = plugins->programs(t.plugin);
            if (!programs.list.empty()) {
                auto place = preset_place(programs.list, programs.current);
                ui.presets_entry = t.plugin;
                presets_track = int8_t(here);
                set_presets_folder(ui, place.folder);
                ui.presets_cursor = uint16_t(place.row);
                return;
            }
        }
    } else {
        bool there = false;
        for (const auto& s : project.chain(owner).slots) there |= s.kind == plugin_effect_kind && unsigned(s.params[0]) == ui.presets_entry;
        if (here == owner && there) return;
    }
    open(ui.presets_back);
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

void Context::pick_plugin_program(unsigned entry, int program) {
    if (!plugins || entry >= max_plugins) return;
    const auto programs = plugins->programs(entry);
    for (const auto& p : programs.list) {
        if (p.index != program) continue;
        plugins->select_program(entry, p.index);
        std::snprintf(project.plugins[entry].preset, sizeof(project.plugins[0].preset), "%s", program_key(p).c_str());
        dirty = true;
        return;
    }
}

// Companion screens (plugin/companion_screen.h) ask; the shell makes the change as a pot turn on the plug-in's card
// makes it (apply(): a frozen track's question, one undo step per control) or as PRESETS picks a program. One asked of
// a plug-in replaced since is dropped. And why a screen stopped, said once.
void Context::companion_requests() {
    static std::vector<ScreenRequest> asked; // the MIDI thread's, its room kept
    asked.clear();
    plugins->take_screen_requests(asked);
    for (const auto& r : asked) {
        if (r.entry >= max_plugins || r.generation != plugins->live_generation(r.entry)) continue;
        if (r.program) { pick_plugin_program(r.entry, r.index); continue; }
        if (r.index < 0 || unsigned(r.index) >= plugin_screen_slot - plugin_index_slot) continue;
        Binding b {Binding::Kind::PluginParam, plugin_binding(r.entry, plugin_index_slot + unsigned(r.index)), 0, plugin_param_max, 0};
        apply(b, int(std::lround(std::clamp(r.value, 0.0f, 1.0f) * plugin_param_max)));
    }
    if (const auto why = plugins->screen_notice(); !why.empty()) notice(why);
}

// An input on a plug-in's own screen: handed to it. False when the page in view is not one (the page's own job then).
bool Context::plugin_screen_input(ScreenKey key, int value) {
    const auto at = plugin_screen_place(project, ui);
    if (at.entry < 0 || !plugins || !ui.plugin_panel.screen) return false;
    plugins->screen_input(unsigned(at.entry), {key, int32_t(value), ui.shift});
    return true;
}

// MUTE or ERASE tapped on a plug-in's own page (not an effect's strip, where the chain's own jobs act): handed to it
// when its page takes that button (melodee_screen.h keys()), asked now rather than from the panel's last refresh.
bool Context::plugin_screen_key(ScreenKey key) {
    const auto at = plugin_screen_place(project, ui);
    if (at.entry < 0 || at.area != 0 || !plugins || !ui.plugin_panel.screen) return false;
    const uint8_t bit = key == ScreenKey::Mute ? screen_key_mute : key == ScreenKey::Erase ? screen_key_erase : 0;
    if (!bit || !(plugins->screen_keys(unsigned(at.entry)) & bit)) return false;
    plugins->screen_input(unsigned(at.entry), {key, 1, ui.shift});
    return true;
}

int Context::plugin_in_focus() const {
    const auto& t = project.tracks[ui.track % track_count];
    if (ui.page == PageId::PluginPresets && ui.presets_entry < max_plugins) return ui.presets_entry;
    if (ui.page == PageId::Stage && t.engine == EngineKind::Plugin && t.plugin < max_plugins) return t.plugin; // B1 PRESETS, EDIT
    const bool mixer_fx = mixer_fx_lit(ui) && channel_chain(mixer_strip(ui)) >= 0; // the mixer's FX block: its lit effect
    if ((fx_chain_page(ui.page) && !ui.picker_open) || mixer_fx) {
        unsigned n = mixer_fx ? unsigned(channel_chain(mixer_strip(ui))) : page_chain(project, ui), k = ui.fx_cursor[UiState::cursor_of(n)];
        const auto& chain = project.chain(n);
        if (k < chain.count() && chain.slots[k].kind == plugin_effect_kind && unsigned(chain.slots[k].params[0]) < max_plugins)
            return chain.slots[k].params[0];
    }
    return -1;
}

void Context::plugins_follow() {
    if (plugins) plugins->sync(project, plugins_asleep());
}

void Context::plugins_tick(uint32_t at) {
    if (!plugins) return;
    if (library) plugins->set_card(library->card()); // the card's top folder, as the sample library found it
    split_shared_plugins();
    presets_follow(); // a copy's plug-in in its own entry now: an open PRESETS list follows it
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
    delete_tick(*this); // the picker listed again once a plug-in is deleted (shell/delete_jobs.h)
    // The web page installed or deleted a plug-in: an open picker lists again, its cursor on the plug-in it was on.
    if (plugins->card_changes() != plugin_card_seen) {
        plugin_card_seen = plugins->card_changes();
        if (ui.page == PageId::Plugins) { // the folder asked for last, which may not be in yet: on the row under the cursor,
            // or, with a landing still to come (B1 not in yet), on the row that one was to land on
            const auto& view = plugin_view;
            if (!plugin_landing) {
                const PluginFile on = ui.plugins_cursor < view.files.size() ? view.files[ui.plugins_cursor] : PluginFile {};
                plugin_landing_row = on.path;
                plugin_landing_id = on.id;
            }
            plugin_landing = plugins->relist(plugin_landing_row, plugin_landing_id);
        }
    }
    if (ui.page == PageId::Plugins && plugins->changes() != plugin_view_seen) take_plugin_view();
    { // A plug-in's own screen in view: what it is told, each tick, so a pot reaches its picture at once.
        ScreenView view;
        if (const auto at = plugin_screen_place(project, ui); at.entry >= 0) {
            view.entry = uint8_t(at.entry);
            view.area = at.area;
            view.shift = ui.shift;
            view.playing = host.playing();
            view.colour = ui::track_colour(ui.track).getARGB() & 0xffffffu;
            for (unsigned k = 0; k < 3; ++k) { view.pots[k] = ui.pots[k].position; view.live[k] = ui.pots[k].engaged ? 1 : 0; }
        }
        plugins->screen_view(view);
    }
    companion_requests();
    // The cards' texts twenty times a second, and at once when the plug-in in view is another (refresh_plugin_panel).
    const int focus = plugin_in_focus();
    const bool moved = focus != (ui.plugin_panel.entry < max_plugins ? int(ui.plugin_panel.entry) : -1);
    if (!moved && at - plugin_panel_at < 50) return;
    plugin_panel_at = at;
    { // the selected track's drum voices, for Keys and the pad lights (loaded after the pick, so asked each time)
        const auto& t = track();
        if (t.engine == EngineKind::Plugin && drum_pads(t)) plugins->drum_voices(t.plugin, ui.drum_voices);
        else if (ui.drum_voices.entry != no_plugin) ui.drum_voices = DrumVoices {};
    }
    refresh_plugin_panel();
}

// The panel of the plug-in the page in view shows, read now: from plugins_tick (often), and by the shell before a
// button or the wheel acts. Stage passes over PANEL unless this panel says the track's plug-in draws its own screen
// (shell/features.h), so one twenty times a second, or another plug-in's (just after Pad Bank + pad, or back from the
// mixer), made − / +, the wheel and B1 act on PRESETS there and load a preset into a track that was on its PANEL.
void Context::refresh_plugin_panel() {
    if (!plugins) return;
    const int entry = plugin_in_focus();
    if (entry < 0) { ui.plugin_panel = PluginPanel {}; return; }
    plugins->panel(unsigned(entry), ui.plugin_row[size_t(entry)], ui.plugin_panel);
    ui.plugin_row[size_t(entry)] = ui.plugin_panel.row; // clamped to its rows
}

} // namespace melodee
