// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/project.h"
#include "melodee/shell/features.h"
#include "melodee/shell/ui_state.h"
#include <array>

// The mixer's channels (docs/MIXER_PLAN.md, Controls): Pad Bank steps between two rows. The first is the sixteen
// tracks, a pad each. The second holds the submixes on pads 1-8, the buses A-D on 9-12, the input on 15 and the
// master on 16 (13 and 14 hold nothing); Basic leaves the submixes out (decision 3). A channel is named by its strip
// number (model/project.h: tracks 0-15, buses 16-19, the master 20, the input 21, the submixes 22-29). Pure, for the
// page, the shell, the pad lights and the tests.
namespace melodee {

enum class ChannelKind : uint8_t { Track, Submix, Bus, Input, Master };

constexpr ChannelKind channel_kind(unsigned strip) {
    return strip < track_count ? ChannelKind::Track : strip < master_chain ? ChannelKind::Bus : strip == master_chain ? ChannelKind::Master
         : strip == strip_input ? ChannelKind::Input : ChannelKind::Submix;
}
constexpr unsigned channel_bus(unsigned strip) { return (strip - bus_chain(0)) % bus_count; }
constexpr unsigned channel_sub(unsigned strip) { return (strip - strip_sub(0)) % sub_count; }

// The second row, left to right: S1-S8, A-D, IN, MST (as the pads hold them).
constexpr unsigned other_count = sub_count + bus_count + 2;
constexpr std::array<uint8_t, other_count> other_row {
    uint8_t(strip_sub(0)), uint8_t(strip_sub(1)), uint8_t(strip_sub(2)), uint8_t(strip_sub(3)),
    uint8_t(strip_sub(4)), uint8_t(strip_sub(5)), uint8_t(strip_sub(6)), uint8_t(strip_sub(7)),
    uint8_t(bus_chain(0)), uint8_t(bus_chain(1)), uint8_t(bus_chain(2)), uint8_t(bus_chain(3)),
    uint8_t(strip_input), uint8_t(master_chain)};
static_assert(sub_count == 8 && bus_count == 4, "the second row's pads");
// Where Basic's second row starts: no submixes.
inline unsigned other_first(const UiState& ui) { return full_features(ui) ? 0 : sub_count; }

// The pad (0-15) of a channel in the second row, or -1 (a track).
constexpr int other_pad(unsigned strip) {
    switch (channel_kind(strip)) {
    case ChannelKind::Submix: return int(channel_sub(strip));
    case ChannelKind::Bus: return int(sub_count + channel_bus(strip));
    case ChannelKind::Input: return 14;
    case ChannelKind::Master: return 15;
    default: return -1;
    }
}
// The channel a pad holds in the second row, or -1 (pads 13 and 14; the submixes in Basic).
inline int pad_other(const UiState& ui, unsigned pad) {
    pad %= 16;
    if (pad < sub_count) return full_features(ui) ? int(strip_sub(pad)) : -1;
    if (pad < sub_count + bus_count) return int(bus_chain(pad - sub_count));
    if (pad == 14) return int(strip_input);
    if (pad == 15) return int(master_chain);
    return -1;
}
// The place in other_row of a strip (0 when it is none of them).
constexpr unsigned other_place(unsigned strip) {
    for (unsigned n = 0; n < other_count; ++n) if (other_row[n] == strip) return n;
    return 0;
}

// The channel in hand: the selected track, or the second row's.
inline unsigned mixer_strip(const UiState& ui) {
    if (ui.mixer_bank == 0) return ui.track % track_count;
    return other_row[std::max<unsigned>(ui.mixer_other % other_count, other_first(ui))];
}
// How many channels the row in view holds, and the place of the one in hand in it.
inline unsigned mixer_row_count(const UiState& ui) { return ui.mixer_bank == 0 ? track_count : other_count - other_first(ui); }
inline unsigned mixer_row_at(const UiState& ui) {
    return ui.mixer_bank == 0 ? ui.track % track_count : other_place(mixer_strip(ui)) - other_first(ui);
}
// The strip at place `n` of the row in view.
inline unsigned mixer_row_strip(const UiState& ui, unsigned n) {
    return ui.mixer_bank == 0 ? n % track_count : other_row[(other_first(ui) + n) % other_count];
}
// The second row's channel `strip` in hand (the first row's go through select_track: the selected track).
inline void pick_other(UiState& ui, unsigned strip) { ui.mixer_bank = 1; ui.mixer_other = uint8_t(other_place(strip)); }

// What a channel has (the plan's table): sends, pan and an output (tracks; submixes play to the master), mute, solo.
constexpr bool channel_sends(ChannelKind k) { return k == ChannelKind::Track || k == ChannelKind::Submix; }
constexpr bool channel_mutes(ChannelKind k) { return k == ChannelKind::Track || k == ChannelKind::Submix || k == ChannelKind::Bus; }
constexpr bool channel_solos(ChannelKind k) { return k == ChannelKind::Track || k == ChannelKind::Submix; }
// The master's compressor is the printed Compressor (Melodee step 4), never its strip's.
constexpr bool channel_comp(ChannelKind k) { return k != ChannelKind::Master; }
// A channel's effect chain by its number (model/project.h), or -1: the input has none.
constexpr int channel_chain(unsigned strip) {
    switch (channel_kind(strip)) {
    case ChannelKind::Track: return int(strip % track_count);
    case ChannelKind::Bus: return int(bus_chain(channel_bus(strip)));
    case ChannelKind::Master: return int(master_chain);
    case ChannelKind::Submix: return int(sub_chain(channel_sub(strip)));
    default: return -1;
    }
}
// B1 on a channel: a track or submix has two pages (SENDS, FX); a bus or the master only FX; the input none yet
// (its INPUT block comes with Melodee step 4).
constexpr bool channel_fx(ChannelKind k) { return k != ChannelKind::Input; }

// The mixer's blocks: what the card shows and K1-K3 turn.
enum : unsigned { block_sends = 0, block_eq = 1, block_comp = 2, block_fx = 3 };
// The block for the channel in hand: B1 lit is SENDS or FX (as `mixer_b1` left it; a bus or the master FX alone),
// B2 EQ, B3 COMP. A key the channel has nothing for shows its EQ (the input's B1, the master's B3 until step 4).
// Basic: SENDS only.
inline unsigned mixer_block_of(const UiState& ui, ChannelKind k) {
    if (!full_features(ui)) return block_sends;
    const unsigned b = ui.mixer_block % 3;
    if (b == 0) {
        if (channel_sends(k)) return ui.mixer_b1 ? block_fx : block_sends;
        return channel_fx(k) ? block_fx : block_eq;
    }
    if (b == 2 && !channel_comp(k)) return block_eq;
    return b;
}
// The FX block lit, on the channel in hand.
inline bool mixer_fx_lit(const UiState& ui) { return ui.page == PageId::Mixer && mixer_block_of(ui, channel_kind(mixer_strip(ui))) == block_fx; }

} // namespace melodee
