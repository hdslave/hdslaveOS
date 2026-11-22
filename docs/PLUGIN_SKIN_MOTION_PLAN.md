# Plug-in skins in motion: plan

Status: proposed 2026-11-22. Nothing built. Five picks open (end of this file).
Follows docs/PLUGIN_PAGES_PLAN.md (skins built b8ed56c). Still data only: a page
file says what moves and with what; no plug-in code runs.

## Today

- Skin controls follow their parameters (knob, switch and slider frames, values,
  curves). The scope and meters follow the plug-in's sound, measured by Melodee
  and read into the panel 20 times a second.
- Nothing moves on time, on the beat or bar, on Play and Stop, or on notes.
- A plug-in's read-only outputs (a compressor's gain reduction, a tuner's note)
  are hidden: the rack shows only parameters that can be automated.
- The screen draws about 10 frames a second when nothing changes (4.7 ms a frame
  on the MPC) and pushes only the rectangles that changed. A whole-screen push
  takes 56 ms, about 18 frames a second; a needle or a light costs little.

## Model

A control's value (0 to 1) can come from a `source` instead of its parameter:

| `source`     | 0 to 1                                                        |
|--------------|---------------------------------------------------------------|
| `param`      | its parameter (today's, the default)                          |
| `output`     | a read-only output of the plug-in, by `param` name or ID      |
| `level-in`   | the sound going in, measured by Melodee (effects)             |
| `level-out`  | the sound coming out                                          |
| `beat`       | where the song is within the beat: 0 on the beat, 1 just before the next |
| `bar`        | the same within the bar                                       |
| `playing`    | 1 while Melodee plays, else 0                                 |
| `note`       | the last note's velocity on the plug-in's track, falling to 0 over `decay` ms |
| `notes`      | 1 while any note is held on that track                        |
| `time`       | a loop, 0 to 1 every `period` ms                              |

- `invert`: 1 − the value (a beat light that is brightest on the beat).
- `decay` (ms): how fast `note`, `level-in` and `level-out` fall back
  (default 250).
- `while`: when a `time` loop runs: `playing` (default) or `always`. Stopped, it
  holds where it is (pick 2).

Two new kinds use a source:

- `rotate`: an image turned about its centre, by `degrees` × the value from
  `start` (a VU needle: `"source": "level-out"`, `"start": -45`, `"degrees":
  90`; a record: `"source": "time"`, `"period": 1800`, 33⅓ turns a minute).
- `glow`: an image drawn with the value as its opacity (a lamp, an LED).

Knob, switch and meter filmstrips take a `source` too (a five-LED meter strip on
`level-out`, a frame per step of a `bar` loop).

```json
{ "kind": "rotate", "image": "art/record.png", "x": 9, "y": 5, "source": "time", "period": 1800 },
{ "kind": "glow", "image": "art/beat-led.png", "x": 250, "y": 24, "source": "beat", "invert": true },
{ "kind": "glow", "image": "art/note-led.png", "x": 270, "y": 24, "source": "note", "decay": 300 },
{ "kind": "rotate", "image": "art/needle.png", "x": 120, "y": 40, "source": "level-out", "start": -45, "degrees": 90 },
{ "kind": "meter", "strip": "art/gr.png", "frames": 12, "x": 40, "y": 70, "source": "output", "param": "Gain Reduction" }
```

## How Melodee draws it

- A skin page with a moving control draws at up to the rate of pick 1 while it
  is in view; other pages keep today's rate. Only the moving controls' boxes
  change, so only those rectangles go to the panel.
- `beat`, `bar` and `playing` come from the song position and tempo the screen
  already has; `note` and `notes` from the notes the track plays (pick 3);
  `level-in` and `level-out` from the panel's measured levels, read every frame
  while a moving page is in view instead of 20 times a second; `output` from the
  rack with the other values.
- Falling values (`decay`) and loops are kept by the screen per control, from
  the frame's time.
- Off with Settings > SCREEN > Plug-in skins (pick 5).

## Steps

**1. Format.** `source`, `invert`, `decay`, `while`, `period`, `start`,
`degrees`, kinds `rotate` and `glow`; tests.

**2. Values.** The rack reads read-only outputs a skin names (pick 4); the panel
carries the level every frame while a moving page is in view; the screen keeps
each control's falling value and loop position.

**3. Drawing.** `rotate` and `glow`; sources for filmstrips; the frame rate while a
moving page is shown; only moving boxes pushed.

**4. Examples.** Melodee Poly's skin gets a beat light, a note light and an
output meter; a Neural Amp Modeler strip (its name is known: NeuralAmpModeler)
with a VU needle and a pilot lamp on the sound going in. Previews of several
moments; checked on the MPC playing a pattern.

**5. Docs.** The skin section of docs/PLUGIN_PAGES.md.

## Picks

1. Frame rate while a moving page is in view.
   a. 20 a second: about 9 % of one core drawing. (recommended)
   b. 30 a second: about 14 %; full-screen motion still tops out near 18.
   c. 15 a second.
2. Time loops (a spinning record) when Melodee is stopped.
   a. The file's `while` decides; `playing` by default, so pages are calm when
      stopped. (recommended)
   b. Always stopped when Melodee is stopped, whatever the file says.
3. Which notes `note` and `notes` follow.
   a. Every note the track plays: the sequencer's, pads and MIDI in.
      (recommended)
   b. Only notes played live (pads, MIDI in).
4. Plug-ins' read-only outputs.
   a. A skin may show them (`output`): gain reduction, tuners, LFO lamps a
      plug-in publishes. (recommended)
   b. Only what Melodee measures (levels in and out).
5. A setting for motion.
   a. Plug-in skins covers it: skins move when they are on. (recommended)
   b. A row of its own, Settings > SCREEN > Skin motion.
