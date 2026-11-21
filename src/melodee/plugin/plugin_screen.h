// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 hdslave
// Melodee's plug-in screen (plugin/melodee_screen.h) as the shell and the pages see it: the picture a plug-in last
// painted, what the page tells it (the pots, Shift, the transport, the track's colour), and its inputs. The rack's
// screen thread does the painting (plugin/plugin_rack.h); the snapshot carries the newest picture to the page, shared
// rather than copied. No JUCE here.
#pragma once
#include "melodee/model/limits.h"
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace melodee {

struct PluginScreenFrame {
    static constexpr unsigned width = 320, height = 218, top = 22; // the screen below the soft keys
    uint8_t entry = no_plugin;     // whose picture (Project::plugins)
    uint32_t serial = 0;           // a new number each new picture
    std::vector<uint32_t> pixels;  // width x height, 0xAARRGGBB (alpha ignored)
};
using PluginScreenFramePtr = std::shared_ptr<const PluginScreenFrame>;

// What the page in view tells the plug-in drawing it.
struct PluginScreenContext {
    std::array<int16_t, 3> pots {-1, -1, -1}; // K1-K3: where each sits, 0..127, -1 not known
    std::array<bool, 3> engaged {};           // and whether it drives its value yet (takeover)
    bool shift = false, playing = false;
    uint32_t accent = 0xff808080;              // the track's colour
    bool operator==(const PluginScreenContext& o) const {
        return pots == o.pots && engaged == o.engaged && shift == o.shift && playing == o.playing && accent == o.accent;
    }
    bool operator!=(const PluginScreenContext& o) const { return !(*this == o); }
};

// The page's inputs it hands on, numbered as MELODEE_SCREEN_WHEEL, _PUSH and _NUDGE are (plugin/plugin_rack.cpp checks).
enum class PluginScreenKey : uint32_t { Wheel = 0, Push = 1, Nudge = 2 };
struct PluginScreenInput {
    uint32_t kind = 0;            // a PluginScreenKey
    int32_t value = 0;
    bool shift = false;
};

} // namespace melodee
