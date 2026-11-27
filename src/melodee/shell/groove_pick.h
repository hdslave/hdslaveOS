// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include "melodee/model/pattern.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

// Time Correct's groove on the encoder (pages/time_correct_page.cpp): none first, then the track's patterns that hold
// notes (`used`, a bit each), the pattern in hand (`here`) left out; a turn steps along them, stopping at the ends.
// A groove taken from another track stays first in the list until a turn leaves it. Pure, for the page and the tests.
namespace melodee {

inline std::vector<uint16_t> groove_choices(const std::array<uint64_t, 2>& used, unsigned track, unsigned here, uint16_t from) {
    std::vector<uint16_t> out {0};
    if (from && groove_track(from) != track % track_count) out.push_back(from); // another track's, kept where it is
    for (unsigned n = 0; n < pattern_count; ++n)
        if (n != here % pattern_count && ((used[(n / 64) % 2] >> (n % 64)) & 1)) out.push_back(groove_source(track, n));
    if (from && std::find(out.begin(), out.end(), from) == out.end()) { out.push_back(from); std::sort(out.begin() + 1, out.end()); }
    return out;
}
inline uint16_t groove_step(const std::array<uint64_t, 2>& used, unsigned track, unsigned here, uint16_t from, int delta) {
    const auto list = groove_choices(used, track, here, from);
    const int at = int(std::find(list.begin(), list.end(), from) - list.begin());
    return list[size_t(std::clamp(at + delta, 0, int(list.size()) - 1))];
}

} // namespace melodee
