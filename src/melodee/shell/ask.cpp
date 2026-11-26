// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/ask.h"
#include "melodee/shell/bindings.h"
#include "melodee/shell/context.h"
#include "melodee/shell/delete_jobs.h"
#include "melodee/shell/looper_jobs.h"
#include "melodee/plugin/plugin_rack.h"
#include "melodee/shell/project_store.h"
#include "melodee/shell/step_edit.h"
#include "melodee/shell/track_copy.h"
#include "melodee/shell/update.h"
#include <algorithm>
#include <filesystem>

namespace melodee {

namespace {
// INSTALL on the update question: stops playback and saves the project if it has unsaved changes.
// Shell::update_tick() starts the install when the save has finished.
void begin_update(Context& c) {
    c.send({Command::Type::Stop});
    c.send({Command::Type::AllNotesOff, 255});
    auto& u = c.ui.update;
    u.phase = uint8_t(UpdatePhase::Saving);
    u.error = 0;
    u.after_save = 0;
    ProjectStore::View v;
    if (c.store) c.store->view(v);
    if (c.dirty && !v.root.empty()) { u.after_save = int(c.store->saves()) + 1; c.save_project(); }
}
} // namespace

void ask(Context& c, UiState::Ask kind, int target) {
    c.ui.ask = kind;
    c.ui.ask_target = target;
    c.ui.ask_pick = 0; // the wheel starts on CANCEL
}

bool ask_button(Context& c, SurfaceButton button) {
    using B = SurfaceButton;
    auto& ui = c.ui;
    if (ui.ask == UiState::Ask::None) return false;
    if (button == B::Play || button == B::Stop) return false; // the transport still answers
    if (button == B::WheelPush) button = ui.ask_pick == 2 ? B::TopRight : B::TopLeft; // the key the wheel highlights
    if (button == B::TopLeft) { // Cancel (LATER on the update question: asked again when the card is returned)
        if (ui.ask == UiState::Ask::InstallUpdate && c.updates) c.updates->decline();
        if (ui.ask == UiState::Ask::Rechop) c.chop_pending = nullptr; // the slices stay as they are
        ui.ask = UiState::Ask::None;
        return true;
    }
    if (button == B::TopMiddle && ui.ask == UiState::Ask::NewProject) { // KIT: a new project with the factory kit on track 1
        ui.ask = UiState::Ask::None;
        c.start_new_project(true);
        return true;
    }
    if (button != B::TopRight) return true;                                  // the rest wait
    auto kind = ui.ask;
    ui.ask = UiState::Ask::None;                                              // Do it
    if (kind == UiState::Ask::EraseKeyEvents) erase_key_events(c, int(ui.ask_target));
    else if (kind == UiState::Ask::EraseLane) c.clear_lane(int(ui.ask_target));
    else if (kind == UiState::Ask::ResetSettings) c.reset_settings();
    else if (kind == UiState::Ask::NewProject) c.start_new_project();
    else if (kind == UiState::Ask::ClearLoop) { looper_do(c, looper::Do::Clear, unsigned(ui.ask_target)); c.notice("L" + std::to_string(ui.ask_target + 1) + " cleared"); }
    else if (kind == UiState::Ask::LooperOff) set_looper(c, false);
    else if (kind == UiState::Ask::ClearPattern) { // Seq held + Erase + pad: that pattern of the selected track
        unsigned n = unsigned(ui.ask_target) % pattern_count;
        c.edit_pattern(ui.track, n, "Clear pattern", [&] { c.clear_pattern(ui.track, n); });
        c.notice("P" + std::to_string(n + 1) + " cleared");
    } else if (kind == UiState::Ask::ErasePunches) c.erase_punches(); // Erase + KEEP on Pad FX (pad-fx C)
    else if (kind == UiState::Ask::ClearTrack) clear_track(c, unsigned(ui.ask_target)); // Pad Bank held + Erase + pad
    else if (kind == UiState::Ask::DeleteFile) delete_asked(c);    // shell/delete_jobs.h: Erase in a browser
    else if (kind == UiState::Ask::ReplacePlugin && c.plugins) { // the plug-in asked about, picked again
        // Its row as listed now when it is there (a web page change may have moved it), else as it was asked about,
        // still on the card (the picker may show another folder by now); one taken away from the card loads nothing.
        const auto& files = c.plugin_view.files;
        const auto same = [&](const PluginFile& f) { return f.path == c.plugin_asked.path && f.id == c.plugin_asked.id; };
        const auto it = std::find_if(files.begin(), files.end(), same);
        std::error_code missing;
        if (it != files.end() || std::filesystem::exists(c.plugin_asked.path, missing)) {
            const PluginFile file = it != files.end() ? *it : c.plugin_asked;
            // The yes is for the track asked about: with another in hand since, that one is asked about first.
            ui.plugin_replace = c.plugin_asked_track < 0 || c.plugin_asked_track == int(ui.track % track_count);
            c.choose_plugin(file);
        } else {
            c.notice("Not on the card");
        }
        ui.plugin_replace = false;
    }
    else if (kind == UiState::Ask::InstallUpdate) begin_update(c); // shell/update.h
    else if (kind == UiState::Ask::Rechop) { // issue 24 (3b): the change asked about, and no more asking this visit
        ui.chop_rechop_ok = true;
        auto change = std::move(c.chop_pending);
        c.chop_pending = nullptr;
        if (change) change();
    }
    else if (kind == UiState::Ask::Unfreeze) { // freeze plan F2c: live again at once; its prints stay for the next freeze
        unsigned t = unsigned(ui.ask_target) % track_count;
        c.unfreeze(t);
    }
    return true;
}

std::string ask_title(const Project& project, const UiState& ui) {
    const auto& t = project.tracks[ui.track % track_count];
    switch (ui.ask) {
    case UiState::Ask::EraseKeyEvents: return "Erase " + key_label(t, int(ui.ask_target)) + " events?";
    case UiState::Ask::EraseLane: return "Erase " + lane_name(t, int(ui.ask_target), &project) + " motion?";
    case UiState::Ask::ResetSettings: return "Reset factory settings?";
    case UiState::Ask::NewProject: return "New project?";
    case UiState::Ask::TakeCardBack: return "Eject anyway?";
    case UiState::Ask::ClearLoop: return "Clear L" + std::to_string(ui.ask_target + 1) + "?";
    case UiState::Ask::LooperOff: return "Turn the Looper off?";
    case UiState::Ask::ClearPattern: return "Clear P" + std::to_string(ui.ask_target % pattern_count + 1) + " of track " + std::to_string(ui.track % track_count + 1) + "?";
    case UiState::Ask::ErasePunches: return "Erase kept punches?";
    case UiState::Ask::ClearTrack: return "Clear track " + std::to_string(ui.ask_target % track_count + 1) + "?";
    case UiState::Ask::InstallUpdate: return std::string("Update to ") + ui.update.version + "?";
    case UiState::Ask::Unfreeze: return "Unfreeze track " + std::to_string(ui.ask_target % track_count + 1) + "?";
    case UiState::Ask::DeleteFile: return std::string("Delete ") + ui.ask_name.data() + "?";
    case UiState::Ask::ReplacePlugin: return std::string("Replace ") + ui.ask_name.data() + "?";
    case UiState::Ask::Rechop: return std::string("Rechop ") + (ui.chop_sample < sample_slots ? project.samples[ui.chop_sample].name : "") + "?";
    default: return {};
    }
}

std::string ask_detail(const Project& project, const UiState& ui, const Pattern& pattern) {
    const auto& t = project.tracks[ui.track % track_count];
    std::string where = " in P" + std::to_string(t.pattern % pattern_count + 1);
    unsigned count = 0;
    if (ui.ask == UiState::Ask::EraseKeyEvents) {
        for (const auto& e : pattern.events) count += e.key == ui.ask_target;
        return std::to_string(count) + (count == 1 ? " event" : " events") + where + " go.";
    }
    if (ui.ask == UiState::Ask::EraseLane) {
        count = pattern.lane_points(lane_param(int(ui.ask_target)), lane_pad(int(ui.ask_target)));
        return std::to_string(count) + (count == 1 ? " move" : " moves") + where + " go.";
    }
    if (ui.ask == UiState::Ask::ErasePunches) {
        count = unsigned(pattern.punches.size());
        return std::to_string(count) + (count == 1 ? " punch" : " punches") + where + " go.";
    }
    if (ui.ask == UiState::Ask::ResetSettings) return "Every setting as Melodee comes; projects stay.";
    if (ui.ask == UiState::Ask::TakeCardBack) return "What the computer was writing may be lost.";
    if (ui.ask == UiState::Ask::ClearLoop) return "Its audio and its undo go.";
    if (ui.ask == UiState::Ask::LooperOff) return "Every loop goes, and L1-L4 with them.";
    if (ui.ask == UiState::Ask::ClearPattern) return "Its notes and motion go; Shift + Minus brings them back.";
    if (ui.ask == UiState::Ask::ClearTrack) return "Its sound and patterns go; Shift + Minus brings them back.";
    if (ui.ask == UiState::Ask::InstallUpdate) return "The project is saved first, then Melodee restarts.";
    if (ui.ask == UiState::Ask::Unfreeze) return "Its sound plays live again; prints stay until what made them changes.";
    if (ui.ask == UiState::Ask::DeleteFile) return "It goes from the card. Undo can't bring it back.";
    if (ui.ask == UiState::Ask::ReplacePlugin) return "Its sound and settings go. Undo brings them back.";
    if (ui.ask == UiState::Ask::Rechop)
        return std::to_string(ui.ask_target) + (ui.ask_target == 1 ? " slice hit changes" : " slice hits change") + " sound. Undo brings them back.";
    if (ui.ask == UiState::Ask::NewProject)
        return ui.ask_target == 1 ? "Changes are saved first." : ui.ask_target == 2 || ui.ask_target == 3 ? "Unsaved changes are lost." : "The open project closes.";
    return {};
}

} // namespace melodee
