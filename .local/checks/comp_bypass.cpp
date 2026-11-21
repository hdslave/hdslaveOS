// The master compressor switched off and on while a project plays, with Sidechain off, keyed by a track and keyed
// by one pad of a kit: what comes out must go on, at a level like before. (A report: bypassing the compressor while
// its sidechain was being set up cut all sound and the meters.)
#include "melodee/engine/engine_host.h"
#include "melodee/engine/sample_data.h"
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>
#include <unistd.h>
namespace melodee { void busy_project(Project& p, PatternBank& b); std::vector<SampleData> bench_samples(); }
using namespace melodee;
int main() {
    int bad = 0;
    alarm(240); // a block that never returns ends the run
    for (int variant = 0; variant < 48; ++variant) {
        const bool late = variant >= 24;
        const bool sidechain = variant % 3 != 0, pad = variant % 3 == 2, rests = variant % 6 < 3;
        const unsigned workers = variant % 12 < 6 ? 0 : 3, rate = variant % 24 < 12 ? 48000 : 44100;
        EngineHost::chain_rests = rests;
        auto project = std::make_unique<Project>();
        auto bank = std::make_unique<PatternBank>();
        busy_project(*project, *bank);
        auto samples = bench_samples();
        auto host = std::make_unique<EngineHost>();
        host->set_render_threads(workers, 150000, false);
        host->set_overload_guard(workers != 0);
        host->load(*project, *bank);
        host->set_audio_format(rate, 384);
        { Command c {Command::Type::Reload}; c.pack = new ProjectPack(*project, *bank); host->send(c); }
        for (unsigned s = 0; s < 4; ++s) host->offer_sample(s, &samples[s]);
        if (late) { // delay compensation on: a Limiter looking ahead on a track, on the key track and on a bus
            for (unsigned chain : {3u, 0u, unsigned(track_count)}) {
                host->send({Command::Type::FxKind, uint8_t(chain), 0, 2, int16_t(EffectKind::Limiter)});
                Command look {Command::Type::FxParam, uint8_t(chain), 2, 2, int16_t(chain == 3 ? 8 : 5)};
                host->send(look);
            }
        }
        host->send({Command::Type::Compressor, 0, 0, 0, 1});
        if (sidechain) {
            host->send({Command::Type::Compressor, 0, 0, 7, 0});                   // the key: track 1
            host->send({Command::Type::Compressor, 0, 0, 8, int16_t(pad ? 0 : -1)}); // its pad 1, or the whole track
            host->send({Command::Type::Compressor, 0, 0, 6, 1});
        }
        host->send({Command::Type::Play});
        std::vector<int32_t> out(256);
        auto run = [&](unsigned blocks, float& level_most) {
            double sum = 0; bool finite = true; level_most = 0;
            for (unsigned n = 0; n < blocks; ++n) {
                host->render(out.data(), 128, 0.7f);
                for (auto v : out) { double x = v / 2147483648.0; sum += x * x; }
                for (unsigned t = 0; t < track_count; ++t) { float l = host->meter(t); finite &= std::isfinite(l); level_most = std::max(level_most, l); }
            }
            return finite ? std::sqrt(sum / (blocks * 256.0)) : -1.0;
        };
        float m0, m1, m2, m3;
        double on = run(400, m0);
        for (int turn = 0; turn < 40; ++turn) { // dialing it in: AMOUNT, ATTACK, RELEASE and In Boost turned while it plays
            host->send({Command::Type::Compressor, 0, 0, uint16_t(2 + turn % 4), int16_t((turn * 7) % (turn % 4 == 3 ? 25 : 101))});
            float m; run(10, m);
        }
        host->send({Command::Type::Compressor, 0, 0, 0, 0}); // bypassed
        double off = run(750, m1);
        host->send({Command::Type::Compressor, 0, 0, 0, 1});
        double again = run(750, m2);
        host->send({Command::Type::Compressor, 0, 0, 6, 0}); // Sidechain off while engaged
        double plain = run(750, m3);
        const bool ok = on > 0.001 && off > 0.001 && again > 0.001 && plain > 0.001;
        bad += !ok;
        std::printf("%s late=%d rests=%d workers=%u rate=%u  on %.4f (meters %.3f)  bypassed %.4f (%.3f)  on again %.4f (%.3f)  sidechain off %.4f (%.3f)  %s\n",
                    !sidechain ? "no sidechain " : pad ? "keyed by pad " : "keyed by track", int(late), int(rests), workers, rate, on, m0, off, m1, again, m2, plain, m3, ok ? "ok" : "SILENT");
    }
    EngineHost::chain_rests = true;
    return bad != 0;
}
