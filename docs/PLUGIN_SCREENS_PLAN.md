# Plug-in screens: plan

Status: built 2026-11-22/23, steps 1-5. All picks made (end of this file); mocks on
the plug-in pages artifact.
Level 2 of docs/PLUGIN_PAGES_PLAN.md: a plug-in draws its own page on the MPC
with its own code. Its maker ships the plug-in; Melodee needs no change for it.
CLAP and VST2 first; VST3 and LV2 later.

## Today

- A plug-in's page is Melodee's: its own pages or groups, arcs over the pots,
  pictures (steps 1-2), or a maker's page file and skin of images (steps 3-4).
- Nothing in a plug-in draws on the MPC's screen. Editors never open
  (plugin_support's no-op X11 and GL).

## The format

One MIT header, `sdk/melodee-screen/melodee_screen.h`, plain C with fixed-width
types. It needs neither the CLAP nor the VST2 headers, so a plug-in of either
format includes it as it is. A plug-in that offers a screen still loads in any
other host: the call is never made there.

**The frame** Melodee hands the plug-in to draw into:

| Field          | What                                                         |
|----------------|--------------------------------------------------------------|
| `pixels`       | 32-bit pixels, 0x00RRGGBB, row after row                      |
| `width`, `height`, `stride` | 320 × 218 (the page) or 320 × 144 (a strip, pick 2) |
| `area`         | page or strip                                                 |
| `redraw_all`   | the frame does not hold this plug-in's last picture: draw all |
| `time_ms`      | Melodee's clock, for animation                                |
| `colour`       | the track's colour                                            |
| `playing`, `shift` | the transport plays; SHIFT is held                        |
| `pots[3]`, `pots_live[3]` | where K1-K3 sit (0-127, -1 not yet known); whether each drives its value yet |

**Calls the plug-in answers**, made by Melodee's screen thread, never the
audio thread and never at the same time as the plug-in's other non-audio
calls; each should take under 2 ms:

- `draw(frame)`: draw the changes (the frame keeps the last picture); returns
  whether anything changed.
- `input(kind, value, shift)`: the wheel (detents), Push (a tap; held is
  Melodee's MENU), Minus / Plus (−1, +1). Returns whether it used it.
- `controls(ids[3])`: the parameters K1-K3 turn now, in the format's own IDs
  (CLAP param id, VST2 index), none allowed. Asked after each input and draw.

**Call the plug-in makes**: `redraw`, from any thread: its picture changed (a
value, a meter, the next frame of an animation). It may also ask to be run
while Melodee lets it sleep, for an input its audio side must take.

**Per format**:

| Format | The plug-in offers it                                   | `redraw`                                      |
|--------|---------------------------------------------------------|-----------------------------------------------|
| CLAP   | `get_extension("melodee.screen/1")`: a table of the three calls | the host extension `"melodee.screen-host/1"` |
| VST2   | `effVendorSpecific` (50) with code `'MLSC'` returns the table; `audioMasterCanDo("melodeeScreen")` tells it Melodee takes it | `audioMasterVendorSpecific` (35) with `'MLSC'` |

## How Melodee shows it

- The rack asks each loaded plug-in for the screen. Its screen thread starts
  while a page with a screen is in view, owns the frame and calls `draw` when
  the plug-in asks, after an input, and when the pots, Shift, Play or the
  track's colour change, at most every 16 ms. A call that would wait for the
  plug-in's other calls comes back on the next tick, so a slow preset load
  never holds the picture.
- The newest picture goes to the screen in the Snapshot, shared, never in
  UiState (it stays memcpy-safe). The panel shows 16-bit colour; changed
  rectangles are pushed, so a needle costs little and a picture that changes
  all over tops out near 18 frames a second.
- Edit on a PLUGIN track and Stage's EDIT show it in place of the pages or
  skin once it has drawn. B1-B3, the MENU, notices and questions stay
  Melodee's, drawn over it.
- K1-K3 turn the three `controls` names, bound as any plug-in parameter
  (takeover, Erase + pot, the fader, Shift + wheel fine steps, motion
  recording).
- Each control is drawn once. On a plug-in's page or strip Melodee draws no
  rings and no K1-K3 cards: the plug-in draws its knobs, and its own pickup
  marks from `pots` and `pots_live` (pick 1 a).
- Order: the plug-in's screen, else a skin, else Melodee's pages.
- VST2 plug-ins in the 32-bit bridge: the frame lives in a second shared block
  with its own request and reply words, served by a second bridge thread, so a
  draw never holds the audio thread's channel.

## Steps

**1. Format and CLAP.** The header; ClapInstance asks for the screen and
offers `redraw`; the rack's screen thread and the Snapshot's picture; Edit and
Stage show it; inputs and K1-K3. Tests: the CLAP fixture draws a page; the
host test checks its pixels, inputs and controls. A preview tool,
`--melodee-plugin-screen PLUGIN DIR steps`, opens one and writes the screen as
PNGs after each step (wheel, Push, Minus / Plus, Shift, pots), for makers
without an MPC at hand.

Built: the header, ClapInstance, the rack's screen thread (it starts with the
first screen in view), the Snapshot's picture, Edit and Stage's EDIT, inputs,
K1-K3 bound as `plugin_screen_slot + k`, Shift's layer kept off the page, the
preview tool. Checked on the MPC with the CLAP fixture on an empty track: its
page under EDIT, the wheel and K1 moving its bar, the page drawn again after
leaving and coming back; then the track undone and the file removed.

**2. VST2.** Vst2Instance: the vendor call, canDo and `redraw`. A VST2 test
fixture (there is none today) built from vst2_abi.h; tests.

Built: CLAP and VST2 instances share one interface the rack's screen thread
calls (plugin/screen_source.h). Melodee asks a VST2 plug-in for its table only
after it answers effCanDo("melodeeScreen") with 1, so one that answers every
vendor call is never trusted. Its non-audio dispatcher calls now share one
lock, which the screen calls only try. The preview tool opens VST2 .so files.

**3. The 32-bit bridge.** The second shared block and bridge thread. Checked
with an armv7 build of the fixture on the MPC.

Built: bridge::ScreenShared, a second memfd passed to vst2_bridge32 at start;
the bridge serves it on a thread of normal priority, under a priority
inheriting lock it shares with the plug-in's other non-audio calls (busy: the
host tries again a tick later). The plug-in draws straight into the block; the
host copies a new picture out. tests/vst2_host.cpp runs through the bridge when
given its folder: passed on the MPC with an armv7 fixture, and in Melodee on an
empty track (its page under EDIT, the wheel and K1).

