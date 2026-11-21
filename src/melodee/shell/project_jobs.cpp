// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// The Project page's jobs from the MPC's project page (parity phase 10):
// New project asks first (B3 DO IT, B1 CANCEL), and Load Kit takes another
// project's kit (pads, samples, their effects; no patterns) onto the
// selected track as one undo; a bank file loads into one bank of it. Push on an MPC kit in the browser
// asks first (the LOAD KIT sheet): which of its filled banks go into which of the track's banks, or all of it.
#include "melodee/shell/ask.h"
#include "melodee/shell/card_access.h"
#include "melodee/shell/context.h"
#include "melodee/shell/lesson.h"
#include "melodee/shell/new_track.h"
#include "melodee/shell/project_kits.h"
#include "melodee/shell/project_store.h"
#include "melodee/shell/sample_library.h"
#include "melodee/shell/track_copy.h"
#include <cstdio>
#include <filesystem>
#include <memory>

namespace melodee {

void Context::ask_new_project() {
    // Autosave Off (issue 18): a project with a file is not saved first either, its changes are left behind (3)
    ask(*this, UiState::Ask::NewProject, !dirty ? 0 : project_path.empty() ? 2 : ui.settings.autosave ? 1 : 3);
}

void Context::start_new_project(bool kit) {
    if (dirty && !project_path.empty() && ui.settings.autosave) save_project(); // Autosave Off: left as it was saved
    // KIT: the MPC's factory kit on track 1 (shell/lesson.h), onto an empty kit while it loads. Not there (another
    // firmware): no tracks, as NEW.
    const bool factory = kit && store && std::filesystem::exists(factory_kit_path);
    new_project(factory);
    notice("New project");
    if (factory) { open(PageId::Stage); load_kit_file(factory_kit_path, factory_kit_name); return; }
    offer_new_track(*this); // no tracks: the track types on track 1 at once
}

void Context::load_kit_file(const std::string& path, const std::string& name, bool ask) {
    if (!store) return;
    kit_from = path;
    kit_ask = ask;
    kit_held.reset();
    store->load(path); // lands in load_kit() instead of switching (shell/lights.cpp)
    notice("Loading kit from " + name);
}

void Context::load_kit(const Project& from, const std::string& name) {
    int k = kit_track(from);
    if (k < 0) { notice("No kit in " + name); return; }
    if (ui.recording) { notice("Load Kit: not in a take"); return; }
    if (kit_ask && track().engine == EngineKind::Sampler) { // the LOAD KIT sheet: its filled banks, the bank in view gets the first
        kit_ask = false;
        kit_held = std::make_unique<Project>(from);
        auto& sheet = ui.kit_sheet;
        sheet = UiState::KitSheet {};
        std::snprintf(sheet.name, sizeof(sheet.name), "%s", name.c_str());
        sheet.pads = kit_bank_pads(from.tracks[unsigned(k)]);
        for (unsigned b = 0; b < sampler_banks; ++b)
            if (sheet.pads[b]) { sheet.into[track().bank % sampler_banks] = int8_t(b); break; }
        ui.sheet_open = true;
        ui.sheet_row = 0; // LOAD
        return;
    }
    kit_ask = false;
    std::string what = "Load kit " + name;
    Edit edit = begin_edit(what.c_str(), ui.track, -1, false);
    edit.motion_before.reset(); // the kit brings no motion
    edit.track_before = std::make_unique<TrackState>(track_state(*this, ui.track));
    auto next = std::make_unique<TrackState>(*edit.track_before);
    auto result = import_kit(from, unsigned(k), next->track, [&](const SampleRef& ref) { return sample_slot(ref.path, ref.name); });
    restore_track(*this, ui.track, *next);
    edit.track_after = std::move(next);
    edits.push(std::move(edit));
    notice(result.missing ? what + ": " + std::to_string(result.missing) + " pads without a slot" : what);
    if (ui.page == PageId::Project) open(PageId::Stage); // the kit's sound (Sample held shows its pads)
}

// LOAD on the LOAD KIT sheet: each bank of the track given one of the kit's gets that bank's filled pads, as one undo.
void Context::load_kit_banks() {
    ui.sheet_open = false;
    std::unique_ptr<Project> from = std::move(kit_held);
    const auto into = ui.kit_sheet.into;
    const std::string name = ui.kit_sheet.name;
    int k = from ? kit_track(*from) : -1;
    if (k < 0) return;
    if (track().engine != EngineKind::Sampler) { notice("Load Kit: kit tracks"); return; }
    if (ui.recording) { notice("Load Kit: not in a take"); return; }
    unsigned banks = 0, last = 0;
    for (unsigned b = 0; b < sampler_banks; ++b) if (into[b] >= 0) { ++banks; last = b; }
    if (!banks) return;
    std::string what = banks == 1 ? std::string("Bank ") + char('A' + last) + ": " + name : std::to_string(banks) + " banks: " + name;
    if (what.size() > 31) what.resize(31); // the notice holds 31 characters
    Edit edit = begin_edit(what.c_str(), ui.track, -1, false);
    edit.motion_before.reset(); // a kit brings no motion
    edit.track_before = std::make_unique<TrackState>(track_state(*this, ui.track));
    auto next = std::make_unique<TrackState>(*edit.track_before);
    KitImport result;
    for (unsigned b = 0; b < sampler_banks; ++b) {
        if (into[b] < 0) continue;
        auto r = import_kit_bank(*from, unsigned(k), unsigned(into[b]), b, next->track,
                                 [&](const SampleRef& ref) { return sample_slot(ref.path, ref.name); });
        result.pads += r.pads; result.missing += r.missing;
    }
    next->track.bank = uint8_t(last); // the pads play the bank loaded last
    restore_track(*this, ui.track, *next);
    edit.track_after = std::move(next);
    edits.push(std::move(edit));
    dirty = true;
    notice(result.missing ? std::to_string(result.missing) + " pads had no slot" : what);
}

// A bank's card on the LOAD KIT sheet: the kit's next filled bank into it, after the last one none.
void Context::kit_sheet_step(unsigned bank) {
    auto& sheet = ui.kit_sheet;
    bank %= sampler_banks;
    for (int b = sheet.into[bank] + 1; b < int(sampler_banks); ++b)
        if (sheet.pads[unsigned(b)]) { sheet.into[bank] = int8_t(b); return; }
    sheet.into[bank] = -1;
}

// A kit bank file (Push on one in the Samples browser): read on the service
// thread, then into the bank in view of the selected kit (files_landed()).
void Context::load_bank_file(const std::string& path, const std::string& name) {
    if (!library) return;
    if (track().engine != EngineKind::Sampler) { notice("Load Bank: kit tracks"); return; }
    bank_from = path;
    library->read_bank(path);
    notice("Loading " + name);
}

void Context::load_bank(const BankFile& bank, const std::string& name) {
    auto& t = track();
    if (t.engine != EngineKind::Sampler) { notice("Load Bank: kit tracks"); return; }
    if (ui.recording) { notice("Load Bank: not in a take"); return; }
    unsigned into = t.bank % sampler_banks;
    std::string bank_name = std::string("Bank ") + char('A' + into), what = "Load " + bank_name; // the notice holds 31 characters
    Edit edit = begin_edit(what.c_str(), ui.track, -1, false);
    edit.motion_before.reset(); // a bank brings no motion
    edit.track_before = std::make_unique<TrackState>(track_state(*this, ui.track));
    auto next = std::make_unique<TrackState>(*edit.track_before);
    auto result = import_bank(bank, into, next->track, [&](const SampleRef& ref) { return sample_slot(ref.path, ref.name); });
    restore_track(*this, ui.track, *next);
    edit.track_after = std::move(next);
    edits.push(std::move(edit));
    dirty = true;
    notice(result.missing ? bank_name + ": " + std::to_string(result.missing) + " pads had no slot" : bank_name + ": " + name);
}

// SD card access (parity phase 10b): the service thread lends the card; the
// page shows how it goes and owns the controls until it is back.
void Context::card_access() {
    if (!card || card_away(ui.card)) return;
    ui.sheet_open = false; ui.move_open = false;
    card->request.store(uint8_t(CardRequest::Lend));
    open(PageId::CardAccess);
}

} // namespace melodee
