// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 hdslave
#include "melodee/ui/paint.h"
#include <algorithm>

namespace melodee::ui {

void Paint::contour_panel(int x, int y, int w, int h) {
    // Each selected tile or browser row takes a crop of one LCD-width engraving.
    static const juce::Image engraving = [] {
        juce::Image image(juce::Image::RGB, zone::width, 66, false);
        juce::Graphics press(image);
        juce::ColourGradient paper(hex(0x210B38), 6, 43, hex(0x310E43), 306, 0, false);
        paper.addColour(0.56, hex(0x1D0B38));
        press.setGradientFill(paper);
        press.fillRect(image.getBounds());

        // Vary the control points as well as the spacing. The lines spread on
        // the outer turns and pinch together as the printed band folds over.
        juce::Path lines;
        for (int n = 0; n < 11; ++n) {
            const float i = float(n);
            lines.startNewSubPath(-9, 65 - 3.4f * i);
            lines.cubicTo(30, 66 - 2 * i, 36, 1 - 3.9f * i, 67 + i, 17 - 3.3f * i);
            lines.cubicTo(105 + i, 37 - 3.5f * i, 58 + 2 * i, 82 - 2 * i, 112, 55 - 2.4f * i);
            lines.cubicTo(147, 38 - 2 * i, 150, -30 + 2 * i, 182 + i, -8 + 2.4f * i);
            lines.cubicTo(213 + i, 12 + i, 197, 76 - i, 232, 55 - 2.8f * i);
            lines.cubicTo(266 + i, 36 - 3 * i, 264 - i, -12 + 3 * i, 330, -4 + 3.8f * i);
        }
        // The warmer ink prints just to one side of the cooler key line.
        press.setColour(hex(0x491746));
        press.strokePath(lines, juce::PathStrokeType(2.0f), juce::AffineTransform::translation(1.2f, 0.8f));
        juce::ColourGradient ink(hex(0x51136D), 25, 0, hex(0x372175), 284, 44, false);
        ink.addColour(0.55, hex(0x421C70));
        press.setGradientFill(ink);
        press.strokePath(lines, juce::PathStrokeType(1.35f));
        // The exposed top edge catches a narrow second impression above the text.
        press.reduceClipRegion(0, 0, image.getWidth(), 2);
        juce::ColourGradient edge(hex(0xC45DE6), 48, 0, hex(0x8E85EC), 275, 0, false);
        press.setGradientFill(edge);
        press.strokePath(lines, juce::PathStrokeType(1.35f));
        return image;
    }();
    g.setOpacity(1.0f);
    // A tile taller than the engraving (66 px: today's tallest, the Settings doors) gets it stretched to its height,
    // not cut short; every shorter one takes its own crop, as drawn.
    g.drawImage(engraving, x, y, w, h, x, 0, w, std::min(h, engraving.getHeight()));
}

void Paint::label_plate(float x, float y, float w, float h, juce::Colour ink) {
    if (!palette().contour_print) {
        fill(x, y, w, h, ink, 3);
        return;
    }
    constexpr float cut = 4;
    juce::Path plate;
    plate.startNewSubPath(x + cut, y);
    plate.lineTo(x + w, y);
    plate.lineTo(x + w, y + h - cut);
    plate.lineTo(x + w - cut, y + h);
    plate.lineTo(x, y + h);
    plate.lineTo(x, y + cut);
    plate.closeSubPath();
    g.setColour(ink);
    g.fillPath(plate);
}

void Paint::note_block(float x, float y, float w, float h, juce::Colour ink, float radius) {
    fill(x, y, w, h, ink, radius);
    if (palette().contour_print && w >= 6 && h >= 4) {
        // A second ink at the foot of a note, inside its velocity and length.
        auto edge = ink.darker(0.55f);
        fill(x + 1, y + h - 1.5f, w - 1, 1.5f, edge);
    }
}

} // namespace melodee::ui
