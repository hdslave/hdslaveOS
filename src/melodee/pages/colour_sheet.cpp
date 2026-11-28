// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// TRACK COLOUR (issue 69, shell/track_colours.h), over the soft keys, the header and the picture: the sixteen colours
// as the pads sit (pads 13-16 along the top), the track's ringed, and its name; B1 BACK, B2 DEFAULT, B3 DONE.
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/track_colours.h"

namespace melodee {

void draw_colour_sheet(ui::Paint& p, const Snapshot& s) {
    using namespace ui;
    const unsigned t = s.ui.colour_track % track_count;
    const auto& track = s.project.tracks[t];
    const unsigned slot = track_colour_slot(track, t);
    const auto c = slot_colour(slot);
    p.fill(0, zone::soft_y, zone::width, zone::knobs_y - zone::soft_y, colour::bg);
    soft_keys(p, SoftKeys {{"BACK", "DEFAULT", "DONE"}, 2, c, {KeyIcon::Back, KeyIcon::None, KeyIcon::Check}});
    Header h = header_of(s);
    h.left.push_back(pill("TRACK COLOUR", colour::text));
    h.right.push_back(tag_of(s, t));
    header(p, h);

    constexpr float cell = 31, gap = 3, grid = 4 * cell + 3 * gap;
    const float gx = zone::pad, gy = zone::picture_y + (zone::picture_h - grid) / 2;
    for (unsigned k = 0; k < 16; ++k) {
        const unsigned row = 3 - k / 4, column = k % 4;
        const float x = gx + column * (cell + gap), y = gy + row * (cell + gap);
        p.fill(x, y, cell, cell, slot_colour(k), 5);
        if (k == slot) p.ring(x - 2.5f, y - 2.5f, cell + 5, cell + 5, colour::text, 7, 2);
    }
    // Its name, and whether it follows the track's place (DEFAULT) or was picked.
    const float tx = gx + grid + 18, tw = zone::width - tx - zone::pad;
    const juce::String name(ui::track_colour_names[slot % 16]);
    p.text(name, tx, gy + grid / 2 - 26, tw, 30, plain(c, type::value, Weight::SemiBold));
    p.text(track.colour ? "PICKED" : "DEFAULT", tx, gy + grid / 2 + 6, tw, 14, caps_text(colour::mid));
}

} // namespace melodee
