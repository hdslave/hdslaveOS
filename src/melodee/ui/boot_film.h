// SPDX-License-Identifier: AGPL-3.0-only
// hdslave_intro.ogv 0-4 s at 2.5x, hdslave_intro.ogv 4-7.74 s at 1.5x, hdslave_intro_blowaway.mkv 0-1.8 s at 3x, hdslave_intro_blowaway.mkv 1.8-5.3 s at 1.25x; 30 fps, zoom 1.1, JPEG quality 4, hold 100 ms.
// Written by tools/boot_film.py; run it again rather than editing this file.
#pragma once
#include <cstdint>

// The boot film (ui/boot.cpp): 320 x 240 frames as Base64 JPEG, shown one after another at `fps`; the last
// stays `hold_ms` before the film fades over the app. The frames themselves are not covered by the AGPL
// (boot_film*.cpp; THIRD_PARTY.md, Boot film).
namespace melodee::ui::boot_film {
constexpr unsigned count = 224, fps = 30;
constexpr uint32_t length_ms = 7466, hold_ms = 100; // length: the last frame's end
extern const char* const frames[count];
} // namespace melodee::ui::boot_film
