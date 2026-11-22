# Plug-in pages: plan

Status: all picks made 2026-11-22: 1 a, 2 (own pages, then groups), 3 c, 4 a (end
of this file). Steps 1-4 built 2026-11-22 (step 4: skins, example in docs/examples/plugin-skin) (step 2: a page shows several pictures'
parts, so it keeps them all and draws the one its row in hand turns most of; step 3: the
format reference is docs/PLUGIN_PAGES.md). Kerem then asked for the plug-in's own GUI
window in a browser at melodee.local: a plan of its own, parked on keep/plugin-windows.
Mockups: https://claude.ai/artifact/TCsNHN1KRY76cWyhUEMyYR; files in
build/plugin-pages-mock: sheet-s1.png (level 0), sheet-s6.png (skins),
sheet-s2.png, sheet-s3.png, sheet-s7.png, sheet-s8.png (picks 1-4), sheet-s9.png
(level 0 with every pick applied);
mocks.html and skins.js draw them; not in Git.

Scope: level 0 (Melodee's own page, built from what the plug-in already
reports) and level 1 (a page file a plug-in maker, Melodee or a user writes),
including a maker's **skin**: its own art, knobs, sliders and font, from data
files, no plug-in code. Level 2 (the plug-in draws with its own code) has
its own plan, docs/PLUGIN_SCREENS_PLAN.md; the skin keeps a `canvas` control
kind reserved for it.

## Today

- Edit and Stage show a plug-in as a list of its parameters, six rows, the
  row of three on K1-K3 raised (pages/plugin_view.cpp). The FX chain shows the
  plug glyph and page dots only (pages/fx_drawing.cpp).
- Order: the plug-in's own, with two exceptions: an Open MPC skin's Q-Link
  pages first (qlink_order, plugin/plugin_files.cpp) and Neural Amp Modeler's
  hand-written order and short names (plugin/nam.cpp).
- Groups are ignored. JUCE already builds a parameter tree from VST3 units and
  LV2 port groups (setHostedParameterTree in its VST3 and LV2 hosts); the rack
  reads the flat getParameters() list. Our CLAP host does not read
  `clap_param_info.module` or the `remote-controls` extension
  (plugin/clap_instance.cpp, readParameters).
- PR 34 (draft): a text file beside an Open MPC VST2's plugin-meta.xml; accent,
  three value readouts and a measured stereo scope.
- Plug-in parameter turns are not motion-recorded (shell/bindings.cpp), so the
  binding id `plugin_binding(entry, shown)` is never saved. Reordering
  parameters into pages breaks no project.

## Model

A plug-in has a list of **pages**. A page has a name, its parameters (rows of
three on K1-K3) and at most one **picture** drawn above the cards.

Pages come from the first source that has them:

1. A page file in the card's `Melodee/Plugin Pages/` (the user's or the
   community's; any file name, matched by the IDs inside).
2. A page file in the plug-in's package (the maker's).
3. A page file in Melodee's own library (built into the binary).
4. CLAP `remote-controls` pages.
5. An Open MPC skin's Q-Link pages.
6. Parameter groups: VST3 units, LV2 port groups, CLAP `module` paths.
7. None: the plug-in's list in rows of three, as today, on one page.

Pick 2, as changed 2026-11-22: the plug-in's own pages (sources 4-5) first,
then, where it has parameter groups, a page per group for the parameters its
own pages leave out (MAIN for those outside every group). Surge XT's
remote-controls pages hold 36 of its 775 parameters; with its groups after
them all 775 are on 19 pages. A plug-in with own pages and no groups (an Open
MPC VST2) shows only its own pages. With no own pages: group pages (MAIN
first for parameters outside every group), else its list on one page.

Melodee always keeps B1-B3, the MENU, notices, questions, the place bar and
Shift. K1-K3 always go through the plug-in parameter path: takeover, Erase +
pot, Shift + wheel steps, the fader and undo work on every page from every
source.

## Pictures

