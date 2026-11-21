// Which kinds of effect come to rest in silence: for each, from new and after two seconds of sound, whether
// process_silent() says "as rest() left it" for a whole resting window of silent blocks. (melodee-hdslave, HOST-1)
#include "melodee/engine/effects.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace melodee;
int main() {
    DelayPool pool;
    std::string never_new, never_after, late;
    unsigned kinds = 0, rest_new = 0, rest_after = 0;
    for (unsigned k = 1; k < unsigned(EffectKind::Count); ++k) {
        auto kind = EffectKind(k);
        if (kind == EffectKind::Plugin || !effect_info(kind).built) continue;
        ++kinds;
        for (int sounded = 0; sounded < 2; ++sounded) {
            Effect e;
            e.set_kind(kind, pool);
            e.finish_clearing();
            std::vector<float> l(128), r(128);
            if (sounded) for (unsigned b = 0; b < 700; ++b) {
                for (unsigned f = 0; f < 128; ++f) { float t = float(b * 128 + f) / 44100.0f; l[f] = 0.5f * std::sin(6.283f * 220 * t); r[f] = 0.4f * std::sin(6.283f * 331 * t); }
                e.process(l.data(), r.data(), 128);
            }
            // Up to ten minutes of silence: the block after which it stayed settled for a window (2,251 blocks).
            unsigned run = 0, settled_at = 0;
            const unsigned most = 10 * 60 * 44100 / 128;
            for (unsigned b = 0; b < most && !settled_at; ++b) {
                std::fill(l.begin(), l.end(), 0.0f); std::fill(r.begin(), r.end(), 0.0f);
                bool as_rest = e.process_silent(l.data(), r.data(), 128);
                bool silent = true;
                for (unsigned f = 0; f < 128; ++f) silent &= std::fabs(l[f]) < Effect::rest_floor && std::fabs(r[f]) < Effect::rest_floor;
                run = as_rest && silent ? run + 1 : 0;
                if (run >= 2251) settled_at = b + 1;
            }
            const char* name = effect_info(kind).name;
            if (!sounded) { if (settled_at) ++rest_new; else never_new += std::string(name) + ", "; }
            else if (!settled_at) never_after += std::string(name) + ", ";
            else { ++rest_after; double seconds = settled_at * 128 / 44100.0; if (seconds > 15) late += std::string(name) + " (" + std::to_string(int(seconds)) + " s), "; }
            e.set_kind(EffectKind::None, pool);
        }
    }
    std::printf("%u kinds. From new: %u rest; never: %s\nAfter sound: %u rest; never within ten minutes: %s\nslow to rest: %s\n", kinds, rest_new, never_new.c_str(), rest_after, never_after.c_str(), late.c_str());
}
