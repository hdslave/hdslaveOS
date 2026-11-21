// The whole app's path for a report: on the Compressor page (Shift + Pad 5) the sidechain is set up while a project
// plays, then B3 (BYPASS) is pressed, and all sound and the meters are gone. The shell is driven as the hands
// drive it and the engine is run a block at a time beside it; what comes out is watched.
#include "melodee/engine/engine_host.h"
#include "melodee/engine/sample_data.h"
#include "melodee/shell/shell.h"
#include "melodee/shell/snapshot.h"
#include "surface/controls.h"
#include "surface/leds.h"
#include <cmath>
#include <cstdio>
#include <memory>
#include <unistd.h>
#include <cstdlib>
#include <string>
#include <vector>
namespace melodee { void busy_project(Project& p, PatternBank& b); std::vector<SampleData> bench_samples(); }
using namespace melodee;
using B = SurfaceButton;
int main(int argc, char** argv) {
    alarm(600);
    auto host = std::make_unique<EngineHost>();
    auto shell = std::make_unique<Shell>(*host);
    busy_project(shell->project(), shell->patterns());
    auto samples = bench_samples();
    host->set_render_threads(3, 150000, false);
    host->set_overload_guard(true);
    host->load(shell->project(), shell->patterns());
    host->set_audio_format(44100, 384);
    { Command c {Command::Type::Reload}; c.pack = new ProjectPack(shell->project(), shell->patterns()); host->send(c); }
    for (unsigned s = 0; s < 4; ++s) host->offer_sample(s, &samples[s]);
    SurfaceLights lights;
    auto frame = std::make_unique<Snapshot>();
    std::vector<int32_t> out(256);
    uint32_t now = 1000;
    auto run = [&](unsigned blocks, const char* what) {
        double sum = 0; float meter = 0;
        for (unsigned n = 0; n < blocks; ++n) {
            now += 3;
            shell->lights(lights, now);
            if (n % 11 == 0) shell->snapshot(*frame, now);
            host->render(out.data(), 128, 0.7f);
            for (auto v : out) { double x = v / 2147483648.0; sum += x * x; }
            for (unsigned t = 0; t < track_count; ++t) meter = std::max(meter, host->meter(t));
        }
        double rms = std::sqrt(sum / (blocks * 256.0));
        const auto& c = shell->project().compressor;
        std::printf("%-44s rms %.4f  meters %.3f  page %u  comp on=%d sidechain=%d key=%u pad=%d  playing=%d  %s\n", what, rms, meter, unsigned(shell->ui().page),
                    int(c.on), int(c.sidechain), unsigned(c.key), c.key_pad == MasterComp::whole_kit ? -1 : int(c.key_pad), int(host->playing()), rms > 0.001 ? "" : "<-- SILENT");
        std::fflush(stdout);
        return rms;
    };
    auto button = [&](B b, bool down) { SurfaceEvent e; e.input.kind = SurfaceInput::Kind::Button; e.input.index = uint8_t(b); e.input.pressed = down; now += 20; shell->surface(e, now); };
    auto tap = [&](B b) { button(b, true); button(b, false); };
    auto pad = [&](unsigned p, bool down) { SurfaceEvent e; e.pad = int(p); e.midi.status = uint8_t(down ? 0x90 : 0x80); e.midi.key = uint8_t(p); e.midi.value = down ? 100 : 0; now += 20; shell->surface(e, now); };
    auto knob = [&](SurfaceControl c, int value) { SurfaceEvent e; e.input.kind = SurfaceInput::Kind::Control; e.input.index = uint8_t(c); e.input.value = uint8_t(value); now += 10; shell->surface(e, now); };
    int bad = 0;
    tap(B::Play);
    bad += run(400, "playing") <= 0.001;
    button(B::Shift, true); pad(4, true); pad(4, false); button(B::Shift, false); // Shift + Pad 5: the Compressor
    bad += run(100, "Compressor page opened") <= 0.001;
    // A random hand on the Compressor page's own controls while the project plays: the three keys, the pads (which
    // pick the key), the wheel and its push, K1-K3 (with and without Shift), the fader, Pad Bank. No control here
    // can silence the mix, so a stretch with nothing coming out is the fault looked for; the moves before it are printed.
    unsigned seed = argc > 1 ? unsigned(std::atoi(argv[1])) : 1, moves = argc > 2 ? unsigned(std::atoi(argv[2])) : 1500;
    std::vector<std::string> said;
    auto say = [&](std::string what) { said.push_back(std::move(what)); if (said.size() > 30) said.erase(said.begin()); };
    bool shift = false;
    for (unsigned n = 0; n < moves; ++n) {
        seed = seed * 1664525u + 1013904223u;
        unsigned r = (seed >> 8) % 100, v = (seed >> 16) % 128;
        if (r < 8) { tap(B::TopLeft); say("B1"); }
        else if (r < 14) { tap(B::TopMiddle); say("B2"); }
        else if (r < 26) { tap(B::TopRight); say("B3"); }
        else if (r < 50) { if (shift) continue; unsigned p = v % 16; pad(p, true); pad(p, false); say("pad " + std::to_string(p + 1)); } // Shift + a pad opens a tool: not this page's
        else if (r < 70) { auto k = v % 3 == 0 ? SurfaceControl::K1 : v % 3 == 1 ? SurfaceControl::K2 : SurfaceControl::K3; knob(k, int((seed >> 4) % 128)); say("K" + std::to_string(1 + v % 3) + (shift ? " (shift)" : "")); }
        else if (r < 78) { knob(SurfaceControl::Fader, int(v)); say("fader"); }
        else if (r < 86) { SurfaceEvent e; e.input.kind = SurfaceInput::Kind::Control; e.input.index = uint8_t(SurfaceControl::Wheel); e.input.delta = int(v % 5) - 2; if (!e.input.delta) e.input.delta = 1; now += 10; shell->surface(e, now); say("wheel"); }
        else if (r < 90) { tap(B::WheelPush); say("push"); }
        else if (r < 96) { shift = !shift; button(B::Shift, shift); say(shift ? "shift down" : "shift up"); }
        else { tap(B::PadBank); say("pad bank"); }
        if (shell->ui().page != PageId::Compressor) { // a push or a sheet took it elsewhere: back, as a player would go
            if (shift) { shift = false; button(B::Shift, false); }
            button(B::Shift, true); pad(4, true); pad(4, false); button(B::Shift, false);
            say("(back to the page)");
        }
        double sum = 0;
        for (unsigned blk = 0; blk < 120; ++blk) {
            now += 3; shell->lights(lights, now);
            if (blk % 11 == 0) shell->snapshot(*frame, now);
            host->render(out.data(), 128, 0.7f);
            for (auto s : out) { double x = s / 2147483648.0; sum += x * x; }
        }
        if (std::sqrt(sum / (120 * 256.0)) <= 0.0005) {
            ++bad;
            const auto& c = shell->project().compressor;
            std::printf("SILENT after move %u (comp on=%d sidechain=%d key=%u pad=%d, playing=%d, guard=%u/%u). The moves before:", n, int(c.on), int(c.sidechain), unsigned(c.key),
                        c.key_pad == MasterComp::whole_kit ? -1 : int(c.key_pad), int(host->playing()), host->guard_steals(), host->guard_blocks());
            for (const auto& s : said) std::printf(" %s;", s.c_str());
            std::printf("\n");
            break;
        }
    }
    std::printf("seed %s: %s\n", argc > 1 ? argv[1] : "1", bad ? "FAULT" : "no silent stretch");
    return bad != 0;
}
