// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// KEYS (Sample's third stop on a synth): what the pads play, the sixteen
// pads filling the picture as they sit: each its note large and its degree
// in the scale small, the roots filled in the track's colour, the pad played
// white. The key and scale are the header's chip (Chromatic, or 16 Levels'
// note, when the layout is that). K1 root, K2 scale, K3 layout; Shift + K3
// transposes the pads by semitones. Minus / Plus the octave. Push opens
// Edit, the engine's every page.
//
// Drums (K3's last layout; a drum plug-in picks it itself): the pads as a
// kit's, notes 36 up whatever the key, each named by the plug-in's voice
// there (else its note, a pad without a voice dark), the drum hit last in the
// track's colour. The chip says DRUMS, the pill the bank; Minus / Plus (and
// Pad Bank) step the bank, A-D.
//
// Prog (K3's last layout): each pad one chord of the track's progression, an
// MPC 3 .progression file (model/progression.h): its name short, a slash bass
// small under it, the file's Root chord in the track's colour, a pad with no
// chord dark. Pad Bank steps banks A-C. K1 the key, K2 the voicing; the wheel
// picks the progression on the card, its name shown large for a moment.
// Pads held on Scale, Chromatic or Chords: B2 STORE takes their notes, the
// layout turns to Prog and the next pad hit takes the chord (B1 BACK, or
// letting go of Sample: none). Keys shows while Sample is held, so the wheel
// and B1-B3 come here through the shell (shell.cpp wheel(), sample_key()).
#include "melodee/model/progression.h"
#include "melodee/model/scales.h"
#include "melodee/pages/common.h"
#include "melodee/pages/pages.h"
#include "melodee/shell/levels_keys.h"
#include "melodee/shell/progression_jobs.h"
#include "melodee/ui/widgets.h"

namespace melodee {
namespace {
using namespace ui;

constexpr unsigned shown_ms = 1500; // the progression's name after a wheel turn

bool picking(const Snapshot& s) { return s.ui.prog_shown_at && s.now - s.ui.prog_shown_at < shown_ms; }

ui::SoftKeys keys_of(const Snapshot& s) {
    auto c = colour_of(s);
    if (s.ui.prog_storing) return {{"BACK", "", ""}, -1, c, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}};
    const auto& t = s.track();
    bool notes = t.layout == PadLayout::Scale || t.layout == PadLayout::Chromatic || t.layout == PadLayout::Chords;
    if (s.ui.held_pads && notes && t.used() && t.engine != EngineKind::Sampler && !t.levels) return {{"", "STORE", ""}, -1, c};
    return sound_keys(s);
}

// The wheel turned: the progression large, its file's own name and size under it, the ones before and after small.
void draw_pick(Paint& p, const Snapshot& s) {
    const auto& t = s.track();
    juce::String title = juce::String::fromUTF8(t.prog.file[0] ? t.prog.file : "No progression");
    auto style = plain(colour::text, 26, Weight::SemiBold);
    auto lines = p.wrap(title, style, zone::width - 2 * 10, 2);
    float y = zone::picture_y + 8;
    for (const auto& line : lines) { p.text(line, 10, y, zone::width - 20, style.size * 1.2f, style); y += style.size * 1.2f; }
    unsigned chords = 0;
    for (const auto& ch : t.prog.chords) chords += ch.used();
    juce::String about = juce::String(int(chords)) + " chords" + juce::String::fromUTF8(" \xc2\xb7 key ") + note_letter(prog_key(t));
    juce::String name = juce::String::fromUTF8(t.prog.name), file = juce::String::fromUTF8(t.prog.file);
    if (name.endsWith(file) && name.length() > file.length()) { // "Prisms 2-A Lydian ...": the pack's name first
        auto pack = name.dropLastCharacters(file.length()).trimCharactersAtEnd(" -");
        if (pack.isNotEmpty()) about = pack + juce::String::fromUTF8(" \xc2\xb7 ") + about;
    }
    p.text(about, 11, y + 4, zone::width - 22, 18, p.fit(about, plain(colour::sec, type::body), zone::width - 22, type::floor));
    float foot = zone::knobs_y - 22;
    if (s.ui.prog_prev[0]) {
        juce::String prev = juce::String::fromUTF8("\xe2\x80\xb9 ") + juce::String::fromUTF8(s.ui.prog_prev);
        p.text(prev, 8, foot, 148, 18, p.fit(prev, plain(colour::mid, type::body), 148, type::floor));
    }
    if (s.ui.prog_next[0]) {
        juce::String next = juce::String::fromUTF8(s.ui.prog_next) + juce::String::fromUTF8(" \xe2\x80\xba");
        p.text(next, 164, foot, 148, 18, p.fit(next, plain(colour::mid, type::body), 148, type::floor), juce::Justification::centredRight);
    }
}

void chord_grid(Paint& p, const Snapshot& s);

void draw_prog(Paint& p, const Snapshot& s) {
    const auto& track = s.track();
    auto c = colour_of(s);
    Header h = header_of(s);
    if (s.ui.prog_storing) { // the chord held, waiting for its pad
        std::array<int, notes_per_step> keys {};
        for (unsigned n = 0; n < s.ui.prog_store_count; ++n) keys[n] = s.ui.prog_store_keys[n];
        std::string name = name_chord(keys.data(), s.ui.prog_store_count);
        if (name.empty()) name = note_name(keys[0]);
        h.left.push_back(chip(juce::String(name) + juce::String::fromUTF8(" \xe2\x86\x92 PAD"), c));
    } else {
        h.left.push_back(chip(juce::String("BANK ") + juce::String::charToString(juce::juce_wchar('A' + prog_bank(track))), c));
        if (s.ui.prog_count && s.ui.prog_at >= 0) h.left.push_back(pill("PROG " + juce::String(s.ui.prog_at + 1) + " / " + juce::String(s.ui.prog_count)));
        else if (track.prog.file[0]) h.left.push_back(pill(juce::String::fromUTF8(track.prog.file)));
    }
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, keys_of(s), h);
    if (picking(s) && !s.ui.prog_storing) { draw_pick(p, s); return; }
    chord_grid(p, s);
}