Drawn by Melodee, in its fonts and the palette's colours, at the 11 px floor.

| Kind       | Bound to                                        | Draws                                               |
|------------|-------------------------------------------------|-----------------------------------------------------|
| `cards`    | the page's parameters                           | today's list (the default)                          |
| `envelope` | attack, decay, sustain, release (hold optional) | the sampler's envelope (draw_envelope)              |
| `filter`   | cutoff, resonance, type (optional)              | the sampler's filter curve (draw_filter_curve)      |
| `eq`       | per band: frequency, gain, Q (type optional)    | the summed response                                 |
| `scope`    | nothing (measured)                              | the plug-in's output, left and right                |
| `meter`    | nothing (measured)                              | output level; an effect's in and out, the gap marked |
| `value`    | one to three parameters                         | large value readouts (PR 34's)                      |
| `canvas`   | reserved for level 2                            | `cards` until then                                  |

Times and frequencies come from the parameter's value text ("12.5 ms",
"1.2 kHz"); a text that does not read as one falls back to the normalised
value on a log scale.

Without a page file Melodee picks `envelope` or `filter` when one page holds
the matching names (attack/decay/sustain/release; cutoff with resonance).
Everything else is Melodee's generic page (pick 4 a).

**Frame (picks 3 c and 4 a).** Every plug-in page, skinned or not, uses
everything under B1-B3 (320 x 218): a page line (page name, dots), then the
page. Melodee's header (tempo, the plug-in's CPU share, the track chip) and the
K1-K3 cards are not drawn on plug-in pages. Melodee's generic page shows the
page's parameters as arcs in rows of three, each column over its pot, the row
in hand raised in the knob colours; choices show their steps as dots, two-step
switches the same. A page with a picture (`envelope`, `filter`, `eq`, `scope`)
draws it on top and the row in hand as arcs under it. The arcs carry what the
cards showed: a faint arc with a white dot where the pot sits until it picks
the value up, the fader glyph, the Shift mark.

Measured pictures read output peaks the audio thread writes for the one
plug-in on screen (PR 34's CustomGuiScope, written only while the UI marks
that plug-in as watched).

## Page file

JSON, read with JUCE's parser on the rack's thread at load. Up to 64 KB, 64
pages. Example:

```json
{
  "melodee-pages": 1,
  "plugin": { "clap": "org.surge-synth-team.surge-xt", "vst3": "…", "lv2": "…" },
  "pages": [
    { "name": "FILTER 1",
      "params": ["Filter 1 Cutoff", "Filter 1 Resonance", { "name": "Filter 1 Type", "label": "TYPE" }],
      "picture": { "kind": "filter", "cutoff": "Filter 1 Cutoff", "resonance": "Filter 1 Resonance", "type": "Filter 1 Type" } },
    { "name": "AMP",
      "params": ["Amp EG Attack", "Amp EG Decay", "Amp EG Sustain", "Amp EG Release"],
      "picture": { "kind": "envelope", "attack": "Amp EG Attack", "decay": "Amp EG Decay",
                   "sustain": "Amp EG Sustain", "release": "Amp EG Release" } }
  ]
}
```

- `plugin`: the IDs it matches, any of `clap` (plug-in id), `vst2`
  (four-character unique ID; Open MPC plug-ins too), `lv2` (URI), or `name`
  (with `vendor`, where given). VST3 goes by name: JUCE gives a VST3 class ID
  only hashed, and DPF's and NAM's bundles carry no moduleinfo.json (built:
  page_file.h).
- A parameter is its name as the plug-in gives it (trimmed, exact), or
  `{ "id": … }` with the format's own ID (CLAP param id, VST3 ParamID, VST2
  index, LV2 symbol; JUCE's HostedParameter::getParameterID()). Names make one
  file work across formats; IDs survive a renamed parameter.
- `label`: the card's name for it, when the plug-in's is too long. This
  replaces nam_card_name.
