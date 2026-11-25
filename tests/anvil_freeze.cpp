// SPDX-License-Identifier: AGPL-3.0-only
#include "melodee/engine/print_render.h"
#include "melodee/plugin/plugin_rack.h"
#include "melodee/shell/print_engine.h"
#include "melodee/shell/shell.h"
#include "melodee/shell/stage_map.h"
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

static void anvil_freeze(const std::string& card, const std::string& plugin, const std::string& mode, unsigned rate, int run_index) {
    CHECK(rate == 44100 || rate == 48000);
    const bool with_note = mode == "anvil-freeze-note";
    const bool ring_launch = mode == "anvil-freeze-ring-launch";
    const bool queued_run = mode == "anvil-freeze-queued-run";
    const bool run_stops = mode == "anvil-freeze-run-stops";
    const bool manual_bank = mode == "anvil-freeze-panel-bank-manual" || mode == "anvil-freeze-panel-handoff-bank";
    const bool manual_picks = mode == "anvil-freeze-panel-handoff-picks";
    const bool handoff = manual_picks || mode == "anvil-freeze-panel-handoff" || mode == "anvil-freeze-panel-handoff-bank";
    const bool queued_bank = handoff || manual_bank || mode == "anvil-freeze-panel-bank";
    const bool panel_pending = queued_bank || mode == "anvil-freeze-panel-pending";
    const bool panel_run = panel_pending || mode == "anvil-freeze-panel-run";
    struct Audio {
        std::vector<float> left, right;
        std::vector<int32_t> main;
    };
    SampleData ring_wav;
    constexpr unsigned tap_after_unfreeze = 4;
    constexpr unsigned block = EngineHost::block;
    const unsigned ring_blocks = rate / (block * 4);
    // Integer frames per tick and a bar divisible by the block size make the WAV comparison exact.
    const unsigned tempo = rate == 44100 ? 875 : 1250, bar_blocks = rate == 44100 ? 945 : 720;
    dsp::device_rate.store(rate);
    auto play = [&](bool freeze) {
        auto host = std::make_unique<EngineHost>();
        host->set_render_threads(0);
        auto rack = std::make_unique<PluginRack>(*host, freeze ? "frozen-" : "live-");
        rack->set_card(card);
        auto printer = std::make_unique<PrintEngine>(nullptr);
        printer->set_pager(&host->streams());
        auto shell = std::make_unique<Shell>(*host);
        shell->set_printer(printer.get());
        shell->set_plugins(rack.get());
        auto& c = shell->context();
        c.project_path = (fs::path(card) / (with_note ? "anvil-note.mldp" : "anvil-empty.mldp")).string();
        c.now = 1000;
        c.project.tempo = tempo;
        c.project.signature = common_time;
        auto& track = c.project.tracks[0];
        load_sound(track, EngineKind::Plugin, 0);
        track.plugin = 0;
        std::snprintf(c.project.plugins[0].path, sizeof(PluginRef::path), "%s", plugin.c_str());
        track.fx.slots[0] = {uint8_t(EffectKind::Echo), false, {60, 0, 35, 60, 9, 0}};
        const unsigned printed_pattern = handoff && !manual_picks ? pattern_pads : 0;
        auto& pattern = c.patterns.at[0][printed_pattern];
        pattern.length = c.project.bar_ticks();
        if (with_note) {
            Step hit;
            hit.notes[0] = {38, 85};
            hit.length = 5;
            pattern.set_step(3, hit);
        }
        if (run_stops) {
            Step hit;
            hit.notes[0] = {38, 85};
            hit.length = 5;
            c.patterns.at[0][1].set_step(3, hit);
        }
        const Pattern original = pattern;
        host->load(c.project, c.patterns);
        host->set_audio_format(rate, block);
        host->set_print_tap(0, true);
        std::array<int32_t, block * 2> audio {};
        auto render = [&] {
            while (host->streams().pump()) {}
            host->render(audio.data(), block, 0.7f);
        };
        auto tick = [&] {
            c.plugins_tick(++c.now);
            c.print_tick();
        };
        auto press = [&](SurfaceButton button, bool pressed) {
            SurfaceEvent event;
            event.input.kind = SurfaceInput::Kind::Button;
            event.input.index = uint8_t(button);
            event.input.pressed = pressed;
            shell->surface(event, c.now + 1);
        };
        auto tap = [&](SurfaceButton button) { press(button, true); press(button, false); };
        auto spin = [&](auto done) {
            for (unsigned n = 0; n < 3000 && !done(); ++n) {
                tick();
                render();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            CHECK(done());
        };
        spin([&] { return rack->status(0) == PluginStatus::Ready; });
        for (unsigned n = 0; n < 32; ++n) { tick(); render(); }
        const auto patch = rack->state(0);
        const uint32_t generation = rack->live_generation(0);
        if (queued_run || panel_run) {
            rack->set_param_index(0, generation, run_index, 0);
            render();
            CHECK(!rack->anvil_sequencer_running(0));
        }
        c.open(PageId::Stage);
        if (freeze && !with_note && !run_stops) {
            c.project.plugins[0].transport = false;
            tap(SurfaceButton::SampleRecord);
            CHECK(!shell->ui().print_sheet.picked(0));
            tap(SurfaceButton::TopRight);
            CHECK(!shell->ui().print_sheet.printing);
            tap(SurfaceButton::TopLeft);
            c.project.plugins[0].transport = true;
        }

        if (panel_run) {
            stage_go(shell->ui(), EngineKind::Plugin, StageKind::PluginScreen);
            c.open(PageId::Stage);
            spin([&] { return bool(rack->screen_picture()); });
            if (run_index == 33) { // staged Anvil: SEQ tab, RUN row, open the editor
                for (unsigned i = 0; i < 6; ++i) rack->screen_input(0, {ScreenKey::Step, 1, false});
                rack->screen_input(0, {ScreenKey::Wheel, 8, false});
                rack->screen_input(0, {ScreenKey::Push, 0, false});
            }
        }

        unsigned blocks = 0;
        auto advance = [&](unsigned until, Audio* capture = nullptr) {
            for (; blocks < until; ++blocks) {
                tick();
                render();
                if (!capture) continue;
                capture->left.insert(capture->left.end(), host->print_left(), host->print_left() + block);
                capture->right.insert(capture->right.end(), host->print_right(), host->print_right() + block);
                capture->main.insert(capture->main.end(), audio.begin(), audio.end());
            }
        };
        tap(SurfaceButton::Play);
        const unsigned requested = bar_blocks / 2;
        advance(requested);
        if (queued_run)
            rack->set_param_index(0, generation, run_index, 1000);
        if (run_stops) {
            tap(SurfaceButton::SampleRecord);
            CHECK(shell->ui().print_sheet.picked(0) && shell->ui().print_sheet.picked(1));
            tap(SurfaceButton::TopRight);
            CHECK(shell->ui().print_sheet.printing && c.anvil_freeze_requested[0].frozen());
            c.set_recording(true);
            c.key(38, 95, true);
            c.key(38, 0, false);
            c.record_motion(motion_wheel, 30);
            c.set_recording(false);
            CHECK(pattern == original);
            rack->set_param_index(0, generation, run_index, 0);
            render();
            CHECK(!rack->anvil_sequencer_running(0));
            c.print_tick();
            CHECK(!printer->busy() && !c.anvil_freeze_requested[0].frozen() && !c.frozen(0));
            CHECK(!c.prints.find(0, 0) && !c.prints.find(0, 1));
            CHECK(std::string(shell->ui().notice) == "Cannot freeze: empty pattern");
            return std::make_pair(Audio {}, Audio {});
        }
        if (queued_bank) {
            c.launch(0, pattern_pads); // B1 is queued while A1 still plays.
            advance(requested + 1);
            CHECK(host->pattern_of(0) == 0 && track.pattern == pattern_pads);
        }
        if (panel_run) {
            if (freeze && panel_pending) {
                // Open PRINT before the preceding panel gesture reaches its worker.
                tap(SurfaceButton::SampleRecord);
                CHECK(shell->ui().page == PageId::Print && !shell->ui().print_sheet.picked(0));
                if (queued_bank) CHECK(shell->ui().pad_pattern(0) == pattern_pads);
                if (manual_bank) tap(SurfaceButton::Plus); // Browse bank C before RUN arrives.
            }
            const auto input = run_index == 33 ? ScreenInput {ScreenKey::Wheel, 1, false}
                                               : ScreenInput {ScreenKey::Push, 0, false};
            rack->screen_input(0, input);
            if (panel_pending) c.plugins_tick(++c.now);
            // Confirm the native panel applied the edit before returning to the shell, without
            // advancing the audio thread or waiting for the rack to refresh its parameter cards.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (rack->state(0) != patch && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            CHECK(rack->state(0) == patch);
        }
        if (handoff) {
            if (freeze) {
                c.print_tick(); // RUN landed while A1 was still playing.
                CHECK(shell->ui().print_sheet.picked(0) && !shell->ui().print_sheet.picked(pattern_pads));
                if (manual_picks) {
                    c.print_sheet_pad(0);
                    c.print_sheet_pad(0); // Explicitly keep A1 selected.
                }
            }
            while (host->pattern_of(0) == 0 && blocks < 2 * bar_blocks) advance(blocks + 1);
            CHECK(host->pattern_of(0) == pattern_pads);
        }
        if (freeze) {
            if (!panel_pending) tap(SurfaceButton::SampleRecord);
            const auto& sheet = shell->ui().print_sheet;
            if (!(panel_pending && rate == 48000)) {
                // At 44.1 kHz exercise the periodic sheet refresh. At 48 kHz press B3 before
                // any print tick can refresh the already open sheet.
                if (panel_pending) c.print_tick();
                CHECK(shell->ui().page == PageId::Print && sheet.picked(printed_pattern) && !sheet.picked(1));
                if (queued_bank) CHECK(shell->ui().pad_pattern(0) == (manual_bank ? 2 * pattern_pads : printed_pattern));
            }
            if (ring_launch) shell->ui().print_sheet.tail = PrintTail::Ring;
            tap(SurfaceButton::TopRight);
            CHECK(sheet.printing && sheet.picked(printed_pattern) && !sheet.picked(1));
            if (handoff) CHECK(!sheet.picked(printed_pattern == 0 ? pattern_pads : 0));
            if (queued_bank) CHECK(shell->ui().pad_pattern(0) == (manual_bank ? 2 * pattern_pads : printed_pattern));
            c.print_tick();
            if (queued_run) {
                CHECK(c.anvil_freeze_requested[0].frozen() && !printer->busy());
                advance(requested + 1);
                c.print_tick();
            }
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (printer->busy() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            CHECK(!printer->busy());
        }
        if (queued_run) {
            advance(requested + 1);
            CHECK(rack->anvil_sequencer_running(0) && rack->state(0) == patch);
        }
        if (queued_bank && printed_pattern == 0)
            c.launch(0, 0); // Keep or return to A1 for the printed and live comparison bars.
        // A request made after the launch reaches the next bar later than the other cases.
        const unsigned printed_bar = handoff ? 3 : 2;
        advance(printed_bar * bar_blocks);
        std::vector<uint8_t> saved;
        Audio launch;
        if (freeze) {
            CHECK(c.frozen(0) && !c.deep(0) && (host->printing_tracks() & 1));
            CHECK(!(host->sleeping_tracks() & 1));
            const auto* record = c.prints.find(0, printed_pattern);
            CHECK(record && record->tap == PrintTap::PostFx &&
                  record->tail == (ring_launch ? PrintTail::Ring : PrintTail::Loop));
            SampleData wav;
            CHECK(read_wav((fs::path(prints_folder(c.project_path)) / record->file).string(), wav).empty());
            CHECK(std::any_of(wav.left.begin(), wav.left.end(), [](float f) { return std::fabs(f) > 0.001f; }));
            if (ring_launch) {
                CHECK(wav.frames >= (bar_blocks + ring_blocks) * block);
                ring_wav = std::move(wav);
            } else {
                CHECK(wav.frames == bar_blocks * block);
                Audio printed;
                advance((printed_bar + 1) * bar_blocks, &printed);
                CHECK(printed.left == wav.left && printed.right == wav.right);
                if (!with_note && !queued_run) {
                    auto states = rack->states();
                    saved = save_project(c.project, c.patterns, &states, &c.prints);
                }
            }
        }
        const unsigned unfreeze = (printed_bar + 1) * bar_blocks + (ring_launch ? ring_blocks : 0);
        if (ring_launch) {
            advance(2 * bar_blocks + bar_blocks / 2);
            c.launch(0, 1);
            advance(3 * bar_blocks);
            advance(unfreeze, &launch);
            CHECK(host->pattern_of(0) == 1 && !(host->printing_tracks() & 1));
        }
        if (freeze) {
            tap(SurfaceButton::SampleRecord);
            press(SurfaceButton::Shift, true);
            tap(SurfaceButton::TopLeft);
            press(SurfaceButton::Shift, false);
        }
        advance(unfreeze + tap_after_unfreeze); // past the five-millisecond handoff
        CHECK(!c.frozen(0) && !(host->printing_tracks() & 1));
        Audio live;
        advance(5 * bar_blocks, &live);
        CHECK(rack->state(0) == patch && rack->live_generation(0) == generation);
        CHECK(pattern == original);
        CHECK(host->print_waits() == 0 && host->print_underruns() == 0);
        if (!saved.empty()) {
            auto project = std::make_unique<Project>();
            auto bank = std::make_unique<PatternBank>();
            PrintTable prints;
            PluginStates states;
            CHECK(load_project(saved, *project, *bank, nullptr, &states, &prints).empty());
            CHECK(bank->at[0][printed_pattern].empty() && prints.freeze[0].plays(printed_pattern));
            c.stop();
            render();
            c.switch_to(*project, *bank, c.project_path, &states, &prints);
            spin([&] { return rack->status(0) == PluginStatus::Ready && (host->frozen_tracks() & 1); });
            c.open_print_sheet();
            CHECK(shell->ui().print_sheet.picked(printed_pattern));
            c.play();
            tick();
            render();
            CHECK(host->printing_tracks() & 1);
        }
        return std::make_pair(std::move(launch), std::move(live));
    };
    if (run_stops) { play(true); return; }
    const auto [live_launch, uninterrupted] = play(false);
    const auto [frozen_launch, resumed] = play(true);
    if (ring_launch) {
        const auto start = bar_blocks * block;
        CHECK(frozen_launch.left.size() == size_t(ring_blocks) * block);
        bool heard_live = false, heard_tail = false;
        for (size_t n = 0; n < frozen_launch.left.size(); ++n) {
            heard_live |= std::fabs(live_launch.left[n]) > 0.001f;
            heard_tail |= std::fabs(ring_wav.left[start + n]) > 0.001f;
            CHECK(frozen_launch.left[n] == live_launch.left[n] + ring_wav.left[start + n]);
            CHECK(frozen_launch.right[n] == live_launch.right[n] + ring_wav.right[start + n]);
        }
        CHECK(heard_live && heard_tail);
    }
    auto same_bytes = [](const auto& a, const auto& b) {
        return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(a[0])) == 0;
    };
    CHECK(same_bytes(resumed.left, uninterrupted.left) && same_bytes(resumed.right, uninterrupted.right));
    CHECK(same_bytes(resumed.main, uninterrupted.main));
}

int main(int argc, char** argv) {
    CHECK(argc == 5 || argc == 6);
    anvil_freeze(argv[1], argv[2], argv[3], std::stoul(argv[4]), argc == 6 ? std::stoi(argv[5]) : 2);
}
