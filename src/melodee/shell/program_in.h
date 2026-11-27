// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/limits.h"
#include "melodee/model/midi_config.h"
#include "melodee/model/project.h"
#include <array>
#include <cstdint>

// Receive program change: Plug-in (MidiConfig::program_change, program_plugin). A Program Change picks a preset by its
// place in the plug-in's list (PRESETS' order, from 0), the same for VST3, VST2, CLAP, LV2 and MPC plug-ins: the host
// loads it (PluginRack::select_program), so a plug-in that ignores MIDI program changes takes it too. Plain decisions
// the MIDI thread acts on (shell/midi_io.cpp) and the host test checks.
namespace melodee {

// The Bank Select CCs heard on each channel (CC 0 the MSB, CC 32 the LSB: bank = MSB x 128 + LSB, as the MIDI spec
// counts it; 0 until one comes) and the place in the list a Program Change then asks for: bank x 128 + its number.
class ProgramBanks {
public:
    void control(unsigned channel, unsigned cc, unsigned value) { // channel 1..16; other CCs change nothing
        if (channel < 1 || channel > 16 || (cc != 0 && cc != 32)) return;
        bank[channel - 1][cc == 0 ? 0 : 1] = uint8_t(value & 0x7f);
    }
    unsigned place(unsigned channel, unsigned number) const {
        if (channel < 1 || channel > 16) return number & 0x7f;
        const auto& b = bank[channel - 1];
        return (unsigned(b[0]) * 128 + b[1]) * 128 + (number & 0x7f);
    }
private:
    std::array<std::array<uint8_t, 2>, 16> bank {};
};

// The plug-in entries a Program Change reaches, a bit an entry: the plug-in of each PLUGIN track in `tracks` (a bit a
// track, note_tracks()), once however many tracks share it. A frozen track (`frozen`, a bit a track) keeps the sound
// its print holds, so its plug-in is left as it is.
inline uint32_t program_entries(const Project& p, uint16_t tracks, uint32_t frozen) {
    static_assert(max_plugins <= 32, "a bit an entry");
    uint32_t entries = 0;
    for (unsigned t = 0; t < track_count; ++t) {
        const auto& tr = p.tracks[t];
        if (!((tracks >> t) & 1) || ((frozen >> t) & 1) || tr.engine != EngineKind::Plugin) continue;
        if (tr.plugin < max_plugins && p.plugins[tr.plugin].used()) entries |= 1u << tr.plugin;
    }
    return entries;
}

// Remote preset CC (MIDI Config): presets a step a turn, + the next, - the previous, from one CC value in `mode`
// (knob_mode_name()). Absolute: one step towards where the knob went since `last` (-1: no value yet, nothing), so a
// knob at its end stops; the relative modes as many steps as the knob says.
inline int knob_steps(uint8_t mode, unsigned value, int last) {
    const int v = int(value & 0x7f);
    switch (mode) {
    case knob_binary_offset: return v - 64;
    case knob_twos_complement: return v < 64 ? v : v - 128;
    case knob_signed_bit: return v & 64 ? -(v & 63) : v & 63;
    case knob_signed_bit_2: return v & 64 ? v & 63 : -(v & 63);
    default: return last < 0 || v == last ? 0 : v > last ? 1 : -1;
    }
}

// Learn on a Remote CC row: the CC (number and channel) a control sends, taken from the first CCs that come; the
// preset row waits for `learn_values` of one control and guesses how it sends from them, as it is turned right.
// Rising values: Absolute; the same value again (or one going round): relative, 64 and up Binary offset, below it
// 2's complement. A control that also fits another relative mode shows the guess; the row's K1 changes it. Bank Select,
// the channel modes and data entry are never taken.
constexpr unsigned learn_values = 3;
constexpr uint32_t learn_ms = 10000; // how long Push on the row waits for a control
inline bool learnable_cc(unsigned cc) { return cc != 0 && cc != 32 && cc != 6 && cc != 38 && !(cc >= 96 && cc <= 101) && cc < 120; }
inline uint8_t guess_knob(const std::array<uint8_t, learn_values>& v) {
    bool rising = true;
    for (unsigned n = 1; n < learn_values; ++n) rising = rising && v[n] > v[n - 1];
    if (rising) return knob_absolute;
    return v[0] >= 64 ? knob_binary_offset : knob_twos_complement;
}

} // namespace melodee