- Unknown keys and picture kinds are skipped; a parameter that does not
  resolve is dropped from its page. A file that does not parse, matches no
  loaded parameter, or has a version above 1 is skipped for the next source.
  The reason goes to `Melodee/Plugin Pages/problems.txt` (one line per file,
  rewritten at each load).

Where a maker puts it:

| Format   | Place                                             |
|----------|---------------------------------------------------|
| VST3     | `<bundle>.vst3/Contents/Resources/melodee-pages.json` |
| LV2      | `<bundle>.lv2/melodee-pages.json`                 |
| CLAP     | `<name>.melodee-pages.json` beside `<name>.clap`  |
| VST2     | `<name>.melodee-pages.json` beside `<name>.so`    |
| Open MPC | `melodee-pages.json` beside `plugin-meta.xml`     |

Other hosts never read the file; the plug-in binary is the same everywhere.
The format reference (docs/PLUGIN_PAGES.md) is MIT licensed so makers and
other hosts can copy it.

## Skins

A page file may add a `skin`: the maker's art for its pages, drawn by Melodee
from images and data. The same plug-in binary; no code runs.

```json
"skin": {
  "font": "fonts/panel.ttf",
  "pages": {
    "FILTER": {
      "background": "art/filter.png",
      "controls": [
        { "param": "Cutoff", "kind": "knob", "strip": "art/knob-cream.png", "frames": 128, "x": 136, "y": 22, "size": 28,
          "label": { "text": "CUTOFF", "x": 150, "y": 76, "colour": "ECE3CF", "size": 11 } },
        { "param": "Key Track", "kind": "switch", "strip": "art/slide3.png", "frames": 3, "x": 148, "y": 101 },
        { "kind": "curve", "of": "filter", "x": 8, "y": 22, "w": 104, "h": 108, "colour": "FFB347", "glow": true },
        { "kind": "scope", "x": 124, "y": 21, "w": 186, "h": 16, "colour": "E9BF89" }
      ]
    }
  }
}
```

- A skin covers everything under B1-B3, 320 x 218 (pick 3): Melodee's
  header (tempo, CPU, track) and the K1-K3 cards are not drawn on a skinned
  page. Coordinates are pixels inside that area. An effect's skin in a chain
  takes the strip under the chain row, 320 x 92.
- `background`: a PNG the area's size. Pages without their own skin page use
  Melodee's.
- Control kinds:
  - `knob`: a filmstrip, frames stacked top to bottom, the frame picked by
    the value (the format Open MPC skins use: Melodee Poly's
    `sh_knob_r40.png` is 128 frames of 90 x 90).
  - `switch`: a filmstrip, one frame per step.
  - `slider`: a track image and a cap image.
  - `meter`: a filmstrip for a measured level.
  - `value`: the parameter's text in the skin's font.
  - `image`: a picture with no parameter.
  - `curve` and `scope`: Melodee's drawings (filter, envelope, EQ, output) in
    the maker's colours.
  - `canvas`: reserved for level 2.
- `label` text and `value` use the skin's `font` (TTF or OTF in the package)
  at 11 px and up; smaller sizes are drawn at 11.
