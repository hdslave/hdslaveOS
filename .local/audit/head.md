# Melodee optimization audit

A performance and battery-life audit of Melodee 0.5.1 for the Akai MPC Sample, and the record of what the `melodee-hdslave` fork changes as a result. Written on 2026-11-20, in three passes: an audit by reading; a second pass for speed alone, measured with a profiler; and a third for what a stopped project and the screen cost.

Melodee is the work of Kerem Kiliç (Ellic Studio). This document covers his app and the sample-base runtime under it (`base/`). The fork keeps his copyright headers, credits and licence.

**Nothing here has been run on an MPC Sample.** Every cost and saving is an estimate from reading the code or a figure worked out from the hardware's published operating points, unless a line says it was measured. What was measured, on the build machine, is that the fork still builds, passes every test and plays the same audio to the bit, and, in the second pass, how many instructions the engine runs for a benchmark project before and after each change.

## Summary

- **128 findings**: 127 from eleven auditors, each covering one part of the code, and one found while implementing. I checked each against the code. 95 are confirmed, 27 turned out to be the same issue reported by a second auditor, 5 are partly confirmed and 1 could not be checked without the compiler's output. None was rejected outright, but one recommended compiler flag turned out to change the audio and was dropped (BASE-5).
- **The processor's speed is left alone.** The first pass built a CPU speed setting that held the cores at a lower clock and voltage to save battery. It was removed at the owner's request before the fork was ever run: the four cores stay at the kernel's 1.8 GHz, as under the stock app, and nothing in the fork touches the clocks.
- **The fork implements 25 findings**, nine of them in part. They remove wasted work in the engine and around it and add three settings (Refresh, Dim after, Pad brightness). The audio output is unchanged: the self-test render and the DSP bench's output are bit-identical to the original build's.
- **Second pass, for speed:** the engine runs **23 % fewer instructions** for the benchmark's busy 16-track project and **12 % fewer** for the same project stopped, playing the same to the bit. Most of it comes from one loop in the sampler.
- **Third pass, silent chains:** a chain of effects that hears nothing and holds nothing is no longer run (HOST-1). Once a stopped project's chains rest, a block costs the engine **76 % fewer instructions**. The output is the same to the bit with resting on and off, through silence and the sound that ends it.
- **Third pass, the screen:** the screen thread no longer copies the kits and redraws every pattern's sketch for frames in which neither can have changed (MODEL-1). With the MPC's cache sizes that takes its snapshot from about 3.4 million cycles a frame to 0.75 million, and the pads wait on its lock for that much less time. Every page gives the same pixels.

### The levers, in order of likely value

| # | Lever | Finding | In the fork |
| --- | --- | --- | --- |
| 1 | The kernel holds all four cores at 1.8 GHz and 1.10 V at all times. At 1.416 GHz the supply is 0.90 V, where the same work takes about a third less switching energy. | BASE-1 | Not used. Built, then removed at the owner's request: full speed always |
| 2 | Any changed pixel bought 30 more full redraws at 60 fps. The place bar moves on every page while playing, so every page was redrawn 60 times a second. | BASE-2, UI-1, UI-4 | Refresh setting (30 fps by default), shorter tail, event-driven wake |
| 2a | Every frame copied 0.8 MB of project and rebuilt 2,048 pattern sketches under the lock the pads use. | MODEL-1 | Third pass: only when a kit or pattern can have changed |
| 3 | Nothing dims: backlight at its level and 16 RGB pads plus about 25 button lights on for hours. | BASE-3, LIGHT-4 | Dim after and Pad brightness settings |
| 4 | Silent tracks, buses and the master still run every effect in their chains on every block. | HOST-1 | Third pass: chains that hold nothing rest; silent tracks and buses are not mixed |
| 4a | Half of a busy project's engine time is the sampler's plain voice loop, most of it for voices that only hold their level. | SMP-9 | Fixed in the second pass: 17 % of the engine's instructions |
| 5 | Background polling: about 715 of the runtime's 1,060 wakeups a second at rest are polls, plus 1,000 a second from Melodee's stream reader. | SMP-1, BASE-2, BASE-6, BASE-7 | Stream reader and screen thread fixed; service and lights loops unchanged |
| 6 | Needless saves to the card: some commands marked the project changed with no edit, in one case once a second for ever; each save re-read every project in the folder. | SHELL-1, LIGHT-6, IO-1 | Fixed |
| 7 | An external MIDI clock kept a still screen drawing at 60 fps. | BASE-4 | Fixed |
| 8 | Recording an audio track spun one core at 100 % for the length of the take. | IO-14 | Fixed |

