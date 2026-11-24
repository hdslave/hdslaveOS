# Mixer: plan

Status: design approved 2026-11-24, then eight stereo submixes added. All
decisions made the same day (end of this file). Core step 1 (model and
files) built: core e971012. Core step 2 (the strip's sound,
`engine/channel_strip.cpp`) built: core 99ed66f, then written for the A55
with NEON: core 84ff289. Measured on the MPC (2026-11-24, 48 kHz, 128-frame
blocks, `--melodee-dsp-bench strips=30 strip=full` / `strip=usual`, one
thread at FIFO 30 beside the running app), 99ed66f then 84ff289:

| 30 strips | 99ed66f | 84ff289 |
|---|---|---|
| all nine bands, compressor, trim, width, gain | 731 µs a block, 27.4 % of one core | 256 µs, 9.6 % (0.32 % a strip) |
| three bands each, compressor on every other | 176 µs, 6.6 % | 94 µs, 3.5 % (0.12 % a strip) |

By part, 30 strips at 84ff289: nine bands 5.7 %, three bands 2.1 %, one band
1.4 %, compressor 3.3 %, trim 0.9 %, width 0.9 % (99ed66f: 18.2 %, 3.7 %,
1.8 %, 6.4 %, 2.0 %, 2.1 %). A band at 0 dB costs nothing. What the A55
needed (measured): a multiply by one lane of a register issues once a cycle,
by a whole register twice; a 128-bit load takes two cycles.

Core step 3 (tracks) built: core dc021e9. Every track plays through its strip
and sounds as before while the strip is flat; nothing sets a strip yet but a
project file (the page is Melodee step 1). Checked: core and Melodee host
tests, melodee-move's sources compile as before, the build runs on the MPC
with a project loaded (no xruns). Owed: old projects with effects on a track
heard by ear (the chain no longer hears the level and pan).

Core step 4 (submixes) built: core cd52b2b. A project's submixes and track
outputs play as the file has them; nothing in Melodee sets them yet. Checked
as step 3, plus a test of delay compensation through a late plug-in on a
submix chain.

Core step 5 (buses, input, master) built: core 3ef6a4f. Every channel of the
mix now has its strip in the engine; flat, each sounds as before. Checked as
step 3; on the MPC the idle render load is as before.