// The chords as the pads sit: each pad its chord's name short, a slash bass small under it, the file's Root chord in
// the track's colour, a pad with no chord dark. On a kit (16 Levels' Chords) the first sixteen chords.
void chord_grid(Paint& p, const Snapshot& s) {
    const auto& track = s.track();
    auto c = colour_of(s);
    constexpr float gap = 4, top = zone::picture_y + 2;
    constexpr float w = (zone::width - 2 * zone::pad - 3 * gap) / 4, ch = (zone::picture_h - 4 - 3 * gap) / 4;
    for (unsigned pad = 0; pad < 16; ++pad) {
        unsigned row = 3 - pad / 4, column = pad % 4;
        float x = zone::pad + column * (w + gap), y = top + row * (ch + gap);
        bool held = s.ui.held_pads & (1u << pad);
        unsigned index = track.engine == EngineKind::Sampler ? pad % 16 : prog_index(track, pad);
        const auto& chord = track.prog.chords[index];
        // 16 Levels' SET (shell/levels_keys.h): the pad whose chord is on the cards
        const bool building = s.ui.levels_sheet == levels_sheet_chord && index == s.ui.chord_index;
        if (!chord.used()) {
            p.fill(x, y, w, ch, colour::quiet, 3);
            if (s.ui.prog_storing || s.ui.levels_sheet == levels_sheet_pad) p.ring(x, y, w, ch, c, 3, 1.5f); // a free pad for the chord
            if (building) p.ring(x, y, w, ch, colour::text, 3, 2);
            continue;
        }
        if (building) p.ring(x - 1.5f, y - 1.5f, w + 3, ch + 3, colour::text, 4, 2);
        bool root = chord.root;
        p.fill(x, y, w, ch, held ? colour::text : root ? c : colour::surf, 3);
        auto fg = held || root ? colour::bg : colour::text;
        auto name = prog_pad_name(track, index);
        juce::String label = juce::String::fromUTF8(name.chord.c_str());
        auto style = p.fit(label, plain(fg, type::list, Weight::SemiBold), w - 10, type::floor);
        if (name.bass.empty()) {
            p.text(label, x + 6, y, w - 10, ch, style);
        } else {
            p.text(label, x + 6, y + 1, w - 10, ch * 0.55f, style);
            p.text("/ " + juce::String::fromUTF8(name.bass.c_str()), x + 6, y + ch * 0.5f, w - 10, ch * 0.45f,
                   plain(held || root ? colour::bg.withAlpha(0.7f) : colour::mid, type::small, Weight::Medium));
        }
    }
}

