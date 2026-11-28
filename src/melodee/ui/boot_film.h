// SPDX-License-Identifier: AGPL-3.0-only
// hdslave_boot_slide.mkv 0-2.25 s at 1.5x, hdslave_intro.ogv 5.25-7.75 s at 2.46x, 0253.png 0-1.4 s at 1x; grade purple, VHS outro 1 s; 30 fps, zoom 1.1, JPEG quality 4, hold 0 ms.
// Written by tools/boot_film.py; run it again rather than editing this file.
#pragma once
#include <cstdint>

// The boot film (ui/boot.cpp): 320 x 240 frames as Base64 JPEG, shown one after another at `fps`; the last
// stays `hold_ms` before the film fades over the app. The frames themselves are not covered by the AGPL
// (boot_film*.cpp; THIRD_PARTY.md, Boot film).
namespace melodee::ui::boot_film {
constexpr unsigned count = 117, fps = 30;
constexpr uint32_t length_ms = 3900, hold_ms = 0; // length: the last frame's end
extern const char* const frames[count];
} // namespace melodee::ui::boot_film