Core step 6 (Motion) built: core d3e0218. A pattern's lanes move a track's
strip; nothing in Melodee records or shows them yet (Melodee step 3: SEQ
RECORD, and the Motion page's names and cards).

Core step 7 (freeze, export) built: core a1e01dd and 94b74cb. Prints, stems
and the MPC 3 export's routes and submixes are done. The exports carry no
strips yet and say so.

## What it is

One page, opened with **PAD FX** + **KNOB FX**. It replaces Tracks, Sends and
the mixer's key row on the effect chains.

- The channels sit in a row. The selected channel is a wide card in the
  centre; its neighbours are narrow strips left and right.
- A narrow strip shows the channel's number, its stereo meter and its fader.
- The card shows the channel's name, mute and solo, a slim meter, the fader,
  the pan, and one block: sends, EQ or compressor. **B1**–**B3** choose the
  block. **K1**–**K3** set it.
- The screen never changes page while you mix.

## Channels

| Channel | Count | Has |
| --- | --- | --- |
| Track | 16 | strip, effect chain, level, pan, mute, solo, sends A–D, output |
| Submix | 8, stereo | strip, effect chain, level, pan, mute, solo, sends A–D |
| Bus (return) | 4, A–D | strip, effect chain, return level, mute |
| Input | 1 | strip, trim, monitor, record dry or wet |
| Master | 1 | strip without a compressor of its own, effect chain, the Compressor |

A track's output is the master or one submix. A submix plays to the master,
never into another submix. Solo on a submix solos the tracks that go to it.

Colours: tracks keep theirs. Submixes get eight colours of their own, also
shown on the header of a track routed to one. Buses keep the Looper's four.
The input has no mute or solo, so its card shows neither.

The Looper's L1–L4 and a kit's pads are not channels of this page. They keep
their own pages (Looper MIX, Stage on a kit).

### The strip

Every channel gets the same fixed strip, in the engine, after the channel's
effect chain: chain, strip, level and pan. A frozen track keeps its strip
live, as it keeps level, pan and sends.

- **Trim**, ±24 dB.
- **EQ**, nine fixed bands: 63, 125, 250, 500 Hz, 1, 2, 4, 8, 16 kHz, ±12 dB
  each. A band at 0 dB costs nothing.
- **Compressor**: amount, ratio, speed; key (sidechain), mix, gain.
- **Width**.

The EQ and the compressor each have an on/off.

## Controls

| Control | Does |
| --- | --- |
| **PAD FX** + **KNOB FX** | Opens the mixer. Again: closes it. |
| Pads | Select the channel, no sound. A pad is lit in its channel's colour, as bright as its level. |
| **PAD BANK** | Pads and screen: tracks 1–16, or submixes (pads 1–8), buses A–D (9–12), input (15), master (16). |
| Fader | Level of the selected channel, always. |
| Encoder | Turn: the next channel comes to the centre. Push: the lit block on or off. Held: the Menu. |
| **−** / **+** | Previous / next channel. **SHIFT**: undo / redo. |
| **B1** | **SENDS** on a track or submix. On a bus or the master: the name of its first effect. On the input: **INPUT**. |
| **B2** | **EQ**. Again: the next three bands (LOW, MID, HIGH). |
| **B3** | **COMP**. On the master: the Compressor (**SHIFT** + **Pad 5**), same settings. |
| **K1**–**K3** | The lit block. **SHIFT**: its second row. |
| **MUTE** | As printed. Held or latched: a pad mutes its channel, **SHIFT** + pad solos. **SHIFT** + **MUTE**: unmute all. |
| **ERASE** | Held + a knob or the fader: that value back. Alone: the lit block flat. **SHIFT** + **ERASE**: copy the block; pads paste. |
| **SEQ RECORD** | On: knob, fader and mute moves are recorded. |
| **SHIFT** + **KNOB FX** | The channel's effect chain, as today. |

Knobs by block:

| Block | K1 | K2 | K3 | SHIFT + K1 | SHIFT + K2 | SHIFT + K3 |
| --- | --- | --- | --- | --- | --- | --- |
| SENDS | Send A | Send B | Send C | Pan | Output | Send D |
| EQ (per group) | band 1 | band 2 | band 3 | – | – | – |
| COMP | Amount | Ratio | Speed | Key | Mix | Gain |
| Bus, master: effect | its first row | | | its second row | | |
| INPUT | Trim | Monitor | Record | – | – | – |
| Master COMP | Attack | Release | Amount | – | – | In Boost |

Width is in the Menu.

## What goes away, and where it went

| Today | After |
| --- | --- |
| Tracks page (`pages/tracks_page.cpp`, `pages/track_grid.cpp`) | The mixer. |
| Sends page (`pages/sends_page.cpp`) | The SENDS block. Its pad view goes; a kit's pad sends stay on Stage **SENDS**. |
| Mixer row TRACKS / SENDS / FX (`shell/mix_hub.h`, `mix_keys` in `pages/common.cpp`) | Removed. The effect chains keep their page and get a way back to the mixer. |
| New track on an empty track's pad | Same: selecting an empty track shows its doors. |
| Name, move, copy, clear, freeze, Looper | Menu (they are there today) and **PAD BANK** held. |
| Track speed (**SHIFT** + **K1**) | A value on Stage. |
| CPU, track load | The Menu's header. |
| Pattern number per track | Dropped: SEQ shows it. |
| Stage's MIX page | Removed. |
| Stage's SENDS page | Removed on every track but a kit. On a kit it stays, for the pads' own sends only; **SHIFT** + **K1** KIT SEND goes (the kit's send is the mixer's). |
| Effect chain keys TRACKS / SENDS / FX | **B1** MIXER (back to this channel), **B2** empty, **B3** FX as today. |
| Menu tiles MIXER, SENDS, EFFECTS (`shell/menu.h:55`) | MIXER and EFFECTS. The SENDS tile is free. |
| Lesson "Mix a track" (`shell/lesson.h:82`) | Rewritten for the new page. |

## Build

Two repositories. The strip, the submixes and the meters are ellic-core
(`core/`), shared with melodee-move. The page is here.

Today all mixing is one function, `EngineHost::render()`
(`core/src/melodee/engine/engine_host.cpp`): a track's level and pan are
applied before its chain (`chain_of`), tracks are summed in order with mute,
solo and sends, then the bus chains, the returns, the input, the master chain
and the Compressor. Meters are one peak a track; buses have none.

### Core

1. **Model and files.** `Strip` (trim, width, nine gains, EQ on, compressor
   values, compressor on). `Submix` (name, `Mix`, `Strip`, `Chain`).
   `Mix::out` (0 master, 1–8 a submix). `sub_count = 8` in `model/limits.h`.
   New file sections that old builds skip, as SNDS and PSND do
   (`model/project_file.cpp`). A project with no such section loads flat, all
   to the master. Tests: write, read, old file.
2. **Strip DSP.** One class beside `engine/effects.cpp`, built on
   `engine/rbj.h`: nine peak filters a side, the compressor of the Compressor
   effect, width, trim, with eases against clicks. Measure it on the MPC with
   `dsp_bench` before going on: 30 channels with every band and the
   compressor on, and the usual case (a few bands a channel).
3. **Tracks.** Built (core dc021e9). A track is now: engine, chain, Post
   FX print, strip, punches, Pad FX, delay compensation, then level and pan
   last. Before, level and pan came first, so an overdriven chain hears a
   turned-down track differently now. A print is taken before the level and
   pan and `print_mix()` is unity and centred for every track: a print holds
   neither, and its project hashes as before except for a track with effects
   (it kept its level and pan before), whose prints are printed again the
   first time it is frozen or loaded. The Track input source (post) is the
   track as heard (after level and pan). Commands: `StripValue` (track = strip
   number, index = `StripPart`, model/strip.h `strip_set` clamps). A strip's
   key goes through `plan_keys` like an effect's (a track keyed by another
   waits for it). `EngineHost::strip_meter(n)`: each side's peak and the gain
   reduction, falling as `meter()` does.
