// SPDX-License-Identifier: AGPL-3.0-only
#include "melodee/engine/print_render.h"
#include "melodee/pages/pages.h"
#include "melodee/plugin/plugin_rack.h"
#include "melodee/plugin/plugin_files.h" // clap_id_hash: the fixture's effect by name, for the fx modes
#include "melodee/plugin/preset_rows.h"
#include "melodee/shell/features.h" // stage_skip: the page Stage shows
#include "melodee/shell/shell.h"
#include "melodee/shell/stage_map.h"
#include "melodee/shell/print_engine.h"
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
    bool failsave = mode == "failsave", lv2wake = mode == "lv2wake", mono = mode == "mono";
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
    if (mode == "stage-presets" || mode == "presets-track" || mode == "undo-state") {
        const auto folder = fs::path(plugin).parent_path() / "TestPresets";
        fs::create_directories(folder);
        std::ofstream(folder / "Bank.tsp") << "Low\t0.25\nMid\t0.5\nHigh\t0.75\nLoud\t1.5\tgain\n"; // and one of Test Gain's
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
    if (mono || dynamicRing) {
        Step step;
        step.notes[0] = {69, 100};
        step.length = 16;
        auto& pattern = shell->patterns().at[0][0];
        if (dynamicRing) pattern.length = sixteenth_ticks;
        pattern.set_step(0, step);
    }
    host->load(project, shell->patterns());
    host->set_audio_format(48000, 384);
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
    auto set_control = [&](const char* name, int value) {
        PluginPanel panel;
        rack->panel(entry, 0, panel);
        for (unsigned k = 0; k < panel.window.size(); ++k) {
            const auto& control = panel.window[k];
            if (control.used && std::strcmp(control.name, name) == 0) {
                rack->set_param(entry, panel.window_first + k, value);
                return;
            }
        }
        throw std::runtime_error(std::string("missing fixture control: ") + name);
    };
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
        set_control("Level", 250); // the live plug-in moves on; the kept state is the 0.75 one
        wait([&] { return state()[0] == 0.25; });
        setenv("MELODEE_TEST_FAIL_SAVES", "1", 1);
        CHECK(rack->state(entry) == good); // a failed save gives the kept state, never an empty or part one
        CHECK(rack->states()[entry] == good);
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
    } else {
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
    shell->set_library(nullptr);
    shell->set_plugins(nullptr);
}
