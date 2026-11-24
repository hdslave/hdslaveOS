// SPDX-License-Identifier: AGPL-3.0-only
#include "melodee/engine/print_render.h"
#include "melodee/plugin/plugin_rack.h"
#include "melodee/plugin/plugin_files.h" // clap_id_hash: the fixture's effect by name, for the fx modes
#include "melodee/plugin/preset_rows.h"
#include "melodee/shell/shell.h"
#include "melodee/shell/print_engine.h"
#include "runtime/controller.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <stdexcept>
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
        CHECK(context.deep(0));
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
    std::snprintf(ref.path, sizeof(ref.path), "%s", plugin.c_str());
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
    wait([&] { return rack->status(entry) == PluginStatus::Ready && !rack->programs(entry).finding; });
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
        tap(SurfaceButton::TopMiddle);
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