4. **Submixes.** Built (core cd52b2b).
   - A track whose output is a submix is summed into the submix's buffers in
     place of the master. A change of output crosses over in one block.
   - Each submix that plays or still sounds runs, after the Looper's mix: its
     chain, strip, level and pan. With two or more, they run on the workers.
     Then mute, its sends into the buses, and into the master, all before the
     buses' run.
   - Commands: `SubMix` (index `SubLevel`, `SubPan`, `SubMute`, `SubSolo`,
     `SubSend0` + bus) and `TrackOut`.
   - Solo: a submix's solo solos its tracks. It counts only while a track that
     plays something goes into it. A track's own solo keeps its submix heard.
   - Chains: submix chains are 25-32 (`sub_chain`, `mix_chain_count`); kit pad
     chains moved to start at 33. `chain_count` stays 25 for melodee-move.
   - Keys: a submix is key 21-28 (`fx_key_sub`). Files and the engine take keys
     below `fx_key_count`. `fx_keys` (21) stays the pickers' range until a
     page offers submixes.
   - A bus or master chain hears a submix in the same block; a track or submix
     hears it a block late. A submix's strip or chain hears a track in the same
     block.
   - Delay compensation: no new lines. A track's latency counts its submix
     chain's, so a track going straight to the master waits for the slowest
     submix through its own line. Left: a routed track's own sends and its Knob
     FX share are that submix chain's latency early.
   - The master Sidechain's key track, sent to a submix, goes through it and is
     ducked with it.
   - Owed in Melodee, when a page edits submix chains: shell code that takes
     `n >= chain_count` for a kit pad's chain (`context.cpp` keys,
     `bindings.cpp` KEY row, `plugin_jobs.cpp`, `plugin/plugin_sleep.h`,
     `track_copy.cpp`, `track_swap.h`) has to count chains 25-32 as submixes.
