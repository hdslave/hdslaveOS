// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
//
// lo-fly's panel on the MPC Sample's screen in Melodee (sdk/melodee-screen/melodee_screen.h).
// Thirteen pages. Each has a picture, one control on the wheel and three on K1-K3.
//   Minus / Plus   previous / next page
//   wheel          the page's wheel control (SHIFT: finer steps)
//   Push           the page's section on or off
// Calls come from Melodee's screen thread only (never the audio thread).
#pragma once

#include "store.h"

#include <melodee_screen.h>

#include <array>
#include <cstdint>
#include <vector>

namespace lofly_melodee {

class Panel {
public:
    // `redraw` asks the host for another draw (an animation still moving); may be null.
    Panel(Store& store, void (*redraw)(void* context), void* context);

    bool draw(const melodee_screen_frame_t& frame);
    // True if it used the input; `changed` true if a parameter moved.
    bool input(const melodee_screen_input_t& in, bool& changed);
    // The CLAP parameter IDs K1-K3 turn on this page.
    void controls(std::uint32_t ids[3]) const;

    int page() const { return page_; }
    void set_page(int page);
    static int page_count();
    static const char* page_title(int page);

private:
    struct Knob {
        lofly::ParamId id;
        const char* label;
        bool used;
    };
    std::array<Knob, 3> knobs() const;
    std::uint64_t signature(const melodee_screen_frame_t& frame) const;
    void build_background(int area);
    void paint(const melodee_screen_frame_t& frame);

    Store& store_;
    void (*redraw_)(void*);
    void* context_;
    int page_ = 0;

    // What it drew last, and the background (grain, card) of the page and area it drew.
    std::uint64_t drawn_ = 0;
    int background_page_ = -1, background_area_ = -1;
    std::vector<std::uint32_t> background_;

    // Animation: the LOFI page's wobble line moves while lo-fly sounds or the host plays; its level falls back.
    std::uint64_t last_ms_ = 0;
    double spin_seconds_ = 0.0;
    float needle_ = 0.0f;
    bool animating_ = false;

    // Each built-in noise as min/max columns, made once (a texture is seconds long).
    std::array<std::array<std::array<float, 2>, 288>, 12> noise_columns_ {};
};

} // namespace lofly_melodee
