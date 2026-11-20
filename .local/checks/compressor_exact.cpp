// The Compressor's shortcut (src/melodee/engine/effects.cpp, optimization.md FX-2) against the sums it replaced:
// the gain must be the same to the bit for every envelope, threshold, ratio and make-up gain.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
static float db(float decibels) { return std::pow(10.0f, decibels / 20.0f); }
static float before(float env, float a, float b, float c) {
    float over = 20 * std::log10(std::max(env, 1e-6f)) - a;
    float reduce = over > 0 ? over * (1 - 1 / b) : 0;
    return db(-reduce) * c;
}
static float after(float env, float a, float b, float c, bool comp_short, float comp_quiet) {
    float reduce = 0;
    if (!(comp_short && env <= comp_quiet)) {
        float over = 20 * std::log10(std::max(env, 1e-6f)) - a;
        reduce = over > 0 ? over * (1 - 1 / b) : 0;
    }
    return reduce != 0 ? db(-reduce) * c : c;
}
static bool same(float x, float y) { uint32_t a, b; std::memcpy(&a, &x, 4); std::memcpy(&b, &y, 4); return a == b || (x != x && y != y); }
int main() {
    std::mt19937 random(1);
    unsigned long long checked = 0, skipped = 0;
    for (int threshold = -130; threshold <= 6; ++threshold)
        for (int ratio : {1, 2, 3, 4, 8, 20, 100})
            for (int makeup : {-12, 0, 6, 24}) {
                const float a = float(threshold), b = float(ratio), c = db(float(makeup));
                const bool comp_short = a > -119.0f;
                const float comp_quiet = comp_short ? db(a) * 0.99f : 0.0f;
                auto check = [&](float env) {
                    ++checked;
                    if (comp_short && env <= comp_quiet) ++skipped;
                    if (!same(before(env, a, b, c), after(env, a, b, c, comp_short, comp_quiet))) {
                        std::printf("differs: env=%.9g a=%g b=%g c=%g before=%.9g after=%.9g\n", env, a, b, c, before(env, a, b, c), after(env, a, b, c, comp_short, comp_quiet));
                        std::exit(1);
                    }
                };
                for (float env : {0.0f, -0.0f, 1e-45f, 1e-38f, 9.9e-7f, 1e-6f, 1.1e-6f, 1.0f, 2.0f, 1e9f, INFINITY, NAN, -1.0f}) check(env);
                // Every float in a band of 20000 either side of the level the shortcut ends at, and of the threshold itself.
                for (float centre : {comp_quiet, db(a)}) {
                    if (!(centre > 0)) continue;
                    float low = centre, high = centre;
                    for (int n = 0; n < 20000; ++n) { low = std::nextafter(low, 0.0f); high = std::nextafter(high, INFINITY); }
                    for (float env = low; env <= high; env = std::nextafter(env, INFINITY)) check(env);
                }
                std::uniform_real_distribution<float> decibels(-140.0f, 12.0f);
                for (int n = 0; n < 20000; ++n) check(db(decibels(random)));
            }
    std::printf("compressor shortcut: %llu gains the same to the bit, %llu of them by the shortcut\n", checked, skipped);
    return 0;
}