5. **Buses, input, master.** Built (core 3ef6a4f).
   - A bus: its chain, then its strip (keyed as its chain is), then its
     return. A bus whose chain is empty but whose strip works runs in the
     buses' run too. New: a bus's mute (`Bus::mute`, a byte at the end of
     BUSS that older builds do not read; command `BusMute`) eases its return
     out.
   - The master: its strip before its chain, its compressor held off (the
     master's compressor is the Compressor; the engine sets a master strip's
     amount to 0).
   - The input: its strip on the source after its gain and Rec Input FX.
     Monitor always hears it after the strip. Takes, Audio Recall and the input
     meter hear it after the strip when `Project::input_wet` (file section
     INWT, command `InputWet`), else before. Its key is heard as the master's
     chain hears one.
   - Meters (`strip_meter`): a bus after its return and mute; the master after
     its chain and the Compressor, before main volume, with the Compressor's
     reduction; the input after its strip.
6. **Motion.** Built (core d3e0218).
   - Sixteen lanes from 206 (`motion_strip_base`, model/strip.h): trim,
     width, the nine bands, amount, ratio, speed, mix, gain. The two switches
     and the key have none. `motion_strip(part)` and `strip_lane_part(lane)`
     map between them; `is_strip_lane()`.
   - Strip lanes are not `is_mix_lane()`: the shells read that range by
     position (a lane from 202 on is a return in Melodee's bindings and
     modulators, and in melodee-move). Prints drop strip lanes as they drop
     mix lanes; `lane_own()` reads the track's strip.
   - The engine keeps each track's strip as its commands set it
     (`strip_set_by`). A lane moves the strip away from it, and a lane's end
     or Stop puts it back, gliding. A modulator's swing moves it without
     marking it moved.
   - Files: a pattern's strip points go in MOTS, the same records as MOTK,
     which older builds skip. They would have taken 206 and up for a kit
     pad's values.
   - `ChannelStrip::set()` works out the speeds again only when the speed
     changes, so a value Motion sets often costs no sines or powers.
7. **Freeze, export.** Built (core a1e01dd, 94b74cb).
   - Prints: never with the strip (decision 1, done in step 3): a frozen track
     keeps its strip, level and pan live.
   - Stems: 28 (`StemRing::stem_count`). Tracks 0-15 as they leave for the
     master or their submix, buses 16-19, submixes 20-27 as they join the
     master. Melodee's mixdown with stems adds a submix a used track plays
     into, named after it (else "Submix N"). A routed track's stem and its
     submix's both hold it: each is that point of the mix. The bounce is the
     master and needs nothing.
   - MPC 3 export (94b74cb): MPC 3 routes a channel with `mixable.audioRoute`
     {destination, audioRouteSubIndex}: 1 a submix (index 0-7), 2 an output
     pair (0 Out 1/2); a pad's 0 is its program. Learnt from one MPC 3 save
     (`~/Documents/MPC 3/submixtest.xpj`, Track 1 to Submix 1). A track sent
     to a submix goes to MPC 3's. Each submix a track plays into gets its
     level, pan, mute and solo, on its bus track and in `data.mixer.submixes`,
     as MPC 3 keeps both. Drum pads now go into their program (0): the
     template's 1, from an imported MPC Sample project, meant Submix 1, past
     the track's channel. Tool changes for tests: `O:T=S` (a track's output),
     `U:S=LEVEL,PAN,MUTE`. Checked: the export run on the MPC, read back by
     `test_submix_routes`, and opened in MPC 3 (2026-11-24): Track 1 on
     Submix 3.
   - Left out, and counted (`mixer_left_out()`, `report.mixer`; Melodee's
     export notice adds "mixer settings left out"): strips (MPC 3 and Live),
     a submix's name, sends and chain, bus mutes; in Live also the routes and
     submixes (group tracks, later).
