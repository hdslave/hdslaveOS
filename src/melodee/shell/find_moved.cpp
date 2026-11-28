// SPDX-License-Identifier: AGPL-3.0-only
// hdslaveOS: find moved files (shell/find_moved.h).
#include "melodee/shell/find_moved.h"
#include "melodee/shell/ask.h"
#include "melodee/shell/card_access.h"
#include "melodee/shell/context.h"
#include "melodee/shell/mpc_wav_slices.h"
#include "melodee/shell/sample_library.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace melodee {

namespace {
// "1 sample missing", "12 samples missing" (a notice holds 31 characters).
std::string samples_text(unsigned n, const char* what) { return std::to_string(n) + (n == 1 ? " sample " : " samples ") + what; }
// The question waits for the Loading sheet and any other sheet over the page; playback does not stop it (decision 4).
bool may_ask(const UiState& ui) {
    return !ui.loading && ui.ask == UiState::Ask::None && !ui.name.open() && !ui.exit_open && !card_owns_controls(ui) && !ui.update.phase
        && !ui.sheet_open && !ui.lesson;
}
} // namespace

FindMoved::FindMoved() {
    worker.read_chop = [](const std::string& path) { return read_mpc_slices(path); };
    worker.current(generation); // the starting project's too (Load Kit into it before any project opens)
}

void FindMoved::switched() {
    ++generation;
    worker.current(generation); // what is left of the last project's jobs is dropped; a walk under way goes on
    not_found.clear();
    added_slots.clear();
    origin.clear(); added_origin.clear();
    check_due = ask_due = false;
    checks_open = 0;
    phase = Phase::Idle;
    hidden = cancelling = false;
    asked = result_found = 0;
}

void FindMoved::opened(const std::string& from, uint32_t now) {
    origin = from;
    check_due = true; // once the library knows the card (at start it may not yet)
    due_at = now;
}

bool FindMoved::missing(unsigned slot) const { return std::binary_search(not_found.begin(), not_found.end(), uint16_t(slot)); }

void FindMoved::set_missing(unsigned slot, bool on) {
    auto at = std::lower_bound(not_found.begin(), not_found.end(), uint16_t(slot));
    const bool there = at != not_found.end() && *at == slot;
    if (on && !there) not_found.insert(at, uint16_t(slot));
    if (!on && there) not_found.erase(at);
}

void FindMoved::post_check(Context& c, const std::vector<uint16_t>& slots, const std::string& from, bool quiet) {
    MovedFiles::Job job;
    job.quiet = quiet;
    job.generation = generation;
    job.origin = from;
    job.card = card;
    // The slots a pad plays (a layer of a kit pad, a keygroup's): one no pad plays (FORGET, a sample taken off its pad)
    // is not asked about; the save frees it (Context::free_unused_samples).
    std::vector<bool> played(sample_slots);
    for (const auto& t : c.project.tracks)
        for (const auto& pad : t.pads) {
            if (pad.used()) played[pad.sample % sample_slots] = true;
            for (const auto& l : pad.layers) if (l.used()) played[l.sample % sample_slots] = true;
        }
    for (uint16_t n : slots) {
        const auto& ref = c.project.samples[n % sample_slots];
        if (ref.used() && ref.path[0] == '/' && played[n % sample_slots]) job.slots.push_back({uint16_t(n % sample_slots), ref.path});
    }
    if (job.slots.empty()) return;
    worker.post(std::move(job));
    ++checks_open;
}