void draw(Paint& p, const Snapshot& s) {
    const auto& track = s.track();
    if (prog_pads(track)) { draw_prog(p, s); return; }
    auto c = colour_of(s);
    Header h = header_of(s);
    bool drums = drum_pads(track), multi = midi_multi(track);
    if (multi) {
        h.left.push_back(chip("MULTI", c));
        h.left.push_back(pill(juce::String::fromUTF8("CH 1\xe2\x80\x93" "16")));
    } else if (drums) {
        h.left.push_back(chip("DRUMS", c));
        h.left.push_back(pill(juce::String("BANK ") + juce::String::charToString(juce::juce_wchar('A' + drum_bank(track)))));
    } else { // the key the pads play in: the root and the scale (or Chromatic, or 16 Levels' note)
        juce::String key = track.layout == PadLayout::Levels ? juce::String(note_name(track.last_note)) + " LEVELS"
                         : juce::String(note_letter(s.project.root)) + " " + (track.layout == PadLayout::Chromatic ? juce::String("Chromatic")
                                                                                                                    : juce::String(scales()[s.project.scale % scales().size()].name));
        h.left.push_back(chip(key, c));
        juce::String octave = "OCT " + juce::String(track.octave);
        if (track.transpose) octave += juce::String::fromUTF8(" \xc2\xb7 ") + (track.transpose > 0 ? "+" : juce::String::fromUTF8("\xe2\x88\x92")) + juce::String(std::abs(track.transpose)) + " st";
        h.left.push_back(pill(octave));
    }
    h.right.push_back(tag_of(s, s.ui.track));
    frame(p, s, keys_of(s), h);

    std::array<uint8_t, 12> degrees {};
    unsigned in_scale = scale_degrees(s.project.scale, degrees);
    auto degree_of = [&](int key) { // its place in the scale, 1-based; 0 outside it
        int offset = ((key - int(s.project.root)) % 12 + 12) % 12;
        for (unsigned d = 0; d < in_scale; ++d) if (degrees[d] == offset) return int(d) + 1;
        return 0;
    };
    constexpr float gap = 4, top = zone::picture_y + 2;
    constexpr float w = (zone::width - 2 * zone::pad - 3 * gap) / 4, ch = (zone::picture_h - 4 - 3 * gap) / 4;
    for (unsigned pad = 0; pad < 16; ++pad) {
        unsigned row = 3 - pad / 4, column = pad % 4;
        float x = zone::pad + column * (w + gap), y = top + row * (ch + gap);
        bool held = s.ui.held_pads & (1u << pad);
        auto note = pad_note(s.project, track, pad);
        if (drums) { // the voice there by name, else its note (or channel in Multi mode); no voice at all: dark
            const char* voice = multi ? nullptr : s.ui.drum_voices.at(note.key);
            bool last = note.key == int(track.last_note), silent = !multi && !voice && s.ui.drum_voices.any();
            p.fill(x, y, w, ch, held ? colour::text : last ? c : silent ? colour::quiet : colour::surf, 3);
            juce::String label = multi ? "CH " + juce::String(pad + 1) : voice ? juce::String(voice) : juce::String(note_name(note.key));
            auto style = plain(held || last ? colour::bg : silent ? colour::dim : colour::text, type::body, Weight::SemiBold);
            p.text(label, x + 7, y, w - 14, ch, p.fit(label, style, w - 14, type::floor));
            continue;
        }
        p.fill(x, y, w, ch, held ? colour::text : note.root ? c : colour::surf, 3);
        auto fg = held || note.root ? colour::bg : colour::text;
        p.text(note_name(note.key), x + 7, y, w - 30, ch, plain(fg, type::list, Weight::SemiBold));
        int degree = track.layout == PadLayout::Chromatic ? 0 : degree_of(note.key);
        if (degree) p.text(juce::String(degree), x + w - 27, y + 3, 20, 14, plain(held || note.root ? colour::bg.withAlpha(0.7f) : colour::dim, type::small, Weight::SemiBold),
                           juce::Justification::centredRight);
    }
}
} // namespace

// 16 Levels held with Chords on (shell/sample_mode.h): the picture shows what each pad plays, the track's chords as
// the pads sit, or for a moment after the encoder turned, the progression's name. The page's header and the held
// cards stay as they are.
void draw_levels_chords(ui::Paint& p, const Snapshot& s) {
    using namespace ui;
    p.fill(0, zone::picture_y, zone::width, zone::picture_h, colour::bg);
    if (picking(s)) { draw_pick(p, s); return; }
    if (s.track().prog.empty()) {
        p.text("No chords yet: B2 FILE, or B3 SET", 0, zone::picture_y, zone::width, zone::picture_h, plain(colour::mid, type::body), juce::Justification::centred);
        return;
    }
    chord_grid(p, s);
}

