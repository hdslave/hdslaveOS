// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/ui/screensaver.h"
#include "melodee/shell/screensaver.h"
#include "melodee/ui/boot_art.h"
#include "melodee/ui/tokens.h"
#include <algorithm>

namespace melodee::ui {
namespace {
float brightness(const juce::PixelARGB& p) { return 0.3f * p.getRed() + 0.59f * p.getGreen() + 0.11f * p.getBlue(); } // premultiplied
}

bool Screensaver::shows(uint8_t choice, uint32_t now, uint32_t touched, bool busy) {
    bool due = screensaver::due(choice, now, touched, busy);
    if (due && !on) { // somewhere new each time, from the clock
        if (!loaded) load();
        began = now;
        x0 = int(now * 2654435761u >> 7);
        y0 = int(now * 2246822519u >> 9);
    }
    on = due;
    return on && logo.isValid();
}

// The boot logo's pixels (boot_art.h), cropped to them and scaled to two thirds: 202 x 34 becomes 135 x 23.
void Screensaver::load() {
    loaded = true;
    juce::MemoryOutputStream png;
    if (!juce::Base64::convertFromBase64(png, boot_logo_png)) return;
    auto art = juce::ImageFileFormat::loadFrom(png.getData(), png.getDataSize());
    if (!art.isValid()) return;
    art = art.convertedToFormat(juce::Image::ARGB);
    juce::Rectangle<int> used;
    {
        juce::Image::BitmapData pixels(art, juce::Image::BitmapData::readOnly);
        int left = art.getWidth(), right = 0, top = art.getHeight(), bottom = 0;
        for (int y = 0; y < art.getHeight(); ++y)
            for (int x = 0; x < art.getWidth(); ++x)
                if (pixels.getPixelColour(x, y).getAlpha()) {
                    left = std::min(left, x); right = std::max(right, x + 1);
                    top = std::min(top, y); bottom = std::max(bottom, y + 1);
                }
        if (left >= right || top >= bottom) return;
        used = {left, top, right - left, bottom - top};
    }
    logo = art.getClippedImage(used).rescaled((used.getWidth() * 2 + 1) / 3, (used.getHeight() * 2 + 1) / 3, juce::Graphics::highResamplingQuality)
               .convertedToFormat(juce::Image::ARGB);
    tinted = logo.createCopy();
    tinted_argb = ground_argb = 0;
    juce::Image::BitmapData pixels(logo, juce::Image::BitmapData::readOnly);
    darkest = 255, lightest = 0;
    for (int y = 0; y < logo.getHeight(); ++y) {
        auto* line = reinterpret_cast<const juce::PixelARGB*>(pixels.getLinePointer(y));
        for (int x = 0; x < logo.getWidth(); ++x)
            if (line[x].getAlpha() > 128) { // the edges' blend left out
                float b = brightness(line[x]) * 255.0f / line[x].getAlpha();
                darkest = std::min(darkest, b); lightest = std::max(lightest, b);
            }
    }
}

// The logo in `colour`, its light and shade kept: its darkest pixel takes one end, its lightest the other. On a dark
// screen the shade is the colour at a third and the light the colour lifted toward white; on a light one (DAY, LCD)
// the shade is the colour itself, as ink, and the light the colour half way to the ground, so the letters keep apart.
void Screensaver::tint(juce::Colour colour, juce::Colour ground, bool light_ground) {
    if (colour.getARGB() == tinted_argb && ground.getARGB() == ground_argb) return;
    tinted_argb = colour.getARGB();
    ground_argb = ground.getARGB();
    const juce::Colour shade = light_ground ? colour : colour.withMultipliedBrightness(0.35f);
    const juce::Colour lit = light_ground ? colour.interpolatedWith(ground, 0.55f) : colour.interpolatedWith(juce::Colours::white, 0.12f);
    juce::Image::BitmapData from(logo, juce::Image::BitmapData::readOnly);
    juce::Image::BitmapData to(tinted, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < logo.getHeight(); ++y) {
        auto* in = reinterpret_cast<const juce::PixelARGB*>(from.getLinePointer(y));
        auto* out = reinterpret_cast<juce::PixelARGB*>(to.getLinePointer(y));
        for (int x = 0; x < logo.getWidth(); ++x) {
            const float a = in[x].getAlpha() / 255.0f;
            if (a <= 0) { out[x].setARGB(0, 0, 0, 0); continue; }
            const float t = std::clamp((brightness(in[x]) / a - darkest) / std::max(lightest - darkest, 1.0f), 0.0f, 1.0f);
            const auto c = shade.interpolatedWith(lit, t);
            auto channel = [&](uint8_t v) { return uint8_t(float(v) * a + 0.5f); }; // premultiplied
            out[x].setARGB(in[x].getAlpha(), channel(c.getRed()), channel(c.getGreen()), channel(c.getBlue()));
        }
    }
}

void Screensaver::draw(juce::Graphics& g, uint32_t now) {
    g.fillAll(colour::bg);
    if (!logo.isValid()) return;
    auto at = screensaver::place(now - began, logo.getWidth(), logo.getHeight(), zone::width, zone::height, x0, y0);
    // A track colour a bounce, five tracks on each time (well apart from the last), the greys left out unless the
    // palette has nothing else.
    unsigned vivid[16], count = 0;
    for (unsigned k = 0; k < 16; ++k)
        if (track_colour(k * 5).getSaturation() >= 0.3f) vivid[count++] = k * 5;
    tint(track_colour(count ? vivid[at.bounces % count] : at.bounces * 5), colour::bg, palette().light);
    g.drawImageAt(tinted, at.x, at.y);
}

} // namespace melodee::ui