### What the fork changes

| Change | Findings | Where |
| --- | --- | --- |
| **Refresh** setting (SCREEN): 60, 30 or 20 fps while the picture moves by itself. A control in use is answered at 60 fps. The loop sleeps between frames and is woken by input. | BASE-2, BASE-4, UI-1, UI-3, UI-4 | `base/src/runtime/run_app.cpp`, `session.h`, `controller.cpp`, `app.h` |
| **Dim after** setting (SCREEN): Off, 1, 2, 5, 10 min with no control used and nothing playing (a Looper loop still heard counts), recording or working. Backlight to its lowest lit level, pads to a sixth, bright buttons to dim. | BASE-3, LIGHT-4 | `shell/lights.cpp` (`power_tick`), `melodee_app.cpp`, `base/src/surface/leds.h` |
| **Pad brightness** setting (PADS): 100, 75, 50, 25 %. | LIGHT-4 | `shell/settings.h`, `base/src/surface/leds.h` |
| Commands that only mirror state or carry a gesture no longer mark the project changed, so an untouched project is not saved again. | SHELL-1, LIGHT-6 | `shell/context.cpp` |
| The project list keeps what it read from each file instead of parsing every project after every save. | IO-1 | `shell/project_store.*` |
| The sample browser's folder is re-listed every 3 s only while it is shown. | IO-2 | `shell/sample_library.*` |
| The stream reader sleeps until a stream opens instead of waking 1,000 times a second. | SMP-1 | `engine/stream_pager.*` |
| The audio-track recorder no longer spins during a take, and looks 10 times a second at rest instead of 40. | IO-14, IO-7 | `shell/audio_tracks.cpp` |
| The MIDI thread no longer spins while SysEx goes out, and no longer walks all 8,192 sample slots every 5 ms. | LIGHT-5, LIGHT-1 | `shell/midi_io.cpp`, `shell/lights.cpp` |
| Render workers no longer stay awake 200 µs for a second run that never comes (one keyed track). | HOST-3 | `engine/engine_host.cpp` |
| The Compressor effect skips its logarithm and power while under its threshold. | FX-2 | `engine/effects.cpp` |
| Kit lanes: one lookup removed from the tick loop. | MODEL-5 | `engine/engine_host.cpp` |
| Screen Mirror's frame timer stops with the stream. | BASE-13 | `base/src/drivers/screen_mirror.cpp` |
| Build: `-mcpu=cortex-a55`, `-fno-trapping-math`, eager binding (`-z now`). | BASE-5, BASE-8 | `base/base.mk` |
| A sampler voice that holds its level plays its frames without the tests and the envelope step that cannot change. | SMP-9 | `engine/sampler.cpp` (`held_run`) |
| A Pad FX that is off moves its history on in one or two copies, not a frame at a time. | SMP-3 | `engine/punch.cpp` |
| The echoes look their SYNC up once a call, not once a frame. | FX-4 | `engine/effects.cpp` |
| The mixer reads a gain that is not moving once a block. | HOST-4 | `engine/engine_host.cpp` |
| An effect chain that hears silence and holds nothing rests, and wakes exactly where it would have been. A track or bus that gives nothing is not mixed. | HOST-1 | `engine/engine_host.cpp` (`run_chain`), `engine/effects.*` (`rest`, `process_silent`) |
| The screen's snapshot leaves the kits and pattern sketches alone in a frame where neither can have changed. | MODEL-1 | `shell/shell.cpp` (`snapshot`), `shell/context.*` (`heavy_serial`) |
| Crash recovery puts the backlight back: the recovery script when the app's service stops, and the runtime itself as the next app starts, should one have been killed in between. | BASE-3 | `base/install/restore-stock.sh`, `base/src/drivers/platform.*` (`recover()`) |

