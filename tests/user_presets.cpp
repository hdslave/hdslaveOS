// SPDX-License-Identifier: AGPL-3.0-only
#include "melodee/engine/engine_host.h"
#include "melodee/plugin/plugin_rack.h"
#include "melodee/plugin/plugin_files.h" // clap_id_hash: the fixture's effect by name, for the fx modes
#include "melodee/plugin/preset_rows.h"
#include "melodee/shell/shell.h"
#include "runtime/controller.h"
#include <chrono>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <thread>

using namespace melodee;
namespace fs = std::filesystem;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)

// A separate invocation starts a fresh project and a new plug-in instance.
// The simulated SD card is the only thing shared by the two invocations.
int main(int argc, char** argv) {
    CHECK(argc == 4);
    const std::string card = argv[1], plugin = argv[2], mode = argv[3];
    bool saving = mode == "save" || mode == "fxsave";
    bool fx = mode == "fxsave" || mode == "fxload"; // the fixture's effect in a chain, not a track's synth
    // Rack-level modes beside the preset ones: a failed save keeping the rack's state, a sleeping LV2 woken by
    // a value set, and a mono plug-in heard through the rack (fixture env hooks in tests/clap_fixture.cpp).
    bool failsave = mode == "failsave", lv2wake = mode == "lv2wake", mono = mode == "mono";
    if (mono) setenv("MELODEE_TEST_MONO", "1", 1); // before the rack opens it: a mono main port
    const unsigned entry = fx ? 2 : mode == "load" ? 4 : 0;
    auto host = std::make_unique<EngineHost>();
    host->set_render_threads(0);
    auto rack = std::make_unique<PluginRack>(*host);
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
    if (mono) { // a held A from the pattern's first step, so the mono sine sounds once it plays
        Step step;
        step.notes[0] = {69, 100};
        step.length = 16;
        shell->patterns().at[0][0].set_step(0, step);
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
