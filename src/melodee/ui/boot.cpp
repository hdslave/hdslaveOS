// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/ui/boot.h"
#include <algorithm>

namespace melodee::ui {
namespace {

juce::Image decode(const char* base64) {
    juce::MemoryOutputStream jpeg;
    if (!juce::Base64::convertFromBase64(jpeg, base64)) return {};
    auto image = juce::ImageFileFormat::loadFrom(jpeg.getData(), jpeg.getDataSize());
    if (!image.isValid() || image.getWidth() != 320 || image.getHeight() != 240) return {};
    return image;
}

float smooth(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

} // namespace

uint32_t BootAnimation::elapsed(uint32_t now) {
    if (!started) { started = true; start = now; }
    return now - start;
}

bool BootAnimation::finished(uint32_t now) {
    if (!loaded) load();
    return broken || elapsed(now) >= fade_from + out_ms;
}

bool BootAnimation::covers(uint32_t now) { return !finished(now) && elapsed(now) < fade_from; }

void BootAnimation::skip(uint32_t now) { fade_from = std::min(fade_from, elapsed(now)); }

// The first frame decides whether there is a film at all: without it the app starts at once.
void BootAnimation::load() {
    loaded = true;
    broken = !show(0);
}

// Frame `index` into `frame`, decoded if it is not the one there; a frame that will not decode leaves the one
// before it on screen.
bool BootAnimation::show(unsigned index) {
    index = std::min(index, boot_film::count - 1);
    if (int(index) == shown) return true;
    auto image = decode(boot_film::frames[index]);
    if (!image.isValid()) return false;
    frame = image;
    shown = int(index);
    return true;
}

void BootAnimation::draw(juce::Graphics& g, uint32_t now) {
    if (finished(now)) return;
    uint32_t at = elapsed(now);
    // The film's own clock: past its end, or once a touch has started the fade, the frame stays where it stands.
    uint32_t film_at = std::min({at, fade_from, boot_film::length_ms});
    show(unsigned(uint64_t(film_at) * boot_film::fps / 1000));
    float opacity = at < fade_from ? 1.0f : 1.0f - smooth(float(at - fade_from) / float(out_ms));
    g.setOpacity(opacity);
    g.drawImageAt(frame, 0, 0);
    g.setOpacity(1.0f);
}

} // namespace melodee::ui