void FindMoved::tick(Context& c, uint32_t now) {
    if (!c.library) return;
    // SD card access: nothing of Melodee's reads the card while it is lent. A search stops (MelodeeApp stops it too as it
    // lends); checks wait until the card is back.
    const bool lent = card_owns_controls(c.ui);
    if (lent && phase == Phase::Searching) cancel(c);
    // The card's top, as the library found it, every half second (and while a check waits for it). Another card, or
    // none: the index of this session is no longer the card's.
    if (check_due || now - card_at >= 500) {
        card_at = now;
        card = c.library->card();
        if (card != card_seen) {
            if (!card_seen.empty()) worker.drop_index();
            card_seen = card;
        }
    }
    if (check_due && !lent && (!card.empty() || now - due_at >= 3000)) {
        check_due = false;
        std::vector<uint16_t> slots;
        for (unsigned n = 0, span = unsigned(c.project.samples.span()); n < span; ++n)
            if (c.project.samples[n].used()) slots.push_back(uint16_t(n));
        post_check(c, slots, origin);
    }
    if (!added_slots.empty() && !check_due && !lent) { // Load Kit, Load Bank: their slots as the project's were
        std::sort(added_slots.begin(), added_slots.end());
        added_slots.erase(std::unique(added_slots.begin(), added_slots.end()), added_slots.end());
        post_check(c, added_slots, added_origin.empty() ? origin : added_origin);
        added_slots.clear();
    }
    if (!quiet_slots.empty() && !check_due && !lent) {
        post_check(c, quiet_slots, origin, true);
        quiet_slots.clear();
    }
    MovedFiles::Answer a;
    while (worker.take(a)) take(c, a);
    if (ask_due && may_ask(c.ui)) { // the question, or with Always the search's sheet at once
        ask_due = false;
        if (!not_found.empty() && phase != Phase::Searching) {
            if (c.ui.settings.find_moved == Settings::FindAlways) search(c, false);
            else ask(c, UiState::Ask::FindMissing, int(not_found.size()));
        }
    }
    // What the sheets and the Samples page show.
    auto& v = c.ui.find;
    const auto p = worker.progress();
    v.missing = uint16_t(std::min<size_t>(not_found.size(), 0xffff));
    v.searching = phase == Phase::Searching;
    v.hidden = hidden;
    v.asked = asked;
    v.found = phase == Phase::Searching ? uint16_t(std::min<uint32_t>(p.matched, asked)) : result_found;
    v.looked = p.looked;
    const auto& t = c.project.tracks[c.ui.track % track_count];
    const auto& pad = t.pads[t.last_pad % sampler_pads];
    v.pad_missing = t.engine == EngineKind::Sampler && pad.used() && missing(pad.sample);
}

void FindMoved::take(Context& c, const MovedFiles::Answer& a) {
    if (a.generation != generation) return; // from before another project was opened
    if (a.kind == MovedFiles::Answer::Kind::Done) { done(c, a); return; }
    const unsigned slot = a.slot % sample_slots;
    const auto& ref = c.project.samples[slot];
    if (a.saved != ref.path) return; // the slot changed since it was asked about
    if (a.kind == MovedFiles::Answer::Kind::Missing || a.found.size() >= sizeof(SampleRef::path)) { set_missing(slot, true); return; }
    // Found: the slot names the file where it is now (the next save writes it), loads it as Load Kit does and takes the
    // file's slices as its chop, as a project's switch does (shell/lights.cpp).
    set_missing(slot, false);
    if (a.found != a.saved) std::snprintf(c.project.samples.edit(slot).path, sizeof(SampleRef::path), "%s", a.found.c_str());
    if (!c.project.chops[slot].unwritten) c.project.chops.edit(slot) = a.chop;
    if (slot < c.samples_to_load && !c.library->load(slot, ref.path, ref.root == root_unread)) c.slots_waiting.push_back(uint16_t(slot));
    ++c.heavy_serial;
}

void FindMoved::done(Context& c, const MovedFiles::Answer& a) {
    if (!a.search) {
        if (checks_open) --checks_open;
        if (a.quiet || !a.missing_count || not_found.empty()) return; // all of this check's in place or found
        const unsigned n = unsigned(not_found.size());
        if (a.indexed) { c.notice(samples_text(n, "not found")); return; } // this session's search answered: nothing to ask
        if (c.ui.settings.find_moved == Settings::FindOff) c.notice(samples_text(n, "missing"));
        else ask_due = true; // asked (Always: searched) once the page is free of other sheets
        return;
    }
    phase = Phase::Idle;
    cancelling = false;
    const bool sheet = c.ui.ask == UiState::Ask::FindSearching;
    if (sheet) c.ui.ask = UiState::Ask::None;
    const bool shown = sheet || hidden;
    hidden = false;
    result_found = uint16_t(std::min<unsigned>(a.found_count, 0xffff));
    if (a.cancelled) { // what the part walked found is in; the rest stay NOT FOUND
        if (shown) c.notice(a.found_count ? "Search stopped, found " + std::to_string(a.found_count) : std::string("Search stopped"));
        return;
    }
    if (a.too_big) { c.notice("Card too big to search"); return; }
    if (!a.missing_count) c.notice(a.found_count == 1 ? std::string("Found 1 sample") : "Found " + std::to_string(a.found_count) + " samples");
    else if (sheet) ask(c, UiState::Ask::FindResult, int(a.missing_count)); // KEEP or FORGET
    else c.notice(samples_text(a.missing_count, "not found")); // after HIDE: FORGET is on the Samples page
}

