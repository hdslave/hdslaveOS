// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/progression_jobs.h"
#include "melodee/model/progression.h"
#include "melodee/shell/progression_file.h"
#include "melodee/shell/sample_library.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace melodee {
namespace {
namespace fs = std::filesystem;

std::string card_of(const Context& c) { return c.library ? c.library->card() : std::string(); }

void list(Context& c) {
    const auto card = card_of(c);
    c.prog_paths = list_progressions(card);
    c.prog_places.clear();
    c.prog_places.reserve(c.prog_paths.size());
    for (const auto& path : c.prog_paths) c.prog_places.push_back(prog_place(card, path));
}

// Where the track's progression is in the list (by file name; Melodee's own folder comes first), and its neighbours.
void show(Context& c) {
    const auto& t = c.track();
    auto& ui = c.ui;
    ui.prog_count = uint16_t(std::min<size_t>(c.prog_paths.size(), 65535));
    ui.prog_at = -1;
    for (size_t n = 0; n < c.prog_paths.size() && t.prog.file[0]; ++n)
        if (progression_stem(c.prog_paths[n]) == t.prog.file) { ui.prog_at = int16_t(n); break; }
    auto name = [&](int at, char* out, size_t size) {
        out[0] = 0;
        if (at >= 0 && size_t(at) < c.prog_paths.size()) std::snprintf(out, size, "%s", progression_stem(c.prog_paths[size_t(at)]).c_str());
    };
    name(ui.prog_at < 0 ? -1 : ui.prog_at - 1, ui.prog_prev, sizeof(ui.prog_prev));
    name(ui.prog_at < 0 ? 0 : ui.prog_at + 1, ui.prog_next, sizeof(ui.prog_next));
}

bool load(Context& c, size_t at) {
    Progression prog;
    auto error = read_progression(c.prog_paths[at], prog);
    if (!error.empty()) { c.notice("Progression: " + error); return false; }
    auto& t = c.track();
    t.prog = prog;
    t.prog_shift = 0;
    if (t.engine != EngineKind::Sampler) t.bank = 0; // its first sixteen chords (a kit's bank is its pads': 16 Levels Chords leaves it)
    c.dirty = true;
    return true;
}

// "Mine 1", "Mine 2" ...: the first not on the card yet.
std::string fresh_name(const std::string& folder) {
    std::error_code e;
    for (unsigned n = 1; n < 1000; ++n) {
        std::string name = "Mine " + std::to_string(n);
        if (folder.empty() || !fs::exists(fs::path(folder) / (name + ".progression"), e)) return name;
    }
    return "Mine";
}
} // namespace

void prog_wheel(Context& c, int delta) {
    if (!delta) return;
    if (c.prog_paths.empty()) list(c);
    show(c);
    c.ui.prog_shown_at = c.now;
    if (c.prog_paths.empty()) { c.notice("No progressions on the card"); return; }
    int count = int(c.prog_paths.size()), at = c.ui.prog_at;
    int next = at < 0 ? (delta > 0 ? 0 : count - 1) : std::clamp(at + delta, 0, count - 1);
    if (next == at) return;
    if (load(c, size_t(next))) show(c);
}

void prog_layout_on(Context& c) {
    auto& t = c.track();
    t.bank = 0;
    list(c);
    if (t.prog.empty() && !t.prog.file[0] && !c.prog_paths.empty()) load(c, 0);
    show(c);
}

void prog_ready(Context& c) {
    auto& t = c.track();
    if (c.prog_paths.empty()) list(c);
    if (t.prog.empty() && !t.prog.file[0] && !c.prog_paths.empty()) load(c, 0);
    show(c);
}

unsigned prog_files(Context& c) {
    list(c);
    show(c);
    return unsigned(c.prog_paths.size());
}

std::string prog_file_name(const Context& c, unsigned at) { return at < c.prog_paths.size() ? progression_stem(c.prog_paths[at]) : std::string(); }

void prog_view_open(Context& c, const std::string& folder, const std::string& select) {
    auto& ui = c.ui;
    c.prog_folder = folder;
    c.prog_view = prog_folder_rows(c.prog_places, folder);
    ui.prog_view_count = uint16_t(std::min<size_t>(c.prog_view.size(), 65535));
    ui.prog_view_own = -1;
    int picked = -1;
    for (size_t n = 0; n < c.prog_view.size() && n < 65535; ++n) {
        const auto& row = c.prog_view[n];
        if (row.file >= 0 && row.file == ui.prog_at) ui.prog_view_own = int16_t(n);
        if (picked < 0 && row.file < 0 && !select.empty() && row.name == select) picked = int(n);
    }
    ui.prog_cursor = uint16_t(picked >= 0 ? picked : std::max<int>(ui.prog_view_own, 0));
    std::snprintf(ui.prog_folder_name, sizeof(ui.prog_folder_name), "%s", prog_folder_name(folder).c_str());
}

bool prog_files_open(Context& c) {
    if (!prog_files(c)) return false;
    const int at = c.ui.prog_at;
    prog_view_open(c, at >= 0 && size_t(at) < c.prog_places.size() ? prog_place_folder(c.prog_places[size_t(at)]) : std::string());
    return true;
}