Changes to `base/` are on a branch named `hdslave` inside the submodule, since the project's own rule is to change the base in sample-base and not from Melodee's tree.

### What you will notice

- The place bar, play head and meters step at 30 fps when your hands are off the controls. Set **Refresh** to 60 to get the old look.
- After two minutes untouched and stopped, the screen and lights go low. Any control brings them back and still does its job.
- The processor runs at its full speed at all times, as it does under the stock app. No setting changes that.
- The settings file gains three lines (`refresh`, `dim_after`, `pad_level`). The original Melodee ignores lines it does not know, so the file still works there.
- A project is no longer marked unsaved after you only played pads with Note Repeat or the arpeggiator, or ran from an external clock.

## How this was done

1. **Platform facts first.** The processor's operating points, the kernel's configuration and the board's wiring were read from inMusic's public kernel release for firmware 1.3.0 (see `../MPC Sample Knowledge Base/`). That is where the fixed 1.8 GHz and the voltage table come from.
2. **Audit.** Eleven auditors each read one area in full: runtime and drivers, the render graph, sampler and Looper, effects, plug-in hosting, shell core, lights and MIDI, storage, the drawing framework, the individual pages, and the project model. Each wrote findings with the file and line, what the code does, the cost, a proposal, and whether the sound would change.
3. **Verification.** I read the cited code for every finding and recorded a verdict. The verification line under each finding says what I read and what I did not.
4. **Implementation.** I chose findings that save the most for the least risk and that the repository's tests can cover, and left out anything whose effect on sound or timing can only be judged on the device.

### What verification corrected

- **`-fno-math-errno` is not bit-exact here.** BASE-5 expected it to be. I added an output hash to the DSP bench and built each flag on its own: with `-fno-math-errno` the bench plays different bits, with `-mcpu=cortex-a55` and `-fno-trapping-math` it plays the same. The flag is not used.
- **The MPC Sample has no Wi-Fi or Bluetooth enabled.** My brief to the auditors said the board had a radio module. The platform file describes one, but the MPC Sample's own device tree leaves it off, so there is nothing to switch off.
- **My first compressor shortcut had an edge case.** The exactness check found a wrong result for a negative envelope with a threshold under −119 dB. Neither occurs in the effect, but the code now excludes it, and 370 million compared gains are identical.
- **One bug was missed by the audit**: the audio-take busy loop (IO-14).
- **Denormals are already handled.** Several of my questions assumed they might not be; flush-to-zero is set on the audio thread and every render worker (HOST-10).

### Review before the commit

Before committing I read every changed line again, against the code around it and not only the diff, and had a second reader go through the same diff without my notes. Between them the two reads changed seven things, each small. Everything was then rebuilt and every test below run again.

What my own read changed:

- **A killed app could leave the CPU limit or the dimmed backlight behind.** The recovery script runs when the app's service stops, but not when one app is swapped for another or updated in place. An app killed during such a stop left its changes, and the next one took the changed values for the MPC's own and "restored" those on leaving. The runtime now puts back what the notes in `/run` hold as it starts (`Platform::recover()`), with a test. (The CPU limit was removed altogether afterwards; the backlight's recovery stays.)
- **A control touched while a frame was being drawn could wait 100 ms for its picture** on a still screen. The original loop had the same gap. Such a control now gets a frame of its own at the fast rate.
- **Start, Continue and Stop from outside wake the screen again**, so a transport started by another machine shows at once. MTC quarter frames joined the clock and Active Sensing as bytes that do not: they come a hundred times a second while timecode runs.
- **A Looper loop that is still heard keeps the lights up.** Dim after looked at the sequencer and at recording, not at a loop playing by itself.

What the second reader found, each confirmed in the code before it was fixed:

- **A control's result could show 100 ms late on a still screen.** A press woke the screen at once, but what a press does often shows a frame later (the engine answers a block later), and the loop went back to its resting rate after one unchanged frame. The original held 60 fps for half a second after any control. While a control has been used in the last 300 ms, every frame now comes at 60 fps.
- **The Power button did not end the dimmed state.** It comes by another thread than the pads and buttons, and the Exit sheet opened on a dimmed screen. It now counts as a control.
- **A change of the Pad FX bank or target was no longer marked unsaved.** In the original nothing marked it either, until the next punch's command did by accident; with punches made exempt (LIGHT-6), nothing did at all. The two setters now mark the project themselves.

The second reader found nothing in the wake-ups and sleeps of the screen thread and the stream reader, the CPU limit and its recovery, the take recorder, the project-list cache, the compressor shortcut or the settings' round trip.

What my read confirmed, where I had relied on an assumption:

- **The dimmed backlight is lit, not off.** The MPC Sample's device tree gives the backlight seven levels (0, 8, 16, 32, 64, 128, 255 of 255); Dim after uses level 1, which is also what Brightness at 20 % sets.
- **None of the thirteen commands that no longer mark the project changed carries an edit.** I read each one's senders: each mirrors a value kept elsewhere (a track parameter, an Input Config setting, a sample's root note) or passes a gesture on. Input Config is saved by its own timer, not by the project's.
- **The project-list cache is touched by one thread only**: saves and scans both run on the service thread.
- **The recovery script's new lines** were run under the container's `sh` against stand-in files: the backlight went back, a line naming another device or carrying no number wrote nothing. (Run again after the CPU limit's part was removed.)

## Second pass: speed, measured

The owner asked for the CPU speed feature to be removed and for more speed with nothing lost. "Nothing lost" was taken strictly: the same features, and the same audio to the bit. The first pass was done by reading; this one was done by measuring.

**How it was measured.** The DSP bench (`--melodee-dsp-bench`) plays a busy project: sixteen kits, three hits on every sixteenth, two effects on each track, four buses of reverbs and echoes, the master compressor. It gained three options (`pace=0`, `guard=0`, `play=0`) so that it can run under a profiler, where time means nothing: the blocks one after another, the overload guard off so that what is played cannot depend on how long a block took, and optionally stopped. Run that way under callgrind it gives an exact count of the instructions the engine executes, the same on every run, and a list of where they go. Each change was kept only if the bench's output hash stayed the same, playing and stopped.

**Where the engine's time goes** (the busy project playing, before the changes):

| Share of instructions | Where |
| --- | --- |
| 37 % | `SamplerEngine::run_plain`, the short loop every plain sampler voice takes |
| 13 % | the rest of the sampler: voices near their start or end, the kit filter, pad chains |
| about 30 % | the effects, spread thinly over many kinds; a third of it is `std::sin` for slow swings and the code round each call |
| about 10 % | the mixer: pan, sends, buses, the master |
| 2 % | ten Pad FX histories recording the master while off |
| 2 % | the echoes looking up their SYNC setting for every frame |

A stopped project ran 46 % as many instructions as the playing one: every effect on every track and bus still works on silence (HOST-1).

**What was changed, and what each measured** (instructions for three seconds of the bench, single thread):

| Step | Playing | Stopped | Output |
| --- | --- | --- | --- |
| Before | 3,011 million | 1,398 million | reference |
| A sampler voice holding its level skips the tests and the envelope step that cannot change (SMP-9) | 2,494 million (−17.2 %) | 1,398 million | same |
| Pad FX off: the history moved in one copy (SMP-3). Echo SYNC looked up once a call (FX-4). Mixer gains that are not moving read once (HOST-4) | 2,321 million (**−22.9 %**) | 1,225 million (**−12.4 %**) | same |

Each count includes about 78 million instructions of the benchmark's own setup and its second of warming up is engine work too, so the engine's own saving is slightly larger than the table's: 23.5 % playing and 13.1 % stopped.

**What was tried and not kept:**

- **`-O3` for the engine.** 4 % fewer instructions, and the bench played different bits. Not used.
- **Working the swings' sines out ahead of the frames (FX-3).** The profile showed heavy code round every `std::sin` call in Chorus, Flanger and the tape effects. Moving the calls out of the frame loop, with the same arguments, changed what three of those effects played against the original build and saved nothing. Inside one build the old and new ways agreed exactly, so the cause is the compiler fusing the surrounding sums differently once the code moved. Reverted. It shows how narrow "the same to the bit" is in this code: an edit that leaves every expression alone can still move a rounding.

**What these numbers are and are not.** They are instruction counts for one benchmark on the build machine's compiler output, which is the same code the MPC runs. They are not times on the MPC's Cortex-A55: a block's time there also depends on cache misses and on which instructions they are. A project of plug-in instruments gains nothing from the sampler change; a project of kits and samples gains about what the bench does.

**What is left, by weight.** The effects are now the largest share, and each kind is a little of it. The exact savings left there are small and each needs its own proof (FX-4's other sites, FX-9). Resting silent chains (HOST-1) is the big one, and it is not exact: a chorus's sweep keeps moving through silence, so a chain that slept would come back at a different point of its sweep. Outside the engine, the screen thread's copy of the whole project for every frame (MODEL-1) is the next largest cost that could go without anything being lost.

## Third pass: silent chains and the screen

The second pass ended with a stopped project still costing the engine about half of what a busy one playing does, and with the note that resting a chain could not be made bit-exact. The owner asked for it anyway. It turned out that it can be made exact, with one measured reservation.

**The rule.** A chain is no longer run once all of this has held for twice the longest delay line and a block (288,128 frames: 6 s at 48 kHz):

- what goes into it is silence, and what comes out of each of its effects is silence;
- every effect was left by every one of those blocks as `Effect::rest()` leaves it. The engine keeps a copy of the effect before the block, applies `rest()` to the copy, and compares the two byte for byte. So `rest()` is not trusted to describe what an effect does in silence: it is checked against the effect itself about 2,250 times in a row before it is used alone;
- no plug-in and no keyed effect is in the chain, and no effect in it is still clearing its memory or holding sound that is not heard (a MIX of 0).

While it rests, each effect's slow swing and the place it writes at in its delay line move on exactly as they would have, a frame at a time. That matters: a delay works out where it reads from the place it writes at, in floating point, and the first version, which left the place where it was, played the next sound differently in its last bits. The chain is run again from the first block that brings it sound, or in which one of its effects, the tempo or the sample rate has changed.

A track or bus whose block is exactly +0 is no longer mixed into the master, Knob FX and the buses: adding +0 to a sum that began at +0 changes nothing.

**The reservation: what counts as silence.** With silence as exactly zero, three of the bench's twenty chains never rested after playing. A filter's last bits can go round for ever at about 1e-38: the engine flushes results that small to zero, but a sum of two such values is not that small, so they keep turning over. And an echo takes minutes to fall from the output's lowest bit to nothing. So silence is anything under 2^-95 (2.5e-29, -571 dB), in what goes in and out and in an effect's running values. The output's lowest bit is 4.7e-10: what is discarded is a trillion trillion times under it and cannot come back. With that floor the output is still the same to the bit in every run below; a chain that reaches true zero is exact with no reservation at all.

**Measured** (instructions a block, one thread, the bench's project never played, so that every chain can rest):

| | Instructions a block |
| --- | --- |
| Stopped, original build | about 880,000 |
| Stopped, after the second pass | 766,710 |
| Stopped, chains resting | 181,205 (**-76 %**) |

The busy project playing costs 0.2 % more than before (the look at each chain's input): 2,326 million instructions for three seconds, against 2,321 million.

**How long until a chain rests.** A chain without long tails (drive, filter, compressor, chorus, phaser, crush) rests 6 to 15 s after its sound ends. One with an echo or a reverb rests when its tail has fallen under the floor. Each kind alone at the values it starts with, after two seconds of sound: Echo 24 s, Room 25 s, Plate 29 s, Reverb 33 s, Tape echo 45 s, Shimmer 76 s. In the bench, where the echoes feed back harder, about three minutes. After the bench's project has played for three seconds and stopped: 12 of its 20 chains rest after 30 s, 15 after 80 s, all 20 after 400 s. A project that has not played since it was loaded rests whole after 6 s.

**Proof that nothing changes.** The bench has a second switch (`rest=0`: every chain run in every block, as before) and can stop and start (`stop=`, `again=`). Its output hash is the same with resting on and off: playing; stopped; and played, stopped and played again after silences of 30, 80, 190 and 400 s, on one thread and with three workers. A new test does the same for every kind of effect Melodee has, a kind a chain, through a silence long enough to rest and the note that ends it, comparing every sample.

**What does not rest.** A chain with a plug-in (plug-ins have their own gate) or a keyed effect. And a chain that holds one of thirteen of Melodee's 41 kinds of effect, which are run as before because the byte comparison never passes for them: Looper keeps sound in its memory; Tremolo, Pedal chorus, Pedal phaser, Spit, Diff delay, Sample delay, Spring, Ensemble, Multi-chorus, Vintage, Vinyl and Tape emulator have running values or line places that move in silence in ways `rest()` does not follow yet. The other 28 kinds rest (`.local/checks/rests_by_kind.cpp` lists them). The master section (the Pad FX histories, the master compressor) is run as before.

**On the MPC:** the status line has `melodee_chains_resting=`.

### The screen's snapshot

For every frame the screen thread takes a copy of what the page needs, under the lock the pads, the buttons and MIDI in also take. That copy was 794 KB: the whole project (539 KB, nine tenths of it the kits: every track's 128 pads, their effect chains and zone trims) and a sketch of each of the 2,048 patterns, rebuilt from the patterns every time (229 KB written, 492 KB read). Thirty times a second while a project plays.

**Measured.** A new tool, `--melodee-frames`, runs the frame as the app does (the tick, the snapshot, the page drawn), and callgrind's cache model was given the MPC's sizes from the vendor's device tree (32 KB of data cache a core, 512 KB shared by the four, 16 ways, 64-byte lines). Instruction counts say little here, since a copy is few instructions and many cache misses, so the figure is the usual estimate of cycles: instructions, plus 10 for a first-level miss, plus 100 for a last-level one. On Stage, playing:

| A frame | Snapshot | Drawing |
| --- | --- | --- |
| Before | about 3.4 million cycles (29,000 last-level misses) | about 3.3 million |
| After | about 0.75 million (5,600) | the same |

So the snapshot cost as much as drawing the page, nearly all of it memory traffic, and now costs a fifth of that. The estimate is rough (a copy's misses are cheaper than 100 cycles each on a real core, since it reads ahead), but the proportion is what matters: about nine tenths of the bytes are gone. The copy was also larger than the MPC's whole shared cache, so every frame pushed the audio threads' data out of it, thirty times a second; no count here shows what that cost them.

**The rule.** `Context::heavy_serial` moves whenever a kit or a pattern may have changed: on every control and MIDI message that reaches the shell, and at each place where such a change is told to the audio thread (an edit sent, a pattern synced, Motion written, a track sent whole, a project switched to), which is how changes that come by themselves arrive (a sample landed, an arp's notes recorded). A frame in which it has not moved is a light one: everything is copied as before except the kits and the sketches, which stay as the last whole frame made them. Three more things make a frame whole: any command sent to the audio thread since the last frame, a project or pattern bank handed out by `Shell::project()` or `patterns()`, and half a second since the last whole one. The last is a net under the rule: if a change ever came by a way that tells nobody, the screen would show it at most half a second late.

**Proof that nothing changes.** Every page was drawn for 120 frames with light snapshots and with whole ones, on three tracks, stopped and playing (276 runs, 33,000 frames): the pixels are the same in every run. The monkey test now keeps one snapshot from draw to draw, as the app does, and has the shell check every light snapshot against a whole one: in 36,000 random moves, none differed.

**What it does not do.** A control in use makes every frame whole, as before: the saving is for frames while your hands are off, which is when a project plays. An external MIDI clock also makes every frame whole, since each clock byte is a MIDI message. Drawing itself is unchanged.

## Tests

All run in the project's own build container (Debian 12, arm64) on an Apple Silicon Mac.

| Check | Result |
| --- | --- |
| Build with `-Wall -Wextra -Werror` | clean |
| Melodee's test suite (44 tests, including the 18,000-line engine and shell test) | all pass |
| New assertions: the three settings (defaults, every value, round trip through the settings file, older files); no setting writes a CPU speed | pass |
| sample-base's whole suite, with new cases for backlight recovery, recovery after a killed app, LED scale and rest | 56 pass; 13 skipped, which need a built sample-base app or tools the container lacks |
| Monkey test (12 seeds × 3,000 random control moves) | 0 failures |
| Page previews | all 777 render; AUDIO and SCREEN checked by eye |
| Self-test audio (6 s render), SHA-256 against the unmodified build | identical |
| DSP bench output hash (16 busy tracks, 10 s) against the original build's, playing and stopped, on one thread and with three workers | identical |
| The sampler's held runs against the loop frame by frame (`held_runs` off), kits and keygroups | identical |
| Resting chains against every chain run every block (`chain_rests` off): every effect kind, a kind a chain, through a silence and the note that ends it | every sample identical |
| The same on the DSP bench: stopped, and stopped and played again after 30, 80, 190 and 400 s | identical hashes |
| Light snapshots against whole ones: every page, three tracks, stopped and playing, 120 frames each | the same pixels in all 276 runs |
| The monkey with every light snapshot checked against a whole one | none differed |
| Compressor shortcut against the original expressions | 370,217,204 gains identical |

Two notes on the test setup:

- The engine test keeps whole projects on the stack and crashes under a container's default 8 MB stack limit. That happens on the unmodified code too. `build.sh` now passes `--ulimit stack=268435456`.
- The DSP bench's hash only compares when its line says `guard=0/0`. When the build machine hiccups, the overload guard steals a voice and the output legitimately differs.

The bench's own block times on the build machine vary too much from run to run (100 to 500 µs with the machine's other work) to show a saving of this size, which is why the second pass counts instructions instead. Neither says what a block takes on the MPC: the compiler flags tune for the Cortex-A55, and the other changes save work outside the audio blocks.

## Before you install it

The package is `dist/Melodee-MPC-Sample-v0.5.1-hdslave.3-unsigned.zip`, made by `tools/package_unsigned.py`. It has the same files and boot checks as a release, without the update signature (only the author has that key). So it installs by hand only, and Melodee's own updater will not accept it.

Things only the device can tell:

1. **What a block now takes.** Settings > AUDIO shows the share of each block's time a project takes (DSP); the status line has it as `melodee_render_load=`. The same project under the original Melodee and under the fork, playing and stopped, is the measurement this document cannot make.
2. **Whether anything sounds different.** It should not: the bench and the self-test play the same bits. A project that uses effects the bench does not (the bench has fifteen kinds) rests on the tests and on the changes leaving those effects' code alone.
3. **How the dimmed state looks** in a dark room and in daylight, and whether a sixth of the pads' light is the right resting level.
4. **Whether 30 fps is acceptable** on the pages you use most.

Going back is one setting each: Refresh 60, Dim after Off, Pad brightness 100 %.

## Not done, in order of likely value

1. **HOST-1, the rest of it.** Chains now rest (third pass). Left: the master section when nothing feeds it, the kinds whose running values `rest()` does not follow, and an idle track's remaining bookkeeping. A rested stopped block is 181,000 instructions, so what is left here is small.
2. **UI-5: skip drawing a screen that cannot have changed.** Would take the resting state from 10 full redraws a second to none. Needs three checks moved out of the screen thread's snapshot and a dependency list per page.
3. **Drawing.** With the snapshot cut, drawing the page is what the screen thread spends its time on: about 3.3 million cycles a frame on Stage by the same estimate. UI-9, UI-11, UI-12 and the page findings are each a part of it.
4. **BASE-6, BASE-7, LIGHT-3: slow the 5 ms service and lights loops when nothing happens.** Needs every pass-counting timer converted to clock time first.
5. **IO-3: write only the loops that changed** when saving.
6. **SMP-2, SMP-4, FX-4's other sites, FX-9: exact DSP savings** in the Looper and the effects. Each is small, and each must be shown to leave the output's bits alone (see FX-3 in the second pass).
7. **HOST-2: tune when rendering goes parallel.** The author measured this on the device; so should any change.
8. **IO-4, IO-5, PLUG-5: cache analysis results and plug-in probes** to shorten loading.