void FindMoved::search(Context& c, bool fresh) {
    if (!c.library) return;
    MovedFiles::Job job;
    job.search = true;
    job.generation = generation;
    job.origin = origin;
    job.card = card = c.library->card();
    job.internal = SampleLibrary::internal_tops();
    job.fresh = fresh;
    for (uint16_t n : not_found) {
        const auto& ref = c.project.samples[n];
        if (ref.used()) job.slots.push_back({n, ref.path});
    }
    if (job.slots.empty()) { c.notice("No samples missing"); return; }
    asked = uint16_t(std::min<size_t>(job.slots.size(), 0xffff));
    result_found = 0;
    phase = Phase::Searching;
    hidden = cancelling = false;
    worker.post(std::move(job));
    ask(c, UiState::Ask::FindSearching, asked);
    c.ui.find.searching = true; c.ui.find.asked = asked; c.ui.find.found = 0; // drawn at once, before the next pass
}

void FindMoved::cancel(Context& c) {
    if (phase != Phase::Searching || cancelling) return;
    cancelling = true;
    worker.cancel(); // its answer says so: "Search stopped"
    hidden = true;   // the notice comes though the sheet is down now
    if (c.ui.ask == UiState::Ask::FindSearching) c.ui.ask = UiState::Ask::None;
}

void FindMoved::hide(Context& c) {
    hidden = phase == Phase::Searching;
    if (c.ui.ask == UiState::Ask::FindSearching) c.ui.ask = UiState::Ask::None;
}

void FindMoved::forget(Context& c) {
    if (not_found.empty()) return;
    auto f = std::make_unique<ForgetSamples>();
    for (uint16_t n : not_found) f->slots.push_back({n, c.project.samples[n].path});
    for (unsigned t = 0; t < track_count; ++t) {
        const auto& tr = c.project.tracks[t];
        for (unsigned k = 0; k < sampler_pads; ++k) {
            const auto& pad = tr.pads[k];
            bool hit = pad.used() && missing(pad.sample);
            for (const auto& l : pad.layers) hit = hit || (l.used() && missing(l.sample));
            if (hit) f->pads.push_back({uint8_t(t), uint8_t(k), pad_state(tr, k)});
        }
    }
    const unsigned n = unsigned(not_found.size());
    Edit edit;
    edit.what = n == 1 ? std::string("Forget 1 sample") : "Forget " + std::to_string(n) + " samples";
    edit.track = uint8_t(c.ui.track % track_count);
    forgot(c, *f, true);
    edit.forget = std::move(f);
    c.edits.push(std::move(edit));
    c.dirty = true; // its next save, by hand or in the background, has no pad on them
    c.notice(n == 1 ? std::string("Forgot 1 sample") : "Forgot " + std::to_string(n) + " samples");
}

void FindMoved::forgot(Context& c, const ForgetSamples& f, bool after) {
    // A slot of this step that still names the path it named then (a search may have found its file elsewhere since)
    // and, done again, is still not found (nor copied back where it was).
    auto named = [&](uint16_t slot) {
        for (const auto& [n, path] : f.slots) if (n == slot) return path == c.project.samples[slot].path;
        return false;
    };
    auto forgotten = [&](uint16_t slot) { return named(slot) && missing(slot); };
    if (after) { // its pads let go of it, all else of theirs kept
        for (const auto& p : f.pads) {
            PadState now = pad_state(c.project.tracks[p.track % track_count], p.pad);
            bool changed = false;
            if (now.sound.used() && forgotten(now.sound.sample)) { now.sound.sample = no_sample; changed = true; }
            for (auto& l : now.sound.layers) if (l.used() && forgotten(l.sample)) { l = PadLayer {}; changed = true; }
            if (changed) c.restore_pad(p.track, p.pad, now);
        }
        for (const auto& [n, path] : f.slots) if (named(n)) set_missing(n, false);
        return;
    }
    // Undone: as they were, their slots looked at again (NOT FOUND again, or found meanwhile), with nothing asked.
    for (const auto& p : f.pads) c.restore_pad(p.track, p.pad, p.before);
    for (const auto& [n, path] : f.slots) if (named(n)) quiet_slots.push_back(n);
}

} // namespace melodee
