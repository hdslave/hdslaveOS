// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
//
// lo-fly's panel in Melodee (see panel.h). The look: night. Each picture sits on a night sky over a skyline with lit
// windows, stars and a moon, with scan lines; curves are neon lines with a glow; knobs are LED rings. Type is Doto,
// a dot matrix like an LED sign, for titles and the wheel's value, glowing in the page's colour; labels and values
// are in Spleen, a sharp bitmap face. The LOFI page draws the tape's wobble as a moving line and the level on two LED columns.
// The pictures come from lo-fly's own code and values: the oscillator's cycle through the converter, the built-in
// noises, the LoFi chain's response, the envelopes, the voices that sound.
#include "panel.h"

#include "lofly/builtin_noise.h"
#include "lofly/hardware.h"
#include "lofly/lofi_processor.h"
#include "lofly/osc.h"
#include "lofly/param_format.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <tuple>

namespace lofly_melodee {
namespace {

using lofly::ParamId;

struct Glyph {
    unsigned offset;
    int width;   // of its coverage map
    int advance; // to the next glyph
    int left;    // where its map starts, from the pen (a script's strokes start left of it)
};
struct Font {
    const Glyph* glyphs;
    const unsigned char* pixels;
    int height;
    int ascent;
};
#include "fonts.h" // dt_title, dt_big: Doto at 22 and 24 px; sp_label, sp_value: Spleen 6 x 12 and 8 x 16

constexpr float pi = 3.14159265f;

// --- type and colours ---

// The type: titles and the wheel's value in Doto, a dot matrix, glowing in the page's colour; labels, values and
// small text in Spleen, a bitmap face, at its own sizes so every pixel is sharp (fonts.py). Where each sits: the
// header's text tops, a knob's label and value under its centre.
struct Type {
    const Font& small;
    const Font& label;
    const Font& value;
    const Font& title;
    const Font& big;
    float title_y, small_y, label_dy, value_dy;
    bool neon; // titles glow in the page's colour; else the video-tape colour fringe
};
const Type doto { sp_label_font, sp_label_font, sp_value_font, dt_title_font, dt_big_font, -2, 6, 21, 32, true };
const Type* T = &doto;

constexpr std::uint32_t ground = 0x100d1b, ink = 0xf4eaff, ink_dim = 0xb3a6d0, ink_faint = 0x6d628e, ink_ghost = 0x2f2848;
constexpr std::uint32_t sky_top = 0x15112a, sky_bottom = 0x3d2654, card_edge = 0x5b4a86;
constexpr std::uint32_t pink = 0xff8db4, yellow = 0xffd27a, mint = 0x8cf0b8, cyan = 0x7fddeb, violet = 0xb9a2ff;
constexpr std::array<std::uint32_t, 3> knob_colour { pink, yellow, cyan }; // K1-K3
constexpr std::uint32_t moon = 0xfff1c9, window = 0xffcf7a, building = 0x0d0a18, knob_face = 0x1c1733;
constexpr float fill_alpha = 0.14f; // the area under a curve

// --- layout (page) ---
constexpr float card_x = 8, card_y = 22, card_w = 304, card_h = 106;
constexpr std::array<float, 3> knob_x { 56, 160, 264 };
constexpr float knob_y_page = 152, knob_y_strip = 66;

// --- pages ---
enum class Pic { Lofi, Tape, Engine, Noise, Filter, Env, Fx, Voice };
struct PageDef {
    const char* title;
    Pic pic;
    int n; // engine, envelope or effect slot
    std::uint32_t accent;
};
constexpr PageDef pages[] {
    { "LOFI", Pic::Lofi, 0, pink },
    { "TAPE", Pic::Tape, 0, yellow },
    { "ENGINE 1", Pic::Engine, 0, cyan },
    { "ENGINE 2", Pic::Engine, 1, cyan },
    { "NOISE", Pic::Noise, 0, violet },
    { "FILTER", Pic::Filter, 0, mint },
    { "FILTER ENV", Pic::Env, 0, mint },
    { "AMP ENV", Pic::Env, 1, yellow },
    { "FX 1", Pic::Fx, 0, pink },
    { "FX 2", Pic::Fx, 1, pink },
    { "FX 3", Pic::Fx, 2, pink },
    { "FX 4", Pic::Fx, 3, pink },
    { "VOICE", Pic::Voice, 0, pink },
};
constexpr int page_total = static_cast<int>(std::size(pages));

ParamId eng(int n, lofly::EngineParam p) { return lofly::engineParam(n, p); }
std::array<ParamId, 4> env_ids(int n)
{
    if (n == 0)
        return { ParamId::FenvAttack, ParamId::FenvDecay, ParamId::FenvSustain, ParamId::FenvRelease };
    return { ParamId::AmpAttack, ParamId::AmpDecay, ParamId::AmpSustain, ParamId::AmpRelease };
}

// The section Push turns on and off.
std::optional<ParamId> on_param(const PageDef& p)
{
    switch (p.pic) {
    case Pic::Lofi:
    case Pic::Tape: return ParamId::LofiOn;
    case Pic::Engine: return eng(p.n, lofly::EngineParam::On);
    case Pic::Noise: return ParamId::NoiseOn;
    case Pic::Filter: return ParamId::FilterOn;
    case Pic::Fx: return lofly::fxParam(p.n, lofly::FxCommon::On);
    case Pic::Env:
    case Pic::Voice: return std::nullopt;
    }
    return std::nullopt;
}

// K2 and K3 of an effect page, per type; a timed field turns into its division while synced.
struct FxPick {
    const char* key = nullptr;
    const char* label = nullptr;
    const char* sync = nullptr;
    const char* division = nullptr;
};
using FxPicks = std::array<FxPick, 2>;
constexpr std::array<FxPicks, lofly::kFxTypes> fx_picks { {
    { {} },                                                                                    // None
    { { { "reverb.decay", "DECAY" }, { "reverb.size", "SIZE" } } },                            // Reverb
    { { { "delay.time", "TIME", "delay.sync", "delay.division" }, { "delay.feedback", "FEEDBACK" } } },
    { { { "tape.intensity", "INTENSITY" }, { "tape.age", "AGE" } } },                          // Tape Echo
    { { { "pitchdly.pitch", "PITCH" }, { "pitchdly.feedback", "FEEDBACK" } } },
    { { { "comp.threshold", "THRESHOLD" }, { "comp.ratio", "RATIO" } } },
    { { { "mb.input", "INPUT" }, { "mb.output", "OUTPUT" } } },
    { { { "mfilter.cutoff", "CUTOFF" }, { "mfilter.resonance", "RESONANCE" } } },
    { { { "eq.b1_gain", "LOW" }, { "eq.b5_gain", "HIGH" } } },
    { { { "dist.drive", "DRIVE" }, { "dist.tone", "TONE" } } },
    { { { "crush.bits", "BITS" }, { "crush.downsample", "DOWNSAMPLE" } } },
    { { { "gate.rate", "RATE", "gate.sync", "gate.division" }, { "gate.morph", "MORPH" } } },
    { { { "unison.detune", "DETUNE" }, { "unison.width", "WIDTH" } } },
    { { { "chorus.rate", "RATE" }, { "chorus.depth", "DEPTH" } } },
    { { { "bbd.rate", "RATE", "bbd.sync", "bbd.division" }, { "bbd.depth", "DEPTH" } } },
    { { { "flanger.rate", "RATE", "flanger.sync", "flanger.division" }, { "flanger.feedback", "FEEDBACK" } } },
    { { { "vflanger.rate", "RATE", "vflanger.sync", "vflanger.division" }, { "vflanger.feedback", "FEEDBACK" } } },
    { { { "phaser.rate", "RATE", "phaser.sync", "phaser.division" }, { "phaser.feedback", "FEEDBACK" } } },
    { { { "pan.rate", "RATE", "pan.sync", "pan.division" }, {} } },                         // Auto Pan
} };

struct FxIds {
    ParamId field = ParamId::Count, sync = ParamId::Count, division = ParamId::Count;
};
// Every slot's picks as parameter IDs, looked up once.
const std::array<std::array<std::array<FxIds, 2>, lofly::kFxTypes>, lofly::kFxSlots>& fx_ids()
{
    static const auto table = [] {
        std::array<std::array<std::array<FxIds, 2>, lofly::kFxTypes>, lofly::kFxSlots> t {};
        auto find = [](int slot, const char* key) {
            if (!key)
                return ParamId::Count;
            const auto id = lofly::findParam("fx" + std::to_string(slot + 1) + "." + key);
            return id ? *id : ParamId::Count;
        };
        for (int s = 0; s < lofly::kFxSlots; ++s)
            for (int type = 0; type < lofly::kFxTypes; ++type)
                for (int k = 0; k < 2; ++k) {
                    const auto& pick = fx_picks[static_cast<std::size_t>(type)][static_cast<std::size_t>(k)];
                    auto& ids = t[static_cast<std::size_t>(s)][static_cast<std::size_t>(type)][static_cast<std::size_t>(k)];
                    ids = { find(s, pick.key), find(s, pick.sync), find(s, pick.division) };
                }
        return t;
    }();
    return table;
}

std::string value_text(ParamId id, float v) { return lofly::formatDisplayValue(spec_of(id), v); }
std::string choice_name(const Store& s, ParamId id) { return value_text(id, s.get(id)); }

// --- drawing ---

std::uint32_t mix(std::uint32_t a, std::uint32_t b, int t) // t: 0..256 of b
{
    const std::uint32_t rb = (((a & 0xff00ffu) * static_cast<std::uint32_t>(256 - t) + (b & 0xff00ffu) * static_cast<std::uint32_t>(t)) >> 8) & 0xff00ffu;
    const std::uint32_t g = (((a & 0x00ff00u) * static_cast<std::uint32_t>(256 - t) + (b & 0x00ff00u) * static_cast<std::uint32_t>(t)) >> 8) & 0x00ff00u;
    return rb | g;
}
std::uint32_t lerp_colour(std::uint32_t a, std::uint32_t b, float t) { return mix(a, b, static_cast<int>(std::clamp(t, 0.0f, 1.0f) * 256.0f)); }

std::uint32_t hash(int x, int y, std::uint32_t seed = 0)
{
    std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393u + static_cast<std::uint32_t>(y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
// A fixed grain per pixel: -3..4 levels. It never changes, so it costs no redraw; it also dithers the 16-bit panel.
int grain(int x, int y) { return static_cast<int>(hash(x, y) & 7u) - 3; }
std::uint32_t shade(std::uint32_t c, int n)
{
    auto ch = [&](int shift) { return static_cast<std::uint32_t>(std::clamp(static_cast<int>((c >> shift) & 255u) + n, 0, 255)) << shift; };
    return ch(16) | ch(8) | ch(0);
}

struct Canvas {
    std::uint32_t* px;
    int w, h, stride;
    int x0 = 0, y0 = 0, x1 = 1 << 20, y1 = 1 << 20; // clip

    bool inside(int x, int y) const { return x >= std::max(0, x0) && y >= std::max(0, y0) && x < std::min(w, x1) && y < std::min(h, y1); }

    void blend(int x, int y, std::uint32_t c, float a)
    {
        if (a <= 0.004f || !inside(x, y))
            return;
        auto& p = px[y * stride + x];
        p = a >= 0.996f ? c : mix(p, c, static_cast<int>(a * 256.0f + 0.5f));
    }
    std::uint32_t& at(int x, int y) { return px[y * stride + x]; }
    void fill(float fx, float fy, float fw, float fh, std::uint32_t c, float a = 1.0f)
    {
        for (int y = static_cast<int>(fy); y < static_cast<int>(std::ceil(fy + fh)); ++y)
            for (int x = static_cast<int>(fx); x < static_cast<int>(std::ceil(fx + fw)); ++x)
                blend(x, y, c, a);
    }
    // Anti-aliased fill of the shape whose signed distance (negative inside) `dist` gives, inside a bounding box.
    template <class Dist> void shape(float bx0, float by0, float bx1, float by1, std::uint32_t c, float a, Dist dist)
    {
        const int ya = std::max(static_cast<int>(std::floor(by0)), std::max(0, y0));
        const int yb = std::min(static_cast<int>(std::ceil(by1)), std::min(h, y1));
        const int xa = std::max(static_cast<int>(std::floor(bx0)), std::max(0, x0));
        const int xb = std::min(static_cast<int>(std::ceil(bx1)), std::min(w, x1));
        for (int y = ya; y < yb; ++y)
            for (int x = xa; x < xb; ++x) {
                const float cover = std::clamp(0.5f - dist(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f), 0.0f, 1.0f);
                if (cover > 0.0f)
                    blend(x, y, c, a * cover);
            }
    }
    void disc(float cx, float cy, float r, std::uint32_t c, float a = 1.0f)
    {
        shape(cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1, c, a, [&](float x, float y) { return std::hypot(x - cx, y - cy) - r; });
    }
    void ring(float cx, float cy, float r, float t, std::uint32_t c, float a = 1.0f)
    {
        const float o = r + t;
        shape(cx - o - 1, cy - o - 1, cx + o + 1, cy + o + 1, c, a,
            [&](float x, float y) { return std::fabs(std::hypot(x - cx, y - cy) - r) - t * 0.5f; });
    }
    // A line `width` wide with round ends.
    void seg(float ax, float ay, float bx, float by, float width, std::uint32_t c, float a = 1.0f)
    {
        const float r = width * 0.5f;
        const float vx = bx - ax, vy = by - ay, len2 = std::max(vx * vx + vy * vy, 1e-6f);
        shape(std::min(ax, bx) - r - 1, std::min(ay, by) - r - 1, std::max(ax, bx) + r + 1, std::max(ay, by) + r + 1, c, a,
            [&](float x, float y) {
                const float t = std::clamp(((x - ax) * vx + (y - ay) * vy) / len2, 0.0f, 1.0f);
                return std::hypot(x - ax - vx * t, y - ay - vy * t) - r;
            });
    }
    static float rrect_dist(float x, float y, float rx, float ry, float rw, float rh, float r)
    {
        const float qx = std::fabs(x - (rx + rw * 0.5f)) - (rw * 0.5f - r);
        const float qy = std::fabs(y - (ry + rh * 0.5f)) - (rh * 0.5f - r);
        return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - r;
    }
    void rrect(float rx, float ry, float rw, float rh, float r, std::uint32_t c, float a = 1.0f)
    {
        shape(rx - 1, ry - 1, rx + rw + 1, ry + rh + 1, c, a, [&](float x, float y) { return rrect_dist(x, y, rx, ry, rw, rh, r); });
    }
    void rrect_line(float rx, float ry, float rw, float rh, float r, float t, std::uint32_t c, float a = 1.0f)
    {
        shape(rx - t - 1, ry - t - 1, rx + rw + t + 1, ry + rh + t + 1, c, a,
            [&](float x, float y) { return std::fabs(rrect_dist(x, y, rx, ry, rw, rh, r)) - t * 0.5f; });
    }
    void polyline(const float* xs, const float* ys, int n, float width, std::uint32_t c, float a = 1.0f)
    {
        for (int i = 1; i < n; ++i)
            seg(xs[i - 1], ys[i - 1], xs[i], ys[i], width, c, a);
    }
    // The area between a curve and `base`, one column a pixel.
    void area(const float* ys, float x, int n, float base, std::uint32_t c, float a)
    {
        for (int i = 0; i < n; ++i) {
            const float top = std::min(ys[i], base), bottom = std::max(ys[i], base);
            const int xi = static_cast<int>(x) + i;
            for (int y = static_cast<int>(top); y < static_cast<int>(std::ceil(bottom)); ++y) {
                const float cover = std::clamp(std::min(static_cast<float>(y) + 1.0f, bottom) - std::max(static_cast<float>(y), top), 0.0f, 1.0f);
                blend(xi, y, c, a * cover);
            }
        }
    }
};

// A neon curve: a wide faint glow, then the line.
void stroke(Canvas& g, const float* xs, const float* ys, int n, std::uint32_t c, float a, float width = 2.0f)
{
    g.polyline(xs, ys, n, width + 4.0f, c, 0.16f * a);
    g.polyline(xs, ys, n, width, c, a);
}

int text_width(const Font& f, std::string_view s, int track = 0)
{
    int width = 0;
    for (char ch : s) {
        const auto code = static_cast<unsigned char>(ch);
        if (code >= 32 && code < 127)
            width += f.glyphs[code - 32].advance + track;
    }
    return s.empty() ? 0 : width - track;
}
void text(Canvas& g, const Font& f, float fx, float fy, std::string_view s, std::uint32_t c, int track = 0, float a = 1.0f)
{
    int x = static_cast<int>(std::lround(fx));
    const int y = static_cast<int>(std::lround(fy));
    for (char ch : s) {
        const auto code = static_cast<unsigned char>(ch);
        if (code < 32 || code >= 127)
            continue;
        const auto& glyph = f.glyphs[code - 32];
        for (int yy = 0; yy < f.height; ++yy)
            for (int xx = 0; xx < glyph.width; ++xx) {
                const unsigned cover = f.pixels[glyph.offset + static_cast<unsigned>(yy * glyph.width + xx)];
                if (cover)
                    g.blend(x + glyph.left + xx, y + yy, c, a * static_cast<float>(cover) / 255.0f);
            }
        x += glyph.advance + track;
    }
}
void text_centre(Canvas& g, const Font& f, float cx, float y, std::string_view s, std::uint32_t c, int track = 0, float a = 1.0f)
{
    text(g, f, cx - static_cast<float>(text_width(f, s, track)) * 0.5f, y, s, c, track, a);
}
// Neon: the text over a soft glow of its colour, the strokes paler than the glow.
void text_neon(Canvas& g, const Font& f, float x, float y, std::string_view s, std::uint32_t c, int track = 0, float a = 1.0f)
{
    for (auto [dx, dy, w] : { std::tuple { -2.0f, 0.0f, 0.1f }, std::tuple { 2.0f, 0.0f, 0.1f }, std::tuple { 0.0f, -2.0f, 0.1f },
             std::tuple { 0.0f, 2.0f, 0.1f }, std::tuple { -1.0f, -1.0f, 0.16f }, std::tuple { 1.0f, -1.0f, 0.16f }, std::tuple { -1.0f, 1.0f, 0.16f },
             std::tuple { 1.0f, 1.0f, 0.16f } })
        text(g, f, x + dx, y + dy, s, c, track, w * a);
    text(g, f, x, y, s, lerp_colour(c, 0xffffff, 0.62f), track, a);
}

// The colour fringe of a worn video tape, red one way and blue the other.
void text_fringe(Canvas& g, const Font& f, float x, float y, std::string_view s, std::uint32_t c, int track = 0, float a = 1.0f)
{
    text(g, f, x - 1.0f, y, s, 0xff4f7a, track, 0.55f * a);
    text(g, f, x + 1.0f, y, s, 0x4fd6ff, track, 0.55f * a);
    text(g, f, x, y, s, c, track, a);
}
// A title: glowing in the page's colour, or fringed.
void title_text(Canvas& g, const Font& f, float x, float y, std::string_view s, std::uint32_t accent, int track = 0, float a = 1.0f)
{
    if (T->neon)
        text_neon(g, f, x, y, s, accent, track, a);
    else
        text_fringe(g, f, x, y, s, ink, track, a);
}

// The page's wheel control: its value large, between two chevrons (drawn and glowing, or pixel triangles).
void selector(Canvas& g, float cx, float y, std::string_view s, std::uint32_t accent, float a)
{
    const float half = static_cast<float>(text_width(T->big, s)) * 0.5f;
    title_text(g, T->big, cx - half, y, s, accent, 0, a);
    const float my = y + static_cast<float>(T->big.ascent) * (T->neon ? 0.66f : 0.6f);
    for (float side : { -1.0f, 1.0f }) {
        const float tip = cx + side * (half + 14.0f), back = tip - side * 5.0f;
        if (!T->neon) {
            for (int i = 0; i < 7; ++i) {
                const float len = 4.0f - static_cast<float>(std::abs(i - 3));
                g.fill(side < 0 ? tip + 4.0f - len : tip - 4.0f, std::round(my) - 3.0f + static_cast<float>(i), len, 1.0f, accent, a);
            }
            continue;
        }
        g.seg(back, my - 5.0f, tip, my, 4.5f, accent, 0.15f * a);
        g.seg(tip, my, back, my + 5.0f, 4.5f, accent, 0.15f * a);
        g.seg(back, my - 5.0f, tip, my, 1.8f, accent, a);
        g.seg(tip, my, back, my + 5.0f, 1.8f, accent, a);
    }
}

// Where a knob's ring starts: 0, or the middle for a control that goes both ways.
float zero_of(ParamId id)
{
    const auto& sp = spec_of(id);
    return sp.min < 0.0f && sp.max > 0.0f ? lofly::toNormalized(sp, 0.0f) : 0.0f;
}
bool lit(float t, float zero, float n) { return t >= std::min(zero, n) - 0.001f && t <= std::max(zero, n) + 0.001f; }

// A knob drawn once, by lo-fly: an LED ring lit from zero to the value, a pointer, then the label and value.
// A pot that has not reached the value yet shows as a small ring at its own place.
void knob(Canvas& g, float cx, float cy, ParamId id, const Store& s, const char* label, int k, int pot, bool live, float a)
{
    constexpr float a0 = -2.36f, a1 = 2.36f;
    constexpr int leds = 15;
    const bool used = id != ParamId::Count;
    const float n = used ? s.normalized(id) : 0.0f, zero = used ? zero_of(id) : 0.0f;
    const float av = a0 + (a1 - a0) * n;
    const std::uint32_t colour = knob_colour[static_cast<std::size_t>(k)];
    const float ua = used ? a : 0.45f * a;
    for (int i = 0; i < leds; ++i) {
        const float t = static_cast<float>(i) / (leds - 1), an = a0 + (a1 - a0) * t;
        const float x = cx + std::sin(an) * 19.0f, y = cy - std::cos(an) * 19.0f;
        if (used && lit(t, zero, n)) {
            g.disc(x, y, 5.0f, colour, 0.2f * a);
            g.disc(x, y, 2.4f, colour, a);
        } else
            g.disc(x, y, 2.0f, ink_ghost, ua);
    }
    g.disc(cx, cy, 13.0f, knob_face, ua);
    g.ring(cx, cy, 12.8f, 1.0f, ink_faint, 0.7f * ua);
    if (!used)
        return;
    g.seg(cx + std::sin(av) * 4.0f, cy - std::cos(av) * 4.0f, cx + std::sin(av) * 10.0f, cy - std::cos(av) * 10.0f, 2.2f, colour, a);
    if (pot >= 0 && !live) {
        const float ap = a0 + (a1 - a0) * static_cast<float>(pot) / 127.0f;
        const float px = cx + std::sin(ap) * 27.0f, py = cy - std::cos(ap) * 27.0f;
        g.disc(px, py, 3.3f, ink, a);
        g.disc(px, py, 1.7f, ground, a);
    }
    text_centre(g, T->label, cx, cy + T->label_dy, label, colour, 1, a);
    const std::string v = value_text(id, s.get(id));
    const Font& vf = text_width(T->value, v) <= 98 ? T->value : T->label;
    text_centre(g, vf, cx, cy + T->value_dy, v, ink, 0, a);
}

// The page's title (after the section's lamp: play while on, stop while off), the preset's name, the page dots.
void header(Canvas& g, const PageDef& p, int page, std::optional<bool> on, const std::string& preset)
{
    if (on) {
        if (*on) {
            for (int i = 0; i < 9; ++i)
                g.fill(9.0f, 6.0f + static_cast<float>(i), 1.0f + static_cast<float>(std::min(i, 8 - i)), 1.0f, mint);
        } else
            g.fill(9.0f, 7.0f, 7.0f, 7.0f, ink_faint);
    }
    const float tx = on ? 22.0f : 10.0f;
    title_text(g, T->title, tx, T->title_y, p.title, p.accent, 1);
    float dx = 311.0f - static_cast<float>((page_total - 1) * 7 + 11);
    const float title_end = tx + static_cast<float>(text_width(T->title, p.title, 1));
    const float name_w = static_cast<float>(text_width(T->small, preset));
    if (title_end + 10.0f + name_w <= dx - 8.0f) // shown whole or not at all
        text(g, T->small, dx - 8.0f - name_w, T->small_y, preset, ink_dim);
    for (int i = 0; i < page_total; ++i) {
        if (i == page) {
            g.fill(dx, 9.0f, 11.0f, 4.0f, p.accent, 0.25f);
            g.fill(dx, 10.0f, 11.0f, 2.0f, p.accent);
            dx += 14.0f;
        } else {
            g.fill(dx + 1.0f, 10.0f, 2.0f, 2.0f, ink_faint);
            dx += 7.0f;
        }
    }
}

// --- the background (built once a page) ---

// Dark ground with scan lines; the card a night sky with stars, a moon (where nothing runs into it) and a skyline
// with a few windows lit.
void night_background(Canvas& g, bool strip, bool with_moon)
{
    for (int y = 0; y < g.h; ++y)
        for (int x = 0; x < g.w; ++x)
            g.at(x, y) = shade(ground, grain(x, y) / 2 - ((y & 1) ? 3 : 0));
    if (strip)
        return;
    auto in_card = [](float x, float y) { return Canvas::rrect_dist(x, y, card_x, card_y, card_w, card_h, 8.0f); };
    for (int y = static_cast<int>(card_y) - 1; y <= static_cast<int>(card_y + card_h); ++y)
        for (int x = static_cast<int>(card_x) - 1; x <= static_cast<int>(card_x + card_w); ++x) {
            const float cover = std::clamp(0.5f - in_card(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f), 0.0f, 1.0f);
            if (cover <= 0.0f)
                continue;
            const float t = (static_cast<float>(y) - card_y) / card_h;
            std::uint32_t c = shade(lerp_colour(sky_top, sky_bottom, t * t), grain(x + 3, y) / 2 - ((y & 1) ? 4 : 0));
            const std::uint32_t h = hash(x, y, 11);
            if (t < 0.7f && (h % 331u) == 0) // a star
                c = lerp_colour(c, 0xfff6e8, 0.35f + static_cast<float>((h >> 12) & 63u) / 100.0f);
            g.blend(x, y, c, cover);
        }
    const float mx = card_x + card_w - 30.0f, my = card_y + 22.0f;
    if (with_moon) { // a glow, then a crescent
        g.disc(mx, my, 19.0f, moon, 0.05f);
        g.disc(mx, my, 14.0f, moon, 0.08f);
        g.shape(mx - 11, my - 11, mx + 11, my + 11, moon, 1.0f, [&](float x, float y) {
            return std::max(std::hypot(x - mx, y - my) - 9.0f, -(std::hypot(x - mx - 4.5f, y - my + 3.0f) - 8.0f));
        });
    }
    float bx = card_x;
    for (int b = 0; bx < card_x + card_w; ++b) {
        const float w = 10.0f + static_cast<float>(hash(b, 1, 5) % 18u), top = 7.0f + static_cast<float>(hash(b, 2, 5) % 17u);
        for (int y = static_cast<int>(card_y + card_h - top); y < static_cast<int>(card_y + card_h); ++y)
            for (int x = static_cast<int>(bx); x < static_cast<int>(bx + w) - 1; ++x) {
                if (in_card(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f) > -0.5f)
                    continue;
                const int wx = x - static_cast<int>(bx), wy = y - static_cast<int>(card_y + card_h - top);
                const bool lit_window = wx % 4 >= 2 && wy % 5 >= 3 && wx > 1 && wx < static_cast<int>(w) - 3 && (hash(x / 4, y / 5, 9) % 5u) == 0;
                g.at(x, y) = lit_window ? window : shade(building, grain(x, y) / 2);
            }
        bx += w;
    }
    g.rrect_line(card_x + 0.5f, card_y + 0.5f, card_w - 1.0f, card_h - 1.0f, 8.0f, 1.0f, card_edge, 0.55f);
}

// --- the LOFI page ---

// The level on two LED columns (the dark segments are in the background); 0 VU at 0.708 of full, as a VU's needle.
constexpr float led_x = card_x + 252.0f, led_y = card_y + 92.0f;
float vu_position(float db) { return std::pow(10.0f, db / 20.0f) / std::pow(10.0f, 3.0f / 20.0f); }
std::uint32_t led_colour(int i) { return i >= 8 ? pink : i >= 6 ? yellow : mint; }
void leds_static(Canvas& g)
{
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < 10; ++i)
            g.fill(led_x + static_cast<float>(c) * 16.0f, led_y - static_cast<float>(i) * 6.0f - 4.0f, 12.0f, 4.0f, ink_ghost);
}
void leds(Canvas& g, float level, float a)
{
    const int on = static_cast<int>(std::lround(std::clamp(level, 0.0f, 1.0f) * 10.0f));
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < on; ++i) {
            const float x = led_x + static_cast<float>(c) * 16.0f, y = led_y - static_cast<float>(i) * 6.0f - 4.0f;
            g.fill(x - 2.0f, y - 2.0f, 16.0f, 8.0f, led_colour(i), 0.15f * a);
            g.fill(x, y, 12.0f, 4.0f, led_colour(i), a);
        }
}

// The character, the tape's wobble as a neon line across the sky (wow and flutter of the character, as deep as
// Wobble × Amount, moving while lo-fly sounds), the level.
void picture_lofi(Canvas& g, const Store& s, double spin, float level, std::uint32_t accent, float a)
{
    const auto& prof = lofly::characterProfile(static_cast<lofly::LofiCharacter>(s.index(ParamId::LofiCharacter)));
    const float wobble = s.get(ParamId::LofiWobble) * s.get(ParamId::LofiAmount);
    selector(g, card_x + 128.0f, card_y + 4.0f, choice_name(s, ParamId::LofiCharacter), accent, a);
    constexpr int n = 224;
    float xs[n], ys[n];
    const float cy = card_y + 62.0f, amp = 3.0f + 16.0f * wobble;
    const float wow = std::max(0.2f, prof.wowHz), flutter = std::max(1.0f, prof.flutterHz);
    for (int i = 0; i < n; ++i) {
        const float x = static_cast<float>(i);
        xs[i] = card_x + 14.0f + x;
        ys[i] = cy + amp * (0.8f * std::sin(2.0f * pi * (x / 140.0f - static_cast<float>(spin) * wow * 0.5f))
                       + 0.2f * std::sin(2.0f * pi * (x / 19.0f - static_cast<float>(spin) * flutter * 0.25f)));
    }
    stroke(g, xs, ys, n, accent, a, 2.2f);
    leds(g, level, a);
}

// The LoFi chain's response, dB at `f` Hz: speaker band, tape loss, head bump, tone tilt (each scaled by Amount).
float lofi_response(const Store& s, float f)
{
    const auto& p = lofly::characterProfile(static_cast<lofly::LofiCharacter>(s.index(ParamId::LofiCharacter)));
    const float amount = s.get(ParamId::LofiAmount);
    auto log_lerp = [](float from, float to, float t) { return from * std::pow(to / from, std::clamp(t, 0.0f, 1.0f)); };
    auto low_pass = [&](float fc, float q) {
        const float r = f / fc;
        return -10.0f * std::log10((1.0f - r * r) * (1.0f - r * r) + (r / q) * (r / q));
    };
    auto high_pass = [&](float fc, float q) {
        const float r = fc / f;
        return -10.0f * std::log10((1.0f - r * r) * (1.0f - r * r) + (r / q) * (r / q));
    };
    auto peak = [&](float fc, float db, float q) {
        const float d = std::log2(f / fc) * q * 1.6f;
        return db * std::exp(-d * d);
    };
    const float speaker = s.get(ParamId::LofiSpeaker) * amount, wear = s.get(ParamId::LofiWear) * amount;
    const float tone = s.get(ParamId::LofiTone) * amount;
    float db = 0.0f;
    db += high_pass(log_lerp(15.0f, p.speakerLowHz, speaker), 0.707f);
    db += low_pass(log_lerp(22000.0f, p.speakerHighHz, speaker), 0.707f);
    db += peak(p.speakerPeakHz, p.speakerPeakDb * speaker, p.speakerPeakQ);
    db += low_pass(log_lerp(22000.0f, p.wearLowpassHz, wear), 0.6f);
    db += low_pass(log_lerp(22000.0f, p.baseLowpassHz, amount), 0.6f);
    db += peak(p.bumpHz, p.bumpDb * amount, 1.0f);
    db += p.tiltMaxDb * tone * std::clamp(std::log2(f / p.tiltPivotHz) / 4.0f, -1.0f, 1.0f);
    return db;
}

// A frequency across the card: 20 Hz to 20 kHz, `n` columns.
float column_hz(int i, int n) { return 20.0f * std::pow(1000.0f, static_cast<float>(i) / static_cast<float>(n - 1)); }

void freq_grid(Canvas& g, float x, float y, float w, float h, float zero_y, float a)
{
    g.fill(x, zero_y, w, 1.0f, ink_ghost, a);
    for (float hz : { 100.0f, 1000.0f, 10000.0f }) {
        const float fx = x + w * std::log10(hz / 20.0f) / 3.0f;
        g.fill(fx, y, 1.0f, h, ink_ghost, 0.6f * a);
    }
}

constexpr int curve_n = 276;

void picture_tape(Canvas& g, const Store& s, std::uint32_t accent, float a)
{
    selector(g, card_x + card_w * 0.5f, card_y + 5.0f, choice_name(s, ParamId::LofiCharacter), accent, a);
    const float x = card_x + 18.0f, y = card_y + 34.0f, h = 50.0f, zero = y + 20.0f;
    freq_grid(g, x, y, curve_n, h, zero, a);
    float xs[curve_n], ys[curve_n];
    for (int i = 0; i < curve_n; ++i) {
        xs[i] = x + static_cast<float>(i);
        ys[i] = std::clamp(zero - lofi_response(s, column_hz(i, curve_n)) * 2.4f, y, y + h);
    }
    g.area(ys, x, curve_n, y + h, accent, fill_alpha * a);
    stroke(g, xs, ys, curve_n, accent, a);
    for (auto [hz, label] : { std::pair { 100.0f, "100" }, std::pair { 1000.0f, "1k" }, std::pair { 10000.0f, "10k" } })
        text(g, T->small, x + curve_n * std::log10(hz / 20.0f) / 3.0f + 3.0f, y + h - 1.0f, label, ink_dim, 0, a);
}

void picture_engine(Canvas& g, const Store& s, int n, std::uint32_t accent, float a)
{
    const float x = card_x + 18.0f, cy = card_y + 66.0f, amp = 24.0f;
    constexpr int w = curve_n;
    const int type = s.index(eng(n, lofly::EngineParam::Type));
    if (type != 0) { // Instrument or Sampler: the content comes from files
        selector(g, card_x + card_w * 0.5f, card_y + 5.0f, choice_name(s, eng(n, lofly::EngineParam::Type)), accent, a);
        g.fill(x, cy, w, 1.0f, ink_ghost, a);
        return;
    }
    selector(g, card_x + card_w * 0.5f, card_y + 5.0f, choice_name(s, eng(n, lofly::EngineParam::Wave)), accent, a);
    // One cycle of lo-fly's own oscillator at A4, through the engine's converter: its rate as steps, its bits as levels.
    constexpr float note = 440.0f, rate = note * w;
    lofly::VaOsc osc;
    osc.reset(0.0f);
    osc.configure(static_cast<lofly::OscWave>(s.index(eng(n, lofly::EngineParam::Wave))), s.get(eng(n, lofly::EngineParam::Timbre)), note, rate);
    float cycle[w];
    for (int i = 0; i < 3 * w; ++i)
        cycle[i % w] = osc.next();
    const auto mode = static_cast<lofly::HardwareMode>(s.index(eng(n, lofly::EngineParam::Hardware)));
    float xs[w], ys[w];
    const auto& hw = lofly::hardwareSpec(mode);
    const bool converter = mode != lofly::HardwareMode::Off;
    const float steps = converter ? hw.rateHz / note : 0.0f;
    const float levels = converter ? std::pow(2.0f, static_cast<float>(hw.bits - 1)) / lofly::HardwareStage::kFullScale : 0.0f;
    int held_step = -1;
    float held = 0.0f;
    for (int i = 0; i < w; ++i) {
        float v = cycle[i];
        if (converter) {
            const int step = static_cast<int>(static_cast<float>(i) / static_cast<float>(w) * steps);
            if (step != held_step) {
                held_step = step;
                held = std::round(v * levels) / levels;
            }
            v = held;
        }
        xs[i] = x + static_cast<float>(i);
        ys[i] = cy - std::clamp(v, -1.3f, 1.3f) * amp;
    }
    g.fill(x, cy, w, 1.0f, ink_ghost, a);
    g.area(ys, x, w, cy, accent, fill_alpha * a);
    stroke(g, xs, ys, w, accent, a);
}

const char* noise_name(int i)
{
    return i < 6 ? lofly::kNoiseTextureNames[static_cast<std::size_t>(i)].data() : lofly::kNoiseTransientNames[static_cast<std::size_t>(i - 6)].data();
}
int noise_index(const Store& s)
{
    return s.index(ParamId::NoiseMode) == 0 ? s.index(ParamId::NoiseTexture) : 6 + s.index(ParamId::NoiseTransient);
}

void picture_noise(Canvas& g, const Store& s, const std::array<std::array<float, 2>, 288>& columns, std::uint32_t accent, float a)
{
    const int i = noise_index(s);
    selector(g, card_x + card_w * 0.5f, card_y + 5.0f, noise_name(i), accent, a);
    text(g, T->small, card_x + 18.0f, card_y + 31.0f, i < 6 ? "TEXTURE" : "TRANSIENT", ink_dim, 1, a);
    const float x = card_x + 18.0f, cy = card_y + 70.0f, amp = 22.0f;
    g.fill(x, cy, curve_n, 1.0f, ink_ghost, a);
    for (int c = 0; c < curve_n; ++c) {
        const auto& col = columns[static_cast<std::size_t>(c * 288 / curve_n)];
        const float top = cy - col[1] * amp, bottom = cy - col[0] * amp;
        g.fill(x + static_cast<float>(c), top - 1.0f, 1.0f, std::max(1.0f, bottom - top) + 2.0f, accent, 0.18f * a);
        g.fill(x + static_cast<float>(c), top, 1.0f, std::max(1.0f, bottom - top), accent, 0.85f * a);
    }
}

// The filter: its response for Clean and Analog; for LoFi a sine sampled and held at the cutoff's rate.
void picture_filter(Canvas& g, const Store& s, std::uint32_t accent, float a)
{
    const int type = s.index(ParamId::FilterType), mode = s.index(ParamId::FilterMode);
    selector(g, card_x + card_w * 0.5f, card_y + 5.0f, choice_name(s, ParamId::FilterType), accent, a);
    constexpr int w = curve_n;
    const float x = card_x + 18.0f, y = card_y + 34.0f, h = 62.0f;
    const float cutoff = s.get(ParamId::FilterCutoff), res = s.get(ParamId::FilterResonance);
    float xs[w], ys[w];
    if (type == 2) {
        const float cy = y + h * 0.5f, amp = 24.0f;
        const float steps = 4.0f + 60.0f * s.normalized(ParamId::FilterCutoff);
        g.fill(x, cy, w, 1.0f, ink_ghost, a);
        for (int i = 0; i < w; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(w);
            xs[i] = x + static_cast<float>(i);
            ys[i] = cy - std::sin(4.0f * pi * t) * amp;
        }
        g.polyline(xs, ys, w, 1.2f, ink_faint, a);
        for (int i = 0; i < w; ++i) {
            const float t = std::floor(static_cast<float>(i) / static_cast<float>(w) * steps) / steps;
            ys[i] = cy - std::sin(4.0f * pi * t) * amp;
        }
        g.area(ys, x, w, cy, accent, fill_alpha * a);
        stroke(g, xs, ys, w, accent, a);
        return;
    }
    text(g, T->small, card_x + 18.0f, card_y + 31.0f, mode == 0 ? "LOW-PASS" : mode == 1 ? "BAND-PASS" : mode == 2 ? "HIGH-PASS" : "NOTCH", ink_dim, 1, a);
    const int stages = type == 0 ? s.index(ParamId::FilterSlope) + 1 : 1;
    const float q = 0.707f + res * res * 9.0f, zero = y + 22.0f;
    freq_grid(g, x, y + 10.0f, w, h - 10.0f, zero, a);
    for (int i = 0; i < w; ++i) {
        const float r = column_hz(i, w) / cutoff;
        auto stage = [&](float qq) {
            const float den = std::sqrt((1.0f - r * r) * (1.0f - r * r) + (r / qq) * (r / qq));
            switch (mode) {
            case 0: return 1.0f / den;
            case 1: return (r / qq) / den;
            case 2: return r * r / den;
            default: return std::fabs(1.0f - r * r) / den;
            }
        };
        float mag = stage(q);
        for (int k = 1; k < stages; ++k)
            mag *= stage(0.707f);
        const float db = 20.0f * std::log10(std::max(mag, 1e-4f));
        xs[i] = x + static_cast<float>(i);
        ys[i] = std::clamp(zero - db * 1.2f, y + 8.0f, y + h);
    }
    g.area(ys, x, w, y + h, accent, fill_alpha * a);
    stroke(g, xs, ys, w, accent, a);
    const float fx = x + w * std::log10(cutoff / 20.0f) / 3.0f;
    const int at = std::clamp(static_cast<int>(fx - x), 0, w - 1);
    g.disc(xs[at], ys[at], 3.2f, ink, a);
}

void picture_env(Canvas& g, const Store& s, int n, std::uint32_t accent, float a)
{
    const auto ids = env_ids(n);
    selector(g, card_x + card_w * 0.5f, card_y + 5.0f, "Sustain " + value_text(ids[2], s.get(ids[2])), accent, a);
    auto width = [](float t) { return 74.0f * std::log(1.0f + t * 25.0f) / std::log(1.0f + 10.0f * 25.0f); };
    const float top = card_y + 38.0f, base = card_y + 96.0f, x = card_x + 20.0f;
    const float sustain = s.get(ids[2]);
    const float xa = x + std::max(2.0f, width(s.get(ids[0])));
    const float xd = xa + std::max(2.0f, width(s.get(ids[1])));
    const float xs_ = xd + 56.0f;
    const float xr = xs_ + std::max(2.0f, width(s.get(ids[3])));
    const float sy = base - (base - top) * sustain;
    // The decay and release curves are drawn exponential, as envelopes sound.
    constexpr int seg_n = 24;
    float xs[2 * seg_n + 4], ys[2 * seg_n + 4];
    int k = 0;
    xs[k] = x; ys[k++] = base;
    xs[k] = xa; ys[k++] = top;
    for (int i = 1; i <= seg_n; ++i) {
        const float t = static_cast<float>(i) / seg_n;
        xs[k] = xa + (xd - xa) * t;
        ys[k++] = sy + (top - sy) * std::exp(-4.0f * t);
    }
    xs[k] = xs_; ys[k++] = sy;
    for (int i = 1; i <= seg_n; ++i) {
        const float t = static_cast<float>(i) / seg_n;
        xs[k] = xs_ + (xr - xs_) * t;
        ys[k++] = base + (sy - base) * std::exp(-4.0f * t);
    }
    g.fill(x, base, card_w - 38.0f, 1.0f, ink_ghost, a);
    float cols[curve_n];
    const int span = std::min(curve_n, static_cast<int>(xr - x) + 1);
    for (int c = 0, j = 1; c < span; ++c) {
        const float cx = x + static_cast<float>(c);
        while (j < k - 1 && xs[j] < cx)
            ++j;
        const float t = xs[j] > xs[j - 1] ? std::clamp((cx - xs[j - 1]) / (xs[j] - xs[j - 1]), 0.0f, 1.0f) : 1.0f;
        cols[c] = ys[j - 1] + (ys[j] - ys[j - 1]) * t;
    }
    g.area(cols, x, span, base, accent, fill_alpha * a);
    stroke(g, xs, ys, k, accent, a);
    for (auto [px, py] : { std::pair { xa, top }, std::pair { xd, sy }, std::pair { xs_, sy } })
        g.disc(px, py, 3.0f, ink, a);
}

void picture_fx(Canvas& g, const Store& s, int slot, std::uint32_t accent, float a)
{
    const float y = card_y + 12.0f, w = 54.0f, h = 20.0f, gap = 16.0f;
    const float x0 = card_x + 6.0f + (card_w - 6.0f - (4.0f * w + 3.0f * gap)) * 0.5f;
    for (int i = 0; i < lofly::kFxSlots; ++i) {
        const float x = x0 + static_cast<float>(i) * (w + gap);
        if (i > 0)
            g.seg(x - gap + 3.0f, y + h * 0.5f, x - 3.0f, y + h * 0.5f, 1.5f, ink_faint, a);
        const bool used = s.index(lofly::fxParam(i, lofly::FxCommon::Type)) != 0 && s.on(lofly::fxParam(i, lofly::FxCommon::On));
        if (i == slot)
            g.rrect_line(x - 3.0f, y - 3.0f, w + 6.0f, h + 6.0f, 4.0f, 1.4f, ink, a);
        if (used) {
            g.rrect(x - 2.0f, y - 2.0f, w + 4.0f, h + 4.0f, 4.0f, accent, 0.2f * a);
            g.rrect(x, y, w, h, 2.0f, accent, a);
        } else
            g.rrect_line(x, y, w, h, 2.0f, 1.2f, ink_faint, a);
        const char digit[2] { static_cast<char>('1' + i), 0 };
        text_centre(g, T->value, x + w * 0.5f, y + 1.0f, digit, used ? ground : ink_dim, 0, a);
    }
    const auto type_id = lofly::fxParam(slot, lofly::FxCommon::Type);
    const int type = s.index(type_id);
    selector(g, card_x + card_w * 0.5f, card_y + 44.0f, choice_name(s, type_id), accent, a);
    if (lofly::isSpatial(static_cast<lofly::FxType>(type)))
        text_centre(g, T->small, card_x + card_w * 0.5f, card_y + 78.0f, s.on(lofly::fxParam(slot, lofly::FxCommon::Send)) ? "SEND" : "INSERT",
            ink_dim, 1, a);
}

void picture_voice(Canvas& g, const Store& s, std::uint32_t accent, float a)
{
    selector(g, card_x + card_w * 0.5f, card_y + 5.0f, choice_name(s, ParamId::VoiceMode), accent, a);
    std::uint32_t mask = s.voices.load(std::memory_order_relaxed);
    mask = (mask & 0xffffu) | (mask >> 16); // the four spare voices (fading after a steal) show on the first lamps
    const int count = s.index(ParamId::VoiceCount);
    const float w = 26.0f, h = 14.0f, gap = 7.0f, x0 = card_x + 6.0f + (card_w - 6.0f - (8.0f * w + 7.0f * gap)) * 0.5f;
    for (int i = 0; i < 16; ++i) {
        const float x = x0 + static_cast<float>(i % 8) * (w + gap), y = card_y + 44.0f + static_cast<float>(i / 8) * 22.0f;
        if (i >= count)
            g.rrect_line(x, y, w, h, 2.0f, 1.0f, ink_ghost, a);
        else if (mask >> i & 1u) {
            g.rrect(x - 3.0f, y - 3.0f, w + 6.0f, h + 6.0f, 5.0f, accent, 0.18f * a);
            g.rrect(x, y, w, h, 2.0f, accent, a);
        } else
            g.rrect(x, y, w, h, 2.0f, ink_ghost, a);
    }
}

} // namespace

// --- Panel ---

int Panel::page_count() { return page_total; }
const char* Panel::page_title(int page) { return pages[static_cast<std::size_t>(std::clamp(page, 0, page_total - 1))].title; }

Panel::Panel(Store& store, void (*redraw)(void*), void* context)
    : store_(store)
    , redraw_(redraw)
    , context_(context)
{
    (void)fx_ids();
    const auto& noises = lofly::builtinNoises();
    for (int i = 0; i < 12; ++i) {
        const auto& b = i < 6 ? noises.textures[static_cast<std::size_t>(i)] : noises.transients[static_cast<std::size_t>(i - 6)];
        const int frames = i < 6 ? std::min(b.frames(), static_cast<int>(b.sampleRate * 1.5f)) : b.frames();
        float peak = 1e-6f;
        for (int f = 0; f < frames; ++f)
            peak = std::max(peak, std::fabs(b.left[static_cast<std::size_t>(f)]));
        auto& cols = noise_columns_[static_cast<std::size_t>(i)];
        for (int c = 0; c < 288; ++c) {
            const int f0 = frames * c / 288, f1 = std::max(f0 + 1, frames * (c + 1) / 288);
            float lo = 0.0f, hi = 0.0f;
            for (int f = f0; f < std::min(f1, frames); ++f) {
                lo = std::min(lo, b.left[static_cast<std::size_t>(f)]);
                hi = std::max(hi, b.left[static_cast<std::size_t>(f)]);
            }
            cols[static_cast<std::size_t>(c)] = { lo / peak, hi / peak };
        }
    }
}

void Panel::set_page(int page)
{
    page_ = std::clamp(page, 0, page_total - 1);
    store_.page.store(page_, std::memory_order_relaxed);
}

std::array<Panel::Knob, 3> Panel::knobs() const
{
    const auto& p = pages[static_cast<std::size_t>(page_)];
    auto k = [](ParamId id, const char* label) { return Knob { id, label, id != ParamId::Count }; };
    switch (p.pic) {
    case Pic::Lofi: return { k(ParamId::LofiAmount, "AMOUNT"), k(ParamId::LofiDrive, "DRIVE"), k(ParamId::LofiWear, "WEAR") };
    case Pic::Tape: return { k(ParamId::LofiTone, "TONE"), k(ParamId::LofiWobble, "WOBBLE"), k(ParamId::LofiSpeaker, "SPEAKER") };
    case Pic::Engine:
        return { k(eng(p.n, lofly::EngineParam::Timbre), "TIMBRE"), k(eng(p.n, lofly::EngineParam::Hardware), "HARDWARE"),
            k(eng(p.n, lofly::EngineParam::Level), "LEVEL") };
    case Pic::Noise: return { k(ParamId::NoiseLevel, "LEVEL"), k(ParamId::NoiseTune, "TUNE"), k(ParamId::NoiseStereo, "STEREO") };
    case Pic::Filter:
        return { k(ParamId::FilterCutoff, "CUTOFF"), k(ParamId::FilterResonance, "RESONANCE"), k(ParamId::FilterEnvAmount, "ENV AMOUNT") };
    case Pic::Env: {
        const auto ids = env_ids(p.n);
        return { k(ids[0], "ATTACK"), k(ids[1], "DECAY"), k(ids[3], "RELEASE") };
    }
    case Pic::Fx: {
        const int type = store_.index(lofly::fxParam(p.n, lofly::FxCommon::Type));
        std::array<Knob, 3> out { k(type != 0 ? lofly::fxParam(p.n, lofly::FxCommon::Mix) : ParamId::Count, "MIX"), k(ParamId::Count, ""),
            k(ParamId::Count, "") };
        for (int i = 0; i < 2; ++i) {
            const auto& ids = fx_ids()[static_cast<std::size_t>(p.n)][static_cast<std::size_t>(type)][static_cast<std::size_t>(i)];
            const auto& pick = fx_picks[static_cast<std::size_t>(type)][static_cast<std::size_t>(i)];
            ParamId id = ids.field;
            if (ids.sync != ParamId::Count && store_.on(ids.sync))
                id = ids.division;
            out[static_cast<std::size_t>(i + 1)] = k(id, pick.label ? pick.label : "");
        }
        return out;
    }
    case Pic::Voice: return { k(ParamId::Glide, "GLIDE"), k(ParamId::LofiVintage, "VINTAGE"), k(ParamId::MasterVolume, "VOLUME") };
    }
    return {};
}

void Panel::controls(std::uint32_t ids[3]) const
{
    const auto ks = knobs();
    for (int i = 0; i < 3; ++i)
        ids[i] = ks[static_cast<std::size_t>(i)].used ? clap_id(spec_of(ks[static_cast<std::size_t>(i)].id).key) : MELODEE_SCREEN_NO_CONTROL;
}

bool Panel::input(const melodee_screen_input_t& in, bool& changed)
{
    changed = false;
    const auto& p = pages[static_cast<std::size_t>(page_)];
    auto step_choice = [&](ParamId id, int by) {
        const float before = store_.get(id);
        changed = store_.set(id, before + static_cast<float>(by)) != before;
    };
    switch (in.kind) {
    case MELODEE_SCREEN_STEP:
        set_page(page_ + (in.value < 0 ? -1 : 1));
        return true;
    case MELODEE_SCREEN_PUSH: {
        const auto on = on_param(p);
        if (!on)
            return false;
        store_.set(*on, store_.on(*on) ? 0.0f : 1.0f);
        changed = true;
        return true;
    }
    case MELODEE_SCREEN_WHEEL:
        switch (p.pic) {
        case Pic::Lofi:
        case Pic::Tape: step_choice(ParamId::LofiCharacter, in.value); return true;
        case Pic::Engine: step_choice(eng(p.n, lofly::EngineParam::Wave), in.value); return true;
        case Pic::Noise: {
            const int now = noise_index(store_), next = std::clamp(now + in.value, 0, 11);
            if (next != now) {
                store_.set(ParamId::NoiseMode, next < 6 ? 0.0f : 1.0f);
                store_.set(next < 6 ? ParamId::NoiseTexture : ParamId::NoiseTransient, static_cast<float>(next < 6 ? next : next - 6));
                changed = true;
            }
            return true;
        }
        case Pic::Filter: step_choice(ParamId::FilterType, in.value); return true;
        case Pic::Env: {
            const auto id = env_ids(p.n)[2];
            const float before = store_.get(id);
            store_.set_normalized(id, store_.normalized(id) + static_cast<float>(in.value) * (in.shift ? 0.002f : 0.01f));
            changed = store_.get(id) != before;
            return true;
        }
        case Pic::Fx: step_choice(lofly::fxParam(p.n, lofly::FxCommon::Type), in.value); return true;
        case Pic::Voice: step_choice(ParamId::VoiceMode, in.value); return true;
        }
        return false;
    default: return false;
    }
}

std::uint64_t Panel::signature(const melodee_screen_frame_t& f) const
{
    std::uint64_t h = 1469598103934665603ull;
    auto add = [&](std::uint64_t v) {
        h ^= v;
        h *= 1099511628211ull;
    };
    add(static_cast<std::uint64_t>(page_));
    add(f.area);
    add(f.width | static_cast<std::uint64_t>(f.height) << 16);
    add(std::hash<std::string> {}(store_.preset()));
    for (const auto& v : store_.values) {
        const float x = v.load(std::memory_order_relaxed);
        std::uint32_t bits;
        std::memcpy(&bits, &x, sizeof bits);
        add(bits);
    }
    for (int k = 0; k < 3; ++k)
        add(static_cast<std::uint64_t>(f.pots[k] + 1) << 1 | f.pots_live[k]);
    const auto pic = pages[static_cast<std::size_t>(page_)].pic;
    if (pic == Pic::Lofi) {
        add(static_cast<std::uint64_t>(spin_seconds_ * 60.0));
        add(static_cast<std::uint64_t>(needle_ * 400.0f));
    }
    if (pic == Pic::Voice)
        add(store_.voices.load(std::memory_order_relaxed));
    return h;
}

void Panel::build_background(int area)
{
    const int h = area == MELODEE_SCREEN_STRIP ? MELODEE_SCREEN_STRIP_HEIGHT : MELODEE_SCREEN_PAGE_HEIGHT;
    background_.assign(static_cast<std::size_t>(MELODEE_SCREEN_WIDTH * h), 0);
    Canvas g { background_.data(), MELODEE_SCREEN_WIDTH, h, MELODEE_SCREEN_WIDTH };
    const auto& p = pages[static_cast<std::size_t>(page_)];
    const bool strip = area == MELODEE_SCREEN_STRIP;
    night_background(g, strip, p.pic != Pic::Fx && p.pic != Pic::Env); // no moon where the slots or a long value run
    if (!strip && p.pic == Pic::Lofi)
        leds_static(g);
    background_page_ = page_;
    background_area_ = area;
}

void Panel::paint(const melodee_screen_frame_t& f)
{
    const bool strip = f.area == MELODEE_SCREEN_STRIP;
    if (background_area_ != static_cast<int>(f.area) || background_page_ != page_ || background_.empty())
        build_background(static_cast<int>(f.area));
    const int h = std::min(static_cast<int>(f.height), static_cast<int>(background_.size() / MELODEE_SCREEN_WIDTH));
    const int w = std::min(static_cast<int>(f.width), static_cast<int>(MELODEE_SCREEN_WIDTH));
    for (int y = 0; y < h; ++y)
        std::memcpy(f.pixels + static_cast<std::size_t>(y) * f.stride, background_.data() + static_cast<std::size_t>(y * MELODEE_SCREEN_WIDTH),
            sizeof(std::uint32_t) * static_cast<std::size_t>(w));

    Canvas g { f.pixels, w, static_cast<int>(f.height), static_cast<int>(f.stride) };
    const auto& p = pages[static_cast<std::size_t>(page_)];
    const auto on_id = on_param(p);
    const bool on = !on_id || store_.on(*on_id);
    header(g, p, page_, on_id ? std::optional<bool>(on) : std::nullopt, store_.preset());
    if (!strip) {
        switch (p.pic) {
        case Pic::Lofi: picture_lofi(g, store_, spin_seconds_, needle_, p.accent, 1.0f); break;
        case Pic::Tape: picture_tape(g, store_, p.accent, 1.0f); break;
        case Pic::Engine: picture_engine(g, store_, p.n, p.accent, 1.0f); break;
        case Pic::Noise: picture_noise(g, store_, noise_columns_[static_cast<std::size_t>(noise_index(store_))], p.accent, 1.0f); break;
        case Pic::Filter: picture_filter(g, store_, p.accent, 1.0f); break;
        case Pic::Env: picture_env(g, store_, p.n, p.accent, 1.0f); break;
        case Pic::Fx: picture_fx(g, store_, p.n, p.accent, 1.0f); break;
        case Pic::Voice: picture_voice(g, store_, p.accent, 1.0f); break;
        }
        if (!on) // the section is off: the picture fades back
            g.rrect(card_x + 1.0f, card_y + 1.0f, card_w - 2.0f, card_h - 2.0f, 8.0f, sky_top, 0.68f);
    }
    const auto ks = knobs();
    const float ky = strip ? knob_y_strip : knob_y_page;
    for (int i = 0; i < 3; ++i) {
        const auto& kn = ks[static_cast<std::size_t>(i)];
        knob(g, knob_x[static_cast<std::size_t>(i)], ky, kn.used ? kn.id : ParamId::Count, store_, kn.label, i, f.pots[i], f.pots_live[i] != 0,
            on ? 1.0f : 0.45f);
    }
}

bool Panel::draw(const melodee_screen_frame_t& f)
{
    const double dt = last_ms_ && f.time_ms > last_ms_ ? std::min(0.25, static_cast<double>(f.time_ms - last_ms_) / 1000.0) : 0.0;
    last_ms_ = f.time_ms;
    const float peak = store_.peak.load(std::memory_order_relaxed);
    const bool sounding = peak > 1e-4f || store_.voices.load(std::memory_order_relaxed) != 0;
    const bool moving = sounding || f.playing || store_.playing.load(std::memory_order_relaxed);
    const bool lofi_page = pages[static_cast<std::size_t>(page_)].pic == Pic::Lofi;
    if (moving && store_.on(ParamId::LofiOn))
        spin_seconds_ += dt;
    // The level: 0 VU at -6 dBFS; rises in 60 ms, falls in 300 ms.
    const float target = peak > 1e-5f ? std::min(1.08f, vu_position(20.0f * std::log10(peak) + 6.0f)) : 0.0f;
    const float tau = target > needle_ ? 0.06f : 0.3f;
    needle_ += (target - needle_) * static_cast<float>(1.0 - std::exp(-dt / tau));
    if (needle_ < 0.001f)
        needle_ = 0.0f;
    animating_ = lofi_page && ((moving && store_.on(ParamId::LofiOn)) || needle_ > 0.0f);

    const std::uint64_t sig = signature(f);
    const bool fresh = f.redraw_all || sig != drawn_;
    if (fresh) {
        paint(f);
        drawn_ = sig;
    }
    // While nothing on the audio side asks for draws (silent, the host plays or the level falls), ask for the next.
    if (animating_ && !sounding && redraw_)
        redraw_(context_);
    return fresh;
}

} // namespace lofly_melodee