8. **melodee-move.** Its mixer (`src/app/screens/mixer.cpp`) reads `Mix`. It
   must build and behave as before after each core step.

### Melodee

1. **Page.** `PageId::Mixer` (`shell/ui_state.h`, a name in
   `shell/usage_log.h`), a `PageSpec` in `pages/mixer_page.cpp`, state for
   bank, channel, block and EQ group. The opener in `shell/shell.cpp`
   (`fx_chord`) opens it. Draw the row, the card and the five block pictures
   with the widgets in `ui/`. Previews in `tools.cpp` for each state in the
   mock.
2. **Knobs.** New `Binding` kinds for the strip values, output and width
   (`shell/bindings.h`); the page's rows in `base_bindings` and
   `shift_bindings`; labels in `describe_card`; commands in
   `Context::apply`; the Mix undo scope. Pickup works as on every page.
3. **Other controls.** Pads select (`pad_job`, pad chip SELECT), the pad
   lights by level (`shell/pad_lights.cpp`, after `tracks_look`), **PAD
   BANK** tap for the second bank, mute mode for submixes and buses
   (`shell/mute_mode.cpp`), the fader's page job (`shell/fader_job.cpp:113`),
   encoder turn, push and **−** / **+** in the `PageSpec`.
4. **Bus, input, master cards.** A bus's knobs are its first effect's
   bindings (`page_chain`). The input's are Input Config's. The master's are
   the Compressor page's.
5. **EQ picture.** The engine hands out the selected channel's sound in a
   ring; the screen thread makes the spectrum.
6. **Take out the old.** Tracks and Sends pages and their previews, Stage's
   MIX page and its SENDS page on every track but a kit (`stage_page.cpp`,
   `bindings.cpp` 550-566), the mixer
   row, the Menu tiles, the pad chip and lights of Tracks, the page-stack
   group, the controls audit rules (`shell/controls_rules.h`), Erase and Copy
   cases (`shell/erase_copy.h`), the lesson, Basic/Full gates
   (`shell/features.h`).
7. **Tests and docs.** `tests/melodee.cpp` (bindings, button_remap,
   fader_and_takeover, pad_lights, erase_copy_everywhere, track_move,
   pad_sends_mix, controls_audit). Guide chapters "Mixer and effects" and
   "Tracks", the shortcut tables, QUICKSTART step 3, MELODEE.md, new screen
   images.

Order: core 1–3 and Melodee 1–3 give a working mixer for tracks. Then core 4
with the submix cards, core 5 with Melodee 4, then the rest.

## Decisions (2026-11-24)

1. **Strip after the effect chain.** A frozen track keeps EQ and compressor
   live.
2. **A submix plays to the master only.**
3. **Basic:** the mixer with SENDS showing send A only. No EQ, COMP or
   submixes.
4. **Stage's MIX page is removed; SENDS stays on kits only,** for the
   pads' own sends (`Binding::PadSend`). It is their only page once the Sends
   page's pad view goes.
5. **Track speed to Stage, CPU and track load to the Menu's header, pattern
   number dropped.**
6. **Submixes get eight colours of their own;** buses keep the Looper's four;
   the input card shows no mute or solo.
7. **Motion records track strips only** in the first version. Submix, bus and
   master strips do not record.
8. **Effect chain keys:** B1 MIXER, B2 empty, B3 FX.
