// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/context.h"
#include "melodee/engine/engine_host.h"
#include "melodee/engine/print_player.h"
#include "melodee/plugin/plugin_rack.h"
#include "melodee/shell/print_engine.h"
#include "melodee/shell/sample_library.h"
#include <filesystem>

// Freeze plan F2a: prints asked for (for now by `melodee-print`, before any page has them) and their records kept.
namespace melodee {

void Context::print(const PrintAsk& ask) {
    if (ask.mode == PrintAsk::Freeze) { freeze(ask); return; } // freeze plan F2b, `melodee-freeze` and `melodee-unfreeze`
    if (ask.mode == PrintAsk::Unfreeze) { unfreeze(ask.track); return; }
    if (ask.mode == PrintAsk::AutoSample) {
        if (ask.check) { if (!autosample_check(ask.track, ask.sampling)) notice("A print is under way"); }
        else autosample(ask.track, ask.sampling, ask.probe_first);
        return;
    }
    if (!printer) return;
    if (project_path.empty()) { printer->note("save the project first: its prints go beside its file"); return; }
    const unsigned t = ask.track % track_count;
    PrintJob job;
    job.project_path = project_path;
    job.track = t;
    job.tap = ask.tap;
    job.tail = ask.tail;
    for (unsigned p = 0; p < pattern_count; ++p) {
        bool asked = (ask.patterns[p / 64] >> (p % 64)) & 1;
        if (ask.all ? !patterns.at[t][p].empty() : asked) job.patterns.push_back(uint8_t(p));
    }
    bool none = !ask.all;
    for (auto bits : ask.patterns) none &= bits == 0;
    if (none) job.patterns.push_back(project.tracks[t].pattern); // the one it plays
    if (job.patterns.empty()) { printer->note("track " + std::to_string(t + 1) + " has no pattern with notes"); return; }
    job.project = std::make_unique<Project>(project);
    if (ui.chop_sample < sample_slots && ui.slices.count) job.project->chops.edit(ui.chop_sample).slices = ui.slices; // slice hits as Chop has them (issue 24)
    job.bank = std::make_unique<PatternBank>(patterns);
    if (plugins) job.states = plugins->states();
    if (library) job.card_top = library->card();
    job.known = prints;
    if (!printer->start(std::move(job))) printer->note("busy: a print is under way");
}

bool Context::freeze(const PrintAsk& ask) {
    if (!printer) return false;
    const unsigned t = ask.track % track_count;
    const auto& tr = project.tracks[t];
    if (clock_thawing & (1u << t)) { notice("Thawing track " + std::to_string(t + 1)); return false; }
    if (project_path.empty()) { printer->note("save the project first: its prints go beside its file"); return false; }
    if (!tr.used() || tr.engine == EngineKind::Midi) { printer->note("track " + std::to_string(t + 1) + " has no sound to freeze"); return false; }
    if (tr.engine == EngineKind::Sampler) // a print holds the kit, not pads sent to submixes
        for (const auto& pad : tr.pads)
            if (pad.used() && pad.out) { printer->note("track " + std::to_string(t + 1) + " has pads sent to submixes: set them to KIT to freeze it"); return false; }
    TrackFreeze f;
    f.state = ask.deep ? FreezeState::Deep : FreezeState::Frozen;
    f.tap = ask.deep ? PrintTap::PostFx : ask.tap; // freeze plan F2d: the chain's plug-ins go too, so the print holds them
    f.tail = ask.tail;
    bool none = !ask.all;
    for (auto bits : ask.patterns) none &= bits == 0;
    for (unsigned p = 0; p < pattern_count; ++p) // a deep freeze: the whole track, as nothing of it plays live
        if (!patterns.at[t][p].empty() && (none || ask.all || ask.deep || ((ask.patterns[p / 64] >> (p % 64)) & 1))) f.set(p, true);
    bool any = false;
    for (auto bits : f.patterns) any |= bits != 0;
    if (!any) { printer->note("track " + std::to_string(t + 1) + " has no pattern with notes to freeze"); return false; }
    request_freeze(t, f);
    return true;
}

void Context::request_freeze(unsigned t, const TrackFreeze& selection) {
    const uint32_t bit = 1u << t;
    finish_freeze_sheet(t);
    if (++freeze_serial[t] == 0) ++freeze_serial[t]; // zero means no active pack in the engine
    freeze_requested[t] = selection;
    freeze_waiting |= bit;
    unfreeze_waiting &= ~bit;
    thawing &= ~bit;
    clock_thawing &= ~bit;
    clock_live_waiting &= ~bit;
}

void Context::finish_freeze_sheet(unsigned t) {
    auto& sheet = ui.print_sheet;
    if (!sheet.printing || sheet.track != t || sheet.serial != freeze_serial[t]) return;
    sheet.printing = false;
    if (ui.page == PageId::Print) back();
}

void Context::cancel_freeze_request(unsigned t) {
    finish_freeze_sheet(t);
    freeze_requested[t] = {};
    freeze_waiting &= ~(1u << t);
}

void Context::reject_freeze(unsigned t, const std::string& error) {
    const uint32_t bit = 1u << t;
    if (freeze_restoring & bit) {
        // A project or track load dropped the old pack, so there is no old audio to keep.
        freeze_restoring &= ~bit;
        deep_hold &= ~bit;
        deep_serial[t] = deep_active_at[t] = 0;
        prints.freeze[t] = {};
        print_peaks[t].clear();
        dirty = true;
    }
    notice("Track " + std::to_string(t + 1) + (frozen(t) ? " kept: " : " live: ") + error);
}

void Context::clear_submitted_freeze(unsigned t) {
    freeze_submitted[t] = {};
    freeze_submitted_serial[t] = 0;
    freeze_submitted_peaks[t].clear();
}

void Context::confirm_freeze(unsigned t) {
    const uint32_t bit = 1u << t;
    const auto selection = freeze_submitted[t];
    const bool hold = selection.state == FreezeState::Deep && (!deep(t) || (deep_hold & bit));
    if (hold) {
        deep_hold |= bit;
        deep_serial[t] = freeze_submitted_serial[t];
        deep_active_at[t] = now ? now : 1;
    } else {
        deep_hold &= ~bit;
        deep_serial[t] = deep_active_at[t] = 0;
    }
    dirty |= !(prints.freeze[t] == selection);
    prints.freeze[t] = freeze_last[t] = selection;
    print_peaks[t] = std::move(freeze_submitted_peaks[t]);
    clear_submitted_freeze(t);
    if (!(freeze_restoring & bit))
        notice("Track " + std::to_string(t + 1) + (deep(t) ? " deep frozen" : " frozen"));
    freeze_restoring &= ~bit;
}

uint32_t Context::plugins_asleep() const {
    uint32_t out = 0;
    for (unsigned t = 0; t < track_count; ++t) {
        const uint32_t bit = 1u << t;
        if (deep(t) && !((deep_hold | thawing) & bit) && freeze_requested[t].state != FreezeState::Frozen &&
            freeze_submitted[t].state != FreezeState::Frozen) out |= bit;
    }
    return out;
}

void Context::refreeze(unsigned track) {
    const unsigned t = track % track_count;
    cancel_freeze_request(t);
    clear_submitted_freeze(t);
    if (!frozen(t)) return;
    // A track load discards the engine's old pack. If the replacement fails there is no pack to fall back to.
    freeze_restoring |= 1u << t;
    if (deep(t)) {
        deep_hold |= 1u << t;
        deep_serial[t] = 0;
        deep_active_at[t] = 0;
    }
    request_freeze(t, prints.freeze[t]);
}

void Context::unfreeze(unsigned track) {
    const unsigned t = track % track_count;
    const uint32_t bit = 1u << t;
    const std::string name = "Track " + std::to_string(t + 1);
    if (clock_live_waiting & bit) return; // the ready plug-ins are already waiting for their first live pass
    cancel_freeze_request(t);
    clock_thawing &= ~bit;
    // A sleeping plug-in reloads beside its prints and goes live once ready (thaw_tick).
    if (deep(t) && !(deep_hold & bit)) {
        if (!(thawing & bit)) { thawing |= bit; thaw_since[t] = now ? now : 1; thaw_ready[t] = 0; }
        notice("Thawing track " + std::to_string(t + 1));
    } else if (go_live(t)) {
        notice(name + " live");
    } else {
        unfreeze_waiting |= bit;
        notice(name + ": waiting to unfreeze");
    }
}

bool Context::go_live(unsigned t) {
    if ((frozen(t) || freeze_submitted_serial[t]) && !host.send({Command::Type::Freeze, uint8_t(t)})) return false;
    finish_unfreeze(t);
    return true;
}

void Context::finish_unfreeze(unsigned t) {
    clear_submitted_freeze(t);
    const uint32_t bit = 1u << t;
    deep_hold &= ~bit;
    thawing &= ~bit;
    clock_thawing &= ~bit;
    clock_live_waiting &= ~bit;
    freeze_restoring &= ~bit;
    unfreeze_waiting &= ~bit;
    deep_serial[t] = deep_active_at[t] = 0;
    dirty |= frozen(t);
    prints.freeze[t] = {};
    print_peaks[t].clear();
}

void Context::print_tick() {
    audio_tick(); // freeze plan F3: audio tracks' regions and takes (shell/audio_jobs.cpp)
    if (!printer) return;
    autosample_tick();
    const uint32_t lost = host.print_underruns();
    if (lost != last_print_underruns) {
        if (!last_print_underruns || now - last_print_loss_at > 3000) notice("Frozen playback missed audio");
        last_print_loss_at = now;
        last_print_underruns = lost;
    }
    // Freeze plan F2b: packs the audio thread is done with, freed on the print thread; prints ready to play, sent to
    // switch on the next bar; and the next track waiting for its prints, started once the print engine is free.
    FreezePack* done = nullptr;
    while (host.take_done_freeze(done)) {
        const unsigned t = done->track % track_count;
        if (done->clock_rejected && freeze_submitted_serial[t] == done->serial && done->serial) {
            clear_submitted_freeze(t);
            reject_freeze(t, "clock changed");
        }
        printer->discard(done);
    }
    for (unsigned t = 0; t < track_count; ++t) {
        if (freeze_submitted_serial[t] && freeze_submitted_serial[t] == host.active_freeze_serial(t)) confirm_freeze(t);
        if (((unfreeze_waiting >> t) & 1) && go_live(t))
            notice("Track " + std::to_string(t + 1) + " live");
        const uint32_t bit = 1u << t;
        const uint32_t clock = host.freeze_clock_changed(t);
        if (clock && clock != clock_thaw_seen[t] && clock == host.active_freeze_serial(t) &&
            deep(t) && !freeze_pending(t) && !((thawing | unfreeze_waiting) & bit)) {
            clock_thaw_seen[t] = clock;
            unfreeze(t);
            clock_thawing |= thawing & bit;
        }
    }
    bool delivery_blocked = false;
    FreezeReady ready;
    while (printer->take_ready(ready)) {
        const unsigned t = ready.track % track_count;
        const bool wanted = ready.project_path == project_path && ready.serial == freeze_serial[t] && freeze_requested[t].frozen();
        if (!wanted) {
            printer->discard(ready.pack);
            continue;
        }
        if (!ready.pack) {
            cancel_freeze_request(t);
            reject_freeze(t, ready.error);
            continue;
        }
        Command c {Command::Type::Freeze, uint8_t(t), 0, 0, 1}; // on the next bar while playing
        c.freeze = ready.pack;
        if (!host.send(c)) {
            printer->return_ready(std::move(ready));
            delivery_blocked = true;
            break;
        }
        freeze_submitted[t] = freeze_requested[t];
        freeze_submitted_serial[t] = ready.serial;
        freeze_submitted_peaks[t] = std::move(ready.peaks);
        cancel_freeze_request(t);
        if (host.playing() && !(freeze_restoring & (1u << t)))
            notice("Track " + std::to_string(t + 1) + " waiting for bar");
    }
    // Start both grace periods only when this request's pack is active, even if an earlier freeze was already playing.
    for (unsigned t = 0; t < track_count; ++t) {
        const uint32_t bit = 1u << t;
        if (!(deep_hold & bit) || !deep_serial[t] || host.active_freeze_serial(t) != deep_serial[t]) continue;
        auto& active = deep_active_at[t];
        if (!active) active = now ? now : 1;
        const uint32_t since = now ? now - active : 0;
        if (((host.sleeping_tracks() & bit) && since >= 200) || since >= 8000) deep_hold &= ~bit;
    }
    thaw_tick();
    uint32_t deep_bits = 0;
    for (unsigned t = 0; t < track_count; ++t) deep_bits |= uint32_t(deep(t)) << t;
    deep_now.store(deep_bits, std::memory_order_relaxed);
    thawing_now.store(thawing, std::memory_order_relaxed);
    thaw_took_now.store(thaw_took, std::memory_order_relaxed);
    uint32_t waiting = freeze_waiting;
    for (unsigned t = 0; t < track_count; ++t)
        if (freeze_submitted_serial[t]) waiting &= ~(1u << t);
    if (waiting && !delivery_blocked && !printer->busy() && !project_path.empty()) {
        unsigned t = 0;
        while (!((waiting >> t) & 1)) ++t;
        const auto& f = freeze_requested[t];
        PrintJob job;
        job.project_path = project_path;
        job.track = t;
        job.tap = f.tap;
        job.tail = f.tail;
        job.freeze = true;
        job.deep = f.state == FreezeState::Deep; // freeze plan F2d
        job.serial = freeze_serial[t];
        for (unsigned p = 0; p < pattern_count; ++p)
            if (f.plays(p) && !patterns.at[t][p].empty()) job.patterns.push_back(uint8_t(p));
        if (job.patterns.empty()) {
            cancel_freeze_request(t);
            reject_freeze(t, "no pattern with notes");
        } else {
            job.project = std::make_unique<Project>(project);
            if (ui.chop_sample < sample_slots && ui.slices.count) job.project->chops.edit(ui.chop_sample).slices = ui.slices; // issue 24
            job.bank = std::make_unique<PatternBank>(patterns);
            if (plugins) job.states = plugins->states();
            if (library) job.card_top = library->card();
            job.known = prints;
            if (printer->start(std::move(job))) freeze_waiting &= ~(1u << t);
        }
    }
    PrintResult r;
    while (printer->take(r)) {
        if (r.project_path != project_path || !r.error.empty()) continue; // another project's, or not made
        const auto* known = prints.find(r.record.track, r.record.pattern);
        if (known && *known == r.record) continue;
        prints.put(r.record);
        dirty = true; // its record goes with the project
    }
}

// Freeze plan F2d: each thawing track goes live once every plug-in it uses (its own and its chain's) has been open
// for 1.5 s (the audio thread has taken it, and work a plug-in does in the background after loading its state, such
// as TONE3000 preparing its models, is done before it plays: done while it played, it cost xruns on the MPC); one
// that fails to open, or 30 s without, an automatic clock thaw keeps the print. An explicit unfreeze goes live.
void Context::thaw_tick() {
    for (unsigned t = 0; t < track_count; ++t) {
        const uint32_t bit = 1u << t;
        if (!(thawing & bit)) continue;
        const std::string name = "Track " + std::to_string(t + 1);
        if (clock_live_waiting & bit) {
            if (!host.active_freeze_serial(t)) {
                finish_unfreeze(t);
                thaw_took = now - thaw_since[t];
                notice(name + " live");
            }
            continue;
        }
        if (!deep(t)) { thawing &= ~bit; clock_thawing &= ~bit; continue; }
        bool ready = true, failed = false;
        if (plugins) {
            auto check = [&](unsigned entry) {
                if (entry >= max_plugins || !project.plugins[entry].used()) return;
                auto st = plugins->status(entry);
                if (st == PluginStatus::Failed || st == PluginStatus::Crashed) failed = true;
                else if (st != PluginStatus::Ready || plugins->asleep(entry)) ready = false;
            };
            const auto& tr = project.tracks[t];
            if (tr.engine == EngineKind::Plugin) check(tr.plugin);
            for (const auto& slot : tr.fx.slots) if (slot.kind == plugin_effect_kind) check(unsigned(slot.params[0]));
        }
        if (failed || now - thaw_since[t] > 30000) {
            if (clock_thawing & bit) {
                thawing &= ~bit;
                clock_thawing &= ~bit;
                notice(name + " kept: plug-in offline");
                continue;
            }
            if (go_live(t)) notice(name + " live: a plug-in did not open");
            continue;
        }
        if (!ready) { thaw_ready[t] = 0; continue; }
        if (!thaw_ready[t]) { thaw_ready[t] = now ? now : 1; continue; }
        if (now - thaw_ready[t] < 1500) continue;
        if (clock_thawing & bit) {
            if (host.send({Command::Type::Freeze, uint8_t(t), 0, 0, 1})) clock_live_waiting |= bit;
        } else if (go_live(t)) {
            thaw_took = now - thaw_since[t];
            notice(name + " live");
        }
    }
}

} // namespace melodee
