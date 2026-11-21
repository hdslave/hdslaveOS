// The whole app's path for a report: no sound at all, in every project start, because an empty track had been
// soloed (Track Mute mode, Shift + its pad) and the file kept it. The shell is driven as the hands drive it, with the
// engine run a block at a time beside it: Shift + an empty track's pad does not solo it (a notice says it is empty)
// and the mix plays on; a used track solos and unsolos as before; an empty track a project kept soloed silences
// nothing and goes off with Shift + its pad.
#include "melodee/engine/engine_host.h"
#include "melodee/engine/sample_data.h"
#include "melodee/shell/shell.h"
#include "melodee/shell/snapshot.h"
#include "surface/controls.h"
#include "surface/leds.h"
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>
namespace melodee { void busy_project(Project& p, PatternBank& b); std::vector<SampleData> bench_samples(); }
using namespace melodee;
using B = SurfaceButton;
int main() {
    alarm(120);
    auto host = std::make_unique<EngineHost>();
    auto shell = std::make_unique<Shell>(*host);
    busy_project(shell->project(), shell->patterns());
    shell->project().tracks[track_count - 1] = Track {}; // the bench project uses every track: one left empty
    auto samples = bench_samples();
    host->set_render_threads(0, 150000, false);
    host->load(shell->project(), shell->patterns());
    host->set_audio_format(44100, 384);
    { Command c {Command::Type::Reload}; c.pack = new ProjectPack(shell->project(), shell->patterns()); host->send(c); }
    for (unsigned s = 0; s < samples.size(); ++s) while (!host->offer_sample(s, &samples[s])) { std::vector<int32_t> o(256); host->render(o.data(), 128, 0.7f); }
    SurfaceLights lights;
    auto frame = std::make_unique<Snapshot>();
    std::vector<int32_t> out(256);
    uint32_t now = 1000;
    auto run = [&](unsigned blocks) {
        double sum = 0;
        for (unsigned n = 0; n < blocks; ++n) {
            now += 3;
            shell->lights(lights, now);
            if (n % 11 == 0) shell->snapshot(*frame, now);
            host->render(out.data(), 128, 0.7f);
            for (auto v : out) { double x = v / 2147483648.0; sum += x * x; }
        }
        return std::sqrt(sum / (blocks * 256.0));
    };
    auto button = [&](B b, bool down) { SurfaceEvent e; e.input.kind = SurfaceInput::Kind::Button; e.input.index = uint8_t(b); e.input.pressed = down; now += 20; shell->surface(e, now); };
    auto tap = [&](B b) { button(b, true); button(b, false); };
    auto pad = [&](unsigned p, bool down) { SurfaceEvent e; e.pad = int(p); e.midi.status = uint8_t(down ? 0x90 : 0x80); e.midi.key = uint8_t(p); e.midi.value = down ? 100 : 0; now += 20; shell->surface(e, now); };
    // Track Mute mode (Mute held), Shift + the track's pad, on Tracks (where the pads are the tracks).
    auto solo_press = [&](unsigned t) {
        shell->ui().page = PageId::Tracks;
        button(B::Mute, true); button(B::Shift, true); pad(t, true); pad(t, false); button(B::Shift, false); button(B::Mute, false);
    };
    auto& project = shell->project();
    int empty = -1, used = -1;
    for (unsigned t = 0; t < track_count; ++t) {
        if (!project.tracks[t].used() && empty < 0) empty = int(t);
        if (project.tracks[t].used() && used < 0) used = int(t);
    }
    int bad = 0;
    auto expect = [&](bool ok, const char* what) { std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what); bad += !ok; };
    if (empty < 0 || used < 0) { std::printf("FAIL the bench project has no empty or no used track\n"); return 1; }
    tap(B::Play);
    expect(run(200) > 0.001, "playing");
    solo_press(unsigned(empty));
    expect(!project.tracks[size_t(empty)].mix.solo, "Shift + an empty track's pad: not soloed");
    expect(std::string(shell->ui().notice) == "Track " + std::to_string(empty + 1) + " is empty", "the notice says it is empty");
    expect(run(200) > 0.001, "the mix plays on");
    solo_press(unsigned(used));
    expect(project.tracks[size_t(used)].mix.solo, "Shift + a used track's pad: soloed");
    expect(run(200) > 0.001, "the soloed track plays");
    float others = 0;
    for (unsigned t = 0; t < track_count; ++t) if (int(t) != used && project.tracks[t].used()) {
        if (host->meter(t) >= 1e-4f) std::printf("      track %u meter %.4f (engine %d)\n", t + 1, host->meter(t), int(project.tracks[t].engine));
        others = std::max(others, host->meter(t));
    }
    std::printf("      soloed track %d meter %.4f\n", used + 1, host->meter(unsigned(used)));
    // Their meters fall from where they were and a tail may ring out: silenced is 40 dB under the soloed track.
    expect(others < 0.01f * host->meter(unsigned(used)), "the other used tracks are silenced");
    solo_press(unsigned(used));
    expect(!project.tracks[size_t(used)].mix.solo && run(200) > 0.001, "Shift + its pad again: solo off, all heard");
    // A project kept an empty track soloed (an older build let it): nothing is silenced, and Shift + its pad turns it off.
    project.tracks[size_t(empty)].mix.solo = true;
    host->send({Command::Type::Solo, uint8_t(empty), 0, 0, 1});
    expect(run(200) > 0.001, "an empty track kept soloed silences nothing");
    solo_press(unsigned(empty));
    expect(!project.tracks[size_t(empty)].mix.solo, "Shift + its pad turns that solo off");
    std::printf("%s\n", bad ? "FAULT" : "all ok");
    return bad != 0;
}
