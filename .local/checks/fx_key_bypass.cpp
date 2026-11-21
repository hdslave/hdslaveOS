// A Compressor (or Gate) effect keyed by another track or a bus, on a track, a bus and the master, bypassed and
// engaged again while a project plays: what comes out must go on. (A report: bypassing a sidechained compressor cut
// all sound and the meters.)
#include "melodee/engine/engine_host.h"
#include "melodee/engine/sample_data.h"
#include <cmath>
#include <cstdio>
#include <memory>
#include <unistd.h>
#include <vector>
namespace melodee { void busy_project(Project& p, PatternBank& b); std::vector<SampleData> bench_samples(); }
using namespace melodee;
int main() {
    alarm(300);
    int bad = 0;
    struct Case { const char* name; unsigned chain, key, kind; };
    const Case cases[] {
        {"track 6 keyed by track 1     ", 5, 1, 25}, {"track 1 keyed by track 6     ", 0, 6, 25}, {"bus A keyed by track 1       ", track_count, 1, 25},
        {"master keyed by track 1      ", master_chain, 1, 25}, {"master keyed by bus A        ", master_chain, 17, 25}, {"track 6 gate keyed by track 1", 5, 1, 26},
        {"track 6 keyed by bus B       ", 5, 18, 25}, {"track 6 keyed by itself      ", 5, 6, 25},
    };
    for (const auto& k : cases) for (unsigned workers : {0u, 3u}) for (int two : {0, 1}) {
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        busy_project(*project, *bank);
        auto samples = bench_samples();
        auto host = std::make_unique<EngineHost>();
        host->set_render_threads(workers, 150000, false);
        host->set_overload_guard(false);
        host->load(*project, *bank);
        host->set_audio_format(44100, 384);
        { Command c {Command::Type::Reload}; c.pack = new ProjectPack(*project, *bank); host->send(c); }
        for (unsigned s = 0; s < 4; ++s) host->offer_sample(s, &samples[s]);
        const unsigned slot = k.chain == master_chain ? 0 : 2;
        host->send({Command::Type::FxKind, uint8_t(k.chain), 0, uint16_t(slot), int16_t(k.kind)});
        host->send({Command::Type::FxKey, uint8_t(k.chain), 0, uint16_t(slot), int16_t(k.key)});
        if (two) { // and a second keyed track, the other way round: two tracks that wait for each other
            host->send({Command::Type::FxKind, 2, 0, 2, 25});
            host->send({Command::Type::FxKey, 2, 0, 2, int16_t(k.chain < track_count ? k.chain + 1 : 4)});
        }
        host->send({Command::Type::Play});
        std::vector<int32_t> out(256);
        auto run = [&](unsigned blocks) {
            double sum = 0;
            for (unsigned n = 0; n < blocks; ++n) { host->render(out.data(), 128, 0.7f); for (auto v : out) { double x = v / 2147483648.0; sum += x * x; } }
            return std::sqrt(sum / (blocks * 256.0));
        };
        double on = run(500);
        for (int turn = 0; turn < 30; ++turn) { Command p {Command::Type::FxParam, uint8_t(k.chain), uint8_t(turn % 4), uint16_t(slot), int16_t((turn * 13) % 90)}; host->send(p); run(8); }
        host->send({Command::Type::FxBypass, uint8_t(k.chain), 0, uint16_t(slot), 1});
        double off = run(500);
        host->send({Command::Type::FxBypass, uint8_t(k.chain), 0, uint16_t(slot), 0});
        double again = run(500);
        host->send({Command::Type::FxKey, uint8_t(k.chain), 0, uint16_t(slot), 0});
        double unkeyed = run(500);
        const bool ok = on > 0.001 && off > 0.001 && again > 0.001 && unkeyed > 0.001;
        bad += !ok;
        std::printf("%s workers=%u two=%d  on %.4f  bypassed %.4f  on again %.4f  key off %.4f  %s\n", k.name, workers, two, on, off, again, unkeyed, ok ? "ok" : "SILENT");
        std::fflush(stdout);
    }
    return bad != 0;
}