// 16 Levels' FILE and SET (shell/levels_keys.h), over the soft keys, the header and the picture; the knob strip
// under them stays the page's (SET's cards: ROOT, CHORD, INVERSION).
//   FILE  the card's progression files, six rows at a time, the row in hand raised, the track's own file checked
//   SET   the chords as the pads sit: first the free pads ringed (tap the pad the chord goes on), then that pad
//         ringed in white, its chord named in the header
void draw_levels_sheet(ui::Paint& p, const Snapshot& s) {
    using namespace ui;
    const auto& track = s.track();
    auto c = colour_of(s);
    p.fill(0, zone::soft_y, zone::width, zone::knobs_y - zone::soft_y, colour::bg);
    Header h = header_of(s); // its first chip says CHORDS (shell/pad_job.h)
    h.right.push_back(tag_of(s, s.ui.track));
    if (s.ui.levels_sheet == levels_sheet_files) {
        soft_keys(p, SoftKeys {{"BACK", "", "PICK"}, 2, c, {KeyIcon::Back, KeyIcon::None, KeyIcon::Check}});
        h.left.push_back(pill("FILE " + juce::String(s.ui.prog_cursor + 1) + " / " + juce::String(s.ui.prog_count)));
        header(p, h);
        constexpr float row_h = 22, row_gap = 1.5f, x = zone::pad, w = zone::width - 2 * zone::pad;
        for (unsigned k = 0; k < s.ui.prog_rows_count; ++k) {
            const unsigned at = s.ui.prog_rows_first + k;
            float y = zone::picture_y + 2 + k * (row_h + row_gap);
            bool cursor = at == s.ui.prog_cursor, own = int(at) == s.ui.prog_at;
            if (cursor) { p.fill(x, y, w, row_h, colour::raised, 3); p.fill(x, y, 2.5f, row_h, c, 1); }
            juce::String name = juce::String::fromUTF8(s.ui.prog_rows[k].data());
            float room = w - 20 - (own ? 20 : 0);
            p.text(name, x + 10, y, room, row_h, p.fit(name, plain(cursor ? colour::text : colour::sec, type::body, cursor ? Weight::Medium : Weight::Regular), room, type::floor));
            if (own) icon(p, Icon::Check, x + w - 20, y + 5, 12, c, 1.5f); // the file the track plays now
        }
        if (s.ui.prog_count > s.ui.prog_rows_count) { // more than the screen holds: where the rows in view sit in the list
            float top = zone::picture_y + 4, height = zone::picture_h - 8, bx = zone::width - 3;
            p.fill(bx, top, 2, height, colour::line, 1);
            p.fill(bx, top + height * float(s.ui.prog_rows_first) / float(s.ui.prog_count), 2,
                   std::max(8.0f, height * float(s.ui.prog_rows_count) / float(s.ui.prog_count)), colour::text, 1);
        }
        return;
    }
    if (s.ui.levels_sheet == levels_sheet_pad) {
        soft_keys(p, SoftKeys {{"BACK", "", ""}, -1, c, {KeyIcon::Back, KeyIcon::None, KeyIcon::None}});
        h.left.push_back(pill("SET: TAP A PAD", colour::text));
        header(p, h);
        chord_grid(p, s);
        return;
    }
    soft_keys(p, SoftKeys {{"BACK", "", "DONE"}, 2, c, {KeyIcon::Back, KeyIcon::None, KeyIcon::Check}});
    const unsigned index = s.ui.chord_index % prog_chords;
    auto name = prog_pad_name(track, index);
    juce::String chord = juce::String::fromUTF8(name.chord.c_str()) + (name.bass.empty() ? juce::String() : "/" + juce::String::fromUTF8(name.bass.c_str()));
    h.left.push_back(pill("PAD " + juce::String(index % 16 + 1) + (chord.isEmpty() ? juce::String() : "  " + chord), colour::text));
    header(p, h);
    chord_grid(p, s);
}

void push(Context& c) { if (c.track().used()) c.open(PageId::Edit); } // the engine's every page (Stage holds the quick ones)
void wheel(Context& c, int delta) { if (prog_pads(c.track()) && !c.ui.prog_storing) prog_wheel(c, delta); }
void soft(Context& c, int key) {
    if (c.ui.prog_storing) { if (key == 0) prog_store_back(c); return; }
    if (key == 1 && prog_can_store(c)) { prog_store_begin(c); return; }
    play_soft(c, key);
}
const PageSpec keys_page {draw, play_pad, wheel, push, soft, octave};

} // namespace melodee