- Melodee always draws, over the skin: a ring in the knob shade round each
  control K1-K3 turn (from the control's box), the page dots, B1-B3, the
  MENU, notices and questions. The rings carry what the cards showed: bright
  and thick while turning; faint with a white dot where the pot sits until
  the pot picks the value up; the fader glyph when the fader drives it; the
  Shift mark where Shift changes it.
- Limits: images PNG, together at most 4 MB decoded per plug-in, loaded on
  the rack's thread at load and freed with the plug-in. The panel shows 16-bit
  colour: art with smooth gradients wants dithering.
- Open MPC skins (`Plugin Skins/TUI.json` and its PNGs) are drawn for
  1280 x 628, four times the MPC's width; their text would be 3-4 px here,
  so Melodee does not show them. A page file may reuse their filmstrips.

Settings > SCREEN gets **Plug-in skins: On / Off**. Off: Melodee's own pages
(level 0 and the page file's pages and pictures) for every plug-in.

## Steps

Each step builds, passes the host tests and runs on the MPC before the next.

**1. Pages in the rack (no new screen).**
- plugin/plugin_pages.h/.cpp: `PluginPages` (pages of parameter indices, names,
  picture kind and bindings), built at load from sources 4-7.
- ClapInstance: read `module` into a parameter tree (setHostedParameterTree);
  read `remote-controls` pages, and follow its `changed()` call.
- The rack reads JUCE's tree for VST3 and LV2. `shown` becomes the pages
  flattened, so bindings and set_param stay as they are.
- PluginPanel: page name, page and page count, row within the page.
- Host tests (tests/clap_host.cpp, clap_fixture.cpp): a fixture with modules
  and remote-controls pages; a VST3 with units; Surge XT on the MPC.

**2. The page on screen.**
- plugin_view.cpp: the frame under B1-B3 on Edit and Stage's plug-in page
  (no header, no cards): page line, arcs over the pots, pictures above the
  row in hand; Minus / Plus turn pages, the wheel moves rows. The pickup dot,
  fader glyph and Shift mark on the arcs (ui/knobs.cpp's card states). The FX
  chain shows the page name and the picture in its smaller area.
- Measured `scope` and `meter` for every plug-in (CustomGuiScope from PR 34,
  gated by the watched entry); automatic `envelope` and `filter`.
- Previews: Surge XT, NAM, Second Spin, an LV2 with groups, a VST2 with no
  pages.

**3. Page files.**
- Parser, sources 1-3, ID matching, name and ID references, `label`,
  `value` and `eq` pictures, problems.txt.
- Library: NAM's file (nam.cpp order and names move into it). Built with
  NAM only: Second Spin's needs its name or VST2 ID as Melodee reads them, and
  Surge XT and Melodee Poly already get good pages from their own data.
- Also built: `"rest": true` (every parameter no page names), the `scope`
  picture, the EQ's scale following its largest gain.
- docs/PLUGIN_PAGES.md (format reference) and a section in
  docs/guide/plug-ins.md.

**4. Skins.**
- Image and font loading (JUCE PNG decoder, Typeface from the TTF's data) on
  the rack's thread; the 4 MB limit; filmstrip knob, switch, slider, meter,
  value, image; curves and scope in the skin's colours; the K1-K3 rings.
- The skin's area (everything under B1-B3) and the chain's strip for effects
  (320 x 92); the ring states.
- Settings > SCREEN > Plug-in skins.
- A skin for Second Spin (PR 34's plug-in) and one of Melodee's own plug-ins,
  to prove the format; previews of both, with Plug-in skins Off.
- docs/PLUGIN_PAGES.md: the skin section, with a filmstrip how-to.

**Later.** The web page's Plug-ins tab lists each loaded plug-in's page source
and problems, and takes a page file upload into `Melodee/Plugin Pages/`.
Level 2: docs/PLUGIN_SCREENS_PLAN.md.

## PR 34

Its scope code is taken into step 2 and its descriptor becomes Second Spin's
page file in step 3. The reply on the PR is shown to Kerem before anything is
posted.

## Picks

Made 2026-11-22:

1. Page turning: **a**. Minus / Plus turn pages; the wheel moves rows and
   runs on into the next page at the ends.
2. Parameters no page lists: **b**, hidden; changed the same day, after
   Surge XT's remote-controls pages turned out to hold 36 of 775: own pages
   first, then the plug-in's groups for the rest (hidden only without groups).
3. How much of the screen a skin may take: **c**, everything under B1-B3
   (320 x 218); no header or cards on a skinned page; the rings carry the
   cards' marks.

4. Melodee's own page for a plug-in with no skin (Kerem asked for a generic
   design; sheet-s8.png): **a**, arcs over the pots in the same frame as a
   skin. The other designs were rows with gauges, and small arcs with the
   header and cards kept.