**4. Chain, pages, setting and skins.** Picks 2-5: effects in a chain,
Melodee's pages one press away, Settings, skins drawn without doubles.

Built: shell/screen_view.h decides which screen is in view, for the shell and
the pages alike. Stage's B1 has PANEL (core StageKind::PluginScreen) between
PRESETS and EDIT, passed over for plug-ins without a screen; Edit's
Shift + B3 swaps the screen and Melodee's pages. An effect with a screen draws
320 x 144 under the chain's slots with no cards; Push opens its full page
under the hub's keys (B3 or Shift + Push back) and Shift + B3 opens a slot's
cards on any effect (Kerem's pick: the cards had been on Push). Settings >
SCREEN > Plug-in screens covers screens and skins. Skins: no rings, a pickup
dot and the fader and Shift marks only while they apply, the strip 320 x 144
with no cards. Checked on the MPC with the CLAP fixture: PANEL, EDIT, the
strip, the full page with the wheel, B3 back, Shift + B3 cards.

**5. Example and docs.** `sdk/melodee-screen/example`: a small MIT plug-in,
one source built as CLAP and as VST2 (aarch64, and armv7 for the bridge),
drawing its own page and strip. docs/PLUGIN_SCREENS.md (MIT): the format
reference. A guide chapter. Checked on the MPC.

Built: Screen Example, a drive effect (Drive, Tone, Mix) in one C++ file, one
library with both entry points (named .clap or .so) and an armv7 build; MIT,
with the header, a LICENSE and a README in sdk/melodee-screen.
docs/PLUGIN_SCREENS.md opens with what a maker needs: no special build, the
extension in the plug-in is enough. The guide's Plug-ins chapter has "A
plug-in's own screen". tests/test_plugin_hosts.py builds the example and
draws it through --melodee-plugin-screen as CLAP and VST2. On the MPC: its
strip's meters moving with Melodee Poly's sound on a spare track, its full
page, and the armv7 build through the bridge in the next slot.

**Later.** VST3: an interface of Melodee's own on the edit controller (JUCE's
host hands out only the component: a small JUCE change). LV2:
`extension_data` with a Melodee URI (JUCE's LV2 host hides the instance: a
JUCE change or Melodee's own LV2 instance). Helpers for plug-in frameworks.

## Picks

1. K1-K3 on a plug-in's screen. **Picked a** (Kerem, 2026-11-22: b and c show
   each control twice).
   a. The plug-in draws everything, its own pickup marks from `pots`.
   b. Melodee's rings round the plug-in's controls.
   c. Melodee's K1-K3 cards along the bottom.
2. Effects in a track's FX chain. **Picked a.**
   a. The plug-in draws everything under the chain's slots, its knobs once
      (320 × 144, no K1-K3 cards); Push opens its full page. (recommended)
   b. Melodee's own strip and K1-K3 cards, as today; its full page from a
      PANEL card in the slot's sheet.
3. Melodee's pages for a plug-in with its own screen. **Picked a.**
   a. One press away: on Stage, B1's cycle goes PRESETS, PANEL (its
      screen), EDIT (Melodee's pages of every parameter); on Edit, a free
      Shift + B key swaps the two. (recommended)
   b. None: its screen is its only page.
4. Setting. **Picked a.**
   a. Settings > SCREEN > Plug-in skins becomes Plug-in screens and covers
      both. (recommended)
   b. A row of its own.
5. **Picked a.** Skins (built b8ed56c) show the same doubles: Melodee's ring round each
   of the maker's knobs, K1-K3 cards under a skin's strip.
   a. Kept, each control drawn once: no rings; a white dot where the pot sits
      only while it has not reached its value, the fader and Shift marks only
      while they apply; a skin's strip runs to the bottom (320 × 144) with no
      cards. (recommended)
   b. Kept as built.
   c. Taken out; page files keep their pages and pictures.
