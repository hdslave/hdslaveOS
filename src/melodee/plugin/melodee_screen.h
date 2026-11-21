/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 hdslave
 *
 * Melodee's plug-in screen: a CLAP extension through which a plug-in draws its own page on the MPC Sample's screen
 * in Melodee (melodee-hdslave), and takes the encoder and Minus / Plus there. A plug-in that offers it is shown on
 * its own page (Edit, and Stage's plug-in page) instead of Melodee's list of its parameters; in any other host it
 * is simply never asked. This header is MIT licensed, apart from the rest of Melodee, so that any plug-in may copy
 * it.
 *
 * The page: the whole screen below Melodee's soft keys, 320 x 218 pixels (the screen's rows 22 to 239). Melodee
 * keeps the soft keys (B1-B3 do what they do on the page) and draws its own overlays over the plug-in's picture:
 * the MENU, notices and questions, and the place bar, a 3-pixel strip over the canvas's first two rows (or, as
 * Settings may have it, its last three): keep nothing there that must be seen. While SHIFT is held Melodee draws
 * nothing over the page; the canvas says so. The panel shows 16-bit colour (RGB565): what the
 * plug-in draws in 32 bits loses its lowest bits there, so smooth gradients want dithering. A picture that changes
 * all over runs at about 18 frames a second at most (the panel's SPI link); small changes are quick.
 *
 * K1-K3: the plug-in names the three parameters they turn (knobs()), and Melodee binds them as it binds any
 * plug-in's: takeover, ERASE + pot (the default), Shift + wheel fine steps, the fader and motion recording all work.
 * The canvas says where each pot sits and whether it drives its value yet, for the plug-in's own knob cards.
 *
 * Threads: paint(), input() and knobs() are main-thread calls (CLAP's sense: never the audio thread), made from a
 * thread of Melodee's own that only calls when the plug-in's other main-thread calls are not running; they should
 * take well under a millisecond or two. request_paint() may be called from any thread.
 */
#pragma once

#include <clap/clap.h>

#ifdef __cplusplus
extern "C" {
#endif

static const char CLAP_EXT_MELODEE_SCREEN[] = "com.hdslave.melodee.screen/1";
static const char CLAP_EXT_MELODEE_SCREEN_HOST[] = "com.hdslave.melodee.screen-host/1";

enum
{
    MELODEE_SCREEN_WIDTH = 320,
    MELODEE_SCREEN_HEIGHT = 218,
};

typedef struct melodee_screen_knob
{
    int16_t position; /* where the pot sits, 0..127; -1 until it is known (not moved since Melodee started) */
    bool engaged;     /* it drives its value now; false: it has yet to meet the value (takeover) */
} melodee_screen_knob_t;

typedef struct melodee_screen_canvas
{
    uint32_t* pixels;  /* 0xAARRGGBB, row after row; alpha is ignored (the picture is opaque) */
    uint32_t width;    /* MELODEE_SCREEN_WIDTH */
    uint32_t height;   /* MELODEE_SCREEN_HEIGHT */
    uint32_t stride;   /* pixels from one row to the next */
    uint64_t now_ms;   /* Melodee's clock, for animation */
    uint32_t accent;   /* the track's colour, 0xFFRRGGBB */
    melodee_screen_knob_t knobs[3]; /* K1-K3 */
    bool shift;        /* SHIFT is held */
    bool playing;      /* the transport plays */
    bool whole;        /* the canvas does not hold this plug-in's last picture (its first paint, or it came back into
                        * view after another plug-in's): draw every pixel, and return true */
} melodee_screen_canvas_t;

enum
{
    MELODEE_SCREEN_WHEEL = 0, /* the encoder turned: value is the detents, clockwise positive */
    MELODEE_SCREEN_PUSH = 1,  /* the encoder pushed and let go (a tap; held, it opens Melodee's MENU instead) */
    MELODEE_SCREEN_NUDGE = 2, /* Minus (value -1) or Plus (+1) */
};

typedef struct melodee_screen_input
{
    uint32_t kind; /* MELODEE_SCREEN_WHEEL, _PUSH, _NUDGE */
    int32_t value;
    bool shift;    /* SHIFT is held */
} melodee_screen_input_t;

typedef struct clap_plugin_melodee_screen
{
    /* [main-thread] Draws the page. The canvas holds the picture this plug-in last drew, so it may draw only what
     * changed, unless canvas->whole says to draw it all. Returns false if nothing changed since the last call, true
     * if it drew a new picture. */
    bool(CLAP_ABI* paint) (const clap_plugin_t* plugin, const melodee_screen_canvas_t* canvas);

    /* [main-thread] An input on the page. Returns true if the plug-in used it. */
    bool(CLAP_ABI* input) (const clap_plugin_t* plugin, const melodee_screen_input_t* input);

    /* [main-thread] The parameters K1-K3 turn now: three clap_ids, CLAP_INVALID_ID for a pot that turns nothing.
     * Asked after each input and each paint. */
    void(CLAP_ABI* knobs) (const clap_plugin_t* plugin, clap_id ids[3]);
} clap_plugin_melodee_screen_t;

typedef struct clap_host_melodee_screen
{
    /* [thread-safe] The plug-in's picture changed (a value, a meter, an animation's next frame): paint() soon. */
    void(CLAP_ABI* request_paint) (const clap_host_t* host);
} clap_host_melodee_screen_t;

#ifdef __cplusplus
}
#endif
