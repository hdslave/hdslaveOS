// SPDX-License-Identifier: AGPL-3.0-only
#include "melodee/engine/print_render.h"
#include "melodee/pages/page.h" // draw_name_sheet: name-target draws the sheet
#include "melodee/pages/pages.h"
#include "melodee/plugin/plugin_rack.h"
#include "melodee/plugin/plugin_files.h" // clap_id_hash: the fixture's effect by name, for the fx modes
#include "melodee/plugin/preset_rows.h"
#include "melodee/shell/features.h" // stage_skip: the page Stage shows
#include "melodee/shell/shell.h"
#include "melodee/shell/stage_map.h"
#include "melodee/shell/print_engine.h"
#include "melodee/shell/project_store.h"
#include "melodee/shell/track_copy.h"
#include "runtime/controller.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <sys/resource.h>
#include <thread>

using namespace melodee;
namespace fs = std::filesystem;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)

static void run_job(PrintEngine& printer, PrintJob job) {
    CHECK(printer.start(std::move(job)));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (printer.busy() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(!printer.busy());
}

static SampleData print_wav(PrintEngine& printer, PrintJob job) {
    const auto folder = prints_folder(job.project_path);
    run_job(printer, std::move(job));
    PrintResult result;
    CHECK(printer.take(result) && result.made && result.error.empty());
    SampleData wav;
    CHECK(read_wav((fs::path(folder) / result.record.file).string(), wav).empty());
    return wav;
}

// The last step feeds a fully wet quarter-note Echo. Empty bars let the same note and its
// effects decay inside the pattern, independently of the shorter print's Ring cutoff.
static void native_ring_wav(const std::string& card, const std::string& plugin) {
    PrintEngine printer(nullptr);
    auto print = [&](unsigned bars, EffectKind kind) {
        PrintJob job;
        job.card_top = card;
        job.project_path = (fs::path(card) / (std::to_string(bars) + ".mldp")).string();
        job.patterns = {0}; job.tail = PrintTail::Ring; job.tap = PrintTap::PostFx;
        job.project = std::make_unique<Project>();
        job.bank = std::make_unique<PatternBank>();
        job.project->tempo = 1200;
        auto& track = job.project->tracks[0];
        load_sound(track, EngineKind::Plugin, 0);
        track.plugin = 0;
        std::snprintf(job.project->plugins[0].path, sizeof(PluginRef::path), "%s", plugin.c_str());
        track.fx.slots[0] = {uint8_t(EffectKind::Echo), false, {60, 0, 100, 60, 9, 0}};
        if (kind == EffectKind::Spring) track.fx.slots[1] = {uint8_t(kind), false, {250, 25, 100, 70, 60, 120}};
        else if (kind == EffectKind::Looper) track.fx.slots[0] = {uint8_t(kind), false, {74, 1, 0, 100, 0, 0}};
        else if (kind != EffectKind::Echo) {
            job.project->tempo = 1500;
            track.fx.slots[1] = {uint8_t(kind), false, {100, 75, 30, 100, 100, 100}};
        }
        auto& pattern = job.bank->at[0][0];
        pattern.length = bars * 16 * sixteenth_ticks;
        Step step;
        step.notes[0] = {69, 110};
        pattern.set_step(15, step);

        return print_wav(printer, std::move(job));
    };
    for (auto kind : {EffectKind::Echo, EffectKind::Spring, EffectKind::Room, EffectKind::Reverb,
                      EffectKind::Plate, EffectKind::Shimmer, EffectKind::Looper}) {
        const auto reference = print(4, kind);
        const auto ring = print(1, kind);
        const double pass = kind == EffectKind::Echo || kind == EffectKind::Spring || kind == EffectKind::Looper ? 2.0 : 1.6;
        const size_t after_pass = size_t(pass * dsp::sample_rate());
        CHECK(ring.frames > after_pass && ring.frames < reference.frames);
        const float expected_peak = kind == EffectKind::Echo || kind == EffectKind::Spring ? 0.4f : 0.05f;
        CHECK(*std::max_element(ring.left.begin() + after_pass, ring.left.end()) > expected_peak);
        CHECK(std::equal(ring.left.begin(), ring.left.end(), reference.left.begin()));
        CHECK(std::equal(ring.right.begin(), ring.right.end(), reference.right.begin()));
        auto quiet = [kind](float v) { return kind == EffectKind::Echo ? v == 0 : std::fabs(v) < 0.000316f; };
        CHECK(std::all_of(reference.left.begin() + ring.frames, reference.left.end(), quiet));
        CHECK(std::all_of(reference.right.begin() + ring.frames, reference.right.end(), quiet));
        if (kind == EffectKind::Shimmer) {
            setenv("MELODEE_TEST_SLOW_OPEN", "1", 1);
            const auto slow = print(1, kind);
            unsetenv("MELODEE_TEST_SLOW_OPEN");
            CHECK(ring.left == slow.left && ring.right == slow.right);
        }
    }
}

// The sample library already holds the short stereo kick. The first hit must fill a clean
// pad delay, and a last-tick hit must get past the pad Limiter before the track Echo repeats.
static void pad_print_wavs(const std::string& card, const std::string& sample, SampleLibrary& library) {
    PrintEngine printer(&library);
    auto run = [&](bool lookahead, unsigned bars, PrintTail tail) {
        PrintJob job;
        job.card_top = card; job.project_path = (fs::path(card) / "pad.mldp").string();
        job.patterns = {0}; job.tap = PrintTap::PostFx; job.tail = tail;
        job.project = std::make_unique<Project>(); job.bank = std::make_unique<PatternBank>();
        job.project->tempo = 1200;
        auto& track = job.project->tracks[0];
        load_sound(track, EngineKind::Sampler, 0);
        track.pads[0].sample = 0; track.pads[0].play = PadPlay::OneShot;
        auto& ref = job.project->samples.edit(0);
        std::snprintf(ref.path, sizeof(ref.path), "%s", sample.c_str()); ref.frames = 128;
        const EffectSlot echo {uint8_t(EffectKind::Echo), false, {60, 0, 100, 60, 9, 0}};
        auto& pad = track.pad_fx.at[0].slots[0];
        if (lookahead) {
            pad = {uint8_t(EffectKind::Limiter), false, {0, 30, 10, 0, 0, 0}};
            track.fx.slots[0] = echo;
        } else pad = echo;
        auto& pattern = job.bank->at[0][0]; pattern.length = bars * job.project->bar_ticks();
        Step step; step.notes[0] = {sampler_first_note, 110}; pattern.set_step(0, step);
        if (lookahead) { pattern.events[0].tick = job.project->bar_ticks() - 1; pattern.events[0].length = 1; }
        return print_wav(printer, std::move(job));
    };
    for (bool lookahead : {false, true}) {
        auto ring = run(lookahead, 1, PrintTail::Ring);
        auto longer = run(lookahead, 2, PrintTail::Cut);
        CHECK(ring.frames < longer.frames);
        CHECK(*std::max_element(ring.left.begin(), ring.left.end()) > 0.05f);
        CHECK(std::equal(ring.left.begin(), ring.left.end(), longer.left.begin()));
        CHECK(std::equal(ring.right.begin(), ring.right.end(), longer.right.begin()));
        CHECK(std::all_of(longer.left.begin() + ring.frames, longer.left.end(), [](float v) { return v == 0; }));
        CHECK(std::all_of(longer.right.begin() + ring.frames, longer.right.end(), [](float v) { return v == 0; }));
    }
}

// Compare the worker's WAV to the full engine, including when Motion enables every Limiter after playback starts.
static void automated_limiter_wav(const std::string& card, const std::string& sample, SampleLibrary& library) {
    PrintJob job;
    job.card_top = card; job.project_path = (fs::path(card) / "motion.mldp").string();
    job.patterns = {0}; job.tap = PrintTap::PostFx; job.tail = PrintTail::Cut;
    job.project = std::make_unique<Project>(); job.bank = std::make_unique<PatternBank>();
    auto& tr = job.project->tracks[0]; load_sound(tr, EngineKind::Sampler, 0);
    tr.pads[0].sample = 0; tr.pads[0].play = PadPlay::OneShot;
    auto& ref = job.project->samples.edit(0);
    std::snprintf(ref.path, sizeof(ref.path), "%s", sample.c_str()); ref.frames = 128;
    auto& pat = job.bank->at[0][0]; pat.length = job.project->bar_ticks();
    Step note; note.notes[0] = {sampler_first_note, 110}; pat.set_step(8, note);
    for (unsigned i = 0; i < track_slots; ++i) {
        tr.fx.slots[i] = {uint8_t(EffectKind::Limiter), false, {0, 30, 0, 0, 0, 0}};
        MotionPoint motion; motion.tick = 2 * sixteenth_ticks; motion.param = motion_fx_param(i, 2); motion.value = 10;
        pat.write_motion(motion);
    }
    print_project(*job.project, *job.bank, 0, 0);
    auto host = std::make_unique<EngineHost>();
    host->set_audio_format(dsp::sample_rate(), EngineHost::block);
    host->load(*job.project, *job.bank);
    auto inputs = library.hold_samples();
    CHECK(inputs[0] && host->offer_sample(0, inputs[0]));
    std::array<int32_t, 2 * EngineHost::block> idle {};
    host->render(idle.data(), EngineHost::block, 1.0f);
    const auto expected = render_print(*host, 0, pat.length, job.tap, job.tail, [] { return true; });
    library.release_samples();
    PrintEngine printer(&library);
    const auto wav = print_wav(printer, std::move(job));
    CHECK(expected.error.empty() && *std::max_element(wav.left.begin(), wav.left.end()) > 0.05f);
    CHECK(wav.left == expected.left && wav.right == expected.right);
}

// A budget shared by inactive launch points and the pager's active read windows.
static size_t locked_bytes, lock_limit;
static bool budget_pin(const void*, size_t bytes, bool lock) {
    if (lock) {
        if (locked_bytes + bytes > lock_limit) return false;
        locked_bytes += bytes;
    } else {
        CHECK(bytes <= locked_bytes);
        locked_bytes -= bytes;
    }
    return true;
}

static void freeze_ring_wavs(const std::string& card, const std::string& plugin, unsigned count, unsigned rate, unsigned budget_mib) {
    dsp::device_rate.store(rate);
    lock_limit = size_t(budget_mib) << 20;
    StreamPager pager(budget_pin);
    PrintEngine printer(nullptr); printer.set_pager(&pager);
    PrintJob job;
    job.card_top = card; job.project_path = (fs::path(card) / "freeze.mldp").string();
    job.freeze = true; job.tap = PrintTap::PostFx; job.tail = PrintTail::Ring;
    job.project = std::make_unique<Project>(); job.bank = std::make_unique<PatternBank>();
    job.project->tempo = 1200; job.project->signature = 0; // 1/4
    auto& track = job.project->tracks[0]; load_sound(track, EngineKind::Plugin, 0); track.plugin = 0;
    std::snprintf(job.project->plugins[0].path, sizeof(PluginRef::path), "%s", plugin.c_str());
    track.fx.slots[0] = {uint8_t(EffectKind::Echo), false, {60, 0, 100, 60, 13, 0}};
    track.fx.slots[1] = {uint8_t(EffectKind::Spring), false, {250, 25, 100, 70, 60, 120}};
    for (uint8_t p = 0; p < count; ++p) {
        auto& pattern = job.bank->at[0][p]; pattern.length = job.project->bar_ticks();
        Step step; step.notes[0] = {uint8_t(60 + p), 110}; pattern.set_step(0, step);
        job.patterns.push_back(p);
    }
    // All fixtures print stereo tails longer than a second and have no other bar lines.
    const size_t head_chunks = (2 * rate + stream_chunk - 1) / stream_chunk;
    if (count * head_chunks * stream_chunk_bytes > lock_limit) {
        // The launch points alone exceed the budget. Exercise the sheet's freeze path through to its notice.
        auto host = std::make_unique<EngineHost>();
        auto shell = std::make_unique<Shell>(*host);
        shell->set_printer(&printer);
        auto& context = shell->context();
        context.project = *job.project; context.patterns = *job.bank; context.project_path = job.project_path;
        Context::PrintAsk ask; ask.all = true; ask.deep = true; ask.tail = PrintTail::Ring;
        context.freeze(ask);
        CHECK(!context.frozen(0));
        context.print_tick();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (printer.busy() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(!printer.busy());
        context.print_tick();
        CHECK(std::string(shell->ui().notice).find("cannot hold") != std::string::npos);
        CHECK(!context.frozen(0) && !context.plugins_asleep() && !host->frozen_tracks());
        return;
    }
    run_job(printer, std::move(job));
    FreezeReady ready;
    CHECK(printer.take_ready(ready) && ready.error.empty() && ready.pack);
    std::unique_ptr<FreezePack> pack(ready.pack);
    CHECK(pack->owned.size() == count);
    PrintPlayer player; player.set_pager(&pager);
    // The first audible repeat arrives several launches later. The tail voices must each
    // retain their stream through the seam and release it even when playback is cut.
    // Leave the kit pager fully occupied. Ring overlaps have their own preallocated cursors.
    float sample = 0;
    ZonePart live;
    live.left = live.right = &sample; live.total = 1; live.pins.resize(1);
    std::array<int, StreamPager::stream_count> live_streams;
    for (int& stream : live_streams) {
        stream = pager.open(live, 0, false, 1, 0);
        CHECK(stream >= 0);
    }
    while (pager.pump()) {}
    const size_t launch_bytes = locked_bytes;
    float peak = 0;
    std::vector<PrintClip*> launched;
    for (unsigned n = 0; n < count + 14; ++n) {
        auto& clip = *pack->clips[n < count ? n : 0];
        launched.push_back(&clip);
        player.follow(&clip, 0, 0, clip.pass, float(clip.rate));
        for (unsigned at = 0; at < clip.pass; at += 128) {
            while (pager.pump()) {}
            float l[128] {}, r[128] {};
            unsigned frames = std::min(128u, clip.pass - at);
            player.render(l, r, frames);
            for (unsigned k = 0; k < frames; ++k) {
                float expected_l = 0, expected_r = 0;
                for (unsigned age = 0; age < launched.size(); ++age) {
                    const auto& source = *launched[n - age];
                    const unsigned frame = age * clip.pass + at + k;
                    if (frame >= source.frames) continue;
                    expected_l += source.data()[2 * frame];
                    expected_r += source.data()[2 * frame + 1];
                }
                CHECK(l[k] == expected_l && r[k] == expected_r);
                peak = std::max(peak, std::fabs(l[k]));
            }
        }
    }
    CHECK(peak > 0.1f && player.underruns == 0 && player.waits == 0);
    player.cut_all();
    while (pager.pump()) {}
    CHECK(locked_bytes == launch_bytes);
    for (int stream : live_streams) pager.close(stream);
    while (pager.pump()) {}
}

// Through the real Print sheet, print worker, audio command queue and render callback.
static void freeze_transitions(const std::string& card, const std::string& plugin, const std::string& mode) {
    constexpr unsigned rate = 48000, block = EngineHost::block;
    const bool memory = mode == "freeze-replace";
    const bool pattern_tempo = mode == "deep-freeze-empty-pattern-tempo";
    const bool empty_handoff = pattern_tempo || mode == "deep-freeze-empty-steady" || mode == "deep-freeze-empty-new-tempo";
    const bool change_tempo = mode == "deep-freeze-tempo" || mode == "deep-freeze-tempo-replace" || mode == "deep-freeze-empty-new-tempo";
    const bool other_lead = mode.rfind("deep-freeze-lead-", 0) == 0;
    const bool switched_off = mode == "deep-freeze-lead-off-stale" || mode == "deep-freeze-lead-off-compatible";
    const bool stale_launch = mode == "deep-freeze-lead-playing-stale" || mode == "deep-freeze-lead-off-stale";
    const bool reject = change_tempo || stale_launch || mode == "deep-freeze-lead-playing-clock-mismatch";
    const bool active_lead = mode == "deep-freeze-active-lead";
    const bool reload_fails = mode == "deep-freeze-active-reload-fails";
    const bool clock_reverts = mode == "deep-freeze-active-clock-reverts";
    const bool active_clock = active_lead || reload_fails || clock_reverts || mode == "deep-freeze-active-tempo";
    const bool unload_grace = mode == "deep-freeze-unload-grace";
    const bool short_bar = change_tempo || empty_handoff || other_lead || active_clock || unload_grace;
    const bool deep = short_bar || mode == "deep-freeze-live" || mode == "deep-freeze-replace";
    const bool prior_freeze = mode == "deep-freeze-replace" || mode == "deep-freeze-tempo-replace";
    if (memory) {
        struct rlimit limit {};
        CHECK(getrlimit(RLIMIT_MEMLOCK, &limit) == 0 && limit.rlim_max >= 8 * 1024 * 1024);
        limit.rlim_cur = 8 * 1024 * 1024;
        CHECK(setrlimit(RLIMIT_MEMLOCK, &limit) == 0);
    }

    dsp::device_rate.store(rate);
    auto host = std::make_unique<EngineHost>();
    host->set_render_threads(0);
    auto rack = std::make_unique<PluginRack>(*host);
    rack->set_card(card);
    auto printer = std::make_unique<PrintEngine>(nullptr);
    printer->set_pager(&host->streams());
    auto shell = std::make_unique<Shell>(*host);
    shell->set_printer(printer.get());
    if (deep) shell->set_plugins(rack.get());
    auto& c = shell->context();
    c.project_path = (fs::path(card) / (mode + ".mldp")).string();
    c.now = 1000;
    c.project.tempo = deep && !short_bar ? 300 : 1200;
    c.project.signature = short_bar ? common_time : deep ? 8 : 0; // 9/4 at 30 BPM is an 18-second bar
    if (pattern_tempo) {
        c.project.seq_tempo = true;
        c.project.pattern_tempo[0] = 1260;
        c.project.pattern_tempo[1] = 1320;
    }
    if (other_lead) {
        c.project.seq_tempo = true;
        c.project.pattern_tempo[0] = stale_launch ? 1200 : 1260;
        c.project.pattern_tempo[1] = 1200;
        load_sound(c.project.tracks[1], EngineKind::Sampler, 0); // quiet track that can take the tempo lead
    }
    if (active_lead) {
        c.project.seq_tempo = true;
        c.project.pattern_tempo[0] = 1200;
        c.project.pattern_tempo[1] = 1140;
        load_sound(c.project.tracks[1], EngineKind::Sampler, 0);
    }
    auto& track = c.project.tracks[0];
    load_sound(track, EngineKind::Plugin, 0);
    track.plugin = 0;
    std::snprintf(c.project.plugins[0].path, sizeof(PluginRef::path), "%s", plugin.c_str());
    if (memory) {
        track.fx.slots[0] = {uint8_t(EffectKind::Echo), false, {60, 0, 100, 60, 13, 0}};
        track.fx.slots[1] = {uint8_t(EffectKind::Spring), false, {250, 25, 100, 70, 60, 120}};
    }
    const unsigned count = memory ? 24 : prior_freeze || pattern_tempo ? 2 : 1;
    for (unsigned p = 0; p < count; ++p) {
        auto& pattern = c.patterns.at[0][p];
        pattern.length = c.project.bar_ticks() * (clock_reverts ? 4 : 1);
        const unsigned steps = deep && !unload_grace ? pattern.length / sixteenth_ticks : 1;
        for (unsigned i = 0; i < steps; ++i) {
            Step note;
            note.notes[0] = {uint8_t(60 + p), 110};
            note.length = deep ? 2 : 1;
            if (unload_grace) note.length = 12; // a single note should sustain through the tempo change
            pattern.set_step(i, note);
        }
    }
    host->load(c.project, c.patterns);
    host->set_audio_format(rate, block);
    host->streams().start();
    std::array<int32_t, block * 2> audio {};
    auto render = [&] { host->render(audio.data(), block, 0.7f); };
    auto prepare = [&](uint64_t picks, bool unload = false) {
        c.open_print_sheet();
        auto& sheet = shell->ui().print_sheet;
        sheet.picks[0] = picks;
        sheet.tail = memory || active_lead ? PrintTail::Ring : PrintTail::Loop;
        c.print_sheet_go(unload);
        CHECK(sheet.printing);
        c.print_tick();
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (printer->busy() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        CHECK(!printer->busy());
    };
    auto deliver = [&] { c.print_tick(); render(); };
    auto selected = [&](uint64_t mask) {
        c.print_tick(); // read the engine's confirmation
        CHECK(c.frozen(0) && (host->frozen_tracks() & 1));
        CHECK(c.prints.freeze[0].patterns[0] == mask);
    };

    if (mode == "freeze-recording") {
        c.play();
        for (unsigned i = 0; i < 32; ++i) render();
        CHECK(host->playing() && host->position_of(0) >= 0);
        auto& pattern = c.patterns.at[0][0];
        c.set_recording(true);
        c.key(78, 110, true);
        for (unsigned i = 0; i < 80; ++i) render();
        const Pattern original = pattern;
        prepare(1);
        c.key(78, 0, false); // a note held before the snapshot cannot change length after it
        auto overdub = [&](int key) {
            c.key(key, 110, true);
            c.key(key, 0, false);
            c.record_motion(motion_wheel, 35);
        };
        overdub(79);
        CHECK(pattern == original); // the worker has already copied this pattern
        deliver();
        CHECK(!(host->frozen_tracks() & 1)); // sent, still waiting for its bar
        overdub(80);
        CHECK(pattern == original);
        c.unfreeze(0);
        render();
        c.print_tick();
        overdub(81);
        CHECK(pattern.events.size() == original.events.size() + 1 && !pattern.motion.empty());
        c.set_recording(false);
        for (unsigned i = 0; i < rate / block; ++i) render();
        CHECK(!c.frozen(0) && !(host->frozen_tracks() & 1));
    } else if (memory) {
        const uint64_t initial = (uint64_t{1} << 16) - 1, smaller = (uint64_t{1} << 15) - 1;
        prepare(initial);
        deliver();
        selected(initial);
        prepare(smaller);
        deliver();
        selected(smaller); // remapping the retained heads would exceed 8 MiB
        prepare((uint64_t{1} << count) - 1);
        deliver();
        selected(smaller); // 24 independent heads cannot fit even with reuse
        CHECK(std::string(shell->ui().notice).find("kept: cannot hold") != std::string::npos);
        // Finish disposing of retired packs before driving the pager on this thread. The
        // accelerated render can then test the lock limit independently of host scheduling.
        shell->set_printer(nullptr);
        printer.reset();
        host->streams().stop();
        CHECK(host->send({Command::Type::Play}));
        bool sounded = false;
        // This fully wet Echo/Spring fixture first sounds several pattern passes later.
        for (unsigned i = 0; i < 8 * rate / block; ++i) {
            while (host->streams().pump()) {}
            render();
            sounded |= std::any_of(audio.begin(), audio.end(), [](int32_t v) { return v != 0; });
        }
        CHECK(host->printing_tracks() & 1);
        CHECK(sounded);
        CHECK(host->print_waits() == 0 && host->print_underruns() == 0);
    } else if (deep) {
        // An ordinary pack that covers only pattern 2 leaves pattern 1 playing live. Its
        // frozen bit must never count as acknowledgment of the requested deep freeze.
        for (unsigned i = 0; i < 1500 && (i < 30 || rack->status(0) != PluginStatus::Ready); ++i) {
            c.plugins_tick(++c.now);
            render();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(rack->status(0) == PluginStatus::Ready);
        if (prior_freeze) {
            prepare(2);
            deliver();
            selected(2);
        }
        constexpr unsigned empty = 2;
        if (unload_grace) {
            CHECK(c.patterns.at[0][empty].empty());
            c.launch(0, empty);
            render();
            CHECK(host->pattern_of(0) == empty);
        }
        CHECK(host->send({Command::Type::Play}));
        for (unsigned i = 0; i < 96; ++i) render(); // ask in the body of the bar
        const auto previous = host->active_freeze_serial(0);
        prepare(1, true);
        deliver();
        CHECK(!c.deep(0) && !(host->printing_tracks() & 1));
        if (unload_grace) {
            c.launch(0, 0); // the print wins the first note of the filled pattern at the handoff bar
            for (unsigned n = 0; n < 4 * rate / block && !(host->printing_tracks() & 1); ++n) render();
            CHECK(host->pattern_of(0) == 0 && (host->printing_tracks() & 1));
            c.print_tick();
            CHECK(c.deep(0) && (c.deep_hold & 1));
            CHECK(rack->status(0) == PluginStatus::Ready && !rack->asleep(0));

            for (unsigned n = 0; n < 24; ++n) render(); // move past the first note-on while the plug-in is still held
            const auto audible = [&] { return std::any_of(audio.begin(), audio.end(), [](int32_t v) { return v != 0; }); };
            CHECK(host->position_of(0) > 2 && audible());
            c.set_tempo(1260);

            const uint32_t since = c.now;
            bool went_live = false;
            for (unsigned n = 0; n < 4 * rate / block; ++n) {
                c.now = since + uint32_t(uint64_t(n) * block * 1000 / rate);
                c.plugins_tick(c.now);
                c.print_tick();
                render();
                if (n < 160) CHECK(audible()); // the printed note has not reached its end
                if (host->frozen_tracks() & 1) continue;
                CHECK(n >= 160 && rack->status(0) == PluginStatus::Ready);
                CHECK(host->position_of(0) >= 0 && host->position_of(0) <= 2);
                went_live = true;
                c.print_tick();
                break;
            }
            CHECK(host->tempo_now() == 1260 && went_live && !c.frozen(0));
            CHECK(host->print_waits() == 0 && host->print_underruns() == 0);
            return;
        }
        if (change_tempo || stale_launch) c.set_tempo(1260); // after the audio engine has queued the 120 BPM pack
        if (other_lead) {
            if (switched_off) c.track_off(0);
            c.launch(1, 1); // another track leads the handoff bar at 120 BPM
        }
        if (empty_handoff) {
            CHECK(c.patterns.at[0][empty].empty());
            c.launch(0, empty);
        }

        const uint32_t begin = c.now;
        bool audible = false, switched = false, saw_empty = false, saw_other_lead = false;
        unsigned windows = 0;
        const unsigned seconds = short_bar ? 12 : 27;
        for (unsigned i = 0; i < seconds * rate / block; ++i) {
            const unsigned frames = i * block;
            c.now = begin + uint32_t(uint64_t(frames) * 1000 / rate);
            if ((empty_handoff || other_lead) && frames == 4 * rate) c.launch(0, 0);
            if (i % 8 == 0) {
                c.plugins_tick(c.now);
                c.print_tick();
                std::this_thread::sleep_for(std::chrono::microseconds(350));
            }
            render();
            switched |= (host->printing_tracks() & 1) != 0;
            saw_empty |= host->pattern_of(0) == empty;
            if (other_lead && frames < 4 * rate && host->pattern_of(1) == 1 && host->tempo_now() == 1200)
                saw_other_lead |= bool(host->stopped_tracks() & 1) == switched_off;
            if (!empty_handoff && !switched_off && !switched) CHECK(!(c.plugins_asleep() & 1) && !rack->asleep(0));
            audible |= std::any_of(audio.begin(), audio.end(), [](int32_t v) { return v != 0; });
            const unsigned elapsed = (frames + block) / rate;
            if (elapsed != windows) {
                // For an empty or switched-off handoff, check complete seconds after the filled pattern returns.
                if ((!empty_handoff && !switched_off) || elapsed > 6) CHECK(audible);
                audible = false;
                windows = elapsed;
            }
        }
        if (empty_handoff) CHECK(saw_empty && host->pattern_of(0) == 0 && !c.freeze_pending(0));
        if (other_lead) {
            CHECK(saw_other_lead && host->pattern_of(0) == 0 && host->tempo_now() == 1260);
            CHECK(!(host->stopped_tracks() & 1) && !c.freeze_pending(0));
        }
        if (reject) {
            CHECK(!switched && !c.deep(0) && !rack->asleep(0));
            CHECK(host->active_freeze_serial(0) == previous);
            CHECK(c.frozen(0) == prior_freeze && bool(host->frozen_tracks() & 1) == prior_freeze);
            if (prior_freeze) selected(2);
            CHECK(std::string(shell->ui().notice).find("clock changed") != std::string::npos);
        } else {
            CHECK(switched && c.deep(0) && (c.plugins_asleep() & 1) && rack->asleep(0));
        }
        if (clock_reverts) {
            CHECK(rack->status(0) == PluginStatus::None && (host->printing_tracks() & 1));
            // Drive the pager from the test thread and leave the rack asleep until after the tempo reversal.
            host->streams().stop();
            const uint32_t since = c.now;
            c.set_tempo(600);
            for (unsigned n = 0; n < 7 * rate / block; ++n) {
                c.now = since + uint32_t(uint64_t(n) * block * 1000 / rate);
                while (host->streams().pump()) {}
                c.print_tick();
                render();
            }
            CHECK(host->tempo_now() == 600 && c.deep(0) && (c.thawing & 1) && rack->asleep(0));
            CHECK(host->print_waits() == 0 && host->print_underruns() == 0);
            c.set_tempo(1200);
            for (unsigned n = 0; n < 32; ++n) {
                if (n >= 6) while (host->streams().pump()) {} // no new pins for the first 16 ms
                c.print_tick();
                render();
                CHECK(std::any_of(audio.begin(), audio.end(), [](int32_t v) { return v != 0; }));
            }
            CHECK(host->tempo_now() == 1200 && c.deep(0) && rack->asleep(0));
            CHECK(host->printing_tracks() & 1);
            CHECK(host->print_waits() == 0 && host->print_underruns() == 0);
            return;
        }
        if (active_clock) {
            CHECK(rack->status(0) == PluginStatus::None && (host->printing_tracks() & 1));
            if (reload_fails) fs::rename(plugin, plugin + ".unavailable");
            const unsigned target = active_lead ? 1140 : 1260;
            if (active_lead) c.launch(1, 1);
            else c.set_tempo(int(target));

            const uint32_t since = c.now;
            bool saw_thaw = false, saw_live = false;
            for (unsigned sec = 0; sec < 12; ++sec) {
                bool sounded = false;
                for (unsigned i = 0; i < rate / block; ++i) {
                    c.now = since + 1000 * sec + uint32_t(uint64_t(i) * block * 1000 / rate);
                    if (i % 8 == 0) {
                        // Leave the rack asleep for three seconds to exercise the print on its own at the new clock.
                        if (sec >= 3) c.plugins_tick(c.now);
                        c.print_tick();
                        saw_thaw |= (c.thawing & 1) != 0;
                        std::this_thread::sleep_for(std::chrono::microseconds(350));
                    }
                    render();
                    sounded |= std::any_of(audio.begin(), audio.end(), [](int32_t v) { return v != 0; });
                    if (!saw_live && !(host->frozen_tracks() & 1)) {
                        CHECK(!reload_fails && rack->status(0) == PluginStatus::Ready);
                        CHECK(host->position_of(0) >= 0 && host->position_of(0) <= 2); // the live pattern starts on its own boundary
                        saw_live = true;
                    }
                }
                CHECK(sounded);
                if (sec == 2) {
                    CHECK(host->tempo_now() == target && saw_thaw && c.deep(0) && rack->asleep(0));
                    CHECK(host->printing_tracks() & 1);
                }
            }
            CHECK(saw_thaw && host->tempo_now() == target);
            CHECK(!(c.thawing & 1));
            if (reload_fails) {
                CHECK(!saw_live && c.deep(0) && (host->printing_tracks() & 1));
                CHECK(std::string(shell->ui().notice) == "Track 1 kept: plug-in offline");
            } else {
                CHECK(saw_live && !c.frozen(0) && !rack->asleep(0));
                CHECK(!(host->printing_tracks() & 1));
            }
        }
        CHECK(host->print_waits() == 0 && host->print_underruns() == 0);
    } else {
        auto fill_queue = [&] {
            Command swing {Command::Type::Swing};
            swing.value = 50;
            unsigned queued = 0;
            while (host->send(swing)) CHECK(++queued <= 4096);
            CHECK(queued > 4000);
        };
        prepare(1);
        fill_queue();
        c.print_tick();
        CHECK(shell->ui().print_sheet.printing && !c.frozen(0) && !(host->frozen_tracks() & 1));
        render();
        c.print_sheet_cancel(); // cancellation must invalidate a ready pack that could not be sent
        deliver();
        CHECK(!shell->ui().print_sheet.printing && !c.frozen(0) && !(host->frozen_tracks() & 1));

        prepare(1);
        fill_queue();
        deliver(); // delivery was refused, then the render drained the queue
        CHECK(shell->ui().print_sheet.printing && !c.frozen(0) && !(host->frozen_tracks() & 1));
        deliver();
        CHECK(!shell->ui().print_sheet.printing);
        selected(1);

        fill_queue();
        c.open_print_sheet();
        c.print_sheet_unfreeze();
        selected(1);
        CHECK(std::string(shell->ui().notice).find("waiting to unfreeze") != std::string::npos);
        render();
        deliver();
        CHECK(!c.frozen(0) && !(host->frozen_tracks() & 1));
        CHECK(std::string(shell->ui().notice) == "Track 1 live");
    }
}

// A separate invocation starts a fresh project and a new plug-in instance.
// The simulated SD card is the only thing shared by the two invocations.
int main(int argc, char** argv) {
    CHECK(argc >= 4);
    const std::string card = argv[1], plugin = argv[2], mode = argv[3];
    if (mode == "ring-freeze") {
        CHECK(argc == 7);
        freeze_ring_wavs(card, plugin, std::stoul(argv[4]), std::stoul(argv[5]), std::stoul(argv[6]));
        return 0;
    }
    CHECK(argc == 4);
    if (mode == "freeze-replace" || mode == "freeze-queue" || mode == "freeze-recording" || mode.rfind("deep-freeze-", 0) == 0) {
        freeze_transitions(card, plugin, mode);
        return 0;
    }
    if (mode == "native-ring") { native_ring_wav(card, plugin); return 0; }
    bool saving = mode == "save" || mode == "fxsave";
    bool fx = mode == "fxsave" || mode == "fxload"; // the fixture's effect in a chain, not a track's synth
    // Rack-level modes beside the preset ones: a failed save keeping the rack's state, a sleeping LV2 woken by
    // a value set, and a mono plug-in heard through the rack (fixture env hooks in tests/clap_fixture.cpp).
    bool failsave = mode == "failsave", lv2wake = mode == "lv2wake", mono = mode == "mono", dead_state = mode == "dead-state";
    const bool dynamicRing = mode == "dynamic-ring";
    if (dynamicRing) setenv("MELODEE_TEST_DYNAMIC_TAIL", "1", 1);
    if (mode == "ringtail") setenv("MELODEE_TEST_RING_TAIL", "1", 1);
    if (mono) setenv("MELODEE_TEST_MONO", "1", 1); // before the rack opens it: a mono main port
    const bool presetKeys = mode == "preset-keys", faceKeys = mode == "face-keys", brokenKeys = mode == "broken-keys";
    if (presetKeys) { // three presets of the fixture's own, in a folder beside it on the card
        const auto folder = fs::path(plugin).parent_path() / "TestPresets";
        fs::create_directories(folder);
        std::ofstream(folder / "Bank.tsp") << "Low\t0.25\nMid\t0.5\nHigh\t0.75\n";
        setenv("MELODEE_TEST_SLOW_OPEN", "1", 1);    // the fixture takes 60 ms to open, and 60 ms more to say where
        setenv("MELODEE_TEST_SLOW_PRESETS", "1", 1); // its presets are: both states last long enough to press keys in
    }
    // The fixture's three presets, as preset-keys has them, without its slow opening.
    if (mode == "stage-presets" || mode == "presets-track" || mode == "undo-state" || mode == "copy-presets" || mode == "copy-presets-late" || mode == "panel-lag"
        || mode == "turn-effect" || mode == "presets-frozen" || mode == "load-wake" || mode == "companion-frozen" || mode == "shared-frozen") {
        const auto folder = fs::path(plugin).parent_path() / "TestPresets";
        fs::create_directories(folder);
        std::ofstream(folder / "Bank.tsp") << "Low\t0.25\nMid\t0.5\nHigh\t0.75\nLoud\t1.5\tgain\n"; // and one of Test Gain's
    }
    const bool steady = mode == "transport-steady", deaf = mode == "transport-off";
    if (mode == "transport-wake" || mode == "load-wake" || steady || deaf) setenv("MELODEE_TEST_TRANSPORT", "1", 1); // the fixture keeps the transport it was told
    if (mode == "undo-steps") { // one instrument a file, and another beside the fixture
        setenv("MELODEE_TEST_ONE_PLUGIN", "1", 1);
        fs::copy_file(plugin, fs::path(plugin).parent_path() / "C.clap");
    }
    if (mode == "undo-entries") { // the same
        setenv("MELODEE_TEST_ONE_PLUGIN", "1", 1);
        fs::copy_file(plugin, fs::path(plugin).parent_path() / "B.clap");
    }
    if (mode == "picker-rows") { // each file one instrument, so a row loads; two more beside it, and one in each of two folders
        setenv("MELODEE_TEST_ONE_PLUGIN", "1", 1);
        const auto folder = fs::path(plugin).parent_path();
        for (const char* name : {"B.clap", "C.clap", "Alpha/One.clap", "Beta/Two.clap"}) {
            fs::create_directories((folder / name).parent_path());
            fs::copy_file(plugin, folder / name);
        }
    }
    const unsigned entry = fx ? 2 : mode == "load" ? 4 : 0;
    auto host = std::make_unique<EngineHost>();
    host->set_render_threads(0);
    auto rack = std::make_unique<PluginRack>(*host, dynamicRing ? "print-" : "", dynamicRing);
    auto shell = std::make_unique<Shell>(*host);
    auto& c = shell->context();
    auto& ui = shell->ui();
    auto& project = shell->project();
    auto& tr = project.tracks[0];
    auto& ref = project.plugins[entry];
    rack->set_card(card);
    shell->set_plugins(rack.get());
    std::snprintf(ref.path, sizeof(ref.path), "%s", (brokenKeys ? plugin + ".gone" : plugin).c_str()); // not on the card
    if (fx) {
        load_sound(tr, EngineKind::Sampler, 0);
        ref.id = clap_id_hash("org.melodee.test.gain");
        auto& slot = project.chain(master_chain).slots[0]; // the master's chain: mixed (so run) even in silence
        slot.kind = plugin_effect_kind;
        slot.params[0] = int16_t(entry);
    } else {
        load_sound(tr, EngineKind::Plugin, 0);
        tr.plugin = uint8_t(entry);
    }
    // Two already-open instances must both see a new patch.
    if (saving && !fx) {
        project.plugins[1] = ref;
        load_sound(project.tracks[1], EngineKind::Plugin, 0);
        project.tracks[1].plugin = 1;
    }
    auto kit_on = [&](unsigned t) { // a kit with a sample on its first pad
        load_sound(project.tracks[t], EngineKind::Sampler, 0);
        project.tracks[t].set_name("DRUMS");
        project.tracks[t].pads[0].sample = 3;
    };
    if (mode == "picker-track" || mode == "undo-state") kit_on(1);
    if (mode == "picker-track") { // and a second kit, named
        kit_on(2);
        project.tracks[2].set_name("BASS");
    }
    if (mode == "undo-state") { // and the fixture's effect on the master's chain
        project.plugins[2] = ref;
        project.plugins[2].id = clap_id_hash("org.melodee.test.gain");
        auto& slot = project.chain(master_chain).slots[0];
        slot.kind = plugin_effect_kind;
        slot.params[0] = 2;
    }
    if (steady) { project.tempo = 1270; project.swing = 60; } // a tick no whole number of frames, and swung
    if (mode == "transport-wake" || steady || deaf) { // and Test Gain on the master's chain (mixed, so run, even in silence)
        project.plugins[2] = ref;
        project.plugins[2].id = clap_id_hash("org.melodee.test.gain");
        auto& slot = project.chain(master_chain).slots[0];
        slot.kind = plugin_effect_kind;
        slot.params[0] = 2;
        if (deaf) project.plugins[2].transport = false; // TRANSPORT off: it hears the song stopped throughout
    }
    if (mode == "turn-effect") { // Test Gain on track 1's chain too
        project.plugins[3] = ref;
        project.plugins[3].id = clap_id_hash("org.melodee.test.gain");
        auto& slot = project.chain(0).slots[0];
        slot.kind = plugin_effect_kind;
        slot.params[0] = 3;
    }
    if (mode == "freeze-race") { // a second track of the synth, its own instance
        project.plugins[1] = ref;
        load_sound(project.tracks[1], EngineKind::Plugin, 0);
        project.tracks[1].plugin = 1;
    }
    if (mode == "copy-full" || mode == "shared-frozen") { // a second plug-in track, and every other entry in use: effects that will not open
        project.plugins[1] = ref;
        load_sound(project.tracks[1], EngineKind::Plugin, 0);
        project.tracks[1].plugin = 1;
        for (unsigned e = 2; e < max_plugins; ++e) {
            std::snprintf(project.plugins[e].path, sizeof(project.plugins[e].path), "/nowhere/X%u.clap", e);
            auto& slot = project.chain(2 + (e - 2) / track_slots).slots[(e - 2) % track_slots];
            slot.kind = plugin_effect_kind;
            slot.params[0] = int16_t(e);
        }
    }
    if (mode == "undo-entries") // every other plug-in entry set once (another project's), none in use
        for (unsigned k = 1; k < max_plugins; ++k) std::snprintf(project.plugins[k].path, sizeof(project.plugins[k].path), "/nowhere/X%u.clap", k);
    const bool late = mode == "copy-presets-late"; // copy-presets with every other entry in use: effects that will not open
    if (mode == "presets-track" || mode == "copy-presets" || late || mode == "panel-lag") { // two tracks of the same synth, each its own instance, and a kit
        project.plugins[1] = ref;
        load_sound(project.tracks[1], EngineKind::Plugin, 0);
        project.tracks[1].plugin = 1;
        kit_on(2);
        for (unsigned at : {3u, 4u}) { // Test Gain on the second track's chain and on the master's
            project.plugins[at] = ref;
            project.plugins[at].id = clap_id_hash("org.melodee.test.gain");
            auto& slot = project.chain(at == 3 ? 1u : unsigned(master_chain)).slots[0];
            slot.kind = plugin_effect_kind;
            slot.params[0] = int16_t(at);
        }
        if (late)
            for (unsigned e = 2, n = 0; e < max_plugins; ++e) {
                if (e == 3 || e == 4) continue;
                std::snprintf(project.plugins[e].path, sizeof(project.plugins[e].path), "/nowhere/X%u.clap", e);
                auto& slot = project.chain(4 + n / track_slots).slots[n % track_slots]; // tracks 5 to 9's chains
                slot.kind = plugin_effect_kind;
                slot.params[0] = int16_t(e);
                ++n;
            }
    }
    if (mono || dynamicRing) {
        Step step;
        step.notes[0] = {69, 100};
        step.length = 16;
        auto& pattern = shell->patterns().at[0][0];
        if (dynamicRing) pattern.length = sixteenth_ticks;
        pattern.set_step(0, step);
    }
    host->load(project, shell->patterns());
    host->set_audio_format(steady ? 44100 : 48000, 384); // transport-steady: the MPC's own rate
    Command reload {Command::Type::Reload};
    reload.pack = new ProjectPack(project, shell->patterns());
    host->send(reload);
    ui.track = 0;
    c.open(PageId::Edit);
    uint32_t now = 1000;
    std::array<int32_t, EngineHost::block * 2> audio {};
    auto tick = [&] {
        c.plugins_tick(now += 5);
        host->render(audio.data(), EngineHost::block, 0.7f);
        const ProjectPack* done = nullptr;
        while (host->take_done_pack(done)) delete done;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    };
    auto wait = [&](auto predicate) {
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!predicate() && std::chrono::steady_clock::now() < until) tick();
        CHECK(predicate());
    };
    auto button = [&](SurfaceButton b, bool down) {
        SurfaceEvent event;
        event.input.kind = SurfaceInput::Kind::Button;
        event.input.index = uint8_t(b);
        event.input.pressed = down;
        shell->surface(event, ++now);
    };
    auto tap = [&](SurfaceButton b) { button(b, true); button(b, false); };
    auto chord = [&] {
        button(SurfaceButton::Shift, true);
        tap(SurfaceButton::SampleSelect);
        button(SurfaceButton::Shift, false);
    };
    auto pad = [&](unsigned n) { // a pad hit and let go, as the surface sends it
        for (uint8_t velocity : {uint8_t(100), uint8_t(0)}) {
            SurfaceEvent event;
            event.pad = int(n);
            event.midi.status = velocity ? 0x90 : 0x80;
            event.midi.value = velocity;
            shell->surface(event, ++now);
        }
    };
    auto pick_track = [&](unsigned n) { // Pad Bank + pad
        button(SurfaceButton::PadBank, true);
        pad(n);
        button(SurfaceButton::PadBank, false);
    };
    auto shifted = [&](SurfaceButton b) {
        button(SurfaceButton::Shift, true);
        tap(b);
        button(SurfaceButton::Shift, false);
    };
    auto undo = [&] { shifted(SurfaceButton::Minus); };
    auto redo = [&] { shifted(SurfaceButton::Plus); };
    auto turn = [&](int detents) { // the wheel
        SurfaceEvent event;
        event.input.kind = SurfaceInput::Kind::Control;
        event.input.index = uint8_t(SurfaceControl::Wheel);
        event.input.delta = detents;
        shell->surface(event, ++now);
    };
    auto values = [&](unsigned at) { // a fixture's Level, Wave and Gain, from its state
        auto bytes = rack->state(at);
        CHECK(bytes.size() == 4 + 3 * sizeof(double));
        std::array<double, 3> v;
        std::memcpy(v.data(), bytes.data() + 4, sizeof(v));
        return v;
    };
    auto ready = [&](unsigned at) { return rack->status(at) == PluginStatus::Ready && !rack->programs(at).finding; };
    // The picker's row for a file or folder of that name on the card (-1 none), and the cursor put on it with the wheel.
    auto row_of = [&](const std::string& name) {
        const auto view = rack->view();
        for (size_t n = 0; n < view.files.size(); ++n)
            if (fs::path(view.files[n].path).filename() == name && view.files[n].id == 0) return int(n);
        return -1;
    };
    // The picker shows that folder, whole: the rack, the shell's count and the page drawn; `kinds`: each plug-in in it
    // opened once, its kind known.
    auto drawn = std::make_unique<Snapshot>();
    auto listing = [&](const std::string& folder, size_t rows, bool kinds = true) {
        wait([&] {
            const auto view = rack->view();
            if (fs::path(view.folder).filename() != folder || view.files.size() != rows || ui.plugins_count != rows) return false;
            for (const auto& f : view.files) if (kinds && !f.folder && f.instrument < 0) return false;
            shell->snapshot(*drawn, now);
            return fs::path(drawn->plugins.folder).filename() == folder && drawn->plugins.files.size() == rows;
        });
    };
    auto cursor_on = [&](const std::string& name) {
        CHECK(row_of(name) >= 0);
        turn(row_of(name) - int(ui.plugins_cursor));
        CHECK(int(ui.plugins_cursor) == row_of(name));
    };
    auto open_picker = [&] { // Stage's PRESETS on the track in hand, then Shift + B1 PLUG-INS
        CHECK(stage_go(ui, EngineKind::Plugin, StageKind::PluginSound));
        c.open(PageId::Stage);
        shifted(SurfaceButton::TopLeft);
        CHECK(ui.page == PageId::Plugins && ui.plugin_target == UiState::PluginTarget::Instrument);
    };
    auto has_patch = [&](unsigned slot) {
        for (const auto& p : rack->programs(slot).list) if (p.user() && p.name == "My Patch") return true;
        return false;
    };
    auto state = [&] {
        auto bytes = rack->state(entry);
        CHECK(bytes.size() == 4 + 3 * sizeof(double));
        std::array<double, 3> values;
        std::memcpy(values.data(), bytes.data() + 4, sizeof(values));
        return values;
    };
    // A plug-in that would not open, on Stage's first page: Minus / Plus have no preset to walk to and leave the pads'
    // keys alone too (a walk that ends in a failed open must not slip to the octave); Keys' octave still answers.
    if (brokenKeys) {
        wait([&] { return rack->status(entry) == PluginStatus::Failed; });
        CHECK(stage_go(ui, EngineKind::Plugin, StageKind::PluginSound));
        c.open(PageId::Stage);
        const int8_t octave = tr.octave;
        const uint8_t bank = tr.bank;
        for (auto key : {SurfaceButton::Plus, SurfaceButton::Minus, SurfaceButton::Minus}) {
            tap(key);
            CHECK(tr.octave == octave && tr.bank == bank && !ui.preset_due && ui.ask == UiState::Ask::None);
        }
        button(SurfaceButton::Sample, true);
        tap(SurfaceButton::Plus);
        button(SurfaceButton::Sample, false);
        CHECK(tr.octave == octave + 1 && !ui.preset_due);
        shell->set_plugins(nullptr);
        return 0;
    }
    wait([&] { return rack->status(entry) == PluginStatus::Ready && !rack->programs(entry).finding; });
    if (mode.rfind("knob-", 0) == 0) {
        ui.settings.knob_takeover = mode == "knob-pickup" ? Takeover::Pickup
                                  : mode == "knob-instant" ? Takeover::Instant : Takeover::Scaled;
        stage_go(ui, EngineKind::Plugin, StageKind::PluginScreen);
        c.open(PageId::Stage);
        auto knob = [&](SurfaceControl k, int position, bool sync = false) {
            SurfaceEvent event;
            event.input.kind = SurfaceInput::Kind::Control;
            event.input.index = uint8_t(k);
            event.input.value = uint8_t(position);
            event.syncing = sync;
            shell->surface(event, ++now);
        };
        wait([&] { return ui.plugin_panel.screen && std::string(ui.plugin_panel.params[1].name) == "Wave"; });
        auto k2 = [&](int position, bool sync = false) { knob(SurfaceControl::K2, position, sync); };
        if (mode == "knob-scaled") {
            knob(SurfaceControl::K1, 10, true);
            knob(SurfaceControl::K1, 11);
            wait([&] { return int(std::lround(state()[0] * 1000)) == 504; });
        }
        const auto level_pot = ui.pots[0];
        k2(0, true);
        k2(127);
        wait([&] { return state()[1] == 2; });
        const auto old = ui.plugin_panel;
        tap(SurfaceButton::Minus);
        CHECK(!rack->set_screen_param(old, 1, 0, false)); // navigation queued, the shell still shows the old card
        wait([&] { return ui.plugin_panel.screen && std::string(ui.plugin_panel.params[1].name) == "Level"; });
        CHECK(!rack->set_screen_param(old, 1, 0, false)); // the screen remapped K2 between the panel read and the write
        auto snapshot = std::make_unique<Snapshot>();
        shell->snapshot(*snapshot, now); // showing the reassignment must preserve K1's fractional Scaled travel
        CHECK(ui.pots[0].scaled == level_pot.scaled);
        CHECK(ui.plugin_panel.params[1].value == (mode == "knob-scaled" ? 504 : 500));
        k2(126);
        const int expected = mode == "knob-scaled" ? 500 : mode == "knob-pickup" ? 500 : 992;
        CHECK(ui.plugin_panel.params[1].value == expected);
        wait([&] { return int(std::lround(state()[0] * 1000)) == expected; });
        CHECK(state()[1] == 2);
        if (mode == "knob-scaled") {
            k2(0); // catch Level at its minimum, then move it using the screen's editor
            wait([&] { return state()[0] == 0; });
            const auto before_wheel = ui.plugin_panel;
            CHECK(c.plugin_screen_input(ScreenKey::Wheel, 60));
            wait([&] { return ui.plugin_panel.params[1].value == 600; });
            CHECK(!rack->set_screen_param(before_wheel, 1, 0, true)); // same assignment but the starting value went stale
            k2(1);
            CHECK(ui.plugin_panel.params[1].value == 603);
            wait([&] { return int(std::lround(state()[0] * 1000)) == 603; });
            CHECK(rack->set_screen_param(before_wheel, 1, 1000, false)); // an absolute edit only needs the same target
            wait([&] { return state()[0] == 1; });
        }
        shell->set_plugins(nullptr);
        return 0;
    }
    // Rack slots follow each fixture's remote-controls page, which can differ from its parameter order.
    auto set_control_of = [&](unsigned at, const char* name, int value) {
        PluginPanel panel;
        rack->panel(at, 0, panel);
        for (unsigned k = 0; k < panel.window.size(); ++k) {
            const auto& control = panel.window[k];
            if (control.used && std::strcmp(control.name, name) == 0) {
                rack->set_param(at, panel.window_first + k, value);
                return;
            }
        }
        throw std::runtime_error(std::string("missing fixture control: ") + name);
    };
    auto set_control = [&](const char* name, int value) { set_control_of(entry, name, value); };
    // the picker opened on a plug-in track, then another track picked (Pad Bank + pad). Push loads onto the
    // track in hand: over a kit it asks first, as over another plug-in, and one undo brings the kit back.
    if (mode == "picker-track") {
        auto& kit = project.tracks[1];
        open_picker();
        listing("Plugins", 1);
        wait([&] { return rack->view().files[0].several(); });
        tap(SurfaceButton::WheelPush); // the fixture's library: its two plug-ins
        listing("Test.clap", 2);
        const auto rows = rack->view().files;
        const int sine = rows[0].name == "Test Sine" ? 0 : 1;
        CHECK(rows[size_t(sine)].name == "Test Sine");
        turn(sine - int(ui.plugins_cursor));
        const size_t steps = c.edits.size();
        pick_track(1);
        CHECK(ui.track == 1 && ui.page == PageId::Plugins);
        tap(SurfaceButton::WheelPush);
        CHECK(ui.ask == UiState::Ask::ReplacePlugin); // asked first, the kit by its name
        CHECK(std::string(ui.ask_name.data()) == "DRUMS" && kit.engine == EngineKind::Sampler && c.edits.size() == steps);
        tap(SurfaceButton::TopLeft); // B1: not now
        CHECK(ui.ask == UiState::Ask::None && kit.engine == EngineKind::Sampler && ui.page == PageId::Plugins);
        tap(SurfaceButton::WheelPush);
        CHECK(ui.ask == UiState::Ask::ReplacePlugin);
        c.select_track(2); // another track in hand before the answer, as MIDI Remote picks one
        tap(SurfaceButton::TopRight); // B3 REPLACE: the answer was the other track's, so this one is asked about
        CHECK(ui.ask == UiState::Ask::ReplacePlugin && std::string(ui.ask_name.data()) == "BASS");
        CHECK(kit.engine == EngineKind::Sampler && project.tracks[2].engine == EngineKind::Sampler && c.edits.size() == steps);
        tap(SurfaceButton::TopLeft);
        pick_track(1);
        tap(SurfaceButton::WheelPush);
        CHECK(ui.ask == UiState::Ask::ReplacePlugin && std::string(ui.ask_name.data()) == "DRUMS");
        tap(SurfaceButton::TopRight); // B3 REPLACE
        CHECK(ui.ask == UiState::Ask::None && kit.engine == EngineKind::Plugin && kit.plugin == 1);
        CHECK(c.edits.size() == steps + 1 && ui.page == PageId::Stage);
        CHECK(tr.engine == EngineKind::Plugin && tr.plugin == entry); // the track the picker was opened on: as it was
        wait([&] { return ready(1) && ready(entry); });
        undo();
        CHECK(kit.engine == EngineKind::Sampler && kit.pads[0].sample == 3 && std::string(kit.name) == "DRUMS");
        wait([&] { return rack->status(1) == PluginStatus::None; });
        redo();
        CHECK(kit.engine == EngineKind::Plugin && kit.plugin == 1);
        wait([&] { return ready(1) && ready(entry); });
        // The picker opened for the kit itself, Stage's engine card turned to PLUGIN: Push asks before the kit goes.
        auto& bass = project.tracks[2];
        pick_track(2);
        std::array<EngineKind, size_t(EngineKind::Count)> engines {};
        const unsigned kinds = available_engines(engines);
        const auto plugin_kind = std::find(engines.begin(), engines.begin() + kinds, EngineKind::Plugin);
        CHECK(plugin_kind != engines.begin() + kinds);
        c.apply(Binding {Binding::Kind::Engine, 0, 0, int(kinds) - 1, 0}, int(plugin_kind - engines.begin()));
        CHECK(ui.page == PageId::Plugins && bass.engine == EngineKind::Sampler);
        listing("Test.clap", 2);
        turn(sine - int(ui.plugins_cursor));
        tap(SurfaceButton::WheelPush);
        CHECK(ui.ask == UiState::Ask::ReplacePlugin && std::string(ui.ask_name.data()) == "BASS");
        tap(SurfaceButton::TopLeft);
        CHECK(bass.engine == EngineKind::Sampler && std::string(bass.name) == "BASS");
        shell->set_plugins(nullptr);
        return 0;
    }
    // the picker's rows fill in from the rack's thread. The plug-in a question was asked about is the one
    // REPLACE loads, though the list moved meanwhile; a folder's cursor starts at its top when the folder lists, not
    // before; and Push acts on the row the page draws under the cursor.
    if (mode == "picker-rows") {
        const auto folder = fs::path(plugin).parent_path();
        auto loaded = [&] { return fs::path(ref.path).filename().string(); };
        open_picker();
        shell->snapshot(*drawn, now); // drawn at once: the card's rows as the rack has them, never NO CARD
        CHECK(!drawn->plugins.root.empty() && !drawn->plugins.top);
        listing("Plugins", 5); // Alpha, Beta, B, C, Test
        cursor_on("C.clap");
        tap(SurfaceButton::WheelPush);
        CHECK(ui.ask == UiState::Ask::ReplacePlugin && loaded() == "Test.clap");
        fs::copy_file(plugin, folder / "A.clap"); // the web page installs one meanwhile: the rows below it move down
        rack->card_changed();
        listing("Plugins", 6);
        CHECK(int(ui.plugins_cursor) == row_of("C.clap")); // the cursor went with its plug-in
        tap(SurfaceButton::TopRight); // REPLACE
        CHECK(ui.ask == UiState::Ask::None && loaded() == "C.clap");
        wait([&] { return ready(entry); });

        // Push on the second folder, and again before it has listed (the rack's thread is busy opening a plug-in):
        // the second Push is not another row's.
        setenv("MELODEE_TEST_SLOW_OPEN", "1", 1);
        open_picker();
        listing("Plugins", 6);
        cursor_on("Beta");
        rack->reload(entry);
        tick(); // the plug-in opens again: the listings asked for now wait their turn
        tap(SurfaceButton::WheelPush);
        tap(SurfaceButton::WheelPush);
        wait([&] { return ready(entry); });
        listing("Beta", 1); // its plug-in's kind too, though the rack had other work queued behind the listing (its presets)
        for (int k = 0; k < 40; ++k) tick();
        CHECK(fs::path(rack->view().folder).filename() == "Beta" && ui.plugins_cursor == 0 && ui.plugins_count == 1);
        unsetenv("MELODEE_TEST_SLOW_OPEN");

        // B1 BACK, and Push once the folder above has listed but before the shell has matched its cursor to it: the
        // row drawn under the cursor is the one picked (here the plug-in in Beta, so it asks), never another folder.
        tap(SurfaceButton::TopLeft);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (fs::path(rack->view().folder).filename() != "Plugins" && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        CHECK(fs::path(rack->view().folder).filename() == "Plugins");
        shell->snapshot(*drawn, now);
        CHECK(!drawn->plugins.files.empty());
        const auto under = drawn->plugins.files[std::min<size_t>(drawn->ui.plugins_cursor, drawn->plugins.files.size() - 1)];
        CHECK(fs::path(under.path).filename() == "Two.clap"); // Beta's rows, still: the folder above is not in yet
        tap(SurfaceButton::WheelPush); // so Push picks that plug-in, another than the track's: it asks
        CHECK(ui.ask == UiState::Ask::ReplacePlugin && std::string(ui.ask_name.data()) == "Test Sine");
        listing("Plugins", 6); // the folder above is in while the question is up: Two.clap is no row of it
        CHECK(int(ui.plugins_cursor) == row_of("Beta")); // BACK's cursor: on the folder it came out of
        tap(SurfaceButton::TopRight); // REPLACE: the plug-in asked about, still on the card
        CHECK(ui.ask == UiState::Ask::None && loaded() == "Two.clap");
        wait([&] { return ready(entry); });
        open_picker();
        listing("Plugins", 6);

        // A plug-in asked about, then deleted from the web page: REPLACE loads nothing, and says so.
        cursor_on("B.clap");
        tap(SurfaceButton::WheelPush);
        CHECK(ui.ask == UiState::Ask::ReplacePlugin);
        fs::remove(folder / "B.clap");
        rack->card_changed();
        listing("Plugins", 5);
        tap(SurfaceButton::TopRight);
        CHECK(ui.ask == UiState::Ask::None && std::string(ui.notice) == "Not on the card" && loaded() == "Two.clap");

        // B1 from Beta, and B1 again while the plug-in opens again: the listing shown again meanwhile (the one the
        // first B1 asked for) leaves the cursor alone, and the folder above lands on the one it came out of.
        fs::create_directories(folder.parent_path() / "Aaa"); // so Plugins is not the Melodee folder's first row
        setenv("MELODEE_TEST_SLOW_OPEN", "1", 1);
        cursor_on("Beta");
        tap(SurfaceButton::WheelPush);
        listing("Beta", 1, false);
        tap(SurfaceButton::TopLeft);
        listing("Plugins", 5);
        CHECK(int(ui.plugins_cursor) == row_of("Beta"));
        rack->reload(entry);
        tick(); // the plug-in opens again (60 ms)
        tap(SurfaceButton::TopLeft); // up to Melodee: listed once it has opened
        rack->reload(entry);
        tick(); // asked again: the rack's listing, still Plugins', is shown again
        wait([&] { return ready(entry); });
        const auto melodee = fs::path(folder).parent_path().filename().string();
        listing(melodee, rack->view().files.size(), false);
        CHECK(row_of("Plugins") > 0 && int(ui.plugins_cursor) == row_of("Plugins"));

        // Into Beta, then B1 while the plug-in opens again, and the web page changes the card before the folder above is
        // in: listed again, it still lands on the folder B1 left, not its top.
        cursor_on("Plugins");
        tap(SurfaceButton::WheelPush);
        listing("Plugins", 5, false);
        cursor_on("Beta");
        tap(SurfaceButton::WheelPush);
        listing("Beta", 1, false);
        rack->reload(entry);
        tick();
        tap(SurfaceButton::TopLeft);
        rack->card_changed();
        tick();
        wait([&] { return ready(entry); });
        listing("Plugins", 5, false);
        for (int k = 0; k < 20; ++k) tick();
        CHECK(int(ui.plugins_cursor) == row_of("Beta"));
        tap(SurfaceButton::TopLeft); // back up to Melodee, as the next part starts
        wait([&] { return fs::path(rack->view().folder).filename() == melodee; });
        listing(melodee, rack->view().files.size(), false);

        // Push into Plugins while the plug-in opens again, and the web page changes the card before the listing is
        // in: the folder listed again is Plugins, the one asked for, not the one shown.
        cursor_on("Plugins");
        rack->reload(entry);
        tick();
        tap(SurfaceButton::WheelPush);
        fs::copy_file(plugin, folder / "D.clap");
        rack->card_changed();
        tick();
        wait([&] { return ready(entry); });
        listing("Plugins", 6, false);
        for (int k = 0; k < 40; ++k) tick();
        shell->snapshot(*drawn, now);
        CHECK(fs::path(rack->view().folder).filename() == "Plugins" && fs::path(drawn->plugins.folder).filename() == "Plugins");

        // With the picker closed, the rack leaves a listed plug-in's kind for later: opening it to ask would hold up a
        // job behind it (a thaw's, a freeze's). Open again, the picker gets it.
        fs::create_directories(folder / "Gamma");
        fs::copy_file(plugin, folder / "Gamma" / "Three.clap");
        rack->card_changed();
        wait([&] { return row_of("Gamma") >= 0 && fs::path(c.plugin_view.folder).filename() == "Plugins"; });
        listing("Plugins", rack->view().files.size());
        cursor_on("Gamma");
        rack->reload(entry);
        tick(); // the plug-in opens again (60 ms): Gamma lists behind it, then its presets are looked for
        tap(SurfaceButton::WheelPush);
        c.open(PageId::Stage); // and the picker closed meanwhile
        wait([&] { return ready(entry) && fs::path(rack->view().folder).filename() == "Gamma"; });
        for (int k = 0; k < 60; ++k) tick();
        CHECK(rack->view().files.size() == 1 && rack->view().files[0].instrument < 0);
        open_picker();
        listing("Gamma", 1);

        // B1 from Gamma while the plug-in opens again, the picker closed and opened again before that listing is in, and
        // both listings (B1's, then the one opening asks for) in before the shell looks: B1's landing goes with the later
        // one, so a web page change after that lists again on the row under the cursor, not on the row B1 was to land on.
        rack->reload(entry);
        tick(); // the plug-in opens again (60 ms): B1's listing waits behind it
        tap(SurfaceButton::TopLeft); // BACK, to land on Gamma
        const uint32_t back = c.plugin_landing;
        CHECK(back != 0);
        open_picker(); // listed afresh: a later listing than B1's
        const auto later = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (rack->view().request != back + 1 && std::chrono::steady_clock::now() < later)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        CHECK(rack->view().request == back + 1 && fs::path(rack->view().folder).filename() == "Plugins");
        tick();
        wait([&] { return ready(entry); });
        listing("Plugins", rack->view().files.size(), false);
        cursor_on("C.clap");
        fs::copy_file(plugin, folder / "Ab.clap"); // the web page installs one
        rack->card_changed();
        wait([&] { return row_of("Ab.clap") >= 0; });
        listing("Plugins", rack->view().files.size(), false);
        for (int k = 0; k < 20; ++k) tick();
        CHECK(row_of("Ab.clap") >= 0 && int(ui.plugins_cursor) == row_of("C.clap"));
        unsetenv("MELODEE_TEST_SLOW_OPEN");
        shell->set_plugins(nullptr);
        return 0;
    }
    // undo after Clear Track, a track copied over it, or its effect removed brings the plug-in back as it was
    // let go: its values and its preset, not its defaults.
    if (mode == "undo-state") {
        const unsigned gain = 2;
        wait([&] { return ready(gain); });
        const auto list = rack->programs(entry).list;
        CHECK(list.size() == 3);
        const auto high = std::find_if(list.begin(), list.end(), [](const PluginProgram& p) { return p.name == "High"; });
        CHECK(high != list.end());
        c.pick_plugin_program(entry, high->index); // a preset, then a value of its own on top of it
        wait([&] { return values(entry)[0] == 0.75 && rack->programs(entry).current == high->index; });
        set_control("Wave", 1000);
        wait([&] { return values(entry)[1] == 2; });
        const std::string preset = ref.preset;
        CHECK(!preset.empty());
        double wave = 2;
        auto as_it_was = [&] {
            wait([&] { return ready(entry); });
            CHECK(tr.engine == EngineKind::Plugin && tr.plugin == entry);
            CHECK(values(entry)[0] == 0.75 && values(entry)[1] == wave);
            CHECK(std::string(ref.preset) == preset && rack->programs(entry).current == high->index);
        };
        auto let_go = [&](unsigned at) { // the rack has dropped it, and ticked on
            wait([&] { return rack->status(at) == PluginStatus::None; });
            for (int k = 0; k < 20; ++k) tick();
        };
        // Clear Track: Pad Bank + Erase + its pad, then B3. From the master's page, where the effect is too.
        c.open(PageId::Master);
        button(SurfaceButton::PadBank, true);
        button(SurfaceButton::Erase, true);
        pad(0);
        button(SurfaceButton::Erase, false);
        button(SurfaceButton::PadBank, false);
        CHECK(ui.ask == UiState::Ask::ClearTrack);
        tap(SurfaceButton::TopRight);
        CHECK(!tr.used());
        let_go(entry);
        CHECK(rack->states()[entry].empty()); // nothing uses it: the project saved now does not carry it
        undo();
        as_it_was();
        set_control("Wave", 500); // a value changed since, then cleared again: that comes back too
        wait([&] { return values(entry)[1] == 1; });
        wave = 1;
        redo();
        CHECK(!tr.used());
        let_go(entry);
        undo();
        as_it_was();
        // The kit copied over it.
        copy_track(c, 1, 0);
        CHECK(tr.engine == EngineKind::Sampler);
        let_go(entry);
        undo();
        as_it_was();
        // Its effect out of the master's chain: Erase on the chain's page.
        set_control_of(gain, "Gain", 250);
        wait([&] { return values(gain)[2] == 0.5; });
        CHECK(ui.page == PageId::Master && project.chain(master_chain).count() == 1);
        tap(SurfaceButton::Erase);
        CHECK(project.chain(master_chain).count() == 0);
        let_go(gain);
        undo();
        CHECK(project.chain(master_chain).count() == 1 && project.chain(master_chain).slots[0].params[0] == int(gain));
        wait([&] { return ready(gain); });
        CHECK(values(gain)[2] == 0.5);
        as_it_was(); // and the track's own was not touched
        // Another project: nothing of this one's comes back. The same plug-in in the same entry opens from its defaults.
        c.new_project();
        wait([&] { return rack->status(entry) == PluginStatus::None && rack->status(gain) == PluginStatus::None; });
        for (int k = 0; k < 20; ++k) tick();
        c.load_sound(0, EngineKind::Plugin, 0);
        tr.plugin = uint8_t(entry);
        std::snprintf(ref.path, sizeof(ref.path), "%s", plugin.c_str());
        wait([&] { return ready(entry); });
        CHECK(values(entry)[0] == 0.5 && values(entry)[1] == 0);
        shell->set_plugins(nullptr);
        return 0;
    }
    // Both plug-in tracks left on PANEL (their own screens). Another track picked (Pad Bank + pad, or as MIDI Remote
    // picks one), or Stage opened again from the master's page: − / + and the wheel go to that track's PANEL at once,
    // never to its PRESETS (the panel the shell reads twenty times a second still being the other track's, or none).
    if (mode == "panel-lag") {
        wait([&] { return ready(1); });
        const auto panel = stage_find(EngineKind::Plugin, StageKind::PluginScreen);
        CHECK(panel.found);
        for (unsigned t : {0u, 1u}) { ui.stage_group[t] = uint8_t(panel.at.group); ui.stage_sub[t] = uint8_t(panel.at.sub); }
        c.open(PageId::Stage);
        wait([&] { return ui.plugin_panel.entry == entry && ui.plugin_panel.has_screen && ui.plugin_panel.status == PluginStatus::Ready; });
        auto shown = [&] { const auto& t = project.tracks[ui.track % track_count]; return stage_page_now(ui, t, stage_skip(project, ui, t)).kind; };
        CHECK(shown() == StageKind::PluginScreen);
        // The first wheel turn on the other track's PANEL, straight after Pad Bank + pad, reaches its own screen.
        pick_track(1);
        CHECK(values(1)[0] == 0.5);
        turn(10);
        wait([&] { return values(1)[0] == 0.6; });
        pick_track(0);
        wait([&] { return ui.plugin_panel.entry == entry && ui.plugin_panel.screen; });
        const int first = rack->programs(entry).current, second = rack->programs(1).current;
        auto none_armed = [&](SurfaceButton key, int detents) {
            tap(key);
            CHECK(!ui.preset_due);
            turn(detents);
            CHECK(!ui.preset_due && shown() == StageKind::PluginScreen);
        };
        pick_track(1);
        none_armed(SurfaceButton::Plus, 1);
        c.select_track(0); // as MIDI Remote picks a track: no button between
        none_armed(SurfaceButton::Minus, -1);
        c.select_track(1);
        c.open(PageId::Master);
        for (int k = 0; k < 30; ++k) tick();
        c.open(PageId::Stage);
        none_armed(SurfaceButton::Plus, 1);
        c.stage_tick(UINT32_MAX);
        for (int k = 0; k < 20; ++k) tick();
        CHECK(rack->programs(entry).current == first && rack->programs(1).current == second);
        shell->set_plugins(nullptr);
        return 0;
    }
    // The whole PRESETS list of track 1's plug-in open, then track 1 copied onto track 4 (Pad Bank + B3 COPY + pad 1 +
    // pad 4): the copy gets a plug-in entry of its own at once. The list then is never track 1's while track 4 is in
    // hand, so a preset picked there never changes track 1's sound. copy-presets-late: every entry in use, so the copy
    // shares track 1's plug-in (the list is both tracks') until an entry is free, and gets one at the next tick
    // (split_shared_plugins); the list follows it then.
    if (mode == "copy-presets" || late) {
        wait([&] { return ready(1); });
        CHECK(stage_go(ui, EngineKind::Plugin, StageKind::PluginSound));
        c.open(PageId::Stage);
        wait([&] { return ui.plugin_panel.entry == entry && ui.plugin_panel.programs == 3; });
        tap(SurfaceButton::WheelPush);
        CHECK(ui.page == PageId::PluginPresets && ui.presets_entry == entry);
        button(SurfaceButton::PadBank, true);
        tap(SurfaceButton::TopRight); // COPY
        pad(0);
        pad(3);
        button(SurfaceButton::PadBank, false);
        for (int k = 0; k < 20; ++k) tick();
        auto& t4 = project.tracks[3];
        if (late) {
            CHECK(ui.track == 3 && t4.engine == EngineKind::Plugin && t4.plugin == entry);
            CHECK(ui.page == PageId::PluginPresets && ui.presets_entry == entry);
            c.set_effect(4, 0, EffectKind::None); // taken off with no undo step left naming its entry (one gone past the 32 kept)
            CHECK(!c.edits.can_redo());
            tick();
        }
        CHECK(ui.track == 3 && t4.engine == EngineKind::Plugin && t4.plugin != entry && tr.plugin == entry);
        CHECK(ui.page != PageId::PluginPresets || ui.presets_entry == t4.plugin);
        wait([&] { return ready(t4.plugin); });
        if (ui.page != PageId::PluginPresets) { // back on track 4's Stage PRESETS: Push opens its own list
            CHECK(ui.page == PageId::Stage);
            wait([&] { return ui.plugin_panel.entry == t4.plugin && ui.plugin_panel.programs == 3; });
            tap(SurfaceButton::WheelPush);
        }
        CHECK(ui.page == PageId::PluginPresets && ui.presets_entry == t4.plugin);
        const auto list = rack->programs(t4.plugin).list;
        const auto rows = preset_rows(list, "", rack->programs(t4.plugin).current);
        turn(1 - int(ui.presets_cursor));
        tap(SurfaceButton::WheelPush);
        wait([&] { return rack->programs(t4.plugin).current == list[rows[1].at].index; });
        for (int k = 0; k < 20; ++k) tick();
        CHECK(rack->programs(entry).current < 0 && ref.preset[0] == 0); // track 1's plug-in: as it was
        shell->set_plugins(nullptr);
        return 0;
    }
    // TRANSPORT off, a plug-in hears the song stopped throughout: Play, a locate and Stop wake nothing. Turning its
    // TRANSPORT on runs it once, so it hears the song as it is now; turning it off again, the same.
    if (deaf) {
        const unsigned gain = 2;
        auto heard = [&] {
            auto bytes = rack->state(gain);
            CHECK(bytes.size() == 4 + 6 * sizeof(double));
            std::array<double, 6> v;
            std::memcpy(v.data(), bytes.data() + 4, sizeof(v));
            return std::pair<int, int> {int(v[3]), int(v[5])}; // playing, calls
        };
        wait([&] { return ready(gain) && rack->idling(gain); });
        const int before = heard().second;
        CHECK(host->send({Command::Type::Play}));
        for (int k = 0; k < 40; ++k) tick();
        Command jump {Command::Type::Locate};
        jump.position = 0;
        CHECK(host->send(jump));
        for (int k = 0; k < 40; ++k) tick();
        CHECK(heard().second == before && rack->idling(gain));
        c.toggle_plugin_transport(gain); // TRANSPORT on
        tick();
        CHECK(heard().second > before && heard().first == 1);
        wait([&] { return rack->idling(gain); });
        const int on = heard().second;
        c.toggle_plugin_transport(gain); // and off again
        tick();
        CHECK(heard().second > on && heard().first == 0);
        wait([&] { return rack->idling(gain); });
        const int off = heard().second;
        CHECK(host->send({Command::Type::Stop}));
        for (int k = 0; k < 40; ++k) tick();
        CHECK(heard().second == off);
        shell->set_plugins(nullptr);
        return 0;
    }
    // At the MPC's 44.1 kHz, 127 BPM and swing 60 the engine's song position wobbles a little about a straight line
    // (ticks on a block's edge, swung ticks). A sleeping effect sleeps on while the song plays with nothing changed;
    // a locate by a sixteenth and Stop still wake it, each for its block.
    if (steady) {
        const unsigned gain = 2;
        struct Heard { int playing; double beats; int calls; };
        auto heard = [&] {
            auto bytes = rack->state(gain);
            CHECK(bytes.size() == 4 + 6 * sizeof(double));
            std::array<double, 6> v;
            std::memcpy(v.data(), bytes.data() + 4, sizeof(v));
            return Heard {int(v[3]), v[4], int(v[5])};
        };
        wait([&] { return ready(entry) && ready(gain) && rack->idling(gain); });
        CHECK(host->send({Command::Type::Play}));
        for (int k = 0; k < 400; ++k) tick(); // over a second of song: woken by Play, asleep again
        CHECK(rack->idling(gain));
        const int before = heard().calls;
        int awake = 0;
        for (int k = 0; k < 1500; ++k) { tick(); awake += rack->idling(gain) ? 0 : 1; }
        CHECK(heard().calls == before && awake == 0);
        const auto at = heard();
        Command back {Command::Type::Locate};
        back.position = host->ticks() - sixteenth_ticks; // a sixteenth back
        CHECK(host->send(back));
        tick();
        CHECK(heard().calls > at.calls && heard().playing == 1);
        for (int k = 0; k < 400; ++k) tick();
        const int still = heard().calls;
        CHECK(host->send({Command::Type::Stop}));
        tick();
        CHECK(heard().calls > still && heard().playing == 0);
        shell->set_plugins(nullptr);
        return 0;
    }
    // A sleeping plug-in given a preset (one of its own, or a user preset, which opens it again with that state): it is
    // run for at least the next block, so it can take the new values; one nobody touched sleeps on.
    if (mode == "load-wake") {
        auto calls = [&] {
            auto bytes = rack->state(entry);
            CHECK(bytes.size() == 4 + 6 * sizeof(double));
            double v[6];
            std::memcpy(v, bytes.data() + 4, sizeof(v));
            return int(v[5]);
        };
        auto asleep_then = [&](auto load) { // asleep a while, then the load: run for the block after it
            wait([&] { return ready(entry) && rack->idling(entry); });
            const int before = calls();
            for (int k = 0; k < 40; ++k) tick();
            CHECK(calls() == before && rack->idling(entry));
            load();
        };
        wait([&] { return ready(entry) && rack->programs(entry).list.size() == 3; });
        const auto list = rack->programs(entry).list;
        int was = 0;
        asleep_then([&] { was = calls(); c.pick_plugin_program(entry, list[1].index); }); // as PRESETS or a companion screen picks
        wait([&] { return rack->programs(entry).current == list[1].index && calls() > was; });
        // A user preset: saved, then loaded while it sleeps (the rack opens it again with the preset's state).
        CHECK(stage_go(ui, EngineKind::Plugin, StageKind::PluginSound));
        c.open(PageId::Stage);
        button(SurfaceButton::Shift, true);
        tap(SurfaceButton::SampleSelect);
        button(SurfaceButton::Shift, false);
        CHECK(ui.name.target == NameTarget::PluginPreset);
        tap(SurfaceButton::TopRight);
        wait([&] { return rack->programs(entry).list.size() == 4 && !rack->programs(entry).finding; });
        c.open(PageId::Master);
        asleep_then([&] { rack->load_user_preset(entry, "My Patch"); });
        wait([&] { return ready(entry) && std::string(ref.preset) == "/user/My Patch"; });
        wait([&] { return !rack->idling(entry) && calls() > 0; });
        shell->set_plugins(nullptr);
        return 0;
    }
    // A plug-in effect picked into an empty slot (the effect picker's Plug-ins, from a chain's plus) is one undo step:
    // undo empties the slot again, redo brings the effect back as it was, its values too.
    if (mode == "effect-undo") {
        const auto& master = project.chain(master_chain);
        CHECK(master.count() == 0);
        c.open(PageId::Master);
        c.open_plugin_picker(UiState::PluginTarget::Effect, master_chain, 0);
        listing("Plugins", 1);
        tap(SurfaceButton::WheelPush); // the fixture's library: its two plug-ins
        listing("Test.clap", 2);
        const auto rows = rack->view().files;
        const int gain_row = rows[0].name == "Test Gain" ? 0 : 1;
        CHECK(rows[size_t(gain_row)].name == "Test Gain");
        turn(gain_row - int(ui.plugins_cursor));
        const size_t steps = c.edits.size();
        tap(SurfaceButton::WheelPush);
        CHECK(master.count() == 1 && master.slots[0].kind == plugin_effect_kind && c.edits.size() == steps + 1);
        const unsigned gain = unsigned(master.slots[0].params[0]);
        wait([&] { return ready(gain); });
        set_control_of(gain, "Gain", 250);
        wait([&] { return values(gain)[2] == 0.5; });
        c.open(PageId::Master);
        undo();
        CHECK(master.count() == 0);
        wait([&] { return rack->status(gain) == PluginStatus::None; });
        for (int k = 0; k < 20; ++k) tick();
        redo();
        CHECK(master.count() == 1 && master.slots[0].kind == plugin_effect_kind && unsigned(master.slots[0].params[0]) == gain);
        wait([&] { return ready(gain); });
        CHECK(values(gain)[2] == 0.5);
        shell->set_plugins(nullptr);
        return 0;
    }
    // A program picked by a companion screen (Context::pick_plugin_program) for a frozen track's plug-in asks to
    // unfreeze first, as PRESETS and Stage do; not frozen, it loads.
    if (mode == "companion-frozen") {
        wait([&] { return ready(entry) && rack->programs(entry).list.size() == 3; });
        const auto list = rack->programs(entry).list;
        c.freeze_requested[0].state = FreezeState::Frozen;
        c.pick_plugin_program(entry, list[1].index);
        CHECK(ui.ask == UiState::Ask::Unfreeze);
        tap(SurfaceButton::TopLeft);
        for (int k = 0; k < 20; ++k) tick();
        CHECK(rack->programs(entry).current < 0 && ref.preset[0] == 0);
        c.freeze_requested[0] = {};
        c.pick_plugin_program(entry, list[1].index);
        wait([&] { return rack->programs(entry).current == list[1].index; });
        shell->set_plugins(nullptr);
        return 0;
    }
    // Push on a preset in the whole PRESETS list of a frozen track's plug-in asks to unfreeze first, as a turn on Stage
    // does; B1 there leaves its sound as it was. Not frozen, Push loads it.
    if (mode == "presets-frozen") {
        CHECK(stage_go(ui, EngineKind::Plugin, StageKind::PluginSound));
        c.open(PageId::Stage);
        wait([&] { return ui.plugin_panel.entry == entry && ui.plugin_panel.programs == 3; });
        tap(SurfaceButton::WheelPush); // the whole list
        CHECK(ui.page == PageId::PluginPresets && ui.presets_entry == entry);
        const auto list = rack->programs(entry).list;
        const auto rows = preset_rows(list, "", -1);
        turn(1 - int(ui.presets_cursor));
        c.freeze_requested[0].state = FreezeState::Frozen;
        tap(SurfaceButton::WheelPush);
        CHECK(ui.ask == UiState::Ask::Unfreeze);
        tap(SurfaceButton::TopLeft); // not now
        for (int k = 0; k < 20; ++k) tick();
        CHECK(ui.ask == UiState::Ask::None && rack->programs(entry).current < 0 && ref.preset[0] == 0);
        c.freeze_requested[0] = {};
        tap(SurfaceButton::WheelPush);
        wait([&] { return rack->programs(entry).current == list[rows[1].at].index; });
        shell->set_plugins(nullptr);
        return 0;
    }
    // A preset turned on Stage's PRESETS, and within the quarter second before it loads, the PRESETS list of an effect
    // on the track's chain opened (its slot's sheet): the turn still lands on the track's plug-in, and the effect's
    // list takes the cursor over only then; the effect plays on as it was.
    if (mode == "turn-effect") {
        const unsigned gain = 3;
        wait([&] { return ready(entry) && ready(gain) && rack->programs(gain).list.size() == 1; });
        CHECK(stage_go(ui, EngineKind::Plugin, StageKind::PluginSound));
        c.open(PageId::Stage);
        wait([&] { return ui.plugin_panel.entry == entry && ui.plugin_panel.programs == 3; });
        const auto list = rack->programs(entry).list;
        CHECK(rack->programs(entry).current < 0);
        turn(1); // none yet: the first
        CHECK(ui.preset_due && ui.presets_entry == entry && ui.presets_cursor == 0);
        c.open(PageId::Rack);
        c.open_plugin_presets(gain);
        CHECK(ui.page == PageId::PluginPresets && ui.presets_entry == gain);
        c.stage_tick(now += stage_preset_ms);
        wait([&] { return rack->programs(entry).current == list[0].index; });
        for (int k = 0; k < 20; ++k) tick();
        CHECK(rack->programs(gain).current < 0 && ui.presets_entry == gain);
        shell->set_plugins(nullptr);
        return 0;
    }
    // A deep freeze lets track 1's plug-in go (PluginRack::sync with its track asleep) while the rack's thread is busy
    // opening another (60 ms), so the freeze's Sleep, which takes the plug-in's state, is still to run. Its track
    // cleared then, or woken again, before it runs: the plug-in comes back as it was edited, not as it was opened.
    if (mode == "freeze-race") {
        wait([&] { return ready(entry) && ready(1); });
        const auto fresh = rack->state(entry); // as opened: what another project's file gives this entry below
        set_control("Wave", 1000);
        wait([&] { return values(entry)[1] == 2; });
        setenv("MELODEE_TEST_SLOW_OPEN", "1", 1);
        auto busy_then_frozen = [&] {
            rack->reload(1);
            rack->sync(project, 0); // the other plug-in opening again, on the rack's thread
            rack->sync(project, 1); // track 1 deep frozen: its plug-in to sleep once that is done
            CHECK(rack->asleep(entry));
        };
        busy_then_frozen();
        tr.engine = EngineKind::None; // cleared, the Sleep still to run
        rack->sync(project, 1);
        wait([&] { return ready(1) && rack->status(entry) == PluginStatus::None; });
        for (int k = 0; k < 20; ++k) tick();
        tr.engine = EngineKind::Plugin; // and back (an undo)
        tr.plugin = uint8_t(entry);
        wait([&] { return ready(entry); });
        CHECK(values(entry)[1] == 2);
        set_control("Wave", 500);
        wait([&] { return values(entry)[1] == 1; });
        busy_then_frozen();
        rack->sync(project, 0); // thawed before the Sleep ran
        wait([&] { return ready(entry) && ready(1); });
        for (int k = 0; k < 20; ++k) tick();
        CHECK(values(entry)[1] == 1);
        // Another project opens while the Sleep waits, the same plug-in in the same entry and awake there: it opens with
        // that project's state, not this one's edits.
        PluginStates next {};
        next[entry] = fresh;
        next[1] = fresh;
        busy_then_frozen();
        rack->set_states(next);
        rack->sync(project, 0);
        CHECK(rack->state(entry) == fresh); // what a save now would keep
        wait([&] { return ready(entry) && ready(1); });
        for (int k = 0; k < 20; ++k) tick();
        CHECK(values(entry)[1] == 0);
        // Another project opens in which that track is deep frozen: the Sleep keeps that project's state for the thaw.
        set_control("Wave", 1000);
        wait([&] { return values(entry)[1] == 2; });
        rack->set_states(next);
        rack->sync(project, 1);
        CHECK(rack->state(entry) == fresh);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (rack->status(entry) != PluginStatus::None && std::chrono::steady_clock::now() < until) // no tick: it would wake it
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::this_thread::sleep_for(std::chrono::milliseconds(300)); // the rack's thread lets it go
        CHECK(rack->state(entry) == fresh);
        rack->sync(project, 0); // thawed: opened with it
        wait([&] { return ready(entry) && ready(1); });
        CHECK(values(entry)[1] == 0);
        // Another project opens (set_states), or a state is put back (restore), while the plug-in is opening again with
        // the state it had, and that load is done before the shell's next sync: the plug-in it opened is not this
        // state's. A read then (a save, a copy's clone) gives the state put in, and the next sync opens it with that.
        for (bool whole : {true, false}) {
            set_control("Wave", 1000);
            wait([&] { return values(entry)[1] == 2; });
            rack->reload(entry);
            rack->sync(project, 0); // opening again (60 ms), with Wave 2
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            CHECK(rack->status(entry) == PluginStatus::Loading);
            if (whole) rack->set_states(next);
            else rack->restore(entry, fresh);
            std::this_thread::sleep_for(std::chrono::milliseconds(300)); // no tick: the load is done meanwhile
            CHECK(rack->state(entry) == fresh);
            rack->sync(project, 0);
            wait([&] { return ready(entry) && ready(1); });
            for (int k = 0; k < 20; ++k) tick();
            CHECK(values(entry)[1] == 0 && rack->state(entry) == fresh);
        }
        unsetenv("MELODEE_TEST_SLOW_OPEN");
        shell->set_plugins(nullptr);
        return 0;
    }
    // A plug-in its gate has put to sleep hears the transport change: on Play, on a jump and on Stop it runs the very
    // block that carries the new transport, with that block's song position; with no change it sleeps on.
    if (mode == "transport-wake") {
        const unsigned gain = 2;
        struct Heard { int playing; double beats; int calls; };
        auto heard = [&](unsigned at) {
            auto bytes = rack->state(at);
            CHECK(bytes.size() == 4 + 6 * sizeof(double));
            std::array<double, 6> v;
            std::memcpy(v.data(), bytes.data() + 4, sizeof(v));
            return Heard {int(v[3]), v[4], int(v[5])};
        };
        auto sleeps_on = [&](unsigned at) { // no change: no process call for a while
            const int calls = heard(at).calls;
            for (int k = 0; k < 40; ++k) tick();
            CHECK(rack->idling(at) && heard(at).calls == calls);
        };
        wait([&] { return ready(entry) && ready(gain); });
        wait([&] { return rack->idling(entry) && rack->idling(gain); }); // stopped, silent: both asleep
        sleeps_on(entry);
        sleeps_on(gain);
        const int effect_before = heard(gain).calls;
        CHECK(host->send({Command::Type::Play}));
        tick(); // the block that starts the song
        CHECK(heard(gain).calls > effect_before && heard(gain).playing == 1 && heard(gain).beats < 0.25);
        CHECK(heard(entry).playing == 1); // an instrument is run while the song plays anyway
        wait([&] { return rack->idling(gain); }); // the effect asleep again, the song playing on
        sleeps_on(gain);
        const auto before_jump = heard(gain);
        CHECK(before_jump.playing == 1 && before_jump.beats > 0.5);
        Command jump {Command::Type::Locate};
        jump.position = 0; // back to the patterns' top
        CHECK(host->send(jump));
        tick();
        CHECK(heard(gain).calls > before_jump.calls && heard(gain).playing == 1 && heard(gain).beats < 0.25);
        wait([&] { return rack->idling(gain); });
        const int instrument_before = heard(entry).calls, effect_still = heard(gain).calls;
        CHECK(host->send({Command::Type::Stop}));
        tick(); // the block that stops it: the effect asleep, the instrument quiet long enough to sleep at once
        CHECK(heard(gain).calls > effect_still && heard(gain).playing == 0);
        CHECK(heard(entry).calls > instrument_before && heard(entry).playing == 0);
        wait([&] { return rack->idling(entry) && rack->idling(gain); }); // and both sleep again
        sleeps_on(entry);
        sleeps_on(gain);
        shell->set_plugins(nullptr);
        return 0;
    }
    // Every plug-in entry in use, some undo history, then track 1 copied onto track 2 (a plug-in track): no entry can
    // be had for the copy without the one only the copy's own step names, so none is taken. The history stays, the
    // copy's step with it (the copy shares track 1's plug-in meanwhile), and undo brings track 2's plug-in back.
    if (mode == "copy-full") {
        wait([&] { return ready(entry) && ready(1); });
        for (int n = 0; n < 5; ++n) { Edit step; step.what = "Step"; c.edits.push(std::move(step)); }
        c.open(PageId::Master);
        button(SurfaceButton::PadBank, true);
        tap(SurfaceButton::TopRight); // COPY
        pad(0);
        pad(1);
        button(SurfaceButton::PadBank, false);
        auto& t2 = project.tracks[1];
        CHECK(t2.engine == EngineKind::Plugin && t2.plugin == entry);
        CHECK(c.edits.size() == 6);
        for (int k = 0; k < 10; ++k) tick();
        CHECK(c.edits.size() == 6 && c.edits.can_undo() && t2.plugin == entry);
        undo();
        CHECK(t2.engine == EngineKind::Plugin && t2.plugin == 1 && fs::path(project.plugins[1].path).filename() == "Test.clap");
        wait([&] { return ready(1); });
        CHECK(c.edits.size() == 5);
        shell->set_plugins(nullptr);
        return 0;
    }
    // Track 1 copied onto track 2 with every entry in use: the copy shares track 1's plug-in. Track 2 frozen, a program
    // or preset loaded into that plug-in asks "Unfreeze track 2?" first, from track 1 too: a companion screen's pick,
    // Push on PRESETS and − / + on Stage's PRESETS. Not now (B1) leaves it as it was; unfrozen, the pick loads.
    if (mode == "shared-frozen") {
        wait([&] { return ready(entry) && ready(1) && rack->programs(entry).list.size() == 3; });
        c.open(PageId::Master);
        button(SurfaceButton::PadBank, true);
        tap(SurfaceButton::TopRight); // COPY
        pad(0);
        pad(1);
        button(SurfaceButton::PadBank, false);
        auto& t2 = project.tracks[1];
        CHECK(t2.engine == EngineKind::Plugin && t2.plugin == entry && tr.plugin == entry);
        c.freeze_requested[1].state = FreezeState::Frozen;
        const auto list = rack->programs(entry).list;
        auto unchanged = [&] {
            CHECK(ui.ask == UiState::Ask::Unfreeze);
            tap(SurfaceButton::TopLeft); // not now
            for (int k = 0; k < 20; ++k) tick();
            CHECK(ui.ask == UiState::Ask::None && rack->programs(entry).current < 0 && ref.preset[0] == 0);
        };
        c.pick_plugin_program(entry, list[1].index);
        unchanged();
        c.select_track(0);
        CHECK(stage_go(ui, EngineKind::Plugin, StageKind::PluginSound));
        c.open(PageId::Stage);
        wait([&] { return ui.plugin_panel.entry == entry && ui.plugin_panel.programs == 3; });
        tap(SurfaceButton::Plus); // Stage's PRESETS: the next preset
        unchanged();
        tap(SurfaceButton::WheelPush); // the whole list
        CHECK(ui.page == PageId::PluginPresets && ui.presets_entry == entry);
        turn(1 - int(ui.presets_cursor));
        tap(SurfaceButton::WheelPush);
        unchanged();
        c.freeze_requested[1] = {};
        c.pick_plugin_program(entry, list[1].index);
        wait([&] { return rack->programs(entry).current == list[1].index; });
        shell->set_plugins(nullptr);
        return 0;
    }
    // Plug-in entries and undo: an entry an undo still names is never given to a new plug-in, even once all 32 have
    // been set; a track copy undone and done again keeps the one entry it had (none taken each time); and when every
    // entry is named, the oldest undo step goes so a new plug-in still has one.
    if (mode == "undo-entries") {
        auto set = [&] { unsigned n = 0; for (const auto& p : project.plugins) n += p.used(); return n; };
        set_control("Wave", 1000);
        wait([&] { return values(entry)[1] == 2; });
        c.open(PageId::Master);
        button(SurfaceButton::PadBank, true);
        button(SurfaceButton::Erase, true);
        pad(0);
        button(SurfaceButton::Erase, false);
        button(SurfaceButton::PadBank, false);
        tap(SurfaceButton::TopRight); // Clear track 1
        CHECK(!tr.used());
        wait([&] { return rack->status(entry) == PluginStatus::None; });
        for (int k = 0; k < 20; ++k) tick();
        c.select_track(3);
        c.open_plugin_picker(UiState::PluginTarget::Instrument);
        listing("Plugins", 2);
        cursor_on("B.clap");
        tap(SurfaceButton::WheelPush); // B.clap onto empty track 4: not into track 1's entry
        auto& t4 = project.tracks[3];
        CHECK(t4.engine == EngineKind::Plugin && t4.plugin != entry);
        CHECK(fs::path(ref.path).filename() == "Test.clap" && fs::path(project.plugins[t4.plugin].path).filename() == "B.clap");
        wait([&] { return ready(t4.plugin); });
        CHECK(values(t4.plugin)[1] == 0); // a new plug-in from its defaults
        c.open(PageId::Master);
        undo(); // the new track
        CHECK(!t4.used());
        undo(); // the clear: track 1's own plug-in back, as it was
        CHECK(tr.engine == EngineKind::Plugin && tr.plugin == entry && fs::path(ref.path).filename() == "Test.clap");
        wait([&] { return ready(entry); });
        CHECK(values(entry)[1] == 2);

        // Track 1 copied onto track 5, then undone and done again five times: the copy keeps one entry and its state.
        const unsigned before = set();
        button(SurfaceButton::PadBank, true);
        tap(SurfaceButton::TopRight); // COPY
        pad(0);
        pad(4);
        button(SurfaceButton::PadBank, false);
        auto& t5 = project.tracks[4];
        CHECK(t5.engine == EngineKind::Plugin && t5.plugin != entry && tr.plugin == entry);
        const unsigned copy = t5.plugin;
        wait([&] { return ready(copy); });
        CHECK(values(copy)[1] == 2);
        set_control_of(copy, "Wave", 500);
        wait([&] { return values(copy)[1] == 1; });
        for (int round = 0; round < 5; ++round) {
            undo();
            CHECK(!t5.used());
            for (int k = 0; k < 10; ++k) tick();
            redo();
            CHECK(t5.engine == EngineKind::Plugin && t5.plugin == copy);
            wait([&] { return ready(copy); });
        }
        for (int k = 0; k < 10; ++k) tick();
        CHECK(set() == before && t5.plugin == copy && tr.plugin == entry && values(copy)[1] == 1 && values(entry)[1] == 2);

        // Every entry nothing uses named by an undo step: the oldest step goes, and B.clap still gets an entry.
        c.edits.clear();
        std::array<bool, max_plugins> in_use {};
        for (unsigned e = 0; e < max_plugins; ++e) in_use[e] = plugin_used_by(project, e);
        unsigned named = 0, oldest = max_plugins;
        for (unsigned e = 0; e < max_plugins; ++e) {
            if (in_use[e]) continue;
            Edit step;
            step.what = "Replace X";
            step.plugin = std::make_unique<PluginSwap>();
            step.plugin->entry = uint8_t(e);
            if (oldest == max_plugins) oldest = e;
            c.edits.push(std::move(step));
            ++named;
        }
        CHECK(named > 0 && c.edits.size() == named);
        c.select_track(6);
        c.open_plugin_picker(UiState::PluginTarget::Instrument);
        listing("Plugins", 2);
        cursor_on("B.clap");
        tap(SurfaceButton::WheelPush);
        auto& t7 = project.tracks[6];
        CHECK(t7.engine == EngineKind::Plugin && t7.plugin == oldest);
        CHECK(c.edits.size() == named); // the oldest step dropped, the new track's kept
        CHECK(std::string(ui.notice) == "Oldest undo step let go"); // and said
        shell->set_plugins(nullptr);
        return 0;
    }
    // Undo two steps at once (no tick between them): a plug-in replaced, edited, its track cleared. The first step brings
    // the track back, the second the plug-in before; redo then brings the edited one back as it was, not its defaults.
    if (mode == "undo-steps") {
        auto loaded = [&] { return fs::path(ref.path).filename().string(); };
        open_picker();
        listing("Plugins", 2);
        cursor_on("C.clap");
        tap(SurfaceButton::WheelPush);
        CHECK(ui.ask == UiState::Ask::ReplacePlugin);
        tap(SurfaceButton::TopRight);
        CHECK(loaded() == "C.clap");
        wait([&] { return ready(entry); });
        set_control("Wave", 1000);
        wait([&] { return values(entry)[1] == 2; });
        c.open(PageId::Master);
        button(SurfaceButton::PadBank, true);
        button(SurfaceButton::Erase, true);
        pad(0);
        button(SurfaceButton::Erase, false);
        button(SurfaceButton::PadBank, false);
        tap(SurfaceButton::TopRight);
        CHECK(!tr.used());
        wait([&] { return rack->status(entry) == PluginStatus::None; });
        for (int k = 0; k < 20; ++k) tick();
        undo(); // the clear
        undo(); // and the replace, in the same tick
        CHECK(tr.used() && loaded() == "Test.clap");
        wait([&] { return ready(entry) && loaded() == "Test.clap"; });
        CHECK(values(entry)[1] == 0);
        redo();
        CHECK(loaded() == "C.clap");
        wait([&] { return ready(entry); });
        for (int k = 0; k < 20; ++k) tick();
        CHECK(values(entry)[1] == 2);
        shell->set_plugins(nullptr);
        return 0;
    }
    // Stage left on PANEL, and PANEL passed over (plug-in screens switched off in Settings): the page shows
    // PRESETS, and what it draws from holds the track's plug-in's presets.
    if (mode == "stage-presets") {
        CHECK(stage_go(ui, EngineKind::Plugin, StageKind::PluginScreen));
        c.open(PageId::Stage);
        wait([&] { return ui.plugin_panel.entry == entry && ui.plugin_panel.has_screen && ui.plugin_panel.programs == 3; });
        auto shown = [&] { return stage_page_now(ui, tr, stage_skip(project, ui, tr)).kind; };
        CHECK(shown() == StageKind::PluginScreen);
        auto frame = std::make_unique<Snapshot>();
        shell->snapshot(*frame, now);
        ui.settings.plugin_skins = false; // Settings > SCREEN > Plug-in screens: off
        CHECK(shown() == StageKind::PluginSound);
        shell->snapshot(*frame, ++now);
        CHECK(frame->presets.list.size() == 3);
        tap(SurfaceButton::Plus); // and its keys walk the list it shows
        CHECK(ui.preset_due && ui.presets_cursor == 0);
        shell->set_plugins(nullptr);
        return 0;
    }
    // the whole PRESETS list opened for a track's plug-in, then another track picked: the list is the plug-in
    // of the track in hand, and a track with no presets leaves the page.
    if (mode == "presets-track") {
        wait([&] { return ready(1); });
        CHECK(stage_go(ui, EngineKind::Plugin, StageKind::PluginSound));
        c.open(PageId::Stage);
        wait([&] { return ui.plugin_panel.entry == entry && ui.plugin_panel.programs == 3; });
        tap(SurfaceButton::WheelPush); // Push on Stage's PRESETS: the whole list
        CHECK(ui.page == PageId::PluginPresets && ui.presets_entry == entry);
        pick_track(1);
        CHECK(ui.track == 1 && ui.page == PageId::PluginPresets && ui.presets_entry == 1);
        const auto list = rack->programs(1).list;
        const auto rows = preset_rows(list, "", -1);
        CHECK(list.size() == 3 && rows.size() == 3 && !rows[1].folder);
        turn(1 - int(ui.presets_cursor));
        tap(SurfaceButton::WheelPush);
        const auto& picked = list[rows[1].at];
        wait([&] { return rack->programs(1).current == picked.index; });
        CHECK(std::string(project.plugins[1].preset) == program_key(picked));
        for (int k = 0; k < 20; ++k) tick();
        CHECK(rack->programs(entry).current < 0 && ref.preset[0] == 0); // the first track's plug-in: as it was
        tap(SurfaceButton::TopLeft); // B1: back where the list was opened from, now this track's Stage
        CHECK(ui.page == PageId::Stage && ui.track == 1);
        tap(SurfaceButton::WheelPush);
        CHECK(ui.page == PageId::PluginPresets && ui.presets_entry == 1);
        pick_track(2); // a kit has no presets list
        CHECK(ui.track == 2 && ui.page == PageId::Stage);
        // An effect's list: one on the old track's chain goes back once another track is picked; the master's stays.
        wait([&] { return ready(3) && ready(4) && rack->programs(3).list.size() == 1 && rack->programs(4).list.size() == 1; });
        pick_track(1);
        c.open(PageId::Rack); // the track's chain, and its slot sheet's PRESETS
        c.open_plugin_presets(3);
        CHECK(ui.page == PageId::PluginPresets && ui.presets_entry == 3);
        pick_track(0);
        CHECK(ui.track == 0 && ui.page == PageId::Rack);
        c.open(PageId::Master);
        c.open_plugin_presets(4);
        CHECK(ui.page == PageId::PluginPresets && ui.presets_entry == 4);
        pick_track(1);
        CHECK(ui.track == 1 && ui.page == PageId::PluginPresets && ui.presets_entry == 4);
        shell->set_plugins(nullptr);
        return 0;
    }
    // Stage's PRESETS on a plug-in track: Minus / Plus walk its presets as the wheel does and stop at the list's
    // ends; the pads keep their keys (the octave and the Drums bank those keys move elsewhere). Its face, a
    // plug-in with no presets, and the track's other pages: the octave, as before.
    if (presetKeys || faceKeys) {
        for (unsigned n = 0; n < unsigned(PageId::Count); ++n) { // every page: the shell calls its handlers unasked
            const auto& spec = page_spec(PageId(n));
            CHECK(spec.draw && spec.pad && spec.wheel && spec.push && spec.soft && spec.nudge);
        }
        CHECK(stage_go(ui, EngineKind::Plugin, StageKind::PluginSound));
        c.open(PageId::Stage);
        const int8_t octave = tr.octave;
        const uint8_t bank = tr.bank;
        if (faceKeys) {
            wait([&] { return ui.plugin_panel.entry == entry && ui.plugin_panel.status == PluginStatus::Ready; });
            CHECK(!ui.plugin_panel.programs);
            tap(SurfaceButton::Plus);
            CHECK(tr.octave == octave + 1 && !ui.preset_due);
            shell->set_plugins(nullptr);
            return 0;
        }
        wait([&] { return ui.plugin_panel.entry == entry && ui.plugin_panel.programs == 3; });
        const auto list = rack->programs(entry).list;
        CHECK(list.size() == 3);
        auto level = [&](unsigned n) { return list[n].name == "Low" ? 0.25 : list[n].name == "Mid" ? 0.5 : 0.75; };
        auto rest = [&] { c.stage_tick(now += stage_preset_ms); }; // the turning stopped: the lights' tick loads it
        auto lands = [&](unsigned n) {
            CHECK(ui.preset_due && ui.presets_cursor == n);
            rest();
            wait([&] { return rack->programs(entry).current == list[n].index && state()[0] == level(n); });
        };
        auto wheel = [&](int detents) {
            SurfaceEvent event;
            event.input.kind = SurfaceInput::Kind::Control;
            event.input.index = uint8_t(SurfaceControl::Wheel);
            event.input.delta = detents;
            shell->surface(event, ++now);
        };
        CHECK(rack->programs(entry).current < 0);
        tap(SurfaceButton::Minus); // none yet: the first, whichever key
        CHECK(ui.preset_due && ui.presets_cursor == 0);
        ui.preset_due = 0; // let go before it landed: none still
        tap(SurfaceButton::Plus);
        lands(0);
        tap(SurfaceButton::Plus);
        lands(1);
        button(SurfaceButton::Sample, true); // Sample held: the keys are Keys' octave, as its cards are Keys'
        tap(SurfaceButton::Plus);
        CHECK(tr.octave == octave + 1 && !ui.preset_due);
        tap(SurfaceButton::Minus);
        CHECK(tr.octave == octave && !ui.preset_due);
        button(SurfaceButton::Sample, false);
        CHECK(ui.page == PageId::Stage && stage_page_now(ui, tr, false).kind == StageKind::PluginSound);
        auto edit = [&] { set_control("Level", 1000); wait([&] { return state()[0] == 1; }); };
        auto kept = [&](unsigned n) { // nothing is sent to the plug-in: the preset it plays, as it was edited
            c.dirty = false;
            rest();
            CHECK(!c.dirty && !ui.preset_due);
            for (int k = 0; k < 20; ++k) tick();
            CHECK(state()[0] == 1 && rack->programs(entry).current == list[n].index);
        };
        edit();
        tap(SurfaceButton::Plus); // a press taken back before it lands
        tap(SurfaceButton::Minus);
        CHECK(ui.preset_due && ui.presets_cursor == 1);
        kept(1);
        wheel(1); // the wheel and the keys walk the same list
        lands(2);
        wheel(-1);
        lands(1);
        for (int n = 0; n < 3; ++n) tap(SurfaceButton::Plus); // three presses from the second: the last, no further
        lands(2);
        edit();
        tap(SurfaceButton::Plus); // against the end: nothing is armed
        CHECK(!ui.preset_due);
        wheel(1);
        CHECK(!ui.preset_due);
        kept(2);
        tap(SurfaceButton::Minus);
        lands(1);
        tap(SurfaceButton::Minus);
        lands(0);
        tap(SurfaceButton::Minus); // and against the start
        CHECK(!ui.preset_due);
        tr.layout = PadLayout::Drums; // a drum machine's pads: their bank stays too
        tap(SurfaceButton::Plus);
        lands(1);
        CHECK(tr.octave == octave && tr.bank == bank);
        tr.layout = PadLayout::Scale;

        // Two user presets: loading one opens the plug-in again, then its presets are looked for again.
        auto save = [&](const char* name, size_t count) {
            chord();
            CHECK(ui.name.target == NameTarget::PluginPreset);
            if (name) ui.name.set(name);
            tap(SurfaceButton::TopRight);
            wait([&] { return rack->programs(entry).list.size() == count && ui.plugin_panel.programs == count; });
        };
        save(nullptr, 4);
        save("Other", 5);
        const auto all = rack->programs(entry).list; // High, Low, Mid, My Patch, Other
        CHECK(all[3].user() && all[3].name == "My Patch" && all[4].user() && all[4].name == "Other");
        auto playing = [&] { return rack->programs(entry).current; };
        auto opening = [&] { return rack->status(entry) == PluginStatus::Loading; };
        auto looking = [&] { return rack->status(entry) == PluginStatus::Ready && rack->programs(entry).finding; };
        auto whole = [&] { return rack->status(entry) == PluginStatus::Ready && !rack->programs(entry).finding && ui.plugin_panel.programs == 5; };
        auto to_factory = [&] { // back onto Mid, the last of the plug-in's own
            wait(whole);
            while (playing() != all[2].index) {
                tap(SurfaceButton::Minus);
                rest();
                wait([&] { return whole() && playing() == all[ui.presets_cursor].index; });
            }
        };
        to_factory();
        tap(SurfaceButton::Plus);
        CHECK(ui.preset_due && ui.presets_cursor == 3);
        rest(); // My Patch: read from the card, then the plug-in opens again
        wait(opening);
        for (auto key : {SurfaceButton::Plus, SurfaceButton::Plus, SurfaceButton::Minus}) { // not the octave's meanwhile
            tap(key);
            CHECK(tr.octave == octave && !ui.preset_due);
        }
        c.freeze_requested[0].state = FreezeState::Frozen; // a frozen track asks, opening (or let go by a deep freeze)
        tap(SurfaceButton::Plus);
        CHECK(ui.ask == UiState::Ask::Unfreeze);
        tap(SurfaceButton::TopLeft);
        CHECK(ui.ask == UiState::Ask::None && opening());
        c.freeze_requested[0] = {};
        wait(looking); // open again, its list not whole yet: said, and nothing moves
        for (auto key : {SurfaceButton::Plus, SurfaceButton::Minus}) {
            ui.notice[0] = 0;
            tap(key);
            CHECK(std::string(ui.notice) == "Looking for presets" && tr.octave == octave && !ui.preset_due);
        }
        wait(whole);
        CHECK(playing() == all[3].index && std::string(ref.preset) == "/user/My Patch");

        // A press that lands while the list is looked for again is dropped: its place would name another preset.
        to_factory();
        tap(SurfaceButton::Plus);
        rest(); // My Patch again
        tap(SurfaceButton::Minus); // before the plug-in has it: from Mid to Low, the second of the whole list
        CHECK(ui.preset_due && ui.presets_cursor == 1);
        wait(looking); // only the user presets are listed: the second is Other
        rest();
        CHECK(!ui.preset_due);
        bool again = false;
        for (int k = 0; k < 120; ++k) { tick(); again |= opening() || std::string(ref.preset) != "/user/My Patch"; }
        CHECK(!again && whole() && playing() == all[3].index);

        // A frozen track asks before its preset changes, and not for a press against the list's end.
        to_factory();
        for (int n = 0; n < 2; ++n) { tap(SurfaceButton::Plus); }
        CHECK(ui.preset_due && ui.presets_cursor == 4);
        rest();
        wait([&] { return whole() && playing() == all[4].index; });
        c.freeze_requested[0].state = FreezeState::Frozen;
        tap(SurfaceButton::Plus);
        CHECK(ui.ask == UiState::Ask::None && !ui.preset_due);
        tap(SurfaceButton::Minus);
        CHECK(ui.ask == UiState::Ask::Unfreeze && !ui.preset_due);
        tap(SurfaceButton::TopLeft); // B1: not now
        CHECK(ui.ask == UiState::Ask::None);
        c.freeze_requested[0] = {};
        CHECK(tr.octave == octave && tr.bank == bank);
        CHECK(stage_go(ui, EngineKind::Plugin, StageKind::Arp)); // off PRESETS the keys are the octave still
        tap(SurfaceButton::Plus);
        CHECK(tr.octave == octave + 1 && !ui.preset_due);
        shell->set_plugins(nullptr);
        return 0;
    }
    if (failsave) { // the rack keeps its good state when the plug-in's save fails (state_now), at rack level
        CHECK(rack->instrument(entry) == 1);
        set_control("Level", 750);
        wait([&] { return state()[0] == 0.75; });
        const uint32_t before = rack->preset_generation(entry);
        CHECK(before != 0);
        rack->reload(entry); // takes what it is now as the kept state, and reopens with it
        wait([&] {
            uint32_t gen = rack->preset_generation(entry);
            return gen != 0 && gen != before && !rack->programs(entry).finding && state()[0] == 0.75;
        });
        auto good = rack->state(entry);
        CHECK(good.size() == 4 + 3 * sizeof(double) && rack->states()[entry] == good);
        set_control("Level", 250); // the live plug-in moves on, and is read at 0.25: that is the last good state
        wait([&] { return state()[0] == 0.25; });
        const auto latest = rack->state(entry);
        CHECK(latest != good && latest.size() == good.size());
        setenv("MELODEE_TEST_FAIL_SAVES", "1", 1);
        CHECK(rack->state(entry) == latest); // a failed save gives the last good state, never an empty or part one
        CHECK(rack->states()[entry] == latest);
        unsetenv("MELODEE_TEST_FAIL_SAVES");
        shell->set_plugins(nullptr);
        return 0;
    }
    // A project saved with an edited plug-in, then saved again once the plug-in gives no state (a dead bridge, a save
    // that fails): the second save keeps the edits the first saved, not the state the plug-in was opened with.
    if (dead_state) {
        const auto opened = rack->state(entry);
        set_control("Wave", 1000);
        wait([&] { return state()[1] == 2; });
        const auto edited = rack->states()[entry];
        CHECK(edited != opened && !edited.empty());
        setenv("MELODEE_TEST_FAIL_SAVES", "1", 1);
        CHECK(rack->states()[entry] == edited && rack->state(entry) == edited);
        unsetenv("MELODEE_TEST_FAIL_SAVES");
        shell->set_plugins(nullptr);
        return 0;
    }
    if (lv2wake) { // a sleeping non-CLAP plug-in runs a block for a value the rack sets (note_change)
        CHECK(rack->instrument(entry) == 1);
        wait([&] { return rack->idling(entry); }); // silent, no key, stopped: it sleeps
        set_control("Level", 250);
        wait([&] { return !rack->idling(entry); }); // woken for the value, no note needed
        shell->set_plugins(nullptr);
        return 0;
    }
    if (dynamicRing) {
        CHECK(host->print_tail_seconds(0, true) == 0);
        const auto made = render_print(*host, 0, sixteenth_ticks, PrintTap::PostFx, PrintTail::Ring);
        CHECK(made.error.empty() && made.pass == 6000);
        CHECK(made.left.size() > 24960 && made.left[24960] > 0.7f); // the repeat at 520 ms, after the old quiet cutoff
        CHECK(host->print_tail_seconds(0, true) == 0.8);
        CHECK(made.left.size() == 24961 + 9600); // one pulse plus the usual 200 ms of quiet
        return 0;
    }
    if (mode == "ringtail") { // the real CLAP rack supplies the declared duration to the print host
        wait([&] { return host->print_tail_seconds(0, false) == 0.75; });
        CHECK(host->print_tail_seconds(0, true) == 0.75);
        return 0;
    }
    if (mono) { // a mono output through the rack: heard on both channels (dual mono), not the left alone
        CHECK(rack->instrument(entry) == 1);
        host->send({Command::Type::Play});
        int64_t peakL = 0, peakR = 0;
        for (int n = 0; n < 600; ++n) {
            tick();
            for (unsigned f = 0; f < EngineHost::block; ++f) {
                peakL = std::max(peakL, int64_t(std::llabs(int64_t(audio[2 * f]))));
                peakR = std::max(peakR, int64_t(std::llabs(int64_t(audio[2 * f + 1]))));
            }
        }
        CHECK(peakL > 1000000);                            // it sounded
        CHECK(peakR > peakL * 9 / 10 && peakL > peakR * 9 / 10); // as much right as left
        shell->set_plugins(nullptr);
        return 0;
    }
    if (fx) { // an effect's user presets: saved and loaded by the rack as an instrument's are
        CHECK(rack->instrument(entry) == 0);
        if (saving) {
            set_control("Gain", 250); // Gain 0.5 of 0..2
            wait([&] { return state()[2] == 0.5; });
            // Save a first preset from the selected effect, before PRESETS has any rows.
            CHECK(rack->programs(entry).list.empty());
            c.open(PageId::Master);
            chord();
            CHECK(ui.name.target == NameTarget::PluginPreset);
            tap(SurfaceButton::TopRight);
            wait([&] { return has_patch(entry); });
            // With a sampler track selected, the shortcut inside PRESETS must still name this effect.
            c.open_plugin_presets(entry);
            CHECK(ui.page == PageId::PluginPresets);
            chord();
            CHECK(ui.name.target == NameTarget::PluginPreset);
            tap(SurfaceButton::TopLeft);
        } else {
            CHECK(has_patch(entry));
            CHECK(state()[2] == 1.0);
            rack->load_user_preset(entry, "My Patch");
            wait([&] { return rack->status(entry) == PluginStatus::Ready && state()[2] == 0.5; });
        }
        shell->set_plugins(nullptr);
        return 0;
    }
    if (saving) {
        wait([&] { return rack->status(1) == PluginStatus::Ready && !rack->programs(1).finding; });
        set_control("Level", 750);
        set_control("Wave", 1000);
        wait([&] { return state()[0] == 0.75 && state()[1] == 2; });
        chord();
        CHECK(ui.name.target == NameTarget::PluginPreset);
        tap(SurfaceButton::TopLeft);
        CHECK(!has_patch(entry));
        chord();
        tap(SurfaceButton::TopRight);
        wait([&] { return has_patch(entry) && has_patch(1) && std::string(ref.preset) == "/user/My Patch"; });
        set_control("Level", 250);
        wait([&] { return state()[0] == 0.25; });
        chord();
        tap(SurfaceButton::TopRight);
        wait([&] { return std::string(ui.notice) == "Preset name already used"; });
    } else if (mode != "name-target" && mode != "samples-rows") { // these two: straight to the browser below
        CHECK(has_patch(entry));
        CHECK(state()[0] == 0.5);
        wait([&] { return ui.plugin_panel.entry == entry && ui.plugin_panel.programs; });
        CHECK(ui.page == PageId::Stage); // Edit opens the plug-in's controls on Stage
        button(SurfaceButton::Shift, true);
        tap(SurfaceButton::TopMiddle);
        button(SurfaceButton::Shift, false);
        CHECK(ui.page == PageId::PluginPresets);
        auto programs = rack->programs(entry);
        auto rows = preset_rows(programs.list, "", programs.current);
        auto row = std::find_if(rows.begin(), rows.end(), [](const PresetRow& r) { return r.folder && r.name == "User Presets"; });
        CHECK(row != rows.end());
        ui.presets_cursor = uint16_t(row - rows.begin());
        tap(SurfaceButton::WheelPush);
        CHECK(std::string(ui.presets_folder.data()) == "User Presets");
        tap(SurfaceButton::WheelPush);
        wait([&] { return rack->status(entry) == PluginStatus::Ready && state()[0] == 0.75 && state()[1] == 2; });
        CHECK(std::string(ref.preset) == "/user/My Patch");
        return 0;
    }

    // The exact same hardware events still reach the regular sample and bank naming sheets.
    auto samples = std::make_unique<SampleLibrary>(*host);
    shell->set_library(samples.get());
    load_sound(tr, EngineKind::Sampler, 0);
    tr.last_pad = 0;
    tr.pads[0].sample = 0;
    std::strcpy(project.samples.edit(0).path, "Kick.wav");
    std::strcpy(project.samples.edit(0).name, "Kick");
    c.open(PageId::Edit);
    fs::path root = fs::path(card) / "Melodee" / "Samples";
    fs::create_directories(root);
    setenv("MELODEE_SAMPLES", root.c_str(), 1);
    SampleData kick;
    adopt(kick, std::vector<float>(128, 0.25f), std::vector<float>(128, 0.5f), 44100);
    auto source = (root / "Kick.wav").string();
    CHECK(write_wav(source, kick).empty());
    std::snprintf(project.samples.edit(0).path, sizeof(SampleRef::path), "%s", source.c_str());
    HardwareMixer mixer;
    Platform platform;
    SystemServices services;
    ClockIo clock;
    CardShare share(services, card, card + "/mounts", card + "/gadget", card + "/saved");
    PowerButton power;
    JackDetect jacks;
    UsbAudioOutput usb;
    MidiPorts midi;
    Hardware hardware {mixer, platform, services, clock, share, power, jacks, usb, midi};
    samples->service(hardware);
    CHECK(samples->load(0, source, false));
    wait([&] { samples->service(hardware); return samples->loads_waiting() == 0; });
    pad_print_wavs(card, source, *samples);
    automated_limiter_wav(card, source, *samples);
    tr.pads[0].start = 32;
    tr.pads[0].end = 96;
    chord();
    CHECK(ui.name.target == NameTarget::Sample);
    ui.name.set("Slice");
    tap(SurfaceButton::TopRight);
    samples->service(hardware);
    SampleData saved;
    CHECK(read_wav((root / "Slice.wav").string(), saved).empty());
    CHECK(saved.frames == 64 && saved.left[32] == 0.25f && saved.right[32] == 0.5f);
    c.open(PageId::Samples);
    chord();
    CHECK(ui.name.target == NameTarget::Bank);
    ui.name.set("KIT");
    tap(SurfaceButton::TopRight);
    samples->service(hardware);
    CHECK(fs::file_size(root / "Banks" / "KIT.mldb") > 0);
    // The browser's rows: B1 out of a folder, then Push once the folder above is listed but before the browser has
    // taken it in (no tick between). Push acts on the row drawn under the cursor, never on the row of the folder above
    // that the cursor's place meant in the folder left.
    if (mode == "samples-rows") {
        fs::create_directories(root / "Sub");
        CHECK(write_wav((root / "Sub" / "Inner.wav").string(), kick).empty());
        auto settle = [&] { for (int k = 0; k < 40; ++k) { samples->service(hardware); c.browser_tick(); } };
        auto frame = std::make_unique<Snapshot>();
        c.select_track(0);
        c.open(PageId::Samples);
        samples->rescan();
        settle();
        shell->snapshot(*frame, ++now);
        CHECK(fs::path(frame->files_folder).filename() == "Samples");
        int sub = -1;
        for (size_t n = 0; n < frame->files.size(); ++n) if (fs::path(frame->files[n].path).filename() == "Sub") sub = int(n);
        CHECK(sub > 0); // not the first row
        ui.files_cursor = uint16_t(sub);
        tap(SurfaceButton::WheelPush);
        settle();
        shell->snapshot(*frame, ++now);
        CHECK(fs::path(frame->files_folder).filename() == "Sub" && frame->files.size() == 1 && ui.files_cursor == 0);
        CHECK(fs::path(frame->files[0].path).filename() == "Inner.wav");
        tap(SurfaceButton::TopLeft); // BACK: the folder above, listed on the service thread
        for (int k = 0; k < 40 && fs::path(samples->folder()).filename() != "Samples"; ++k) samples->service(hardware);
        CHECK(fs::path(samples->folder()).filename() == "Samples");
        shell->snapshot(*frame, ++now);
        CHECK(fs::path(frame->files_folder).filename() == "Sub"); // not taken in yet: Sub's rows still drawn
        tap(SurfaceButton::WheelPush); // on Inner.wav, the row drawn
        settle();
        CHECK(fs::path(project.samples[tr.pads[tr.last_pad % sampler_pads].sample].path).filename() == "Inner.wav");
        shell->snapshot(*frame, ++now); // and now the folder above, the cursor on the folder it came out of
        CHECK(fs::path(frame->files_folder).filename() == "Samples" && ui.files_cursor < frame->files.size());
        CHECK(fs::path(frame->files[ui.files_cursor].path).filename() == "Sub");
        // STARRED: a starred file's row drawn, B1, and Erase before the browser has taken the folder above in. Erase in a
        // list takes a file off it; here the list is no longer the library's, so it does nothing, and never asks to
        // delete the file from the card.
        CHECK(write_wav((root / "Sub" / "Other.wav").string(), kick).empty()); // in a folder of the card's, in no project
        samples->star((root / "Sub" / "Other.wav").string());
        settle();
        shell->snapshot(*frame, ++now);
        int starred = -1;
        for (size_t n = 0; n < frame->files.size(); ++n) if (frame->files[n].list && frame->files[n].name == "STARRED") starred = int(n);
        CHECK(starred >= 0);
        ui.files_cursor = uint16_t(starred);
        tap(SurfaceButton::WheelPush);
        settle();
        shell->snapshot(*frame, ++now);
        CHECK(frame->files.size() == 1 && fs::path(frame->files[0].path).filename() == "Other.wav" && ui.files_cursor == 0);
        tap(SurfaceButton::TopLeft);
        for (int k = 0; k < 40 && fs::path(samples->folder()).filename() != "Samples"; ++k) samples->service(hardware);
        CHECK(fs::path(samples->folder()).filename() == "Samples");
        ui.notice[0] = 0;
        tap(SurfaceButton::Erase);
        CHECK(ui.ask != UiState::Ask::DeleteFile && fs::exists(root / "Sub" / "Other.wav") && ui.notice[0] == 0);
    }
    // The Name sheet saves what it was opened for, though another track is in hand at Done (MIDI Remote picks one).
    if (mode == "name-target") {
        auto& other = project.tracks[1]; // a kit with the same sample, all of it
        load_sound(other, EngineKind::Sampler, 0);
        other.last_pad = 0;
        other.pads[0] = tr.pads[0];
        other.pads[0].start = 0;
        other.pads[0].end = 128;
        c.select_track(0);
        c.open(PageId::Edit);
        chord();
        CHECK(ui.name.target == NameTarget::Sample);
        ui.name.set("Mine");
        c.select_track(1);
        tap(SurfaceButton::TopRight);
        samples->service(hardware);
        SampleData mine;
        CHECK(read_wav((root / "Mine.wav").string(), mine).empty());
        CHECK(mine.frames == 64); // track 1's pad's region, not track 2's whole sample
        c.select_track(0);
        c.open(PageId::Samples);
        chord();
        CHECK(ui.name.target == NameTarget::Bank);
        ui.name.set("MINE");
        c.select_track(1);
        tap(SurfaceButton::TopRight);
        samples->service(hardware);
        std::ifstream in(root / "Banks" / "MINE.mldb", std::ios::binary);
        const std::vector<uint8_t> bank((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(bank == save_bank(bank_of(project, 0, tr.bank, "MINE")) && bank != save_bank(bank_of(project, 1, other.bank, "MINE")));
        setenv("MELODEE_PROJECTS", (fs::path(card) / "Melodee" / "Projects").c_str(), 1);
        setenv("MELODEE_INTERNAL_PROJECTS", (fs::path(card) / "internal").c_str(), 1);
        auto store = std::make_unique<ProjectStore>(); // Save sound needs somewhere to keep the sound
        shell->set_store(store.get());
        for (unsigned t : {2u, 3u}) load_sound(project.tracks[t], EngineKind::Midi, 0);
        c.select_track(2);
        c.open(PageId::Edit);
        chord();
        CHECK(ui.name.target == NameTarget::Sound);
        ui.name.set("LEAD");
        c.select_track(3);
        tap(SurfaceButton::TopRight);
        CHECK(std::string(project.tracks[2].name) == "LEAD" && std::string(project.tracks[3].name) != "LEAD");
        // Its header and accent say what it was opened for, whichever track or sequence is in hand when it is drawn: the
        // band over the knobs is drawn the same with another in hand as with its own.
        auto band = [&] {
            shell->snapshot(*drawn, now);
            juce::Image image(juce::Image::ARGB, int(ui::zone::width), int(ui::zone::height), true);
            {
                juce::Graphics g(image);
                ui::Paint paint(g);
                g.fillAll(ui::colour::bg);
                draw_name_sheet(paint, *drawn);
            }
            std::vector<uint32_t> pixels;
            for (int y = 0; y < int(ui::zone::knobs_y); ++y)
                for (int x = 0; x < int(ui::zone::width); ++x) pixels.push_back(image.getPixelAt(x, y).getARGB());
            return pixels;
        };
        auto same = [&](auto open, auto elsewhere, auto back) { // opened and drawn; another in hand, drawn again; cancelled
            back();
            open();
            ui.name.set("NAME");
            const auto own = band();
            elsewhere();
            const auto other = band();
            CHECK(ui.name.open());
            tap(SurfaceButton::TopLeft); // CANCEL
            CHECK(!ui.name.open() && own == other);
        };
        project.tracks[1].bank = 1; // another bank letter in hand
        same([&] { c.open(PageId::Edit); chord(); CHECK(ui.name.target == NameTarget::Sound); }, [&] { c.select_track(3); }, [&] { c.select_track(2); });
        same([&] { c.open(PageId::Samples); chord(); CHECK(ui.name.target == NameTarget::Bank); }, [&] { c.select_track(1); },
             [&] { c.select_track(0); });
        same([&] { c.open(PageId::Edit); chord(); CHECK(ui.name.target == NameTarget::Sample); }, [&] { c.select_track(1); },
             [&] { c.select_track(0); });
        same([&] { c.name_track(2); CHECK(ui.name.target == NameTarget::Track); }, [&] { c.select_track(0); }, [&] { c.select_track(2); });
        same([&] { c.name_scene(2); CHECK(ui.name.target == NameTarget::Scene); }, [&] { project.scene = 0; }, [&] { project.scene = 2; });
        project.scene = 0;
        shell->set_store(nullptr);
    }
    shell->set_library(nullptr);
    shell->set_plugins(nullptr);
}
