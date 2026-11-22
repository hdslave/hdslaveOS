// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/shell/mute_mode.h"
#include "melodee/shell/looper_jobs.h"
#include "melodee/shell/context.h"
#include "melodee/engine/engine_host.h"

namespace melodee {

void mute_button(Context& c, bool pressed) {
    auto& ui = c.ui;
    if (pressed) {
        if (ui.sample_held && !ui.shift) { // Sample + Mute: the light show on or off; Mute's release does nothing
            ui.sample_used = true;
            ui.light_show = !ui.light_show;
            c.notice(ui.light_show ? "Light show on" : "Light show off");
            return;
        }
        if (ui.shift) { ui.shift_used = true; unmute_all(c); return; } // its release changes nothing: Mute was not held
        ui.mute = true; ui.mute_down = c.now; ui.mute_used = false;
        return;
    }
    if (!ui.mute) return;
    ui.mute = false;
    // A tap turns the latch on or off; a hold (or one that muted something) leaves it as it was.
    if (!ui.mute_used && c.now - ui.mute_down <= mute_hold_ms) ui.mute_latch = !ui.mute_latch;
}

bool pad_muted_now(const Context& c, unsigned t, unsigned kit) {
    t %= track_count;
    int position = c.host.playing() ? c.host.position_of(t) : -1;
    return pad_muted_now(c.project.tracks[t], c.patterns.at[t][c.host.pattern_of(t) % pattern_count], position, kit);
}

// The mute as set, and as heard now (a mute lane may have moved it, issue 32). With Seq Record on, the selected
// track's pattern keeps it on the pad's mute lane, where it plays.
void set_pad_mute(Context& c, unsigned t, unsigned kit, bool on) {
    t %= track_count; kit %= sampler_pads;
    auto& track = c.project.tracks[t];
    if (track.pad_muted(kit) == on && pad_muted_now(c, t, kit) == on) return;
    track.set_pad_muted(kit, on);
    c.send({Command::Type::PadMute, uint8_t(t), 0, uint16_t(kit), int16_t(on)});
    if (t == c.ui.track % track_count) c.record_motion(motion_lane(motion_pad_mute, kit), on);
}

void mute_pad(Context& c, unsigned pad, bool kit_pads) {
    if (c.ui.page == PageId::Looper) { loop_mute_pad(c, pad); return; } // the loop tracks, a column each
    auto& ui = c.ui;
    ui.mute_used = true;
    pad %= 16;
    if (!mute_on_kit(ui, kit_pads)) { // the tracks
        const auto& mix = c.project.tracks[pad].mix;
        if (ui.shift) { ui.shift_used = true; c.set_solo(pad, !mix.solo); }
        else c.set_mute(pad, !mix.mute);
        return;
    }
    auto& t = c.track();
    unsigned kit = t.bank % sampler_banks * 16 + pad;
    if (!t.pads[kit].used()) { c.notice(pad_text("Empty", kit)); return; }
    if (!ui.shift) { set_pad_mute(c, ui.track, kit, !pad_muted_now(c, ui.track, kit)); return; } // as heard now (issue 32)
    // Shift: solo the pad (every other used pad muted); on the soloed pad, the kit back.
    ui.shift_used = true;
    bool alone = pad_soloed(t, kit);
    for (unsigned n = 0; n < sampler_pads; ++n)
        if (t.pads[n].used()) set_pad_mute(c, ui.track, n, alone ? false : n != kit);
    c.notice(alone ? std::string("Solo off") : pad_text("Solo", kit));
}

void unmute_all(Context& c) {
    for (unsigned t = 0; t < track_count; ++t) {
        auto& track = c.project.tracks[t];
        if (track.mix.mute) c.set_mute(t, false);
        if (track.mix.solo) c.set_solo(t, false);
        if (track.any_pad_muted() || track.sampled()) { // a kit's: a mute lane may have muted a pad (issue 32)
            if (t == c.ui.track % track_count) // Seq Record keeps the unmutes on the selected kit's lanes
                for (unsigned pad = 0; pad < sampler_pads; ++pad) if (pad_muted_now(c, t, pad)) c.record_motion(motion_lane(motion_pad_mute, pad), 0);
            track.pad_mutes = {};
            c.send({Command::Type::PadMute, uint8_t(t), 0, 0xffff, 0});
        }
    }
    c.notice("Unmute all");
}

} // namespace melodee