bool prog_view_enter(Context& c) {
    const unsigned at = c.ui.prog_cursor;
    if (at >= c.prog_view.size()) return false;
    const auto row = c.prog_view[at];
    if (row.file < 0) { prog_view_open(c, c.prog_folder.empty() ? row.name : c.prog_folder + "/" + row.name); return true; }
    if (prog_pick(c, unsigned(row.file))) c.notice(std::string(c.track().prog.file).substr(0, 28));
    return false;
}

bool prog_view_up(Context& c) {
    if (c.prog_folder.empty()) return false;
    const std::string from = prog_folder_name(c.prog_folder);
    prog_view_open(c, prog_folder_up(c.prog_folder), from);
    return true;
}

bool prog_pick(Context& c, unsigned at) {
    if (at >= c.prog_paths.size() || !load(c, at)) return false;
    show(c);
    return true;
}

void prog_save(Context& c, unsigned pad, unsigned index) {
    auto& t = c.track();
    std::string folder = own_progression_folder(card_of(c));
    if (!t.prog.file[0]) std::snprintf(t.prog.file, sizeof(t.prog.file), "%s", fresh_name(folder).c_str());
    if (!t.prog.name[0]) std::memcpy(t.prog.name, t.prog.file, sizeof(t.prog.name));
    c.dirty = true;
    const auto shown = prog_pad_name(t, index); // as the pad names it, in the key K2 KEY moved it to
    const std::string chord = shown.bass.empty() ? shown.chord : shown.chord + "/" + shown.bass;
    if (!folder.empty() && write_progression(folder + "/" + t.prog.file + ".progression", t.prog)) c.notice("Pad " + std::to_string(pad + 1) + ": " + chord);
    else c.notice("Not saved: no card");
    c.prog_paths.clear(); // listed again on the next wheel turn, with this one
}

bool prog_can_store(const Context& c) {
    const auto& t = c.project.tracks[c.ui.track % track_count];
    if (c.ui.prog_storing || !t.used() || t.engine == EngineKind::Sampler || t.levels) return false;
    if (t.layout != PadLayout::Scale && t.layout != PadLayout::Chromatic && t.layout != PadLayout::Chords) return false;
    return c.ui.held_pads != 0;
}

void prog_store_begin(Context& c) {
    auto& ui = c.ui;
    ui.prog_store_count = 0;
    for (const auto& pad : c.pad_keys)
        for (auto k : pad) {
            if (k < 0 || ui.prog_store_count >= ui.prog_store_keys.size()) continue;
            bool seen = false;
            for (unsigned n = 0; n < ui.prog_store_count; ++n) seen |= ui.prog_store_keys[n] == k;
            if (!seen) ui.prog_store_keys[ui.prog_store_count++] = uint8_t(k & 0x7f);
        }
    if (!ui.prog_store_count) return;
    auto& t = c.track();
    ui.prog_storing = true;
    ui.prog_store_from = uint8_t(t.layout);
    t.layout = PadLayout::Prog;
    if (t.prog.empty() && !t.prog.file[0]) t.bank = 0; // a new progression: from bank A
    c.dirty = true;
}

void prog_store_back(Context& c) {
    auto& ui = c.ui;
    if (!ui.prog_storing) return;
    ui.prog_storing = false;
    c.track().layout = PadLayout(std::min<unsigned>(ui.prog_store_from, unsigned(PadLayout::Prog) - 1));
}

bool prog_store_pad(Context& c, unsigned pad) {
    auto& ui = c.ui;
    auto& t = c.track();
    if (!ui.prog_storing || !prog_pads(t)) return false;
    std::array<int, notes_per_step> keys {};
    for (unsigned n = 0; n < ui.prog_store_count; ++n) keys[n] = ui.prog_store_keys[n];
    // Stored as heard: the key K1 and the octave move the pads by are taken off again.
    int move = t.prog_shift + t.transpose + 12 * (t.octave - 2);
    for (unsigned n = 0; n < ui.prog_store_count; ++n) keys[n] -= move;
    unsigned index = prog_index(t, pad);
    if (!store_prog_chord(t.prog, index, keys.data(), ui.prog_store_count)) return false;
    ui.prog_storing = false;
    c.dirty = true;
    std::string folder = own_progression_folder(card_of(c));
    if (!t.prog.file[0]) std::snprintf(t.prog.file, sizeof(t.prog.file), "%s", fresh_name(folder).c_str());
    static_assert(sizeof(t.prog.name) == sizeof(t.prog.file), "name and file the same size");
    if (!t.prog.name[0]) std::memcpy(t.prog.name, t.prog.file, sizeof(t.prog.name));
    if (!folder.empty() && write_progression(folder + "/" + t.prog.file + ".progression", t.prog)) c.notice("Pad " + std::to_string(pad + 1) + ": " + t.prog.chords[index].name);
    else c.notice("Not saved: no card");
    c.prog_paths.clear(); // listed again on the next wheel turn, with this one
    return true;
}

} // namespace melodee
