// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
//
// lo-fly's panel in Melodee (see panel.h). The look: a two-ink print on paper, as a risograph prints it. Blue carries
// the lines and the type, fluorescent pink the fills; both multiply onto the paper, so they turn violet where they
// cross, and the pink plate sits a fraction of a pixel off register. Areas under curves are a pink halftone screen.
// Type is National Park. The pictures come from lo-fly's own code and values: the oscillator's cycle through the
// converter, the built-in noises, the LoFi chain's response, the envelopes, the voices that sound.
#include "panel.h"

#include "lofly/builtin_noise.h"
#include "lofly/hardware.h"
#include "lofly/lofi_processor.h"
#include "lofly/osc.h"
#include "lofly/param_format.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

namespace lofly_melodee {
namespace {

using lofly::ParamId;

struct Glyph {
    unsigned offset;
    int width;   // of its coverage map
    int advance; // to the next glyph
    int left;    // where its map starts, from the pen
};
struct Font {
    const Glyph* glyphs;
    const unsigned char* pixels;
    int height;
    int ascent;
};
#include "fonts.h" // National Park: np_title, np_wheel, np_value, np_value_s, np_name, np_label, np_small (fonts.py)

constexpr float pi = 3.14159265f;

// --- inks and layout ---

constexpr std::uint32_t paper = 0xf3efe6, blue = 0x0d63ad, pink = 0xff4fa7;
constexpr float plate_dx = 0.6f, plate_dy = 0.4f; // the pink plate, off register

// Page: header to y 30, the wheel's choice at 41-59, the picture at 64-132, the knobs below. The strip: header, knobs.
constexpr float title_base = 27, wheel_base = 56;
constexpr std::array<float, 3> knob_x { 56, 160, 264 };
constexpr float knob_y_page = 169, knob_y_strip = 74;

// --- pages ---
enum class Pic { Lofi, Tape, Engine, Noise, Filter, Env, Fx, Voice };
struct PageDef {
    const char* title;
    Pic pic;
    int n; // engine, envelope or effect slot
};
constexpr PageDef pages[] {
    { "LOFI", Pic::Lofi, 0 },
    { "TAPE", Pic::Tape, 0 },
    { "ENGINE 1", Pic::Engine, 0 },
    { "ENGINE 2", Pic::Engine, 1 },
    { "NOISE", Pic::Noise, 0 },
    { "FILTER", Pic::Filter, 0 },
    { "FILTER ENV", Pic::Env, 0 },
    { "AMP ENV", Pic::Env, 1 },
    { "FX 1", Pic::Fx, 0 },
    { "FX 2", Pic::Fx, 1 },
    { "FX 3", Pic::Fx, 2 },
    { "FX 4", Pic::Fx, 3 },
    { "VOICE", Pic::Voice, 0 },
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
// Ink over what is printed already: each channel multiplied.
std::uint32_t multiply(std::uint32_t a, std::uint32_t b)
{
    auto ch = [&](int shift) { return (((a >> shift) & 255u) * ((b >> shift) & 255u) + 127u) / 255u << shift; };
    return ch(16) | ch(8) | ch(0);
}

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

// The halftone screen: dots 3 px apart on a grid turned 30 degrees. Coverage of the screen at a pixel's centre.
float screen(float x, float y)
{
    constexpr float c = 0.8660254f, s = 0.5f, pitch = 3.0f, r = 1.0f;
    const float u = (x * c + y * s) / pitch, v = (y * c - x * s) / pitch;
    const float du = (u - std::round(u)) * pitch, dv = (v - std::round(v)) * pitch;
    return std::clamp(0.5f - (std::hypot(du, dv) - r), 0.0f, 1.0f);
}

struct Canvas {
    std::uint32_t* px;
    int w, h, stride;
    int x0 = 0, y0 = 0, x1 = 1 << 20, y1 = 1 << 20; // clip
    bool ink = false;          // multiply onto the paper and what is printed on it
    float dx = 0.0f, dy = 0.0f; // where this plate sits against the other (shapes only)

    bool inside(int x, int y) const { return x >= std::max(0, x0) && y >= std::max(0, y0) && x < std::min(w, x1) && y < std::min(h, y1); }

    void blend(int x, int y, std::uint32_t c, float a)
    {
        if (a <= 0.004f || !inside(x, y))
            return;
        auto& p = px[y * stride + x];
        const std::uint32_t to = ink ? multiply(p, c) : c;
        p = a >= 0.996f ? to : mix(p, to, static_cast<int>(a * 256.0f + 0.5f));
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
        const int ya = std::max(static_cast<int>(std::floor(by0 + dy)), std::max(0, y0));
        const int yb = std::min(static_cast<int>(std::ceil(by1 + dy)), std::min(h, y1));
        const int xa = std::max(static_cast<int>(std::floor(bx0 + dx)), std::max(0, x0));
        const int xb = std::min(static_cast<int>(std::ceil(bx1 + dx)), std::min(w, x1));
        for (int y = ya; y < yb; ++y)
            for (int x = xa; x < xb; ++x) {
                const float cover = std::clamp(0.5f - dist(static_cast<float>(x) + 0.5f - dx, static_cast<float>(y) + 0.5f - dy), 0.0f, 1.0f);
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
    static float rect_dist(float x, float y, float rx, float ry, float rw, float rh)
    {
        const float qx = std::fabs(x - (rx + rw * 0.5f)) - rw * 0.5f, qy = std::fabs(y - (ry + rh * 0.5f)) - rh * 0.5f;
        return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f);
    }
    void rect(float rx, float ry, float rw, float rh, std::uint32_t c, float a = 1.0f)
    {
        shape(rx - 1, ry - 1, rx + rw + 1, ry + rh + 1, c, a, [&](float x, float y) { return rect_dist(x, y, rx, ry, rw, rh); });
    }
    void rect_line(float rx, float ry, float rw, float rh, float t, std::uint32_t c, float a = 1.0f)
    {
        shape(rx - t - 1, ry - t - 1, rx + rw + t + 1, ry + rh + t + 1, c, a,
            [&](float x, float y) { return std::fabs(rect_dist(x, y, rx, ry, rw, rh)) - t * 0.5f; });
    }
    // A convex quadrilateral, corners clockwise.
    void quad(const float (&qx)[4], const float (&qy)[4], std::uint32_t c, float a = 1.0f)
    {
        const float bx0 = std::min({ qx[0], qx[1], qx[2], qx[3] }), bx1 = std::max({ qx[0], qx[1], qx[2], qx[3] });
        const float by0 = std::min({ qy[0], qy[1], qy[2], qy[3] }), by1 = std::max({ qy[0], qy[1], qy[2], qy[3] });
        shape(bx0 - 1, by0 - 1, bx1 + 1, by1 + 1, c, a, [&](float x, float y) {
            float d = -1e9f;
            for (int i = 0; i < 4; ++i) {
                const int j = (i + 1) % 4;
                const float ex = qx[j] - qx[i], ey = qy[j] - qy[i], len = std::max(std::hypot(ex, ey), 1e-6f);
                d = std::max(d, ((x - qx[i]) * ey - (y - qy[i]) * ex) / len);
            }
            return d;
        });
    }
    // A band along a circle from angle `a0` to `a1` (radians from 12 o'clock, clockwise), round ends.
    void arc(float cx, float cy, float r, float width, float a0, float a1, std::uint32_t c, float a = 1.0f)
    {
        const float o = r + width;
        const float ex0 = cx + std::sin(a0) * r, ey0 = cy - std::cos(a0) * r, ex1 = cx + std::sin(a1) * r, ey1 = cy - std::cos(a1) * r;
        shape(cx - o - 1, cy - o - 1, cx + o + 1, cy + o + 1, c, a, [&](float x, float y) {
            const float an = std::atan2(x - cx, cy - y);
            if (an >= a0 && an <= a1)
                return std::fabs(std::hypot(x - cx, y - cy) - r) - width * 0.5f;
            return std::min(std::hypot(x - ex0, y - ey0), std::hypot(x - ex1, y - ey1)) - width * 0.5f;
        });
    }
    void polyline(const float* xs, const float* ys, int n, float width, std::uint32_t c, float a = 1.0f)
    {
        for (int i = 1; i < n; ++i)
            seg(xs[i - 1], ys[i - 1], xs[i], ys[i], width, c, a);
    }
    // The halftone screen between two curves, one column a pixel from `x`.
    void halftone(float x, int n, const float* top, const float* bottom, std::uint32_t c, float a)
    {
        for (int i = 0; i < n; ++i) {
            const float t = std::min(top[i], bottom[i]), b = std::max(top[i], bottom[i]);
            const int xi = static_cast<int>(x) + i;
            for (int y = static_cast<int>(t); y < static_cast<int>(std::ceil(b)); ++y) {
                const float cover = std::clamp(std::min(static_cast<float>(y) + 1.0f, b) - std::max(static_cast<float>(y), t), 0.0f, 1.0f);
                blend(xi, y, c, a * cover * screen(static_cast<float>(xi) + 0.5f - dx, static_cast<float>(y) + 0.5f - dy));
            }
        }
    }
    // A row (or column) of single dots, `step` apart.
    void dots_h(float fx0, float fx1, float fy, float step, std::uint32_t c, float a)
    {
        for (float fx = fx0; fx <= fx1; fx += step)
            fill(fx, fy, 1.0f, 1.0f, c, a);
    }
    void dots_v(float fx, float fy0, float fy1, float step, std::uint32_t c, float a)
    {
        for (float fy = fy0; fy <= fy1; fy += step)
            fill(fx, fy, 1.0f, 1.0f, c, a);
    }
};

// While alive, shapes print on the pink plate: off register by (plate_dx, plate_dy).
struct PinkPlate {
    Canvas& g;
    explicit PinkPlate(Canvas& c)
        : g(c)
    {
        g.dx = plate_dx;
        g.dy = plate_dy;
    }
    ~PinkPlate() { g.dx = g.dy = 0.0f; }
};

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
// Text on its baseline `base`.
void text(Canvas& g, const Font& f, float fx, float base, std::string_view s, std::uint32_t c, int track = 0, float a = 1.0f)
{
    int x = static_cast<int>(std::lround(fx));
    const int y = static_cast<int>(std::lround(base)) - f.ascent;
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
void text_centre(Canvas& g, const Font& f, float cx, float base, std::string_view s, std::uint32_t c, int track = 0, float a = 1.0f)
{
    text(g, f, cx - static_cast<float>(text_width(f, s, track)) * 0.5f, base, s, c, track, a);
}
void text_right(Canvas& g, const Font& f, float rx, float base, std::string_view s, std::uint32_t c, int track = 0, float a = 1.0f)
{
    text(g, f, rx - static_cast<float>(text_width(f, s, track)), base, s, c, track, a);
}

// The wheel's choice: its name over a stroke of pink marker, between two chevrons. A chevron fades where the list ends.
void wheel(Canvas& g, std::string_view s, bool prev, bool next, float a)
{
    const float hw = static_cast<float>(text_width(np_wheel_font, s)) * 0.5f + 7.0f;
    {
        PinkPlate plate(g);
        g.quad({ 160.0f - hw, 160.0f + hw, 160.0f + hw - 1.0f, 160.0f - hw + 1.0f }, { 47.0f, 45.5f, 58.0f, 59.0f }, pink, 0.85f * a);
    }
    text_centre(g, np_wheel_font, 160.0f, wheel_base, s, blue, 0, a);
    for (float side : { -1.0f, 1.0f }) {
        const float tip = 160.0f + side * 143.0f, back = tip - side * 7.0f;
        const float ca = (side < 0 ? prev : next) ? a : 0.25f * a;
        g.seg(back, 43.0f, tip, 49.0f, 2.2f, blue, ca);
        g.seg(tip, 49.0f, back, 55.0f, 2.2f, blue, ca);
    }
}

// Where a knob's arc starts: 0, or the middle for a control that goes both ways.
float zero_of(ParamId id)
{
    const auto& sp = spec_of(id);
    return sp.min < 0.0f && sp.max > 0.0f ? lofly::toNormalized(sp, 0.0f) : 0.0f;
}

// "60.0 %" as "60.0" and "%"; a value that is not a number and a unit stays whole.
std::pair<std::string, std::string> split_unit(const std::string& v)
{
    const auto space = v.rfind(' ');
    if (space == std::string::npos || space == 0)
        return { v, "" };
    const std::string number = v.substr(0, space), unit = v.substr(space + 1);
    const char first = number[0];
    const bool numeric = (first >= '0' && first <= '9') || first == '-' || first == '+' || first == '.';
    if (!numeric || unit.empty() || std::any_of(unit.begin(), unit.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return { v, "" };
    return { number, unit };
}

// A knob, printed: a disc in its ink (K1 pink, K2 blue, K3 both: violet), a dotted track with the value's arc over
// it (one mark a setting for a choice), a pointer; the label above, the value below. A pot that has not reached the
// value yet shows as a pink dot at its own place.
void knob(Canvas& g, float cx, float cy, ParamId id, const Store& s, const char* label, int k, int pot, bool live, float a)
{
    constexpr float a0 = -2.356f, a1 = 2.356f, r = 21.0f;
    const bool used = id != ParamId::Count;
    for (int i = 0; i <= 30; ++i) {
        const float an = a0 + (a1 - a0) * static_cast<float>(i) / 30.0f;
        g.disc(cx + std::sin(an) * r, cy - std::cos(an) * r, 0.75f, blue, (used ? 0.8f : 0.35f) * a);
    }
    if (!used)
        return;
    const auto& spec = spec_of(id);
    const float n = s.normalized(id), av = a0 + (a1 - a0) * n;
    if (k != 1) {
        PinkPlate plate(g);
        g.disc(cx, cy, 15.0f, pink, a);
    }
    if (k != 0)
        g.disc(cx, cy, 15.0f, blue, (k == 1 ? 0.35f : 0.55f) * a);
    const int choices = spec.kind == lofly::ParamKind::Choice ? static_cast<int>(spec.choices.size()) : 0;
    if (choices >= 2 && choices <= 32) {
        for (int i = 0; i < choices; ++i) {
            const float an = a0 + (a1 - a0) * static_cast<float>(i) / static_cast<float>(choices - 1);
            g.seg(cx + std::sin(an) * 19.5f, cy - std::cos(an) * 19.5f, cx + std::sin(an) * 23.0f, cy - std::cos(an) * 23.0f, 1.4f, blue, a);
        }
    } else {
        const float z = a0 + (a1 - a0) * zero_of(id);
        if (std::fabs(av - z) > 0.01f)
            g.arc(cx, cy, r, 3.2f, std::min(z, av), std::max(z, av), blue, a);
    }
    g.seg(cx + std::sin(av) * 4.0f, cy - std::cos(av) * 4.0f, cx + std::sin(av) * 15.0f, cy - std::cos(av) * 15.0f, 2.4f, blue, a);
    if (pot >= 0 && !live) {
        const float ap = a0 + (a1 - a0) * static_cast<float>(pot) / 127.0f;
        PinkPlate plate(g);
        g.disc(cx + std::sin(ap) * 27.0f, cy - std::cos(ap) * 27.0f, 3.0f, pink, a);
    }
    text_centre(g, np_label_font, cx, cy - 25.0f, label, blue, 1, a);
    // The value: number and unit on one baseline, the number in the largest size that keeps 8 px to the next knob's.
    const auto [number, unit] = split_unit(value_text(id, s.get(id)));
    const int unit_w = unit.empty() ? 0 : text_width(np_small_font, unit) + 3;
    const Font* f = &np_value_font;
    for (const Font* size : { &np_value_font, &np_value_s_font, &np_small_font }) {
        f = size;
        if (text_width(*f, number) + unit_w <= 96)
            break;
    }
    const float x = cx - static_cast<float>(text_width(*f, number) + unit_w) * 0.5f, base = cy + 41.0f;
    text(g, *f, x, base, number, blue, 0, a);
    if (!unit.empty())
        text(g, np_small_font, x + static_cast<float>(text_width(*f, number) + 3), base, unit, blue, 0, a);
}

// The page's title after the section's lamp (pink while on, a pink ring while off), the preset's name, the page number.
void header(Canvas& g, const PageDef& p, int page, std::optional<bool> on, const std::string& preset)
{
    if (on) {
        PinkPlate plate(g);
        if (*on)
            g.disc(13.0f, 16.0f, 5.0f, pink);
        else
            g.ring(13.0f, 16.0f, 4.2f, 1.6f, pink);
    }
    const float tx = on ? 22.0f : 9.0f;
    text(g, np_title_font, tx, title_base, p.title, blue);
    const float title_end = tx + static_cast<float>(text_width(np_title_font, p.title));
    const float name_w = static_cast<float>(text_width(np_name_font, preset));
    if (title_end + 12.0f + name_w <= 312.0f) // shown whole or not at all
        text(g, np_name_font, 312.0f - name_w, 15.0f, preset, blue);
    char number[16];
    std::snprintf(number, sizeof number, "%02d / %d", page + 1, page_total);
    text_right(g, np_small_font, 312.0f, 29.0f, number, blue, 0, 0.75f);
}

// The paper, with a fixed grain (built once a page).
void paper_background(Canvas& g)
{
    for (int y = 0; y < g.h; ++y)
        for (int x = 0; x < g.w; ++x)
            g.at(x, y) = shade(paper, grain(x, y) / 2);
}

// A frequency across the picture: 20 Hz to 20 kHz, `n` columns.
float column_hz(int i, int n) { return 20.0f * std::pow(1000.0f, static_cast<float>(i) / static_cast<float>(n - 1)); }
constexpr float curve_x = 12.0f;
constexpr int curve_n = 296;

// 100 Hz, 1 kHz and 10 kHz as dotted lines with their names, and the 0 dB line.
void freq_grid(Canvas& g, float top, float bottom, float zero_y, float a)
{
    g.dots_h(curve_x, curve_x + curve_n, zero_y, 4.0f, blue, 0.7f * a);
    for (auto [hz, label] : { std::pair { 100.0f, "100" }, std::pair { 1000.0f, "1k" }, std::pair { 10000.0f, "10k" } }) {
        const float fx = std::round(curve_x + curve_n * std::log10(hz / 20.0f) / 3.0f);
        g.dots_v(fx, top, bottom, 3.0f, blue, 0.55f * a);
        text(g, np_small_font, fx + 3.0f, top + 9.0f, label, blue, 0, 0.8f * a);
    }
}

// A curve in blue over the pink screen down (or up) to `base`.
void print_curve(Canvas& g, const float* xs, const float* ys, int n, float base, float a, float width = 2.0f)
{
    float bases[curve_n + 8];
    for (int i = 0; i < n; ++i)
        bases[i] = base;
    {
        PinkPlate plate(g);
        g.halftone(xs[0], n, ys, bases, pink, a);
    }
    g.polyline(xs, ys, n, width, blue, a);
}

// --- the LOFI page ---

// The level on two bars; 0 VU at -6 dBFS sits at 0.708 of the bar, as a VU's needle.
float vu_position(float db) { return std::pow(10.0f, db / 20.0f) / std::pow(10.0f, 3.0f / 20.0f); }
void level_bars(Canvas& g, float level, float a)
{
    constexpr float top = 72.0f, h = 54.0f;
    const float fill = std::clamp(level, 0.0f, 1.0f) * h;
    for (float x : { 266.0f, 288.0f }) {
        if (fill > 0.5f) {
            PinkPlate plate(g);
            g.rect(x, top + h - fill, 16.0f, fill, pink, a);
        }
        g.rect_line(x + 0.5f, top + 0.5f, 15.0f, h - 1.0f, 1.0f, blue, a);
    }
    for (float db : { -20.0f, -10.0f, -5.0f, 0.0f, 3.0f }) {
        const float y = std::round(top + h - vu_position(db) * h);
        g.fill(db == 0.0f ? 282.0f : 283.0f, y, db == 0.0f ? 6.0f : 4.0f, 1.0f, blue, a);
    }
}

// The character, the tape's wobble as a line (wow and flutter of the character, as deep as Wobble × Amount, moving
// while lo-fly sounds), the level.
void picture_lofi(Canvas& g, const Store& s, double spin, float level, float a)
{
    const auto& prof = lofly::characterProfile(static_cast<lofly::LofiCharacter>(s.index(ParamId::LofiCharacter)));
    const float wobble = s.get(ParamId::LofiWobble) * s.get(ParamId::LofiAmount);
    constexpr int n = 231;
    float xs[n], ys[n];
    const float cy = 96.0f, amp = 3.0f + 14.0f * wobble;
    const float wow = std::max(0.2f, prof.wowHz), flutter = std::max(1.0f, prof.flutterHz);
    for (int i = 0; i < n; ++i) {
        const float x = static_cast<float>(i);
        xs[i] = 10.0f + x;
        ys[i] = cy + amp * (0.8f * std::sin(2.0f * pi * (x / 140.0f - static_cast<float>(spin) * wow * 0.5f))
                       + 0.2f * std::sin(2.0f * pi * (x / 19.0f - static_cast<float>(spin) * flutter * 0.25f)));
    }
    g.dots_h(10.0f, 240.0f, cy, 4.0f, blue, 0.7f * a);
    print_curve(g, xs, ys, n, cy, a, 2.2f);
    level_bars(g, level, a);
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

void picture_tape(Canvas& g, const Store& s, float a)
{
    constexpr float top = 68.0f, bottom = 128.0f, zero = 88.0f;
    freq_grid(g, top, bottom, zero, a);
    float xs[curve_n], ys[curve_n];
    for (int i = 0; i < curve_n; ++i) {
        xs[i] = curve_x + static_cast<float>(i);
        ys[i] = std::clamp(zero - lofi_response(s, column_hz(i, curve_n)) * 2.0f, top, bottom);
    }
    print_curve(g, xs, ys, curve_n, bottom, a);
}

// One cycle of lo-fly's own oscillator at A4, through the engine's converter: its rate as steps (and one tick a
// sample under the picture), its bits as levels.
void picture_engine(Canvas& g, const Store& s, int n, float a)
{
    constexpr float cy = 97.0f, amp = 25.0f;
    constexpr int w = curve_n;
    g.dots_h(curve_x, curve_x + w, cy, 4.0f, blue, 0.7f * a);
    if (s.index(eng(n, lofly::EngineParam::Type)) != 0) // Instrument or Sampler: the content comes from files
        return;
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
        xs[i] = curve_x + static_cast<float>(i);
        ys[i] = cy - std::clamp(v, -1.3f, 1.3f) * amp;
    }
    print_curve(g, xs, ys, w, cy, a);
    if (converter && steps * 2.0f <= static_cast<float>(w))
        for (int k = 0; k < static_cast<int>(steps); ++k)
            g.fill(std::floor(curve_x + (static_cast<float>(k) + 0.5f) * static_cast<float>(w) / steps), 127.0f, 1.0f, 3.0f, blue, 0.8f * a);
}

const char* noise_name(int i)
{
    return i < 6 ? lofly::kNoiseTextureNames[static_cast<std::size_t>(i)].data() : lofly::kNoiseTransientNames[static_cast<std::size_t>(i - 6)].data();
}
int noise_index(const Store& s)
{
    return s.index(ParamId::NoiseMode) == 0 ? s.index(ParamId::NoiseTexture) : 6 + s.index(ParamId::NoiseTransient);
}

// The built-in noise itself: its highs and lows per column, screened pink between blue edges.
void picture_noise(Canvas& g, const Store& s, const std::array<std::array<float, 2>, 288>& columns, float a)
{
    const int i = noise_index(s);
    text(g, np_label_font, curve_x, 75.0f, i < 6 ? "TEXTURE" : "TRANSIENT", blue, 1, 0.8f * a);
    constexpr float cy = 102.0f, amp = 22.0f;
    float xs[curve_n], hi[curve_n], lo[curve_n];
    for (int c = 0; c < curve_n; ++c) {
        const auto& col = columns[static_cast<std::size_t>(c * 288 / curve_n)];
        xs[c] = curve_x + static_cast<float>(c);
        hi[c] = cy - col[1] * amp;
        lo[c] = cy - col[0] * amp;
        if (lo[c] - hi[c] < 1.0f)
            lo[c] = hi[c] + 1.0f;
    }
    {
        PinkPlate plate(g);
        g.halftone(curve_x, curve_n, hi, lo, pink, a);
    }
    g.polyline(xs, hi, curve_n, 1.2f, blue, a);
    g.polyline(xs, lo, curve_n, 1.2f, blue, a);
}

// The filter: its response for Clean and Analog; for LoFi a sine sampled and held at the cutoff's rate.
void picture_filter(Canvas& g, const Store& s, float a)
{
    const int type = s.index(ParamId::FilterType), mode = s.index(ParamId::FilterMode);
    constexpr int w = curve_n;
    const float cutoff = s.get(ParamId::FilterCutoff), res = s.get(ParamId::FilterResonance);
    float xs[w], ys[w];
    if (type == 2) {
        constexpr float cy = 97.0f, amp = 25.0f;
        const float steps = 4.0f + 60.0f * s.normalized(ParamId::FilterCutoff);
        g.dots_h(curve_x, curve_x + w, cy, 4.0f, blue, 0.7f * a);
        for (int i = 0; i < w; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(w);
            xs[i] = curve_x + static_cast<float>(i);
            ys[i] = cy - std::sin(4.0f * pi * t) * amp;
        }
        g.polyline(xs, ys, w, 1.0f, blue, 0.45f * a);
        for (int i = 0; i < w; ++i) {
            const float t = std::floor(static_cast<float>(i) / static_cast<float>(w) * steps) / steps;
            ys[i] = cy - std::sin(4.0f * pi * t) * amp;
        }
        print_curve(g, xs, ys, w, cy, a);
        return;
    }
    text(g, np_label_font, curve_x, 75.0f, mode == 0 ? "LOW-PASS" : mode == 1 ? "BAND-PASS" : mode == 2 ? "HIGH-PASS" : "NOTCH", blue, 1, 0.8f * a);
    const int stages = type == 0 ? s.index(ParamId::FilterSlope) + 1 : 1;
    constexpr float top = 80.0f, bottom = 128.0f, zero = 94.0f;
    const float q = 0.707f + res * res * 9.0f;
    freq_grid(g, top, bottom, zero, a);
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
        xs[i] = curve_x + static_cast<float>(i);
        ys[i] = std::clamp(zero - db * 1.2f, 68.0f, bottom);
    }
    print_curve(g, xs, ys, w, bottom, a);
    const int at = std::clamp(static_cast<int>(curve_n * std::log10(cutoff / 20.0f) / 3.0f), 0, w - 1);
    {
        PinkPlate plate(g);
        g.disc(xs[at], ys[at], 3.6f, pink, a);
    }
    g.ring(xs[at], ys[at], 3.6f, 1.4f, blue, a);
}

// The envelope's shape; decay and release are drawn exponential, as envelopes sound. Pink points at its corners.
void picture_env(Canvas& g, const Store& s, int n, float a)
{
    const auto ids = env_ids(n);
    auto width = [](float t) { return 74.0f * std::log(1.0f + t * 25.0f) / std::log(1.0f + 10.0f * 25.0f); };
    constexpr float top = 70.0f, base = 126.0f, x = curve_x + 2.0f;
    const float sustain = s.get(ids[2]);
    const float xa = x + std::max(2.0f, width(s.get(ids[0])));
    const float xd = xa + std::max(2.0f, width(s.get(ids[1])));
    const float xs_ = xd + 56.0f;
    const float xr = xs_ + std::max(2.0f, width(s.get(ids[3])));
    const float sy = base - (base - top) * sustain;
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
    g.dots_h(curve_x, curve_x + curve_n, base, 4.0f, blue, 0.7f * a);
    float cols[curve_n], bases[curve_n];
    const int span = std::min(curve_n, static_cast<int>(xr - x) + 1);
    for (int c = 0, j = 1; c < span; ++c) {
        const float cx = x + static_cast<float>(c);
        while (j < k - 1 && xs[j] < cx)
            ++j;
        const float t = xs[j] > xs[j - 1] ? std::clamp((cx - xs[j - 1]) / (xs[j] - xs[j - 1]), 0.0f, 1.0f) : 1.0f;
        cols[c] = ys[j - 1] + (ys[j] - ys[j - 1]) * t;
        bases[c] = base;
    }
    {
        PinkPlate plate(g);
        g.halftone(x, span, cols, bases, pink, a);
    }
    g.polyline(xs, ys, k, 2.0f, blue, a);
    for (auto [px, py] : { std::pair { xa, top }, std::pair { xd, sy }, std::pair { xs_, sy } }) {
        {
            PinkPlate plate(g);
            g.disc(px, py, 3.4f, pink, a);
        }
        g.ring(px, py, 3.4f, 1.3f, blue, a);
    }
}

// The four slots in a row: pink = an effect that is on, a bar under this page's slot, and under the bar SEND or
// INSERT for a reverb or delay type.
void picture_fx(Canvas& g, const Store& s, int slot, float a)
{
    constexpr float y = 82.0f, w = 54.0f, h = 20.0f, gap = 16.0f, x0 = 160.0f - (4.0f * w + 3.0f * gap) * 0.5f;
    for (int i = 0; i < lofly::kFxSlots; ++i) {
        const float x = x0 + static_cast<float>(i) * (w + gap);
        if (i > 0)
            g.seg(x - gap + 3.0f, y + h * 0.5f, x - 3.0f, y + h * 0.5f, 1.2f, blue, a);
        const bool used = s.index(lofly::fxParam(i, lofly::FxCommon::Type)) != 0 && s.on(lofly::fxParam(i, lofly::FxCommon::On));
        if (used) {
            PinkPlate plate(g);
            g.rect(x, y, w, h, pink, a);
        }
        g.rect_line(x + 0.5f, y + 0.5f, w - 1.0f, h - 1.0f, 1.2f, blue, used || i == slot ? a : 0.55f * a);
        if (i == slot)
            g.rect(x, y + h + 4.0f, w, 3.0f, blue, a);
        const char digit[2] { static_cast<char>('1' + i), 0 };
        text_centre(g, np_label_font, x + w * 0.5f, y + 14.0f, digit, blue, 0, used || i == slot ? a : 0.55f * a);
    }
    const int type = s.index(lofly::fxParam(slot, lofly::FxCommon::Type));
    if (lofly::isSpatial(static_cast<lofly::FxType>(type)))
        text_centre(g, np_label_font, x0 + static_cast<float>(slot) * (w + gap) + w * 0.5f, y + h + 20.0f,
            s.on(lofly::fxParam(slot, lofly::FxCommon::Send)) ? "SEND" : "INSERT", blue, 1, 0.8f * a);
}

// A lamp a voice: pink while it sounds, a frame while free, faint past the polyphony.
void picture_voice(Canvas& g, const Store& s, float a)
{
    std::uint32_t mask = s.voices.load(std::memory_order_relaxed);
    mask = (mask & 0xffffu) | (mask >> 16); // the four spare voices (fading after a steal) show on the first lamps
    const int count = s.index(ParamId::VoiceCount);
    constexpr float w = 26.0f, h = 14.0f, gap = 7.0f, x0 = 160.0f - (8.0f * w + 7.0f * gap) * 0.5f;
    for (int i = 0; i < 16; ++i) {
        const float x = x0 + static_cast<float>(i % 8) * (w + gap), y = 78.0f + static_cast<float>(i / 8) * 24.0f;
        if (i < count && (mask >> i & 1u)) {
            PinkPlate plate(g);
            g.rect(x, y, w, h, pink, a);
        }
        g.rect_line(x + 0.5f, y + 0.5f, w - 1.0f, h - 1.0f, 1.2f, blue, i < count ? a : 0.25f * a);
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
    paper_background(g);
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
    g.ink = true; // everything from here prints onto the paper
    const auto& p = pages[static_cast<std::size_t>(page_)];
    const auto on_id = on_param(p);
    const bool on = !on_id || store_.on(*on_id);
    header(g, p, page_, on_id ? std::optional<bool>(on) : std::nullopt, store_.preset());
    if (!strip) {
        const float a = on ? 1.0f : 0.35f; // the section is off: its picture prints faint
        auto choice_wheel = [&](ParamId id, const std::string& name) {
            const int i = store_.index(id), count = static_cast<int>(spec_of(id).choices.size());
            wheel(g, name, i > 0, i < count - 1, a);
        };
        switch (p.pic) {
        case Pic::Lofi:
        case Pic::Tape: choice_wheel(ParamId::LofiCharacter, choice_name(store_, ParamId::LofiCharacter)); break;
        case Pic::Engine: {
            const auto type = eng(p.n, lofly::EngineParam::Type), wave = eng(p.n, lofly::EngineParam::Wave);
            choice_wheel(wave, choice_name(store_, store_.index(type) != 0 ? type : wave));
            break;
        }
        case Pic::Noise: {
            const int i = noise_index(store_);
            wheel(g, noise_name(i), i > 0, i < 11, a);
            break;
        }
        case Pic::Filter: choice_wheel(ParamId::FilterType, choice_name(store_, ParamId::FilterType)); break;
        case Pic::Env: {
            const auto id = env_ids(p.n)[2];
            const float n = store_.normalized(id);
            wheel(g, "Sustain " + value_text(id, store_.get(id)), n > 0.0f, n < 1.0f, a);
            break;
        }
        case Pic::Fx: {
            const auto id = lofly::fxParam(p.n, lofly::FxCommon::Type);
            choice_wheel(id, choice_name(store_, id));
            break;
        }
        case Pic::Voice: choice_wheel(ParamId::VoiceMode, choice_name(store_, ParamId::VoiceMode)); break;
        }
        switch (p.pic) {
        case Pic::Lofi: picture_lofi(g, store_, spin_seconds_, needle_, a); break;
        case Pic::Tape: picture_tape(g, store_, a); break;
        case Pic::Engine: picture_engine(g, store_, p.n, a); break;
        case Pic::Noise: picture_noise(g, store_, noise_columns_[static_cast<std::size_t>(noise_index(store_))], a); break;
        case Pic::Filter: picture_filter(g, store_, a); break;
        case Pic::Env: picture_env(g, store_, p.n, a); break;
        case Pic::Fx: picture_fx(g, store_, p.n, a); break;
        case Pic::Voice: picture_voice(g, store_, a); break;
        }
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
