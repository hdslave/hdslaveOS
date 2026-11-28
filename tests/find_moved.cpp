// SPDX-License-Identifier: AGPL-3.0-only
// hdslaveOS: find moved files through the whole shell (src/melodee/shell/find_moved.h): projects opened from a card
// made in a temporary folder, the shell, the sample library, the project store and the plug-in rack driven pass by
// pass as the MIDI and service threads drive them, and the engine rendered. Modes (argv): session (the question and
// its answers, the sheets, the index kept, FIND MISSING, Load Kit, a project that cannot be read), always, off,
// plugin (with the CLAP fixture as argv[3]).
//   test-find-moved TOP MODE [PLUGIN]
#include "melodee/engine/engine_host.h"
#include "melodee/engine/engine_info.h"
#include "melodee/engine/sample_data.h"
#include "melodee/model/project_file.h"
#include "melodee/plugin/plugin_rack.h"
#include "melodee/shell/project_store.h"
#include "melodee/shell/sample_library.h"
#include "melodee/shell/shell.h"
#include "runtime/controller.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace melodee;
namespace fs = std::filesystem;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__ ":") + std::to_string(__LINE__) + ": " #x); } while (false)

namespace {
void wav(const std::string& path, float level = 0.25f) {
    fs::create_directories(fs::path(path).parent_path());
    SampleData d;
    adopt(d, std::vector<float>(2048, level), std::vector<float>(2048, level), 44100);
    CHECK(write_wav(path, d).empty());
}
void touch(const std::string& path) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream(path) << "x";
}
// A project of one kit, its pads 1.. on the slots in turn, and a plug-in track for each plug-in path.
void write_project(const std::string& path, const std::vector<std::string>& samples, const std::vector<std::string>& plugins = {}) {
    auto p = std::make_unique<Project>();
    auto b = std::make_unique<PatternBank>();
    load_sound(p->tracks[0], EngineKind::Sampler, 0);
    for (size_t n = 0; n < samples.size(); ++n) {
        auto& ref = p->samples.edit(n);
        std::snprintf(ref.path, sizeof(ref.path), "%s", samples[n].c_str());
        std::snprintf(ref.name, sizeof(ref.name), "%s", fs::path(samples[n]).stem().string().c_str());
        p->tracks[0].pads[n].sample = uint16_t(n);
    }
    for (size_t k = 0; k < plugins.size(); ++k) {
        std::snprintf(p->plugins[k].path, sizeof(p->plugins[k].path), "%s", plugins[k].c_str());
        std::snprintf(p->plugins[k].name, sizeof(p->plugins[k].name), "%s", fs::path(plugins[k]).stem().string().c_str());
        load_sound(p->tracks[1 + k], EngineKind::Plugin, 0);
        p->tracks[1 + k].plugin = uint8_t(k);
    }
    fs::create_directories(fs::path(path).parent_path());
    const auto bytes = save_project(*p, *b);
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

int run(const std::string& top, const std::string& mode, const std::string& plugin) {
    const std::string card = top + "/media/CARD", old = top + "/media/F2EC-CC50", projects = card + "/Melodee/Projects";
    fs::create_directories(projects);
    fs::create_directories(top + "/internal");
    setenv("MELODEE_SAMPLES", card.c_str(), 1);   // the card's top, as the library finds it
    setenv("MELODEE_PROJECTS", projects.c_str(), 1);
    setenv("MELODEE_INTERNAL", (top + "/internal").c_str(), 1);
    setenv("MELODEE_INTERNAL_PROJECTS", (top + "/internal-projects").c_str(), 1);
    HardwareMixer mixer;
    Platform platform;
    SystemServices services;
    ClockIo clock;
    CardShare share(services, card, card + "/mounts", card + "/gadget", card + "/saved");
    PowerButton power;
    JackDetect jacks;
    UsbAudioOutput usb;
    MidiPorts midi;
    Hardware hw {mixer, platform, services, clock, share, power, jacks, usb, midi};
    auto host = std::make_unique<EngineHost>();
    host->set_render_threads(0);
    auto rack = std::make_unique<PluginRack>(*host);
    auto library = std::make_unique<SampleLibrary>(*host);
    auto store = std::make_unique<ProjectStore>();
    auto shell = std::make_unique<Shell>(*host);
    shell->load_engine();
    shell->set_library(library.get());
    shell->set_store(store.get());
    rack->set_card(card);
    shell->set_plugins(rack.get());
    auto& c = shell->context();
    auto& ui = shell->ui();
    auto& moved = shell->moved_files();
    SurfaceLights lights;
    std::vector<int32_t> audio(EngineHost::block * 2);
    uint32_t now = 1000;
    auto pass = [&] {
        library->service(hw);
        store->service(hw);
        shell->lights(lights, now += 5);
        host->render(audio.data(), EngineHost::block, 0.7f);
    };
    auto until = [&](const char* what, std::function<bool()> done, int seconds = 30) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (!done()) {
            if (std::chrono::steady_clock::now() > end)
                throw std::runtime_error(std::string("not within the time: ") + what + " (ask " + std::to_string(int(ui.ask)) + ", missing "
                                         + std::to_string(moved.missing_count()) + ", idle " + std::to_string(moved.idle()) + ", notice '"
                                         + ui.notice + "', project '" + c.project_path + "', loading " + std::to_string(ui.loading) + ")");
            pass();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    auto button = [&](SurfaceButton b, bool down) {
        SurfaceEvent e;
        e.input.kind = SurfaceInput::Kind::Button;
        e.input.index = uint8_t(b);
        e.input.pressed = down;
        shell->surface(e, now += 5);
        pass();
    };
    auto press = [&](SurfaceButton b, bool shift = false) {
        if (shift) button(SurfaceButton::Shift, true);
        button(b, true);
        button(b, false);
        if (shift) button(SurfaceButton::Shift, false);
    };
    auto play_stop = [&] { // Play and Stop answer under any sheet
        const auto asked = ui.ask;
        press(SurfaceButton::Play);
        until("playing", [&] { return host->playing(); }, 5);
        press(SurfaceButton::Stop);
        until("stopped", [&] { return !host->playing(); }, 5);
        CHECK(ui.ask == asked);
    };
    auto path = [&](unsigned n) { return std::string(c.project.samples[n].path); };
    auto ready = [&](unsigned n) { return library->slot(n).ready; };
    auto open = [&](const std::string& file) {
        c.project_path.clear(); // so a project opened again is told apart
        store->load(file);
        until("opened", [&] { return c.project_path == file && !ui.loading && c.samples_to_load >= sample_slots && library->loads_waiting() == 0; });
        until("looked for", [&] { return moved.idle() && library->loads_waiting() == 0; });
    };
    auto notice = [&] { return std::string(ui.notice); };
    until("store and card", [&] { ProjectStore::View v; store->view(v); return v.settings_read && library->card() == card; });

    if (mode == "always" || mode == "off") {
        ui.settings.find_moved = mode == "always" ? Settings::FindAlways : Settings::FindOff;
        wav(card + "/Deep/M.wav");
        write_project(projects + "/P.mldp", {card + "/Old/M.wav", card + "/Gone/G.wav"});
        c.project_path.clear();
        store->load(projects + "/P.mldp");
        until("opened", [&] { return c.project_path == projects + "/P.mldp" && !ui.loading; });
        if (mode == "always") { // no question: the search sheet at once, and its result
            until("the search sheet", [&] { return ui.ask == UiState::Ask::FindSearching || ui.ask == UiState::Ask::FindResult; });
            until("the result", [&] { return ui.ask == UiState::Ask::FindResult; });
            CHECK(moved.files().walks() == 1 && path(0) == card + "/Deep/M.wav" && ui.ask_target == 1);
        } else { // no question, no walk, and the notice
            until("looked for", [&] { return moved.idle(); });
            for (int k = 0; k < 50; ++k) pass();
            CHECK(ui.ask == UiState::Ask::None && moved.files().walks() == 0 && notice() == "2 samples missing" && path(0) == card + "/Old/M.wav");
        }
        std::puts("find moved ok");
        return 0;
    }

    if (mode == "plugin") { // the hdslave plug-ins moved into Melodee/Plugins/hdslave/, and one gone
        CHECK(fs::exists(plugin));
        const std::string moved_to = card + "/Melodee/Plugins/hdslave/Test.clap", saved = card + "/Melodee/Plugins/Test.clap";
        fs::create_directories(fs::path(moved_to).parent_path());
        fs::copy_file(plugin, moved_to, fs::copy_options::overwrite_existing);
        write_project(projects + "/Synth.mldp", {}, {saved, card + "/Melodee/Plugins/Gone.clap"});
        open(projects + "/Synth.mldp");
        until("the plug-in open", [&] { return rack->status(0) == PluginStatus::Ready; }, 60);
        until("the other's notice", [&] { return notice() == "Plug-in missing: Gone"; }, 60);
        until("its path", [&] { return std::string(c.project.plugins[0].path) == moved_to; });
        CHECK(rack->status(1) == PluginStatus::Failed && !c.dirty); // opening writes nothing
        for (int k = 0; k < 100; ++k) pass(); // and nothing opens again once the project names where it is
        CHECK(rack->status(0) == PluginStatus::Ready);
        // Opened again (nothing saved: the file names the old path): found and followed again, the notice again.
        ui.notice[0] = 0;
        open(projects + "/Synth.mldp");
        until("its path again", [&] { return std::string(c.project.plugins[0].path) == moved_to && rack->status(0) == PluginStatus::Ready; }, 60);
        until("the other's notice again", [&] { return notice() == "Plug-in missing: Gone"; }, 60);
        shell->set_plugins(nullptr);
        std::puts("find moved ok");
        return 0;
    }

    CHECK(mode == "session");
    wav(card + "/Kit/A.wav");
    wav(card + "/Kit/B.wav");
    // Load Kit into the project Melodee starts with, before any project is opened: its slots looked for too.
    write_project(projects + "/Boot kit.mldp", {old + "/Kit/A.wav"});
    c.select_track(0);
    c.load_kit_file(projects + "/Boot kit.mldp", "Boot kit");
    until("the kit", [&] { return c.kit_from.empty() && moved.idle(); });
    {
        int kit = -1;
        for (unsigned n = 0; n < 8; ++n) if (path(n) == card + "/Kit/A.wav") kit = int(n);
        CHECK(kit >= 0);
        until("loaded", [&] { return ready(unsigned(kit)); });
    }
    // Every file in place: no path changes, no question, no notice, no walk.
    write_project(projects + "/P1.mldp", {card + "/Kit/A.wav", card + "/Kit/B.wav"});
    open(projects + "/P1.mldp");
    CHECK(path(0) == card + "/Kit/A.wav" && path(1) == card + "/Kit/B.wav" && ready(0) && ready(1));
    CHECK(ui.ask == UiState::Ask::None && ui.find.missing == 0 && notice().empty() && !c.dirty && moved.files().walks() == 0);
    const unsigned looked_for = moved.files().looked_for(); // the boot kit's one
    CHECK(looked_for == 1); // every file of P1 in place: none looked for
    // A renamed card: found at once, loaded, nothing asked, nothing written.
    write_project(projects + "/P2.mldp", {old + "/Kit/A.wav", old + "/Kit/B.wav"});
    open(projects + "/P2.mldp");
    until("loaded", [&] { return ready(0) && ready(1); });
    CHECK(path(0) == card + "/Kit/A.wav" && path(1) == card + "/Kit/B.wav" && ui.ask == UiState::Ask::None && !c.dirty && moved.files().walks() == 0);
    // Deleted samples: its other track plays at once, and the question opens with no search.
    write_project(projects + "/P3.mldp", {card + "/Kit/A.wav", card + "/Gone/X.wav", card + "/Gone/Y.wav"});
    open(projects + "/P3.mldp");
    CHECK(ready(0));
    until("asked", [&] { return ui.ask == UiState::Ask::FindMissing; });
    CHECK(ui.ask_target == 2 && ui.find.missing == 2 && moved.files().walks() == 0);
    play_stop();
    press(SurfaceButton::TopLeft); // NOT NOW
    for (int k = 0; k < 50; ++k) pass();
    CHECK(ui.ask == UiState::Ask::None && ui.find.missing == 2 && moved.files().walks() == 0 && path(1) == card + "/Gone/X.wav");
    open(projects + "/P3.mldp"); // asked again
    until("asked again", [&] { return ui.ask == UiState::Ask::FindMissing; });
    press(SurfaceButton::TopMiddle); // FORGET: only the missing samples go (off their pads), the project changed, one undo
    const auto& pads = c.project.tracks[0].pads;
    CHECK(ui.ask == UiState::Ask::None && moved.files().walks() == 0 && c.dirty && moved.missing_count() == 0);
    CHECK(pads[0].sample == 0 && !pads[1].used() && !pads[2].used() && ready(0));
    c.undo();
    CHECK(path(1) == card + "/Gone/X.wav" && path(2) == card + "/Gone/Y.wav" && pads[1].sample == 1 && pads[2].sample == 2);
    until("not found again", [&] { return moved.idle() && moved.missing_count() == 2; });
    CHECK(ui.ask == UiState::Ask::None); // nothing asked
    c.redo();
    CHECK(!pads[1].used() && !pads[2].used() && moved.missing_count() == 0);
    const unsigned saves = store->saves();
    c.save_project();
    until("saved", [&] { return store->saves() > saves; });
    open(projects + "/P3.mldp"); // asks nothing
    for (int k = 0; k < 50; ++k) pass();
    CHECK(ui.ask == UiState::Ask::None && ui.find.missing == 0 && path(0) == card + "/Kit/A.wav" && !pads[1].used() && !pads[2].used());
    // FORGET undone, the file copied back and found (FIND MISSING), FORGET done again: the pad keeps the sample found.
    write_project(projects + "/P3b.mldp", {card + "/Kit/A.wav", card + "/Gone/Q.wav"});
    open(projects + "/P3b.mldp");
    until("asked", [&] { return ui.ask == UiState::Ask::FindMissing; });
    press(SurfaceButton::TopMiddle); // FORGET
    CHECK(!pads[1].used());
    c.undo();
    until("not found again", [&] { return moved.idle() && moved.missing_count() == 1; });
    wav(card + "/Gone/Q.wav");
    c.open(PageId::Samples);
    press(SurfaceButton::TopLeft, true); // FIND MISSING
    until("found", [&] { return moved.idle() && ui.ask == UiState::Ask::None && moved.missing_count() == 0; });
    c.redo();
    CHECK(pads[1].sample == 1 && path(1) == card + "/Gone/Q.wav");
    until("loaded", [&] { return ready(1); });
    const unsigned walks_now = moved.files().walks();
    // SEARCH, then CANCEL within a folder: what the quick checks found stays; the next SEARCH walks from the start.
    for (int d = 0; d < 80; ++d) touch(card + "/Noise/D" + std::to_string(d) + "/n.txt");
    wav(card + "/Deep/Down/M1.wav");
    write_project(projects + "/P4.mldp", {old + "/Kit/A.wav", card + "/Old/M1.wav", card + "/Old/Z.wav"});
    moved.files().folder_pause_us = 20000; // a slow card
    moved.files().drop_index(); // the files were added after FIND MISSING's walk (as the web page or SD card access would)
    open(projects + "/P4.mldp");
    until("asked", [&] { return ui.ask == UiState::Ask::FindMissing; });
    CHECK(ui.ask_target == 2 && path(0) == card + "/Kit/A.wav");
    press(SurfaceButton::TopRight); // SEARCH
    CHECK(ui.ask == UiState::Ask::FindSearching && ui.find.asked == 2);
    until("walking", [&] { return moved.files().progress().walking; });
    play_stop();
    until("one found by the part walked", [&] { return moved.files().progress().matched >= 1; }); // Deep/ comes first
    CHECK(moved.files().progress().walking);
    const auto cancelled_at = std::chrono::steady_clock::now();
    press(SurfaceButton::TopLeft); // CANCEL: the sheet goes at once
    CHECK(ui.ask == UiState::Ask::None);
    until("stopped", [&] { return moved.idle(); }, 5);
    CHECK(std::chrono::steady_clock::now() - cancelled_at < std::chrono::seconds(1)); // within a folder (20 ms) or so
    CHECK(notice() == "Search stopped, found 1" && moved.files().walks() == walks_now + 1 && !moved.files().indexed());
    CHECK(path(0) == card + "/Kit/A.wav" && ready(0) && path(1) == card + "/Deep/Down/M1.wav" && ui.find.missing == 1); // what it found stays
    moved.files().folder_pause_us = 0;
    open(projects + "/P4.mldp");
    until("asked", [&] { return ui.ask == UiState::Ask::FindMissing; });
    press(SurfaceButton::TopRight); // SEARCH again
    until("the result", [&] { return ui.ask == UiState::Ask::FindResult; });
    CHECK(moved.files().walks() == walks_now + 2 && moved.files().indexed() && path(1) == card + "/Deep/Down/M1.wav");
    CHECK(ui.ask_target == 1 && ui.find.found == 1 && ui.find.asked == 2);
    until("loaded", [&] { return ready(1); });
    play_stop();
    press(SurfaceButton::TopLeft); // KEEP
    CHECK(ui.ask == UiState::Ask::None && ui.find.missing == 1);
    // HIDE: the search goes on, its result a notice. (The index dropped first, as a file uploaded from the web page does.)
    moved.files().drop_index();
    wav(card + "/Deep/Other/M5.wav");
    write_project(projects + "/P5.mldp", {card + "/Old/M5.wav"});
    moved.files().folder_pause_us = 5000;
    open(projects + "/P5.mldp");
    until("asked", [&] { return ui.ask == UiState::Ask::FindMissing; });
    press(SurfaceButton::TopRight);
    until("walking", [&] { return moved.files().progress().walking; });
    press(SurfaceButton::TopRight); // HIDE
    CHECK(ui.ask == UiState::Ask::None && ui.find.searching && ui.find.hidden && moved.searching());
    moved.files().folder_pause_us = 0;
    until("found", [&] { return moved.idle(); });
    CHECK(notice() == "Found 1 sample" && path(0) == card + "/Deep/Other/M5.wav" && moved.files().walks() == walks_now + 3 && ui.ask == UiState::Ask::None);
    // A second project after a search: answered from the index, no question, no walk.
    write_project(projects + "/P6.mldp", {card + "/Old/M1.wav", card + "/Gone/Back.wav"});
    open(projects + "/P6.mldp");
    for (int k = 0; k < 50; ++k) pass();
    CHECK(ui.ask == UiState::Ask::None && moved.files().walks() == walks_now + 3 && path(0) == card + "/Deep/Down/M1.wav" && moved.missing_count() == 1);
    CHECK(notice() == "1 sample not found");
    // FIND MISSING (the Samples page, Shift + B1) finds a file copied back: the card walked anew.
    wav(card + "/Gone/Back.wav");
    c.open(PageId::Samples);
    press(SurfaceButton::TopLeft, true);
    until("found", [&] { return moved.idle() && ui.ask == UiState::Ask::None; });
    CHECK(moved.files().walks() == walks_now + 4 && path(1) == card + "/Gone/Back.wav" && moved.missing_count() == 0 && notice() == "Found 1 sample");
    until("loaded", [&] { return ready(1); });
    // Another project opened mid-search: nothing of the old one's lands in it.
    moved.files().drop_index();
    wav(card + "/Deep/Stale/S7.wav");
    write_project(projects + "/P7.mldp", {card + "/Old/S7.wav"});
    write_project(projects + "/P8.mldp", {card + "/Old/S7.wav"}); // the same path: only the project tells them apart
    moved.files().folder_pause_us = 20000;
    open(projects + "/P7.mldp");
    until("asked", [&] { return ui.ask == UiState::Ask::FindMissing; });
    press(SurfaceButton::TopRight);
    until("walking", [&] { return moved.files().progress().walking; });
    open(projects + "/P8.mldp");
    moved.files().folder_pause_us = 0;
    until("the walk done", [&] { return moved.idle() && !moved.files().progress().walking; });
    for (int k = 0; k < 50; ++k) pass();
    CHECK(path(0) == card + "/Old/S7.wav" && ui.find.missing == 1 && ui.ask == UiState::Ask::FindMissing); // P8's own question
    CHECK(notice().rfind("Found", 0) != 0 && moved.files().indexed()); // the walk went on, for the index
    press(SurfaceButton::TopLeft); // NOT NOW
    // SD card access lent mid-search: the search stops (nothing reads the card while it is lent).
    moved.files().drop_index();
    moved.files().folder_pause_us = 20000;
    open(projects + "/P7.mldp");
    until("asked", [&] { return ui.ask == UiState::Ask::FindMissing; });
    press(SurfaceButton::TopRight);
    until("walking", [&] { return moved.files().progress().walking; });
    shell->card().state.store(2); // lent
    until("stopped", [&] { return !moved.searching() && !moved.files().progress().walking && moved.idle(); }, 5);
    CHECK(!moved.files().indexed() && ui.ask == UiState::Ask::None && ui.page == PageId::CardAccess);
    shell->card().state.store(0);
    shell->card().returns.fetch_add(1);
    moved.files().folder_pause_us = 0;
    for (int k = 0; k < 20; ++k) pass();
    open(projects + "/P8.mldp");
    // Load Kit: the kit's slots through the same check (a renamed card), beside the kit's own file.
    if (ui.ask != UiState::Ask::None) press(SurfaceButton::TopLeft);
    write_project(projects + "/Kit.mldp", {old + "/Kit/B.wav"});
    c.select_track(0);
    c.load_kit_file(projects + "/Kit.mldp", "Kit");
    until("the kit", [&] { return c.kit_from.empty() && moved.idle(); });
    int kit = -1;
    for (unsigned n = 0; n < 8; ++n) if (path(n) == card + "/Kit/B.wav") kit = int(n);
    CHECK(kit > 0);
    until("loaded", [&] { return ready(unsigned(kit)); });
    // A project file that cannot be read: said.
    touch(projects + "/Bad.mldp");
    std::ofstream(projects + "/Bad.mldp", std::ios::trunc).close();
    store->load(projects + "/Bad.mldp");
    until("the notice", [&] { return notice() == "Could not open Bad"; });
    CHECK(path(0) == card + "/Old/S7.wav" && c.project_path == projects + "/P8.mldp"); // the project open stays
    shell->set_plugins(nullptr);
    std::puts("find moved ok");
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: test-find-moved TOP session|always|off|plugin [PLUGIN]\n"); return 2; }
    try {
        return run(argv[1], argv[2], argc > 3 ? argv[3] : "");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAILED %s\n", e.what());
        std::fflush(stderr);
        std::_Exit(1);
    }
}
