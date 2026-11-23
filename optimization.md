# Melodee optimization audit

A performance and battery-life audit of Melodee 0.5.1 for the Akai MPC Sample, and the record of what the hdslaveOS fork (first called `melodee-hdslave`) changes as a result. Written on 2026-11-20, in three passes: an audit by reading; a second pass for speed alone, measured with a profiler; and a third for what a stopped project and the screen cost.

Melodee is the work of Kerem Kiliç (Ellic Studio). This document covers his app and the sample-base runtime under it (`base/`). The fork keeps his copyright headers, credits and licence.

**Nothing here has been run on an MPC Sample.** Every cost and saving is an estimate from reading the code or a figure worked out from the hardware's published operating points, unless a line says it was measured. What was measured, on the build machine, is that the fork still builds, passes every test and plays the same audio to the bit, and, in the second pass, how many instructions the engine runs for a benchmark project before and after each change.

## Summary

- **128 findings**: 127 from eleven auditors, each covering one part of the code, and one found while implementing. I checked each against the code. 95 are confirmed, 27 turned out to be the same issue reported by a second auditor, 5 are partly confirmed and 1 could not be checked without the compiler's output. None was rejected outright, but one recommended compiler flag turned out to change the audio and was dropped (BASE-5).
- **The processor runs at full speed by default.** The first pass built a CPU speed setting that can hold the cores at a lower clock and voltage to save battery. It ships as the CPU speed setting (Auto, Full, Balanced, Eco) and defaults to Full: untouched, the four cores stay at the kernel's 1.8 GHz, as under the stock app.
- **The fork implements 25 findings**, nine of them in part. They remove wasted work in the engine and around it and add the Refresh, Dim after, Pad brightness and CPU speed settings. The audio output is unchanged: the self-test render and the DSP bench's output are bit-identical to the original build's.
- **Second pass, for speed:** the engine runs **23 % fewer instructions** for the benchmark's busy 16-track project and **12 % fewer** for the same project stopped, playing the same to the bit. Most of it comes from one loop in the sampler.
- **Third pass, silent chains:** a chain of effects that hears nothing and holds nothing is no longer run (HOST-1). Once a stopped project's chains rest, a block costs the engine **76 % fewer instructions**. The output is the same to the bit with resting on and off, through silence and the sound that ends it.
- **Third pass, the screen:** the screen thread no longer copies the kits and redraws every pattern's sketch for frames in which neither can have changed (MODEL-1). With the MPC's cache sizes that takes its snapshot from about 3.4 million cycles a frame to 0.75 million, and the pads wait on its lock for that much less time. Every page gives the same pixels.

### The levers, in order of likely value

| # | Lever | Finding | In the fork |
| --- | --- | --- | --- |
| 1 | The kernel holds all four cores at 1.8 GHz and 1.10 V at all times. At 1.416 GHz the supply is 0.90 V, where the same work takes about a third less switching energy. | BASE-1 | CPU speed setting, Full by default: full speed unless the player chooses less |
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
- The processor runs at its full speed, as it does under the stock app, unless CPU speed is set below Full.
- The settings file gains four lines (`refresh`, `dim_after`, `pad_level`, `cpu_speed`). The original Melodee ignores lines it does not know, so the file still works there.
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

- **A killed app could leave the CPU limit or the dimmed backlight behind.** The recovery script runs when the app's service stops, but not when one app is swapped for another or updated in place. An app killed during such a stop left its changes, and the next one took the changed values for the MPC's own and "restored" those on leaving. The runtime now puts back what the notes in `/run` hold as it starts (`Platform::recover()`), with a test.
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
- **The recovery script's new lines** were run under the container's `sh` against stand-in files: the backlight went back, a line naming another device or carrying no number wrote nothing.

## Second pass: speed, measured

The owner asked for more speed with nothing lost, the processor at full speed by default. "Nothing lost" was taken strictly: the same features, and the same audio to the bit. The first pass was done by reading; this one was done by measuring.

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
| New assertions: the settings (defaults, every value, round trip through the settings file, older files); CPU speed defaults to Full | pass |
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

## All findings at a glance

128 findings: 95 confirmed, 27 merged, 5 partly confirmed, 1 not checked.
"Merged" means the same issue was reported by more than one auditor and is written up once, under the ID its verification line names.

### Runtime, drivers, power and build

| ID | Finding | Gain | Sound or behaviour | Verified | In the fork |
| --- | --- | --- | --- | --- | --- |
| BASE-1 | No CPU power policy: the cluster sits at 1800 MHz and 1.10 V all the time | battery High (the largest software lever on SoC power I found). | bit-exact. | Confirmed | Implemented |
| BASE-2 | The idle screen loop polls at 250 Hz and redraws the whole screen 10 times a second; busy is always 60 fps | battery High idle, CPU Medium busy. | steps 1 and 2 none; step 3 visible (30 fps animation; a … | Confirmed | Implemented in part |
| BASE-3 | The backlight never dims or blanks, and the base has no idle signal for it | battery High (estimate). | visible; make it a setting. | Confirmed | Implemented |
| BASE-4 | MIDI clock or Active Sensing from an external device keeps the screen at 60 fps | battery Medium (only with such a device attached). | none. | Confirmed | Implemented |
| BASE-5 | Build flags: no `-mcpu`, math functions keep errno, no eager binding, no section garbage collection | CPU Medium (a few percent expected; more where `lround` sits in a … | `-fno-math-errno`, `-fno-trapping-math`, `-mcpu`, section … | Confirmed | Implemented in part |
| BASE-6 | The service thread wakes 200 times a second and makes four syscalls each time for sources that could all be waited on | battery Medium. | none if the wake is used; otherwise slow requests are … | Confirmed | Not implemented |
| BASE-7 | The MIDI thread runs the lights tick every 5 ms and reads every port on every pass | battery Medium to Low. | none for sound; LED animation steps at 50 Hz instead of 200 … | Confirmed | Not implemented |
| BASE-8 | Real-time hazards on the audio thread: lazy symbol binding, no locked memory, priority under every IRQ thread | latency (dropout risk) Medium for (a), Low to Medium for (b). | bit-exact. | Confirmed | Implemented in part |
| BASE-9 | The USB audio threads poll every 20 ms and scan the sound cards four times a second while no computer streams | battery Low to Medium. | none; recording start is detected at least as fast. | Confirmed | No change |
| BASE-10 | While a computer records, the converter filters all 18 channels, silent stems included | CPU Medium while recording (proportional to unused tracks); battery … | bit-exact at the encoded output. | Confirmed | No change |
| BASE-11 | Boot: the binary is hashed three times, the stock binary once, and a six-second render self-test runs on every start | load time Medium (estimate: one to a few seconds of the … | none. | Confirmed | No change |
| BASE-12 | A changed frame is compared twice and copied whole | CPU Low. | none (the same rectangles are sent). | Confirmed | No change |
| BASE-13 | Screen Mirror's frame timer is never disarmed, so its thread keeps waking 30 or 60 times a second after a stream ends | battery Low. | none. | Confirmed | Implemented |
| BASE-14 | The MIDI thread enumerates every sound card once a second | latency Low, battery Low. | none. | Confirmed | No change |
| BASE-15 | Cross-area bug: the MIDI thread can spin at full speed while a SysEx goes out during playback | CPU Low (rare). | none. | Merged | See LIGHT-5 |

### Audio engine: the render graph

| ID | Finding | Gain | Sound or behaviour | Verified | In the fork |
| --- | --- | --- | --- | --- | --- |
| HOST-1 | Track, bus, loop and master chains never rest; a silent track costs its full strip every block | battery High; CPU High while stopped or in sparse arrangements; no … | steps 1 and 3 bit-exact (they skip additions of +0.0). Step … | Confirmed | Implemented in part |
| HOST-2 | The pool goes to four cores at 150 µs of track work, about 5 % of a block | battery Medium to High for light and medium projects; none for heavy … | bit-exact (the pool sums results in a fixed order whoever … | Confirmed | Not implemented |
| HOST-3 | Workers spin 200 µs each for a second run that does not come | CPU and battery Medium in those setups; none elsewhere. | bit-exact. | Confirmed | Implemented in part |
| HOST-4 | The host's strip and mix loops are compiled scalar | CPU Medium (per-frame cost of every sounding track); battery Low to … | intended bit-exact: element-wise vector code keeps IEEE … | Partly confirmed | Implemented in part |
| HOST-5 | A compensation line that starts clears 128 kB on the audio thread; all lines start together | latency (dropout risk) Medium; CPU none in steady state. | bit-exact (the same zeros are read). | Confirmed | No change |
| HOST-6 | Ten punch histories record the master every block, a frame at a time | CPU Low; battery Low. | bit-exact (the same samples reach the history; no other … | Merged | See SMP-3 |
| HOST-7 | Fixed work repeated every block that can be kept or skipped | CPU Low; battery Low (it is part of the idle floor). | bit-exact for 1, 2, 4, 6, 7 (the same expressions … | Partly confirmed | No change |
| HOST-8 | USB-C stems and the full-volume USB mix are rendered whether or not a computer records | CPU Low; battery Low. | none at the jacks. Over USB-C the first service pass (up to … | Confirmed | No change |
| HOST-9 | Master compressor: a sine a frame for a 0.6 Hz wobble, log and power every fourth frame | CPU Low. | the hoisted division is bit-exact; the ramped sine and the … | Confirmed | No change |
| HOST-10 | Flush-to-zero is set once per thread and never checked again | CPU (protection of headroom) Low. | none (values under about 1e-38 only). | Confirmed | No change |

### Audio engine: sampler voices, streaming and the Looper

| ID | Finding | Gain | Sound or behaviour | Verified | In the fork |
| --- | --- | --- | --- | --- | --- |
| SMP-1 | The stream pager's thread wakes 1,000 times a second for as long as the app runs | battery Medium; CPU Low. | none. A voice opened with nothing held (`safe` 0: a … | Confirmed | Implemented |
| SMP-2 | A stretched loop's window search runs about half a millisecond in one block, calls `fmod` 24,640 times, and all four loops search in the same block | latency (dropout risk) High if the estimate holds; CPU Medium. | step 1 bit-exact. Step 2 not bit-exact: the same windows … | Confirmed | No change |
| SMP-3 | Ten Punch histories record the master every block, frame by frame, with nothing held | battery Low to Medium; CPU Low (A). Memory Low (B). | A bit-exact. | Confirmed | Implemented in part |
| SMP-4 | A playing loop works out one `fmod` and four double divisions per frame from values that do not change in the block | CPU Low to Medium (about 1 % of a core with four loops playing). | the first step bit-exact; the second rounding only (the … | Confirmed | No change |
| SMP-5 | A kit track with nothing sounding still walks its voices four times and filters silence; there is no early return | battery Low to Medium (it grows with the number of kit tracks); CPU … | bit-exact (zeros through a filter at rest are zeros). | Confirmed | No change |
| SMP-6 | A voice whose envelope has decayed to a sustain of 0 plays on in silence | CPU and battery Medium where such envelopes are used; voices freed … | bit-exact while parked (the voice only ever added `x * 0`). … | Confirmed | No change |
| SMP-7 | Each cloud grain calls `std::sin` and divides once per frame | CPU Medium while a Cloud pad plays; none otherwise. | rounding only (the window differs by about 1e-6 with the … | Confirmed | No change |
| SMP-8 | The pad filter and the kit filter divide two to six times a frame by values that change at most every 16 frames | CPU Low to Medium. | rounding only (a product in place of a quotient: the last … | Confirmed | No change |
| SMP-9 | The short voice loop still runs the envelope's state machine and three exit tests on every frame | CPU Low to Medium (about 40 % off an unfiltered voice; about 1 % of a … | bit-exact (the same operations in the same order on the … | Confirmed | Implemented |
| SMP-10 | A mono sample is held twice | memory Medium; CPU Low. | bit-exact (the voice reads the same values). | Confirmed | No change |
| SMP-11 | The Looper's block runs in full when the Looper is off, and frame by frame when its loops are empty | battery Low. | bit-exact. | Confirmed | No change |
| SMP-12 | About 68 MB is made and zeroed at start-up for features that may never be used | memory Low. | none. | Confirmed | No change |
| SMP-13 | A held Pad FX works out constants and LFO-swept coefficients with libm on every frame | CPU Low. | the constants bit-exact (the same expression, worked out … | Confirmed | No change |

### Audio engine: effects and analysis

| ID | Finding | Gain | Sound or behaviour | Verified | In the fork |
| --- | --- | --- | --- | --- | --- |
| FX-1 | Track, bus, master and loop chains never rest: every effect runs every block, on silence too | battery Medium: about a tenth of one core for a project with many … | not bit-exact. Tails are cut once they are under -100 dBFS … | Merged | See HOST-1 |
| FX-2 | The slot Compressor calls `log10f` and `powf` on every sample, also when it is not compressing | CPU Medium. The Compressor drops from "heavy" to "cheap" for every … | bit-exact. | Confirmed | Implemented |
| FX-3 | Slow LFOs are computed with `std::sin` on every sample in ten effects and in the master compressor's Color | CPU Medium for the modulation effects (about half their time), Low … | rounding only. Because the rotation restarts from the true … | Confirmed | Tried and withdrawn |
| FX-4 | Values that are constant for the whole block are recomputed on every sample | CPU Low to Medium, spread over about twenty effects. | bit-exact, as long as each hoisted value is a whole … | Partly confirmed | Implemented in part |
| FX-5 | Every sample load re-runs the full onset, beat and auto-chop analysis; nothing is kept | load time Medium (High for projects with long files), battery Low to … | none (1 changes when the work happens; 2 and 3 are … | Merged | See IO-4 |
| FX-6 | The master compressor does its full work on silence, and Color adds a `sin` and five divisions per sample | CPU Low; battery Low (it is one instance). | (a) bit-exact, (b) rounding only, (c) rounding only (the … | Merged | See HOST-9 |
| FX-7 | Stereo filter chains run as two scalar passes although both sides use the same coefficients | CPU Medium for these kinds (perhaps 30 to 45 % of their time), Low … | rounding only at worst. The arithmetic per lane is the … | Confirmed | No change |
| FX-8 | Divisions per sample by a value that only changes with a parameter | CPU Low (Medium for Formant). | rounding only (the last bit of each product). | Confirmed | No change |
| FX-9 | The frame loops reload and store every member on every sample because the audio pointers may alias the effect | CPU Low to Medium. | bit-exact. | Not checked | No change |
| FX-10 | Shimmer's octave shifter calls `fmod` and `sin` twice per sample | CPU Low to Medium (one effect). | rounding only for the sine; the `fmod` change is bit-exact. | Confirmed | No change |
| FX-11 | Short-delay effects sweep their whole 1.15 MB block; the reverb's predelay ring is two thirds of its cache footprint | CPU Low; memory traffic Low to Medium. | bit-exact for both. | Confirmed | No change |

### Plug-in hosting

| ID | Finding | Gain | Sound or behaviour | Verified | In the fork |
| --- | --- | --- | --- | --- | --- |
| PLUG-1 | An instrument plug-in never sleeps while the song plays | CPU High and battery High during playback for projects with plug-in … | step 1 bit-exact for a plug-in that reports honestly. Step … | Confirmed | Not implemented |
| PLUG-2 | The armv7 bridge busy-waits on both sides of every block | CPU Medium for projects with bridged plug-ins; battery Low–Medium … | bit-exact. | Confirmed | Implemented |
| PLUG-3 | The audio thread waits on the bridge's lock behind program and state calls | latency: removes a whole-engine dropout. Medium. | audible only where today the whole mix would stall: that … | Confirmed | No change |
| PLUG-4 | JUCE's LV2 and VST3 hosts keep waking for plug-ins that are asleep | battery Low–Medium while an LV2 plug-in is loaded; Low for VST3. | none. LV2 worker jobs (a plug-in loading a sample) would … | Confirmed | No change |
| PLUG-5 | Every plug-in on the card is listed and probed at start-up; the results are not kept | load time Medium, battery Low (once per boot), memory Low. | none in sound. Visible: with (a) alone the … | Confirmed | No change |
| PLUG-6 | Listing a folder reads every instrument library whole | load time Medium (picker open, project load); battery Low. | none. | Confirmed | No change |
| PLUG-7 | A plug-in's data files are copied into RAM with its code, for probes too | memory Medium for bundles with large resources, load time Low–Medium. | none expected. | Confirmed | No change |
| PLUG-8 | The idle gate ignores tails: a plug-in delay longer than half a second loses its echoes | correctness. CPU slightly negative: an effect stays awake a few … | audible, for the better: echoes and late reflections that … | Confirmed | No change |
| PLUG-9 | The cards of a bridged plug-in in view cost up to 36 round trips every 50 ms | CPU Low; fewer lock meetings with the audio thread. | none. A plug-in that changes a parameter's text without … | Confirmed | No change |
| PLUG-10 | Small periodic work with no plug-in loaded | battery Low, CPU Low. | none; a project change is followed up to 50 ms later. | Confirmed | No change |
| PLUG-11 | A zip that fails part-way is unzipped again at every listing | card wear and battery Low. | none. | Confirmed | No change |

### Shell: service work, saving, settings

| ID | Finding | Gain | Sound or behaviour | Verified | In the fork |
| --- | --- | --- | --- | --- | --- |
| SHELL-1 | Engine-mirror commands mark the project dirty, so some projects autosave about once a second while the device sits idle | battery High and card wear High for affected projects; removes one … | bit-exact; the only behaviour change is that these projects … | Confirmed | Implemented |
| SHELL-2 | Every save, autosaves included, rewrites every Looper loop's WAV whether or not the loop changed | battery High and card wear High whenever the Looper holds audio; CPU … | none (bit-exact; the files on the card are the same). | Merged | See IO-3 |
| SHELL-3 | There is no idle policy at all: nothing dims, slows or powers off, and no "time since last activity" is published | battery High (unmeasured; depends on the backlight's share). | visible (the screen dims after the chosen time; off by … | Merged | See BASE-3 |
| SHELL-4 | `ProjectStore::scan()` re-reads and fully parses every project in the folder after every request, and at boot before the last project is loaded | CPU Medium (service thread), load time Medium to High at boot with … | none. | Merged | See IO-1 |
| SHELL-5 | The screen snapshot copies the whole `Project` and rebuilds 2048 pattern sketches every frame while holding the lock the MIDI thread takes on every pass | CPU Medium (screen thread, and cache pressure on the audio thread) … | none intended; visible only if an epoch bump is missed (a … | Merged | See MODEL-1 |
| SHELL-6 | Remote-`status` mirrors are rebuilt every 5 ms on the MIDI thread, including a walk of all 8192 sample slots | CPU Low to Medium (about 0.5 to 1 % of a core, constant), battery Low. | none (a `status` reply may be up to 100 ms old). | Merged | See LIGHT-1 |
| SHELL-7 | Shell housekeeping runs at the lights' 5 ms rate and allocates and locks on every pass | CPU Low (0.1 to 0.3 % of a core), latency Low (fewer cross-thread … | none audible; a landed file, a finished print or a new … | Confirmed | No change |
| SHELL-8 | The Samples browser's file list and the Project list are deep-copied every frame under the shell lock | CPU Medium on those pages, latency Medium (pad response while … | none. | Merged | See IO-12 |
| SHELL-9 | `MelodeeApp::service()` re-derives the same facts on every 5 ms pass; the mount table is parsed about 3 times a second from six places | battery Low from the app-side changes alone; Medium with the base … | none. | Merged | See IO-9 |
| SHELL-10 | The sample library re-lists the browser's folder every 3 s for ever, reading every MPC program file in it | CPU Low to Medium (depends on the folder), battery Low. | none; a file copied onto the card behind the app's back … | Merged | See IO-2 |
| SHELL-11 | During a project load the MIDI thread reads up to 15 sample files per pass from the card while holding the shell lock | latency Medium during loads; load time Low. | none (a sample's chop arrives with its length instead of … | Merged | See IO-6 |
| SHELL-12 | Helper threads that poll on a timer for ever, started whether or not their feature is in use | battery Medium (mostly the pager), CPU Low. | bit-exact if the pager is woken at the same points it would … | Merged | See SMP-1 |
| SHELL-13 | Settings are rewritten in full, unconditionally, once per step, and not atomically | battery Low, card wear Low, CPU Low to Medium (the scans avoided). | none (the file lands up to a second later). | Confirmed | No change |

### Lights, MIDI and controls

| ID | Finding | Gain | Sound or behaviour | Verified | In the fork |
| --- | --- | --- | --- | --- | --- |
| LIGHT-1 | The `status` mirror in `Shell::lights()` is rebuilt every 5 ms, with a walk over all 8192 sample slots | CPU and battery, Medium (paid while stopped too). | none. With the 10 Hz variant `status` can be up to 100 ms … | Confirmed | Implemented |
| LIGHT-2 | Other work `lights()` repeats every tick though nothing changed | CPU Low to Medium (Medium for 16 Levels playback); battery Low. | none. | Partly confirmed | No change |
| LIGHT-3 | The tick is a fixed 200 Hz; nothing slows down when nothing animates | battery and CPU, Medium. It also halves LED traffic (LIGHT-8). | visible in principle (fades in 10 ms steps); none expected … | Confirmed | Not implemented |
| LIGHT-4 | LEDs have no global brightness and no idle dim | battery, Medium and possibly High; unmeasured. | visible (a setting, and the idle dim). | Confirmed | Implemented |
| LIGHT-5 | The MIDI thread busy-waits while a SysEx goes out and any MIDI event is pending | CPU and battery: Low on average because it is rare, High while it … | none. | Confirmed | Implemented |
| LIGHT-6 | Playing gestures and sync messages mark the project changed, so the background save writes the card | battery Low; card writes Medium; it also removes a stall of the MIDI … | none in sound. The project no longer shows as unsaved after … | Confirmed | Implemented in part |
| LIGHT-7 | Pads, MIDI in and clock out wait on `Shell::lock` while the screen thread copies the project | latency, Medium. | none. | Merged | See MODEL-1 |
| LIGHT-8 | LED traffic: one SysEx for every one-step colour change, one `write()` for every message | CPU Low (perhaps 0.1-0.5 % of a core while playing). | visible in principle; meant not to be. Check low levels by … | Confirmed | No change |
| LIGHT-9 | Pad pressure is not coalesced; every message becomes a timed engine command | CPU Low to Medium while pads are held; audio headroom, depending on … | not bit-exact: pressure reaches the engine in steps of up … | Confirmed | No change |
| LIGHT-10 | MIDI ports are rescanned through ALSA every second on the MIDI thread | battery Low; latency Low to Medium. | none; a new USB device is seen as fast or faster. | Merged | See BASE-14 |
| LIGHT-11 | The idle MIDI loop reads every port and allocates a `std::function` on each pass | CPU Low. | none. | Confirmed | No change |

### Storage, loading and background jobs

| ID | Finding | Gain | Sound or behaviour | Verified | In the fork |
| --- | --- | --- | --- | --- | --- |
| IO-1 | Every store request re-lists the project folder and fully parses every project in it | CPU and battery Medium (after every edit burst); start and load time … | none. | Confirmed | Implemented |
| IO-2 | The sample browser's folder is listed again every 3 s, with a stat per file and every MPC program file read | battery Medium (Low with a small folder, High with a folder of MPC 3 … | none while the Samples page is shown. With it closed, a … | Confirmed | Implemented |
| IO-3 | Every save writes every loop's WAV to the card again, changed or not | card writes and battery High while the Looper is in use (none … | none. | Confirmed | Not implemented |
| IO-4 | A sample cannot play until its hit, beat and note analysis is done, and the analysis is repeated on every load | load time Medium (time until the Loading sheet closes and pads … | none in sound. Chop's hits and a sample's note arrive a … | Confirmed | No change |
| IO-5 | A long (streamed) sample's whole file is read from the card and analysed again after every load | load time, card reads and battery High for projects with long … | none. | Confirmed | No change |
| IO-6 | Adding samples and opening a project read each WAV's chunks from the card on the MIDI thread | latency Medium (pads, lights and MIDI out during loads). | none in sound; a sample's saved slices arrive with the … | Confirmed | No change |
| IO-7 | Threads started from the MIDI thread inherit SCHED_FIFO 15; the audio-tracks thread also wakes 40 times a second for ever | battery Low (40 wakeups a second gone); latency and robustness Low to … | none. | Confirmed | Implemented |
| IO-8 | The MIDI thread takes at least twelve plain mutexes each pass, four of them on a lock a SCHED_IDLE thread holds | latency Medium in the worst case, Low on average; CPU negligible. | none. | Partly confirmed | No change |
| IO-9 | Six separate pollers read the mount table and statvfs every mount, about three times a second | battery Low. | none; a card change is seen as fast as the card-status … | Confirmed | No change |
| IO-10 | The web server and mDNS threads wake four times a second each, and the service thread republishes the page's facts every pass | battery Low. | none. | Confirmed | No change |
| IO-11 | Every use of a cache file writes its timestamp to the card | card writes Low. | none (`tidy()` works in days). | Confirmed | No change |
| IO-12 | The Samples page copies the whole file listing each frame and each wheel detent | CPU Low (Medium in a 4000-file folder while scrolling). | none. | Confirmed | No change |
| IO-13 | A sample in memory is always two float vectors, so a mono 16-bit file takes four times its size | memory Medium for mono-heavy kits; no CPU change expected. | bit-exact (the same values are read). | Merged | See SMP-10 |
| IO-14 | The audio-track recorder's thread spins a whole core for as long as a take lasts | battery High and CPU High while recording an audio track; none … | none. The take's end is noticed up to 25 ms later. | Confirmed | Implemented |

### Screen: the frame loop and drawing framework

| ID | Finding | Gain | Sound or behaviour | Verified | In the fork |
| --- | --- | --- | --- | --- | --- |
| UI-1 | The whole pipeline runs at 60 fps whenever anything moves; nothing separates "a hand is on the controls" from "a play head is moving" | battery High, CPU High (about half the screen thread's time during … | bit-exact audio. Visible: play head, place bar and meters … | Merged | See BASE-2 |
| UI-2 | Every frame copies 0.8 MB of state under the lock the MIDI thread needs, and rebuilds the sketches of all 2048 patterns | CPU Medium, latency Medium (MIDI out jitter, pad-to-sound latency) … | bit-exact audio; MIDI timing gets tighter, never looser. … | Merged | See MODEL-1 |
| UI-3 | External MIDI clock and Active Sensing count as "input" and push the idle screen back to 60 fps | battery High in that setup, none otherwise. | none. | Merged | See BASE-4 |
| UI-4 | After every change the loop draws 30 more full frames, and slow animations are drawn at 60 fps | battery Medium, CPU Medium. | none in the audio. Visible: a change with no input behind … | Merged | See BASE-2 |
| UI-5 | A still screen is redrawn in full 10 times a second for ever; there is no "nothing changed" test before drawing | battery Medium (it is the device's resting state), CPU Low. | none in the audio (step 4 moves a check that sends an … | Merged | See BASE-2 |
| UI-6 | Lists, the sample in hand and zoomed peaks are copied or recomputed every frame although generation counters exist | CPU Low-Medium on the browser and zoom pages, latency Low-Medium. | none. | Merged | See IO-12 |
| UI-7 | A sheet that owns the screen is drawn over a page that was drawn in full first | CPU Medium while loading, load time Low, battery Low. | none. | Merged | See PAGE-12 |
| UI-8 | Meters and bars are drawn at sub-pixel sizes, so the picture differs on almost every frame they are on screen | CPU Low-Medium, battery Low-Medium (fewer pushes; more with UI-4). | visible, barely: bars move in whole pixels. | Confirmed | No change |
| UI-9 | Vector drawings that do not change are rebuilt and re-stroked every frame | CPU Medium on effect and sampler pages, Low elsewhere. | none for the icon cache (same path, same pixels). An image … | Confirmed | No change |
| UI-10 | A changed frame is compared and copied three times over between `present()` and the worker | CPU Low. | none. | Merged | See BASE-12 |
| UI-11 | Every small rounded rectangle goes through path rasterisation | CPU Medium on pages with many thin bars, Low elsewhere. | visible in principle: four corner pixels of each bar change … | Confirmed | No change |
| UI-12 | Each `Paint::text` call scans and hashes its string several times, and the caches are dropped whole when full | CPU Low, latency Low (no periodic hitch). | none. | Confirmed | No change |
| UI-13 | The boot animation recomposes every pixel on frames that cannot differ, and keeps its three images afterwards | memory Low, CPU Low (start-up only). | none. | Confirmed | No change |

### Screen: individual pages

| ID | Finding | Gain | Sound or behaviour | Verified | In the fork |
| --- | --- | --- | --- | --- | --- |
| PAGE-1 | Readouts and pulses that keep a stopped page at 60 fps | battery High while one of these is on screen (Record and Input Config … | visible (no pulse, coarser meter and load steps). | Confirmed | No change |
| PAGE-2 | Pads grid draws up to 512 rounded bars a frame, each through the path rasteriser | CPU Medium, battery Medium (Pads, and Stage of a frozen track). | none that can be seen at this size; strictly, visible at … | Confirmed | No change |
| PAGE-3 | Stage of an instrument pad redraws every zone of the map each frame | CPU High for instrument tracks, battery Medium. | none for (1) and (3); (2) squares the corners of tiles … | Confirmed | No change |
| PAGE-4 | Motion page walks every Motion point six times and strokes one path per lane, every frame | CPU Medium (High on dense Motion), battery Low. | none at pixel level for the decimation. | Confirmed | No change |
| PAGE-5 | The zoomed waveform's peaks are worked out from the audio again every frame, under the shell's lock | CPU Low to Medium, latency Medium (shorter hold of the shell lock). | none. | Confirmed | No change |
| PAGE-6 | Lists draw six rows but are copied, sorted or rebuilt whole every frame | CPU Low to Medium, latency Low. | none. | Merged | See IO-12 |
| PAGE-7 | Song draws the parts of long clips that are out of view | CPU Low to Medium (long arrangements). | none. | Confirmed | No change |
| PAGE-8 | Roll: one fill per note of the whole pattern for the strip at the foot, and a full scan for the marks | CPU Low (Medium on very dense patterns). | none. | Confirmed | No change |
| PAGE-9 | Looper rings and the Compressor's ghost bar are stroked afresh every frame though they rarely change | CPU Low to Medium on those two pages. | none. | Confirmed | No change |
| PAGE-10 | A time readout that changes every frame defeats the text caches | CPU Low. | visible (a coarser moving readout). | Confirmed | No change |
| PAGE-11 | Auto Sample's running light redraws the screen 60 times a second for the whole job | CPU Low, and the job finishes a little sooner. | visible. | Confirmed | No change |
| PAGE-12 | A page is drawn in full, then covered by a sheet that owns the screen | CPU Low, load time Low. | none. | Confirmed | No change |

### Project model and sequencing

| ID | Finding | Gain | Sound or behaviour | Verified | In the fork |
| --- | --- | --- | --- | --- | --- |
| MODEL-1 | Every screen frame copies the whole Project and rebuilds 2048 pattern sketches under the shell lock | battery Medium (the idle 10 Hz copy goes from ~1.7 MB to ~80 KB of … | none (audio bit-exact; the screen shows the same data). | Confirmed | Implemented |
| MODEL-2 | A time signature change re-sends all 2048 patterns for each knob value: leaked pattern copies, a burst on the audio thread, and a queue that can overflow | memory Medium (removes a leak of up to ~250 KB a knob value), latency … | bit-exact when nothing is dropped today; where a command is … | Confirmed | No change |
| MODEL-3 | Each pattern edit makes two deep copies and an allocation, and knob-driven note edits send one per value | CPU Low to Medium (it matters only for long patterns); fewer … | bit-exact for step 1. Step 2 delays an edit's effect by at … | Confirmed | No change |
| MODEL-4 | A parameter lock ending scans the pattern's Motion backwards from the playhead on the audio thread | latency Low to Medium (removes a worst-case spike on the audio … | bit-exact. | Confirmed | No change |
| MODEL-5 | Kit lanes of their own length cost a quadratic lookup on every tick | CPU Low. | bit-exact. | Confirmed | Implemented |

# Findings in full

Each finding is as its auditor wrote it, with two lines added: what I found when I checked it against the code, and what the fork does about it. Costs are estimates from reading the code unless a line says it was measured.

## Runtime, drivers, power and build

#### Idle wakeup inventory (question 1)

App idle: no notes, static screen, no computer recording, Screen Mirror off, 44.1 kHz. Base threads only; the app's own threads (render pool, pager, loaders, web server) are not counted.

| Thread | Policy | Wakeups per second when idle | What each pass does | Where |
|---|---|---|---|---|
| Audio | FIFO 30 | 344.5 (375 at 48 kHz, 750 at 96 kHz) | 1 capture read, `render()`, 1–3 `snd_pcm_writei` + 1 `snd_pcm_wait` | audio.cpp:117-143 |
| MIDI | FIFO 15 | at least 200 | one non-blocking read per open input port (3 on the device: surface, DIN, USB gadget), `midi_output()`, `lights()`, flush, `ppoll`; once a second a full ALSA port scan | controller.cpp:124-161 |
| Service | normal | about 200 | `poll` 5 ms + 3 more non-blocking reads (sd-bus, jack control, power key), then `App::service()`; four times a second about 15 configfs/sysfs calls | controller.cpp:32-38, 85-100 |
| Screen (main) | normal | 250 input polls + 10 full draws | 3 atomic loads per poll; `App::draw()` and a 307 KB compare per draw | run_app.cpp:127-157 |
| Display worker | normal | 0 (condition variable) | — | display.cpp:119 |
| USB audio out | FIFO 20 | 50, plus 4 ALSA card scans | `sleep_for(20 ms)`; `find_target()` every 250 ms | usb_audio.cpp:287-318 |
| USB audio in | FIFO 20 | 50 + 4 scans, only while the app has USB-C input selected | same loop | usb_audio.cpp:132-156 |
| Screen Mirror worker | normal | 5 when never used; 30 or 60 after the first stream has ended (BASE-13) | `poll` 200 ms | screen_mirror.cpp:533-534 |
| JackDetect, ClockIo, SystemServices, PowerButton, CardShare, mixer | — | no thread of their own | polled by the service thread on every pass | controller.cpp:85-100 |

Total about 1,060 wakeups a second, of which about 345 (audio) are inherent and about 715 are polling. The SoC has no idle state deeper than WFI, so a wakeup costs its active time rather than an expensive idle exit: my estimate is 5–25 µs each (timer interrupt, on PREEMPT_RT a hand-off through the timer softirq thread for normal-priority sleepers, two context switches, the syscalls listed), which puts all the polling together at roughly 1–2 % of one core. Unmeasured. To measure: sample `voluntary_ctxt_switches` in `/proc/<pid>/task/*/status` twice, 10 s apart, and `utime`/`stime` in `/proc/<pid>/task/*/stat`. The base's threads are not named (`pthread_setname_np` is used only by the app's render pool), so naming them first makes this readable.

How each could block instead is in BASE-2 (screen), BASE-6 (service), BASE-7 (MIDI), BASE-9 (USB) and BASE-13 (mirror).

### BASE-1: No CPU power policy: the cluster sits at 1800 MHz and 1.10 V all the time

- **Verification:** Confirmed. No file in the fork mentions cpufreq or a governor (grep). The vendor `az07_defconfig` sets `CONFIG_CPU_FREQ_DEFAULT_GOV_PERFORMANCE=y`, and `rk356x.dtsi` gives 0.90 V up to 1416 MHz and 1.10 V at 1800 MHz. Not checked on the device: the governor in effect at run time.
- **In the fork:** Implemented. A CPU speed setting (Auto, Full, Balanced, Eco) through `Platform::cpu_limit()` in the base, defaulting to Full: untouched, the cores stay at the kernel's 1.8 GHz, as under the stock app; below Full the player has chosen the lower clock (and its lower voltage) to save battery.

- **Where:** nowhere, which is the finding. `grep -rniE "cpufreq|governor|scaling_(max|min|gov)|schedutil"` over the repository (without `.git`, `.local`) returns nothing. The place a policy fits: `base/src/drivers/platform.cpp:108-124` (`Platform::~Platform`, `Platform::brightness`), `base/install/restore-stock.sh:99-104`.
- **Thread and frequency:** whole system, always.
- **Now:** the kernel's default `performance` governor is never changed. The existing pattern for a setting the runtime changes and puts back:
  ```cpp
  Platform::~Platform() {
      if (!changed_backlight.empty()) write_text(changed_backlight / "brightness", saved_brightness);
  }
  ```
- **Cost:** all four cores share one rail, and it stays at 1.10 V even while every core is in WFI. Energy per unit of DSP work scales with V²: 0.90² / 1.10² = 0.67, so the same project costs about a third less CPU energy at the 0.90 V points, and leakage (paid in WFI too) falls with the voltage as well. 1416 MHz is the fastest 0.90 V point; every point below it has the same voltage, so going slower saves nothing more per unit of work. Throughput loss at 1416 MHz: 21 %. Unmeasured: compare supply current with `scaling_max_freq` at 1800000 and 1416000, idle and with a heavy project playing; confirm with `cpufreq/policy0/stats/time_in_state`.
- **Proposal:** a `Platform` method, called from the service thread, restored on exit and in the recovery script:
  ```cpp
  bool Platform::cpu_max_khz(unsigned khz) {            // platform.cpp
      auto file = sys / "devices/system/cpu/cpufreq/policy0/scaling_max_freq";
      if (saved_cpu_max.empty()) saved_cpu_max = read_text(file);
      return !saved_cpu_max.empty() && write_text(file, std::to_string(khz));
  }   // ~Platform(): write saved_cpu_max back
  ```
  ```sh
  # restore-stock.sh, beside the pmdown_time lines
  for P in /sys/devices/system/cpu/cpufreq/policy*; do
      cat "$P/cpuinfo_max_freq" > "$P/scaling_max_freq" 2>/dev/null || true
  done
  ```
  Keep the `performance` governor so the frequency is fixed and no transition happens under the audio thread. The app starts at 1416000 and steps to 1608000 / 1800000 from its own measured render load (it already has one), with hysteresis: up after a few blocks above about 60 % or on any xrun, down after about 10 s below about 35 %. Do not use `schedutil`: by default it requests the maximum frequency whenever a real-time task is runnable, and the FIFO audio and render threads wake 344 times a second, so it would sit at 1800 MHz or flap; and its transitions run in a deadline-class kernel thread that preempts FIFO threads while it talks to the regulator.
- **Gain:** battery High (the largest software lever on SoC power I found).
- **Sound or behaviour change:** bit-exact.
- **Risk:** Medium. 21 % less headroom until the step up; a sudden jump in load can xrun before the step. `platform.h:38` says "No raw GPIO, I2C, regulator ... writes" and HARDWARE.md:60 says "no radio, network or thermal policy changes", so this crosses a boundary the base set for itself and should be opt-in by the app, as brightness is.
- **Effort:** S for a fixed cap, M with the adaptive step.
- **Confidence:** Medium. Not checked: that the vendor kernel lets root write `scaling_max_freq` (standard), how long a voltage step takes, the actual current saved.

### BASE-2: The idle screen loop polls at 250 Hz and redraws the whole screen 10 times a second; busy is always 60 fps

- **Verification:** Confirmed. Read `run_app.cpp:120-158`: 16.7 ms busy interval, 100 ms idle interval after 500 ms without a changed frame, and a 4 ms `sleep_for` poll of three input counters while idle.
- **In the fork:** Implemented in part. The screen thread now sleeps on a futex and is woken by controls, notes from outside and screenshot requests, instead of polling every 4 ms. After a change it draws 8 more frames at falling rates instead of 30 at 60 fps. With no control in use it draws at the app's `frame_interval()` (new setting SCREEN > Refresh, 30 fps by default). A still screen is still drawn 10 times a second: skipping that needs the dependency tracking described under UI-5. Review before the commit: a control used while a frame is being drawn now gets a frame of its own at the fast rate, where before (and in the original) it could wait out a still screen's 100 ms. And while a control has been used in the last 300 ms every frame comes at the fast rate, whether or not the frame changed: what a control does often shows a frame later, and the first version of this loop could then wait 100 ms.

- **Where:** `base/src/runtime/run_app.cpp:127-157` (`run_app`)
- **Thread and frequency:** screen; 250 polls and 10 draws a second idle, 60 draws a second busy.
- **Now:**
  ```cpp
  constexpr std::chrono::microseconds busy_interval {16667}, idle_interval {100000}, idle_after {500000}, input_poll {4000};
  ...
  bool idle = !display || start - last_change > idle_after;
  next = std::max(next + (idle ? idle_interval : busy_interval), ScreenClock::now() - busy_interval);
  if (!idle) { std::this_thread::sleep_until(next); continue; }
  auto seen = inputs();
  while (ScreenClock::now() < next && inputs() == seen && session.running.load() && !interrupted && !session.screenshot.load())
      std::this_thread::sleep_for(input_poll);
  ```
  The app has no way to say "nothing to draw": whether a frame changed is found only by drawing all of it and comparing all of it (`Display::present`).
- **Cost:** idle, forever: 10 × (`App::draw()` of 320 × 240 by JUCE's software renderer + a 307 KB compare) and 250 timer wakeups a second. Busy: 60 full draws a second even when only a play head moves. The draw time is already recorded (`screen_draw_us` in `--ctl status`); if it is 4 ms (unmeasured guess) idle costs 4 % of a core and busy 24 %.
- **Proposal:** three independent steps.
  1. Event-driven wake. Add an eventfd (or a futex word) to `Session`; the MIDI thread signals it where it counts events (controller.cpp:188, 195), the service thread for the power key, `screenshot` and `prompt`. Arm it only while the screen thread waits, so busy MIDI traffic pays no syscall:
     ```cpp
     session.screen_wake.armed.store(true);
     if (inputs() == seen) { pollfd p {session.screen_wake.fd, POLLIN, 0}; poll(&p, 1, ms_until(next)); }
     // signal(): if (armed.exchange(false)) write(fd, &one, 8);
     ```
  2. Let the app report nothing to draw: `virtual bool App::draw_needed()` (default true), checked instead of a full draw on the idle tick, plus a `session().wake_screen()` for app-side changes. Cheaper still, with no API change: `image` is created once (run_app.cpp:129) and keeps the last frame, so an app may return from `draw()` without painting when its own state is unchanged; `present()` then finds nothing changed after one compare. The app must repaint once after a remote prompt (`session().prompting`).
  3. A lower busy cap: 33 ms between frames unless there was input in the last 250 ms (then 16.7 ms, so knob turns stay smooth), or let the app name the interval.
- **Gain:** battery High idle, CPU Medium busy.
- **Sound or behaviour change:** steps 1 and 2 none; step 3 visible (30 fps animation; a full-screen push already takes 54 ms).
- **Risk:** Low to Medium. A wake source that is missed shows as a stale screen until the next safety tick; keep a 250–500 ms tick at first.
- **Effort:** M (step 1 alone is S).
- **Confidence:** High on the mechanism; the draw cost is unmeasured and belongs to the UI audit.

### BASE-3: The backlight never dims or blanks, and the base has no idle signal for it

- **Verification:** Confirmed. The only writer of the backlight is `MelodeeApp::service` applying the Brightness setting (`melodee_app.cpp:529-535`). No idle timer reaches it.
- **In the fork:** Implemented. New setting SCREEN > Dim after (default 2 min): the backlight goes to its lowest level and the pad and button lights low. The level before the first change is restored on exit and by `restore-stock.sh`. A loop still heard in the Looper counts as something playing, so the lights stay up for it. A backlight left dimmed by a killed app is put back when the next one starts (`Platform::recover()`). Both were added in the review before the commit. The Power button ends the dimmed state as any other control does.

- **Where:** `base/src/drivers/platform.cpp:111-124` (`Platform::brightness`); used only by `src/melodee/melodee_app.cpp:529-535` and the `brightness` command (commands.cpp:62).
- **Thread and frequency:** service; on a settings change only.
- **Now:**
  ```cpp
  unsigned screen = shell.screen();
  if (screen && screen != screen_applied) {
      auto backlight = hardware.platform.backlight();
      if (backlight.maximum > 0) hardware.platform.brightness(unsigned(std::max(1L, backlight.maximum * long(screen) / 100)));
      screen_applied = screen;
  }
  ```
  The setting is 20–100 %, default 100 % (settings.h:88). I found no idle timer, dimmer or blanking anywhere (grep for dim, blank, idle, standby in src/melodee and base/src). The pad and button LEDs have none either.
- **Cost:** the backlight and the 16 RGB pad LEDs are probably the largest loads after the SoC; a 2.4 inch panel's backlight is typically 0.15–0.25 W at full drive. Unmeasured: read the supply current at brightness 6, 1 and 0, and with `led off`.
- **Proposal:** the mechanism exists; add the policy. In the app's `service()`: when `session().hardware_events` has not moved for N minutes (and the transport is stopped), `platform.brightness(1)`, later 0; restore on the first event. A small base addition makes the LEDs follow: a dim factor in `SurfaceLights::update()` applied to RGB values before they are sent (leds.h:139-141), set by the same timer. The panel keeps its picture from its own memory while dark, so no redraw is needed on wake. A full blank through DRM (disabling the CRTC also switches the backlight off in mipi-dbi) is not needed.
- **Gain:** battery High (estimate).
- **Sound or behaviour change:** visible; make it a setting.
- **Risk:** Low. Whether level 0 switches the LED driver fully off is unknown. `Platform` restores brightness only on a normal exit (platform.cpp:108-110); after a crash a dimmed level would stay until the stock app sets its own, so add it to restore-stock.sh.
- **Effort:** S (backlight), M (with LED dimming).
- **Confidence:** Medium: the absence is certain, the saving is not measured.

### BASE-4: MIDI clock or Active Sensing from an external device keeps the screen at 60 fps

- **Verification:** Confirmed. `Controller::midi_packet` increments `external_events` for every external packet, clock and Active Sensing included (`controller.cpp:195`), and the screen loop counts that as input (`run_app.cpp:128, 157`).
- **In the fork:** Implemented. MIDI clock, Active Sensing and MTC quarter frames no longer wake the screen. Notes and controllers from outside still do, and so do Start, Continue and Stop, so the screen shows a transport started from outside at once.

- **Where:** `base/src/runtime/controller.cpp:195-197` (`Controller::midi_packet`), `base/src/runtime/run_app.cpp:128, 154-157`
- **Thread and frequency:** screen; 60 draws a second for as long as the device is connected.
- **Now:** every external packet counts as input, real-time bytes included (`MidiStreamParser::feed` emits each byte ≥ 0xF8 as a packet, surface/midi.h:24-27):
  ```cpp
  ++session.external_events;
  if (packet.size == 1 && packet.bytes[0] == 0xf8) ++session.clocks;
  app.midi(port, packet);
  ```
  ```cpp
  auto inputs = [&] { return session.hardware_events.load() + session.external_events.load() + session.synthetic_events.load(); };
  ...
  if (inputs() != seen) { last_change = ScreenClock::now(); next = last_change; }
  ```
  Setting `last_change` makes the loop "busy" for `idle_after` (500 ms). Active Sensing arrives every 300 ms or less and MIDI clock 48 times a second at 120 BPM, so the idle rate is never reached again: a static screen is drawn 60 times a second (about 40 on average with Active Sensing alone).
- **Cost:** 4–6 times the idle draw work of BASE-2 whenever a keyboard that sends Active Sensing, or any clock source, is plugged in. Unmeasured; check `screen_presented` against time in `--ctl status` with such a device attached.
- **Proposal:** a separate wake counter that leaves out system real-time messages:
  ```cpp
  ++session.external_events;
  if (packet.size != 1 || packet.bytes[0] < 0xf8) ++session.screen_events;   // run_app's inputs() reads this one
  ```
  `external_events` stays as the diagnostic it is in the status line.
- **Gain:** battery Medium (only with such a device attached).
- **Sound or behaviour change:** none.
- **Risk:** Low. An app that shows incoming clock on screen still redraws on the idle tick.
- **Effort:** S.
- **Confidence:** High on the mechanism. Not checked: whether the control surface itself sends anything periodically (see "Could not determine").

### BASE-5: Build flags: no `-mcpu`, math functions keep errno, no eager binding, no section garbage collection

- **Verification:** Confirmed. `base.mk:21-23`: `-O2` only, plain `$(CXX) $(OBJECTS) $(LDLIBS)` link.
- **In the fork:** Implemented in part. `-mcpu=cortex-a55`, `-fno-trapping-math` and `-Wl,-z,now` are on. **`-fno-math-errno` is left out: measured, it changes the DSP bench's output**, so the auditor's "bit-exact expected" was wrong for that flag. With the two kept flags the bench output and the self-test audio are identical to the original build's. Section garbage collection was not tried. Second pass: `-O3` for the engine was measured too. It ran 4 % fewer instructions and changed the bench's output, so the engine stays at `-O2`.

- **Where:** `base/base.mk:21-23, 36`
- **Thread and frequency:** every thread; build time.
- **Now:**
  ```make
  CXXFLAGS := -std=c++17 -O2 -pthread -MMD -MP
  CFLAGS := -O2 -MMD -MP
  ...
  	$(CXX) $(OBJECTS) $(LDLIBS) -o $@
  ```
  There is no `LDFLAGS` at all. tools/stems/build.py:38 already builds the stem tool with `-mcpu=cortex-a55`.
- **Cost:** (a) GCC 12 for generic ARMv8-A tunes for an out-of-order core and calls out-of-line helpers for every atomic read-modify-write (`-moutline-atomics` is the default); the A55 is in-order and has the ARMv8.1 atomics. (b) With errno kept, `lround`/`lrint` are library calls instead of one `fcvtas` instruction, and `sqrt` cannot be hoisted or vectorised; the engine has about 75 `lround`/`lrint` sites and 28 `sqrt` sites (grep count; which are per sample is for the engine audit). (c) GCC 12's `-O2` vectoriser uses the `very-cheap` cost model, which refuses loops that need a runtime alias check or a scalar tail, that is, most `float*` block loops. (d) Unused JUCE code stays in the binary, which lives in RAM (tmpfs) and is hashed three times at boot (BASE-11). Unmeasured; the app's own DSP benchmark (src/melodee/dsp_bench.cpp) and `screen_draw_us` are the measures.
- **Proposal:**
  ```make
  ARCH_FLAGS ?= -mcpu=cortex-a55
  CXXFLAGS := -std=c++17 -O2 $(ARCH_FLAGS) -fno-math-errno -fno-trapping-math -ffunction-sections -fdata-sections -pthread -MMD -MP
  CFLAGS := -O2 $(ARCH_FLAGS) -ffunction-sections -fdata-sections -MMD -MP
  LDFLAGS ?= -Wl,--gc-sections -Wl,-z,relro -Wl,-z,now
  # link: $(CXX) $(OBJECTS) $(LDFLAGS) $(LDLIBS) -o $@
  ```
  Then try `-fvect-cost-model=dynamic` on the engine and `base/src/dsp` objects only, and measure. `-fno-plt` is not worth it once `-z now` is there. Not proposed: `-ffast-math` (reorders arithmetic), `-O3` everywhere (code growth on a 32 KB instruction cache; build.py notes it measured no faster for the stem tool), LTO (see below).
- **Gain:** CPU Medium (a few percent expected; more where `lround` sits in a per-sample loop), memory and load time Low.
- **Sound or behaviour change:** `-fno-math-errno`, `-fno-trapping-math`, `-mcpu`, section GC and `-z now` do not change any arithmetic result by themselves: bit-exact expected. One caveat applies to every codegen change: C++ builds with `-ffp-contract=fast` by default (the ISO-mode exception is for C only), so the compiler already fuses multiply-adds wherever inlining puts them side by side, and a change in inlining can move one. Confirm with a render hash before and after (tests/melodee.cpp already has `print_hash`); if it differs the change is rounding only. `-fvect-cost-model=dynamic`: same expectation, same check. LTO: rounding-only risk is real (cross-file inlining creates new fusions); treat as a separate, measured step.
- **Risk:** Low. `-mcpu=cortex-a55` emits ARMv8.2 instructions, so the self-test that build.sh runs in the container needs an ARMv8.2 host (Apple Silicon is; an older arm64 CI machine is not); `ARCH_FLAGS` can be overridden with `-mtune=cortex-a55` there.
- **Effort:** S.
- **Confidence:** Medium. Nothing was compiled; the `lround` and vectoriser statements are from GCC's documented behaviour, not from this build's assembly.

### BASE-6: The service thread wakes 200 times a second and makes four syscalls each time for sources that could all be waited on

- **Verification:** Confirmed. `Controller::run` loops on `server.serve(5, …)` and calls `poll_platform()` on every pass (`controller.cpp:32-38, 85-100`).
- **In the fork:** Not implemented. Several of Melodee's timers count service passes (for example `++ticks % 600` for 3 s in `ProjectStore` and `SampleLibrary`), so a slower loop needs each of them changed to clock time first.

- **Where:** `base/src/runtime/controller.cpp:32-38` (`Controller::run`), `85-100` (`poll_platform`), `base/src/runtime/control_socket.cpp:39-43`
- **Thread and frequency:** service every 5 ms.
- **Now:**
  ```cpp
  while (session.running.load()) {
      record_surface_packets();
      MidiCatalog update;
      while (catalog_updates.pop(update)) catalog = std::move(update);
      poll_platform(session);
      server.serve(5, [&](const std::string& request) { return command(session, request); });
  }
  ```
  Per pass: `poll()` on the control socket with a 5 ms timeout, one non-blocking socket read inside `sd_bus_process()` (system_services.cpp:72), one `snd_ctl_read()` on the jack control (jacks.cpp:56), one `read()` on the power key's input device (power_button.cpp:50), then `App::service()`. Four times a second `CardShare::poll()` adds `opendir`/`readdir`, two or three file reads and directory listings of configfs (card_share.cpp:96-109). `ClockIo::poll()` and the mixer's speaker hold only take a mutex.
- **Cost:** about 200 wakeups and 800 syscalls a second, each wake through the timer softirq thread on PREEMPT_RT because the thread is not real-time. Estimated 0.3–0.6 % of a core before the app's own `service()` work. Unmeasured.
- **Proposal:** one `poll()` over every source, with a wake eventfd for the app and a longer default tick:
  ```cpp
  pollfd fds[] {{server.fd(), POLLIN, 0}, {services.fd(), POLLIN, 0}, {jacks.fd(), POLLIN, 0},
                {power.fd(), POLLIN, 0}, {clock.fd(), POLLIN, 0}, {session.service_wake.fd, POLLIN, 0}};
  poll(fds, 6, app.service_interval_ms());   // 5 today; 50 when the app has nothing queued
  ```
  (`sd_bus_get_fd`, `snd_ctl_poll_descriptors`, the evdev and GPIO event fds are all pollable; poll ignores a negative fd.) Timed duties bound the timeout: the speaker hold's `until`, CardShare's 250 ms follow (1 s would do while the card is home). `App::service_interval_ms()` defaults to 5 so other apps keep today's behaviour; Melodee returns a longer one unless a request is pending, and other threads call `session().wake_service()` when they queue one.
- **Gain:** battery Medium.
- **Sound or behaviour change:** none if the wake is used; otherwise slow requests are picked up to 50 ms later.
- **Risk:** Medium. `App::service()` is documented as "every few milliseconds" (APPS.md:101) and Melodee does a lot there (saves, loads, looper memory, card requests, the power key); each needs the wake or must tolerate the longer tick.
- **Effort:** M.
- **Confidence:** Medium. The syscall inside `sd_bus_process()` is from my knowledge of libsystemd, not verified on the device's version.

### BASE-7: The MIDI thread runs the lights tick every 5 ms and reads every port on every pass

- **Verification:** Confirmed. `run_midi` sets `lights_due = now + 5 ms` and sleeps until the earliest of lights, app output, audition release and port scan (`controller.cpp:153-161`).
- **In the fork:** Not implemented. See LIGHT-3.

- **Where:** `base/src/runtime/controller.cpp:123-161` (`Controller::run_midi`), `base/src/drivers/hardware_midi.cpp:118-139` (`HardwareMidi::wait`, `read`)
- **Thread and frequency:** MIDI every 5 ms (FIFO 15).
- **Now:**
  ```cpp
  if (now >= lights_due) {
      app.lights(lights, now);
      lights.update(send_surface, 48);
      lights_due = now + std::chrono::milliseconds(5);
  }
  midi.flush();
  auto until = std::min({lights_due, output.wake, release, scan});
  midi.wait(std::chrono::duration_cast<std::chrono::nanoseconds>(until - Clock::now()).count());
  ```
  After each wake `midi.read()` calls `snd_rawmidi_tread()`/`read()` on every open input whether or not `ppoll` reported it ready (the `revents` are thrown away at hardware_midi.cpp:129).
- **Cost:** 200 wakeups a second with 1 `ppoll` + 3 failed reads each on the device's three ports: about 800 syscalls a second, plus `App::lights()` and `App::midi_output()` 200 times a second. LEDs do not need 200 updates a second. Unmeasured.
- **Proposal:** (1) lights at most every 5 ms after input, otherwise every 20 ms: keep `last_lights`, use `spacing = input_this_pass ? 5 ms : 20 ms`. A pad hit then lights as fast as today, and the idle rate falls from 200 to 50 a second. (2) Keep the `pollfd` array from `wait()` and read only ports whose `revents` are set (all of them on the once-a-second scan pass). Timed MIDI output is not affected: the app already schedules it with `wake_at()` (midi_io.cpp:600, 630).
- **Gain:** battery Medium to Low.
- **Sound or behaviour change:** none for sound; LED animation steps at 50 Hz instead of 200 Hz when nothing is touched.
- **Risk:** Low to Medium. APPS.md:98-99 promises `lights()` every 5 ms and `midi_output()` at least every 5 ms, and Melodee also drains its remote-control queues in `lights()` (melodee_app.cpp:166-173), which would then wait up to 20 ms. Document the new contract.
- **Effort:** S.
- **Confidence:** High on the counts; the cost of Melodee's `lights()` itself is outside my area.

### BASE-8: Real-time hazards on the audio thread: lazy symbol binding, no locked memory, priority under every IRQ thread

- **Verification:** Confirmed. The link line has no `-Wl,-z,now`; `mlockall` appears nowhere in the fork; the audio thread asks for `SCHED_FIFO` 30 (`audio.cpp:109-110`) while PREEMPT_RT interrupt threads default to 50.
- **In the fork:** Implemented in part. Eager binding (`-Wl,-z,now`) is on. Memory locking and the audio thread's priority are unchanged: both need a dropout test on the device.

- **Where:** `base/base.mk:36` (link line), `base/src/drivers/audio.cpp:109-111` (`AudioDevice::run`), whole repository for `mlockall` (absent)
- **Thread and frequency:** audio per block; the hazards are rare events, not steady cost.
- **Now:**
  ```cpp
  sched_param priority {}; priority.sched_priority = 30;
  int scheduling = pthread_setschedparam(pthread_self(), SCHED_FIFO, &priority);
  ```
  (a) The binary is linked without `-z now`, and Debian's GCC does not add it, so each shared-library function is resolved by the dynamic loader on its first call. For the audio thread that means the first `sinf`, `expf` or `snd_pcm_prepare` (the xrun path) walks the libraries' symbol tables from inside `render()` or recovery. (b) `grep mlockall` finds nothing; only the app's stream pager uses `mlock` on its own chunks. The binary runs from tmpfs and cannot be evicted, but the shared libraries' code (libc, libm, libstdc++, libasound) is file-backed on eMMC and may be reclaimed under memory pressure, since without swap file pages are all the kernel can reclaim. A fault on such a page from the audio thread waits for eMMC. (c) No CPU affinity is set in the base (the app's render pool can pin; `render_pinned_default` is false). (d) Audio at FIFO 30, USB audio at 20, MIDI at 15 are all below the kernel's threaded IRQ handlers (50), so USB (audio, MIDI and network on one controller), SD/eMMC, SPI and UART interrupt threads can each preempt the audio thread.
- **Cost:** none in steady state; each is a possible dropout. Unmeasured. Check xruns while copying files over USB networking and while streaming samples from the card, and after loading enough samples to push memory low.
- **Proposal:** (a) `-Wl,-z,relro -Wl,-z,now` (in BASE-5): certain, free. (b) `mlockall(MCL_CURRENT | MCL_ONFAULT)` once the threads and libraries are up. Not `MCL_FUTURE`: it would lock the pager's sample mappings and defeat its `MADV_DONTNEED` (stream_pager.cpp:48). Plain `MCL_CURRENT` without `MCL_ONFAULT` would populate every thread's 8 MB stack. (c) and (d): the priorities are a sound conservative default (the audio thread can never starve an interrupt thread) and I would not change them blind. If xruns follow USB or card activity, raise the audio and render threads above the unrelated IRQ threads and the I2S/DMA IRQ thread above both, as rtirq setups do.
- **Gain:** latency (dropout risk) Medium for (a), Low to Medium for (b).
- **Sound or behaviour change:** bit-exact.
- **Risk:** (a) Low: a few milliseconds more at start. (b) Medium: locks resident library pages (tens of MB at most); plug-in libraries opened later are not covered.
- **Effort:** S.
- **Confidence:** High that binding is lazy and nothing is locked; Low on how often either actually bites.

### BASE-9: The USB audio threads poll every 20 ms and scan the sound cards four times a second while no computer streams

- **Verification:** Confirmed. `UsbAudioOutput::run` sleeps 20 ms per pass with no stream and calls `find_target()` every 250 ms (`usb_audio.cpp:287-318`). `push()` returns at once while no computer records (`usb_audio.cpp:230`), so the audio thread pays nothing.
- **In the fork:** No change.

- **Where:** `base/src/drivers/usb_audio.cpp:286-318` (`UsbAudioOutput::run`), `131-156` (`UsbAudioInput::run`), `66-98` (`find_target`, `find_uac2_gadget`, `uac2_rate`)
- **Thread and frequency:** USB audio out, always (it is started unless `--usb-output off`, run_app.cpp:95); USB audio in while the app has USB-C input selected. 50 wakeups and 4 scans a second each.
- **Now:**
  ```cpp
  if (now >= next_check) {
      next_check = now + std::chrono::milliseconds(250);
      Target target = find_target(requested, "Playback Rate");
  ...
  if (!playback.pcm) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }
  ```
  `find_target()` walks every sound card with `snd_ctl_open()` (alsa-lib re-checks its configuration files and builds the device definition on each open), reads the card info, then opens the gadget's control device again to read one value.
- **Cost:** 50 wakeups a second on a FIFO 20 thread that has nothing to do, and about 4 × (5–8 control-device opens) a second; I estimate 0.5–1 ms of CPU a second. Unmeasured.
- **Proposal:** (1) small: while closed, wait until `next_check` on a condition variable that `stop()` notifies, not 20 ms at a time: 4 wakeups a second, and `stop()` still returns at once. (2) better: keep one control handle open on the gadget card, `snd_ctl_subscribe_events()`, and `poll()` its descriptor; mainline u_audio notifies the "Playback Rate"/"Capture Rate" controls when the computer starts or stops. Fall back to a 1 s re-scan when the card is gone (a gadget rebind).
- **Gain:** battery Low to Medium.
- **Sound or behaviour change:** none; recording start is detected at least as fast.
- **Risk:** Low for (1). For (2), the notification must exist in the vendor's 6.12 kernel.
- **Effort:** S for (1), M for (2).
- **Confidence:** High for the polling; Medium for the event (not checked against the vendor kernel source).

### BASE-10: While a computer records, the converter filters all 18 channels, silent stems included

- **Verification:** Confirmed. `SincResampler::feed` mixes every channel for every output frame with no test for silent ones (`frame_ring.h:166-168`). Only while a computer records.
- **In the fork:** No change.

- **Where:** `base/src/dsp/frame_ring.h:143-175` (`SincResampler::feed`), `base/src/drivers/usb_audio.cpp:249, 264-270` (`UsbAudioOutput::run`)
- **Thread and frequency:** USB audio out, per output frame, only while a computer records.
- **Now:**
  ```cpp
  for (unsigned j = 0; j < taps; ++j) {
      double at = std::abs(first - j * scale);
      unsigned point = unsigned(at);
      weights[j] = kernel[point].value + float(at - point) * kernel[point].slope;
      sum += weights[j];
  }
  std::array<float, Channels> mixed {};
  for (unsigned j = 0; j < taps; ++j)
      for (unsigned c = 0; c < Channels; ++c) mixed[c] += weights[j] * window[Channels * j + c];
  ```
  With equal nominal rates `taps` is 64, so each output frame costs 64 weight computations in double precision and 64 × 18 = 1,152 multiply-adds, whatever the gadget carries (`carried` at usb_audio.cpp:270 only limits the encoding) and however many stems are silent.
- **Cost:** about 51 million multiply-adds and 2.8 million weight computations a second at 44.1 kHz; I estimate 5–8 % of one core. Unmeasured.
  What is already cheap: when no computer records, `push()` returns on its first line (`if (!accepting.load(...)) return;`, usb_audio.cpp:230), so the audio thread pays one atomic load, and the output thread only polls (BASE-9). Note for the engine audit: the app still hands `render()` its stem block on every block regardless (melodee_app.cpp:137).
- **Proposal:** skip a channel whose whole window is zero. Count consecutive zero input samples per channel while copying the frame into the history (18 compares a frame); when `quiet[c] >= taps`, leave `mixed[c]` at 0. The filter would have produced exactly 0 too, so the encoded PCM is identical. Also filter only the channels the open stream carries (`min(playback.channels, channels)`), which matters with `--usb-stems off`.
- **Gain:** CPU Medium while recording (proportional to unused tracks); battery none otherwise.
- **Sound or behaviour change:** bit-exact at the encoded output.
- **Risk:** Low.
- **Effort:** M (the per-channel skip breaks the inner loop's simple shape; check it still vectorises).
- **Confidence:** Medium: counts are exact, the CPU share is an estimate.

### BASE-11: Boot: the binary is hashed three times, the stock binary once, and a six-second render self-test runs on every start

- **Verification:** Confirmed. `sd-worker.sh` hashes the stock binary, checks `SHA256SUMS` on the card, copies to RAM and hashes both copies again, then runs `--self-test` before every start.
- **In the fork:** No change.

- **Where:** `base/install/sd-worker.sh:10-11, 22-25, 44`; the self-test is `src/melodee/tools.cpp:2879-2928` (`sound`)
- **Thread and frequency:** on every boot, before the stock app is stopped.
- **Now:**
  ```sh
  [ "$(sha256sum '/usr/bin/MPC Sample' | cut -d ' ' -f 1)" = "$EXPECTED" ] || { ...
  (cd "$DIR" && sha256sum -c SHA256SUMS) || { ...
  ...
  (cd "$DIR" && cp $FILES "$RELEASE/")
  (cd "$DIR" && sha256sum $FILES) | (cd "$RELEASE" && sha256sum -c -)
  chmod 755 "$RELEASE/sample-base"
  "$RELEASE/sample-base" --self-test "$RELEASE/test.wav" || { ...
  ```
  What is not slow: only the binary and two scripts are copied to RAM. The stems program and its model stay on the card and are not hashed at boot (`SHA256SUMS` covers the five top-level files only, tools/make_release.py:94). The release binary is stripped (release.sh:9).
- **Cost:** three SHA-256 passes over the Melodee binary (card copy, card copy again, RAM copy) and one over the stock binary; a second process start; an offline render of 6 s of audio (2,067 blocks) with its own engine and a 1 MB WAV written to RAM (never removed); a `systemctl daemon-reload`. I cannot put seconds on it: the binary's size is not in this checkout and `sha256sum`'s speed depends on whether the device's build uses the CPU's SHA instructions. Measure with timestamps in `/run/melodee-sd.log` or `journalctl -o short-monotonic -u sample-base-sd-boot -u sample-base`.
- **Proposal:** (1) copy first, then check the RAM copy against the card's `SHA256SUMS` (still verify the two scripts on the card): one hash of the binary for the same guarantee, not three. (2) Run the self-test once per binary: after it passes, write the binary's hash to a stamp file beside it on the card and skip the test while the stamp matches the hash just verified; sd-update.sh already self-tests a new binary before installing it (line 227). (3) `rm` the WAV after the test.
- **Gain:** load time Medium (estimate: one to a few seconds of the Melodee-specific part; the wait for the stock app to boot first is untouched).
- **Sound or behaviour change:** none.
- **Risk:** (1) Low. (2) Medium: it weakens "on any doubt the stock app keeps running" slightly (a binary that passed once is trusted on the same hash), and adds one small card write per version.
- **Effort:** S.
- **Confidence:** Medium: the redundancy is certain, the time is unmeasured.

### BASE-12: A changed frame is compared twice and copied whole

- **Verification:** Confirmed. `Display::present` compares each row with `memcmp` and copies changed rows, then copies the whole frame into `pending`; the worker's `display_damage::plan` compares `pending` with `shown` again (`display.cpp:90-101, 138`).
- **In the fork:** No change.

- **Where:** `base/src/drivers/display.cpp:90-94, 101` (`Display::present`), `138` (`Display::push`), `base/src/drivers/display_damage.h:56-60` (`plan`)
- **Thread and frequency:** screen and display worker, per changed frame (up to 60 a second).
- **Now:**
  ```cpp
  for (int row = 0; row < height; ++row) {
      auto* line = bitmap.getLinePointer(row);
      auto* kept = latest.data() + row * width;
      if (memcmp(kept, line, width * 4) != 0) { memcpy(kept, line, width * 4); same = false; }
  }
  ...
  pending = latest;
  ```
  then the worker's `plan()` runs `memcmp` over all 240 rows again, `pending` against `shown`. `present()` waits until the worker is neither queued nor busy before it queues, so at that moment `shown` equals the previous `latest`: the worker's row pass finds exactly the rows `present()` just found.
- **Cost:** per changed frame, a second pass over 614 KB and a 307 KB copy under the lock that are not needed; about 0.2–0.5 ms, so up to 1–3 % of a core at 60 fps. An unchanged frame costs only the one compare (about 0.1–0.3 ms, 10 times a second when idle): negligible beside the draw that precedes it, and skipped entirely once the app can say nothing changed (BASE-2). Unmeasured.
- **Proposal:** `present()` records the first and last changed row, copies only those rows into `pending`, and hands the span to the worker; `plan()` takes a row range and scans only it. Full range on the first frame.
- **Gain:** CPU Low.
- **Sound or behaviour change:** none (the same rectangles are sent).
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** High on the redundancy, Medium on the size of the saving.

  The rest of the path, for question 4: the worker copies changed rectangles into a 32-bit dumb buffer (turned to portrait pixel by pixel for the base's own driver) and calls `drmModeDirtyFB` once per rectangle. In the kernel, the stock mipi-dbi helpers convert the clip to RGB565 and send three window commands plus the pixels by SPI at 25 MHz; a whole screen is 153,600 bytes, 49 ms on the wire, which matches the measured 54 ms. Nothing is sent when nothing changed; the panel refreshes from its own memory.

### BASE-13: Screen Mirror's frame timer is never disarmed, so its thread keeps waking 30 or 60 times a second after a stream ends

- **Verification:** Confirmed. `stream_on` arms the timerfd (`screen_mirror.cpp:344-346`); `stream_off` never disarms it (`:369-380`), and the worker's `poll` includes the timer (`:533`).
- **In the fork:** Implemented. The frame timer is disarmed in `stream_off()` and `close_device()`.

- **Where:** `base/src/drivers/screen_mirror.cpp:344-346` (`stream_on`), `369-380` (`stream_off`), `252-257` (`close_device`), `533-546` (`run`)
- **Thread and frequency:** Screen Mirror worker; 30 or 60 a second from the first stream until the app exits.
- **Now:**
  ```cpp
  long tick_ns = long(format.interval()) * 100;
  itimerspec every {{0, tick_ns}, {0, tick_ns}};
  timerfd_settime(timer, 0, &every, nullptr);
  ```
  This is the only `timerfd_settime` in the file. `stream_off()`, `close_device()` and `apply()` leave the periodic timer running; the loop then wakes on it, reads it and does nothing because `streaming` is false.
- **Cost:** 30 or 60 pointless wakeups a second after anyone has used Screen Mirror once, even with it switched off again.
- **Proposal:** disarm in `stream_off()` and `close_device()`: `itimerspec none {}; timerfd_settime(timer, 0, &none, nullptr);`. While at it, the 200 ms poll with the mirror off can wait on an eventfd that `set_mode()` writes.
- **Gain:** battery Low.
- **Sound or behaviour change:** none.
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** High.

### BASE-14: The MIDI thread enumerates every sound card once a second

- **Verification:** Confirmed. `run_midi` calls `midi.rescan()` once a second (`controller.cpp:129-134`).
- **In the fork:** No change.

- **Where:** `base/src/runtime/controller.cpp:129-134` (`run_midi`), `base/src/drivers/hardware_midi.cpp:25-65, 83-108` (`enumerate`, `rescan`)
- **Thread and frequency:** MIDI (FIFO 15), once a second.
- **Now:**
  ```cpp
  if (now >= scan) {
      if (midi.rescan()) app.midi_lost();
      require(!surface_required || midi.has_surface(), "Control surface disconnected");
      MidiCatalog update {midi.json(), midi.outputs(), midi.open_ports()}; catalog_updates.push(update);
      scan = now + std::chrono::seconds(1);
  }
  ```
  `rescan()` opens the control device of every card through alsa-lib, queries each raw MIDI device, and the catalogue is rebuilt as JSON and string vectors and copied to the service thread, changed or not.
- **Cost:** an estimated 0.5–2 ms once a second on the thread that reads the pads; a pad hit that arrives during the scan is handled that much later (its kernel time stamp is still right). Unmeasured.
- **Proposal:** add an inotify descriptor for `/dev/snd` (`IN_CREATE | IN_DELETE`) to the `ppoll` set and rescan only when it fires, with a slow safety rescan (every 30 s); push a new catalogue only when `generation()` moved.
- **Gain:** latency Low, battery Low.
- **Sound or behaviour change:** none.
- **Risk:** Low.
- **Effort:** M.
- **Confidence:** Medium (scan time estimated).

### BASE-15: Cross-area bug: the MIDI thread can spin at full speed while a SysEx goes out during playback

- **Verification:** Merged. Same issue as LIGHT-5.
- **In the fork:** See LIGHT-5.

## Audio engine: the render graph

### HOST-1: Track, bus, loop and master chains never rest; a silent track costs its full strip every block

- **Verification:** Confirmed. `run_chain` runs every packed slot of a chain with no test of its input (`engine_host.cpp:1305-1317`), and `chain_of` calls it for every track that is not a frozen track asleep (`:2891`).
- **In the fork:** Implemented in part. Third pass. Steps 1 and 2. A chain that has heard and given silence for twice the longest delay line (6 s at 48 kHz), with every effect in it left by each of those blocks as `Effect::rest()` leaves it, is no longer run; its effects' swings and write places move on exactly, so it wakes where it would have been. It wakes on the first block that brings sound or in which an effect, the tempo or the rate changed. Silence is anything under 2^-95 (-571 dB). A track or bus that gives exactly nothing is not mixed. Measured: a stopped project's block falls from 767,000 instructions to 181,000 once its chains rest; the bench's output is the same to the bit with resting on and off, across silences of 30 to 400 s and the sound that ends them. Chains with a plug-in or a keyed effect never rest, nor kinds that keep sound in memory (Looper). Step 3 (the master section) is not done. See "Third pass".

- **Where:** `src/melodee/engine/engine_host.cpp:1305` (`run_chain`), `:2863-2911` (`chain_of`), `:2966-2997` (mix), `:3044-3062` (bus chains and returns), `:3108-3120` (master).
- **Thread and frequency:** audio thread and render workers, per block, per track that has an engine, per bus with a chain.
- **Now:** every effect in a chain is processed whatever goes in:
  ```cpp
  void EngineHost::run_chain(unsigned chain, float* left, float* right, unsigned frames) {
      for (unsigned k = 0; k < max_slots; ++k) {
          auto& effect = fx[chain][k];
          if (effect.kind() == EffectKind::None) break; // slots are packed from the front
          ...
          effect.process(left, right, frames);
  ```
  `Effect::process` rests only when bypassed (`effects.cpp:622`, `if (bypass && on <= 0) { resting = true; return; }`); it has no test of its input or its tail, and a case-insensitive grep for quiet, silent, idle or sleep finds nothing in `effects.cpp` or `effects_extra.cpp`. `chain_of` always runs the level/pan stage (`:2878-2885`) and `run_chain` (`:2891`); the mix loop (`:2982-2989`) is skipped only for a muted track (`:2966`). Knob FX counts as fed by routing, not by signal (`:2980`, `knob_fed |= !slot.knob_share.silent();`), so it runs every block while a picked track exists. A plain MIDI track (`MidiEngine::render` writes zeros) pays the whole strip too.
  The project already rests four other places by the same idea: kit pad chains (`sampler.h:367`, 1.5 s under 1e-5), Knob FX after its targets let go (`engine_host.h:876`, `engine_host.cpp:3065-3077`), plug-ins (`plugin/idle_gate.h:21-22`, 0.5 s under 1e-5) and frozen tracks' engines (`engine_host.cpp:2895-2899`).
- **Cost:** unmeasured. Per silent track without effects I estimate 3–5 µs a block (two 128-frame scalar loops of about 10 and 28 floating-point operations a frame, plus the sampler's idle render). Each effect adds roughly 5–25 µs. For the bench's project when stopped (32 track effects, 4 bus effects) that is about 0.3–0.6 ms of CPU a block, 11–22 % of one core at 48 kHz, for silence. Because that is above 150 µs, the three workers are also woken every block (HOST-2). Measure on the device: load a project with effects, stop, wait ten seconds, read `melodee_render_load` from `status` and `track_load=` from `melodee-freeze status`; or give `--melodee-dsp-bench` a mode that never sends Play.
- **Proposal:** three steps, each useful alone.
  1. *Silent track, exact.* After `chain_of`, note whether the track's block is all zero bits (a 256-word OR, or a flag from the engine plus `print_fed`). If so skip the mix loop, the sends, the stem write and set `level = 0`. Do the same for a bus nothing was sent into (skip its returns add) and for Knob FX input.
  2. *Chain rest.* Per chain keep `quiet` frames and `asleep`. Input all zero and output peak under 1e-5 for longer than the longest delay line: stop calling `run_chain`. Any non-zero input sample wakes it for that block. Leave chains holding a plug-in to the plug-in's own gate. Use the same rule for the four bus chains (fed = any send or pad send this block), the loop chains and the master chain.
  3. *Master section.* When nothing fed the master and its chain is asleep: skip the ten punches once they have taken `Punch::history` frames (192000) of zeros (their history is then all zero, which is exactly the state `Punch::clear()` gives; call `clear()` on waking), skip the compressor and write the output block as zeros.
- **Gain:** battery High; CPU High while stopped or in sparse arrangements; no gain at full load.
- **Sound or behaviour change:** steps 1 and 3 bit-exact (they skip additions of +0.0). Step 2 is not bit-exact: tails under -100 dBFS are dropped after the quiet window and free-running LFO phases pause while asleep; inaudible, and the rule the kit pad chains and Knob FX already follow.
- **Risk:** Medium. The quiet window must be longer than any delay in the chain. The existing 1.5 s is not: a synced echo may be `1/1` (`effect_info.cpp:150`, 4 quarters) and its line reaches `DelayPool::half - 64` frames (`effects.cpp:461`), 3.26 s at 44.1 kHz. Use `DelayPool::half` frames plus a margin. (The same gap exists today at 44.1 and 48 kHz: Knob FX stops after 1.5 s of quiet and only its 30 s path clears the effect, `engine_host.cpp:3075-3076`, so a repeat later than 1.5 s is lost and what the line still holds can play when it is next fed. The kit pad chains use the same rule, `sampler.cpp:1762-1767`, which I did not trace further. For the effects and sampler auditors.) An effect that makes sound from silence (the Looper effect, a self-oscillating echo) keeps the output loud, so it never sleeps. Keep `compensate()` running while compensation is on.
- **Effort:** step 1 S; step 2 M; step 3 M.
- **Confidence:** High that all of this work runs on silence (read every line); Medium on the size (no measurement). Not checked: each effect kind's behaviour on zero input.

### HOST-2: The pool goes to four cores at 150 µs of track work, about 5 % of a block

- **Verification:** Confirmed. `parallel` needs only `track_work_ns >= parallel_from_ns` (`engine_host.cpp:2669`), and the app sets that to 150,000 ns (`melodee_app.cpp:86`).
- **In the fork:** Not implemented. The threshold was set by the author from measurements on the device (`docs/ROADMAP.md`, P2); changing it needs the same.

- **Where:** `src/melodee/melodee_app.cpp:86`; `src/melodee/engine/engine_host.cpp:2669-2674`; `src/melodee/engine/render_pool.cpp:61-66`, `:125-133`.
- **Thread and frequency:** audio thread per block; three worker wake-ups per block while parallel (1034, 1125 or 2250 a second at 44.1, 48, 96 kHz).
- **Now:**
  ```cpp
  host.set_render_threads(workers, 150000, pinned);                    // melodee_app.cpp:86
  bool parallel = workers.workers() && parallel_allowed.load(std::memory_order_relaxed) && track_work_ns >= parallel_from_ns
               && !plugin_in_two_places();                              // engine_host.cpp:2669-2670
  if (whole && parallel) workers.wake();                                // :2674
  syscall(SYS_futex, reinterpret_cast<uint32_t*>(&word), FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0); // render_pool.cpp:63
  ```
  The test is last block's total track time against a fixed 150 µs, the same at every rate, with no hysteresis. All three workers are woken whatever the number or size of the jobs.
- **Cost:** unmeasured. At 150 µs of work a serial block would use 5 % of its budget; the deadline is not at risk until several times that. Going parallel costs a futex wake on the audio thread, three wake-ups from WFI (about 70 µs latency each by `docs/ROADMAP.md:182`), the workers' spin between `wake()` and the dispatch and between the two runs, and cold caches for the jobs moved. I estimate 60–180 µs of extra CPU a block, 2–6 % of a core at 44.1 kHz and twice that share at 96 kHz, and it keeps all four cores out of WFI every block. With one cluster and the `performance` governor there is no frequency to gain by spreading light work. Measure with what exists: `melodee-render serial` against `parallel` on a medium project, comparing the four core loads on About and `melodee_render_load`; or the bench with `threads=0` and `tracks=4`.
- **Proposal:** make the threshold a share of the block's period, set in `set_audio_format` (for example parallel from 35 % of the period, back to serial under 25 %, on a slowly falling peak of `track_work_ns` rather than the last block). Wake only as many workers as help: `FUTEX_WAKE` with `min(jobs - 1, wanted)` where `wanted` grows with the load (one helper from 35 %, two from 70 %, three above). The overload guard stays as the safety net.
- **Gain:** battery Medium to High for light and medium projects; none for heavy ones. Latency: slightly less headroom at medium load.
- **Sound or behaviour change:** bit-exact (the pool sums results in a fixed order whoever renders them; the `one_dispatch` host test compares both ways).
- **Risk:** Medium. A block whose load jumps is detected one block late, as today, but from a higher level; the periods queued ahead of the jacks (three in the roadmap's measurements) absorb one late block. Needs a listening and xrun check at 96 kHz. If the governor is later changed to `schedutil`, spreading work may allow a lower operating point; measure both ways before fixing the numbers.
- **Effort:** S for the threshold, M with the worker count.
- **Confidence:** High on what the code does; Medium on the gain (it depends on the wake-up and spin costs, not measured here).

### HOST-3: Workers spin 200 µs each for a second run that does not come

- **Verification:** Confirmed. With exactly one keyed track the first `workers.run` is told another run follows (`waiting_count
- **In the fork:** Implemented in part. Case (a) is fixed: the workers are told to stay awake for a keyed run only when two or more tracks wait. Cases (b) and (c) are unchanged.

- **Where:** `src/melodee/engine/engine_host.cpp:2943`, `:2947`, `:3051`; `src/melodee/engine/render_pool.cpp:150-153`; `render_pool.h:49-52`.
- **Thread and frequency:** three workers, per block, in the cases below.
- **Now:** the first run tells the workers to stay awake when a later run is expected:
  ```cpp
  if (parallel) workers.run(active + 1, first_job, waiting_count || (whole ? second_jobs + (knob_ringing || knob_heard) >= 2 : true));
  ...
  if (parallel && waiting_count > 1) workers.run(waiting_count, keyed_job, ...);
  else for (unsigned i = 0; i < waiting_count; ++i) keyed_job(i);
  ...
  if (parallel && !plugin_on_two_buses()) workers.run(second_jobs, second_job);
  ```
  and `run()` does a run of fewer than two jobs inline (`if (threads.empty() || count < 2)`), without a dispatch. A worker told to linger then spins to the end of its 200 µs (`while (... == seen && steady_clock::now() < until) relax();`). The prediction is wrong when: (a) exactly one track is keyed by another track (`waiting_count == 1`) and fewer than two second-run jobs exist; (b) one bus has a chain and Knob FX is on (`knob_heard`) but not running (`knob_runs` false: nothing picked or fed); (c) two buses share a plug-in. In the right case too, all three linger although a second run of two jobs needs one helper.
- **Cost:** in cases (a) to (c), 3 × 200 µs of spinning a block: 21 % of one core at 44.1 kHz, 45 % at 96 kHz, for as long as the project is in that state and parallel. Case (a) is an ordinary setup (a bass compressor keyed by the kick, one reverb bus). Unmeasured; visible as `workers_pct` in the bench if a keyed compressor is added to its project.
- **Proposal:** pass the exact expectation: `more = waiting_count > 1 || second_jobs + (knob_ringing && knob.kind() != EffectKind::None) >= 2`, and for the keyed run the second half alone. Optionally make the linger a count (`second_jobs - 1`) so only the helpers a run needs stay awake.
- **Gain:** CPU and battery Medium in those setups; none elsewhere.
- **Sound or behaviour change:** bit-exact.
- **Risk:** Low. When Knob FX starts being fed, the first block's second run finds the workers asleep and wakes them (one futex wake, as on any first block).
- **Effort:** S (the count: M).
- **Confidence:** High on the logic (both sides read); Medium on how often users are in these states.

### HOST-4: The host's strip and mix loops are compiled scalar

- **Verification:** Partly confirmed. The flags are as stated (`base.mk:21`) and the mix loops have the shape described (for example `engine_host.cpp:2982-2989`). Whether GCC 12 vectorises them at `-O2` was not checked in the assembly.
- **In the fork:** Implemented in part. Second pass. Step 3 for four loops: a gain that does not move in the block is read once (the track's pan stage, its way to the master and Knob FX, its sends, the buses' returns). Same sums, same output. No flag change: `-O3` on the engine was measured, saved 4 % and changed what the bench played, so it is not used.

- **Where:** `base/base.mk:21` (`CXXFLAGS := -std=c++17 -O2 -pthread -MMD -MP`); loops at `src/melodee/engine/engine_host.cpp:2879`, `:2881-2884`, `:2890`, `:2894`, `:2971-2974`, `:2982-2989`, `:2993-2996`, `:3004-3007`, `:3031`, `:3034`, `:3061`, `:3067-3070`, `:3079`, `:3083`, `:3105`, `:3367`, `:3384-3392`; `compensation.h:44-58`; `engine_host.h:52` (`Ease::at`).
- **Thread and frequency:** audio thread and workers, per frame, per track.
- **Now:** every loop runs to `frames`, a run-time value, and most read a ramp a frame:
  ```cpp
  for (unsigned f = 0; f < frames; ++f) {
      float a = slot.audible.at(f), k = slot.knob_share.at(f);
      float l = slot.left[f] * a, r = slot.right[f] * a;
      knob_l[f] += l * k; knob_r[f] += r * k;
      rest_l[f] += l - l * k; rest_r[f] += r - r * k;
      level = std::max(level, std::max(std::fabs(l), std::fabs(r)));
      if (stem) stem[f] = (l + r) * unpan;
  }
  ```
  g++ 12 vectorises at `-O2` only under the very-cheap cost model, which per the GCC manual "only allows vectorization if the vector code would entirely replace the scalar code", that is when the iteration count is a known multiple of the vector width. With a run-time count and NEON (no partial vectors) these loops stay scalar. The `level` maximum is a float reduction, which also blocks it without fast-math. The ramps (`at(f)`) are flat (step 0) in every block except the 10 ms after a mix change, and `knob_share` is 0 unless Knob FX has picked the track, yet the full expression is evaluated.
- **Cost:** unmeasured: about 28 scalar floating-point operations a frame in the mix loop and 10 in the level stage, roughly 3–4 µs per track per block, 50–60 µs for sixteen tracks: 2 % of a 44.1 kHz block, 4 % at 96 kHz. Vector code would be about three times faster on these loops. The same flags compile the sampler and effects, where the stake is larger (other auditors).
- **Proposal:** (1) Check first: compile `engine_host.cpp` once with `-fopt-info-vec-missed` and read the report. (2) Give the engine objects a wider model, in Melodee's own Makefile: `build/obj/melodee/engine/%.o: CXXFLAGS += -fvect-cost-model=dynamic` (or `-O3`), and consider `-mcpu=cortex-a55` for the whole build (it tunes for the in-order A55 and inlines the LSE atomics). No `-ffast-math`. (3) In the source: a steady-state path when the eases are flat, with constant gains and a fixed count of `block` when `frames == block`; skip the Knob FX terms when `knob_share.silent()`; take the peak in its own loop.
  ```cpp
  if (frames == block && slot.mix_l.flat() && slot.mix_r.flat()) {
      const float gl = slot.mix_l.to, gr = slot.mix_r.to;
      for (unsigned f = 0; f < block; ++f) { slot.left[f] *= gl; slot.right[f] *= gr; }
  }
  ```
- **Gain:** CPU Medium (per-frame cost of every sounding track); battery Low to Medium.
- **Sound or behaviour change:** intended bit-exact: element-wise vector code keeps IEEE results, and with step 0 `from + step * (f + 1)` is exactly `from`. One caution: g++ 12 defaults to `-ffp-contract=fast`, so any change of expression shape can move where a fused multiply-add is formed; that would be rounding only. The sample-exact host tests show which it is.
- **Risk:** Low to Medium (larger code; rounding to verify).
- **Effort:** flags S; source paths M.
- **Confidence:** Medium. I did not compile; the claim rests on the GCC 12 release notes and manual (very-cheap at `-O2`) and on the loops' shape.

### HOST-5: A compensation line that starts clears 128 kB on the audio thread; all lines start together

- **Verification:** Confirmed. A line idle since the previous block clears both of its buffers with `memset` on the audio thread (`compensation.h:38-41`).
- **In the fork:** No change.

- **Where:** `src/melodee/engine/compensation.h:38-41` (`CompensationLines::run`); callers `engine_host.cpp:2902`, `:3029-3037`, `:3048-3056`; switch at `:1539-1540`.
- **Thread and frequency:** audio thread and workers, once per line each time delay compensation comes on or a line resumes.
- **Now:**
  ```cpp
  if (line.block + 1 != block) { // idle since: what it holds is old
      std::memset(l, 0, sizeof(float) * most); std::memset(r, 0, sizeof(float) * most);
      line.delay = delay;
  }
  ```
  `most` is 16384 frames, so 131 kB per line. Compensation is off until some track, bus or Knob FX reports latency (a plug-in, a Limiter's LOOKAHEAD), and goes off again one block after the last one is bypassed (`plan.on = late || (pdc.on && ...)`). In the block it comes on, every sounding track's line, the four pad-send lines, the Knob pads line, the direct line and each bus line start at once: 26 lines or more for a 16-track project, about 3.4 MB of memset inside one block. A line skipped for one block (Knob FX not fed, a track that lost and regained its engine) clears again when it resumes.
- **Cost:** unmeasured; at 2–4 GB/s the audio thread's own share (10 lines, 1.3 MB) is 0.3–0.7 ms and the tracks' 2.1 MB is spread over the workers, or serial if the pool is not in use. That cannot fit a 96 kHz block (1.33 ms) on a loaded project: a dropout at the moment a latent effect is un-bypassed or a look-ahead is turned up while playing.
- **Proposal:** do not clear. Keep per line a count of frames written since it (re)started, and read silence for positions older than that: split the read loop into the part before the count (zeros) and the part within it. A restart is then `line.fresh = 0`. Alternatively clear in slices over the next blocks, as `Effect::process` does for delay memory (`effects.cpp:624-627`).
- **Gain:** latency (dropout risk) Medium; CPU none in steady state.
- **Sound or behaviour change:** bit-exact (the same zeros are read).
- **Risk:** Low; the easing between two delays must also honour the count.
- **Effort:** S to M (about 30 lines).
- **Confidence:** High on the code path; Low on the memset rate. Not checked: how often users toggle latent effects while playing.

### HOST-6: Ten punch histories record the master every block, a frame at a time

- **Verification:** Merged. Same issue as SMP-3.
- **In the fork:** See SMP-3.

### HOST-7: Fixed work repeated every block that can be kept or skipped

- **Verification:** Partly confirmed. I read two of the listed sites (the per-track meter fall at `engine_host.cpp:3025-3026`). The others were not read one by one.
- **In the fork:** No change.

- **Where:** `src/melodee/engine/engine_host.cpp:3025`, `:3137`, `:3197`, `:3395`, `:2848-2861`, `:2963-2965`, `:2865`, `:2910`, `:2917-2919`, `:2706-2709`, `:3183`, `:1458-1469`, `:1506-1546`, `:2854-2857`; `engine_host.h:46-51` (`Ease::aim`); `dsp.h:38-41` (`fall_per_block`).
- **Thread and frequency:** audio thread per block (the clock reads also on workers).
- **Now** (line numbers in `engine_host.cpp` unless said):
  1. `:3025` `meters[t].load(...) * dsp::fall_per_block(0.97f, frames)` inside the 16-track loop, `:3395` the same for four loops, `:3197` once more. `dsp.h:38-41`: at 44.1 kHz it returns the constant, at any other rate it calls `std::pow`. So 17–21 `pow` a block at 48 and 96 kHz. `:3137` `const float meter_fall = std::pow(10.0f, -meter_fall_db / 20 / float(dsp::sample_rate()));` every block at every rate. `take_recorder.h:88` one more at 48/96 kHz.
  2. `engine_host.h:46-51` `Ease::aim`: two divisions and an atomic load per call; about ten calls per track with an engine (`:2848-2861`, `:2963-2965`) plus buses, loops, volume, monitor, park: up to 170 calls, 340 divisions a block.
  3. Four `steady_clock::now()` per track per block (`:2865`/`:2910` in `chain_of`, `:2917`/`:2919` in `track_job`), two for the Looper, two for the block: 68 for sixteen tracks. `track_ns` (16 × 4 bytes, one cache line) is written twice per job by whichever core runs it.
  4. `:2706-2709` clears `buffer`, `left` and `right` of all sixteen slots, also those with no engine (never read: every reader checks `slot.engine`) and `buffer` of stereo engines (never read).
  5. `:3183` `publish_taps`: every block it builds a 784-byte `TrackTaps`, scans the sampler's voices and stores 196 atomic words (`voice_tap.h:77-85`), with no voice sounding. `watch_taps` is always a track (`shell.cpp:925`, `lights.cpp:130`, `:515`).
  6. `:1506-1546` and `:1458-1469`: `plan_compensation` and `plan_keys` rescan the 25 chains and store 38 atomics every block.
  7. `:2854-2857`: a frozen track's reference pan, `std::cos` and `std::sin`, every block.
- **Cost:** unmeasured, each about 0.5–4 µs; together 10–15 µs a block: about 0.4 % of a core at 44.1 kHz and 1 % at 96 kHz, in every block including silent ones.
- **Proposal:** (1) compute the three fall factors in `set_audio_format` or at the top of `render()` once, and use them in the loops; (2) compute `most` and `1 / frames` once a block and pass them to `aim` (for 128 frames the reciprocal is exact); (3) one pair of clock reads per track job, the time added to a field of the `Slot`; (4) clear only slots with an engine, and `buffer` only for mono engines; (5) publish taps only when the track has a voice or grain, or had one in the last published block; (6) recompute the plans when a command, a plug-in offer or a reported latency changed; (7) keep the two values in the `FreezePack`.
- **Gain:** CPU Low; battery Low (it is part of the idle floor).
- **Sound or behaviour change:** bit-exact for 1, 2, 4, 6, 7 (the same expressions, evaluated once); none for 3 and 5.
- **Risk:** Low. For 6, a plug-in's latency can change on its own, so keep a cheap per-block check of `runner->latency()`.
- **Effort:** S each.
- **Confidence:** High that they run every block; Low on the individual times.

### HOST-8: USB-C stems and the full-volume USB mix are rendered whether or not a computer records

- **Verification:** Confirmed. `MelodeeApp::render` always passes the stem block, so the mix loops fill `stem[f]` every block (`engine_host.cpp:2988, 3014`); the full-volume copy is made whenever USB volume is Full (`:3174-3177`).
- **In the fork:** No change.

- **Where:** `src/melodee/melodee_app.h:36-37`, `src/melodee/melodee_app.cpp:135-139`; `src/melodee/engine/engine_host.cpp:2953-2954`, `:2988`, `:3014`, `:3174-3177`; `base/src/drivers/usb_audio.cpp:230`.
- **Thread and frequency:** audio thread, per block.
- **Now:** the app always hands the host a stem block, and the fixed mix when Settings' USB volume is full:
  ```cpp
  const UsbStems* stems() const override { return &stem_block; }
  host.render(output.data(), audio_frames, shell.volume(), input.data(), &stem_block.track[0][0], ..., usb_full ? usb_block.data() : nullptr);
  ```
  The host clears and writes sixteen stems (8 kB) and, with `fixed`, clips and converts the master a second time. The runtime then drops them unless the computer is streaming: `if (!accepting.load(std::memory_order_acquire)) return;`.
- **Cost:** unmeasured: about 5–8 µs a block (0.2–0.3 % of a core), always.
- **Proposal:** on the service thread read `hardware.usb_output.state()` (`base/src/runtime/app.h:51`; it reads an atomic) into an `std::atomic<bool>`; in `render()` pass `nullptr` for `stems` and `fixed` while it is not `Running`, and return `nullptr` from `stems()` and `usb_mix()`.
- **Gain:** CPU Low; battery Low.
- **Sound or behaviour change:** none at the jacks. Over USB-C the first service pass (up to 5 ms) of a new recording could carry silent stems and the post-volume mix; the ring primes with 1024 frames of silence first, so it is likely never heard. To check.
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** High.

### HOST-9: Master compressor: a sine a frame for a 0.6 Hz wobble, log and power every fourth frame

- **Verification:** Confirmed. `MasterCompressor::colour` calls `std::sin` per frame for a 0.6 Hz wobble, and `process` calls `log10` every fourth frame (`master_comp.cpp:37-50, 71-80`).
- **In the fork:** No change.

- **Where:** `src/melodee/engine/master_comp.cpp:37-50` (`colour`), `:71-85` (`process`), `:93-104` (`duck`).
- **Thread and frequency:** audio thread, per frame, only while the compressor is on (off by default, `model/compressor.h:21`).
- **Now:**
  ```cpp
  wobble += 0.6f / fs(); wobble -= std::floor(wobble);
  float back = (40 + 6 * std::sin(2 * dsp::pi * wobble)) * stretch; // 0.8-1 ms
  ...
  float over = 20 * std::log10(std::max(level, 1e-6f)) - threshold;   // every fourth frame
  step = (target(reduce) - gain) * 0.25f;                              // target(): std::pow(10, -reduce / 20)
  ```
  `fs()` is an atomic load, so the division is done every frame. The gain computer calls `log10` and `pow` 32 times a block even when the level is under the threshold.
- **Cost:** unmeasured: with Color about 10 µs a block (128 `sinf` and five divisions a frame: `0.6f / fs()`, two in `soft_clip`, two `/ 1.1f`), without it about 3 µs.
- **Proposal:** keep `0.6f / fs()` in `set()`; step the wobble's sine once a block and ramp it (or a two-multiply rotation); compare `level` with a linear threshold first and skip `log10`/`pow` when under it.
- **Gain:** CPU Low.
- **Sound or behaviour change:** the hoisted division is bit-exact; the ramped sine and the linear compare are rounding only (a delay modulation differing by under a thousandth of a frame; a gain differing at the threshold's last bit).
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** High on the code; Low on the times.

### HOST-10: Flush-to-zero is set once per thread and never checked again

- **Verification:** Confirmed. `flush_denormals()` runs once per thread, behind a `thread_local` flag on the audio thread and at the top of each worker (`engine_host.cpp:2621-2624`, `render_pool.cpp:136`). So denormals are already handled; the finding is only that a plug-in could switch the mode back.
- **In the fork:** No change.

- **Where:** `src/melodee/engine/engine_host.cpp:2621-2624`; `src/melodee/engine/render_pool.cpp:136`.
- **Thread and frequency:** audio thread and workers, once at their first block.
- **Now:**
  ```cpp
  static thread_local bool flushed = false;
  if (!flushed) { // the thread's first block: denormals off; ...
      flush_denormals();
      flushed = true;
  ```
  VST3, LV2 and CLAP plug-ins run inside these threads (`PluginRunner::render` from `run_chain` and `PluginEngine::render_stereo`). A plug-in that resets the floating-point environment clears the bit for the rest of the session, and nothing sets it again or reports it.
- **Cost:** none while the bit holds. If it is lost, every decaying filter and envelope on that thread runs into subnormal arithmetic, with no sign of why the load rose.
- **Proposal:** at the top of `render()` and after each worker wake-up read FPCR and set bit 24 if it is clear (one `mrs`, a test, rarely an `msr`), counting the times for `status`.
- **Gain:** CPU (protection of headroom) Low.
- **Sound or behaviour change:** none (values under about 1e-38 only).
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** Medium. I did not find a plug-in that does this; it is a guard, not an observed fault.

## Audio engine: sampler voices, streaming and the Looper

### SMP-1: The stream pager's thread wakes 1,000 times a second for as long as the app runs

- **Verification:** Confirmed. `StreamPager::run` is `while (running) if (!pump()) sleep_for(1 ms)` at `SCHED_FIFO` 20 (`stream_pager.cpp:240-251`).
- **In the fork:** Implemented. The pager sleeps on a futex while no stream is open and is woken by `open()`, `restart()`, `close()`, `flush()` and `stop()`. With a stream open it looks every millisecond as before.

- **Where:** `src/melodee/engine/stream_pager.cpp:250` (`StreamPager::run`); started on every service pass at `src/melodee/shell/sample_library.cpp:448` (`host.streams().start();`).
- **Thread and frequency:** the pager's own thread (`SCHED_FIFO` 20, real-time I/O class), every 1 ms, always: with no project, with no streamed sample, with the transport stopped.
- **Now:**
  ```cpp
  while (running.load(std::memory_order_relaxed))
      if (!pump()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ```
  `pump()` (426-439) pops the message queue, then calls `step()` for all 96 slots; with nothing open each returns at `if (!w.part || w.dead) return false;` (389). There is no condition variable or futex: a poll loop.
- **Cost:** 1,000 timer wakeups a second on one core, more than the audio thread (344 a second at 44.1 kHz), the MIDI thread (200) and the service thread (200) together. Each is a timer interrupt, a switch into and out of a FIFO thread, and about a hundred loads. I estimate 5-15 µs each, so 0.5-1.5 % of a core, and that core never rests in WFI for more than 1 ms. To measure: read `voluntary_ctxt_switches` in `/proc/<pid>/task/<tid>/status` ten seconds apart (expect about 10,000), `perf stat -e sched:sched_switch -t <tid>`, and the supply current with the thread stopped.
- **Proposal:** block while nothing is open; look every 10 ms while streams are open and caught up.
  ```cpp
  // run(): `word` is bumped by open(), restart(), flush() and stop(), which FUTEX_WAKE only when `asleep` is set
  while (running.load(std::memory_order_relaxed)) {
      const uint32_t seen = word.load(std::memory_order_acquire);
      if (pump()) continue;                       // took a chunk or a message: straight round again, as now
      bool open = false;
      for (const auto& w : work) open |= w.part && !w.dead;
      futex_wait(word, seen, open ? 10 /* ms */ : 0 /* until woken */);
  }
  ```
  The futex is the one `render_pool.cpp:35-63` already uses from the audio thread. Ten milliseconds is safe while a stream plays: the pager reads two chunks times the speed ahead (`ahead`, line 393: 0.74 s at 44.1 kHz), and `pump()` already goes round without sleeping while any stream took a chunk. `flush()` (310-314) must wake it too, or each flush waits out the timeout.
- **Gain:** battery Medium; CPU Low.
- **Sound or behaviour change:** none. A voice opened with nothing held (`safe` 0: a reversed zone, a locate into an audio take or a print) gets its first chunk sooner than now, because the pager is woken instead of found by the next poll. A voice that ran past what was pinned (`SamplerEngine::pass`) is noticed up to 10 ms later unless `underrun()` also wakes.
- **Risk:** Low to Medium. One `FUTEX_WAKE` on the audio thread per `open()`/`restart()` while the pager sleeps (the render pool makes one every block). The word must be read before `pump()` so a message that lands in between is not slept through.
- **Effort:** S to M (about 40 lines).
- **Confidence:** High on what the code does; Medium on the size of the saving.

### SMP-2: A stretched loop's window search runs about half a millisecond in one block, calls `fmod` 24,640 times, and all four loops search in the same block

- **Verification:** Confirmed. `LoopStretch::best` tries 385 offsets × 64 taps, each through `wrap()`, which is `std::fmod` (`looper.cpp:21-24, 42-60`). The time per call is the auditor's estimate.
- **In the fork:** No change.

- **Where:** `src/melodee/engine/looper.cpp:42` (`LoopStretch::best`), called at line 74 (`LoopStretch::frame`) from line 508 (`Looper::play_frame`); the phase is set at 507 and 671; sizes at `looper.h:95` (`window = 2048, hop = window / 2, search = 384, match = 64`).
- **Thread and frequency:** audio (the Looper's job), once every 1,024 output frames for each loop that plays at a tempo other than its own: every 23 ms at 44.1 kHz, every 10.7 ms at 96 kHz. Also the service thread, in `fit_render` (865-888).
- **Now:**
  ```cpp
  for (int d = -int(search); d <= int(search); d += 2) {
      double s = around + d;
      float dot = 0, energy = 1e-9f;
      for (unsigned k = 0; k < match; ++k) {
          auto i = uint32_t(wrap(s + double(k) * 2 * step, len));
          float x = l[i] + r[i];
          dot += x * want[k];
          energy += x * x;
  ```
  `wrap()` is `std::fmod` plus a test (21-24); without `-ffast-math` it is a call into libm. 385 starts × 64 points = 24,640 calls a search. Every `reset()` sets `since = hop` (`looper.h:96`). Play resets all four loops in one block (667-672), and a tempo change makes every loop stretched on the same frame (`if (!l.was_stretched) l.stretch.reset();`, 507), so from then on the four searches fall on the same frame of the same block.
- **Cost:** about 40-50 cycles an iteration, so 1.0-1.2 million cycles, 0.55-0.7 ms a search. A block lasts 2.9 ms at 44.1 kHz and 1.33 ms at 96 kHz. One stretched loop: a fifth of a block at 44.1 kHz, about half a block at 96 kHz. Four in phase: 2.2-2.8 ms, the whole block at 44.1 kHz and twice the block at 96 kHz, every 1,024 frames. The Looper is one job of the render pool (`engine_host.cpp:2927-2933`), so the four run one after another on one core. Average load about 2.5-3 % of a core for each stretched loop at 44.1 kHz. To measure: the host already times this job (`loop_ns`, `engine_host.cpp:2930-2932`); log its maximum with four loops recorded at 120 BPM and played at 110, at 44.1 and at 96 kHz. In `fit_render` a 64 s loop is 2,756 searches, 1.5-2 s in which the service thread does nothing else.
- **Proposal:**
  1. Take `fmod` out, bit for bit. Work `off[k] = double(k) * 2 * step` out once (the same expression, so the same value) and replace `wrap(x, len)` by `if (x >= len) x -= len; else if (x < 0) x += len;`. For x in (-len, 2·len) that is the value `wrap` gives (x - len is exact when len ≤ x < 2·len). Keep the `fmod` path when `len < 2 * (search + 2 * match * step)`; `play_frame` only stretches loops of 8,192 frames or more (504), so the fast path always holds there. About 2.5 times faster.
  2. Stop the four searching together. Either start each loop's `since` at `hop - n * hop / 4` in `reset()` (S), or work the next start out ahead at a point of the hop that differs by loop, as the sampler's Tones does with `prepare_at` (`sampler.cpp:373`, `1044-1052`) (M).
- **Gain:** latency (dropout risk) High if the estimate holds; CPU Medium.
- **Sound or behaviour change:** step 1 bit-exact. Step 2 not bit-exact: the same windows, placed on other frames; no audible difference expected.
- **Risk:** Low for step 1. Low to Medium for step 2.
- **Effort:** S for step 1; S or M for step 2.
- **Confidence:** Medium. The iteration count and the shared phase are certain; the cost of one iteration is my estimate of glibc 2.36's `fmod` on this core.

### SMP-3: Ten Punch histories record the master every block, frame by frame, with nothing held

- **Verification:** Confirmed. `Punch::process` loops frame by frame and writes its history whenever `left` is allocated, held or not; `run_live(255, …)` and the song's punch run on the master every block (`punch.cpp:97-108`, `engine_host.cpp:2286-2289, 3109, 3119`).
- **In the fork:** Implemented in part. Second pass. Step A: a Pad FX that is off moves its history on in one or two copies instead of a frame at a time. Measured with the two changes below it in the table of the second pass. Step B (one shared history) is not done.

- **Where:** `src/melodee/engine/punch.cpp:97` (`Punch::process`); called from `engine_host.cpp:2287-2288` (`run_live`), `3109` and `3118`. Every slot starts on the master: `engine_host.h:743` (`uint8_t punch_target = 255;`), `748` and `774` (`uint8_t target = 255`).
- **Thread and frequency:** audio, every block, per frame, for ten `Punch` objects (the song's punch, Flex Beat, four live Pad FX slots, four kept pattern slots), playing or stopped.
- **Now:**
  ```cpp
  float amount = held ? depth * float(std::min(1.0, double(begun + 1) / ease_in)) : fading * depth;
  if (amount <= 0 && frozen) frozen = false; // the release is over: the history runs again
  ...
  if (!frozen && !lines) { left[write] = dry_l; right[write] = dry_r; }
  ...
      if (++write >= history) write = 0;
      if (fresh < history) ++fresh;
  ```
  By design (`punch.h:54`: "Run while it is off too"), but the off path is the full per-frame loop, and on a new project the ten rings (192,000 frames × 2 floats = 1.5 MB each) all hold the same master audio.
- **Cost:** about 25 instructions a frame: 10 × 128 × ~17 cycles ≈ 22,000 cycles ≈ 12 µs a block, plus 10 kB of ring written a block (160 cache lines that are never in cache, since the rings total 15.4 MB), which also turns over a third of a 32 kB L1 data cache every block. I estimate 0.5-1 % of a core, always. To measure: a `steady_clock` pair round `engine_host.cpp:3109-3119`, or `perf record` on the idle audio thread and read `Punch::process`'s share.
- **Proposal:** A, a fast path at the top of `process()`:
  ```cpp
  if (!held && fading <= 0) { // off: only the history moves
      frozen = false;
      for (unsigned done = 0; done < frames;) {
          const unsigned n = std::min(frames - done, history - write);
          std::memcpy(left.get() + write, l + done, n * sizeof(float));
          std::memcpy(right.get() + write, r + done, n * sizeof(float));
          write = write + n == history ? 0 : write + n;
          done += n;
      }
      fresh = std::min(history, fresh + frames);
      return;
  }
  ```
  With `held` false and `fading` 0, `amount` is 0, so the loop today clears `frozen`, changes no sample, writes each frame and moves `write` and `fresh` on: the same result. B, later: one history a target shared by the idle punches on it (an idle punch passes its input through unchanged, so the ten see the same signal), a punch taking a ring of its own when it starts. That needs a design for the kinds that write into the history (Echo, Chorus, Comb, Granulator, Delay, Reverb) and for two punches held at once; it would remove nine tenths of the memory traffic and 13.8 MB.
- **Gain:** battery Low to Medium; CPU Low (A). Memory Low (B).
- **Sound or behaviour change:** A bit-exact.
- **Risk:** A Low. B Medium.
- **Effort:** A is S; B is L.
- **Confidence:** High on the path; Medium on the cost.

### SMP-4: A playing loop works out one `fmod` and four double divisions per frame from values that do not change in the block

- **Verification:** Confirmed. `Looper::play_frame` calls `index_at()` per frame: one `fmod` and a division by `l.ticks` (`looper.cpp:149-152, 494-505`).
- **In the fork:** No change.

- **Where:** `src/melodee/engine/looper.cpp:494` (`Looper::play_frame`), `149` (`index_at`), `290` (`stretched`), `looper.h:260` (`ratio`); the frame loop at `looper.cpp:739-741`.
- **Thread and frequency:** audio, per frame, per loop that sounds.
- **Now:**
  ```cpp
  double at = index_at(l, tick);                                          // 503
  bool stretch = stretched(l) && l.length >= 4 * LoopStretch::window;    // 504
  return wrap(tick - l.start, l.ticks) / l.ticks * double(l.length);     // 151, index_at
  double speed = double(l.length) / l.ticks * last_per_frame;            // 292, stretched
  double r = speed / ratio(l);                                           // 293; ratio: double(l.rate) / rate
  ```
- **Cost:** per frame per loop: `fmod` (its quotient is how many times the loop has gone round, so libm's shift-and-subtract loop runs), and divisions by `l.ticks` (twice), by `rate` and by `ratio(l)`; a fifth (`ratio(l)` again, 508 or 514) when stretched. About 150-200 cycles a frame: 128 × 4 loops × 175 ≈ 90,000 cycles ≈ 50 µs a block, about 1.7 % of a core for four loops playing at their own tempo. Read `loop_ns` to check.
- **Proposal:** work `stretched(l)` and `ratio(l)` out once for each run of frames `[f, end)` at line 739 (they read only `l.length`, `l.ticks`, `l.rate`, `last_per_frame` and `rate`, which change only in `act()` and `close_take()`, between runs) and pass them to `play_frame`. Then, optionally, call `index_at` once at the start of a run and add `per / l.ticks * l.length` a frame, with a compare-and-subtract at `l.length`; seeded again every run, so nothing drifts.
- **Gain:** CPU Low to Medium (about 1 % of a core with four loops playing).
- **Sound or behaviour change:** the first step bit-exact; the second rounding only (the read place differs by about 1e-12 of a frame).
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** High on the code; Medium on the numbers.

### SMP-5: A kit track with nothing sounding still walks its voices four times and filters silence; there is no early return

- **Verification:** Confirmed. `SamplerEngine::render_stereo` starts straight into its voice and grain loops with no "nothing sounding" test (`sampler.cpp:1615-1640`).
- **In the fork:** No change.

- **Where:** `src/melodee/engine/sampler.cpp:1615` (`SamplerEngine::render_stereo`), called without a test at `engine_host.cpp:2782` and `1254`. `engine_host.cpp` never calls `Engine::sounding()` (its four `sounding()` calls are the print's and the audio player's).
- **Thread and frequency:** audio or a render worker, every block (more than once a block when the block is cut into runs), for every kit and keygroup track.
- **Now:** with no voice live the call still makes four passes over the 40 voices (lines 1621, 1628, 1674 and 1769, reading `porta` at offset 636, `pad` at 0 and `stream` at 408 of a 648-byte `Voice`), one over the 48 grains (1640), two over the 16 lanes (1664, 1737), and then, when the kit's FILTER is under 99 or its TYPE is not low pass:
  ```cpp
  if (!filtered) return;
  float* io[2] {left, right};
  for (unsigned side = 0; side < 2; ++side)
      for (unsigned f = 0; f < frames; ++f) {
          float x = io[side][f];
          float hp = (x - (k + g) * s1[side] - s2[side]) / (1 + g * (g + k));
  ```
- **Cost:** about 180 cache lines touched (11.5 kB of a 32 kB L1 data cache) per idle kit track per call, and with the kit filter on, 256 iterations with a division on the recursive path (about 30 cycles each, about 4 µs). Roughly 1-2 µs a block for an idle kit, 5-6 µs with its filter on: 0.05-0.2 % of a core a track, 1-3 % for a project of sixteen silent kits. The host already keeps `track_ns[t]` (`engine_host.cpp:2785`): read it for an idle kit with FILTER at 100 and at 50.
- **Proposal:**
  1. One pass at the top: `bool live = false; for (auto& v : voices) live |= v.pad >= 0;`, with a count of grains in use (`spawn()`, and where a grain ends at 1655, 779, 790) and of lanes in use (`lane_for()`, line 1767). Nothing live: keep the bookkeeping (`played_frames`, `retune_voices()` to clear the marks, `send_used = 0`, `knob_used = false`) and go straight to the filter test.
  2. Skip the kit filter when nothing was rendered in this call and `s1`, `s2` are all exactly 0. They do reach 0: the audio thread flushes denormals (`engine_host.cpp:2622`).
  3. For the host auditor: `sounding()` (795-799) leaves out lanes still ringing and the kit filter's state, so the host cannot gate a kit on it as it stands.
- **Gain:** battery Low to Medium (it grows with the number of kit tracks); CPU Low.
- **Sound or behaviour change:** bit-exact (zeros through a filter at rest are zeros).
- **Risk:** Low. A voice that ends in a block is still seen live at that block's start, so line 1769 still closes its stream; outside `render_stereo` every path that frees a voice closes its stream first (`start()` 76, `sample_gone()` 777, `silence()` 789).
- **Effort:** S to M.
- **Confidence:** High on the code; Low to Medium on the numbers (I do not know this SoC's L2/L3 sizes).

### SMP-6: A voice whose envelope has decayed to a sustain of 0 plays on in silence

- **Verification:** Confirmed. `Envelope::next` parks in `Stage::Sustain` at `level = s.sustain` (`dsp.h:162-166`); with a sustain of 0 the level is 0 but the stage is not `Off`, so `env.active()` stays true and the voice keeps running.
- **In the fork:** No change.

- **Where:** `src/melodee/engine/dsp.h:162-166` (`Envelope::next`); the shape at `sampler.cpp:208-212` (`SamplerEngine::aim`); the only envelope test that ends a voice at `sampler.cpp:989`, `1135`, `1543`, `1577`, `1732` (`if (!env.active()) { v.pad = -1; ... }`).
- **Thread and frequency:** audio, per sample, for as long as such a voice lasts.
- **Now:**
  ```cpp
  case Stage::Decay:
      level = s.sustain + (level - s.sustain) * s.decay;
      if (std::fabs(level - s.sustain) < 1e-4f) stage = Stage::Sustain;
      break;
  case Stage::Sustain: level = s.sustain; break;
  ...
  bool active() const { return stage != Stage::Idle; }
  ```
  With SUSTAIN at 0 and DECAY under 100 on a Gate pad, on a one shot set to decay from its start, or on an instrument zone whose own offsets bring the sustain to 0 (an SFZ pluck's `ampeg_sustain=0`), the envelope settles in Sustain at level 0 and stays active. The voice goes on reading, interpolating and filtering, adding exact zeros, until the key is let go or the region ends. A one shot that loops never ends until its pad is hit again (`hit()`, 632-638). A streamed voice keeps its stream, and the pager keeps reading the card ahead of it.
- **Cost:** a full voice (about 30 cycles a frame plain, about 100 filtered) for no sound, and one of the 32 voices. An instrument held under the sustain pedal can sit at 32 silent voices with 32 open streams. How often real kits do this I cannot tell from the code.
- **Proposal:** park it. In the per-voice dispatch (1674-1735), when `v.env.stage == Stage::Sustain && v.shape.sustain <= 0 && !v.gliding && !v.choked && !v.stretch`, do not render: move the voice on in time as `pass()` does for a starved stream (1419-1440: its place, round its loop, out at its end, `pager->place`) and `continue`. It keeps its key and its place, so a note off still ends it and a SUSTAIN raised under the held note (`refresh()`, 416) brings it back. A one shot that cannot be let go (`!v.gate`) and does not loop can simply end. The same test can take a voice whose `gain` and `aim.gain` are both 0.
- **Gain:** CPU and battery Medium where such envelopes are used; voices freed for stealing; no card reads for silence.
- **Sound or behaviour change:** bit-exact while parked (the voice only ever added `x * 0`). After it is brought back, its pad filter's state is as it was when parked: a rounding-level difference in the first frames.
- **Risk:** Low to Medium. `pass()` returns at once for stretched, cloud and unstreamed voices (1420), so parking needs its own mover or a wider `pass()`. The tests that compare the plain and full loops see the same output.
- **Effort:** S to M.
- **Confidence:** High that it happens (traced through `Envelope::next` and every place a voice ends); Low on how common it is.

### SMP-7: Each cloud grain calls `std::sin` and divides once per frame

- **Verification:** Confirmed. `std::sin(pi * age / length)` per grain per frame (`sampler.cpp:1651`).
- **In the fork:** No change.

- **Where:** `src/melodee/engine/sampler.cpp:1651` (`SamplerEngine::render_stereo`, the grains' loop). The same in `punch_mpc.cpp:201` (Granulator).
- **Thread and frequency:** audio, per frame, per grain; up to 48 grains a kit.
- **Now:**
  ```cpp
  float w = std::sin(dsp::pi * float(g.age) / float(g.length)) * g.gain * 0.5f; // a smooth window
  left[f] += (l[i] + (l[j] - l[i]) * frac) * w * g.left; right[f] += (r[i] + (r[j] - r[i]) * frac) * w * g.right;
  ```
- **Cost:** `sinf` (a libm call, about 30 cycles) and a division (about 11) are roughly half of a grain's 85-95 cycles a frame. One grain: about 0.22 % of a core. A cloud at its defaults (24 grains a second of 0.1 s: 2.4 at once) about 0.5 %; DENSITY and SIZE at 100 fill all 48 grains: 10-11 % of a core, half of it the window.
- **Proposal:** the window from a table, as Tones already does (`tones_table`, 1141-1151): a half sine of 1,025 points made at start-up, read at `age * (1024.0f / length)` with a straight line between points (the factor kept in the grain, worked out in `spawn()`). Or the exact recurrence `w[n+1] = c * w[n] - w[n-1]` with `c = 2 cos(pi / length)` kept in the grain as doubles.
- **Gain:** CPU Medium while a Cloud pad plays; none otherwise.
- **Sound or behaviour change:** rounding only (the window differs by about 1e-6 with the table).
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** High on the code; Medium on the numbers.

### SMP-8: The pad filter and the kit filter divide two to six times a frame by values that change at most every 16 frames

- **Verification:** Confirmed. The state-variable stage divides by `(1 + g * (g + k))` on every call (`sampler.cpp:919`).
- **In the fork:** No change.

- **Where:** `src/melodee/engine/sampler.cpp:918-922` (`voice_out`, the `stage` lambda), `928-931` (Classic), `1775` (the kit filter). The same shape in `punch.cpp:175` and `punch_mpc.cpp:28`.
- **Thread and frequency:** audio, per frame, per filtered voice and side.
- **Now:**
  ```cpp
  auto stage = [&](float x, float k, float* s) { // one state-variable stage: low, band and high pass
      float hp = (x - (k + g) * s[0] - s[1]) / (1 + g * (g + k));
      float bp = g * hp + s[0], lp = g * bp + s[1];
      s[0] = g * hp + bp; s[1] = g * bp + lp;
  ```
  `g` moves every 16 frames (912-916) and `k1`, `k2` only during a glide, but the compiler may not turn `/ d` into `* (1 / d)` without `-ffast-math`, so each stage divides on each side: 2 a frame for a 12 dB filter, 4 for 24 dB, 6 for Classic (which adds two `soft_clip` divisions a side, `dsp.h:70-73`).
- **Cost:** 2-4 divisions of about 11 cycles on the filter's recursive path: 25-45 of a filtered voice's roughly 100 cycles a frame. Sixteen filtered voices: about 1.5 % of a core.
- **Proposal:** keep `inv1 = 1 / (1 + g * (g + k1))` and `inv2` in the voice, worked out where `fg` is (every 16 frames) and on each frame while `v.gliding`; `hp = (x - (k + g) * s[0] - s[1]) * inv`. For the kit filter, work the reciprocal out in `set_param()` beside `g` and `k`, and take the `p[FilterType]` choice out of the frame loop (three loops, or select after).
- **Gain:** CPU Low to Medium.
- **Sound or behaviour change:** rounding only (a product in place of a quotient: the last bit).
- **Risk:** Low. The tests that compare `run_plain` with the full loop still agree, since both go through `voice_out`.
- **Effort:** S.
- **Confidence:** High on the code; Medium on the cycle counts.

### SMP-9: The short voice loop still runs the envelope's state machine and three exit tests on every frame

- **Verification:** Confirmed. `run_plain` calls `env.next()` and tests the end, the loop and `env.active()` on every frame (`sampler.cpp:973-990`).
- **In the fork:** Implemented. Second pass. `held_run()` in `sampler.cpp` plays the frames of a voice that holds its sustain, forwards and unfiltered, without the per-frame tests and the envelope's step, for `run_plain()` and `zone_run_plain()`. Measured: 17 % fewer instructions for the bench's busy project, the output the same to the bit. The tests compare it with the loop frame by frame (`held_runs` off) for kits and keygroups.

- **Where:** `src/melodee/engine/sampler.cpp:973-990` (`run_plain`) and `1524-1544` (`zone_run_plain`).
- **Thread and frequency:** audio, per frame, per voice: the common path (a drum hit with the default envelope is in Sustain from about its ninth frame to its end).
- **Now:**
  ```cpp
  unsigned i = unsigned(position);
  float frac = float(position - i);
  unsigned j = i + 1 < v.end ? i + 1 : loop ? v.loop_at : v.end - 1;
  float a = env.next(v.shape) * gain * fade;
  float sl = l[i] + (l[j] - l[i]) * frac, sr = r[i] + (r[j] - r[i]) * frac;
  ...
  position += reverse ? -step : step;
  if (!env.active()) { v.pad = -1; break; }
  ```
  Linear interpolation; a `double` place and step, turned into an index and a `float` fraction each frame (three conversions); the loop/end test before it. About 48 instructions a frame unfiltered, about 30 cycles. Not vectorisable as written (early exits, a carried `double`, indexed loads), at `-O3` either.
- **Cost:** about 0.08 % of a core a voice at 44.1 kHz; 32 voices about 2.5 %.
- **Proposal:** inside `run_plain`, when `!filtered && env.stage == Stage::Sustain`, play a run whose length is worked out once: `n` frames that surely stay two frames inside the region or loop (one division a block), with `const float a = v.shape.sustain * gain * fade;` (what `env.next()` returns there, times the same factors in the same order) and a loop of only the two reads, the two sums and `position += step`. The frames left go through the loop as it is. About 26 instructions a frame.
- **Gain:** CPU Low to Medium (about 40 % off an unfiltered voice; about 1 % of a core at 32 voices).
- **Sound or behaviour change:** bit-exact (the same operations in the same order on the frames it takes).
- **Risk:** Low to Medium: the bound on `n` must be conservative both ways round (reversed, the frame at `v.end - 1` reads its neighbour through `j`).
- **Effort:** M.
- **Confidence:** Medium: instruction counts are by hand from the source, not from the compiler's output.

### SMP-10: A mono sample is held twice

- **Verification:** Confirmed. `read_wav` always sizes both `l` and `r` to the frame count (`sample_data.cpp:23-25`).
- **In the fork:** No change.

- **Where:** `src/melodee/engine/sample_data.cpp:23` (`read_wav`) and `91` (`wav_frames`); `sample_data.h:39` (`std::vector<float> left, right;`).
- **Thread and frequency:** on load; then per voice per block as cache traffic.
- **Now:**
  ```cpp
  std::vector<float> l(reader.frames()), r(reader.frames());
  ...
  for (unsigned n = 0; n < frames; ++n) { left[n] = sample(n, 0); right[n] = sample(n, std::min(1u, channels - 1)); }
  ```
  Every sample in memory is two float vectors at its own rate: 8 bytes a frame, whatever the file. A 16-bit mono hit takes four times its file. A streamed part already shares one side (`multisample.h:110`: `right; // == left: one side`), and `make_overview()` already finds whether the sides differ (`sample_data.cpp:384`).
- **Cost:** half the memory of every mono sample (a kit of 128 one-second mono hits: 22 MB of 45 MB); the load budget is `MemAvailable` less 256 MB (`shell/sample_memory.h:27`), so it also decides how much fits. A voice on a mono sample pulls 16 cache lines a block where 8 would do.
- **Proposal:** let `SampleData` hold one side when the two are alike: `right` empty and a `right_data()` that then gives `left.data()`; the sampler takes its pointers there (`sampler.cpp:92`). Everything that edits `right` (the shell's Trim, Chop, normalise, `write_wav`, `as_written`) has to go through the same door.
- **Gain:** memory Medium; CPU Low.
- **Sound or behaviour change:** bit-exact (the voice reads the same values).
- **Risk:** Medium: many users of `left`/`right` outside my area; an edit that makes the sides differ must split them again.
- **Effort:** L.
- **Confidence:** High on the engine side; I did not count the shell's users.

### SMP-11: The Looper's block runs in full when the Looper is off, and frame by frame when its loops are empty

- **Verification:** Confirmed. `Looper::run` clears eight output buffers and carries on without testing whether the Looper is on (`looper.cpp:583-600`).
- **In the fork:** No change.

- **Where:** `src/melodee/engine/looper.cpp:583` (`Looper::run`), called every block at `engine_host.cpp:3351`; `looper.cpp:769-793` (`publish`).
- **Thread and frequency:** audio, every block.
- **Now:** off: `run()` zeroes 4 kB of output (585), goes through its queues and loops, then publishes all four loops (766): about twenty atomics each and
  ```cpp
  for (unsigned c = 0; c < looper::peak_columns; ++c) v.peaks[c].store(l.peaks[c], std::memory_order_relaxed);
  ```
  1,024 single-byte atomic stores a block for waveforms that have not changed. On, with every loop empty or stopped, the frame loop (739-741) still calls `play_frame` 128 times a loop, each returning at line 500.
- **Cost:** off about 3,000 cycles (under 2 µs, 0.06 % of a core); on and empty about 13,000 (7 µs, 0.25 %). Small, but paid every block.
- **Proposal:** publish a loop's peaks only when they changed (a flag set in `write_at`, `peak_column`, `reset`, `begin_take` at 349, and the halving at 466); return after the bookkeeping when `!enabled`; in the frame loop skip a run for a loop with `l.gain <= 0`, no sound due and `l.write == Write::Off` (adding `per * count` to `halt_tick` when halted, and clearing `was_stretched`).
- **Gain:** battery Low.
- **Sound or behaviour change:** bit-exact.
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** High.

### SMP-12: About 68 MB is made and zeroed at start-up for features that may never be used

- **Verification:** Confirmed. `TakeRecorder` allocates and zeroes two buffers of 60 s at the highest rate at construction (`take_recorder.h:34, 39-40`): 2 × 5.76 M floats = 46 MB.
- **In the fork:** No change.

- **Where:** `src/melodee/engine/take_recorder.h:34` and `40`; `punch.h:40` and `44`; `sampler.h:405-407` and `413`.
- **Thread and frequency:** on start.
- **Now:**
  ```cpp
  static constexpr uint32_t capacity = 60 * uint32_t(dsp::max_rate);      // its buffer: 60 s at the highest rate
  : left_(buffer ? new float[capacity]() : nullptr), right_(buffer ? new float[capacity]() : nullptr) {
  ```
  The take buffer is 46 MB (2 × 5,760,000 floats, value-initialised, so every page is touched). Ten punch histories are 15.4 MB. Each of the sixteen samplers keeps 402 kB of Tones band-limited tables (40 voices × `BandRows`) and a 34 kB `AlignScratch`: 7 MB, used only by a Tones voice read faster than 1×.
- **Cost:** 68 MB of a 2 GB machine with no swap, before any sample is loaded; it comes off what `memory_allows()` lets a project load. The Looper's 181-393 MB is asked for only when the Looper is turned on and is documented (`docs/LOOPER.md:88-92`).
- **Proposal:** none of these can be made on the audio thread, so only where the need is known ahead: size the take buffer for the device's rate when the rate is set (21 MB at 44.1 kHz in place of 46 MB) from the service thread; give the band tables to voices from a small shared pool (say eight) in place of 40 a kit × 16 kits. Low priority.
- **Gain:** memory Low.
- **Sound or behaviour change:** none.
- **Risk:** Medium for the take buffer (a rate change while armed); Low for the tables.
- **Effort:** M.
- **Confidence:** High on the sizes.

### SMP-13: A held Pad FX works out constants and LFO-swept coefficients with libm on every frame

- **Verification:** Confirmed. A held Low/High/Phone/Wobble/Sweep punch computes `std::tan` (and `exp2`, `sin` for the swept kinds) per frame (`punch.cpp:168-171`).
- **In the fork:** No change.

- **Where:** `src/melodee/engine/punch.cpp:168-171` (Low, High, Phone, Wobble, sweeps), `215` (Vinyl); `punch_mpc.cpp:114-116` (LP/HP/BP Filter), `133` (LoFi), `146`, `165`, `176` (Color).
- **Thread and frequency:** audio, per frame, while a punch of that kind is held (a kept pattern punch or Flex Beat on Loop can be on for a whole song).
- **Now:**
  ```cpp
  float g = std::tan(dsp::pi * std::clamp(hz, 30.0f, 18000.0f) / dsp::sample_rate()), k = kind == PunchKind::Phone ? 1.2f : 0.7f;   // punch.cpp:171
  float g = std::tan(dsp::pi * 3500 / dsp::sample_rate()) / (1 + std::tan(dsp::pi * 3500 / dsp::sample_rate()));                     // punch.cpp:215
  float a = 1 - std::exp(-two_pi * 7000 / fs());                                                                                     // punch_mpc.cpp:146
  ```
  For Low, High and Phone `hz` is a constant; Vinyl's two `tan` and Color's `exp` are constants; LoFi's `exp2` is a parameter. The LP/HP/BP Filter calls `sin`, `exp2` and `tan` a frame, where the Phaser beside it already retunes every 16 frames (`punch_mpc.cpp:91`).
- **Cost:** one to three libm calls a frame, 0.15-0.8 % of a core while held.
- **Proposal:** put the constants in `per_frame` in `start()` (it exists for this: `punch.h:101-107`), and `set_params()` for LoFi. For the swept filters, retune every 16 frames as the Phaser does.
- **Gain:** CPU Low.
- **Sound or behaviour change:** the constants bit-exact (the same expression, worked out once; `start()` already fixes the rate for the hold); the swept filters change by a 16-frame stair on the cutoff, not audible but not bit-exact.
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** High.

## Audio engine: effects and analysis

### FX-1: Track, bus, master and loop chains never rest: every effect runs every block, on silence too

- **Verification:** Merged. Same issue as HOST-1.
- **In the fork:** See HOST-1.

### FX-2: The slot Compressor calls `log10f` and `powf` on every sample, also when it is not compressing

- **Verification:** Confirmed. The Compressor case computes `log10` and then `db(-reduce)` for every frame, with no shortcut when `over <= 0` (`effects.cpp:553-560`).
- **In the fork:** Implemented. Bit-exact: 370 million gains compared against the original expressions, including every float within 20,000 steps of each threshold (`.local/checks/compressor_exact.cpp`).

- **Where:** `engine/effects.cpp:553-560` (`frames_of`, `case K::Compressor`).
- **Thread and frequency:** audio per sample.
- **Now:**
  ```cpp
  float level = std::max(std::fabs(heard_l[f]), std::fabs(heard_r[f]));
  env = level > env ? level + (env - level) * d : level + (env - level) * e;
  float over = 20 * std::log10(std::max(env, 1e-6f)) - a;
  float reduce = over > 0 ? over * (1 - 1 / b) : 0;
  float g = db(-reduce) * c;          // db(x) = std::pow(10.0f, x / 20.0f)
  wet_l = l * g; wet_r = r * g;
  ```
  The master compressor runs its gain computer every fourth sample (`master_comp.cpp:75`); this one runs it on every sample.
- **Cost:** two maths calls and two divisions per frame, about 80 of the roughly 100 ns the effect takes. That is about 13 µs per block per Compressor. `dsp_bench.cpp` puts a Compressor on four of its sixteen tracks. Measure with a per-effect loop over `Effect::process`.
- **Proposal:** skip the gain computer while the envelope is under the threshold. When `over <= 0` the code computes `pow(10, -0.0f / 20) * c`, and `pow(x, -0)` is exactly 1, so `g == c` exactly. Keep a linear threshold with a small guard band in `update()` and only take the logarithm above it:
  ```cpp
  // update(): thresh_lin = db(a) * 0.999f;   keep_ratio = 1 - 1 / b;
  float g = c;
  if (env > thresh_lin) {
      float over = 20 * std::log10(std::max(env, 1e-6f)) - a;
      float reduce = over > 0 ? over * keep_ratio : 0;
      g = db(-reduce) * c;
  }
  ```
  The band (0.009 dB) is hundreds of times wider than the rounding error of `20 * log10f(env)`, so every sample the shortcut takes would have had `over < 0` anyway.
- **Gain:** CPU Medium. The Compressor drops from "heavy" to "cheap" for every sample under its threshold, which includes all silence.
- **Sound or behaviour change:** bit-exact.
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** High on the logic. Medium on the saving, which depends on how much of the material sits under the threshold.

### FX-3: Slow LFOs are computed with `std::sin` on every sample in ten effects and in the master compressor's Color

- **Verification:** Confirmed. For example Chorus/Flanger call `std::sin` twice per frame (`effects.cpp:476`). I read three of the eleven sites.
- **In the fork:** Tried and withdrawn. Second pass. Working the swings' sines out ahead of the frames, with the same arguments, changed what Chorus, Flanger and Tape echo played against the original build (the two ways agreed with each other inside one build: the compiler fused the surrounding sums differently once the code had moved) and saved no instructions. Reverted.

- **Where:** `engine/effects.cpp:462` (Tape echo), `:476` (Chorus, Flanger: two calls), `:498` (Auto-pan), `:504` (Tremolo), `:521` (Tape); `engine/effects_extra.cpp:307` (Ensemble: six calls), `:322` (Multi-chorus: one call per voice, 3, 4 or 6), `:370` (Vinyl), `:398` (Tape emulator); `engine/master_comp.cpp:40` (Color).
- **Thread and frequency:** audio per sample.
- **Now:** for example Chorus, and the 0.6 Hz wow of Tape echo:
  ```cpp
  lfo += a / fs(); lfo -= std::floor(lfo);
  float ml = base + depth * b * std::sin(two_pi * lfo), mr = base + depth * b * std::sin(two_pi * (lfo + 0.25f));
  ...
  if (Kind == K::TapeEcho) { lfo += 0.6f / fs(); lfo -= std::floor(lfo); back *= 1 + c * 0.004f * std::sin(two_pi * lfo); }
  ```
  Five of these LFOs run at a fixed rate under 1 Hz (Tape echo 0.6, Tape 0.8, Vinyl 0.55, Tape emulator 0.7, Color 0.6). Phaser and Pedal phaser already evaluate theirs once every 16 frames (`effects.cpp:414`, `:484`).
- **Cost:** one `sinf` is about 25 to 40 ns. Per frame that is about 30 ns of 55 for Auto-pan and Tremolo, 60 of 120 for Chorus and Flanger, 180 of 260 for Ensemble, 120 to 180 for Multi-chorus, and 30 of about 110 for the four wow effects and Color.
- **Proposal:** keep `lfo` advancing exactly as now, and get the sine from a rotation that is re-seeded from `lfo` at the start of every `process()` call:
  ```cpp
  const float inc = a / rate;                                   // once per call
  const float cw = std::cos(two_pi * inc), sw = std::sin(two_pi * inc);
  float s = std::sin(two_pi * first_phase), c = std::cos(two_pi * first_phase);
  // per frame: use s (and c for the tap a quarter turn on), then
  float n = s * cw + c * sw; c = c * cw - s * sw; s = n;
  ```
  That is four maths calls per block instead of 128 to 768. Ensemble's three voices a third of a turn apart and Multi-chorus's voices each need their own seed (and Multi-chorus its own `cw`, `sw` per voice, which can be kept in `update()`).
- **Gain:** CPU Medium for the modulation effects (about half their time), Low for a whole project.
- **Sound or behaviour change:** rounding only. Because the rotation restarts from the true phase every block, its error stays near 1e-6 of the LFO's depth.
- **Risk:** Low. Check Tremolo's square shape and Ensemble's second LFO (`env2`) are handled apart.
- **Effort:** M (ten sites, one small helper).
- **Confidence:** High that the calls are per sample. Medium on the nanoseconds.

### FX-4: Values that are constant for the whole block are recomputed on every sample

- **Verification:** Partly confirmed. I read the echo case: `0.6f / fs()` and `sync_back()` run per frame (`effects.cpp:461-462`). The other sites in the list were not read one by one.
- **In the fork:** Implemented in part. Second pass. The echoes' SYNC look-up (`sync_back()`, two calls into another file for every frame) is made once a call in Echo, Tape echo and Ping pong; each frame keeps its glide. The other sites are unchanged: each has to be shown to compile to the same sums, and the one tried next did not (FX-3).

- **Where:** the per-frame loops of `frames_of` (`engine/effects.cpp:357`) and `extra_frames_of` (`engine/effects_extra.cpp:180`), and `MasterCompressor::colour` (`engine/master_comp.cpp:38`); the sites are listed under Now.
- **Thread and frequency:** audio per sample.
- **Now:**
  - A division by the sample rate for every LFO step: `lfo += a / fs()` at `effects.cpp:401, 413, 462, 474, 483, 497, 503, 520`, `effects_extra.cpp:301-302, 321, 368, 391`, and `wobble += 0.6f / fs()` at `master_comp.cpp:38`. `fs()` is an atomic load the compiler cannot hoist (see FX-9).
  - `effects.cpp:461` calls `sync_back()` per frame in Echo, Tape echo, Ping pong and (`effects_extra.cpp:187`) Diff delay. It makes two calls into another source file (`delay_sync_param`, `delay_sync_quarters`, `effect_info.cpp:149-159`) on every sample, only to learn that SYNC is off.
  - `effects.cpp:434` (Spit): `float steps = std::exp2(10 - b * 7);` on every frame, used only while a spit lasts.
  - `effects.cpp:511` (Drive) and `:523-524` (Tape): `/ std::sqrt(a)` per frame.
  - `effects_extra.cpp:279` (Tube drive): `dsp::soft_clip(c)` (one division) and `std::sqrt(a)`, per side, per frame.
  - `effects_extra.cpp:248-256` (Pumper): the ATTACK, HOLD, RELEASE and SHAPE values are rebuilt from the integer parameters on every frame, with five divisions by `100.0f`:
    ```cpp
    float A = 0.005f + p[3] / 100.0f * 0.2f, H = p[4] / 100.0f * 0.3f, R = std::max(0.02f, p[5] / 100.0f * (1 - A - H));
    ...
    else if (t < A + H + R) duck = std::pow(1 - (t - A - H) / R, 1 + p[1] / 100.0f * 4);
    float target = 1 - p[2] / 100.0f * duck;
    ```
  - `effects_extra.cpp:321, 326` (Multi-chorus): `a * (1 + 0.11f * n) / fs()` per voice and `2.0f / float(delay)` per frame.
- **Cost:** per frame, about 7 ns for each LFO division, 10 to 15 ns for the `sync_back` calls (a quarter of Echo's time), 25 to 30 ns for Spit's `exp2f`, 30 ns for Tube drive, 40 ns for Pumper's parameter maths.
- **Proposal:** compute each of these once, either in `update()` (parameter-only values: Spit's `steps`, `sqrt(a)`, `soft_clip(c)`, Pumper's `A`, `H`, `R`, exponent and depth, Multi-chorus's scale) or in a local before the frame loop (anything that reads the rate or the clock: the LFO steps, the sync target). `sync_back` keeps only its glide per frame: `synced += (target - synced) * glide[1]`.
- **Gain:** CPU Low to Medium, spread over about twenty effects.
- **Sound or behaviour change:** bit-exact, as long as each hoisted value is a whole sub-expression of the original (g++ fuses `a * b + c` into one instruction on this CPU, so splitting an expression in a new place can change the last bit; not checked in the build's assembly). The host tests that compare output sample for sample would catch a slip.
- **Risk:** Low.
- **Effort:** M (mechanical, many small sites).
- **Confidence:** High that the work is repeated. I did not look at the compiled code, so the compiler may already merge a few of these (for example the two `sqrt(a)` in Drive).

### FX-5: Every sample load re-runs the full onset, beat and auto-chop analysis; nothing is kept

- **Verification:** Merged. Same issue as IO-4 and IO-5.
- **In the fork:** See IO-4.

### FX-6: The master compressor does its full work on silence, and Color adds a `sin` and five divisions per sample

- **Verification:** Merged. Same issue as HOST-9.
- **In the fork:** See HOST-9.

### FX-7: Stereo filter chains run as two scalar passes although both sides use the same coefficients

- **Verification:** Confirmed. EQ runs three biquads on the left and then three on the right as separate scalar chains (`effects.cpp:533-534`).
- **In the fork:** No change.

- **Where:** `engine/effects.cpp:533-534` (EQ, 3 biquads a side), `:490-491` (Phaser, 6 allpasses a side), `:421-422` (Pedal phaser, 4), `:448-456` (Formant, 3 state-variable filters a side), `:538-545` (Filter); `engine/effects_extra.cpp:263-270` (Amp sim, 6 biquads a side), `:235` (Spring, 12 allpasses a side), `:340-347` (Auto-wah), `:356-363` (Vintage), `:277-283` (Tube drive).
- **Thread and frequency:** audio per sample.
- **Now:** for example EQ and Phaser:
  ```cpp
  wet_l = eq[2].run(eq[1].run(eq[0].run(l)));
  wet_r = eq[5].run(eq[4].run(eq[3].run(r)));
  ...
  for (unsigned side = 0; side < 2; ++side)
      for (auto& s : allpass[side]) { float y = env2 * x[side] + s; s = x[side] - env2 * y; x[side] = y; }
  ```
  Each side is a serial chain, and the A55 executes in order, so the right side simply waits for the left. The state is laid out side by side by filter (`allpass[side][n]`, `eq[side * 3 + n]`), which stops the compiler pairing left with right.
- **Cost:** these chains are most of the time of EQ, Phaser, Pedal phaser, Formant, Amp sim and Spring (about 40 to 115 ns per frame each).
- **Proposal:** hold the state as `[stage][side]` pairs and run each stage once on a two-lane NEON vector (`float32x2_t`), by intrinsics or by a loop the compiler can pair. The coefficients are already identical for both sides (`update()` fills both from the same values, `effects.cpp:204-211`, `effects_extra.cpp:119-127`).
- **Gain:** CPU Medium for these kinds (perhaps 30 to 45 % of their time), Low for a whole project.
- **Sound or behaviour change:** rounding only at worst. The arithmetic per lane is the same, but whether the result is bit-exact depends on the compiler fusing multiply-adds the same way in both forms.
- **Risk:** Medium (hand-written vector code in ten places, or a refactor that needs its assembly checked).
- **Effort:** L.
- **Confidence:** Medium. I have not seen the compiled code and the saving is an estimate.

### FX-8: Divisions per sample by a value that only changes with a parameter

- **Verification:** Confirmed. Formant divides by `(1 + g * (g + formant_k))` six times per frame (`effects.cpp:452`); the sampler's pad filter has the same shape (`sampler.cpp:919`).
- **In the fork:** No change.

- **Where:** the state-variable filter's `/ (1 + g * (g + k))` at `engine/effects.cpp:452` (Formant, six per frame), `:541` (Filter, two), `engine/effects_extra.cpp:343` (Auto-wah, two), `:359` (Vintage, two). Constant divisors: `/ 1.3f` (`effects.cpp:405-406`), `/ (1 + b * 0.5f)` (`:441`), `/ 1.4f` (`effects_extra.cpp:402`), `/ d` (`:362`), `/ 1.1f` (`master_comp.cpp:48-49`).
- **Thread and frequency:** audio per sample.
- **Now:**
  ```cpp
  float hp = (x[side] - (formant_k + g) * s.s1 - s.s2) / (1 + g * (g + formant_k));
  ```
  The build has no `-ffast-math`, so the compiler must keep every one of these as a real division.
- **Cost:** about 6 to 7 ns each; Formant spends about 40 of its roughly 90 ns per frame in them.
- **Proposal:** store the reciprocal in `update()` (for Auto-wah, where `svf_g` changes every eighth frame, at that point) and multiply.
- **Gain:** CPU Low (Medium for Formant).
- **Sound or behaviour change:** rounding only (the last bit of each product).
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** High.

### FX-9: The frame loops reload and store every member on every sample because the audio pointers may alias the effect

- **Verification:** Not checked. A claim about what the compiler does with member loads in `frames_of`; it needs the generated assembly, which I did not read.
- **In the fork:** No change.

- **Where:** `engine/effects.cpp:338` (`frames_of`), `:280` (`reverb_block`), `engine/effects_extra.cpp:177` (`extra_frames_of`).
- **Thread and frequency:** audio per sample.
- **Now:** the loops read and write `float` members (`lfo`, `env`, `tone_l`, `allpass[][]`, `damp`, `feedback`, `mix`, `on` and so on) and store through plain `float*`:
  ```cpp
  float m = blend();
  left[f] = l + (wet_l - l) * m;
  right[f] = r + (wet_r - r) * m;
  ```
  A store through `float*` may legally change any `float` member, and `dsp::sample_rate()` reads a global the same store could change. So after each such store the compiler has to reload everything, and no state stays in a register across frames. In `reverb_block` the comb loop reloads `damp` and `feedback` and recomputes `1 - damp` for every sample (`effects.cpp:300-301`).
- **Cost:** on an in-order core each of these loads and stores takes an issue slot. For Phaser that is about 30 extra memory operations per frame on top of about 60 arithmetic ones. I estimate 10 to 20 % of the time of the filter-heavy effects.
- **Proposal:** copy the state and parameters a kind uses into locals before the loop and write the state back after it (the reverb's comb loop already does this for `at`, `length` and `filter`); or mark `left`, `right` and the line pointers `__restrict`. Take care with Compressor and Gate: `heard_l` is the same pointer as `left` unless keyed (`effects.cpp:635`), so those two must not get `__restrict`.
- **Gain:** CPU Low to Medium.
- **Sound or behaviour change:** bit-exact.
- **Risk:** Low with locals; Medium with `__restrict` if a caller ever passes the same buffer twice (none does today: `run_chain`, the pad lanes and the macro stage all pass two buffers).
- **Effort:** M.
- **Confidence:** Medium. The aliasing rule is certain; how much g++ loses to it here needs a look at the assembly.

### FX-10: Shimmer's octave shifter calls `fmod` and `sin` twice per sample

- **Verification:** Confirmed. Shimmer's two taps each call `std::fmod` and `std::sin` per frame (`effects.cpp:376-378`).
- **In the fork:** No change.

- **Where:** `engine/effects.cpp:376-386` (`frames_of`, `case K::Shimmer`).
- **Thread and frequency:** audio per sample.
- **Now:**
  ```cpp
  for (float offset : {0.0f, window / 2}) {
      float ph = std::fmod(shift_phase + offset, window), back = window - ph + 1;
      float g = std::sin(dsp::pi * ph / window);
      ...
      out[side] += (x0 + (x1 - x0) * fr) * g * g; // sin² windows half a window apart sum to one
  ```
  Shimmer also runs the reverb one frame at a time (`reverb()`, `:250`), not in the block form the other three reverbs use, because its shifted sound feeds the next frame.
- **Cost:** two `sinf`, two `fmodf` and two divisions, about 100 of Shimmer's roughly 200 ns per frame. Shimmer is the heaviest of the reverbs.
- **Proposal:** `shift_phase + offset` is always under `2 * window`, so `fmod` is a compare and a subtract (exact in float, so bit-exact). The two window gains are `sin` and `cos` of the same angle; take them from a rotation re-seeded each block as in FX-3.
- **Gain:** CPU Low to Medium (one effect).
- **Sound or behaviour change:** rounding only for the sine; the `fmod` change is bit-exact.
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** High.

### FX-11: Short-delay effects sweep their whole 1.15 MB block; the reverb's predelay ring is two thirds of its cache footprint

- **Verification:** Confirmed. Chorus and Flanger wrap their write index at `DelayPool::half` though they read at most a few milliseconds back (`effects.cpp:479`).
- **In the fork:** No change.

- **Where:** `engine/effects.cpp:479, 525, 409, 585` (`if (++write >= DelayPool::half) write = 0;` in Chorus, Flanger, Tape, Pedal chorus, the look-ahead Limiter) and `:246` (`delayed`); `:285-287` (the reverb's predelay).
- **Thread and frequency:** audio per sample.
- **Now:** Chorus reads at most 22 ms back (2,112 frames at 96 kHz), Flanger 5.5 ms, Tape 8 ms, Pedal chorus 13.5 ms, the Limiter 10 ms. All five still write round a ring of `DelayPool::half` = 144,000 frames a side, so each block touches cache lines last used 3.3 s ago. The reverb writes a 9,600-frame predelay ring a side (77 kB for both) whatever PREDELAY is; Plate starts at PREDELAY 0 and Shimmer forces `predelay = 0` (`effects.cpp:151`). At 44.1 kHz with the initial values the combs and allpasses of Room, Reverb and Plate take 38 to 54 kB, so the ring is 59 to 67 % of the memory a reverb cycles through.
- **Cost:** unknown without a measurement. Each such effect pulls about 16 cold cache lines per block and pushes as many useful ones out. The SoC's caches are small (see "Could not determine").
- **Proposal:** (a) for the five short-delay kinds, wrap the stored index at a divisor of 144,000 that covers the longest reach, for example 4,500 frames, and index with `% 4500`. `write` and `delayed()`'s position arithmetic stay as they are, so the same samples are read with the same fractions. (b) For Shimmer, where the predelay is always 0, feed the reverb the input directly: `pre[back]` is the value written one line earlier.
- **Gain:** CPU Low; memory traffic Low to Medium.
- **Sound or behaviour change:** bit-exact for both.
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** High on what the code touches; Low on the gain.

## Plug-in hosting

### PLUG-1: An instrument plug-in never sleeps while the song plays

- **Verification:** Confirmed. `IdleGate::asleep` returns false for an instrument whenever `playing` is true (`idle_gate.h:62`). The header comment shows this is the author's choice, for plug-ins that run their own sequencer or arpeggiator from the transport.
- **In the fork:** Not implemented. The author's deliberate behaviour (plug-ins that follow the transport); TRANSPORT off already gives the sleeping behaviour per plug-in.

- **Where:** `src/melodee/plugin/idle_gate.h:62` (`IdleGate::asleep`), `src/melodee/plugin/plugin_rack.cpp:358` (`follow_clock`), `src/melodee/model/project.h:373`.
- **Thread and frequency:** audio (or a render worker), per block, for every PLUGIN track, for as long as the transport runs.
- **Now:** playing keeps every instrument awake, whether or not it has notes:
  ```cpp
  if (midi_waiting) { quiet = 0; return false; }
  if (instrument) {
      if (held_count || sustain || playing) return false;
  ```
  `playing` is forced false only when the entry's TRANSPORT is off, and `PluginRef::transport` defaults to `true`. The reason is given in the header: a plug-in with its own sequencer plays from Play alone.
- **Cost:** the plug-in's full idle DSP on every block of playback, on a track whose pattern is empty, or between its phrases. `tools/vst_probe/README.md:125` measured Acid-303 at 22 % of a block mean whatever it plays, and 8W8 at 2.8 %. Measure with `status` (`plugins=` gives each entry's load in tenths of a percent, `plugin_rack.cpp:541`): play a pattern with no notes on the plug-in's track and compare with stopped, where it reads 0.
- **Proposal:** two steps.
  1. Safe, CLAP only: keep the status `plugin->process()` returns (`clap_instance.cpp:626`) and let the gate sleep while playing when the last status was `CLAP_PROCESS_SLEEP`. A CLAP plug-in with a running sequencer returns `CONTINUE`.
  2. All formats, a behaviour change: let an instrument sleep while playing once it has been quiet for a long window (for example 8 s or two bars, counted again from each Play), unless it has shown that it sounds by itself (output above -100 dB while playing, no key held, long after the last MIDI). Sketch:
     ```cpp
     if (instrument) {
         if (held_count || sustain) return false;
         if (playing && (self_playing || play_quiet < play_quiet_needed)) return false;
     }
     ```
     with `self_playing` set in `heard()` and `play_quiet` reset on the stop-to-play edge in `follow_clock`.
- **Gain:** CPU High and battery High during playback for projects with plug-in instruments that rest; none when stopped.
- **Sound or behaviour change:** step 1 bit-exact for a plug-in that reports honestly. Step 2 audible in one case: a plug-in sequencer whose first sound comes later than the window after Play stays silent until it is sent MIDI.
- **Risk:** step 1 Low. Step 2 Medium, for the case above; TRANSPORT off already gives users the sleeping behaviour per entry today.
- **Effort:** step 1 S; step 2 S–M plus tests beside the existing ones (`tests/melodee.cpp:11985`).
- **Confidence:** High on what the code does. Low on the gain for CLAP: wrappers built with clap-juce-extensions (Surge XT) may never return `SLEEP`; not checked.

### PLUG-2: The armv7 bridge busy-waits on both sides of every block

- **Verification:** Confirmed. The host spins 20,000 loads before sleeping on the futex (`vst2_bridge_instance.cpp:221-223`); the bridge spins 4,000 and its `futexWait` has a 100 ms timeout (`vst2_bridge32.cpp:38-41, 280-283`). The comments show the spins were chosen on purpose; whether a wake costs more than the spin is unmeasured.
- **In the fork:** Implemented. The host still spins for every command, but a process block whose reply outlasts the spin makes the next 64 blocks wait on the futex at once; then one spins again, so a plug-in that has become quicker is caught again. The bridge spins 1,000 loads after a reply instead of 4,000, enough for a command that follows at once (a block rendered in parts, a run of parameter reads), and then waits with no timeout, so an idle bridged plug-in no longer wakes 10 times a second. Bit-exact: `.local/checks/bridge-wait.sh` runs Melodee's `Vst2BridgeInstance` against `vst2_bridge32` built for aarch64 and a fixture plug-in with a set DSP time; the output hashes are the same before and after in every scenario (steady blocks, blocks in two parts, parameter reads between blocks), and an idle bridge went from 9 wake-ups a second to 0. The VM's CPU times are too noisy to put a number on the saving (two rounds of the same build differed by 2-3x); by the loop counts, a slow plug-in's host spin (20,000 loads) now runs on 1 block in 65, and the bridge's spin after every block is a quarter of what it was. Not yet measured on the MPC with a real armv7 plug-in.

- **Where:** `src/melodee/plugin/vst2_bridge_instance.cpp:221` (`Connection::call`), `src/melodee/plugin/bridge32/vst2_bridge32.cpp:280` and `:40` (`main`, `futexWait`).
- **Thread and frequency:** host: audio thread or a render worker, once per `processBlock` of each bridged instance (more when an instrument is rendered in parts). Bridge: its `SCHED_FIFO` 30 thread, after every reply.
- **Now:** host:
  ```cpp
  for (int spin = 0; spin < 20000; ++spin)
      if (bridge::load (shared->reply) == request)
          return true;
  ```
  bridge:
  ```cpp
  uint32_t request = bridge::load (shared->request);
  for (int spin = 0; request == handled && spin < 4000; ++spin)
      request = bridge::load (shared->request);
  if (request == handled) { futexWait (&shared->request, handled); continue; }
  ```
  The bridge comment says "in steady playback the next block arrives within a period, and a wake-up costs more than the spin". The next block arrives 2.9 ms later, so that spin always runs out. `futexWait` there has a 100 ms timeout, so an idle bridge wakes 10 times a second and spins each time.
- **Cost:** the built host loop is 7 instructions per iteration (`ldar`), the bridge loop about 10 including `dmb ish`. On an in-order A55 at 1.8 GHz that is roughly 50–100 µs for the host and 25–55 µs for the bridge (unmeasured). Whenever the plug-in's block takes longer than the host's spin (Acid-303: 624 µs mean), the whole host spin is wasted and the host then sleeps on the futex anyway. Together about 75–155 µs of a 2902 µs period: 2.5–5 % of one core per bridged instance while it plays. With two or three bridged plug-ins on render workers, spinning waiters and bridge threads are all at real-time priority on four cores, so a spinner can hold the core the bridge it waits for needs. Measure: set both spin counts to 0 in a test build and compare the plug-in's load and the bridge's CPU time in `/proc/<pid>/stat` over a minute; `measureRoundTrips` exists for the handoff alone.
- **Proposal:** make both spins adaptive, and drop the idle timeout.
  - Bridge: time the gap from a reply to the next request (`clock_gettime`); spin only when the last gap was short (a burst of parameter calls, or an instrument rendered in parts). Wait with no timeout: `PR_SET_PDEATHSIG` already ends the bridge when the host dies.
  - Host: remember whether the last `process` reply arrived inside the spin; if it did not, skip the spin for the next blocks and go straight to `futexWait`, trying the spin again every so often.
- **Gain:** CPU Medium for projects with bridged plug-ins; battery Low–Medium while they play; removes 10 wakeups a second per idle bridged instance.
- **Sound or behaviour change:** bit-exact.
- **Risk:** Low. A block whose reply would have been caught by the spin now costs one real-time futex wake (the README's peaks suggest tens of µs).
- **Effort:** S.
- **Confidence:** Medium. Instruction counts are from the built objects; cycle counts and wake latency are not measured.

### PLUG-3: The audio thread waits on the bridge's lock behind program and state calls

- **Verification:** Confirmed. `processBlock` takes `connection->lock` with a plain `lock_guard` (`vst2_bridge_instance.cpp:302`), the same lock `Connection::call` holds for a whole round trip (`:213`).
- **In the fork:** No change.

- **Where:** `src/melodee/plugin/vst2_bridge_instance.cpp:302` (`processBlock`), `:353` (`setCurrentProgram`), `:372` (`getStateInformation`), `:398` (`setStateInformation`), `:211` (`Connection::call`, default timeout 2000 ms).
- **Thread and frequency:** audio per block, against the rack thread (PRESETS) and the MIDI thread (save, autosave, track copy, undo, print: `PluginRack::states()`/`state()`/`clone()`).
- **Now:** every exchange holds one lock for its whole round trip, and the audio thread takes it unconditionally:
  ```cpp
  void Vst2BridgeInstance::processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi)
  {
      const std::lock_guard<InheritingMutex> held (connection->lock);
  ```
  ```cpp
  const std::lock_guard<InheritingMutex> held (connection->lock);
  shared->i0 = index;
  call (bridge::Command::setProgram);
  ```
  The header says the audio thread waits "for that one read", which holds for a parameter read. It does not hold for `setProgram`, `getState` (the plug-in builds its chunk, then up to 1 MB is copied) or `setState`. Priority inheritance does not help: the work is in another process, already at `SCHED_FIFO` 30. `InheritingMutex::try_lock()` exists and is not used.
- **Cost:** while the bridge runs one of those commands, `render()` does not return, so every track drops out, not only that plug-in. How long depends on the plug-in; a 3 ms chunk save is a missed block. Not measured. Separately, a slow `process` holds the audio thread for up to `bridge_block_ms = 40` (`plugin_rack.cpp:34`), 14 periods, before the plug-in counts as crashed.
- **Proposal:** bound the audio thread's wait. Give `InheritingMutex` a timed lock (`pthread_mutex_clocklock`, `CLOCK_MONOTONIC`) and use it in `processBlock` with a short limit (a few hundred µs covers a parameter read); on timeout clear the buffer and return, so only this plug-in is silent while it loads a program or saves. A plain `try_lock` would be wrong: the 20 Hz card refresh (PLUG-9) would silence a block now and then.
- **Gain:** latency: removes a whole-engine dropout. Medium.
- **Sound or behaviour change:** audible only where today the whole mix would stall: that plug-in is silent for those blocks.
- **Risk:** Low–Medium. The short silence clicks for a sounding instrument; the limit needs choosing on the device.
- **Effort:** S.
- **Confidence:** High that the wait is unbounded by design. Low on how often it bites: I did not measure any plug-in's `setProgram` or `getChunk`.

### PLUG-4: JUCE's LV2 and VST3 hosts keep waking for plug-ins that are asleep

- **Verification:** Confirmed. JUCE's LV2 worker sleeps 1 ms in a loop when it has no work (`juce_LV2PluginFormatImpl.h:1155-1163`) and its VST3 parameter dispatcher starts a 60 Hz timer (`juce_VST3PluginFormatImpl.h:148`).
- **In the fork:** No change.

- **Where:** `build/deps/JUCE/modules/juce_audio_processors_headless/format_types/juce_LV2PluginFormatImpl.h:1151–1163` (`SharedThreadedWorker`), `juce_VST3PluginFormatImpl.h:148` and `:2250` (`EditControllerParameterDispatcher`), reached from `src/melodee/plugin/plugin_rack.cpp:1019`. JUCE is pinned by `base/tools/dependencies.py:12`.
- **Thread and frequency:** JUCE's LV2 worker thread, every 1 ms while any LV2 instance exists; JUCE's timer and message threads, 60 times a second per VST3 instance. Neither depends on `IdleGate`, bypass or the transport.
- **Now:** LV2, one thread shared by all instances:
  ```cpp
  while (! shouldExit)
  {
      const auto submitter = incoming.pop (buffer);
      if (! buffer.empty() && submitter.isValid())
          submitter.doWork (Realtime::yes, (uint32_t) buffer.size(), buffer.data());
      else
          std::this_thread::sleep_for (std::chrono::milliseconds (1));
  }
  ```
  VST3, per instance: `startTimerHz (60);` in `start()`, which `initialise` always calls. Each tick is a timer-thread wake, a message-thread wake and a second timer-thread wake (`juce_events/timers/juce_Timer.cpp:101–124`).
- **Cost:** about 1000 wakeups a second while one LV2 plug-in is loaded, and up to about 180 per loaded VST3 instance (fewer when instances' timers coincide). CPU is small, perhaps 0.3–0.5 % of a core for LV2 (unmeasured). The base load of the app is roughly 750 wakeups a second (audio 344, MIDI 200, service 200), so one LV2 plug-in more than doubles it, stopped or playing. Measure with `voluntary_ctxt_switches` in `/proc/<pid>/task/*/status`, with and without an LV2 plug-in loaded.
- **Proposal:** nothing in `src/melodee` can turn either off. The LV2 loop is a small patch to the pinned JUCE (a longer sleep such as 20 ms, or a wait that `schedule()` signals); that is a change in sample-base's dependency step, not here. For VST3 the timer carries parameter values to the edit controller; a lower rate would also need a patch. If neither is wanted, say in the docs that an LV2 build costs standby power and a VST3 or CLAP build of the same plug-in does not.
- **Gain:** battery Low–Medium while an LV2 plug-in is loaded; Low for VST3.
- **Sound or behaviour change:** none. LV2 worker jobs (a plug-in loading a sample) would start up to the new sleep later.
- **Risk:** Low, but it means carrying a patch to a dependency.
- **Effort:** S in code; cross-repository.
- **Confidence:** High that the loops are there (read in the pinned checkout). Medium on the wakeup counts; not measured on the device.

### PLUG-5: Every plug-in on the card is listed and probed at start-up; the results are not kept

- **Verification:** Confirmed. `probe_listed` probes every loadable file not yet in `known`, and `known` lives only in memory (`plugin_rack.cpp:935-961`).
- **In the fork:** No change.

- **Where:** `src/melodee/plugin/plugin_rack.cpp:765` (`set_card`), `:819` (`run`, Browse), `:935–961` (`probe_listed`), `:894–930` (`probe`); `src/melodee/shell/plugin_jobs.cpp:289`.
- **Thread and frequency:** rack thread; once when the card is first seen (start-up, and each time a card comes back), then on every picker open for anything new.
- **Now:** the first `plugins_tick` with a card queues a listing of `Melodee/Plugins`, and a listing always ends in probing:
  ```cpp
  card_top = top;
  root = wanted_root;
  ...
  }
  browse("");
  ```
  ```cpp
  // A VST3 bundle or a CLAP is opened once even when its kind is known: it may hold several.
  if (!f.loadable() || f.id != 0 || (f.instrument >= 0 && !named_inside(f.format))) return false;
  return std::none_of(known.begin(), known.end(), [&](const Known& k) { return k.path == f.path; });
  ```
  Each probe unpacks the 11 MB support pack into `/run` and `dlopen`s its 28 libraries (first time only), copies the plug-in into `/run`, opens it (JUCE's scan for VST3/LV2, `clap_entry.init` for CLAP, a full `effOpen` for VST2, a bridge process for armv7), then deletes the copy. `known` is a vector in memory.
- **Cost:** at every boot, for a user who may not touch a plug-in that session: card reads of every bundle in the top Plugins folder while the project's samples are loading from the same card, RAM churn in `/run`, and JUCE's message thread started for good. The README gives 210–257 ms to open Acid-303. Not measured as a whole; count it with timestamps around `probe()`.
- **Proposal:** (a) do not probe from `set_card`'s listing; probe when the picker is first opened (it lists again then anyway, `plugin_jobs.cpp:83`). (b) Keep `known` on the card, keyed by path, size and modification time, beside the CLAP preset cache (`Plugin Data/.cache/melodee/`), so a plug-in is opened once ever, not once per boot.
- **Gain:** load time Medium, battery Low (once per boot), memory Low.
- **Sound or behaviour change:** none in sound. Visible: with (a) alone the INSTRUMENT/EFFECT tags of a first picker open fill in as they do now after a card change; with (b) they are there at once.
- **Risk:** Low. The cache must be dropped when a file's size or time changes, which `known` does not do today either (a plug-in replaced at the same path keeps its old tag until restart).
- **Effort:** M.
- **Confidence:** High on the flow. The cost depends on what is on the card.

### PLUG-6: Listing a folder reads every instrument library whole

- **Verification:** Confirmed. `read_drums` reads up to 32 MB of the library file to look for a drum map (`plugin_files.cpp:101-112`).
- **In the fork:** No change.

- **Where:** `src/melodee/plugin/plugin_files.cpp:101–112` (`read_drums`), called from `:183` (`describe_open_mpc`) and `:224` (`describe_plugin`); callers `list_plugins` `:250`, `PluginRack::load` `plugin_rack.cpp:972`.
- **Thread and frequency:** rack thread; every picker open, every folder change in it, every card change, and once more for each plug-in loaded.
- **Now:**
  ```cpp
  auto size = fs::file_size(library, error);
  std::ifstream in(library, std::ios::binary);
  if (error || !in) return;
  std::string bytes;
  bytes.resize(size_t(std::min<std::uintmax_t>(size, 32u << 20)));
  in.read(bytes.data(), std::streamsize(bytes.size()));
  bytes.resize(size_t(std::max<std::streamsize>(in.gcount(), 0)));
  f.drums = drum_voices_in(bytes);
  ```
  It runs for every Open MPC instrument and every bare `.so` (only a known effect is skipped). The same pass re-reads each LV2 bundle's `.ttl` files (`:134`) and re-parses each VST3's `moduleinfo.json` (`:74`). The picker's rows are replaced only when the whole listing is done (`plugin_rack.cpp:889`).
- **Cost:** the first listing reads the sum of those libraries from the SD card before any row shows; later ones copy the same bytes out of the page cache and scan them again. With 30 instruments of a few MB each that is on the order of 100 MB: seconds from the card the first time, a few hundred ms of CPU each time after (unmeasured; I do not have the plug-ins' sizes). The 32 MB buffers are freed but not trimmed.
- **Proposal:** remember each path's description (drum voices, kind, name) in the rack, keyed by size and modification time, so a repeat listing is a directory walk and `stat` calls; persist it with PLUG-5's cache. In `load()`, reuse the entry instead of describing again.
- **Gain:** load time Medium (picker open, project load); battery Low.
- **Sound or behaviour change:** none.
- **Risk:** Low.
- **Effort:** S–M.
- **Confidence:** High on the code path; Medium on the magnitude.

### PLUG-7: A plug-in's data files are copied into RAM with its code, for probes too

- **Verification:** Confirmed. `stage_copy` copies the plug-in to the RAM folder before it is opened (`plugin_rack.cpp:107-114`). I did not check what real bundles contain.
- **In the fork:** No change.

- **Where:** `src/melodee/plugin/plugin_rack.cpp:107–132` (`stage_copy`), used by `probe` `:901` and `load` `:998`.
- **Thread and frequency:** rack thread, per load and per probe.
- **Now:** the card is `noexec`, so the code must run from `/run`. But everything else is copied as well:
  ```cpp
  if (it->is_directory(e)) fs::create_directories(target / rel, e);
  else fs::copy_file(it->path(), target / rel, fs::copy_options::overwrite_existing, e);
  ```
  That is an Open MPC folder whole (skins that are never drawn), an LV2 bundle whole (its `_ui.so` too), and a VST3 bundle's `Contents/Resources`. Sibling folders are already symlinked instead (`link_neighbours`, `:95`).
- **Cost:** `/run` is tmpfs and there is no swap, so each loaded instance holds its bundle's data in RAM for as long as it is loaded, and each probe reads it from the card only to delete it. Size depends on the plug-in; not measured.
- **Proposal:** copy only ELF files; symlink every other file and folder to the card, as `link_neighbours` does. With PLUG-5's cache a probe then copies nothing in the common case.
- **Gain:** memory Medium for bundles with large resources, load time Low–Medium.
- **Sound or behaviour change:** none expected.
- **Risk:** Low–Medium: a plug-in that writes beside itself would write to the card (it writes to RAM and loses it today); a plug-in that refuses symlinked resources would need the copy.
- **Effort:** S.
- **Confidence:** Medium. I have not seen the bundles; the gain is nil for a plug-in that is one `.so`.

### PLUG-8: The idle gate ignores tails: a plug-in delay longer than half a second loses its echoes

- **Verification:** Confirmed. The gate uses one fixed `quiet_seconds = 0.5` for instruments and effects and never asks the plug-in for its tail (`idle_gate.h:21-22, 53-68`).
- **In the fork:** No change.

- **Where:** `src/melodee/plugin/idle_gate.h:21–22`, `:53–56`, `:59–68`; `src/melodee/plugin/clap_instance.cpp:626–631`, `:120`.
- **Thread and frequency:** audio, per block.
- **Now:** the window is fixed, and the only evidence is the output level:
  ```cpp
  static constexpr float quiet_level = 1e-5f; // -100 dB
  static constexpr double quiet_seconds = 0.5;
  ```
  An effect whose input stopped and whose output has been quiet for 0.5 s is no longer run, so its delay line stops. A snare sent once to a bus with a plug-in delay of 750 ms: the output is quiet from the end of the hit to the echo, more than 0.5 s, so the plug-in sleeps before the echo and never plays it. The next sound wakes it and the old echo comes out then, at the wrong time. The plug-in's own answer (`getTailLengthSeconds()`, VST3 `getTailSamples`, VST2 `effGetTailSize`, CLAP `tail` and the `process` status) is never read. Also not acted on while asleep: a program picked in PRESETS (VST3's reaches the processor "at its next block", `plugin_rack.cpp:706–711`; a CLAP preset likewise, `clap_instance.cpp:988`) and CLAP's `request_process`.
- **Cost:** this is a sound bug, not a CPU cost; fixing it gives back a little of the gate's saving.
- **Proposal:** read the reported tail once at load on the rack thread and use `max(window, min(tail, cap))` as `quiet_needed`; give effects a longer default window than instruments (seconds, not half a second), since many plug-ins report no tail. Treat a pending program change and CLAP `request_process` as a wake, like MIDI.
- **Gain:** correctness. CPU slightly negative: an effect stays awake a few seconds longer after each sound.
- **Sound or behaviour change:** audible, for the better: echoes and late reflections that are cut today play.
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** High on the logic (no code path runs a sleeping effect without input). Not reproduced with a real plug-in.

### PLUG-9: The cards of a bridged plug-in in view cost up to 36 round trips every 50 ms

- **Verification:** Confirmed. `PluginRack::panel` reads each shown parameter's name, value, text, default and step count on every call (`plugin_rack.cpp:641-652`).
- **In the fork:** No change.

- **Where:** `src/melodee/plugin/plugin_rack.cpp:641–652` (`PluginRack::panel`), `src/melodee/shell/plugin_jobs.cpp:300–309`, `src/melodee/plugin/vst2_bridge_instance.cpp:52`, `:61`, `:65–70`.
- **Thread and frequency:** MIDI thread, 20 times a second while a plug-in's page is in view (a PLUGIN track's Edit or Stage, a chain page with the cursor on a plug-in slot), static screen included.
- **Now:** for each of up to nine parameters:
  ```cpp
  std::string name = p->getName(int(sizeof(card.name) - 1)).trim().toStdString();
  put(card.name, sizeof(card.name), h->nam ? nam_card_name(name) : name);
  float v = p->getValue();
  put(card.text, sizeof(card.text), tidy(p->getText(v, 64).trim().toStdString()));
  ```
  Through the bridge that is four round trips a parameter (name, value, value again inside `getText`, display), each taking the lock the audio thread needs (PLUG-3) and waking the `SCHED_FIFO` 30 bridge. `tools/vst_probe/README.md:187` already says a real build "would cache parameter values in shared memory ... so the UI never makes its own round trip".
- **Cost:** about 720 round trips a second; back to back they are cheap (1.2 µs measured), so perhaps 0.3 % of a core plus 20 bridge wakes and trailing spins a second (unmeasured), and a small, bounded wait for the audio thread now and then. For the native formats the same refresh is direct calls and a few small allocations: negligible.
- **Proposal:** read each shown parameter's name once at load into `PluginHandle` (they do not change); in `panel()` read the value and fetch the text only when the value, row or entry changed. That leaves nine round trips a refresh.
- **Gain:** CPU Low; fewer lock meetings with the audio thread.
- **Sound or behaviour change:** none. A plug-in that changes a parameter's text without changing its value would show the old text until the row is left; refresh once a second to cover it.
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** High on the counts; Low on the cost in µs.

### PLUG-10: Small periodic work with no plug-in loaded

- **Verification:** Confirmed. `PluginRack::run` wakes every 50 ms (`plugin_rack.cpp:791`), and `plugins_tick` calls `set_card` and `sync` on every MIDI pass (`plugin_jobs.cpp:287-291`).
- **In the fork:** No change.

- **Where:** `src/melodee/plugin/plugin_rack.cpp:791` (`run`), `:754–758` (`set_card`), `:446–479` (`sync`), `:1214–1223` (`idle_plugins`); `src/melodee/shell/plugin_jobs.cpp:287–291`.
- **Thread and frequency:** rack thread 20 times a second; MIDI thread every 5 ms.
- **Now:** the rack polls whether or not anything is retiring or any CLAP plug-in exists:
  ```cpp
  wake.wait_for(held, std::chrono::milliseconds(50), [&] { return stopping || !jobs.empty(); });
  ```
  The tick builds the card's Plugins path before comparing, then takes the rack lock and, for every used entry, walks 16 tracks and 25 chains of 6 slots and builds a `std::string` from its path:
  ```cpp
  std::string wanted_root = plugin_folder(top);
  {
      std::lock_guard<std::mutex> held(lock);
      if (top == card_top) return;
  ```
  ```cpp
  std::string want = project.plugins[e].used() && referenced(project, e) ? project.plugins[e].path : "";
  ```
- **Cost:** a few µs per tick and a handful of `malloc`/`free` pairs, 200 times a second: well under 0.1 % of a core (unmeasured). 20 extra wakeups a second from the rack.
- **Proposal:** compare `top` with `card_top` before building the path; run `split_shared_plugins`/`sync` at the 50 ms cadence the cards already use (the direct `sync` calls after a pick, undo and TRANSPORT stay); let the rack wait without a timeout unless something is retiring, unoffered, or a CLAP plug-in is loaded.
- **Gain:** battery Low, CPU Low.
- **Sound or behaviour change:** none; a project change is followed up to 50 ms later.
- **Risk:** Low. The audio thread cannot signal the rack, so the timed wait must stay whenever a retired plug-in may come back.
- **Effort:** S.
- **Confidence:** High.

### PLUG-11: A zip that fails part-way is unzipped again at every listing

- **Verification:** Confirmed. A zip is removed only when every step succeeded (`plugin_files.cpp:311`), so one that fails part-way is unzipped again at the next listing.
- **In the fork:** No change.

- **Where:** `src/melodee/plugin/plugin_files.cpp:270–312` (`unzip_plugins`), called from `plugin_rack.cpp:871`.
- **Thread and frequency:** rack thread; every listing of a folder under `Melodee/Plugins` (start-up, picker open, folder change, after a delete).
- **Now:** a zip is deleted only when every entry came out and every folder moved into place:
  ```cpp
  fs::remove_all(temp, error);
  if (done) fs::remove(path, error);
  ```
  One with a bad entry, or whose folder will not rename, stays, and the next listing writes its entries to the card again and removes them again.
- **Cost:** repeated SD-card writes of the zip's contents for as long as the zip sits there. Rare, but it is the only place in this area that writes to the card without a user action.
- **Proposal:** remember a failed zip's path, size and time in the rack and skip it until one changes; it already lists dimmed as "Won't unzip".
- **Gain:** card wear and battery Low.
- **Sound or behaviour change:** none.
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** Medium: read, not reproduced.

## Shell: service work, saving, settings

### SHELL-1: Engine-mirror commands mark the project dirty, so some projects autosave about once a second while the device sits idle

- **Verification:** Confirmed. `Context::send` sets `dirty` for every command type outside its exempt list (`context.cpp:53-61`); `midi_audio_tick` re-sends `MidiAudio` once a second for a MIDI track with an audio input (`midi_track_jobs.cpp:110-121`); `autosave_due` then fires as soon as the controls have been still for 3 s.
- **In the fork:** Implemented. `MidiAudio` is exempt (see LIGHT-6), which ends the save-every-second loop.

- **Where:** `src/melodee/shell/context.cpp:53` (`Context::send`), `src/melodee/shell/midi_track_jobs.cpp:110` (`midi_audio_tick`), `src/melodee/shell/lights.cpp:64` (`Shell::autosave_tick`)
- **Thread and frequency:** MIDI thread; `midi_audio_tick` runs every 5 ms from `record_tick` (`sample_record.cpp:261`) and re-sends once a second; the autosave it triggers runs once a second for as long as nobody touches the device.
- **Now:** `Context::send` treats every command not on a short list as a project change:
  ```cpp
  // context.cpp:55-62
  // Anything but playing changes the project (the metronome is Melodee's setting, not the project's).
  switch (command.type) {
  case Type::NoteOn: case Type::NoteOff: case Type::AllNotesOff: case Type::Wheel: case Type::Bend: case Type::Pressure:
  ...
  case Type::Click: break;
  default: dirty = true;
  }
  ```
  `MidiAudio` is not on the list, and a MIDI track with an audio input (`AudioIn` not None, not "in use") is told its input again every second through that same `send`:
  ```cpp
  // midi_track_jobs.cpp:110-111, 119-120
  bool again = c.now - c.midi_audio_at >= 1000; // a track reloaded forgets: told again a second on
  if (again) c.midi_audio_at = c.now;
  ...
  if (give == c.midi_audio_sent[t] && !(again && give)) continue;
  c.send({Type::MidiAudio, uint8_t(t), 0, 0, int16_t(give)});
  ```
  `autosave_due` (`autosave.h:27-31`) needs only `dirty`, stopped transport and 3 s since the last input. After a save `dirty` is false, the next resend sets it again, and the idle condition still holds, so the save repeats at the resend rate.
- **Cost:** For a project with such a track, each second while idle: `save_project_to` on the MIDI thread under the shell lock (`context.cpp:76-83`: loop files, chops, every plug-in's state read, the whole project serialised), a full project file written to the card and renamed (`project_store.cpp:393-407`), then `ProjectStore::scan()` re-reading and parsing every project in the folder (SHELL-4), and every Looper WAV rewritten (SHELL-2). The SD card never goes idle and takes a full-file write per second. Measure with the existing counter: `ProjectStore::saves()` should not move while the device is untouched; watch it (or `ls -l --time-style=full-iso` on the project file) for a minute with a MIDI track whose AUDIO IN is set.
  The same mechanism dirties a project nobody edited in other cases: at boot the settings load sends `Repeat` to 16 tracks (`lights.cpp:298`, `context.cpp:2126`) and `record_tick` sends `Monitor` once (`sample_record.cpp:247`), so an untouched new project gets a file on the card 3 s after boot; after a load, `switch_to` clears `dirty` at `context.cpp:179` and then resets `midi_audio_sent` at `:183`, and `keyed_roots_sent` / `chained_sent` are never reset (`lights.cpp:221-226`, `scene_jobs.cpp:180-183`), so projects with a MIDI-track input, a keygroup whose root is not C, or a chain are likely rewritten once, unchanged, 3 s after they finish loading (I traced the MidiAudio path end to end, the other two only to the `send`).
- **Proposal:** Stop engine-state refreshes from touching `dirty`. Smallest change: add the mirror-only types to the exempt list in `Context::send` (`MidiAudio`, `Monitor`, `LoopMonitor`, `InputTaken`, `RecordGain`, `KeyedRoot`, `Chained`, `Repeat` index 5, `Fill`, `ExtSync`), or better, send them with `host.send()` directly as `record_tick` already does for `AudioQuiet` (`sample_record.cpp:244`). The model fields these commands mirror are set (and dirtied) where the user changes them (`set_setting`, `apply`), so nothing is lost. Add a test that runs `lights()` for a few simulated seconds on an untouched project with a MIDI-track input and checks `store.saves()` stays 0.
- **Gain:** battery High and card wear High for affected projects; removes one needless save after boot/load for the others.
- **Sound or behaviour change:** bit-exact; the only behaviour change is that these projects stop saving while nothing changed.
- **Risk:** Low. The risk is exempting a command that really is the only place a saved value changes; check each type's call sites before exempting it.
- **Effort:** S.
- **Confidence:** High for the once-a-second MidiAudio loop (read every step of the path); Medium for the after-load cases. Not run on the device.

### SHELL-2: Every save, autosaves included, rewrites every Looper loop's WAV whether or not the loop changed

- **Verification:** Merged. Same issue as IO-3.
- **In the fork:** See IO-3.

### SHELL-3: There is no idle policy at all: nothing dims, slows or powers off, and no "time since last activity" is published

- **Verification:** Merged. Same issue as BASE-3 and LIGHT-4; see IDLE-1 for the design used in the fork.
- **In the fork:** See BASE-3.

### SHELL-4: `ProjectStore::scan()` re-reads and fully parses every project in the folder after every request, and at boot before the last project is loaded

- **Verification:** Merged. Same issue as IO-1.
- **In the fork:** See IO-1.

### SHELL-5: The screen snapshot copies the whole `Project` and rebuilds 2048 pattern sketches every frame while holding the lock the MIDI thread takes on every pass

- **Verification:** Merged. Same issue as MODEL-1.
- **In the fork:** See MODEL-1.

### SHELL-6: Remote-`status` mirrors are rebuilt every 5 ms on the MIDI thread, including a walk of all 8192 sample slots

- **Verification:** Merged. Same issue as LIGHT-1.
- **In the fork:** See LIGHT-1.

### SHELL-7: Shell housekeeping runs at the lights' 5 ms rate and allocates and locks on every pass

- **Verification:** Confirmed. `plugins_tick` and the other housekeeping ticks are called from `Shell::lights` on every pass, inside `Shell::lock` (`lights.cpp:180-282`, `plugin_jobs.cpp:287-291`).
- **In the fork:** No change.

- **Where:** `src/melodee/shell/lights.cpp:190-282` (`Shell::lights`) and the ticks it calls: `plugin_jobs.cpp:289-291`, `plugin/plugin_rack.cpp:446-462` and `:754-755`, `sample_files.cpp:401`, `audio_jobs.cpp:40-46`, `context.cpp:1737-1739`, `lights.cpp:86-87`
- **Thread and frequency:** MIDI thread, every 5 ms, under `Shell::lock`.
- **Now:** Twenty-odd `*_tick()` calls run on every lights pass whether or not anything can have changed. Several build strings or take another component's mutex each time:
  ```cpp
  // plugin_jobs.cpp:289-291
  if (library) plugins->set_card(library->card()); // the card's top folder, as the sample library found it
  split_shared_plugins();
  plugins->sync(project, plugins_asleep());
  // plugin_rack.cpp:754-755
  void PluginRack::set_card(const std::string& top) {
      std::string wanted_root = plugin_folder(top);
  // plugin_rack.cpp:458
  std::string want = project.plugins[e].used() && referenced(project, e) ? project.plugins[e].path : "";
  ```
  Per pass that is: `SampleLibrary::lock` three times with a string copy each (`card()`, and `folder()` at `sample_files.cpp:401` and `lights.cpp:491`), a `std::filesystem::path` built from three parts, `PluginRack::lock` twice, a path string per plug-in in use plus a scan of 16 tracks and 25 chains for each (`plugin_used_by`, `model/project.h`), `UpdateLink`'s mutex twice (`lights.cpp:86-87`), `takes_folder()` building a string (`audio_jobs.cpp:40`), a scan of 128 patterns per audio track (`audio_jobs.cpp:44`), and `column_fit` over 16 tracks (`context.cpp:1739`).
- **Cost:** Each item is small (I estimate 5 to 15 µs a pass in total with a handful of plug-ins, so 0.1 to 0.3 % of a core), but it is unconditional, it is all on the real-time MIDI thread, and every mutex here is shared with a non-real-time thread (service, screen, plug-in), so each is a chance for the MIDI thread to wait behind one. Measure: per-tick timing with a steady clock around `lights.cpp:190-282`.
- **Proposal:** Two tiers inside `Shell::lights()`. Keep every pass: `card_follow`, `update_tick`'s atomics, `chop_tick`, `reload_tick`, `live_erase_tick`, `chain_tick`, `follow_tick`, `capture_tick`, `seq_follow`, the queue drains (`take_done_pack`, `free_retired`), `looper_tick`, `encoder_tick`, `tap_tick`. Move to a 50 ms tier (`if (now - slow_at >= 50)`): `autosave_tick`, `usage_tick`, the library-generation block, `files_landed`, `record_tick` (its debounce timers are 500 ms and 1 s), `browser_tick`, `plugins_tick` (it already limits its own panel refresh to 50 ms at `plugin_jobs.cpp:300`), `print_tick`, `mpc3_tick`, `stems_tick`, `stage_tick`, `free_tick`, the store checks, `lesson_tick`. Where a tick only compares against another component's state, compare a generation counter first (as `library_->generation()` at `lights.cpp:196` and `plugins->changes()` already do) instead of copying a string.
- **Gain:** CPU Low (0.1 to 0.3 % of a core), latency Low (fewer cross-thread locks on the MIDI thread).
- **Sound or behaviour change:** none audible; a landed file, a finished print or a new plug-in panel value shows up to 50 ms later.
- **Risk:** Low to Medium: `record_tick` also lets go of held record keys and sends the input route; check nothing in the slow tier is on a pad-to-sound path.
- **Effort:** M.
- **Confidence:** Medium: I read each tick's entry but not every branch; the cost is an estimate.

### SHELL-8: The Samples browser's file list and the Project list are deep-copied every frame under the shell lock

- **Verification:** Merged. Same issue as IO-12 (plus the Project list, copied the same way at `shell.cpp:847`).
- **In the fork:** See IO-12.

### SHELL-9: `MelodeeApp::service()` re-derives the same facts on every 5 ms pass; the mount table is parsed about 3 times a second from six places

- **Verification:** Merged. Same issue as IO-9 for the app side and BASE-6 for the loop itself.
- **In the fork:** See IO-9.

### SHELL-10: The sample library re-lists the browser's folder every 3 s for ever, reading every MPC program file in it

- **Verification:** Merged. Same issue as IO-2.
- **In the fork:** See IO-2.

### SHELL-11: During a project load the MIDI thread reads up to 15 sample files per pass from the card while holding the shell lock

- **Verification:** Merged. Same issue as IO-6.
- **In the fork:** See IO-6.

### SHELL-12: Helper threads that poll on a timer for ever, started whether or not their feature is in use

- **Verification:** Merged. Its three parts are SMP-1 (stream pager), IO-7 (audio tracks) and IO-10 (web server and mDNS).
- **In the fork:** See SMP-1.

### SHELL-13: Settings are rewritten in full, unconditionally, once per step, and not atomically

- **Verification:** Confirmed. `change_setting` calls `store->save_settings()` on every step (`context.cpp:1030-1034`), and `write_settings` truncates and rewrites the file in place with no temporary file (`project_store.cpp:510-518`).
- **In the fork:** No change.

- **Where:** `src/melodee/shell/context.cpp:1030-1034` (`change_setting`), `:1051-1056` (`set_look`), `:2117-2121` (`set_click`), `:1192`; `src/melodee/shell/project_store.cpp:443-446` and `:510-518` (`write_settings`)
- **Thread and frequency:** MIDI thread queues, service thread writes: once per −/+ press on a Settings row, once per wheel detent on the Palette and Font cards, once per metronome toggle and per starred sound. K1 is already debounced to one write a second after it rests (`context.cpp:1048`, `sample_record.cpp:282-285`).
- **Now:**
  ```cpp
  // context.cpp:1051-1055
  void Context::set_look(unsigned row, unsigned value) {
      if (row == Settings::Palette) ui.settings.palette = uint8_t(value % ui::palettes.size());
      else if (row == Settings::Font) ui.settings.font = uint8_t(value % ui::face_choices);
      else return;
      if (store) store->save_settings(ui.settings.text());
  // project_store.cpp:512-515
  std::ofstream file(fs::path(root).parent_path() / "settings", std::ios::trunc);
  if (!start.empty()) file << "start=" << start << '\n';
  if (!last.empty()) file << "last=" << last << '\n';
  file << settings;
  ```
  The handler does not compare the new text with the old (`:443-445`), the file is truncated in place rather than written beside and renamed as projects are (`:393-402`), and each write is followed by the full folder scan of SHELL-4.
- **Cost:** Scrolling through nine palettes is nine card writes and nine project-folder scans in a second or two. Each write is about 1 to 2 KB; the scans are the expensive part. A power cut during the truncate-and-write leaves an empty settings file, which loses the sample rate, the resume project and every other setting (a robustness bug rather than a power one). Measure: count `write_settings` calls while turning the wheel on the Palette cards.
- **Proposal:** Route every settings change through the existing debounce: replace the direct `store->save_settings(...)` calls with `settings_save_at = std::max<uint32_t>(now, 1) + 1000;` (and flush at once from `save_project_to` and the Exit sheet so nothing is lost on leaving). In `ProjectStore::run`, skip the write when `r.path == settings`; write to `settings.new` and rename; do not `scan()` after a `Settings` or `Usage` request.
- **Gain:** battery Low, card wear Low, CPU Low to Medium (the scans avoided).
- **Sound or behaviour change:** none (the file lands up to a second later).
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** High.

## Lights, MIDI and controls

### LIGHT-1: The `status` mirror in `Shell::lights()` is rebuilt every 5 ms, with a walk over all 8192 sample slots

- **Verification:** Confirmed. `for (const auto& ref : project_->samples) used += ref.used();` walks all 8192 slots on every `lights()` call (`lights.cpp:485-486`); `SlotTable::span()` exists for exactly this (`slot_table.h:66-68`).
- **In the fork:** Implemented. Uses `span()`.

- **Where:** `src/melodee/shell/lights.cpp:371-505` (`Shell::lights`), worst line `:484`; `src/melodee/model/slot_table.h:46,67-68`.
- **Thread and frequency:** MIDI every 5 ms (200 Hz), stopped or playing.
- **Now:** after the housekeeping, `lights()` republishes about 45 atomics and four strings. Their only reader is `Shell::summary()` and its helpers in `shell.cpp` (I grepped every one), which runs when a remote `status` command arrives (`melodee_app.cpp:619`). `tempo_now` is the exception: the light show reads it (`lights.cpp:153`). Inside the block:
  ```cpp
  483            unsigned used = 0;
  484            for (const auto& ref : project_->samples) used += ref.used();
  491            std::string folder = library_ ? library_->folder() : std::string();
  492            std::lock_guard<std::mutex> guard(text_lock);
  495            folder_text = folder.empty() ? folder : folder.substr(folder.find_last_of('/') + 1);
  497            undo_text = last ? last->what : std::string();
  ```
  `SlotTable::end()` is `sample_slots` (8192): "Every slot in turn, the empty ones too" (`slot_table.h:46,67`). The header says `span()` is "For what runs every tick or frame" (`:68`); line 198 of the same function uses it, line 484 does not. The block also takes `text_lock` twice (`:383`, `:492`) and the sample library's mutex once (`folder()`, `sample_library.cpp:256`), builds temporary strings (heap when longer than 15 characters), and on the Roll page copies the whole `UiState` (`UiState roll = ui_;`, `:408`, about 8 KB).
- **Cost:** 1.64 million slot checks a second. About 8 cycles a slot gives 20-40 µs a tick for the walk alone, 0.4-0.8 % of one core at all times; the rest of the block perhaps 2-5 µs. Unmeasured. To measure: two `Clock::now()` calls round `app.lights()` in `run_midi` summed into a `Session` counter, or `perf stat -t <MIDI tid>` before and after.
- **Proposal:** (a) the one-line fix: walk `span()`, or keep the count where slots are written. (b) Move lines 372-503 into a `publish_status()` that runs at 10 Hz, or on demand: `summary()` sets an atomic flag and the next tick publishes. Keep `tempo_now.store` (`:387`) and `close_gesture()` (`:504`) on every tick. Assign each string only when it differs.
- **Gain:** CPU and battery, Medium (paid while stopped too).
- **Sound or behaviour change:** none. With the 10 Hz variant `status` can be up to 100 ms old.
- **Risk:** Low. Remote test scripts that press and then read `status` at once may need the on-demand variant; I did not check them.
- **Effort:** S for (a), M for (b).
- **Confidence:** High for what the code does; the cost is an estimate.

### LIGHT-2: Other work `lights()` repeats every tick though nothing changed

- **Verification:** Partly confirmed. `plugins_tick` runs every pass as described. I did not read the pad-light and 16 Levels sites in the list.
- **In the fork:** No change.

- **Where:** `src/melodee/shell/plugin_jobs.cpp:287-291`; `src/melodee/plugin/plugin_rack.cpp:754-758,446-462`; `src/melodee/plugin/plugin_files.h:67-70`; `src/melodee/shell/pad_lights.cpp:60-64,97-110,239-272,767-768,842`; `src/melodee/shell/lights.cpp:574-575`; `src/melodee/shell/roll.cpp:161-166`.
- **Thread and frequency:** MIDI every 5 ms.
- **Now:**
  1. `plugins_tick()` calls `plugins->set_card(library->card())`. `card()` locks the library and copies a string (`sample_capture.cpp:17`). `set_card` builds the plug-in folder, three `std::filesystem::path` objects and a string, before it checks whether the card changed:
     ```cpp
     754 void PluginRack::set_card(const std::string& top) {
     755     std::string wanted_root = plugin_folder(top);
     756     {
     757         std::lock_guard<std::mutex> held(lock);
     758         if (top == card_top) return;
     ```
     Then `sync()` locks the rack again and, for each of 32 entries, builds a `std::string want` from the project's path and scans the 16 tracks and 25 chains (`plugin_rack.cpp:452-462`, `model/project.h:440-446`).
  2. `pad_looks()` starts with `auto keys = keys_in(shown(s));` (`pad_lights.cpp:768`), a walk over every event of the pattern in view. Only the Erase, Copy, Roll and Time Correct looks read `keys` (`:808-811`, `:819-820`).
  3. While playing with a synth, plug-in or MIDI track selected, `heard_now()` walks all the pattern's events each tick (`:103-109`). Under 16 Levels, `levels_pattern()` walks them once per pad, sixteen walks a tick (`:113`, `:265-271`).
  4. `lights.cpp:574-575` runs a full `page_bindings()` to get one bool (`erase_pots`). On Roll with notes marked that calls `roll_marked_events()`, which allocates a `std::vector` and compares every event with every mark, twice a tick (`bindings.cpp:1078,1088`).
  5. With Settings' Pad lights Off the whole pad pass still runs and each result is thrown away at the end (`pad_lights.cpp:842`).
- **Cost:** item 1 is some 10-20 allocations and three mutex round trips a tick, about 2-5 µs. Item 2 is about 3 µs a tick for 1000 events. Item 3 under 16 Levels with 1000 events is 16,000 event tests a tick, 50-100 µs, 1-2 % of a core while playing. All unmeasured.
- **Proposal:** 1: compare `top == card_top` first; run `sync()` only when the project's plug-in table, a track's engine, a chain's kinds or the asleep mask changed (a counter bumped where they are edited), or at 10 Hz. 2: fill `keys` lazily, in the contexts that read it. 3: one pass over the events that fills all sixteen pads (collect each pad's key, pitch, velocity and cutoff first). 4: cache `erase_pots` against page, track and Shift; let `roll_marked_events` fill a fixed array. 5: return early from `pad_looks()` when Pad lights is Off (after the lesson branch at `:777-780`, which lights pads regardless).
- **Gain:** CPU Low to Medium (Medium for 16 Levels playback); battery Low.
- **Sound or behaviour change:** none.
- **Risk:** Low.
- **Effort:** S each; M for the plug-in change counter.
- **Confidence:** High for the code; costs are estimates.

### LIGHT-3: The tick is a fixed 200 Hz; nothing slows down when nothing animates

- **Verification:** Confirmed. Same lines as BASE-7; `Shell::lights` has no early return for an unchanged state.
- **In the fork:** Not implemented. `Shell::lights()` also runs about twenty housekeeping ticks; each has to be checked for an assumption about the 5 ms period before the tick slows.

- **Where:** `base/src/runtime/controller.cpp:123,152-156,160-161` (`Controller::run_midi`); `src/melodee/shell/lights.cpp:179-363`.
- **Thread and frequency:** MIDI every 5 ms.
- **Now:**
  ```cpp
  152            if (now >= lights_due) {
  153                app.lights(lights, now);
  154                lights.update(send_surface, 48);
  155                lights_due = now + std::chrono::milliseconds(5);
  156            }
  ```
  `Shell::lights()` is the app's whole housekeeping tick as well as its lights (`lights.cpp:190-193`, `:227-282`, `:353-363`). What each animation needs:
  - Continuous: a held pad's white, a sound's flare and its fade (time constants 60-250 ms, `settings.h:77`), the count-in pulse, track meters on Tracks and in mute mode, the Resample pulse, the Looper's clear fill. 60-100 Hz is enough; a 10 ms step is far above what an eye sees as a step.
  - Edges: blinks of 150, 200 and 250 ms, the Tap Tempo beat flash, the step playhead. An edge 10 ms late is not visible.
  - Meters: 20 Hz already.
  - Housekeeping: live erase looks 40 ms ahead by design ("before the next frame looks again (40 ms)", `erase_copy.cpp:223-225`); chains act in a pattern's last bar; button holds are 400-500 ms.
- **Cost:** the whole pass (wake, `lights()`, syscalls) is probably 40-100 µs, so 1-2 % of one core all the time, and 200 wakeups a second. Unmeasured.
- **Proposal:** let the app choose the period (base change: `lights()` returns a duration, or a new `lights_period()`), and have the base bring `lights_due` forward when surface input was handled in the pass, so a press still lights at once. The app returns 10 ms while anything moves (playing, voices in the taps, glow above zero, a pad or button held, a blink state, count-in, loading, a take or recorder not idle, the light show, a Chop audition pending) and 33-50 ms otherwise. A first step with no predicate: a constant 10 ms.
- **Gain:** battery and CPU, Medium. It also halves LED traffic (LIGHT-8).
- **Sound or behaviour change:** visible in principle (fades in 10 ms steps); none expected to be noticed.
- **Risk:** Low for a constant 10 ms. Medium for the adaptive rate: a missed "active" case makes something sluggish. The light show's history (48 entries, one a tick) still covers its 70 ms look-back.
- **Effort:** M (sample-base, then move the submodule).
- **Confidence:** Medium. I did not read every housekeeping tick in full (see Coverage).

### LIGHT-4: LEDs have no global brightness and no idle dim

- **Verification:** Confirmed. Pad levels are fixed constants (`pad_light.h:23`), and `SurfaceLights::rgb` stores what it is given with only a clamp to 127 (`leds.h:147-151`). No setting scales them.
- **In the fork:** Implemented. New setting PADS > Pad brightness (100, 75, 50, 25 %), and the lights go low with Dim after. `SurfaceLights` gained `scale()` and `rest()`.

- **Where:** `src/melodee/shell/pad_light.h:23`; `src/melodee/shell/pad_lights.cpp:164-171,179-182`; `src/melodee/shell/lights.cpp:529,543,549-553,585`; `src/melodee/shell/button_jobs.cpp:30-39`; `src/melodee/shell/settings.h:75-80`; `base/src/surface/leds.h:147-151`; `base/src/surface/led_colours.h:13-17`.
- **Thread and frequency:** MIDI sets the state; the LEDs draw current all the time.
- **Now:** five rungs, squared before the drive:
  ```cpp
  23 constexpr float off = 0, faint = 0.25f, dim = 0.4f, mid = 0.65f, full = 1.0f;
  ```
  so Faint is 6 %, Dim 16 %, Mid 42 % and Full 100 % of full drive. At rest (stopped, nothing held):
  - Kit track: each used pad Dim in its colour, the last pad hit Full, empty pads off (`kit_base`).
  - Synth, plug-in or MIDI track on keys: all sixteen pads lit. Roots in the track colour at Mid, notes in the key white at Dim (20,20,20), notes out of it white at Faint (8,8,8) (`note_look`).
  - Tracks page: each used track between Dim and Mid, the selected one Full.
  - Buttons: `on()` gives Dim to every button with a job (`lights.cpp:585`) and `button_has_job` is true by default (`button_jobs.cpp:38`), so about sixteen buttons are Dim and one or two Bright.
  - One bank light of eight (they are on or off only, `leds.h:15-16`). The fader light follows its value. Meters are dark in silence.
  Settings has Pad lights (Track colours, White, Off), Pad fade and Tap Tempo light. Nothing scales brightness and nothing dims after a time without input. By a rough sum of drives, resting pads draw 10-15 % of what sixteen full-white pads would.
- **Cost:** LED current, unknown per LED. Measure on a bench supply or USB-C meter: Pad lights Off against Track colours against the light show (the maximum).
- **Proposal:**
  1. Brightness. The one place every RGB light passes is `SurfaceLights::rgb()` (`leds.h:147`): pads, fader and light show. A `set_brightness(scale)` applied there keeps the dedupe exact, because the stored and sent values are both scaled. Without a base change, scale at `lights.cpp:529` and `:543` and in `show_light_frame` (`light_show.h:379-380`). `Look::level` and the rungs that `status` and the tests read are taken before the store, so they do not move. Drive is linear and the eye is not (`led_colours.h:7-9`): offer the setting as a perceived level b and multiply the drive by b squared; "70 %" then halves pad LED power. Below b squared of about 0.35 a Faint colour (drive 8) loses its hue (`pad_light.h:17-20`), so stop the setting near 60 %, or raise `faint` with it, and never round a lit channel to zero.
  2. Idle dim. `last_input_` already holds the time of the last control or note (`shell.h:177`, set at `shell.cpp:96,130`). Stopped, no voices, and idle for 30 s: pads to a quarter of their drive and Dim buttons off but Play. Idle for some minutes: all off but one. Any surface event sets `last_input_`, so the lights come back at once.
  3. Button, bank and meter LEDs cannot be scaled; only the idle rule reaches them.
- **Gain:** battery, Medium and possibly High; unmeasured.
- **Sound or behaviour change:** visible (a setting, and the idle dim).
- **Risk:** Low.
- **Effort:** S for brightness and its Settings row, M for the idle dim.
- **Confidence:** High for the code. The power saved is unknown until the current is measured.

### LIGHT-5: The MIDI thread busy-waits while a SysEx goes out and any MIDI event is pending

- **Verification:** Confirmed. `MidiIo::pump` skips delivery while `sysex.active` but still calls `out.wake_at(pending[0].due)` with a time already past (`midi_io.cpp:616, 630`), so the MIDI thread's wait returns at once and the loop spins.
- **In the fork:** Implemented. While SysEx goes out the thread is woken a millisecond on, not at a time already past.

- **Where:** `src/melodee/shell/midi_io.cpp:616,630` (`MidiIo::pump`); `base/src/runtime/controller.cpp:160-161`; `base/src/drivers/hardware_midi.cpp:118-119`.
- **Thread and frequency:** MIDI (`SCHED_FIFO` 15), for as long as a SysEx is being sent while clock or notes go out.
- **Now:**
  ```cpp
  616    while (!sysex.active && sent < pending_count && pending[sent].due <= now_ns + 200000) {
  630    if (pending_count) out.wake_at(Clock::time_point(std::chrono::nanoseconds(pending[0].due)));
  ```
  While `sysex.active` nothing pending is delivered, but the wake for `pending[0]` is still asked for. Once that time has passed, `until` in `run_midi` lies in the past and `HardwareMidi::wait` returns at once (`if (timeout_ns <= 0) return;`). The loop then spins, each turn reading every port, taking `Shell::lock` and running `pump`, until the SysEx ends. SysEx is paced at 320 µs a byte on the jacks and 20 µs on USB (`midi_io.cpp:591`): a 4 KB dump on the jacks takes 1.3 s, 64 KB takes 20 s.
- **Cost:** one core at 100 % at real-time priority for that time, and `Shell::lock` taken tens of thousands of times a second against the screen thread. With `MELODEE_RENDER_THREADS=pinned` (every other thread on core 0, `melodee_app.cpp:101-119`) it would starve the screen and service threads. It needs a MIDI track's SysEx sent while any MIDI goes out (Sync Out on, MIDI-track notes, pads sending notes); the code expects that case ("SysEx going out: notes wait", `:578`).
- **Proposal:** `if (pending_count && !sysex.active) out.wake_at(...)`. Line 600 already asks for the wake at `sysex.next_ns`, and the pass that ends the SysEx delivers what waited.
- **Gain:** CPU and battery: Low on average because it is rare, High while it happens.
- **Sound or behaviour change:** none.
- **Risk:** Low.
- **Effort:** S (one condition).
- **Confidence:** High from the code; not run.

### LIGHT-6: Playing gestures and sync messages mark the project changed, so the background save writes the card

- **Verification:** Confirmed. Same mechanism as SHELL-1: `Context::send` exempts only 17 command types (`context.cpp:55-60`). `ExtSync`, `RepeatKey`, `ArpKey` and the punch commands are not among them.
- **In the fork:** Implemented in part. Thirteen command types that only mirror state or carry a gesture no longer mark the project changed: `MidiAudio`, `Monitor`, `LoopMonitor`, `InputTaken`, `RecordGain`, `RecordSource`, `KeyedRoot`, `ExtSync`, `MtcIn`, `RepeatKey`, `ArpKey`, `PunchOn`, `PunchOff`. `PunchDepth`, `MidiSync`, `MidiTrack` and `FlexBeat` are left as they were, because for each I found a path where the command is the only thing that marks a real edit. Review before the commit: with `PunchOn` and `PunchOff` exempt, nothing marked a change of the Pad FX bank or target as unsaved (in the original the next punch's command did, by accident). `set_punch_target()` and `step_punch_bank()` now mark the project themselves.

- **Where:** `src/melodee/shell/context.cpp:53-62` (`Context::send`). Senders: `midi_io.cpp:158,165,187` (one `ExtSync` per MIDI clock pulse), `:399-405,421` (`MidiSync`, `MidiTrack`), `context.cpp:354-360` (`RepeatKey`, `ArpKey`), `pad_fx.cpp:25-39,133-135,256` (`PunchOn`, `PunchOff`, `PunchDepth`, `FlexBeat`), `lights.cpp:225` (`KeyedRoot`), `sample_record.cpp` `record_tick` (`Monitor`, `RecordGain`). Consumer: `lights.cpp:64-82` (`autosave_tick`), `autosave.h:27-31`, `context.cpp:72-88`.
- **Thread and frequency:** MIDI, per message. The result is one project save 3 s after the hands leave the controls.
- **Now:**
  ```cpp
  56    switch (command.type) {
  57    case Type::NoteOn: case Type::NoteOff: case Type::AllNotesOff: case Type::Wheel: case Type::Bend: case Type::Pressure:
  58    case Type::Control: case Type::Audition: case Type::PadNote: case Type::Play: case Type::Stop: case Type::Reload:
  59    case Type::RecordArm: case Type::RecordNow: case Type::RecordStop: case Type::RecordAutoEnd: case Type::Locate: case Type::Recorded:
  60    case Type::Click: break;
  61    default: dirty = true;
  62    }
  ```
  Everything not listed sets `dirty`. So following MIDI clock, MTC in, playing pads with Note Repeat or the arp on, holding a Pad FX punch or pressing it harder, a Flex Beat pad, a MIDI Config change and the input monitor all mark the project changed though nothing in it changed. `autosave_due()` then saves the whole project once it is stopped and idle for 3 s.
- **Cost:** each needless save runs `save_project_to()` on the MIDI thread under `Shell::lock` (it reads each plug-in's state and serialises the project and every pattern), then the service thread writes the file to the card. Size and time unmeasured. It follows every externally clocked run and every Note Repeat phrase.
- **Proposal:** add the transient types to the exempt list (`ExtSync`, `MtcIn`, `MidiSync`, `MidiTrack`, `ArpKey`, `RepeatKey`, `PunchOn`, `PunchOff`, `PunchDepth`, `FlexBeat` index 0, `Monitor`, `LoopMonitor`, `RecordGain`, `RecordSource`, `KeyedRoot`). Cleaner: `send()` leaves `dirty` alone and each real edit sets it, as many already do (`pad_fx.cpp:148,270`).
- **Gain:** battery Low; card writes Medium; it also removes a stall of the MIDI thread after each such gesture.
- **Sound or behaviour change:** none in sound. The project no longer shows as unsaved after merely playing.
- **Risk:** Low for the list. Medium for the inversion, which needs every real edit checked.
- **Effort:** S for the list, M for the inversion.
- **Confidence:** High for the mechanism. I did not check whether start-up sends leave a freshly opened project dirty (the order of the settings load and `switch_to()`, which clears the flag at `context.cpp:179`).

### LIGHT-7: Pads, MIDI in and clock out wait on `Shell::lock` while the screen thread copies the project

- **Verification:** Merged. Same root cause as MODEL-1 (the screen thread's hold of `Shell::lock`).
- **In the fork:** See MODEL-1.

### LIGHT-8: LED traffic: one SysEx for every one-step colour change, one `write()` for every message

- **Verification:** Confirmed. `SurfaceLights::update` sends one message per light whose stored value differs from the last sent (`leds.h:125-143`); nothing quantises a fade's steps.
- **In the fork:** No change.

- **Where:** `base/src/surface/leds.h:125-143` (`SurfaceLights::update`); `src/melodee/shell/pad_light.h:54-71`; `src/melodee/shell/pad_lights.cpp:112-135,329-330,799-801`; `base/src/drivers/hardware_midi.cpp:181-196` (`flush`); `base/src/runtime/controller.cpp:154`.
- **Thread and frequency:** MIDI every tick while anything fades.
- **Now:** `update()` sends whenever the stored RGB differs from the one last sent. Fades are continuous floats rounded per channel (`mix_light`, `led_colours.h:35-39`), so through most of a fade every tick differs. `flush()` writes packet by packet:
  ```cpp
  183        while (port->output && port->head != port->tail) {
  184            const auto& packet = port->pending[port->tail];
  185            unsigned remaining = packet.size - port->offset;
  186            auto size = snd_rawmidi_write(port->output, packet.bytes.data() + port->offset, remaining);
  ```
  Estimated rates, from the code:
  - Stopped on a static page: 0 messages a second.
  - One pad hit and release at the default 100 ms fade: 1 + about 50-70 SysEx over 400 ms.
  - Playing a drum pattern with the kit in view (6-10 hits a second on the bank): 300-700 SysEx a second, plus 2 CC a beat for Tap Tempo and at most 40 CC a second for the meters.
  - Tracks page or mute mode while playing: each sounding track's pad follows its meter, 40-90 changes a second a pad, up to about 1,400 SysEx a second.
  - Recording adds the Seq Record blink (4 CC a second while armed or counting in) and the count-in pulse on all sixteen pads.
  - Light show: all 17 RGB lights every tick, about 3,400 a second, by choice.
  The link is not the limit (4 Mbaud is about 400 kB a second). The cost is a syscall and a 272-byte `MidiPacket` built and copied for each 12-byte message, and work for the surface's microcontroller.
- **Proposal:** (1) the 10 ms tick of LIGHT-3 halves it. (2) Hysteresis for RGB in `update()`: skip a change smaller than 2-3 steps in every channel unless the wanted value has stood still for one pass, so the final colour is always exact; use one step below a drive of 16. (3) In `flush()`, gather the whole queued packets of a port into one buffer and write once a pass. (4) Count messages: `update()` returns the number sent and `controller.cpp:154` drops it; add it to `Session` for `status`.
- **Gain:** CPU Low (perhaps 0.1-0.5 % of a core while playing).
- **Sound or behaviour change:** visible in principle; meant not to be. Check low levels by eye.
- **Risk:** Low to Medium (the hysteresis at very low drive).
- **Effort:** S to M, in sample-base.
- **Confidence:** Medium; the rates are estimates.

### LIGHT-9: Pad pressure is not coalesced; every message becomes a timed engine command

- **Verification:** Confirmed. `Context::pressure` sends one `Pressure` command per held key for every pressure message (`context.cpp:1195-1201`).
- **In the fork:** No change.

- **Where:** `src/melodee/shell/shell.cpp:169`; `src/melodee/shell/context.cpp:1195-1201` (`Context::pressure`), `:63-67`; `src/melodee/shell/pad_fx.cpp:118-137`; `src/melodee/engine/engine_host.cpp:256-279,2738-2740`.
- **Thread and frequency:** MIDI per pressure message (the brief gives about 1000 in a short hold); audio per block.
- **Now:**
  ```cpp
  1195 void Context::pressure(unsigned pad, uint8_t value) {
  1196     if (pad >= 16 || !ui.settings.pressure) return; // Settings: Pad pressure off
  1197     if (punch_pressure(pad, value)) return;         // Pad FX: a held punch's depth
  1199     unsigned to = pad_track[pad] % track_count;
  1200     for (auto k : pad_keys[pad]) if (k >= 0) send({Type::Pressure, uint8_t(to), 0, uint16_t(k & 0xff), int16_t(value)});
  ```
  `Pressure` is sent with `send_at` (`context.cpp:65-66`), so each command carries a frame, and the render loop ends its run at the next timed command (`engine_host.cpp:264`, `:2739-2740`). There is no check for an unchanged value, no minimum spacing, and one command for each note of a chord. On the MIDI side each message is a full pass of the loop: the `ppoll` return, a read on every port, `midi_output()` with its lock and `pump`, and a flush.
- **Cost:** MIDI side perhaps 15-20 µs a message, 1.5-2 % of a core at 1000 messages a second (unmeasured). Audio side: up to about three more run boundaries in a 128-frame block; what a boundary costs with one-dispatch blocks on (`engine_host.h:487`) is for the engine audit.
- **Proposal:** thin on the MIDI thread. For each pad keep the value last sent and when; pass a new value on only if it differs and a block's time (about 3 ms) has passed for that pad; send the newest waiting value from the tick so the last one always lands. The engine already thins MIDI-track slot values at 2 ms (`midi_thinned`, `engine_host.cpp:2663`). Thinned pressure can go untimed. The same for `PunchDepth`.
- **Gain:** CPU Low to Medium while pads are held; audio headroom, depending on the engine.
- **Sound or behaviour change:** not bit-exact: pressure reaches the engine in steps of up to 3 ms instead of about 1 ms. Audible only if the engine does not smooth pressure, which I did not check; it needs a listen.
- **Risk:** Medium (the feel of pressure).
- **Effort:** S to M.
- **Confidence:** Medium.

### LIGHT-10: MIDI ports are rescanned through ALSA every second on the MIDI thread

- **Verification:** Merged. Same issue as BASE-14.
- **In the fork:** See BASE-14.

### LIGHT-11: The idle MIDI loop reads every port and allocates a `std::function` on each pass

- **Verification:** Confirmed. `HardwareMidi::read` tries every input port on every pass, and `wait` rebuilds its `pollfd` list each time (`hardware_midi.cpp:118-139`).
- **In the fork:** No change.

- **Where:** `base/src/runtime/controller.cpp:125-127,145-161`; `base/src/drivers/hardware_midi.cpp:118-158`; `base/src/drivers/hardware_midi.h:26`; `src/melodee/shell/midi_io.cpp:543-570`.
- **Thread and frequency:** MIDI, every pass: at least 200 a second, plus one for each incoming message and each timed output.
- **Now:** `wait()` polls the inputs and throws away which were ready; `read()` then calls `snd_rawmidi_tread` or `snd_rawmidi_read` on every open input:
  ```cpp
  132    for (auto& port : ports) {
  133        if (!port->input) continue;
  135        for (unsigned batch = 0; batch < 8; ++batch) {
  137            auto size = port->stamped ? snd_rawmidi_tread(port->input, &stamp, bytes, sizeof(bytes))
  138                                      : snd_rawmidi_read(port->input, bytes, sizeof(bytes));
  139            if (size == -EAGAIN || size == 0) break;
  ```
  `read` takes `const std::function&`. The lambda at `controller.cpp:125` captures `this`, `session` and `syncing_until`, 24 bytes against libstdc++'s 16-byte inline store, so each pass allocates and frees once. `MidiIo::pump` works out the output list again on every pass with string compares and an `snprintf` for each USB device (`midi_io.cpp:557-570`), and `configure()` compares the config and reads sixteen tracks' channels.
- **Cost:** with the surface, the jacks and the USB-C gadget open: three reads and a `ppoll`, about 800 syscalls a second when idle, perhaps 1-1.5 ms a second. Unmeasured.
- **Proposal:** in the base, have `wait()` keep `POLLIN` per port and `read()` skip the rest; make `read` a template, as `SurfaceLights::update` is. In the app, redo the output list only when the ports changed (the base counts that: `generation()`, `controller.cpp:173`).
- **Gain:** CPU Low.
- **Sound or behaviour change:** none.
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** High for the code. The closure's size is by the language rules, not read from a binary.

## Storage, loading and background jobs

### IO-1: Every store request re-lists the project folder and fully parses every project in it

- **Verification:** Confirmed. `ProjectStore::run` ends with `scan()` for every request that does not return early, and `scan()` calls `load_project` on every project file in the folder (`project_store.cpp:499-500, 521-542`).
- **In the fork:** Implemented. `ProjectStore::scan` keeps each project's track count and tempo by path, modification time and size; a saved file is always read again.

- **Where:** `src/melodee/shell/project_store.cpp:499-500` (`ProjectStore::run`), `:521-548` (`ProjectStore::scan`), `:267-272` (`ProjectStore::service`); callee `src/melodee/model/project_file.cpp:802-803`, `:1518-1519` (`load_project`).
- **Thread and frequency:** service thread. After every request except Load and ExportMpc3: each Save, so each autosave; each settings write (1 s after any settings or Input Config change); Opened; OpenAtStart; Rename; Move; Duplicate; Remove; NewFolder; SaveSound; each usage-log append (every 10 s while that log is on). Also once at start, before the project to resume is read.
- **Now:**
  ```cpp
  // project_store.cpp:499-500, where run() ends for every kind that did not return early
      if (error) publish_message(error.message());
      scan();
  // project_store.cpp:540-542, in scan(), for every .mldp of the folder in view
                  auto project = std::make_unique<Project>();
                  auto patterns = std::make_unique<PatternBank>();
                  if (load_project(read_file(e.path), *project, *patterns).empty()) { e.tracks = project->used_tracks(); e.tempo = project->tempo; }
  ```
  `load_project` makes a second `Project` and `PatternBank` (`project_file.cpp:802-803`), parses into them and ends with `target = *project; target_patterns = *patterns;` (`:1518-1519`). `read_file` fills its vector through `istreambuf_iterator`, a byte at a time (`project_store.cpp:65-68`). `scan()` then walks every folder under the root four deep for Move (`:574-586`).
- **Cost:** for each project file about 2 MB of construction (two `Project` at 539 KB, two `PatternBank` at 480 KB with 2048 `Pattern` constructors each), a 1 MB deep copy, 4096 `Pattern` destructors, a byte-wise read and a byte-wise parse. Estimate, unmeasured: 1.5-3 ms a small project, tens of ms for one carrying plug-in state (PLUG is read with `state.push_back` a byte, `project_file.cpp:1310`). A folder of 40 projects: 60-120 ms or more of service-thread time after every autosave and every settings change, during which the 5 ms service loop (take drain, sample hand-over, render drain, card and power polling) does not run. The same sum delays the first project at start. Measure: `steady_clock` around `scan()` printed to the journal, with a real Projects folder; `perf record -g -p <pid>` during an autosave.
- **Proposal:**
  1. Do not call `scan()` for `Usage`, `Settings`, `Opened` and `OpenAtStart`: none changes a listing (`OpenAtStart` only needs `known.start` set under the lock). About 10 lines.
  2. Keep a map on the service thread, path to (size, mtime, tracks, tempo); `scan()` parses a file only when its size or mtime changed.
  3. Replace the full parse with a peek: walk the section tags with the existing `Reader`, read PROJ (tempo) and the engine byte of each TRAK, and count MIDT, PLGT, KGRP and AUDT as used tracks. No `Project` is built.
  4. At start, queue the resume project (`:269-272`) before `scan_sounds()` and `scan()` (`:267-268`).
  5. On the load path proper: give `Reader` a bulk `bytes(n)` for PLUG's state, and let `load_project` move or swap into its targets instead of copying.
- **Gain:** CPU and battery Medium (after every edit burst); start and load time Medium with many projects; service-loop latency Medium.
- **Sound or behaviour change:** none.
- **Risk:** Low. The list shows the same rows; a cache keyed by size and mtime misses a file rewritten in the same second at the same size, which a save always re-keys anyway.
- **Effort:** S for 1 and 4, M for 2 or 3.
- **Confidence:** High on the path (read every line of `run` and `scan`). The per-project time is an estimate. I did not read every section of `load_project`, so the peek's list of track-carrying sections should be checked against lines 821-1300.

### IO-2: The sample browser's folder is listed again every 3 s, with a stat per file and every MPC program file read

- **Verification:** Confirmed. `SampleLibrary::service` runs `scan()` every 600 passes regardless of the page shown (`sample_library.cpp:474-488`); `list_into` calls `is_drum_program` for every instrument file.
- **In the fork:** Implemented. The folder is re-listed every 3 s only while something reads the listing; a card change, a file Melodee wrote, a browse or the page opening still lists it at once.

- **Where:** `src/melodee/shell/sample_library.cpp:474-488` (`SampleLibrary::service`), `:411-437` (`list_into`), `:493-543` (`scan`); `src/melodee/shell/mpc_program.cpp:155-165` (`is_drum_program`).
- **Thread and frequency:** service thread, every 600 passes (3 s), for the whole session, whatever page is shown, transport stopped or not. Also on each browse and after each file the library writes.
- **Now:**
  ```cpp
  // sample_library.cpp:474, 486-487
      if (++ticks % 600 == 1 || scan_wanted.load() || browse_wanted) {
          scan_wanted.store(false);
          scan();
  // sample_library.cpp:432, 434, in list_into(), for each entry
          if (instrument && is_drum_program(f.path)) { f.kit = true; f.instrument = false; }
          if (!folder_here) f.bytes = it->file_size(error);
  // mpc_program.cpp:158-162
      if (!read_bytes(path, head, 16384)) return false;
      if (head.size() >= 2 && uint8_t(head[0]) == 0x1f && uint8_t(head[1]) == 0x8b) { // JSON: read whole (an .xty is a few hundred KB)
          return read_bytes(path, whole, 8u << 20) && read_program(path, whole, program).empty() && program.type == "Drum";
  ```
  The library keeps the folder the browser was last in, so the scan goes on after the Samples page is left. Each scan also reads the mount table (`:478`), builds up to 4000 `SampleFile`s with about ten temporary strings and paths each, sorts them, and only then finds the listing unchanged (`:536-538`). Opening the Samples page already asks for a fresh list (`stage_jobs.cpp:77`, `menu.cpp:41`, `new_track.cpp:151`, `shell.cpp:472`), and every writer sets `scan_wanted`.
- **Cost:** unmeasured. A plain file: one `stat` plus allocations, about 6-10 us; 500 samples is 3-5 ms every 3 s (0.1-0.2 % of a core), 4000 is 25-40 ms (about 1 %) and a stall of that length in the service loop. An XML `.xpm`: a `stat`, an open and a 16 KB read each time. A gzip program (MPC 3 `.xty` or `.xpm`): the whole file read, inflated, parsed as JSON and built into a `KeygroupProgram`, every 3 s; if one costs 100 ms (the code's own comment gives a third of a second for a 1.2 MB XML program before trimming), a folder of 20 keeps the service thread busy two thirds of the time while the device looks idle. Measure: `strace -c -f -p <pid>` for 10 s with the browser left in a large folder (count `newfstatat`, `openat`); `pidstat -t` for the service thread.
- **Proposal:** list only when something asked, and keep the 3 s look only while the Samples page is in view (so files a print engine job writes still appear):
  ```cpp
  // browser_tick() already computes `here`; hand it over: library->watch(here);  (an atomic<bool>)
  const bool timed = ++ticks % 600 == 1;
  if (timed || scan_wanted.load() || browse_wanted) {
      // root lookup as now (or from IO-9's shared card path)
      const bool wanted = scan_wanted.exchange(false);
      if (changed || wanted || browse_wanted || (timed && watched.load())) scan();
  }
  ```
  Second, remember `is_drum_program()` by (path, size, mtime) in a small map in `sample_library.cpp`, so a re-list of an unchanged folder opens no file. The browse itself gets faster too.
- **Gain:** battery Medium (Low with a small folder, High with a folder of MPC 3 programs); service-loop latency Medium.
- **Sound or behaviour change:** none while the Samples page is shown. With it closed, a file written by something that does not set `scan_wanted` appears when the page is next opened, which already re-lists.
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** High on the path. The cost of `read_mpc_json_program` on the device is not measured.

### IO-3: Every save writes every loop's WAV to the card again, changed or not

- **Verification:** Confirmed. `save_loop_files` queues every kept loop for writing on every save with no change test (`looper_files.cpp:70-86`).
- **In the fork:** Not implemented. Needs a change counter in the Looper engine, which I did not read closely enough to place safely.

- **Where:** `src/melodee/shell/looper_files.cpp:70-86` (`save_loop_files`), `:173-189` (`SampleLibrary::loop_service`); called from `src/melodee/shell/context.cpp:76` (`Context::save_project_to`).
- **Thread and frequency:** asked on the MIDI thread at every save, so at every autosave (3 s after any edit, not only a Looper one); written on the service thread, one loop a pass.
- **Now:**
  ```cpp
  // looper_files.cpp:76-77: a loop is kept when it holds audio; nothing asks whether it changed
          bool keep = lp.on && v.length && v.ticks > 0 && v.state != Looper::State::Empty && v.state != Looper::State::Armed && v.state != Looper::State::Recording;
          if (keep) { l.frames = to_model(v.length, v.rate); ...; mask |= uint8_t(1u << n); }
  // looper_files.cpp:181-186
          adopt(data, std::vector<float>(looper.held_left(n), looper.held_left(n) + frames),
                std::vector<float>(looper.held_right(n), looper.held_right(n) + frames), v.rate);
          looper.release(n);
          fs::create_directories(loop_job.dir, error);
          auto failed = write_wav(file + ".new", data); // at the loop's own rate
          if (failed.empty()) fs::rename(file + ".new", file, error);
  ```
  `write_wav` writes 24-bit stereo through `file.put()` a byte at a time (`engine/sample_data.cpp:271-274`).
- **Cost:** 6 bytes a frame: 265 KB a second of loop at 44.1 kHz, 576 KB at 96 kHz. Four 8-bar loops at 90 BPM (21 s each) are 22 MB to the card after every edit burst while the Looper is on; a loop may be 64 s (`engine/looper.h:29`), 17 MB each. That is seconds of card writing (the card's write current, and wear), two full copies of the loop in memory, about six `put()` calls a frame, and a service pass that lasts as long as the write. Measure: `/sys/block/mmcblk*/stat` sectors written across one autosave with loops held.
- **Proposal:** give each loop an edit count: an `std::atomic<uint32_t>` in the Looper that the audio thread bumps whenever it stops writing a loop's audio (a take ends, an overdub ends, an undo or redo swaps buffers, a clear, a fit swap, a load). Carry it in `Looper::View` and in the `LoopFiles` job. In `loop_service`, remember per loop the folder and the count last written (and the count after a load); when both match and the file exists, skip the hold, the copies and the write. A Save As has another folder, so it still writes.
- **Gain:** card writes and battery High while the Looper is in use (none otherwise); service-loop latency Medium.
- **Sound or behaviour change:** none.
- **Risk:** Medium. A missed bump would save a stale loop, so the count must be bumped conservatively (any block that wrote to the loop, every buffer swap). The Looper itself is engine code I only skimmed.
- **Effort:** M.
- **Confidence:** High that the rewrite is unconditional (read both functions and the caller). Not checked: whether `host.playing()` is true while only loops play, which would hold autosave off in that case.

### IO-4: A sample cannot play until its hit, beat and note analysis is done, and the analysis is repeated on every load

- **Verification:** Confirmed. The load task runs `analyse_sample` before it pushes the sample to `files_loaded` (`sample_library.cpp:1083-1088`), and nothing stores the result.
- **In the fork:** No change.

- **Where:** `src/melodee/shell/sample_library.cpp:1074-1088` (the pool task in `SampleLibrary::dispatch`), `:36-40` (`analyse_sample`).
- **Thread and frequency:** load pool (3 threads, nice 5), once per sample per load: every project open, every kit load, every card return.
- **Now:**
  ```cpp
  // sample_library.cpp:1074, 1082-1084, 1087-1088
          if (error.empty()) error = read_audio(request.path, *h.data, stream_most_seconds, nullptr); // at the file's own rate
              loaded.frames = h.data->model_frames();
              loaded.stereo = make_overview(*h.data, loaded.overview, &loaded.sides);
              analyse_sample(*h.data, loaded, request.find_note);
          std::lock_guard<std::mutex> guard(loaded_lock);
          files_loaded.push_back(std::move(h));
  ```
  `analyse_sample` sums the sides to mono at 44.1 kHz, then runs the onset finder (a 2048-point complex FFT for every 882 frames, band sums, an autocorrelation for the tempo, the Auto chop reading: `engine/onsets.cpp:47,100-102`) and, the first time a slot is loaded, the note finder (up to 48 windows, two 4096-point FFTs each, plus 8192-point chord FFTs: `engine/root_find.cpp:33-36,75-78,119`). The hits, beats and Auto reading are used only by Chop and by a take's cuts; a long streamed sample already plays first and gets them later (`:1052-1062`, `LoadedSample::shaped`).
- **Cost:** unmeasured, but the code times it: each load prints `load: ... threads: read X, analyse Y ... ms` (`sample_library.cpp:1318`), so one project load on the device gives the split. From the loop counts I expect about 10 ms of CPU a second of audio for the hits, against 5-9 ms to read a second of 16-bit stereo from the card, so analysis is plausibly half or more of the pool's time, and it sits in front of the next file's read in the queue. The file is also opened three times and `/proc/meminfo` read twice per sample (`:1033-1044`, `:1067-1069`, `:1074`); minor.
- **Proposal:** hand the sample over first. In the pool task: read, overview, `auto mono = model_mono(*h.data)` (a private copy, so nothing touches the `SampleData` once the service thread owns it), push the `Handover` with `shaped = false`, then run `find_hits_in` and `found_root` on `mono` and post the result through the existing `shapes_read` path (`take_loaded()` at `:1243-1253`, with its `if (!shown.streamed) continue;` widened). The two places that wait for a streamed sample's shape (`sample_files.cpp:280`, `stems_jobs.cpp:156`) then wait on `!shaped` alone. With IO-5's cache file, a later load skips the analysis altogether.
- **Gain:** load time Medium (time until the Loading sheet closes and pads sound); with the cache, CPU at every load Medium.
- **Sound or behaviour change:** none in sound. Chop's hits and a sample's note arrive a moment after the sample is playable, as they already do for long samples.
- **Risk:** Medium: every reader of `hits`, `beats`, `autos` right after `ready` must tolerate the gap (the streamed path shows which ones exist).
- **Effort:** M.
- **Confidence:** Medium. The ordering is certain; the share of analysis in load time needs the journal line from the device.

### IO-5: A long (streamed) sample's whole file is read from the card and analysed again after every load

- **Verification:** Confirmed. `stream_shape` opens the file and walks all of it to build the overview and hits (`sample_library.cpp:1126-1173`); there is no cache of the result.
- **In the fork:** No change.

- **Where:** `src/melodee/shell/sample_library.cpp:1057-1062` (`dispatch`), `:1126-1173` (`SampleLibrary::stream_shape`).
- **Thread and frequency:** load pool, once per streamed sample per load.
- **Now:**
  ```cpp
  // sample_library.cpp:1057-1059
                  ShapeRead shape;
                  shape.slot = request.slot; shape.serial = serial;
                  stream_shape(from, shape, request.find_note);
  // sample_library.cpp:1144, in stream_shape(): the whole file, 16384 frames at a time
      for (uint32_t got; !shapes_stopped.load(std::memory_order_relaxed) && (got = file.read(l.data(), r.data(), piece)) > 0;)
  ```
  The result (512-column overview, the two sides, a peak for each 1024 frames, hits, beats, the Auto reading, the note) is kept in memory only.
- **Cost:** a 30-minute 16-bit stereo WAV is 317 MB: 8-16 s of continuous card reading at 20-40 MB/s on every project open, in competition with the stream pager and the other loads, plus a transient mono copy of 4 bytes a 44.1 kHz frame (317 MB for 30 minutes, when memory allows) and one FFT per 882 frames over the whole length. Estimates, unmeasured. Measure: the `analyse` figure of the load line with one long sample in the project; `iostat` during it.
- **Proposal:** write the shape beside the other cache files, named as `SampleCache::name_of` names them (path, size, mtime, version), e.g. `<hash>.shape`, and read it instead of the file when present. `AudioTracks::peaks_of` already does this for takes (`audio_tracks.cpp:109-130`). The same file can hold a short sample's hits for IO-4.
- **Gain:** load time, card reads and battery High for projects with long samples; none for others.
- **Sound or behaviour change:** none.
- **Risk:** Low: the key changes when the file does; a version field covers analysis changes; the write goes through `.part` and rename as the cache does.
- **Effort:** M.
- **Confidence:** High on the path.

### IO-6: Adding samples and opening a project read each WAV's chunks from the card on the MIDI thread

- **Verification:** Confirmed. `Context::sample_slot` and the reload loop in `Shell::lights` both call `read_mpc_slices`, which opens the file and parses its chunks (`context.cpp:1828`, `lights.cpp:329`, `mpc_wav_slices.cpp:87-97`). Both run on the MIDI thread.
- **In the fork:** No change.

- **Where:** `src/melodee/shell/context.cpp:1828` (`Context::sample_slot`), `src/melodee/shell/lights.cpp:329` (`Shell::lights`); callee `src/melodee/shell/mpc_wav_slices.cpp:87-95` (`read_mpc_slices`).
- **Thread and frequency:** MIDI thread (SCHED_FIFO 15), once per sample: each file loaded from the browser, each sample of a Load Kit or Load Bank (all inside one call), and every sample of a project as it opens, up to 16 a pass.
- **Now:**
  ```cpp
  // context.cpp:1828
      project.chops.edit(size_t(free)) = read_mpc_slices(path); // the file's own slices (an MPC's), not the slot's last file's
  // lights.cpp:329
          if (real && !project_->chops[slot].unwritten) project_->chops.edit(slot) = read_mpc_slices(ref.path); // the file's slices are its chop (an older project's waits to be written)
  // mpc_wav_slices.cpp:89-91
      std::ifstream file(path, std::ios::binary);
      if (!file) return chop;
      const auto chunks = chunks_of(file);
  ```
  `chunks_of` seeks from chunk to chunk, past the audio to whatever follows it, then the `atem` chunk is read and parsed as JSON.
- **Cost:** two or three card reads a file, at the same moment the load pool starts reading the same card. A 64-pad kit is 64 opens in one pass of the MIDI thread: by estimate 0.3 s uncontended and more behind the pool's reads. For that time no pad, button, light or MIDI output is served, and `Shell::lock` is held, so the screen thread waits too. Loading a kit while a pattern plays is the case that shows. Unmeasured. Measure: time `lights()` passes (max per second) during a Load Kit.
- **Proposal:** read the slices where the file is opened anyway: in the pool task of `SampleLibrary::dispatch`, return the `SampleChop` with the `Handover` (a field of `LoadedSample`), and let the MIDI thread take it in the block that already runs when `library_->generation()` moves (`lights.cpp:196-215`), unless the slot's chop is `unwritten`.
- **Gain:** latency Medium (pads, lights and MIDI out during loads).
- **Sound or behaviour change:** none in sound; a sample's saved slices arrive with the sample instead of a moment before it.
- **Risk:** Low to Medium: Chop opened before the sample is in would briefly show no slices (the sample is not playable then either).
- **Effort:** M.
- **Confidence:** High that the read is on the MIDI thread (`Context` runs there; `lights()` is its pass). The stall length is an estimate.

### IO-7: Threads started from the MIDI thread inherit SCHED_FIFO 15; the audio-tracks thread also wakes 40 times a second for ever

- **Verification:** Confirmed. `AudioTracks::run` waits with a 25 ms timeout for ever (`audio_tracks.cpp:148-158`). The inherited real-time priority is not checked on the device.
- **In the fork:** Implemented. The idle wait is 100 ms instead of 25 ms. See IO-14 for the bug found in the same loop.

- **Where:** `src/melodee/shell/audio_jobs.cpp:36-37` (`Context::sync_audio`), `src/melodee/shell/audio_tracks.cpp:27-28`, `:148-158` (`AudioTracks::run`); `src/melodee/shell/chop_mode.cpp:208` (`Context::write_chops`).
- **Thread and frequency:** the audio-tracks thread is made on the first MIDI pass and never ends; it wakes every 25 ms. The chop writer is made at a save when slices changed.
- **Now:**
  ```cpp
  // audio_jobs.cpp:36-37, reached from Shell::lights() -> print_tick() -> audio_tick()
      if (!audio_tracks) {
          audio_tracks = std::make_shared<AudioTracks>(host);
  // audio_tracks.cpp:157-158
              wake.wait_for(held, std::chrono::milliseconds(25),
                            [&] { return stopping || !jobs.empty() || !trash.empty() || take_wanted || !prunes.empty(); });
  // chop_mode.cpp:208
      std::thread([due = std::move(due)] { for (const auto& [path, chop] : due) write_mpc_slices(path, chop); }).detach();
  ```
  The base gives the MIDI thread FIFO 15 with `pthread_setschedparam` (`base/src/runtime/controller.cpp:108-109`), without `SCHED_RESET_ON_FORK`, and `std::thread` uses default attributes, so a thread it creates starts with the same policy and priority. Neither thread sets its own; the stems worker and the print engine do (`stems.cpp:98`, `print_engine.cpp:316`).
- **Cost:** (a) 40 wakeups a second on an idle device with no audio track, each preempting whatever normal thread holds that core. (b) File work at real-time priority: `peaks_of()` reads a whole take through when it has no `.peaks` file (`audio_tracks.cpp:116-128`), `prune()` deletes files, `drain()` dithers and writes a take, all above the screen, service and load threads and level with the MIDI thread. Small in CPU; the risk is the service or screen thread starved on a busy core. Unmeasured. Measure: `ps -eLo tid,cls,rtprio,comm | grep melodee` shows the policy; `voluntary_ctxt_switches` in `/proc/<pid>/task/<tid>/status` over 10 s shows the wakeups.
- **Proposal:** at the top of `AudioTracks::run()` and of the chop writer's lambda set the policy on purpose, as `StemSeparator::run` does (`sched_setscheduler(0, SCHED_OTHER, ...)`, nice 0 for the take writer). In `run()`, wait without a timeout unless a take is wanted or being written, or the engine's take ring is not idle:
  ```cpp
  auto pred = [&] { return stopping || !jobs.empty() || !trash.empty() || take_wanted || !prunes.empty(); };
  if (taking || host.audio_take().state() != Bounce::State::Idle) wake.wait_for(held, std::chrono::milliseconds(25), pred);
  else wake.wait(held, pred);
  ```
  (`record()` already notifies. If the ring can leave Idle without `record()`, the "nobody asked for it" branch at `:238-243`, keep a slow fallback such as 500 ms.)
- **Gain:** battery Low (40 wakeups a second gone); latency and robustness Low to Medium.
- **Sound or behaviour change:** none.
- **Risk:** Low. If the take writer was meant to be real-time, give it an explicit low FIFO priority instead; the engine's ring holds 10 s (comment at `:156`), so normal priority looks enough.
- **Effort:** S.
- **Confidence:** High on the 25 ms loop. Medium on the inherited priority: it follows from Linux's default (`PTHREAD_INHERIT_SCHED`), not from a run on the device.

### IO-8: The MIDI thread takes at least twelve plain mutexes each pass, four of them on a lock a SCHED_IDLE thread holds

- **Verification:** Partly confirmed. `Shell::lights` takes `Shell::lock` on every pass (`lights.cpp:180`). I did not count the other eleven mutexes.
- **In the fork:** No change.

- **Where:** `src/melodee/shell/lights.cpp:180,86-87,227,229,274,276` (`Shell::lights` and what it calls); the locks: `update.cpp:50-51,94-95`, `sample_library.cpp:256,574-575`, `sample_library_stems.cpp:45-46`, `audio_tracks.cpp:58-59,83-84`, `print_engine.cpp:213-214,263-264,271-272,279-280`.
- **Thread and frequency:** MIDI thread, SCHED_FIFO 15, every pass (at least every 5 ms).
- **Now:** one pass takes `Shell::lock`, `UpdateLink::lock` twice (`take_note`, `take_offer`), `SampleLibrary::lock` three times (`take_bank`, `folder()`, `take_stems`), `AudioTracks::lock` twice (`take_made`, `take_recorded`) and `PrintEngine::lock` four times (`due`, `take_probed`, `take_sampled`, `take_ready`), almost always to learn that nothing is waiting. `folder()` also returns a `std::string` by value, a heap allocation each pass:
  ```cpp
  // sample_files.cpp:401, in Context::browser_tick()
      auto folder = library->folder();
  // print_engine.cpp:263-265
  bool PrintEngine::due(uint64_t now, Command& out) {
      std::lock_guard<std::mutex> held(lock);
      if (outbox.empty() || outbox.front().first > now) return false;
  ```
  The print thread runs at SCHED_IDLE (`print_engine.cpp:316`) and holds `PrintEngine::lock` in many short sections (`:331-335`, `:340-345`, ...); it also takes `SampleLibrary::lock` and copies the 64 KB `data_of` array under it (`:336`, `sample_library_instruments.cpp:496-500`). `std::mutex` has no priority inheritance.
- **Cost:** in CPU almost nothing (about 0.5 us a pass). The cost is a stall: if the SCHED_IDLE thread is preempted inside one of those sections while every core has normal work (a project load: three pool threads, the screen, the service thread), the MIDI thread blocks until the idle thread is next scheduled, which can be hundreds of milliseconds. The service thread (seven takes of `SampleLibrary::lock` a pass) and the screen thread wait on the same lock. Rare, unmeasured, and only while a print or freeze runs.
- **Proposal:** in each `take_*` and in `due`, test an atomic first and lock only when something waits (the store already does this with its lock-free queues, `project_store.cpp:103-106,191-194`):
  ```cpp
  std::atomic<unsigned> ready_count {0};   // bumped under the lock by whoever pushes
  bool PrintEngine::take_ready(FreezeReady& out) {
      if (!ready_count.load(std::memory_order_acquire)) return false;
      std::lock_guard<std::mutex> held(lock); ...
  ```
  For `browser_tick()`, compare `library->generation()` (already an atomic) before asking for `folder()`. For the locks that must still be shared with the print thread, a small mutex class with `PTHREAD_PRIO_INHERIT` removes the unbounded wait.
- **Gain:** latency Medium in the worst case, Low on average; CPU negligible.
- **Sound or behaviour change:** none.
- **Risk:** Low.
- **Effort:** S to M (about a dozen accessors).
- **Confidence:** High on the lock list (each call and each lock read). Low on how often the stall happens: not measured, and the print engine was only skimmed.

### IO-9: Six separate pollers read the mount table and statvfs every mount, about three times a second

- **Verification:** Confirmed. Six call sites of `platform.storage()` on timers (`melodee_app.cpp:499, 546, 698, 750`, `sample_library.cpp:478`, `project_store.cpp:248`).
- **In the fork:** No change.

- **Where:** `src/melodee/melodee_app.cpp:499` (`read_machine`, every 5 s), `:546` (`warm_card`, every 2 s; every 250 ms with no card), `:698` (`card_update`, every 2 s), `:750` (`web_service`, every 1 s); `src/melodee/shell/sample_library.cpp:478` (every 3 s); `src/melodee/shell/project_store.cpp:248` (every 3 s). Callee: `base/src/drivers/platform.cpp:199-211` (`Platform::storage`).
- **Thread and frequency:** service thread, 2.9 calls a second in all.
- **Now:**
  ```cpp
  // melodee_app.cpp:478-479: the one place that says how often is safe
  // About (round 2): each core's load and the plug-ins' every half second, the card's space every five (statvfs()
  // can wait on a slow card, so not more often, and only while the card is Melodee's).
  // melodee_app.cpp:750-751: one of the other five, every second
              for (const auto& space : hardware.platform.storage())
                  if (space.path.rfind("/media/", 0) == 0) { web_card = space.path; break; }
  ```
  Each call reads `/proc/self/mountinfo` a byte at a time (`platform.cpp:14`), parses each line through two `istringstream`s (`:59-67`) and calls `statvfs` on every block-device mount (`:205`), the card included. All five other callers want only the card's path.
- **Cost:** by estimate 0.3-0.5 ms a call, about 0.1 % of a core in all, and the `statvfs` on the card that the comment above set out to limit to once in 5 s happens 2.9 times a second. Unmeasured. Measure: `strace -c -e trace=statfs,openat -p <pid>` for 10 s.
- **Proposal:** one `card_top` in `MelodeeApp`, refreshed where `read_machine` already reads the mounts and at once when `hardware.card.status()` changes state or `returns`; `warm_card`, `card_update` and `web_service` read the member; `library.service()` and `store.service()` take it (and the free space) as arguments instead of calling `platform.storage()`.
- **Gain:** battery Low.
- **Sound or behaviour change:** none; a card change is seen as fast as the card-status change that already drives `library.rescan()` (`melodee_app.cpp:585-590`).
- **Risk:** Low.
- **Effort:** S to M.
- **Confidence:** High on the call sites; the per-call cost is an estimate.

### IO-10: The web server and mDNS threads wake four times a second each, and the service thread republishes the page's facts every pass

- **Verification:** Confirmed. Both loops use `poll(..., 250)` (`web_server.cpp:208-209`, `mdns.cpp:216-217`).
- **In the fork:** No change.

- **Where:** `src/melodee/web/web_server.cpp:208-209` (`Server::run`), `src/melodee/web/mdns.cpp:216-217,223` (`Mdns::run`), `src/melodee/melodee_app.cpp:754-768` (`web_service`), `src/melodee/web/web_app.cpp:89-95` (`WebApp::publish`).
- **Thread and frequency:** two threads at 4 Hz each, for the whole session; the service thread every 5 ms.
- **Now:**
  ```cpp
  // web_server.cpp:208-209
          pollfd wait {listener, POLLIN, 0};
          if (::poll(&wait, 1, 250) <= 0) continue; // a quarter second at most before stop() is seen
  // mdns.cpp:223: the address looked up (getifaddrs) for every packet heard, before the packet is parsed
          auto reply = mdns_answer(packet, size_t(got), mdns_name, own_address(), legacy);
  // melodee_app.cpp:754-756, 766: every pass; facts.card is a heap string
      melodee::web::Facts facts;
      facts.version = melodee::melodee_version();
      facts.card = web_card;
      web.publish(facts);
  ```
- **Cost:** 8 wakeups a second with no computer attached, plus per service pass one allocation, two mutex takes and a `notify_all`. Under 0.05 % of a core by estimate; listed because it is pure idle work and the fix is small.
- **Proposal:** an `eventfd` beside each socket, written by `stop()`, and `poll` with no timeout (mDNS: the time to the next of its two announcements, then none):
  ```cpp
  pollfd wait[2] {{listener, POLLIN, 0}, {wake_fd, POLLIN, 0}};
  if (::poll(wait, 2, -1) <= 0 || !(wait[0].revents & POLLIN)) continue;
  ```
  In `web_service`, publish only when a field differs from the last published copy. In `Mdns::run`, pass a callback or check the question first so `own_address()` runs only for a question that names `melodee.local`.
- **Gain:** battery Low.
- **Sound or behaviour change:** none.
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** High.

### IO-11: Every use of a cache file writes its timestamp to the card

- **Verification:** Confirmed. `SampleCache::ready` calls `utimes()` on every hit (`sample_cache.cpp:75`).
- **In the fork:** No change.

- **Where:** `src/melodee/shell/sample_cache.cpp:75` (`SampleCache::ready`), `:167` (`SampleCache::wav_ready`).
- **Thread and frequency:** load pool, once per cached file per load: each FLAC, Ogg, AIFF, MP3 or SoundFont part of an instrument, each long non-WAV sample, at every project open and card return.
- **Now:**
  ```cpp
  // sample_cache.cpp:71-75
      struct stat st {};
      if (stat(file.c_str(), &st) || uint64_t(st.st_size) != bytes_of(h.frames, h.channels)) return false;
      ...
      utimes(file.c_str(), nullptr); // played lately, as tidy() goes by
  ```
  `tidy()` only needs to know whether a file was used in the last 30 days (`sample_cache.h:32`).
- **Cost:** one dirtied directory entry per file per load, flushed to the card soon after: a 200-part instrument is 200 metadata updates each time its project opens. Small writes, but writes on a read path. Unmeasured.
- **Proposal:** touch only when the stamp is old: `if (time(nullptr) - st.st_mtime > 86400) utimes(...)` in `ready()` (the `stat` is already there), and the same with one `stat` in `wav_ready()`.
- **Gain:** card writes Low.
- **Sound or behaviour change:** none (`tidy()` works in days).
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** High.

### IO-12: The Samples page copies the whole file listing each frame and each wheel detent

- **Verification:** Confirmed. `SampleLibrary::files()` returns the vector by value under its lock (`sample_library.cpp:260`), and `Shell::snapshot` calls it every frame on the Samples page (`shell.cpp:874-875`).
- **In the fork:** No change.

- **Where:** `src/melodee/shell/sample_library.cpp:260` (`SampleLibrary::files`); callers `src/melodee/shell/shell.cpp:874-875` (`Shell::snapshot`), `src/melodee/shell/sample_files.cpp:383` (`Context::preview_cursor`) and `:416`, `src/melodee/pages/samples_page.cpp:174,209`, `src/melodee/shell/lights.cpp:216`.
- **Thread and frequency:** screen thread per frame while the Samples page is shown (10 to 60 a second); MIDI thread per wheel detent with Preview on.
- **Now:**
  ```cpp
  // sample_library.cpp:260
  std::vector<SampleFile> SampleLibrary::files() const { std::lock_guard<std::mutex> guard(lock); return known.files; }
  // shell.cpp:874-875
          if (ui_.page == PageId::Samples) {
              out.files = library_->files(); out.sample_root = library_->root(); out.files_folder = library_->folder(); out.files_top = library_->top();
  ```
- **Cost:** two heap strings per entry copied and freed under the library's lock: 500 files is about 1000 allocations and frees a frame (roughly 150 us, 1 % of a core at 60 frames a second), 4000 files eight times that. The MIDI thread pays the same per detent, and `lights.cpp:216` copies the list to read its size. Unmeasured.
- **Proposal:** in `snapshot`, keep `out.files` and copy only when `library_->generation()` differs from the one stored with it (the snapshot object is reused between frames, `melodee_app.cpp:437`). Add `SampleLibrary::file_at(size_t)` and `file_count()` for the callers that want one row or the size.
- **Gain:** CPU Low (Medium in a 4000-file folder while scrolling).
- **Sound or behaviour change:** none.
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** High on the copies; the per-frame figure is an estimate. This overlaps the screen auditor's `Shell::snapshot`.

### IO-13: A sample in memory is always two float vectors, so a mono 16-bit file takes four times its size

- **Verification:** Merged. Same issue as SMP-10.
- **In the fork:** See SMP-10.

### IO-14: The audio-track recorder's thread spins a whole core for as long as a take lasts

- **Verification:** Confirmed. Found by me while implementing IO-7, not by an auditor. `take_wanted` stays true from `record()` until the take's file is closed (`audio_tracks.cpp:77, 258, 301`), and the wait's predicate returns true whenever it is set (`:158`), so the loop never waits during a take.
- **In the fork:** Implemented. The predicate is now `(take_wanted && !taking)` and the wait is 25 ms during a take.

- **Where:** `src/melodee/shell/audio_tracks.cpp:148-158` (`AudioTracks::run`), with `:226-262` (`AudioTracks::drain`).
- **Thread and frequency:** the audio-tracks thread, without pause, for the whole of every audio-track take.
- **Now:** the thread waits on a condition variable whose predicate includes `take_wanted`:
  ```cpp
  wake.wait_for(held, std::chrono::milliseconds(25),
                [&] { return stopping || !jobs.empty() || !trash.empty() || take_wanted || !prunes.empty(); });
  ```
  `take_wanted` is set when a take is asked for and cleared only when the take has ended and its file is closed (`audio_tracks.cpp:258`, `:301`). While it is true the predicate is true, so `wait_for` returns at once, `drain()` finds little or nothing new in the ring, and the loop goes round again with no wait. The comment above the wait says the intent was a look every 25 ms.
- **Cost:** one core fully busy for the length of the take, on a device whose cores are otherwise mostly idle while recording. If the thread inherited the MIDI thread's real-time priority (IO-7), it also keeps every normal-priority thread off that core. Found by reading the code while fixing IO-7; not measured.
- **Proposal:** a take asked for should end the wait only until its file is open: use `(take_wanted && !taking)` in the predicate. `taking` is this thread's own flag, set once the file is open. The loop then looks every 25 ms during a take, as the comment intends; the engine's ring holds 10 s.
- **Gain:** battery High and CPU High while recording an audio track; none otherwise.
- **Sound or behaviour change:** none. The take's end is noticed up to 25 ms later.
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** High on the logic (both functions read in full). Not run on the device.

## Screen: the frame loop and drawing framework

### UI-1: The whole pipeline runs at 60 fps whenever anything moves; nothing separates "a hand is on the controls" from "a play head is moving"

- **Verification:** Merged. Same issue as BASE-2.
- **In the fork:** See BASE-2.

### UI-2: Every frame copies 0.8 MB of state under the lock the MIDI thread needs, and rebuilds the sketches of all 2048 patterns

- **Verification:** Merged. Same issue as MODEL-1.
- **In the fork:** See MODEL-1.

### UI-3: External MIDI clock and Active Sensing count as "input" and push the idle screen back to 60 fps

- **Verification:** Merged. Same issue as BASE-4.
- **In the fork:** See BASE-4.

### UI-4: After every change the loop draws 30 more full frames, and slow animations are drawn at 60 fps

- **Verification:** Merged. Same issue as BASE-2 (the 500 ms tail at 60 fps).
- **In the fork:** See BASE-2.

### UI-5: A still screen is redrawn in full 10 times a second for ever; there is no "nothing changed" test before drawing

- **Verification:** Merged. Same issue as BASE-2 (the 10 redraws a second at rest). Skipping `draw()` altogether first needs `edit_loop_check`, `check_pickup` and `fader_check` moved out of `Shell::snapshot` (`shell.cpp:806-809`), which only the screen thread calls.
- **In the fork:** See BASE-2.

### UI-6: Lists, the sample in hand and zoomed peaks are copied or recomputed every frame although generation counters exist

- **Verification:** Merged. Same issues as IO-12 (lists) and PAGE-5 (zoomed peaks).
- **In the fork:** See IO-12.

### UI-7: A sheet that owns the screen is drawn over a page that was drawn in full first

- **Verification:** Merged. Same issue as PAGE-12.
- **In the fork:** See PAGE-12.

### UI-8: Meters and bars are drawn at sub-pixel sizes, so the picture differs on almost every frame they are on screen

- **Verification:** Confirmed. `input_meter` draws its bar `w * level` wide with a fractional width (`input_meter.cpp:23-25`), so any change in level changes pixels.
- **In the fork:** No change.

- **Where:** `src/melodee/pages/track_grid.cpp:19-23`, `:52`, `:73`; `src/melodee/pages/input_meter.cpp:23-25`; `src/melodee/ui/widgets.cpp:80-84`; `src/melodee/pages/common.cpp:819`, `:832`
- **Thread and frequency:** screen, per frame; each changed frame is a push on the display worker
- **Now:**
  ```cpp
  void column(Paint& p, float x, float y, float w, float h, float fill, juce::Colour c) {
      p.fill(x, y, w, h, colour::line, w / 2);
      fill = std::clamp(fill, 0.0f, 1.0f);
      if (fill > 0) p.fill(x, y + h * (1 - fill), w, h * fill, c, w / 2);
  }
  ```
  ```cpp
  float level = fraction(db_of(side ? s.take.right : s.take.left)), sy = by + side * 4.5f;
  p.fill(x, sy, w, 3.5f, colour::line, 1.75f);
  if (level > 0) p.fill(x, sy, w * level, 3.5f, c, 1.75f);
  ```
  A float extent is anti-aliased, so a change of a hundredth of a pixel changes the edge pixel. Input noise alone can move the Record meter every frame; the sixteen load bars on Tracks (`track_grid.cpp:52`) each make their own damage when their load moves.
- **Cost:** it defeats UI-4 and UI-5 on those pages and makes a push every frame (under 5 ms each on the worker, kernel conversion and SPI). Unmeasured.
- **Proposal:** round bar extents to whole pixels where a level is drawn (`std::round(w * level)`), and step input levels by 1 dB. The place bar already does this (`pages/place_bar.cpp:48-59`, "Only the columns the edge moved change"). Animated elements in shared code and how often they change:

  | Element | Where | Changes |
  |---|---|---|
  | place bar edge | `place_bar.cpp:53-59` | by the pixel: about 8.7 to 154 px a second at 120 BPM for 16 bars to 1 bar; plus a tick each beat |
  | BPM text | `common.cpp:49-53` | only with the tempo (can flicker by 0.1 under external clock) |
  | count-in chip, render clock | `common.cpp:66`, `:69-73` | each beat, each second |
  | knob card "turning" | `common.cpp:173` | once, 1.5 s after a turn |
  | fader line, notice, capture, key list | `overlays.cpp:146`, `:211`, `:187`, `:86` | once, 1.2 to 3 s after the event |
  | lesson ring, Sequence sheet blink | `overlays.cpp:98`, `seq_sheet.cpp:43` | every 250 ms, every 150 ms |
  | MIDI route dots and roll | `common.cpp:817-834` | every frame for 0.7 s and 4 s after each note sent |
  | track meter, load bars, input meter | above | every frame while there is signal (sub-pixel) |
  | voice heads and wakes | `shell/playheads.h` | every frame while a voice sounds, 350 ms after |
- **Gain:** CPU Low-Medium, battery Low-Medium (fewer pushes; more with UI-4).
- **Sound or behaviour change:** visible, barely: bars move in whole pixels.
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** Medium: how `EngineHost::meter()` decays, and so how often a rounded bar would still change, is outside my area.

### UI-9: Vector drawings that do not change are rebuilt and re-stroked every frame

- **Verification:** Confirmed. `icon()` copies the stroke path, transforms it and strokes it on every call (`glyphs.cpp:237-249`).
- **In the fork:** No change.

- **Where:** `src/melodee/ui/glyphs.cpp:237-249` (`icon`, 137 call sites in `pages/` and `ui/`); `src/melodee/pages/fx_drawing.cpp:33-44` and its six `draw_*` functions; `src/melodee/pages/sampler_common.cpp:153-170` (`draw_filter_curve`), `:178-219` (`draw_envelope`); `src/melodee/pages/track_grid.cpp:77-86`
- **Thread and frequency:** screen, per frame; icons on most pages, curves on the effect and sampler pages
- **Now:**
  ```cpp
  auto line = s.stroke;
  line.applyTransform(place);
  p.g.strokePath(line, juce::PathStrokeType(stroke, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
  ```
  ```cpp
  juce::Path curve(const std::function<float(float)>& f, float x, float w, int steps = 96) {
      juce::Path path;
      for (int i = 0; i <= steps; ++i) {
          float u = float(i) / float(steps);
          if (i == 0) path.startNewSubPath(x, f(u)); else path.lineTo(x + w * u, f(u));
  ```
  Each icon copies its path (heap), transforms it, builds a stroked outline with rounded joints, then rasterises. An effect picture evaluates `exp`, `sin` or `tanh` at 60 to 360 points through a `std::function`, strokes the polyline with rounded joints and fills the area under it; `draw_dirt` does three such curves and a dashed line. The filter curve does 121 `pow`, `sqrt` and `log10`.
- **Cost:** my estimate is 30-60 us an icon and 0.5-2 ms an effect picture on the A55; unmeasured. A list page or the MENU draws 6 to 16 icons a frame.
- **Proposal:** cache the stroked outline of an icon by (icon, size, stroke) at the origin and draw it with `g.fillPath(outline, AffineTransform::translation(x, y))`; bound the cache as `paint.cpp` bounds its own. Cache an effect picture, the filter curve and the envelope as one small `juce::Image` each, keyed on the slot's kind, bypass and eight values, the rectangle, the colour and the palette, and blit it.
- **Gain:** CPU Medium on effect and sampler pages, Low elsewhere.
- **Sound or behaviour change:** none for the icon cache (same path, same pixels). An image cache gives the same picture to within one level of rounding, and must be made at the same sub-pixel position.
- **Risk:** Low: content-keyed caches cannot go stale.
- **Effort:** M.
- **Confidence:** Medium: the costs are estimates and JUCE's stroker was not read here.

### UI-10: A changed frame is compared and copied three times over between `present()` and the worker

- **Verification:** Merged. Same issue as BASE-12.
- **In the fork:** See BASE-12.

### UI-11: Every small rounded rectangle goes through path rasterisation

- **Verification:** Confirmed. `Paint::fill` sends any radius above zero to `fillRoundedRectangle` (`paint.cpp:245-249`).
- **In the fork:** No change.

- **Where:** `src/melodee/ui/paint.cpp:245-249` (`Paint::fill`); 431 `p.fill` call sites, 119 of them with a literal radius of 2 px or less; example `src/melodee/pages/pads_page.cpp:52-55`
- **Thread and frequency:** screen, per frame, tens to hundreds of calls
- **Now:**
  ```cpp
  void Paint::fill(float x, float y, float w, float h, juce::Colour c, float radius) {
      g.setColour(c);
      if (radius > 0) g.fillRoundedRectangle(x, y, w, h, radius);
      else g.fillRect(juce::Rectangle<float>(x, y, w, h));
  }
  ```
  ```cpp
  for (unsigned k = 0; k < wave.size(); ++k) {
      float bh = std::max(1.0f, wave[k] / 255.0f * wh);
      p.fill(wx + k * bw, wy + (wh - bh) / 2, std::max(1.0f, bw - 0.6f), bh, ink, 0.5f);
  }
  ```
  As I remember JUCE 7/8 (not checked here), `fillRoundedRectangle` builds a `juce::Path` with four curved corners and fills it through an edge table (several heap allocations and a flattening pass), while `fillRect(Rectangle<float>)` with a plain colour has an allocation-free path. The Pads page draws 16 x 32 = 512 bars this way; the place bar, gauges and page dots use radius 1 to 1.5 on shapes 2-3 px high.
- **Cost:** if a small rounded fill costs 5-15 us on the A55, the Pads page spends 3-7 ms a frame on bars alone and shared chrome about 0.2-0.5 ms. Unmeasured; time `Paint::fill` over a frame to confirm before changing anything.
- **Proposal:** in `Paint::fill`, draw as a plain rectangle when `radius <= 0.5f` (the corner is under half a pixel). Larger radii keep the path. The same timing will show whether the radius 1 and 1.5 shapes of the chrome (place bar, gauges, dots) deserve a cached mask; I would not change their look without that number.
- **Gain:** CPU Medium on pages with many thin bars, Low elsewhere.
- **Sound or behaviour change:** visible in principle: four corner pixels of each bar change by a fraction of their alpha.
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** Medium: the JUCE behaviour is from memory and the cost is an estimate.

### UI-12: Each `Paint::text` call scans and hashes its string several times, and the caches are dropped whole when full

- **Verification:** Confirmed. The font, width and layout caches are cleared whole when they reach their limit (`paint.cpp:119-132`).
- **In the fork:** No change.

- **Where:** `src/melodee/ui/paint.cpp:214-243` (`Paint::text`), `:169-185`, `:119`, `:130-154`
- **Thread and frequency:** screen, per text run (357 `p.text` call sites; roughly 30-80 calls a frame)
- **Now:**
  ```cpp
  auto value = shown(original);
  ...
  struct Back { unsigned face; ~Back() { face_in_use = face; tight = false; } } back {narrow_for(value, style, w)};
  ...
  return width(value, style);
  ```
  ```cpp
  template <typename Map> void bound(Map& map, size_t most) { if (map.size() >= most) map.clear(); }
  ```
  How text works: embedded TrueType faces (a pixel face is also a TrueType file), one `juce::Typeface` a weight loaded on first use, `juce::Font` cached by style, HarfBuzz shaping and justification cached per (string, style, box, justification). The author's comment at `paint.cpp:83-88` says text was most of a frame before these caches. What is left per call: `shown()` runs three times (once directly, once in each `width()`), the width cache is looked up twice and the layout cache once (each hashes the whole string), then the cached glyphs are drawn. Numbers are formatted every frame (`std::snprintf` for the tempo at `common.cpp:49-53`, `juce::String(int)` throughout `describe_card`); each non-empty `juce::String` is a heap allocation.
- **Cost:** a few microseconds a call, perhaps 0.1-0.3 ms a frame; unmeasured. The whole-cache clear matters more: every new (string, style, box) is an entry, so a readout that takes a new value each frame would empty all 2048 layouts about every 34 s at 60 fps (slower readouts proportionally later), and the next frame then reshapes every string on screen, a hitch of a few milliseconds.
- **Proposal:** in `Paint::text`, compute the width once and reuse it for `narrow_for` and the return value; pass the already-mapped string to an internal `width`. Replace `clear()` with two generations (keep the current map and the previous one; on overflow drop the previous) so entries in use survive.
- **Gain:** CPU Low, latency Low (no periodic hitch).
- **Sound or behaviour change:** none.
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** High on the code; the per-glyph cost inside JUCE's own glyph cache was not examined.

### UI-13: The boot animation recomposes every pixel on frames that cannot differ, and keeps its three images afterwards

- **Verification:** Confirmed. `BootAnimation::compose` recomposes all 320 columns whenever the millisecond changes (`boot.cpp:63-70`). Start-up only.
- **In the fork:** No change.

- **Where:** `src/melodee/ui/boot.cpp:63-86` (`compose`), `:88-96` (`draw`), `:41-58` (`load`)
- **Thread and frequency:** screen, per frame for 3.85 s at start (`boot.h:15-17`)
- **Now:**
  ```cpp
  void BootAnimation::compose(uint32_t at) {
      if (composed == int64_t(at)) return;
      composed = at;
  ```
  `at` is in milliseconds, so the early return never fires on consecutive frames. During the 200 ms of black, the 300 ms and the 900 ms holds the result is the same picture, computed again in float arithmetic for each of the 76,800 pixels. During the backdrop's 900 ms fade every pixel changes, so each frame is a whole-screen push (about 54 ms) and the rate falls to about 18 fps by itself. `logo`, `backdrop` and `canvas` (3 x 307 kB) are never released.
- **Cost:** about 1-2 ms a frame for about 1.4 s, once; and 0.9 MB held for the life of the process. Unmeasured.
- **Proposal:** clamp `at` to the end of the phase it is in before the `composed` test (the wipe ends at 1400 ms, the fade at 2600 ms), and in `finished()` release the three images once it returns true for good.
- **Gain:** memory Low, CPU Low (start-up only).
- **Sound or behaviour change:** none.
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** High.

## Screen: individual pages

### PAGE-1: Readouts and pulses that keep a stopped page at 60 fps

- **Verification:** Confirmed. The lesson ring's alpha alternates every 250 ms from the clock (`overlays.cpp:98`), and the input meter's width follows the level continuously (`input_meter.cpp:22-25`). I did not read the Settings load bars or the Mod LFO line.
- **In the fork:** No change.

- **Where:** several draw functions, listed in the table.
- **Thread and frequency:** screen, per frame. The effect is 60 frames a second instead of 10 for as long as the element is on screen with the transport stopped.
- **Now:**

| Element | Where (draw function) | What changes | How sure |
|---|---|---|---|
| Lesson pulse ring | `overlays.cpp:98` (`draw_lesson`, from `draw_overlays:91`) | alpha flips every 250 ms for as long as a lesson step sits on a soft key or a knob card | certain |
| Input level bars | `input_meter.cpp:22-26` (`input_meter`, from `record_page.cpp:106` and `input_config_page.cpp:82`); `stage_page.cpp:913-918` (`input_picture`, an audio track's Stage INPUT) | bar width is `w * level` in float; the level is a peak with a 0.95 fall per block (`engine/take_recorder.h:83-91`) | certain whenever the input carries any noise |
| DSP load bar and percent | `settings_page.cpp:183-188` (Settings > AUDIO), `315-344` (About) | `block_load` is rewritten every block (`engine_host.cpp:3197-3199`) | certain |
| Free LFO's position line | `mod_page.cpp:62` (`draw_mods`) | a free LFO advances every block, not gated on the transport (`engine_host.cpp:1435`, called at `2703`) | certain with a free LFO that has a target in view |
| Plug-in load bar and percent | `stage_page.cpp:1086-1089` (`plugin_sound_picture`, a plug-in with no presets), `1156`; `edit_page.cpp:161` | the panel is refreshed 20 times a second (`shell/plugin_jobs.cpp:300`) | only while the plug-in is awake |
| "CPU n %" pill, track percent, load bars | `tracks_page.cpp:26-37`; `track_grid.cpp:51-52` (`cell`) | integer percent of a value that moves every block; a bar moves only above 5 % (it is clamped to 2 px below) | likely while playing; not known stopped |
| Late timeouts | `common.cpp:173` (a card's "turning" state, 1.5 s), `common.cpp:190` (fader header, 1.5 s), `overlays.cpp:86` (key list, 1.2 s), `overlays.cpp:187, 211` (notices) | one changed frame when the time runs out, which starts another 500 ms at 60 fps | certain, small |

```cpp
// overlays.cpp:98
const auto ring = colour::text.withAlpha((s.now / 250) % 2 ? 1.0f : 0.3f);
// input_meter.cpp:23-25
float level = fraction(db_of(side ? s.take.right : s.take.left)), sy = by + side * 4.5f;
p.fill(x, sy, w, 3.5f, colour::line, 1.75f);
if (level > 0) p.fill(x, sy, w * level, 3.5f, c, 1.75f);
```
- **Cost:** 50 extra snapshots and full-page draws a second. If a frame costs 3 to 6 ms (unmeasured), that is 15 to 30 % of a core more than an idle page, for the whole time the page is open. Measure with `screen_presented` over ten seconds on Record, Input Config, Stage INPUT of an audio track, Mod, and any page during a lesson, all stopped: 100 means idle, 600 means pinned.
- **Proposal:** (1) Lesson: a steady ring, or pulse only for the first two seconds of a step. (2) Meters: round the bar to whole pixels and draw nothing below about -72 dBFS, so a quiet input can stand still. (3) Load readouts: keep the shown value in a page-static and refresh it once a second, bars in whole pixels. (4) LFO line: whole pixels (it still moves; that is what the page shows). These are the page's share. The rest is the frame loop's: a meter at 20 Hz or a pulse at 4 Hz needs neither 60 fps nor a 500 ms tail, so a frame-rate cap a page can ask for would help every row above. That is for whoever owns `run_app.cpp`.
- **Gain:** battery High while one of these is on screen (Record and Input Config are the likely ones), Low otherwise.
- **Sound or behaviour change:** visible (no pulse, coarser meter and load steps).
- **Risk:** Low.
- **Effort:** S each.
- **Confidence:** High for the lesson ring, the Settings bars and the LFO line (from the code alone). Medium for the meters: I did not measure the ADC's noise floor with nothing plugged in. Low for the CPU pill while stopped: it depends on block-time jitter.

### PAGE-2: Pads grid draws up to 512 rounded bars a frame, each through the path rasteriser

- **Verification:** Confirmed. Pads draws 32 bars per pad with a 0.5 px radius, so each goes through the rounded-rectangle path (`pads_page.cpp:52-55`, `paint.cpp:247`).
- **In the fork:** No change.

- **Where:** `src/melodee/pages/pads_page.cpp:52-55` (`draw`); the same pattern at `stage_page.cpp:1006-1009` (`frozen_picture`, 96 bars), `chop_page.cpp:248-253` (`draw`, a 3 px square per hit, up to `max_hits` = 512), `stage_page.cpp:635-646` (`wave_picture` on FOLLOW: a `juce::Path`, a `strokePath` and a circle per hit, up to 512).
- **Thread and frequency:** screen, per frame. Pads is also drawn over another page while Sample is held (`overlays.cpp:72-75`).
- **Now:**
```cpp
// pads_page.cpp:50, 52-55
float bw = ww / float(wave.size());
for (unsigned k = 0; k < wave.size(); ++k) {
    float bh = std::max(1.0f, wave[k] / 255.0f * wh);
    p.fill(wx + k * bw, wy + (wh - bh) / 2, std::max(1.0f, bw - 0.6f), bh, ink, 0.5f);
}
```
`ww` is 64 px and `wave` holds 32 columns, so each bar is 1.4 px wide with a 0.5 px radius, and each goes to `fillRoundedRectangle` (`paint.cpp:247`). A full bank is 16 x 32 = 512 of them, on top of the 16 pad tiles and 16 names.
- **Cost:** 512 path builds and edge-table fills a frame, plausibly 2 to 5 ms (unmeasured), 60 times a second while the transport runs or a pad sounds. Measure `screen_draw_us` on Pads with a full bank against an empty one.
- **Proposal:** radius 0 for these bars (a 0.5 px corner on a 1.4 px bar cannot be seen), and better, one call per pad: fill a `juce::RectangleList<float>` with the 32 bars and `p.g.fillRectList(list)`. The same for `frozen_picture`'s bars and Chop's hit marks. On FOLLOW, add every join to one `juce::Path` and stroke it once. A guard in `Paint::fill` (`if (radius > 0 && std::min(w, h) > 3)`) would catch all of them in one place; that file is another auditor's.
- **Gain:** CPU Medium, battery Medium (Pads, and Stage of a frozen track).
- **Sound or behaviour change:** none that can be seen at this size; strictly, visible at sub-pixel level.
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** High that the calls are made; Medium on the cost per call (JUCE not in the tree).

### PAGE-3: Stage of an instrument pad redraws every zone of the map each frame

- **Verification:** Confirmed. `zone_tiles` loops over every zone shape and fills or rings each one, every frame (`instrument_map.cpp:89-102`).
- **In the fork:** No change.

- **Where:** `src/melodee/pages/instrument_map.cpp:89-102` (`zone_tiles`), reached from `stage_page.cpp:1213` (`draw`) -> `wave_picture:532` -> `instrument_picture:342` -> `map_picture:281`, `key_strip:300` or `tune_picture:320`. Every sample page of Stage (ZONES, TUNE, the envelopes, PLAY, FOLLOW, CLOUD, the filters) and the stand-alone sampler pages (`kit_page`) go through one of the three when the pad holds an instrument.
- **Thread and frequency:** screen, per frame.
- **Now:**
```cpp
// instrument_map.cpp:89-91, 94-95, 98
for (size_t n = 0; n < m.shapes.size(); ++n) {
    const auto& z = m.shapes[n];
    if (z.flags & ZoneShape::Release) continue; // a key let go: under the press's own zone
    ...
    const float r = std::min(2.0f, std::min(w, hh) / 3);
    switch (state_of(m, z)) {
    ...
    case PartState::Landed: p.fill(x0, y0, w, hh, int(n) == lit ? c : c.withAlpha(alpha), r); break;
```
`r` is never zero (`w` and `hh` are at least 1), so even a one-pixel tile is a rounded path. One rounded fill per zone, plus a `fillEllipse` root dot where it fits (`:100-101`). Round-robin zones share one box and are drawn on top of each other. Nothing in the picture changes between frames unless the zone in hand or the pad's tune changes.
- **Cost:** linear in the instrument's zones: a piano of 88 keys by 8 velocity layers is 704 fills a frame, an SFZ with round robins several thousand. At several microseconds each that is 3 to 15 ms a frame (unmeasured); at the high end that is most of a core while playing. Measure `screen_draw_us` on Stage with a large SFZ loaded against a one-sample pad.
- **Proposal:** in order of effort: (1) skip a zone whose box was already drawn in the same state (round robins share a box; compare with the previous zone if they are stored together, which I did not check, else keep a small set of the boxes seen); (2) `fillRect` when `min(w, hh) < 4`; (3) draw the tiles once into a `juce::Image` kept by the page, keyed on the library's generation (`SampleLibrary::generation()` exists, `sample_library.h:172`; it would have to be copied into the Snapshot), the key row's ends, `lit`, the colour and the palette, and blit it each frame.
- **Gain:** CPU High for instrument tracks, battery Medium.
- **Sound or behaviour change:** none for (1) and (3); (2) squares the corners of tiles under 4 px.
- **Risk:** Low for (1) and (2); Medium for (3) (a stale image if the key misses something).
- **Effort:** S for (1) and (2), M for (3).
- **Confidence:** High on the loop; I did not check how many zones real instruments on the card have.

### PAGE-4: Motion page walks every Motion point six times and strokes one path per lane, every frame

- **Verification:** Confirmed. Motion builds a `juce::Path` per lane from the pattern's Motion points on every frame (`motion_page.cpp:50-62`).
- **In the fork:** No change.

- **Where:** `src/melodee/pages/motion_page.cpp:50-86` (`draw`).
- **Thread and frequency:** screen, per frame (60 fps while playing: the playhead at `:88-90` moves every frame).
- **Now:**
```cpp
// motion_page.cpp:66-67, 70-72, 78-80
for (const auto& m : motion) {
    if (!m.on(lp, lk) || m.lock() || m.tick >= length) continue;
    ...
    line.lineTo(ex, y_of(value));
    value = m.value;
    line.lineTo(ex, y_of(value));
}
...
p.g.strokePath(line, juce::PathStrokeType(in_hand ? 2.2f : 1.2f, juce::PathStrokeType::mitered, juce::PathStrokeType::butt));
for (const auto& m : motion) { // locks: a bar at the value for as long as each holds
    if (!m.on(lp, lk) || !m.lock() || m.tick >= length) continue;
```
Both loops sit inside `for (unsigned order = 0; order < 3; ++order)` (`:50`), so every point is visited six times a frame. A pattern holds up to 8192 points at tick resolution (`model/limits.h:17`). Each lane adds two path segments a point, on a graph 308 px wide.
- **Cost:** a pot swept over eight bars records thousands of points; the path then has thousands of segments, all stroked and rasterised each frame for a picture that only changes when the pattern does. I would expect several milliseconds a frame on a dense pattern (unmeasured). Measure `screen_draw_us` on Motion while playing a pattern with a long recorded sweep.
- **Proposal:** build each lane's path from at most one vertical span per pixel column: walk `motion` once, and for each lane keep per column the first, lowest, highest and last value, then emit at most four vertices a column. That bounds the path at about 1200 vertices whatever the pattern holds. Further, keep the three paths in a page-static keyed on a pattern revision and redraw only the playhead; there is no such counter today, and `Context::sync_pattern` (`shell/context.cpp:1225-1231`) is the one place every edit passes through.
- **Gain:** CPU Medium (High on dense Motion), battery Low.
- **Sound or behaviour change:** none at pixel level for the decimation.
- **Risk:** Low.
- **Effort:** S for the decimation, M for the cache (it needs the counter in the shell and the Snapshot).
- **Confidence:** High on the loop; Medium on the cost.

### PAGE-5: The zoomed waveform's peaks are worked out from the audio again every frame, under the shell's lock

- **Verification:** Confirmed. `SampleLibrary::detail` scans the sample's frames for each column under the library lock (`sample_library.cpp:186-196`), called from `Shell::snapshot` every frame while zoomed (`shell.cpp:861-866`).
- **In the fork:** No change.

- **Where:** `src/melodee/shell/shell.cpp:861-866` (`Shell::snapshot`) calling `SampleLibrary::detail`, `src/melodee/shell/sample_library.cpp:186-208`. Outside `pages/`, but it is the answer to "is the overview cached per zoom level": it feeds `wave_picture` (`stage_page.cpp:599-603`) and Chop's `draw` (`chop_page.cpp:181-182`).
- **Thread and frequency:** screen, per frame, while Trim or Chop is zoomed in. It runs inside `Shell::lock` (`shell.cpp:805`), which `Shell::midi` also takes (`shell.cpp:93-94`), and inside `SampleLibrary::lock` (`sample_library.cpp:187`).
- **Now:**
```cpp
// sample_library.cpp:195, 197-201
for (unsigned c = 0; c < columns; ++c) {
    ...
    float l = 0, r = 0; // at most 128 frames a column looked at, so a wide view stays cheap
    uint32_t stride = std::max<uint32_t>(1, (b - a) / 128);
    for (uint32_t f = a; f < std::max(b, a + 1) && f < to; f += stride) {
        l = std::max(l, std::fabs(data->left[f]));
        r = std::max(r, std::fabs(data->right[f]));
    }
```
`columns` is 512 (the Snapshot's `detail`). The whole-file overview is made once at load (512 columns, `LoadedSample::overview`) and the pages never touch audio. The zoomed view is not kept: the same `(slot, from, to)` gives the same 512 columns frame after frame.
- **Cost:** up to 512 x 128 x 2 = 131,072 sample reads a frame, spread over the whole view, so mostly cache misses once the view is wider than about a second. Roughly 1 to 3 ms a frame at a low zoom on a long in-memory sample (unmeasured), and the MIDI thread waits for it if a pad is hit meanwhile. A long streamed sample is cheaper at wide views, where its columns come from a peaks table of 1024-frame blocks (`sample_library.cpp:225-238`); closer in it reads pinned frames with the same cap (`:245-251`).
- **Proposal:** keep the last result in the Shell with its key and reuse it:
```cpp
struct { unsigned slot, generation; uint32_t from, to; Overview all; std::array<Overview, 2> sides; bool ok; } detail_kept;
// in snapshot(): recompute only when slot, view.from, view.to or library_->generation() changed
```
- **Gain:** CPU Low to Medium, latency Medium (shorter hold of the shell lock).
- **Sound or behaviour change:** none.
- **Risk:** Low, as long as the library's generation moves when a sample's audio changes (normalize, a take landing). The comment at `sample_library.h:171` says it does; I did not trace every writer.
- **Effort:** S.
- **Confidence:** High that it is recomputed; Medium on the cost.

### PAGE-6: Lists draw six rows but are copied, sorted or rebuilt whole every frame

- **Verification:** Merged. Same issue as IO-12.
- **In the fork:** See IO-12.

### PAGE-7: Song draws the parts of long clips that are out of view

- **Verification:** Confirmed. `draw_clip` loops over a clip's passes from its start, not from the first one in view (`song_page.cpp:67-68`).
- **In the fork:** No change.

- **Where:** `src/melodee/pages/song_page.cpp:63-69, 79-86, 88-90` (`draw_clip`, from `draw:236-237`).
- **Thread and frequency:** screen, per frame (60 fps while the song plays).
- **Now:**
```cpp
// song_page.cpp:67, 80-82, 84-85
for (float px = cx; px < cx + cw - 1; px += std::max(pass, 2.0f))
...
for (unsigned k = 0; k < steps; ++k) {
    unsigned at = k % length;
    if (at >= 64 || !sketch.keys[at]) continue;
    ...
    if (nx > right) break;
    p.fill(nx, ny, std::max(1.0f, step_w * 0.8f), 1, note);
```
The first loop calls `region_wave` once for each pass of an audio clip's pattern (`:68`); `cx` is the clip's own left edge. A clip wholly out of view returns early (`:57`), but one that is partly in view is drawn from its own start: every step and every audio pass to the left of the view is still issued and thrown away by the clip rectangle. Only the right edge is tested.
- **Cost:** a 64-bar clip seen at zoom 4 with the view at its end issues up to 1024 fills of which 64 can show; an audio clip of a one-bar loop placed for 64 bars runs `region_wave` 64 times (about 45 columns each, a peak scan a column) for 4 passes on screen. Around a millisecond a frame in those cases (unmeasured); nothing for short clips.
- **Proposal:** start at the first visible index: `unsigned k0 = cx < left ? unsigned((left - cx) / step_w) : 0;`, the same for the pass loop (`px` from the pass that reaches `left`, stop at `right`) and the repeat notches.
- **Gain:** CPU Low to Medium (long arrangements).
- **Sound or behaviour change:** none.
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** High.

### PAGE-8: Roll: one fill per note of the whole pattern for the strip at the foot, and a full scan for the marks

- **Verification:** Confirmed. Roll's foot strip fills one rectangle per event of the whole pattern (`roll_page.cpp:186-190`).
- **In the fork:** No change.

- **Where:** `src/melodee/pages/roll_page.cpp:186-190`, `:75-77`, `:135-139` (`draw`).
- **Thread and frequency:** screen, per frame (60 fps while playing).
- **Now:**
```cpp
// roll_page.cpp:186-190, then :76
for (const auto& e : pattern.events) {
    if (e.tick >= pattern.length) break;
    if (roll_row(e, lane) < 0) continue;
    p.fill(gutter + tw * float(e.tick) / float(pattern.length), by, 1, 2, colour::dim);
}
...
unsigned n = unsigned(roll_marked_events(pattern, ui).size());
```
The first loop draws the whole pattern onto a strip `tw` = 294 px wide; the second line runs while notes are marked.
`roll_marked_events` (`shell/roll.cpp:161-166`) tests every event against every mark (up to 128) and returns a `std::vector`. The note loop at `:135` passes over every event before the view and does not stop at the view's end (`continue` at `:139`; the events are in tick order).
- **Cost:** the note grid itself is well culled (16 columns, 12 rows). The strip costs one fill a note: 2000 notes in a 128-bar pattern is 2000 fills for 294 pixels, about a millisecond or two a frame (unmeasured). A pattern holds up to 65,535 events (`model/limits.h:32`).
- **Proposal:** mark a `std::array<bool, 294>` of columns and fill the runs; count the marks by looking each one up with `pattern.lower(mark.tick)` instead of testing every event against every mark and building a vector; `break` once `e.tick >= to`.
- **Gain:** CPU Low (Medium on very dense patterns).
- **Sound or behaviour change:** none.
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** High.

### PAGE-9: Looper rings and the Compressor's ghost bar are stroked afresh every frame though they rarely change

- **Verification:** Confirmed. `draw_ring` builds and strokes a path for each loop every frame (`looper_page.cpp:80-95`). I did not read the Compressor's ghost bar.
- **In the fork:** No change.

- **Where:** `src/melodee/pages/looper_page.cpp:80-95` (`draw_ring`, four times from `draw_loops:128-133`); `src/melodee/pages/compressor_page.cpp:80-89` (`draw`).
- **Thread and frequency:** screen, per frame; 60 fps while a loop plays (the hand at `looper_page.cpp:97-105`) or the transport runs.
- **Now:**
```cpp
// looper_page.cpp:83, 90-91, 94
for (unsigned k = 0; k < ticks; ++k) {
    ...
    wave.startNewSubPath(cx + (r - len / 2) * std::cos(a), cy + (r - len / 2) * std::sin(a));
    wave.lineTo(cx + (r + len / 2) * std::cos(a), cy + (r + len / 2) * std::sin(a));
}
p.g.strokePath(wave, juce::PathStrokeType(1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
// compressor_page.cpp:87
juce::PathStrokeType(1.2f).createDashedStroke(dashed, ghost, dashes, 2);
```
`ticks` is 72 (`looper_page.cpp:60`): 72 two-point sub-paths with round caps for each loop that holds something. The Compressor's `ghost` is the bar before, 154 points (`comp_bar_columns`), dashed again every frame though it changes once a bar.
- **Cost:** a loop's waveform changes only while it records or overdubs, the ghost once a bar. Four rings are 288 round-capped segments and about 1150 `cos`/`sin` a frame; I would guess 0.5 to 1 ms a frame for the Looper and a few hundred microseconds for the Compressor (unmeasured).
- **Proposal:** keep each ring's stroked `juce::Path` in a page-static, rebuilt when a cheap sum of its 256 peaks, its state or the palette changes, and `fillPath` it; keep the dashed ghost path until `comp_ghost` changes (compare the array, 154 entries).
- **Gain:** CPU Low to Medium on those two pages.
- **Sound or behaviour change:** none.
- **Risk:** Low.
- **Effort:** S each.
- **Confidence:** Medium (the saving depends on what stroking costs in JUCE).

### PAGE-10: A time readout that changes every frame defeats the text caches

- **Verification:** Confirmed. Trim's time readout is rebuilt as a new string every frame while a voice is heard (`stage_page.cpp:754-759`).
- **In the fork:** No change.

- **Where:** `src/melodee/pages/stage_page.cpp:754-759` (Trim, while a voice is heard) and `:763-768` (Cloud with Scan), both in `wave_picture`; the caches are `ui/paint.cpp:119, 130-153`.
- **Thread and frequency:** screen, per frame while the pad sounds.
- **Now:**
```cpp
// stage_page.cpp:757-759
juce::String now = seconds(uint32_t(std::clamp(into, 0.0, double(end - start))));
if ((heard->flags & tap::Loop) && heard->pass) two_part("PASS " + juce::String(heard->pass + 1), dot + now);
else two_part(now, " OF " + seconds(end - start));
// paint.cpp:119
template <typename Map> void bound(Map& map, size_t most) { if (map.size() >= most) map.clear(); }
```
`seconds()` formats with `"%.3f s"` (`stage_page.cpp:438-442`). Millisecond resolution means a new string nearly every frame: one new shaped layout and one new width a frame, never used again.
- **Cost:** one HarfBuzz shaping a frame (tens of microseconds, unmeasured), and the layout cache (2048 entries) is emptied whole about every 34 s of sound, after which the next frame shapes every string on the page again: a small hitch on the screen thread.
- **Proposal:** show hundredths while it plays, or advance the readout ten times a second (the head on the waveform already shows the fine movement). The cache's clear-all policy belongs to `paint.cpp`'s auditor.
- **Gain:** CPU Low.
- **Sound or behaviour change:** visible (a coarser moving readout).
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** High on the mechanism, Low on the size of the hitch.

### PAGE-11: Auto Sample's running light redraws the screen 60 times a second for the whole job

- **Verification:** Confirmed. Auto Sample's running light is placed from `s.now` and so moves every frame (`auto_sample_page.cpp:200-204`).
- **In the fork:** No change.

- **Where:** `src/melodee/pages/auto_sample_page.cpp:200-204` (`draw`), also the 250 ms and 200 ms blinks at `:102` and `:154`.
- **Thread and frequency:** screen, per frame, for as long as a sampling job runs (minutes).
- **Now:**
```cpp
// auto_sample_page.cpp:200-204
const float run = dw + 30, at = std::fmod(float(s.now % 100000) * 0.12f, run) - 15; // 120 px a second
for (int k = -12; k <= 12; ++k) {
    const float sx = x0 + at + k;
    if (sx > x0 + 1 && sx < x0 + dw - 2) p.fill(sx, gy + 1, 1, 4, juce::Colours::white.withAlpha(0.8f * (1.0f - std::abs(k) / 12.0f)));
}
```
- **Cost:** a full page at 60 fps beside a job that renders as fast as the machine allows; the bar and the note's waveform already show progress.
- **Proposal:** drop the light, or step it four times a second in whole pixels.
- **Gain:** CPU Low, and the job finishes a little sooner.
- **Sound or behaviour change:** visible.
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** High.

### PAGE-12: A page is drawn in full, then covered by a sheet that owns the screen

- **Verification:** Confirmed. `MelodeeApp::draw` draws the page and then `draw_overlays`, whose update and loading sheets return after covering the whole screen (`melodee_app.cpp:465-466`, `overlays.cpp:63-64`).
- **In the fork:** No change.

- **Where:** `src/melodee/melodee_app.cpp:465-466` with `src/melodee/pages/overlays.cpp:63-64` (update and loading sheets), `:72-75` (Sample held: the screen is cleared and Pads or Keys drawn over the page).
- **Thread and frequency:** screen, per frame while the sheet is up.
- **Now:**
```cpp
// melodee_app.cpp:465-466
page_spec(frame->ui.page).draw(p, *frame);
draw_overlays(p, *frame);
// overlays.cpp:64, 73-74
if (s.ui.loading) { draw_loading_sheet(p, s); return; } // it owns the screen and the controls until it is in
p.fill(0, 0, zone::width, zone::height, colour::bg);
(s.track().engine == EngineKind::Sampler ? pads_page : keys_page).draw(p, s);
```
`draw_loading_sheet` starts by filling the whole screen too (`overlays.cpp:231`).
- **Cost:** one wasted page draw a frame during a project load (when the cores are busy) and while Sample is held.
- **Proposal:** skip the page's draw when `ui.loading`, `ui.update.phase` or the Sample sheet will cover it.
- **Gain:** CPU Low, load time Low.
- **Sound or behaviour change:** none.
- **Risk:** Low (Pads and Keys draw their own frame and knob strip from the same Snapshot, so nothing of the page beneath shows).
- **Effort:** S.
- **Confidence:** High.

## Project model and sequencing

### MODEL-1: Every screen frame copies the whole Project and rebuilds 2048 pattern sketches under the shell lock

- **Verification:** Confirmed. `Shell::snapshot` does `out.project = *project_` and loops over all 16 × 128 patterns to rebuild their sketches, inside `Shell::lock` (`shell.cpp:804-815, 878-894`). The 539 KB figure is the auditor's `sizeof` probe; I did not repeat it.
- **In the fork:** Implemented. Third pass. A frame in which no kit and no pattern can have changed leaves the screen's copy of the kits (nine tenths of a project's bytes) and the 2,048 pattern sketches as the last whole frame made them. `Context::heavy_serial` moves with every control and MIDI message and wherever a kit or pattern change is told to the audio thread; any command sent to the audio thread, and half a second at most, also make the next frame whole. Measured with the MPC's cache sizes: the snapshot falls from about 3.4 million cycles a frame to 0.75 million, and it holds the lock the pads wait on for that much less time. Every page drawn 120 frames with light snapshots and with whole ones, stopped and playing: the same pixels. The monkey checks every light snapshot against a whole one. See "Third pass".

- **Where:** `src/melodee/shell/shell.cpp:804` (`Shell::snapshot`), called by `src/melodee/melodee_app.cpp:437` (`MelodeeApp::draw`). The data: `Project` (`model/project.h:382`), `Snapshot` (`shell/snapshot.h:34`). The function is in the shell auditor's file; I report it because its cost is the model's size.
- **Thread and frequency:** screen thread, every `App::draw()`: up to 60 Hz while the picture changes (a moving playhead or meter), 10 Hz when static (`base/src/runtime/run_app.cpp:127`). It holds `Shell::lock`, which the MIDI thread takes for every pad, button and MIDI message (`shell.cpp:94`, `:128`) and for `lights()` every 5 ms (`lights.cpp:180`).
- **Now:**
  ```cpp
      std::lock_guard<std::mutex> guard(lock);                      // shell.cpp:805
          out.project = *project_;                                  // :813
      out.pattern = context_.pattern();                             // :817
      for (unsigned t = 0; t < track_count; ++t)                    // :878
          for (unsigned n = 0; n < pattern_count; ++n) {
              const auto& pattern = patterns_->at[t][n];
              auto& sketch = out.sketches[t][n];
              sketch.length = pattern.length; sketch.keys.fill(0);  // :882
  ```
  (The trailing `//` comments are my line markers; lines between the marked ones are left out.)
- **Cost:** per frame it copies 538 968 bytes of `Project` (plus 20 480 bytes for every 64 sample slots in use), rewrites all 229 376 bytes of sketches while reading the 491 520-byte array of pattern headers, and copies the selected pattern's events and motion (16 bytes a note: a 5000-note pattern is 80 KB a frame, a full one 1 MB). That is about 1.6 to 1.8 MB moved through the caches per frame, more than an A55's L1 and private L2 hold, so it also evicts what the other threads keep in the shared cache. My estimate is 0.4 to 0.8 ms a frame at 1.8 GHz, so 0.4 to 0.8 % of a core when idle (10 Hz) and up to 2 to 5 % while playing; this is unmeasured. The MIDI thread (SCHED_FIFO 15) waits for the lock that long when a pad hit lands during a frame, and longer if the screen thread is preempted while holding it (`std::mutex` has no priority inheritance). To measure: time `shell.snapshot()` in `draw()` with `steady_clock` and publish mean and maximum in the status line, with a project of 16 kit tracks and a long pattern selected.
- **Proposal:** copy only what changed since the last frame; the `Snapshot` object already persists between frames (`melodee_app.h:87`).
  1. Patterns: give `Context` a revision per pattern, bumped in `sync_pattern()` (every audible pattern change already goes through it), for a whole track in `send_track()` (`track_copy.cpp:14`) and for all in `switch_to()` and the reload path. `snapshot()` rebuilds a sketch, and copies `out.pattern`, only when its revision differs from the one stored in the `Snapshot`:
     ```cpp
     if (out.sketch_rev[t][n] == context_.pattern_rev[t][n]) continue; // 16 KB compared instead of ~700 KB rebuilt
     ```
  2. Kits: a revision per track for `pads`, `pad_fx` and `zone_trims`, bumped where a kit change is sent to the engine (`send_pad`, `sync_chain(pad_chain(...))`, `send_trims`, `send_track`, a load). `snapshot()` copies those three arrays for a track only when its revision moved and copies the remaining ~58 KB of `Project` every frame as now. The structural alternative is to move the three arrays out of `Track` into a heap `Kit` that only sampled tracks own; it shrinks every other whole-`Project` copy too, but touches every `track.pads[...]` in the code.
- **Gain:** battery Medium (the idle 10 Hz copy goes from ~1.7 MB to ~80 KB of traffic), CPU Medium while playing, latency Medium (the lock is held about a tenth as long).
- **Sound or behaviour change:** none (audio bit-exact; the screen shows the same data).
- **Risk:** Medium. A write that skips its hook leaves the screen stale. For patterns the hook is the same call the audio thread depends on; for kits every writer of `Track::pads` has to be checked.
- **Effort:** M for part 1 and part 2 with revisions; L for the structural split.
- **Confidence:** High on what is copied and how often (code read, sizes measured). Medium on the time. Not checked: every writer of `Track::pads`, and which pages read other tracks' kits from `Snapshot::project`.

### MODEL-2: A time signature change re-sends all 2048 patterns for each knob value: leaked pattern copies, a burst on the audio thread, and a queue that can overflow

- **Verification:** Confirmed. `set_signature` calls `sync_pattern` for every whole-bar pattern, up to 2048 (`context.cpp:1774-1781`); the engine's retire queue holds 1024 patterns plus 64 in limbo, and `retire` leaks past that (`engine_host.cpp:246-250`, `engine_host.h:703-704`).
- **In the fork:** No change.

- **Where:** `src/melodee/shell/context.cpp:1769` (`Context::set_signature`), reached from the `TimeSignature` card (`context.cpp:681`, Shift + K2 on Patterns); `src/melodee/engine/engine_host.cpp:246` (`EngineHost::retire`); `engine_host.h:674`, `:703`, `:704`.
- **Thread and frequency:** MIDI thread, once for every value the pot passes (24 signatures on the card); the audio thread then applies the burst at the start of one or two blocks.
- **Now:**
  ```cpp
          for (unsigned p = 0; p < pattern_count; ++p) {            // context.cpp:1776
              auto& pattern = patterns.at[t][p];
              if (pattern.length % before) continue;
              pattern.length = std::min<uint32_t>(pattern.length / before * after, project.max_length());
              sync_pattern(t, p);                                   // :1780
  void EngineHost::retire(Pattern* pattern) {                       // engine_host.cpp:246
      if (!pattern || retired_patterns.push(pattern)) return;
      for (auto& q : limbo) if (!q) { q = pattern; return; } // tried again each block
  ```
  (The file and line markers are mine.) The next line of `retire` is the comment "No room anywhere: it is leaked rather than freed here."
- **Cost:** every pattern of whole bars is re-sent, and a new pattern is one 4/4 bar and a cleared one a bar of the project's signature (`pattern.h:205`, `track_copy.cpp:139`), so in practice all 2048 go: 2048 allocations and 2048 `PatternData` commands for each value. Three effects follow.
  1. Leak. The audio thread applies up to 4096 commands in one `take_commands` call (`engine_host.cpp:257`) and retires one old pattern for each. `retired_patterns` holds 1023 and `limbo` 64. When more than 1087 land in one block the rest are leaked: up to 961 `Pattern` objects (240 bytes each plus their notes) for each knob value. The MIDI thread only drains the queue from inside its own loop and from `lights()`, so the whole burst landing in one block is the common case.
  2. Dropout risk. Popping 2048 commands of 200 bytes reads 410 KB of cold queue memory at the start of a block. My estimate is 0.3 to 0.6 ms of a 2.9 ms block (unmeasured).
  3. Overflow. The command queue holds 4095. Two pot values handled before the audio thread drains make 4096, so the last `PatternData` and the `Signature` command sent after the loop (`context.cpp:1782`) can be dropped (`if (!host.send(c)) delete c.data;`, `context.cpp:1229`). The audio thread then keeps a stale pattern length, or a stale bar length, until the next change. Whether two pot values can arrive inside one block I could not determine.
  To measure: turn Shift + K2 on Patterns through several signatures with `host.dropped()` and the process's resident size in the status line, and watch the audio overrun count.
- **Proposal:**
  1. Stop the leak (S): size `retired_patterns` like the command queue, `MidiQueue<4096, Pattern*>` (32 KB). Each queued command retires at most one pattern, and the MIDI thread drains before each send, so it can no longer fill.
  2. Shrink the burst (M): one command per track carrying a heap array of 128 `Pattern*`. The audio thread swaps the 128 pointers and hands the array of old ones back as one entry. `set_signature` then sends 16 commands instead of 2048; `set_speed` (`feel_jobs.cpp:46`) and `restore_motion` (`context.cpp:1573`) send one instead of up to 128.
  3. Optionally apply the signature when the pot comes to rest rather than at every value it passes.
- **Gain:** memory Medium (removes a leak of up to ~250 KB a knob value), latency Medium (removes a spike on the audio thread), correctness.
- **Sound or behaviour change:** bit-exact when nothing is dropped today; where a command is dropped today, the fix corrects the pattern or bar length that plays.
- **Risk:** Low for step 1. Medium for step 2 (a new command and hand-back path).
- **Effort:** S for step 1, M for step 2.
- **Confidence:** High on the leak (the code says so itself, and the counts follow from the queue sizes). Medium on the spike's size. Low on the overflow happening in practice: I did not check how fast the pot's values arrive.

### MODEL-3: Each pattern edit makes two deep copies and an allocation, and knob-driven note edits send one per value

- **Verification:** Confirmed. `sync_pattern` allocates `new Pattern(played_pattern(…))`, and `played_pattern` is `speed_pattern(feel_pattern(p), speed)`, two functions that each return a pattern by value (`context.cpp:1225-1231`, `pattern.cpp:293`).
- **In the fork:** No change.

- **Where:** `src/melodee/model/pattern.cpp:229`, `:261`, `:293` (`feel_pattern`, `speed_pattern`, `played_pattern`); `src/melodee/shell/context.cpp:1225` (`Context::sync_pattern`). Knob and fader callers: `step_edit.cpp:129` (the fader's nudge), `step_edit.cpp:149` (`edit_selected`: velocity, length, chance, ratchet, ramp, condition), `context.cpp:656` (Steps' cards), `context.cpp:939` (Roll's marked velocities), `feel_jobs.cpp:35` (the Feel knobs).
- **Thread and frequency:** MIDI thread, once per edit; once per knob or fader value on the callers above.
- **Now:**
  ```cpp
  Pattern feel_pattern(const Pattern& p) {
      if (p.feel.plain()) return p;                                 // pattern.cpp:230, copy 1
      Pattern out = p;                                              // :231, copies the events, then clears them
  Pattern speed_pattern(const Pattern& p, unsigned speed) {
      if (speed >= speed_count || speed == speed_normal) return p;  // :262, copy 2
  Pattern played_pattern(const Pattern& p, unsigned speed) { return speed_pattern(feel_pattern(p), speed); }
      c.data = new Pattern(played_pattern(...));                    // context.cpp:1228
  ```
  (The trailing comments are mine, and the last line's arguments are shortened to `...`.)
- **Cost:** for the usual case (plain feel, normal speed) the pattern's vectors are allocated and copied twice and the first set freed at once. A 200-note pattern is a few microseconds. A 5000-note recording is 160 KB copied and about five allocations per knob value, my estimate 60 to 100 µs each; a full 65 535-note pattern is 2 MB per value. Only recorded Motion is throttled (`sync_motion`, 40 ms). This is unmeasured; to measure, time `sync_pattern` while turning Step Edit's velocity card on a long pattern.
- **Proposal:**
  1. One copy in the plain case (S):
     ```cpp
     Pattern played_pattern(const Pattern& p, unsigned speed) {
         const bool normal = speed >= speed_count || speed == speed_normal;
         if (p.feel.plain()) return normal ? p : speed_pattern(p, speed);
         return normal ? feel_pattern(p) : speed_pattern(feel_pattern(p), speed);
     }
     ```
     and in `feel_pattern` and `speed_pattern` build `out` without first copying the vectors they clear (`events`, `motion`, `punches`).
  2. Coalesce knob-driven syncs (M): mark the pattern as waiting and send it once at the end of the MIDI thread's pass, as `sync_motion` and `flush_motion` do, keeping the order against `Recorded`, `Rebase` and `Pattern` commands.
- **Gain:** CPU Low to Medium (it matters only for long patterns); fewer allocations on the MIDI thread.
- **Sound or behaviour change:** bit-exact for step 1. Step 2 delays an edit's effect by at most one MIDI tick (5 ms).
- **Risk:** Low for step 1. Medium for step 2 (command ordering).
- **Effort:** S for step 1, M for step 2.
- **Confidence:** High on the copy count (C++17 elides only the final `new Pattern(...)`). Medium on the times. Not checked: how many values a second the pots and the fader deliver.

### MODEL-4: A parameter lock ending scans the pattern's Motion backwards from the playhead on the audio thread

- **Verification:** Confirmed. `Pattern::lane_value` walks Motion backwards from the playhead until it finds a point on the lane (`pattern.cpp:123-127`).
- **In the fork:** No change.

- **Where:** `src/melodee/engine/engine_host.cpp:1615` (`EngineHost::play_motion`), `src/melodee/model/pattern.cpp:123` (`Pattern::lane_value`).
- **Thread and frequency:** audio thread, once for each lock that ends (up to 16 at once a track, `engine_host.h:414`), typically a few every sixteenth on a track with parameter locks.
- **Now:**
  ```cpp
              int value = pattern.lane_value(at, param, pad, none);   // engine_host.cpp:1615
  int Pattern::lane_value(uint32_t tick, unsigned param, unsigned pad, int own) const {
      for (size_t n = std::min(motion_lower(tick + 1), motion.size()); n-- > 0;)
          if (motion[n].on(param, pad) && !motion[n].lock()) return motion[n].value;
      return own;
  ```
  (The first line is from `play_motion`; the marker is mine.)
- **Cost:** a lane that holds only locks (the usual way locks are used) has no plain point to stop at, so the scan walks every point before the playhead. Motion holds up to 8192 points of 12 bytes (98 KB). With three locks a step over 100 bars (about 4800 points before the playhead) my estimate is 15 to 25 µs a scan and three scans every sixteenth for each such track; with 16 locks ending on one tick in a full pattern it approaches 0.5 ms inside one 2.9 ms block. This is unmeasured; to measure, time `play_motion` with a 128-bar pattern of per-step locks.
- **Proposal:** work the answer out where the play copy is made. In `played_pattern` fill a vector parallel to `motion` (kept out of `operator==` and of the file) with, for each lock, the value its lane holds at `tick + hold`, or "none". `play_motion` stores the lock's index in `Slot::Lock` when it starts and reads that entry when it ends, falling back to `lane_value` when the pattern was swapped meanwhile (index out of range, or another lane at that index).
- **Gain:** latency Low to Medium (removes a worst-case spike on the audio thread; no change for typical short patterns).
- **Sound or behaviour change:** bit-exact.
- **Risk:** Medium: the pattern can be replaced while a lock is running, so the fallback must be right; a lock started again on the same lane must take the later point's value.
- **Effort:** M.
- **Confidence:** High on the algorithm. Low on the time. Not checked: how many locks real projects hold.

### MODEL-5: Kit lanes of their own length cost a quadratic lookup on every tick

- **Verification:** Confirmed. The tick loop calls `pattern.lane_length(l.key)` for each lane, and `lane_length` itself searches the lanes (`engine_host.cpp:2546-2547`, `pattern.h:272-275`).
- **In the fork:** Implemented. The lane's own length is used directly; identical as long as each key has one lane, which `set_lane_length` guarantees.

- **Where:** `src/melodee/engine/engine_host.cpp:2543-2552` (`EngineHost::tick`), `src/melodee/model/pattern.h:272` (`Pattern::lane_length`, `own_lane`).
- **Thread and frequency:** audio thread, every tick, for each kit track whose pattern has lanes of their own length (Euclid rhythms shorter than the pattern create them, `generators.cpp:38`).
- **Now:**
  ```cpp
          for (size_t n = pattern.lower(at); n < events.size() && events[n].tick == at; ++n)
              if (!lanes || !pattern.own_lane(events[n].key)) play_event(t, slot, pattern, n, at, length, pass, seq, holding);
          for (const auto& l : pattern.lanes) { // kit lanes of their own length (roadmap P1): each loops, and counts its passes, alone
              uint32_t loop = pattern.lane_length(l.key);
              if (loop >= length) continue;
              uint32_t lane_at = elapsed % loop;
              for (size_t n = pattern.lower(lane_at); n < events.size() && events[n].tick == lane_at; ++n)
  ```
  `lane_length(l.key)` and `own_lane()` each search `pattern.lanes` linearly (`pattern.h:272-276`), here for the entry `l` the loop already holds.
- **Cost:** with L lanes, each tick does L linear searches of the lanes, one more for every note on the tick, and L binary searches of the events. For 16 lanes that is about 400 steps, my estimate 1 to 2 µs a tick a track (0.3 to 0.6 % of a core for 16 such tracks at 120 BPM); it grows with L squared, and a kit has 128 pads. This is unmeasured.
- **Proposal:** use the entry in hand, `uint32_t loop = std::max<uint32_t>(std::min(l.length, length), 1);` (the same value: keys are unique in `lanes`, `pattern.cpp:132`). Optionally keep a 256-bit mask of keys with their own lane in the play copy so `own_lane()` is one bit test.
- **Gain:** CPU Low.
- **Sound or behaviour change:** bit-exact.
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** High on the code. Low on the time.

# Appendices

## Checked and found fine

### Runtime, drivers, power and build

- `UsbAudioOutput::push` does nothing while no computer records: one atomic load, no conversion, no ring write (`usb_audio.cpp:230`).
- The display sends nothing over SPI for an unchanged frame, and its worker blocks on a condition variable (`display.cpp:98, 119`); the damage planner already limits pushes to changed rectangles (`display_damage.h`).
- MIDI input is event-driven (`ppoll` on the raw MIDI descriptors with kernel time stamps, `hardware_midi.cpp:118-130`); only the 5 ms tick is periodic (BASE-7).
- Jack detection subscribes to control events and re-reads the two jacks only on an event (`jacks.cpp:53-62`); sd-bus uses signal matches, with `GetManagedObjects` only on demand (`system_services.cpp:49-55, 89`). Their per-pass cost is the non-blocking read counted in BASE-6.
- The audio device is opened as `hw:` in S32 with no plug or conversion layer (`audio.cpp:44-48, 63`).
- Audio loop per block (`audio.cpp:117-143`): two 1 KB fills, two peak scans, 3–5 syscalls. The peak scan is scalar double precision over 256 samples, twice (`block_peak`, lines 18-22): about 1–2 µs a block by my estimate, 0.05 % of a block. An integer max-abs would be exact and vectorisable, but the gain is negligible. The non-blocking write often writes part of a block and then waits (the queue sits between 3 and 4 periods); that is one extra `ioctl` a block, also negligible.
- On xrun (`audio.cpp:97-104, 139-141`) the stream is prepared again and the next three blocks render back to back to refill the start threshold; no unbounded recovery loop.
- `MidiPacket` is 272 bytes and is copied three or four times per message (`midi.h:12-16`, `hardware_midi.cpp:153`, `controller.cpp:188`): at MIDI rates this is well under 1 MB/s.
- `midi.read()` takes a `std::function` built from a three-reference lambda on every pass (`controller.cpp:125`), which allocates; about 200 small allocations a second from the thread's own cache, no measurable cost.
- `Session` packs flags read by every thread beside counters the audio thread writes each block (`session.h:25-38`): some false sharing, a few hundred cache-line transfers a second, negligible.
- The base writes nothing to the card or eMMC periodically: its state files are in `/run`, screenshots are on request only (`run_app.cpp:149`).
- Screen Mirror makes a picture only when the screen changed (`screen_mirror.cpp:465-471`) and is off unless the app asks for it.
- `CardShare::poll` reads configfs four times a second (`card_share.cpp:96-109`): about 60 syscalls a second, small; folded into BASE-6.
- The release binary is stripped (`release.sh:9`, `tools/make_release.py:211`).
- `sd-boot.sh` returns immediately (`systemd-run --no-block`, line 12); nothing of the ~100 MB package but the binary and two scripts goes to RAM.
- Stopped and left running: the unit stops only `ac50.service` (`sd-worker.sh:34`); the app also stops `az0x-webserver.service` for port 80 (`melodee_app.cpp:48-56`) and `restore-stock.sh:117` starts it again. Every other stock service is left as it was.
- The panel driver is the stock mipi-dbi path with a different address mode; it adds no periodic work (`sample_base_panel.c`).

### Audio engine: the render graph

- Workers do not spin between blocks: they sleep on a futex until `run()` or `wake()` (`render_pool.cpp:154-160`); the roadmap records the measurement (`docs/ROADMAP.md:178-183`).
- The audio thread's wait at the end of a run (`render_pool.cpp:122`, `relax()` loop) is usually short: it claims straight after storing the ticket, so it most often takes the first (heaviest) job itself; a worker already spinning can win that race.
- Denormals: FZ on the audio thread and every worker (see answer 4); a print host's thread gets it through `render()`.
- No lock, allocation, logging or file access on the host's render path; queues are single-producer rings (`base/src/surface/midi.h:132-158`); effect-memory clears are sliced (`effects.cpp:624-627`); project loads are parked (`engine_host.cpp:2631-2641`).
- Engine-less tracks cost only their buffer clears and a meter store; sends at zero and pad sends not fed are skipped (`engine_host.cpp:2711-2712`, `:2992`, `:3001`); buses without a chain are not jobs (`:2926`).
- Looper off: `mix_loops` and `keep_sources` return at once (`engine_host.cpp:3375`, `:3405`); `SyncOut::run` returns when stopped (`midi_out.h:138`); modulators only with a target (`:2703`); stems to the card only during a mixdown (`:2185`); `Bounce::process` returns when idle (`bounce.h:76`).
- Audio Recall's ring: 128 frames copied a block into a ring made at start (`recall_ring.h:31-40`); about 1 µs.
- `std::fill`/`std::copy` on float ranges become `memset`/`memmove`; they are not the scalar loops of HOST-4.
- The arp clock runs while stopped (`engine_host.cpp:2755`) but only 96 times a quarter note and does nothing without held keys.
- `web_service` publishes its facts every 5 ms pass (`melodee_app.cpp:766`: a mutex and four short string copies); negligible, and the condition variable has a waiter only during an upload.
- `read_machine` reads `/proc/stat` twice a second and the card's space every five seconds (`melodee_app.cpp:480-504`); negligible.
- The binary runs from a RAM folder (`base/install/sd-worker.sh:17-23`), so its code pages cannot be re-read from the card under the audio thread.

### Audio engine: sampler voices, streaming and the Looper

- Denormals: the audio thread and the render workers set FPCR's flush-to-zero (`engine_host.cpp:2622-2623`, `render_pool.cpp:136`), so the pad and kit filters' states and the envelopes' tails reach exact 0. The envelope also snaps to Idle under 1e-4 (`dsp.h:169`). No recursive DSP runs on the pager's or the service thread.
- A voice whose envelope reaches Idle is freed on that frame (`sampler.cpp:989`, `1135`, `1543`, `1577`, `1732`); a choke fades over 64 frames (1727); a muted pad starts nothing (630). The one gap is SMP-6.
- The pad filter is a two-stage state-variable filter; its `exp2` and `tan` run every 16 frames, not per sample (`sampler.cpp:912-916`). The kit filter's coefficients are worked out only in `set_param()` (24-32). Envelope coefficients (`std::exp`) are worked out in `aim()`, at a note or a change, never per sample. A filter that is Off costs one test (909; hoisted in `run_plain`, 967).
- The short loops for plain runs exist already (`plain_run`/`run_plain` 949-993, `zone_plain`/`zone_run_plain` 1507-1546); SMP-9 builds on them.
- Tones: its window comes from a table (`tones_table`, 1141), its fast reads are a hand-written NEON kernel (`band_read`, 1359-1368), and its searches are spread over blocks by voice (`prepare_at`, 373 and 1044). `align()`'s fine pass could hoist `std::lround(way * k * gap)` out of its candidate loop (1219) bit for bit, but that is about 40 µs every 30 ms a voice.
- Layers, Follow and Cloud cost nothing per block when off: layers are picked at the hit (572-617), stretch runs only for a voice with `v.stretch`, and with no cloud only the 48-grain scan remains (SMP-5).
- A pad's chain has an idle gate: its lane goes after 1.5 s of quiet or 30 s of ringing, a lane for sends alone at once (`sampler.cpp:1759-1767`).
- A note on: at most four linear passes over the 40 voices (`free_voice`, the mute group, mono, a looped one shot) and about fifteen libm calls in `aim()`; per event, not per block.
- Streaming never waits on the card on the audio thread: a voice short of pinned frames ducks, goes silent and keeps time (`admit`, `pass`, 1415-1477). Messages go through a single-producer queue under a short spin flag (`stream_pager.h:136-142`).
- The pager reads one chunk (16,384 frames: 32-131 kB of file, 64 kB a side as floats) per stream per pass, two chunks times the speed ahead, one kept behind (`stream_pager.cpp:393-411`); reads are plain `pread` through the page cache (146). A stream pins about four chunks (256-512 kB); chunks no longer pinned stay up to a 200 MB budget, the longest idle going first (`stream_pager.h:36`, `trim()` 75-89).
- Sample storage: floats at the file's own rate, left and right apart, never converted at playback (`sample_data.h:17-26`); a rate difference is folded into the voice's step (`sampler.cpp:178-180`). A WAV over 10 s is streamed, not loaded (`shell/sample_memory.h:21`).
- `TakeRecorder::process` while idle: a 128-frame peak scan and three atomic stores (`take_recorder.h:84-94`); `std::pow` once a block at 48 and 96 kHz (88), about 0.1 µs.
- `PrintPlayer` and `RegionPlayer` are rendered only while `sounding()` (`engine_host.cpp:2791`, `2804`); a print's inner loop is a plain add (`print_player.cpp:173-175`). `RegionPlayer::align` is coarse-then-fine and about 0.1 ms a window, as its comment says (`audio_player.cpp:123-160`).
- `SyncOut::run` returns at once when the song is not going (`midi_out.h:138`). `arp.h` and `note_repeat.h` are 16-entry arrays worked on per event. `external_notes.cpp` polls every 5 ms only while the auto sampler runs. `print_render.cpp` is offline. `keygroup.cpp` is a load-time reader.
- Compiler flags will not help this area much. GCC 12's `-O2` does run the vectoriser, but with the very-cheap cost model (no run-time alias checks, no scalar remainder), so loops over `frames` with separate `float*` are left alone; and the hot loops here are recursive (filters, envelopes), indexed reads (interpolation) or float sums that need `-fassociative-math`. The few plain loops that would gain from `-O3` or `-ftree-vectorize`: the lane sum at `sampler.cpp:1744-1747` once its peak is taken in a loop of its own, `print_player.cpp:174`, `wav_frames` (`sample_data.cpp:90-91`), and the punch history copy (SMP-3, better as `memcpy`). Check with `-fopt-info-vec-missed`.

### Audio engine: effects and analysis

- The per-kind `switch` is settled at compile time through `frames_of<Kind>` and `extra_frames_of<Kind>` (`effects.cpp:337-339, 638-668`; `effects_extra.cpp:176, 417-432`); there is no dispatch per frame.
- Filter and reverb coefficients are worked out in `update()` on a parameter change, not per sample (`effects.cpp:129-240`, `effects_extra.cpp:80-172`). Phaser, Pedal phaser and Auto-wah retune every 16 or 8 frames (`effects.cpp:414, 484`, `effects_extra.cpp:334`).
- Room, Reverb and Plate run one delay line at a time over the block (`effects.cpp:280-323`); the combs and allpasses wrap by compare, not modulo.
- `dsp::soft_clip` is a rational curve with one division, not `tanh` (`dsp.h:70-73`).
- Nothing allocates in `process()`; a dirty pool block is cleared a slice per call (`effects.cpp:624-627`) and scrubbed off the audio thread on a project load (`engine_host.cpp:133-136`).
- The modulo operators on delay indices are all by compile-time constants (`DelayPool::half`, `pre_length`, `shift_room`), so they compile to a multiply and shift, not a hardware division. `tap()`'s `% length` (`effects_extra.cpp:27`) is constant only if the function is inlined; a compare-and-subtract would be bit-exact and never divide.
- Modulators write an effect parameter only when its integer value changes (`engine_host.cpp:1449`). `restore()` calls `set_param` 36 times per chain, each running `update()` (`engine_host.cpp:1572-1573`), but only at a pattern's first tick after Motion moved the chain; `set_params` (`effects.cpp:103`) would do it in six.
- Denormals: flush-to-zero is set on the audio thread and the workers (answer 4).
- Master compressor metering and its off state (answer 6).
- `fft.h` (answer 7).
- Kit-pad chains, Knob FX, macro effects and hosted plug-ins already rest when quiet (FX-1).
- Not a performance matter: the reverb's `write` only ever counts up (`effects.cpp:322, 362, 368`) and is used as `write % pre_length`; when it wraps at 2^32 frames (27 hours of running at 44.1 kHz) the predelay position jumps by 4,096 frames once.

### Plug-in hosting

- A sleeping plug-in costs almost nothing: `plugin_rack.cpp:289–295` returns before any buffer work; `IdleGate::loud` is a short scan (`idle_gate.h:72`).
- No allocation or rack lock on the audio path: `memory`, `midi_in.ensureSize(8192)`, `pending[512]` (`plugin_rack.cpp:239–241`, `:395–399`); CLAP's 1024 events (`clap_instance.cpp:357`); VST2's 512.
- `juce::AudioBuffer` over existing memory allocates only from 32 channels up (`juce_AudioSampleBuffer.h:1214`); `PluginHandle` uses the main buses only.
- Buffer copies are at most four of 1 KB per block per plug-in (`plugin_rack.cpp:311`, `:331`; `clap_instance.cpp:603`, `:631`; `vst2_instance.cpp:300`, `:308`). Not worth removing.
- The bridge's plug-in works straight in the shared audio arrays (`vst2_bridge32.cpp:125–129`); no copy on that side.
- `PolyphaseResampler`: weights computed once, NEON FMA dot product, no per-frame transcendental maths (`polyphase.h:52–64`, `:95–104`). The bridge runs only for Open MPC plug-ins off 44.1 kHz.
- CLAP parameter sync is one atomic exchange per block when idle (`clap_instance.cpp:514`); CLAP `idle()` does work only when flagged (`:409–415`); the audio path never takes `mainLock`.
- CLAP preset discovery re-reads only files whose size or time changed (`clap_instance.cpp:922–931`) and rewrites its cache only then (`:953`).
- `PluginProgramList` is shared, not copied, so the per-frame `programs()` for PRESETS is cheap (`plugin_panel.h:59–71`).
- `posix_spawn`, not `fork`, starts the bridge (`vst2_bridge_instance.cpp:120–125`).
- A new plug-in is warmed with 8 silent blocks on the rack thread before the audio thread sees it (`plugin_rack.cpp:272–282`, `:1069`).
- No editor is ever opened or idled for any format; the GUI stubs are empty functions (`make_stubs.py:74`).
- Deep freeze unloads plug-ins and returns the heap (`plugin_sleep.h:53–88`, `plugin_rack.cpp:1229`).
- A print host's rack is made per print job, not kept beside the live one (`print_engine.cpp:156`; its teardown not read).
- `nam.cpp` runs only at load and in the 20 Hz card refresh (a string compare per card).
- The picker's `view()` is deep-copied for each screen frame while the picker is open (`shell.cpp:848`); small, and `changes()` is there if it ever matters.

### Shell: service work, saving, settings

- No shell lock on the audio thread: `MelodeeApp::render` reads two atomics and calls the host (`melodee_app.cpp:135-139`).
- Undo keeps parts, not projects, and a pot's turns are one edit, not one per tick (`undo_restore.cpp:85-93`, `history.h:139-169`).
- `SlotTable` keeps the per-frame copy of the sample table proportional to the samples in use and allocation-free once warm (`model/slot_table.h:25-33`).
- Backlight is written only when the setting changes (`melodee_app.cpp:531`).
- K1 on a setting is debounced: one settings write a second after it rests, and rows that reopen the audio device or the camera wait 500 ms (`context.cpp:1038-1049`, `sample_record.cpp:274-285`).
- Motion recording sends the audio thread at most one pattern copy per 40 ms (`context.cpp:1285-1290`).
- The crash log does nothing until a crash; `crash_log::note` per input is a clock read, an `snprintf` and a `strncpy` into a fixed ring (`crash_log.cpp:282-292`).
- The usage log writes nothing while off, and nothing while on but idle (`usage_log.h:91`).
- `autosave_due` never fires while playing, recording, printing, exporting or loading, and retries a missing card only every 10 s (`autosave.h:27-31`).
- `keep_rest_off()` is one branch unless pinning is turned on (`melodee_app.cpp:106`).
- `page_bindings` (called per frame and per lights pass) returns three plain structs through a switch; no allocation (`bindings.cpp:758-771`).
- Stream holds are recomputed every 100 ms, not every pass (`lights.cpp:334`).
- `Shell::summary()` and `MelodeeApp::status()` build long strings but only when a remote `status` command asks.
- `reload_tick` returns the last project's heap to the system once per load (`context.cpp:268`), not periodically.
- `PageEdit` on a pad hit on the default Stage page takes no state at all (`undo_scope.cpp:141-145` returns no scope for a plain pad).

### Lights, MIDI and controls

- LED output is deduplicated on the rounded values that are sent; a light never set is never sent (`base/src/surface/leds.h:125-143`).
- No light oscillates from float noise at rest: the glow is cut to zero under 0.002 (`pad_lights.cpp:121`), a flare under 0.002 is ignored (`pad_light.h:65`), a track meter under -48 dB reads zero (`pad_lights.cpp:47-50`), and meter LEDs are 5-bit masks.
- Meters: levels read every 50 ms (`meter_lights.h:21,36-41`) from lock-free atomics (`engine_host.h:258-259`); the mask is stored each tick and sent on change, at most 20 CC a second a strip (`lights.cpp:560-565`).
- Clock, MTC and CV pulses are made on the audio thread only while the song runs and the output is on (`engine/midi_out.h:137-176`); the defaults are off (`model/midi_config.h:39,42`). The MIDI thread sleeps until each event is heard (`midi_io.cpp:603-630`). At 120 BPM that is 48 wakes a second for clock and 100 for MTC. CV costs two wakes, a mutex and an ioctl a pulse (`midi_io.cpp:528-539`, `base/src/drivers/clock_io.cpp:56-63`): 8 a second at the default 2 pulses a quarter, 768 at the 192 maximum, which is what driving a GPIO from user space costs.
- With Sync Out on and no port that matches, events are still made and woken for, then dropped in `send_all` (`midi_io.cpp:438-441`); a few µs each.
- What `lights()` reads from the engine is lock-free: `playing()`, `ticks()`, `position_of()`, `meter()`, `out_level()` are atomics and `taps()` is a sequence lock (`engine_host.h:223-259`, `voice_tap.h:72`).
- Dispatch: `surface_input()` is a scan of 24 buttons or 6 controls (`base/src/surface/controls.h:46-64`); pages are a `switch` to static specs with function pointers (`pages/pages.cpp:12`); `page_bindings()` returns three 20-byte cards by value with no allocation (`bindings.cpp:758-775`). `pot()` may run it up to three times a CC (`shell.cpp:578-583`), which is cheap.
- A knob's settings write to the card waits a second after its last move (`context.cpp:820,884-899`, `sample_record.cpp:282-284`); the undo snapshot is taken once a gesture, not once a CC (`undo_restore.cpp:85-93`).
- A pad hit takes one mutex and one bounded-queue push before its note is sent. The undo step round it (`PageEdit`, `shell.cpp:692`) copies nothing on Stage and one pad's state on the kit pages; on Roll and Step Edit it copies the pattern once a press.
- The crash-log line: one `snprintf` of at most 80 characters for a button or note, none for pressure, and repeated turns of one pot reuse one ring slot (`shell.cpp:133-151`, `crash_log.cpp:282-292`).
- MIDI in: a `juce::MidiMessage` is made for each external message (`midi_io.cpp:120`); short messages need no allocation by JUCE's design (JUCE not present to check). `ClockFollower::jitter_us()` is a 48-step loop a pulse (`midi_route.cpp:89-99`).
- Stream holds are worked out at 10 Hz (`lights.cpp:334`) and plug-in card texts at 20 Hz (`plugin_jobs.cpp:300`).
- The light show sends every RGB light on every tick; it is a mode for filming, switched on by hand (`lights.cpp:510`).

### Storage, loading and background jobs

- Load pool: three threads, blocked on a condition variable with no timeout, nice 5 (`load_pool.cpp:48-52`).
- Card warm: one pass per card and per project load, no timer of its own, stops when asked (`card_warm.cpp:163-176`, `melodee_app.cpp:556-559`).
- Stems: a child process at nice 19 with three threads, model freed on exit, built for the Cortex-A55 (`stems_engine.c:249-262,275`, `tools/stems/build.py:37-38`); its watcher polls at 100 ms only while a job runs (`stems.cpp:172`).
- Print engine thread: blocked on a condition variable, SCHED_IDLE (`print_engine.cpp:316-322`).
- Samples stay at their file's rate; nothing is resampled at load (`engine/sample_data.h:20-26`, `sample_library.cpp:1074`).
- One file, one slot: a path already in the project is not loaded again (`context.cpp:1823`).
- A stale load is dropped by slot serial without waiting (`sample_library.cpp:989,1096`).
- Audio Recall: a plain copy of each block, no conversion or analysis (`engine/recall_ring.h:31-40`).
- Note capture ring: written per note played, 192 KB, MIDI thread only (`capture_ring.h:28-45`).
- Take and render drains do work only while a take or a render runs (`sample_library.cpp:807-814`, `sample_capture.cpp:48-52,110`).
- Project file: sparse tagged sections, no CRC or compression pass; written to `.new` and renamed (`model/project_file.cpp:233-242`, `project_store.cpp:396-402`).
- `ProjectStore::take_loaded` and `take_note` are lock-free queue pops (`project_store.cpp:103-106,191-194`).
- A changed chop is patched into its WAV in place; the audio is never copied (`mpc_wav_slices.cpp:196-260`). The comment at `chop_mode.cpp:196` ("a file is copied whole") no longer describes it.
- Usage log: off by default, batched every 10 s or 4 KB (`usage_log.h:37-38,91`).
- Held chunks of streamed samples are recomputed 10 times a second on the MIDI thread, one walk over the kit pads (`lights.cpp:334-352`, `stream_holds.h:37-62`): about 10 us by estimate.
- Web handlers run only on a request; downloads use `sendfile` (`web_server.cpp:89`); an upload is fsynced once at its end (`web_app.cpp:41`); the page polls the device only while it is open in a browser.
- The 1 ms `sleep_for` loops in `release_instruments()` are bounded (500 turns) and run only while the card is being lent (`sample_library_instruments.cpp:572-576,587-591`).

### Screen: the frame loop and drawing framework

- Typefaces load once a face and weight; `juce::Font`, widths and layouts are cached by plain keys: `src/melodee/ui/paint.cpp:18-34`, `:121-154`.
- Icon shapes are parsed from their SVG strings once: `src/melodee/ui/glyphs.cpp:171-178`.
- No `ColourGradient`, `DropShadow`, transparency layer, `drawImage` or image rescaling in `pages/` or `ui/` except the boot animation's one unscaled `drawImageAt` (grep).
- The frame's `Snapshot` is allocated once (`melodee_app.cpp:61`); its vectors and strings reuse their capacity.
- `SlotTable`'s copy allocates nothing when repeated: `src/melodee/model/slot_table.h:25-33`. The chops table is moved aside so it is not copied: `shell.cpp:811-815`.
- Project, plug-in and file lists are only copied on their own pages: `shell.cpp:847-849`, `:874`.
- `use_palette()` returns at once when the palette is unchanged: `src/melodee/ui/tokens.h:27-29`.
- The place bar moves in whole pixels: `src/melodee/pages/place_bar.cpp:48-59`.
- `Display::present()` returns without waiting for an identical frame; only damaged clips are pushed, from a worker thread: `base/src/drivers/display.cpp:98`, `:133-166`.
- Screen Mirror off: per frame one atomic load in `screen_mirror()`, one in `wants_frame()`, an early return in `offer()` before any pixel is touched, a short lock and nine atomic stores for the status: `run_app.cpp:134`, `:146-148`, `base/src/drivers/screen_mirror.cpp:564-567`. Its worker thread waits in `poll` with a 200 ms timeout (`screen_mirror.cpp:534`).
- The light show drives lights only; nothing in `pages/` or `ui/` reads `ui.light_show`.
- Shared code has no page transitions. The only eased motions are the new-track carousel (`ui/widgets.cpp:187-228`) and Chop's `eased_view` (page code).
- `ui::sheet()` shapes its body text uncached every call (`ui/widgets.cpp:94-97`) but no page calls it.
- `page_bindings()` runs four to six times a frame (`shell.cpp:806`, `common.cpp:146-147`, `:195`); it is a switch with short loops and not worth caching.

### Screen: individual pages

- **Waveform overviews are not recomputed.** Every page draws from `LoadedSample::overview` (512 peaks made at load) through `ui::waveform`, about 92 plain rectangles for Stage's width, twice for stereo (`ui/waveform.cpp:36-62`). Cost does not grow with the sample's length. Only the zoomed view is recomputed (PAGE-5).
- **Pads' thumbnails** come from the overview, 16 x 32 columns (`sample_library.cpp:108-123`); the Looper's and Record's waveforms are arrays of `std::atomic<uint8_t>` the audio thread fills (`engine/looper.h:178, 248`, `take_recorder.h:130`); an audio track's take uses a shared peaks table (`common.cpp:114-131`, `model/audio_track.h:118-123`).
- **Lists are virtualised.** Samples, Sounds, Plug-ins, Presets and Project build `ListRow`s for six rows only (`samples_page.cpp:72`, `sounds_page.cpp:38`, `plugins_page.cpp:71`, `plugin_presets_page.cpp:54`, `project_page.cpp:175`). Directory listings are cached and scanned off the screen thread. What is wasteful is the hand-over (PAGE-6).
- **No file-system call and no lock in any draw function under `pages/`** (grep for `std::filesystem`, `stat`, `fopen`, `ifstream`, `lock_guard`, `mutex`). The four `std::filesystem::path` uses only split strings (`samples_page.cpp:38`, `plugins_page.cpp:53`, `project_page.cpp:50, 63`).
- **Step Edit** draws one bar of 16 steps with binary searches into the events (`step_edit_page.cpp:96-167`, `model/pattern.cpp:323-331`); cost does not grow with the pattern.
- **Roll's note grid** is culled to 16 columns and 12 rows, has no text per note, and a rounded rectangle only per visible note (`roll_page.cpp:135-173`).
- **Patterns, Scenes and Song's clips** read `PatternSketch` (64 bytes of first notes a pattern), never the patterns (`shell/snapshot.h:23-32`); 16 cells a page.
- **Song** skips clips, markers, tempo points and punches out of view (`song_page.cpp:57, 161, 189, 262`), apart from PAGE-7.
- **Meters cross from the audio thread as single atomics** with the peak hold and fall done there (`engine_host.h:174-177, 249`, `take_recorder.h:83-91`); the page reads one float. The Compressor's trail is 80 and 154 columns advanced in the snapshot (`shell/comp_trail.h`). Voice heads come from the engine's taps, at most a handful a frame.
- **The place bar is drawn in whole pixels** on purpose (`place_bar.cpp:48-57`).
- **Blink timers on Patterns, Scenes and the Seq sheet** (`patterns_page.cpp:50`, `scenes_page.cpp:64`, `seq_sheet.cpp:43`) only show for a queued pattern, which exists only while playing. The Looper's armed blink (`looper_page.cpp:27, 73`) ends at the next boundary.
- **Chop's view ease** ends after 180 ms (`chop_page.cpp:81-109`); Stage's new-track carousel eases for 220 ms (`shell/new_track.h:56-63`) and Keys shows a progression's name for 1.5 s (`keys_page.cpp:38-40`).
- **Stage (sample and synth pages), Edit, Chain, Sends, Keys, Fader, Knob FX, Tempo** hold still when stopped and silent. Apart from the rows in PAGE-1, what they read live is the voice heads while a pad sounds, a MIDI track's route for four seconds after a note goes out (`common.cpp:805-832`), and the track meter on Stage MIX and SENDS, which is clamped to nothing below -48 dB (`stage_page.cpp:227-230, 288-290`).
- **`UiState ui = s.ui;` in Roll's draw** (`roll_page.cpp:53`) is a copy of a plain struct, not worth changing.
- **Small per-frame allocations are everywhere** (`Header`'s two vectors, `juce::String` labels, `name_case`, `pad_name`'s scan of 128 pads) but they are small beside rasterising, and text shaping is cached by string (`ui/paint.cpp:121-154`).
- **Stage's filter curve, envelope and LFO pictures** (121 to 301 points of `pow`/`sin`, one path each: `sampler_common.cpp:153-176`, `stage_page.cpp:198-217`) are tens of microseconds.

### Project model and sequencing

- Pattern storage is sparse: `std::vector` per pattern, a 240-byte header (`model/pattern.h:204`).
- The audio thread never reads the shell's `Project` or `PatternBank`; it swaps pointers and never allocates or frees (`engine_host.cpp:875-888`, `:246`).
- The sequencer does nothing while stopped and one binary search a track a tick while playing (`engine_host.cpp:2742-2746`, `:2544`).
- Trig conditions are O(1) a note (`model/trig.h:28`, `engine_host.cpp:2000`).
- Recorded Motion reaches the audio thread at most every 40 ms (`shell/context.h:215`, `context.cpp:1257`).
- Undo keeps only the part that changed and takes one snapshot a pot gesture, not one a value (`shell/undo_restore.cpp:85-93`, `history.h:173`: 32 deep).
- `SlotTable` keeps only written blocks, and a repeated copy allocates nothing (`model/slot_table.h:25-33`).
- A pattern's start restores kit pads only if Motion moved them (`engine/sampler.h:74`).
- Modulators are skipped when none is active and cost a few transcendental calls a block when active (`engine_host.cpp:1415-1454`, `:2703`); the per-track mod matrix is never evaluated (`engine/engine.h:45`).
- The tempo lane is a linear scan of at most 64 points per tick in song mode only (`model/tempo_lane.h:55`, `engine_host.cpp:2507`). `bars_at()` (about 80 passes over the lane, with a `log` for each ramp) runs on the audio thread only at an MTC locate (`engine_host.cpp:1850`).
- Song mode: `clip_at` is linear over at most 256 clips, 16 times a bar (`engine_host.cpp:1819`).
- A follow action reads all 128 of a track's play copies (`engine_host.cpp:1792`), once per action.
- `chain_tick`, `follow_tick` and `free_tick` walk the 16 tracks on each MIDI tick (`chain_jobs.cpp:48`, `scene_jobs.cpp:175`, `context.cpp:1737`): a few dozen cache lines every 5 ms.
- `live_erase_tick` runs a `remove_if` over the pattern each MIDI tick, only while Erase and a pad are held during play (`erase_copy.cpp:213`).
- Generators, variations, groove extraction, note edits, key and step copy, the keygroup migration and `flatten_lane` run on a button press, a knob value or a load (`generators.cpp`, `note_edits.cpp`, `note_copy.h`, `keygroup_track.h`, `song_flatten.h`).
- Scales, chords and progressions: `static const` tables and loops of at most 12 entries; nothing on the audio thread (`model/scales.h`, `chords.h`).
- Track copy, move and clear copy one `TrackState` (63 KB) and send one `TrackPack`, per button press (`track_copy.cpp:38-51`).
- A print job copies the whole `Project` and `PatternBank` per pattern printed and blanks the rest (`print_engine.cpp:532`, `model/print.cpp:33`): on the print worker, small beside the render itself.
- `sync_song()` re-sends every clip and tempo point (up to 323 commands) on each song edit (`context.cpp:2068`): about 65 KB of queue traffic an edit.
- `Command` is 200 bytes because it carries a `PadSound` by value (`engine/command.h:174`), and the ring is 819 KB; at the rate of hands on controls the copies are negligible. It matters only in MODEL-2's burst.
- Tick arithmetic: no overflow or drift found (see answer 8).

## Open questions that need the device

### Runtime, drivers, power and build

- **All absolute numbers.** Draw time (`screen_draw_us`), per-thread CPU and wakeups, and supply current need the device. Recipe: name the threads, then sample `/proc/<pid>/task/*/status` (`voluntary_ctxt_switches`) and `/proc/<pid>/task/*/stat` over 10 s; power from a bench supply or an inline meter.
- **cpufreq on the vendor kernel:** whether `scaling_max_freq` is writable as expected, how long a voltage step takes, whether 1416 MHz carries a heavy project, and whether `CONFIG_UCLAMP_TASK` is set (it decides whether `schedutil` could ever be made usable with real-time threads).
- **Heartbeat LED:** nothing in the repository touches `/sys/class/leds`; whether the device tree defines an LED with a heartbeat trigger needs `ls /sys/class/leds; cat /sys/class/leds/*/trigger` on the device.
- **Wi-Fi and Bluetooth:** nothing in the repository touches the radios, rfkill or their services, and HARDWARE.md:60 says the base makes no radio policy changes. Whether the stock image loads the BCM43456 drivers or runs a supplicant or Bluetooth daemon after `ac50.service` stops needs `rfkill list`, `lsmod`, and `systemctl list-units --state=running` on the device (the binary's `--hardware` mode already lists `/sys/class/net` states, `platform.cpp:170-175`). Whether the web page is meant to be reachable over Wi-Fi decides if they may be blocked.
- **Backlight and LED power:** how much levels 6, 1 and 0 and the pad LEDs draw, and whether level 0 switches the LED driver off.
- **USB gadget rate notification:** whether the vendor's `u_audio.c` notifies the rate controls (BASE-9, step 2).
- **Boot timing:** the binary's size, the speed of the device's `sha256sum`, the self-test's duration, how long `ac50.service` takes to stop, and `daemon-reload`'s cost.
- **Control surface chatter:** whether the microcontroller sends anything when untouched; if `hardware_events` grows on its own, it defeats the idle screen rate the way BASE-4 describes.
- **Interrupt-thread interference:** whether USB networking traffic or card I/O causes xruns with audio at FIFO 30 under IRQ threads at 50 (BASE-8).
- **SPI clock:** the base's panel module could raise `spi->max_speed_hz` before `mipi_dbi_spi_init()` to shorten the 54 ms full-screen push; whether the panel and its cable tolerate more than 25 MHz is a hardware test. No battery effect.
- **Taking cores offline** for light projects would remove their leakage, but CPU hotplug stalls the system briefly; untested and probably not worth the dropout risk.
- **Where the journal goes:** the runtime prints only on state changes, but whether journald stores to eMMC or RAM on this image is unknown.

### Audio engine: the render graph

- All times. On the device: `status` (`melodee_render_load`, `melodee_render_parallel`), `melodee-freeze status` (`track_load=`), About's core loads, `melodee-render serial|parallel`, and the bench. The bench needs a stopped mode and the stems/input/USB arguments to cover HOST-1, HOST-7 and HOST-8.
- Whether the loops of HOST-4 are vectorised by the project's real compiler (`-fopt-info-vec-missed` answers it in one compile).
- Whether `clock_gettime(CLOCK_MONOTONIC)` is served by the vDSO on this kernel. If it is a real system call, the 68 reads a block of HOST-7 cost about 1 % of a block instead of 0.1 %.
- How long `TrackLoad` (`engine_host.cpp:559-579`, `load_track` at `:343-392`: 128 pads set, the chain and any pad effects rebuilt) holds the audio thread on a track paste or undo.
- The idle cost of `SamplerEngine::render_stereo` with no voice, and whether it can report "nothing written this block" for HOST-1 step 1 (sampler auditor).
- Whether the 32-bit VST2 bridge process should set FZ in FPSCR (plug-in auditor); and the size of the subnormal penalty on the A55 in either mode.
- The power the ADC path draws by staying routed for Audio Recall (`shell/input_route.h:19`); a Settings switch for Audio Recall could let it power down. Hardware measurement needed.
- Whether rarely used shared-library pages on the eMMC (libm's `tanhf`, used only above -1 dBFS) can be evicted and fault on the audio thread; there is no `mlockall` in `base/src` or `src`.
- Cache sizes of the RK3566 (I assumed 32 kB L1D per core and a small shared L3 from memory, not from a source in the repository).

### Audio engine: sampler voices, streaming and the Looper

- The real cost of the pager's 1 ms wake (SMP-1) and of `LoopStretch::best` (SMP-2) on the device. For the second, `loop_ns` is already there to read.
- This SoC's L2 and L3 sizes, on which the idle-scan estimates in SMP-3 and SMP-5 lean.
- Whether GCC inlines `voice_out` into `run_plain` (five call sites, a large body: probably not, a call a frame for a filtered voice); look at the object code.
- How common envelopes with SUSTAIN at 0 are in users' kits and imported SFZ or keygroup programs (SMP-6).
- The pager runs at FIFO 20, above the MIDI thread (15): a chunk's read and conversion, `mlock`, `madvise`, and the `std::map`/`std::list` nodes allocated in `now_idle()` (`stream_pager.cpp:53-57`) all run ahead of pad and LED handling. Whether a chord on a many-layered streamed instrument delays the MIDI thread audibly needs a measurement.
- A part read in place is held twice for a while: the file's pages in the page cache and the float copy in the part's own memory. The page cache gives way first under pressure; whether `posix_fadvise(POSIX_FADV_DONTNEED)` after a chunk is worth it depends on how tight memory gets.
- `engine/recall_ring.h` and the `audio_in` take write every block (`engine_host.cpp:3134`, `3148`); neither was on my list and I did not read them.
- Whether the host could skip a silent kit altogether is the host auditor's question; SMP-5 item 3 says what the sampler would have to report first.

### Audio engine: effects and analysis

- **Real cost per effect on the device.** `--melodee-dsp-bench` times whole blocks of one busy project. That project uses 15 of the 41 kinds (Chorus, Drive, Phaser, Filter, Crush, Echo, Compressor, Width, Flanger, Tape, Tape echo, Ping pong, Reverb, Plate, Shimmer) and none of the thirteen newer ones (`effects_extra.cpp`). A loop over every kind that calls `Effect::process` on noise and on silence and prints nanoseconds per frame would replace every estimate in this report; `Effect` is a public class, so it is about 30 lines beside `dsp_bench`.
- **How much of a busy block is effects.** `docs/ROADMAP.md:763-767` records the bench at 48 kHz as a mean block of 1,376 to 1,487 µs and 187 to 192 % CPU. My estimates put the effects at roughly 5 to 10 % of that CPU time, with the sampler voices (and the workers' waiting) taking most of the rest; that split is not measured.
- **Whether a hosted plug-in can clear flush-to-zero.** The flag is set once per thread. If a plug-in's process call restores a floating-point state without it, nothing sets it again. Re-asserting it at the top of `render()` costs two instructions.
- **Cache sizes and the cost of the memory traffic in FX-11.** I recall Rockchip's data sheets giving 32 kB of L1 data cache per core and one shared 512 kB L3; I did not verify it, and the cost of cold cache lines needs `perf stat` on the device.
- **Which maths library the device runs.** My call costs assume glibc 2.36 as in the Debian bookworm build container.
- **What g++ actually emits** for the points in FX-4, FX-7 and FX-9 and for answer 5 (no cross-compiler here; nothing was built).
- **The Pad FX and Flex Beat DSP** (`punch.cpp`, `punch_mpc.cpp`) were not audited. From the header and the first lines of `Punch::process`: each `Punch` holds two 192,000-float history lines (1.5 MB) and writes them every block also while off (`punch.cpp:107`), and `flex.process` runs on the master every block (`engine_host.cpp:3118`).
- **Sample analysis time on the device:** read it from the `load:` line the app prints (FX-5).

### Plug-in hosting

- Real spin and wake times on the device (PLUG-2), and how long real armv7 plug-ins take in `setProgram`/`getChunk` (PLUG-3).
- Wakeups from threads that plug-ins start themselves (a JUCE-built plug-in brings its own timer thread, and its messages wake the host's message thread). Count per loaded plug-in with `/proc/<pid>/task/*/status`.
- Whether armv7 plug-ins slow down on denormals: the host sets `FPCR.FZ` on the audio thread and workers (`engine_host.cpp:18–24`, `render_pool.cpp:26–31`), the bridge never sets `FPSCR.FZ`. An instrument kept awake by playback (PLUG-1) can sit on denormal state indefinitely. Setting the bit in the bridge would be a rounding-only change.
- Neural Amp Modeler's own DSP (scalar or NEON, direct or partitioned convolution for the IR): it is in the plug-in's fork, not here. The documented 17 % for an 8192-tap IR is consistent with direct convolution.
- How long `malloc_trim(0)` (`plugin_rack.cpp:1231`) holds glibc's arena locks with a large heap, and whether the MIDI thread waits on it when a plug-in is freed.
- Sizes of real bundles' data (PLUG-7) and libraries (PLUG-6).
- For the engine-host auditor: `PluginHandle::asleep` is not visible through `PluginRunner`, and `PluginEngine::sounding()` returns true whenever a plug-in is attached (`plugin_engine.h:87`); nothing reads either. If the host ever skips a silent track's chain and mix, the gate's state would have to be exposed.

### Shell: service work, saving, settings

- How long `shell.snapshot`, the status block and a whole `Shell::lights()` pass really take on the device; the figures above are estimates from data sizes. A steady-clock pair around each, printed in `status`, would settle it.
- Sizes: `sizeof(Project)`, `sizeof(UiState)` and `sizeof(Snapshot)` are hand counts (about 0.54 MB, about 8 KB, about 0.8 MB). A `static_assert` or a printed `sizeof` would confirm.
- Whether the card is mounted with a synchronous or `flush` option. If it is, every write in question 4 reaches the card at once; if not, the kernel batches them over several seconds. `/proc/self/mountinfo` on the device shows it.
- Whether `Platform::supplies()` can tell mains from battery on this board (an `online` attribute), which decides whether auto power-off can be "battery only".
- The backlight's share of the power draw, and so the real value of dimming.
- How many projects, and of what size, real cards hold in `Melodee/Projects` (it scales SHELL-4), and how often users keep a MIDI track with an audio input (it decides how common SHELL-1's once-a-second case is).
- Whether a `Reload` resets the engine's record source so that `record_tick` sends `source_command` (and so dirties the project) after every load (`sample_record.cpp:264-267`); and whether `host.record_source()` can stay unequal for long, which would resend on every pass.
- What an empty `StreamPager::pump()` costs and what else wakes the pager (engine auditor).
- What `sd-update.sh check` costs per zip at boot (it runs `unzip` and a signature check; base, not read in detail).
- Whether `InputRoute::service` can retry `mixer.speaker()` on every pass if the call keeps failing (`input_route.cpp:33-34`: the stored state only changes on success).

### Lights, MIDI and controls

- What `Shell::lights()` and a whole pass of the MIDI loop cost on the device. It needs per-thread CPU time (`perf stat -t`, or `/proc/<pid>/task/<tid>/stat`) with and without each change.
- LED current: per drive step for the pads, for a button at Dim and at Bright, and whether the microcontroller's dimming is linear at the bottom. It decides whether LIGHT-4 is Medium or High.
- How fast the surface sends pressure for a held pad, and whether it repeats unchanged values.
- What a mid-block run boundary costs in the engine with one-dispatch blocks on, and whether the engines smooth pressure (LIGHT-9).
- The longest hold of `SampleLibrary::lock`, `PluginRack::lock` and `UpdateLink::lock` by the service and loader threads; `lights()` takes each several times a tick. `PluginRack::panel()` asks the plug-in for parameter names and value texts under the rack's lock on the MIDI thread, 20 times a second while a plug-in's cards are in view (`plugin_rack.cpp:617-650`); a bridged VST2 may make that slow.
- How long `Shell::snapshot()` holds `Shell::lock` (LIGHT-7), and how often `timed_missed()` rises in normal use.
- Whether the commands sent at start-up (`MidiIo::configure`, `record_tick`, `send_repeat_velocity()` at `lights.cpp:298`) leave an untouched project dirty and so cause one save on every start (LIGHT-6).
- What alsa-lib's enumeration costs on the device (LIGHT-10).
- Whether remote test scripts rely on `status` being fresher than 100 ms (LIGHT-1).

### Storage, loading and background jobs

- The read and analyse shares of a project load on the device. The app prints them (`load: ... threads: read X, analyse Y ...`, `sample_library.cpp:1318`); one journal line settles IO-4.
- What one gzip MPC program costs in `read_mpc_json_program` on the A55 (IO-2), and how long `ProjectStore::scan()` takes with a real folder (IO-1).
- Whether `usb0` is up with an address when no computer is attached. If it is not when the first service pass runs, mDNS never starts (`mdns.cpp:172`, `melodee_app.cpp:737-745`).
- Whether the base locks memory (`mlockall`). The recall ring and the take buffer are 66 MB, sized for 96 kHz and zero-filled at start (`engine/recall_ring.h:24,28`, `engine/take_recorder.h:34,40`). Sizing them for the device's rate would save about 36 MB at 44.1 kHz, but needs an allocation off the audio thread when the rate changes; engine code, for that auditor.
- How often the IO-8 stall occurs: it needs a trace of the MIDI thread's longest pass while a freeze prints during a project load.
- Whether `host.playing()` covers the Looper playing alone; if not, autosave (and IO-3's rewrite) can run while loops play.
- How the card is mounted (flush or sync options) and how the kernel's exFAT driver caches negative lookups: they scale the cost of IO-11 and of the `fs::exists` name searches (`free_name`, `TAKE n.wav`), which I left out as cold code.
- `plugin/plugin_rack.cpp:791` waits with a 50 ms timeout (20 wakeups a second). Outside this area; not read.

### Screen: the frame loop and drawing framework

- The real `draw_us`, and how it splits between `Shell::snapshot()`, the page, the overlays and `present()`. The status line has one number for the first three. Adding `snapshot_us` and `present_us` to `ScreenStats` would rank UI-1, UI-2, UI-9 and UI-11 by measurement.
- What JUCE's software renderer costs per glyph, per rounded rectangle and per stroked path on the A55 (JUCE is not checked out).
- Whether the control surface's microcontroller sends anything periodic that counts in `hardware_events`. If it does, the screen never idles (the same mechanism as UI-3). `status` at rest shows it: `hardware_events` should not move.
- How much CPU the kernel spends per push (RGB565 conversion, and whether the SPI controller uses DMA or interrupts at 25 MHz).
- Whether a non-PI `std::mutex` held by the screen thread delays the MIDI thread measurably in practice (UI-2); a histogram of the wait in `Shell::lights()` would show it.
- Cache sizes on the RK3566, so the claim in UI-2 that the copy evicts the audio threads' data is an inference from the sizes, not a measurement.
- Per-page animation and live readouts (another auditor): they decide how much UI-4 and UI-5 save on each page.

### Screen: individual pages

- **What a frame costs on the device**, and how it splits between the snapshot, text and shapes. Every estimate above rests on assumed costs for JUCE's `fillRoundedRectangle`, `strokePath` and `fillRect`. `screen_draw_us` per page would settle it.
- **Whether the input level stands still with nothing plugged in** (the ADC's noise floor against the meter's -96 dB bottom), so whether Record, Input Config and an audio track's Stage INPUT ever reach idle today.
- **Whether "CPU n %" on Tracks flips while stopped.** It depends on how much the block time jitters around an integer boundary.
- **Whether a plug-in's load reaches exactly zero when its idle gate closes** (`plugin_rack.cpp:293` multiplies by 0.95 a block; the panel shows tenths of a percent), so whether Stage of a plug-in without presets idles when stopped.
- **The longest sample kept in memory** before the library streams it, which sets the worst case of PAGE-5.
- **`frame()`, `cards_of()` and the chrome every page calls** (`common.cpp`, `ui/chrome.cpp`, `ui/knobs.cpp`): shared by all pages and probably a large part of each frame; another auditor has them.
- **The frame loop's policy.** Several elements change at a low rate (a pulse at 4 Hz, the step chase at 8 Hz at 120 BPM, a load readout at 20 Hz, the place bar by pixels), yet any change buys 500 ms at 60 fps. A draw rate that follows the rate of change would cut more than any single page fix here; it is outside `pages/`.

### Project model and sequencing

- The real time of `Shell::snapshot()` on the device, the frame rate while a pattern plays (the SPI push limits it), and how often the screen thread is preempted while it holds `Shell::lock`.
- How fast the pots and the fader deliver values, which decides whether MODEL-2's overflow can happen and how much MODEL-3 costs.
- Which pages read other tracks' kits from `Snapshot::project`, and whether every write to `Track::pads` is followed by `send_pad()`; both decide how MODEL-1's second part can be done.
- How many notes and locks real projects hold; MODEL-3 and MODEL-4 only matter for long, dense patterns.
- Whether the save path writes the fixed arrays whole, and what an autosave three seconds after each edit costs on the card (`model/project_file.cpp`, another auditor).
- The device's RAM size and whether the process locks its memory. A pattern copy above glibc's mmap threshold (128 KB, about 8200 notes) is first served by `mmap` and page faults on the MIDI thread.

## Other notes from the auditors

### Audio engine: the render graph: Answers to the assignment's questions

1. **A block with nothing sounding.** There is no early-out for a silent track, a silent bus, an idle chain or the master. With the transport stopped, every track that has an engine still gets: its engine's render call, the level/pan stage (128 frames), its whole effect chain, the mix loop into master/Knob FX/stem (128 frames), meters, and four clock reads. Every bus that has a chain runs it; the returns of all four buses are added (4 × 128 × 2). The master chain, ten `Punch` histories, the compressor, the input select, both meter loops, the Recall ring write and the output loop always run. What already rests: engine-less tracks, muted tracks' mix stage (not their chain), sends at zero, buses without a chain, Knob FX after its tail, kit pad chains, the Looper when off, plug-ins (their own idle gate), frozen tracks' engines, sync out, modulators with no target. See HOST-1, HOST-6, HOST-8.
   - Zeroed each block (16 tracks, no Knob FX, no sidechain, no compensation): 16 × 3 track buffers 24.6 kB (`engine_host.cpp:2708-2709`, for all 16 slots, engine or not), buses/master/knob 6.1 kB (`:2822-2824`), stems 8.2 kB (`:2954`), the Looper's stack block 4.1 kB (`looper.cpp:585`), the base's two `AudioBlock`s 2 kB: about 45 kB a block.
   - Copied each block: ten punch histories 10.2 kB, Recall ring 1 kB, S32 out 1 kB, taps 0.8 kB twice.
   - Audio Recall records always (`engine_host.cpp:3148`); its cost is small (about 2 µs with `select_input` and the input meter). Stems for USB-C are always rendered (HOST-8).
2. **Render pool.** Workers sleep in `FUTEX_WAIT_PRIVATE` on the generation word; `run()` wakes all of them with `FUTEX_WAKE … INT_MAX`. They spin (a `yield` plus a `steady_clock::now()` per turn, at most 200 µs) only when told another run follows in the same block. The audio thread works on jobs too and then spins until the last job is done. Three workers at `SCHED_FIFO` 30 (the audio thread's priority), floating over all cores by default (`render_pinned_default = false`). Jobs are one per track (engine and chain) plus the Looper, handed out one at a time by a CAS, heaviest first by last block's time; a second run takes the bus chains and Knob FX. The pool is used once the previous block's track work reached 150 µs. With fewer than two jobs it runs inline. It never spins between blocks. It does wake all three workers every block as soon as that 150 µs is reached, which a stopped project with a few effects reaches (HOST-1, HOST-2), and it can spin 3 × 200 µs for a run that never comes (HOST-3).
3. **Per-block overheads.** Listed in HOST-7; the larger ones are the scalar strip loops (HOST-4) and the punch histories (HOST-6). The host itself has no per-sample `pow/exp/sin` except the metronome blip (only while it sounds), the master compressor (HOST-9) and `tanh` on samples over -1 dBFS; a modulator costs one `exp` or one LFO value a block. Draining an empty command queue is one atomic load.
4. **Denormals.** Flush-to-zero is already set: FPCR bit 24 on the audio thread at its first block (`engine_host.cpp:2621-2624`) and on each pool worker at start (`render_pool.cpp:136`). Any other thread that calls `EngineHost::render` (the print engine's lean host) gets it through the same `thread_local` path. Not set: the plug-in rack's loader thread (eight warm-up blocks, cold), the service/loader threads, and the 32-bit VST2 bridge process (no FPSCR write anywhere under `src/`; the plug-in auditor's area). It does not change the sound (only values under about 1e-38). One gap: it is set once per thread and never checked again (HOST-10).
5. **Buffers and layout.** 32-bit float, planar (separate left/right arrays of 128), interleaved only at the S32 in/out and USB edges. All block buffers are members of `EngineHost` or `Slot` (heap, inside `MelodeeApp`, `main.cpp:25`), except each track's pad sends (heap, 4 kB) and print rows (heap); temporaries on the stack are small (Looper 4 kB, macros 2 kB). No `alignas` anywhere; NEON does not need it. No large per-block `memcpy` apart from the lists above. False sharing between workers: only `track_ns` (16 × 4 bytes, one line, written by every job twice, HOST-7) and the pool's own `ticket`/`done`/`generation` words, which share a line by design.
6. **96 kHz and the 128-frame block.** Everything counted per block doubles per second and has half the budget: housekeeping, worker wake-ups (about 70 µs each by the roadmap's own measurement, 5 % of a 96 kHz block), the 200 µs spin (15 % of a block), the fixed 150 µs threshold (11 % of a block), 17–21 `std::pow` calls a block that 44.1 kHz does not make (HOST-7), and the compensation lines' clear (HOST-5), which cannot fit in a 1.33 ms block.
7. **Blocking or allocating in `render()`.** No mutex, allocation, logging or file access in the host files (grep of `engine_host.cpp`, `render_pool.cpp`, `master_comp.cpp`, `macros.cpp`, the headers). Things that can still stall the audio thread: the compensation lines' 128 kB clears (HOST-5); `TrackLoad` (`engine_host.cpp:559-579`), which rebuilds a whole track on the audio thread (not measured); the pager's spin flag, shared only by the audio thread and its workers. Project loads are parked off the audio thread (`melodee_app.cpp:88`).
8. **`dsp_bench.cpp`.** It plays one busy project (16 kits, three hits a sixteenth with ratchets, two effects a track, four bus effects, compressor on) at FIFO 30, a block a period, 48 kHz by default, and prints mean/p50/p99/max block time, blocks over budget, process CPU %, audio-thread % and workers %. It calls `render()` without input, stems, USB or the fixed mix, so those paths are not measured, and it has no stopped or idle case and no per-stage breakdown. Results are kept only as prose in `docs/ROADMAP.md` (lines 178–197 and 761–767: mean 1376–1487 µs, p99 1498–1796 µs, 187–192 % CPU at 48 kHz); there is no results file and no figure for a stopped project.

### Audio engine: effects and analysis: Answers to the questions asked

#### 1. Inventory of the 41 effects

Classes are for one stereo frame: **cheap** under about 40 ns, **medium** 40 to 100 ns, **heavy** over 100 ns (estimates, see the top of this report). "Calls" are maths-library calls per frame; "div" counts float divisions and square roots per frame.

| Effect | Class | Per frame | Delay memory (floats used of the 288,000 in a pool block) |
|---|---|---|---|
| Room, Reverb, Plate | medium | 8 combs + 4 allpasses, block form; no calls | 53,360 reserved; about 29,000 to 33,000 cycled at 44.1 kHz |
| Shimmer | heavy | the reverb frame by frame + 2 `sinf`, 2 `fmodf`, 2 div | 62,576 reserved |
| Echo, Ping pong | medium | 2 interpolated reads, 2 one-poles, `sync_back` calls | the whole block (2 x 144,000 ring) |
| Tape echo | heavy | Echo + 1 `sinf`, 3 div | the whole block |
| Looper | cheap | 2 div while recording | 2 x LENGTH, up to the whole block |
| Chorus, Flanger | heavy | 2 `sinf`, 1 div, 2 reads | the whole block cycled; needs 2 x 2,112 at most |
| Phaser | medium | 12 allpasses; `sinf` + `exp2f` + `tanf` every 16 frames | none |
| Pedal chorus | medium | 5 div, 2 reads | half a block cycled; needs 1,296 |
| Pedal phaser | medium | 8 allpasses; `exp2f` + `tanf` every 16 frames | none |
| Auto-pan, Tremolo | medium | 1 `sinf`, 1 div (Tremolo's square shape: no call) | none |
| Drive | medium | 4 div, 1 sqrt | none |
| Tape | heavy | 1 `sinf`, 5 div, 1 sqrt, 2 reads | the whole block cycled; needs 2 x 768 |
| Crush | cheap | one 64-bit integer division | none |
| Spit | medium | 1 `exp2f`, 4 div | none |
| Fuzz | cheap | clamps and 2 one-poles | none |
| EQ | cheap | 6 biquads | none |
| Filter | cheap | 2 state-variable filters, 2 div | none |
| Formant | medium | 6 state-variable filters, 6 div | none |
| Width | cheap | 4 multiply-adds | none |
| Compressor | heavy as written (cheap under threshold after FX-2) | `log10f`, `powf`, 2 div | none |
| Gate, Transient | cheap | Transient: 2 div | none |
| Limiter | cheap | at most 1 div | none; the whole block cycled while LOOKAHEAD > 0 (needs 2 x 960) |
| Diff delay | medium | 2 reads, 8 allpass diffusers, `sync_back` calls | 202,560 |
| Sample delay | cheap | 2 reads | 48,200 |
| Spring | heavy | 24 allpasses, 4 one-poles | 65,800 |
| Pumper | heavy | 2 double div, about 6 div, `powf` during the release part | none |
| Amp sim | medium | 12 biquads, 2 div (no oversampling) | none |
| Tube drive | medium | 8 div, 2 sqrt | none |
| Soft clipper | heavy, medium or cheap by SHAPE | Tanh (the initial shape): 2 `tanhf`; Sine: 2 `sinf`; Parabolic: no call | none |
| Ensemble | heavy | 6 `sinf`, 2 div, 3 reads | 9,000 |
| Multi-chorus | heavy | per voice 1 `sinf`, 1 div, 1 read (3, 4 or 6 voices) | 9,000 |
| Auto-wah | medium | 2 div; `exp2f` + `tanf` every 8 frames | none |
| Vintage | medium | 6 div | none |
| Vinyl | medium to heavy | 1 `sinf`, 1 div, 2 reads, 3 noise draws | 18,000 |
| Tape emulator | heavy | 1 `sinf`, 5 div, one 64-bit integer division, 2 reads | 18,000 |

There is no convolution, no granular effect and no oversampling in this set (the granulator is a Pad FX in `punch_mpc.cpp`, not read).

Memory:
- **Per slot, always:** one `Effect` object of 992 bytes holds every kind's state (12 biquads, 24 allpass states and so on, `effects.h:118-169`). There are 150 chain slots, Knob FX and its input twin, 48 macro slots and 16 x 128 x 4 = 8,192 kit-pad slots (`engine_host.cpp:55`), about 8.3 MB in all, allocated up front.
- **Delay memory, shared:** one pool of 48 blocks of 288,000 floats, 55.3 MB, allocated and zeroed when the host is made (`effects.cpp:41`). A slot takes a block only when a delay-memory kind is selected (`set_kind`, `effects.cpp:77-78`) and gives it back when the kind changes to one that needs none; the Limiter takes one only while LOOKAHEAD is above 0 (`:107-111`). Every block is the same size, so a Chorus holds as much as a Looper. When the pool is empty the effect passes sound through (`effects.cpp:618`).

#### 2. Idle behaviour
- An **empty** slot ends the chain loop (`engine_host.cpp:1308`): free.
- A **bypassed** effect fades for 10 ms, then `process()` returns after a few branches (`effects.cpp:622`): free. It keeps its pool block.
- A **non-bypassed** effect never rests on its own. On silent input it still advances its delay lines (writing zeros), turns its LFOs and runs its detector. Delay lines are not re-cleared while idle; clearing happens only after a kind change, `clear()` or coming back from bypass, 16,384 floats per call (`effects.cpp:624-627`).
- Resting exists only outside `Effect`, for kit-pad chains, Knob FX, macro effects and hosted plug-ins. Track, bus, master and loop chains have none (FX-1).

#### 3. Per-sample waste
See FX-2 (logarithm and power per sample), FX-3 and FX-10 (`sin` for LFOs and windows), FX-4 (block constants recomputed), FX-8 (divisions), FX-9 (member reloads) and FX-7 (stereo as two scalar passes). On the two remaining points: every modulo on a delay index is by a compile-time constant (see "Checked and fine"), and `double` is used per sample only in Pumper's beat position (`effects_extra.cpp:249-250`, two double divisions per frame), where the song clock needs it.

#### 4. Denormals
No recursive structure has a guard of its own. The list: the reverb's eight combs with their damping one-poles and four allpasses, and Shimmer's feedback; the feedback lines and tone one-poles of Echo, Tape echo, Ping pong, Diff delay (plus its eight diffusers) and Flanger; Looper's KEEP; the allpass states and feedback of Phaser and Pedal phaser; Spring's two loops, 24 dispersion allpasses, damping and low-cut states; every biquad (EQ, Amp sim) and state-variable filter (Filter, Formant, Auto-wah, Vintage); the tone one-poles of Drive, Fuzz, Pedal chorus, Vinyl and Tape emulator; Tube drive's DC blocker; Vinyl's `pop *= fall[0]`, hiss and rumble; the envelopes and gain smoothers of Compressor, Gate, Transient, Limiter, Tremolo and Pumper; the master compressor's envelope, gain ramp and Color low-pass; `dsp::Ladder` and `dsp::Smooth`.

A global flush-to-zero covers all of them, and it is **already set**: `engine_host.cpp:18-25` sets FPCR bit 24 on the first block of any thread that calls `EngineHost::render` (`:2621-2624`, so print hosts too), and `render_pool.cpp:135-136` sets it on each render worker. It is set once per thread and never re-asserted; see "Could not determine" for hosted plug-ins.

#### 5. Vectorisation
- Almost every effect loop is a feedback recurrence (combs, allpasses, biquads, one-poles, envelope followers) and cannot be vectorised across frames whatever the flags.
- g++ 12 at `-O2` runs the vectoriser with its "very cheap" cost model. As I understand it (not checked against this build), that model refuses any loop that needs a scalar remainder or a run-time aliasing test, so loops over a run-time `frames` stay scalar. `-fopt-info-vec-missed` on `effects.cpp` would confirm it.
- Loops that could vectorise with `-O3` or a fixed 128-frame count plus `__restrict`: the dry fade while a block is being cleared (`effects.cpp:630`), the reverb's predelay copy (`:283-289`, once the three `% pre_length` are split at the wrap) and its wet/dry mix (`:346-351`), and Width (`:548-551`). The last two are blocked today by `blend()` (`effects.h:103-107`), which carries the bypass fade from frame to frame through a member; computing the mix once for the block when no fade is running (`on == aim`) removes that and is bit-exact.
- The reverb's allpass loops (`:312-318`) have no dependence shorter than their line (242 frames or more), so splitting each at its wrap point would let them vectorise. The comb loops (`:298-304`) cannot: the damping one-pole is a true recurrence.
- The larger win is across the two channels, not across frames (FX-7).

#### 6. The master compressor and metering
- The gain computer (`log10f`, `powf`) runs every fourth sample with a linear ramp between (`master_comp.cpp:75-81`). There is no look-ahead buffer; the only buffer is Color's 512-frame wobble line a side, inside the object, and its `% line` compiles to a mask because 512 is a power of two.
- Metering is cheap: per block one to three relaxed atomic stores and one `log10f` (`master_comp.cpp:52-57, 86-87`). `take_activity` is two atomic exchanges on the screen thread.
- Off, it returns at once (`:65`). On, it costs the same on silence as on signal (FX-6).

#### 7. Analysis code
- `fft.h` is an iterative, in-place radix-2 FFT on `std::complex<float>`. It does not allocate; the caller passes the twiddle table, built once per analysis (`onsets.cpp:86-87`, `root_find.cpp:140-141, 252-253`). The bit-reversal order is recomputed on each call (about 5 % of a call). `analyse` already packs two real frames into one complex FFT (`onsets.cpp:100-107`). It never runs on the audio thread.
- Onset detection and auto-chop reading run when a sample **loads**, not when Chop opens, so Chop itself is instant: `chop()`, `auto_slices()` and `pick()` work on at most 512 stored hits. The load cost is FX-5.
- `read_auto`'s two median filters (`auto_chop.cpp:119-129`) do two `nth_element` calls of 17 bytes per band per frame, perhaps 0.2 s for a minute of audio; a stride caps the work at 12,000 frames (`:116`).
- `find_root` is bounded to the first 8 s and 48 windows (`root_find.cpp:36-37`), about 0.1 to 0.15 s for a long sample, and runs once per sample ever. Its chord path recomputes the Hann window with a double `cos` per sample for each window (`root_find.cpp:151`: 4,096 calls x up to 48 windows); a table removes about 15 ms.
- `auto_probe.cpp`, `loop_find.cpp` and `release_fit.cpp` are called only from the Auto Sample job (`shell/print_engine.cpp:834-1335`). The slowest case I found is `probe_glide` (`auto_probe.cpp:91-97`): up to 500 calls of an O(window x lags) `probe_pitch`, a few seconds when the pitch never settles. It is a one-off job, so I do not rate it.
- The instrument readers (`sfz.cpp`, `sf2.cpp`, `exs.cpp`, `dspreset.cpp`) parse text or headers only, in linear passes with `std::map`; nothing there costs seconds.

### Plug-in hosting: Answers to the area questions

1. **Idle gating.** There are two separate mechanisms.
   - `idle_gate.h` (`IdleGate`) is the per-block gate. It lives in `PluginHandle` (`plugin_rack.cpp:394`), the only `PluginRunner`, so it covers VST3, LV2, CLAP, VST2 and bridged armv7 alike, instruments and effects, on track, bus, master and loop chains (all go through `run_chain`, `engine_host.cpp:1305`). `PluginHandle::render` asks it before every render (`plugin_rack.cpp:289`); asleep, `processBlock` is not called at all, an effect's buffer is zero-filled, an instrument adds nothing. Cost while asleep: a clock copy and a few compares for an instrument, plus a 256-sample level scan for an effect.
   - An **instrument** sleeps when no key is held, no sustain pedal is down, the song is not playing as the plug-in hears it, and its output has stayed under 1e-5 (-100 dB) for 0.5 s of its own frames. An **effect** sleeps when this block's input is under -100 dB and its output has been for 0.5 s; playing does not matter. Any MIDI for it, or a loud input sample, wakes it for that block.
   - **The tail is measured, not asked for.** `getTailLengthSeconds()` is implemented in all three of Melodee's own hosts but nothing calls it; CLAP's `process` status is used only to test for `CLAP_PROCESS_ERROR` (`clap_instance.cpp:631`); `request_process` is a no-op (`clap_instance.cpp:120`). So neither a reported tail nor CLAP's sleep state is honoured (PLUG-8).
   - **Not covered:** an instrument while the song plays with TRANSPORT on, which is the default (PLUG-1); an instrument with a key or pedal held (by design); a plug-in whose output never falls under -100 dB (noise floor, drone, DC); an effect fed by a live input whose noise is above -100 dB; a plug-in with no audio output (`run()` returns before `idle.heard()`, `plugin_rack.cpp:323`, so it never counts quiet); the warm-up blocks. A bypassed plug-in effect is not rendered at all (`engine_host.cpp:1312`). A frozen track's engine is not rendered (`engine_host.cpp:2780`).
   - `plugin_sleep.h` is unrelated to silence: it is the deep-freeze plan. An entry used only by deep-frozen tracks is unloaded with its state kept, and reloaded on thaw.
   - JUCE's own per-instance housekeeping threads are not gated by either (PLUG-4).

2. **Per-block overhead beyond the DSP** (`PluginHandle::render`/`run`, `plugin_rack.cpp:283–333`): two `steady_clock::now()`; a 56-byte clock copy and a `PositionInfo` rebuilt per `processBlock`; a 512-byte pointer array zeroed; `buffer.clear()`; up to four 1 KB copies (in to `memory`, `memory` to the host wrapper's own buffer, back, and out through a clamp); a level scan of the output; four relaxed atomics. No allocation: `memory`, `midi_in` (8192 bytes reserved) and `pending[512]` are made up front. No rack lock on the audio thread. MIDI is 3-byte structs in a fixed array, converted per format in fixed arrays. Parameter sync: CLAP one atomic exchange per block when nothing is pending; VST2 none; the bridge none on the audio path. Locks: JUCE's VST3 host takes an uncontended spin lock; the bridge takes a priority-inheriting mutex and makes at least one syscall per block (PLUG-2, PLUG-3). Delay compensation reads `latency()` per runner per block, which is trivial. I estimate about 1 µs per block for the native formats. An instrument is rendered in parts when calls fall inside a block (`play_calls`), so each part pays this again, and for a bridged plug-in each part is a round trip.

3. **The armv7 bridge.** One `vst2_bridge32` process per plug-in instance (`posix_spawn`, `vst2_bridge_instance.cpp:125`), and one more, briefly, for each probe of an armv7 plug-in. Host and bridge share a 1.5 MB `memfd` mapping; the plug-in reads and writes the shared audio arrays directly. Two futex words carry the handoff. **Both sides busy-wait before sleeping:** the host for 20,000 loads, the bridge for 4,000 loads after every reply (PLUG-2). The bridge runs at normal priority until the plug-in is open, then `SCHED_FIFO` 30 with reset-on-fork for every command, not only `process`. `tools/vst_probe/README.md:173` measured an empty back-to-back round trip at 1.2 µs mean, 1.8 µs p99, peaks of 23–85 µs; in real playback requests are 2.9 ms apart, so the bridge has gone to sleep and each block also pays a futex wake. Idle (plug-in asleep): no commands are sent; the bridge wakes 10 times a second from a 100 ms futex timeout and spins each time. Each round trip holds a lock the audio thread shares with parameter and state calls (PLUG-3).

4. **Rate bridging.** Only an Open MPC plug-in (a folder with `plugin-meta.xml`: `fixed_rate = 44100`, `plugin_files.cpp:173`), native or bridged, and only when the device runs at 48 or 96 kHz. A bare VST2 `.so`, VST3, LV2 and CLAP run at the device rate. `PolyphaseResampler` precomputes one row of weights per output phase and does a NEON FMA dot product per output frame and channel: 64 taps up and 72 down at 48 kHz, 64 up and 140 down at 96 kHz; tables of 40–82 KB. That is about 16,400 multiply-adds per block for an instrument and twice that for an effect, roughly 5–15 µs a block (unmeasured). The plug-in itself runs 8 % fewer frames at 48 kHz. See "Checked and fine".

5. **Threads and timers.**
   - Rack thread: `wait_for(50 ms)`, so 20 wakeups a second for the life of the app, with or without plug-ins (`plugin_rack.cpp:791`). It frees retired plug-ins and calls CLAP `idle()` (`on_main_thread`, rescan, latency), which does work only when the plug-in asked.
   - MIDI thread, every 5 ms: `plugins_tick` runs `set_card`, `split_shared_plugins`, `sync` (PLUG-10); every 50 ms it refreshes the cards of the plug-in in view by calling into the plug-in (PLUG-9).
   - JUCE message thread: started on the first VST3 or LV2 probe or load and kept until the last rack goes. Idle, it polls with a 2 s timeout (`juce_events/native/juce_Messaging_linux.cpp:363`).
   - JUCE timer thread: exists while any JUCE `Timer` does. Each JUCE-hosted VST3 instance starts a 60 Hz timer (`juce_VST3PluginFormatImpl.h:148`, started at 2250), asleep or not.
   - JUCE LV2 worker thread: polls with `sleep_for(1 ms)` while any LV2 instance exists (`juce_LV2PluginFormatImpl.h:1151–1163`), asleep or not (PLUG-4).
   - VST2 and bridged: no editor exists (`hasEditor()` false), so `effEditIdle` is never sent and `audioMasterIdle` is answered with 0. The bridge has one thread plus whatever the plug-in starts.
   - Threads that plug-ins start themselves are outside this repository (see "Could not determine").

6. **Scanning and loading.** The picker lists a folder on the rack thread every time it opens or moves (`plugin_jobs.cpp:83`), and once when the card is first seen, whether or not the picker is ever opened (`plugin_rack.cpp:765`). Listing does not `dlopen`: it reads ELF headers, `plugin-meta.xml`, `moduleinfo.json`, every `.ttl` of an LV2 bundle, and the whole library of every instrument or unknown VST2 for a drum map (PLUG-6). Then `probe_listed()` opens each plug-in whose kind or contents are not known: always for VST3, LV2 and CLAP, and for a VST2 with no `isInstrument` in its XML. A probe copies the plug-in into `/run`, opens it (a bridge process for armv7), and deletes the copy. The results are cached in memory only (`known`), for the life of the process (PLUG-5). Project load: `set_states` marks all 32 entries for reload; each used one is loaded in turn on the rack thread (describe, copy to `/run`, open, `prepareToPlay`, `setStateInformation`, 8 silent warm-up blocks, offered to the audio thread). Save: `states()` calls `getStateInformation` on every live plug-in on the MIDI thread under the rack lock. Zips in `Melodee/Plugins` are unzipped at every listing (PLUG-11).

7. **Open MPC plug-ins.** No GUI work happens: the host never opens an editor, the stubs are functions that return 0, and only `Plugin Skins/Q-Links.json` is read, once at load. What does cost: the whole folder, skins included, is copied into RAM at each load and probe (PLUG-7); the library is read whole for a drum map at every listing (PLUG-6); the 44.1 kHz contract brings the rate bridge at 48/96 kHz; and 130 of the catalogue's 228 entries are armv7 only (`tools/vst_probe/README.md:158`), so they run through the bridge (PLUG-2, PLUG-3).

8. **NAM.** `nam.cpp` holds no DSP: it orders the parameter page, shortens card names and makes two folders. Neural Amp Modeler is an external VST3 plug-in hosted through JUCE like any other, on whichever thread runs its chain; Melodee adds no threading and no SIMD for it. It is gated like any effect: it sleeps after 0.5 s of input and output under -100 dB, and does not sleep on a live input whose noise is above that. `docs/MELODEE.md:2472` gives about 17 % of a block for a lite capture and about 17 % for an 8192-tap IR at 48 kHz.

### Shell: service work, saving, settings: Answers to the assignment's questions

#### 1. What runs on each pass, and what could run less often

**Service thread, `MelodeeApp::service()` (`melodee_app.cpp:506-536`), every 5 ms:**

| Call | Per pass | Timed work inside | Could be |
| --- | --- | --- | --- |
| `keep_rest_off()` `:104` | one branch (pinning is off by default, `melodee_app.h:80`) | when pinned: lists `/proc/self/task` and sets affinity for each thread every 2 s | fine; event-driven when a thread is created if pinning is ever on |
| `note_dropout()` `:92` | one atomic load | none | fine |
| `read_machine()` `:480` | clock read, `card.status()` | `/proc/stat` parse + `plugins.usage()` (mutex) every 500 ms; `storage()` every 5 s | only while About is in view |
| `input_route.service()` | four atomics | mixer writes on change only | fine |
| `host.looper().service()` | queue pop, 4 failed CAS | allocates loop buffers, runs a fit, on demand | fine |
| `card_access()` `:565` | one atomic exchange, six atomic stores | lend/return on request | fine |
| `card_update()` `:626` | `web.take_rescan()`, `link.take_request()` (mutex + string copy) | status file every 250 ms only during an install; `storage()` every 2 s; runs `/bin/sh sd-update.sh check` per zip when the card changes | flag before the mutex; shared card path (SHELL-9) |
| `web_service()` `:735` | builds `Facts` (strings), `publish` (mutex, `notify_all`), `take_note` (mutex) | `storage()` every 1 s; port retry every 10 s | on change or 1 Hz |
| `warm_card()` `:540` | clock read, one atomic | `storage()` every 2 s (every 250 ms with no card) | shared card path |
| `library.service()` | about 15 queue pops and mutex checks (take, render, recall, stems, loops, holds, loads, banks, installs, removals, preview) | `storage()` + full folder re-list every 600 passes (SHELL-10) | drains need 5 ms only while a take, render or preview runs; listing on events |
| `store.service()` | one counter, one queue pop | `storage()` every 600 passes; one request per pass, each followed by `scan()` (SHELL-4) | on request only |
| power off / power key `:521-528` | three reads | `sync()` + `systemctl poweroff` once | fine |
| backlight `:530-535` | one atomic load | sysfs write on change only | fine |

No `stat`, directory scan or free-space query happens on *every* pass; they are all behind the timers above. Around this hook the base loop does, per pass: `clock.poll()`, `services.poll()` (`sd_bus_process`), `mixer.poll()`, `jacks.poll()` (`snd_ctl_read`), `card.poll()` (sysfs every 250 ms), `power.poll()` (`read`), and a 5 ms `poll()` on the control socket (`controller.cpp:32-38`, `:85-100`).

**MIDI thread, `Shell::lights()` (`lights.cpp:179`), every 5 ms under `Shell::lock`:** `card_follow`; `update_tick` (two mutexes); `autosave_tick`; `usage_tick`; `chop_tick`; library generation check; 16-track keyed-root check; `files_landed`; `record_tick` (includes `midi_audio_tick`, SHELL-1); `browser_tick` (string copy under the library lock); format-change check; three queue drains; `store_->take_loaded()` / `take_note()`; `reload_tick`; `song_tick`; `free_tick`; `plugins_tick` (SHELL-7); `print_tick` → `audio_tick` → `sync_audio`; `mpc3_tick`; `stems_tick`; `stage_tick`; `live_erase_tick`; `chain_tick`; `follow_tick`; `capture_tick`; `seq_follow`; three store-generation checks; `want_audio_format` (three atomic stores); the sample-load loops; stream holds every 100 ms (walks every pad of every kit track); `encoder_tick`, `lesson_tick`, `tap_tick`, `looper_tick`; then the status mirrors (SHELL-6) and the LEDs. `Shell::midi_output()` takes the same lock on every MIDI loop pass. See SHELL-7 for which of these can move to 50 ms.

Clock reads: `now_ms()` is `steady_clock` (`melodee_app.cpp:128-130`), called several times per pass on both threads (vDSO, cheap). Wall-clock time is read only to name the usage-log file (`lights.cpp:299`, `context.cpp:889`) and the crash report.

#### 2. Locks

| Lock | Taken by | How often | Held during |
| --- | --- | --- | --- |
| `Shell::lock` (`shell.h:146`) | MIDI thread: `lights`, `midi_output`, `surface`, `midi`, `midi_lost`, `looper_ask`, `midi_panic`. Screen thread: `snapshot` | MIDI ≥ 400 times a second; screen 10 to 60 times a second | Screen: the whole snapshot (SHELL-5, SHELL-8), with the library's, the store's and the rack's locks nested inside. MIDI: everything, including an autosave's serialisation (`context.cpp:76-83`), `read_mpc_slices` file reads (SHELL-11), `fs::exists` probes (`looper_files.cpp:82`; `project_store.cpp:198-208` `free_path`, up to 1000 probes, on a first autosave or Save As), and directory listings on some button presses (`midi_track_jobs.cpp:47-48`, `progression_file.cpp:36-38`) |
| `Shell::text_lock` (`shell.h:225`) | MIDI thread twice per pass (`lights.cpp:383`, `:492`); any thread in `summary()` | 400 times a second | three string assignments |
| `UpdateLink::lock` (`update.h:61`) | MIDI thread twice per pass; service thread once per pass | 600 times a second in all | string copies |
| `ProjectStore::lock` | service thread (publish at the end of `scan`, `write_settings`); MIDI thread (`view`, `free_path`); screen thread (`view` inside `snapshot`) | on change / per frame on Project | copies only; not held across the file reads in `scan()` |
| `SampleLibrary::lock` | MIDI thread about three times per pass (`card()`, `folder()` ×2) plus `measure`, `slot`, `files`; screen thread per frame (`slot`, `detail`, `thumbnail` ×16, `files`…); service thread | constant | copies; `detail()` reads up to 512 × 128 frames of the sample under it (`sample_library.cpp:186-207`) |
| `PluginRack::lock` | MIDI thread twice per pass (`set_card`, `sync`); service every 500 ms (`usage`); screen (`view`, `programs`); the rack's thread | constant | `sync` holds it across a `getStateInformation` on a rate change only |
| `WebApp::lock` | service thread twice per pass; the web thread | 400 times a second | string copies |
| `Session::prompt_lock` | screen thread only while a remote prompt shows | rare | a string copy |

**The audio thread takes none of these.** `MelodeeApp::render` (`melodee_app.cpp:135-139`) reads two atomics (`usb_full`, `main_volume`) and calls the engine host; everything the shell sends goes through `host.send` / `host.send_at` queues. I did not audit the engine host itself.

The risk in this table is not the audio thread but the MIDI thread: it is real-time, the mutexes are plain `std::mutex` (no priority inheritance requested), and every one of them is shared with a normal-priority thread.

#### 3. Undo, history, snapshots

- **Undo history** (`history.h:171-207`): RAM only, two `std::deque<Edit>` capped at 32 entries each (`depth = 32`), cleared on a project switch (`context.cpp:157`). Never written to the card.
- **An `Edit` is partial, not a copy of the project** (`history.h:139-169`): two inline `PadState`s (about 250 bytes each) and optional heap parts for what the scope covers: one `Pattern`, the `Song` (a few KB), a `Chain`, the mix, a track's sound, the master state, Chop's slices, the tempo. The largest are `TrackState` (a `Track`, about 32 KB, plus 128 patterns; used by track copy, clear and move, and pad-gap close at `context.cpp:1482`) and a `Pattern` with many notes (up to 65 535 events of 16 bytes).
- **When:** a pot's turns are one edit per control: `touch_gesture` takes the "before" once when the gesture opens and returns at once while it continues (`undo_restore.cpp:89`); the "after" is taken when another control is used or after 2 s still (`lights.cpp:504`). A button or pad press on an editing page takes a "before", runs the press, and keeps the edit only if something differs (`PageEdit`, `undo_restore.cpp:182-195`). A Seq Record take is one edit (pattern + song, `context.cpp:2253`).
- **No multi-megabyte copy on control input or on a timer** in the undo path. The per-frame `Project` copy for the screen (SHELL-5) is the only large periodic copy I found.
- Related, not undo: every pattern change sends the audio thread a fresh heap copy of the pattern (`context.cpp:1228`), so a pot sweep on a note card (Step velocity and the like, `context.cpp:634-658`) makes one pattern copy per controller message. Motion recording is throttled to one per 40 ms (`context.h:215`); note-card edits are not. Small for ordinary patterns.

#### 4. Writes to the card

| What | Trigger | At most | Size | Synced | Unchanged data rewritten |
| --- | --- | --- | --- | --- | --- |
| Project autosave (`lights.cpp:64-82`, `context.cpp:112-122`) | `dirty`, transport stopped, no take/print/export/load, no sheet open, 3 s since the last control or note | once per change-then-pause; about once a second in the SHELL-1 case | the whole project file, plug-in states and print table included; `.new` then rename | no `fsync` (kernel writeback decides) | yes: whole file each time; also when only engine-mirror commands "changed" it (SHELL-1) |
| Looper WAVs (`looper_files.cpp:70-86`) | every project save | as above | every held loop, 24-bit stereo, up to about 18 MB each | no | yes (SHELL-2) |
| Chop slices into sample WAVs (`keep_chop` / `write_chops`, `context.h:174-175`) | every save | as above | changed chops only, by its comment (not read further) | not checked | no |
| Settings file (`project_store.cpp:510-518`) | each −/+ step, each look detent, metronome toggle, star, lesson done; K1 after 1 s rest; when the open project's path changes (`remember`, `:503-508`) | per press | 1 to 2 KB, truncated in place | no | yes: no compare (SHELL-13) |
| Usage log (`usage_log.h:37-38`, `project_store.cpp:476-483`) | only when the setting is on and lines wait: every 10 s or 4096 bytes | once per 10 s while the hands are busy; nothing when idle | appended lines | no | no |
| Crash report (`crash_log.cpp:148-224`) | a crash | once | a few KB | yes, `fsync` (`:219`) | n/a |
| Power off (`melodee_app.cpp:37`) | POWER OFF on the Exit sheet | once | `sync()` of everything | yes | n/a |

Each of the first, fourth and fifth rows is also followed by a parse of every project in the folder (SHELL-4), which is reading, not writing.

#### 5. Power-related settings that exist today

| Setting | Where | Effect on power or CPU | How it reaches the hardware |
| --- | --- | --- | --- |
| Brightness, 20 to 100 % in tens | `settings.h:88` | backlight | MIDI thread copies it to `Shell::screen_wanted` (`lights.cpp:303`; left untouched at 100 % so the stock level stays); service thread reads `shell.screen()` (`shell.h:59`) and calls `Platform::backlight()` then `Platform::brightness()` on change (`melodee_app.cpp:530-535`); `Platform::brightness` writes `/sys/class/backlight/*/brightness` and restores the old value on exit (`platform.cpp:108-124`) |
| Screen mirror on/off, resolution, 30/60 fps | `settings.h:104-106` | mirror threads, JPEG encoding, USB | `Shell::mirror_wanted` (`shell.h:158-163`) → `App::screen_mirror()` → the runtime's screen loop (`run_app.cpp:134`) |
| Sample rate 44.1/48/96 kHz, audio buffer 2 to 8 periods | `settings.h:94-97` | audio CPU per second | `Shell::audio_wanted` → `App::audio_format()` → the runtime reopens the device (`run_app.cpp:133`) |
| USB volume Full/Knob | `settings.h:100` | Full adds a second mix block for USB each audio block (`melodee_app.cpp:136-138`) | `Shell::usb_full` atomic |
| Pad lights (track colours, white, off), Pad fade, Tap Tempo light | `settings.h:75-80` | LED current | read in `Shell::lights()` |
| Features Basic/Full | `settings.h:44` | none: it hides pages only (`features.h`) | n/a |
| Usage log | `settings.h:46` | a card write every 10 s of use | `ProjectStore::append_usage` |
| Sample browser PREVIEW | `settings.h:56` | reads each file under the cursor | library preview |

There is **no** idle timer, screen dim, screensaver, pad-LED dim, auto power-off, eco mode or CPU governor control anywhere in `src/melodee` or `base/src`. Where a Power setting goes: see SHELL-3.

#### 6. Signals available for "idle for N minutes"

- **Last input:** `Shell::last_input_` (`shell.h:177`), ms on the steady clock. Stamped by every control-surface message (`shell.cpp:130`), every external MIDI message except real-time ones (`shell.cpp:96`), and every pass while a pad or a modal button is held (`lights.cpp:66`). Private to the MIDI thread today; needs an atomic copy. Not stamped by the power key (the service thread counts it: `hardware.power.presses()`, `melodee_app.cpp:526`) or by web-page activity.
- **Transport and jobs**, already gathered every pass in `Shell::autosave_tick` (`lights.cpp:68-76`): `host.playing()`, `host.song_playing()`, `host.looper().recording()` / `armed()`, `ui.recording`, `ui.rec_armed`, `host.recorder().state()`, `printer->busy()`, the print and auto-sample sheets, `store_->exporting()`, `ui.loading`, `samples_to_load`, an open question, name sheet, exit sheet, card access or update.
- **Sound:** `host.out_level(0/1)` and `host.in_level()` (used for the meters, `lights.cpp:560`), `host.meter(t)` per track, the voice taps (`host.taps`), `host.monitoring()`, each loop's `view(n).sounding`.
- **External clock:** `midi_io.following()` / `following_mtc()` (`shell.cpp:831-832`), `session().clocks`.
- **Counters from the runtime:** `session().hardware_events`, `external_events`, `synthetic_events` (`session.h:37`), the same ones the screen loop uses to wake from its idle rate.
- **USB and card:** `hardware.card.status()` (state, `connected`: a computer has configured the gadget), `hardware.platform.usb_state()`, `input_route.usb_state()`, `session().mirror.state` (a computer is showing the camera), `hardware.usb_output`.
- **Jacks:** `session().plugged(Jack::Headphones / LineOut)`.
- **Supply:** `Platform::supplies()` returns each supply's name, type, `online` and `voltage_now` (`platform.cpp:179-184`). Melodee never calls it. Whether this board reports mains through `online` I could not tell from the code.

#### 7. Start-up and project load

- `MelodeeApp::MelodeeApp()` reads the mount table and the settings file before the threads start (`melodee_app.cpp:72-74`), deliberately, so the audio device opens once at the right rate.
- First service pass: `stop_stock_web_server()` spawns `systemctl stop` and waits for it (`melodee_app.cpp:48-56`, `:738`); the card warm starts and holds the library and the store back for up to 5 s (`:540-561`).
- About 250 ms in, `card_update` runs `/bin/sh sd-update.sh check` for each zip in the card's top folder, up to 16, 10 s timeout each, on the service thread (`base/src/drivers/sd_update.cpp:174-192`). It overlaps the card warm, so it only costs time if zips are present.
- `ProjectStore::service` then reads and parses **every** project in `Melodee/Projects` before it reads the one to resume (SHELL-4).
- `switch_to` builds the project pack and asks for every sample again; the MIDI thread reads each sample file's slice chunk itself (SHELL-11). A card warm is started again before each project load (`melodee_app.cpp:556-559`).
- Samples are loaded on the library's pool, not synchronously in the shell (not audited further).

#### 8. Bugs looked for

- **Wall-clock timers:** none. All shell timers are `uint32_t` ms on `steady_clock`. Two comparisons are not wrap-safe (`at >= settings_save_at`, `sample_record.cpp:282`; `now >= boot_ms`, `melodee_app.cpp:639`), which matters only after 49 days of uptime.
- **Pass-count timers:** `++ticks % 600` in the store and the library, `hold_passes` in the looper files. Correct at today's 5 ms; they silently stretch if the service period ever changes.
- **Busy loops:** none in my files. Outside them: the stream pager's 1 ms sleep loop (SHELL-12) and `sample_library_instruments.cpp:575`, `:590` (1 ms sleeps, not read).
- **Joins on the service thread:** `card_warm.stop()` joins the warm thread when the card is lent (`melodee_app.cpp:573`, `card_warm.cpp:213-216`; the worker is passed a `stopping` flag, so it should be prompt); `ProjectStore::start_export` joins only a finished exporter (`project_store.cpp:112-114`).
- **Unbounded containers:** none found. History is capped at 32; the crash ring is fixed at 256 notes; the usage log flushes at 4096 bytes and its request is dropped when there is no card; `update_skip` and `slots_waiting` are bounded by what exists.

### Storage, loading and background jobs: Answers to the assignment's questions

1. **Web server and mDNS.** Both start with the first service pass and run for the whole session (`melodee_app.cpp:737-741`, `web_app.cpp:73-75`); nothing looks at whether a computer is connected. Each has one thread that waits in `poll(..., 250)` (`web_server.cpp:209`, `mdns.cpp:217`), so the idle rate is 4 + 4 wakeups a second. mDNS announces twice, a second apart, at start only (`mdns.cpp:207-215`); it never announces again and only answers questions for `melodee.local`. If no IPv4 interface is up when the web page starts, mDNS fails with "no network" (`mdns.cpp:172`) and is never tried again, because `web_running` is already true. With a blocking `poll` and an eventfd for `stop()`, both threads would sleep in the kernel until a packet arrives; no interface tracking is needed (IO-10).
2. **Threads in this area.**

   | Thread | Started by | Waits how | Priority | Exits when idle |
   | --- | --- | --- | --- | --- |
   | Load pool x 3 (`load_pool.cpp:13-15`) | `SampleLibrary` construction | condition variable, no timeout (`:52`) | SCHED_OTHER, nice 5 (`:48`) | no, blocked |
   | Card warm (`card_warm.cpp:201`) | service thread: card found, card returned, each project load | none: one pass of `pread`s, then ends | inherits the service thread's | yes |
   | Web server (`web_server.cpp:195`) | service thread, first pass | `poll` 250 ms | inherits the service thread's | no |
   | mDNS (`mdns.cpp:187`) | with the web server | `poll` 250 ms | inherits the service thread's | no |
   | Stems worker (`stems.cpp:71`) | service thread, a job | `poll` on the child's pipe, 100 ms (`:172`) | SCHED_OTHER, nice 5 (`:98-99`) | yes, with the job |
   | Stems program (child process) | `posix_spawn` (`stems.cpp:155`) | CPU-bound, 3 threads | SCHED_OTHER, nice 19, `oom_score_adj` 1000 (`stems_engine.c:254-260`) | yes |
   | Audio tracks (`audio_tracks.cpp:28`) | **MIDI thread**, first `sync_audio()` (`audio_jobs.cpp:37`) | **`wait_for` 25 ms, forever** (`:157`) | **inherits SCHED_FIFO 15** (IO-7) | no |
   | Chop writer (`chop_mode.cpp:208`) | **MIDI thread**, a save with changed slices; detached | none | **inherits SCHED_FIFO 15** (IO-7) | yes |
   | MPC 3 export (`project_store.cpp:123`) | service thread, a job | none | inherits the service thread's | yes |
   | Print engine (`print_engine.cpp:197`, skimmed) | app construction | condition variable, no timeout (`:322`) | SCHED_IDLE (`:316`) | no, blocked |

   `sleep_for` loops in this area: `sample_library_instruments.cpp:575` and `:590` (1 ms, at most 500 turns each, only while the card is being lent) and `print_engine.cpp:173` (5 ms, while a print host's plug-ins open). None runs when idle. Seen outside the area: `plugin/plugin_rack.cpp:791` waits with a 50 ms timeout.
3. **Card warm.** No timer of its own. The service thread reads the mount table every 2 s to see whether the card changed (`melodee_app.cpp:542-547`) and starts a warm when it did, and again before every project load (`:556-559`). A warm reads the whole FAT (up to 32 MB, in 1 MB `pread`s) and then every folder's entries in 128 KB stretches, up to 6000 folders, Melodee's first (`card_warm.cpp:153-176`). It always reads everything again, cached or not: a cached run is a copy out of the page cache (the code's own comment says a tenth of a second). While it runs, for at most 5 s, `library.service()` and `store.service()` are not called (`melodee_app.cpp:516`). Not a finding: the cost is known and small, and the repeated run replaces a minute of single-sector reads when the cache was dropped.
4. **Sample loading.** One pool task a file, three side by side (`sample_library.cpp:1020`). A PCM WAV of at most 10 s: the whole data chunk into a byte vector, converted to two float vectors at the file's own rate (`engine/sample_data.cpp:102-112`); no resampling at load. MP3, FLAC, Ogg, AIFF: JUCE decodes into an `AudioBuffer`, copied to the two vectors (`sample_decode.cpp:120-134`). Then one pass for the 512-column overview, and the hit, beat and note analysis (IO-4). RAM: two float vectors always, 8 bytes a frame, a mono file's side written twice (IO-13). Longer than 10 s (or over 2 s when memory is short): a PCM WAV is read in place from the card as it plays with half a second held where each pad starts (`stream_holds.h`, `sample_library.cpp:1179-1223`); other kinds are decoded once into a PCM WAV in `Melodee/Cache` and that is streamed. The whole long file is read again after every load for its waveform and hits (IO-5). The same path is never loaded twice: `Context::sample_slot` returns the slot that already names it (`context.cpp:1823`).
5. **Project save and load.** A save is the project's own data only (tagged sections, empty patterns left out, sample paths not audio, each plug-in's state blob): no CRC, no hash, no compression. It is built on the MIDI thread (`context.cpp:83`), written whole by the service thread to `<path>.new` and renamed (`project_store.cpp:396-402`); there is no `fsync`. Beside it: the settings file when the last project changed, a changed chop patched into its WAV in place, and **every loop's WAV, changed or not** (IO-3). After the write the store lists the folder again and parses every project in it (IO-1). Autosave: 3 s after the last control while the project is dirty and nothing plays, records, prints or loads (`autosave.h:12,27-31`).
6. **Writes while idle or playing.** None on a timer by default. Autosave never runs while the sequencer plays. Writes that do happen: autosave after edits, the settings file 1 s after a settings or input change (`sample_record.cpp:282-285`), the usage log every 10 s only when the user turned it on (`usage_log.h:37`), a take or render as it is recorded, and one `utimes()` per cache file per load (IO-11). Audio Recall and the note capture ring are memory only. A stems job works in `/run` (RAM) and writes four files at its end.
7. **Stems.** A separate process, three threads of its own with OpenBLAS told to use one (`stems_engine.c:275`, `stems.cpp:32`), nice 19, SCHED_OTHER, first to be killed when memory is short. It cannot preempt the audio thread (FIFO 30) or the render workers (FIFO). No affinity is set. Measured by the author: 13 s for 13 s of sound, 36 s for 60 s, 478-656 MB at its fullest (`stems.h:33-34`). The model is read by the child and freed when it exits; nothing stays in Melodee. Built `-O2 -mcpu=cortex-a55` with OpenBLAS `TARGET=CORTEXA55` (`build.py:37-38`). Fine.
8. **Recording.** Not recording, the audio thread does three cheap passes over each 128-frame input block: a peak scan (`engine/take_recorder.h:85`), the input meter (`engine_host.cpp:3142-3145`) and a plain copy into the Audio Recall ring (`engine/recall_ring.h:31-40`). Nothing is converted or analysed. The ring is 26 s of stereo float at 96 kHz, 20 MB, and the take buffer 60 s at 96 kHz, 46 MB; both are allocated zero-filled at start (`recall_ring.h:28`, `take_recorder.h:40`), so 66 MB is resident whatever the device's rate. `shell/capture_ring.h` is the note capture, 8192 notes (192 KB), written only when a note is played.
9. **File-system scans.** The sample browser's folder is listed again every 3 s whether or not the browser is open, with a `stat` per file and a read of every MPC program file (IO-2). The project list is rebuilt, each project parsed, after every store request (IO-1). The Samples page copies the whole listing each frame and each wheel detent (IO-12). The mount table is read about three times a second by six separate pollers (IO-9).

### Project model and sequencing: Answers to the lead's questions

**1. Size and layout**

| Type | sizeof | Notes |
|---|---|---|
| `Event` (a note) | 16 B | `std::vector<Event>` per pattern, sorted by tick; at most 65 535 (1 MB) |
| `MotionPoint` (a point or a parameter lock) | 12 B | `std::vector`, at most 8192 a pattern (98 KB) |
| `Step` (the pages' view only, never stored) | 30 B | |
| `Pattern` (header) | 240 B | seven vectors, the feel, follow, audio region |
| `PatternBank` (16 x 128 headers) | 491 520 B | events on the heap: storage is sparse |
| `PadSound` | 132 B | x 128 pads a track |
| `Track` | 32 312 B | `pads` 16 896 + `pad_fx` 12 544 + `zone_trims` 640 = 30 080 B (93 %) is kit data, present on every track whatever its engine |
| `Project` | 538 968 B | 16 tracks = 516 992 B; of the whole, 481 280 B (89 %) is kit data |
| `SampleRef` / `SampleChop` | 320 B / 920 B | `SlotTable`: 128 pointers inline, blocks of 64 slots (20 480 B / 58 880 B) made on first write |
| `Command` | 200 B | a whole `PadSound` inline; the queue is `MidiQueue<4096, Command>` = 819 200 B |
| `PatternSketch` | 112 B | x 2048 in `Snapshot` = 229 376 B |
| `Snapshot` (one screen frame's state) | 793 888 B | holds a `Project` by value |
| `ProjectPack` / `TrackPack` | 555 352 B / 33 344 B | plus one heap `Pattern` a pattern |
| `TrackState` (undo of a track copy) | 63 032 B | |

Pattern storage is sparse. The fixed part of an empty project is about 0.5 MB of pattern headers on the shell side, the same again as 2048 separately allocated play copies on the audio side, and 0.54 MB of `Project`. The audio thread touches none of it in a block that has no tick. On a tick it reads, for each playing track, two or three cache lines of the pattern header plus a binary search in the events (and in the motion, when the pattern has any).

**2. How the audio thread reads the project.** There is no shared `Project`. The shell (MIDI thread) owns `Project` and `PatternBank`, guarded against the screen thread by `Shell::lock` (a `std::mutex`). The audio thread owns mirrors: its `Slot`s, a `Song`, and `PlayPatterns` (128 `Pattern*` a track). Changes travel as `Command`s through a single-producer, single-consumer ring. A pattern edit sends a new heap copy (`Context::sync_pattern`, `context.cpp:1225`); the audio thread swaps the pointer and hands the old one back through `retired_patterns` to be freed on the MIDI thread, so it never allocates or frees. A track copy sends a `TrackPack`, a load a `ProjectPack`. The cost of one pattern edit is proportional to that pattern (two deep copies and an allocation, MODEL-3). Recorded Motion is throttled to one copy every 40 ms (`context.h:215`); knob-driven note edits and the Feel knobs are not. One operation re-sends all 2048 patterns for each knob value (MODEL-2).

**3. Per-block scheduling.** The sequencer is driven per tick (96 a quarter: 192 ticks a second at 120 BPM, roughly every second block at 44.1 kHz), not per block, and not at all while stopped (`engine_host.cpp:2742-2746`). Each tick does one `std::lower_bound` per playing track (`engine_host.cpp:2544`), at most 17 probes for a full pattern, then plays only the events on that tick. Trig conditions are O(1). Ratchets put their later hits in 32 `PendingHit` slots a track, counted down per run. Swing is a per-tick length (`tick_samples`). Song mode looks clips up once a bar (16 x a linear `clip_at` over at most 256 clips, `engine_host.cpp:1819`); `song_flatten.h` is only used by exports. The tempo lane is a linear scan of at most 64 points per tick, in song mode only (`engine_host.cpp:2507`). My estimate for 16 tracks of full 128-bar patterns is well under 10 µs a tick (unmeasured). Two things scale worse: a parameter lock ending (MODEL-4) and kit lanes of their own length (MODEL-5).

**4. Modulation.** The project's eight modulators run once a block, only while one is active (`if (mods_on)`, `engine_host.cpp:2703`). An active LFO costs one `pow` (`lfo_hertz`), one `sin` and up to four `lround` a block; a follower a 128-frame peak scan, a `pow` and an `exp`. A lane is written only when its integer value changed. An unused modulator costs one comparison. The per-track mod matrix (`Track::mods`, eight `ModRoute`) is evaluated nowhere: `Engine::set_mod` is an empty virtual (`engine/engine.h:45`) and no engine overrides it, so every route is free.

**5. Generators, chords, scales, progressions.** Nothing here is reachable from the audio thread. The tables are `static const`. `scale_degrees()` refills a 12-entry array on each call and `prog_pad_name()` runs about fifteen `std::string` replaces a pad; both are called per frame by pages for at most 16 pads. Euclid, variations, groove extraction, `flatten_lane` and the note edits run on a button press or a knob value.

**6. Copies on edit.** Note jobs, erase, copy, scene and chain jobs run on button presses. Each copies one pattern for undo (before, and after when it changed) and sends one play copy. Track copy, move and clear copy a 63 KB `TrackState` and send a `TrackPack`. Undo for pots is one snapshot a gesture, not one a tick (`undo_restore.cpp:89`). The exceptions are in MODEL-2 and MODEL-3.

**7. Wasted memory.** `Project` is 0.54 MB of fixed arrays, 89 % of it kit data, and it is copied whole for every screen frame (MODEL-1), every load (`ProjectPack`), every pattern printed (`print_engine.cpp:532`) and every export job. I did not read the save path.

**8. Bugs.** One found: MODEL-2 (a leak and a possible queue overflow). I found no overflow or drift: `tick_now` is `uint32_t` and wraps after 103 days at 300 BPM; `elapsed = tick_now - slot.start` is modular and consistent; `until_tick` carries the fractional remainder in a `double`, so tempo does not drift; `frames_to_ticks`, `played_ticks` and `lane_tick` widen to 64 bits; note lengths in samples stay below 2^31 at 30 BPM and 96 kHz. There is no rescan at pattern wrap because every tick searches afresh.

# Measuring on the device

Everything below needs Melodee running on the MPC and a shell on it (root SSH over USB-C; see `../MPC Sample Knowledge Base/05-community-mods-and-tools.md`). `device.py ctl status` prints the status line.

| Question | How |
| --- | --- |
| Are the clocks untouched? | `cat /sys/devices/system/cpu/cpufreq/policy0/scaling_{governor,max_freq,cur_freq}`: `performance`, 1800000, 1800000, as under the stock app |
| What does a block take? | `melodee_render_load=` in the status line (tenths of a percent of a block), the same project under the original Melodee and under the fork, playing and stopped |
| Dropouts | Settings > AUDIO shows them; the status line has the audio `xruns` count, `melodee_dropout_track=` and `melodee_guard=` |
| Screen thread cost | `screen_presented` (frames drawn), `screen_pushed` (frames that changed) and `screen_draw_us` (last draw) in the status line, read twice ten seconds apart on each page, stopped and playing |
| Is the screen dimmed? | `melodee_dimmed=` in the status line |
| Per-thread CPU | `top -H -p $(pidof sample-base)` for a minute, stopped and playing: the screen, service, MIDI and pager threads should be near zero when stopped |
| Wakeups | `grep ctxt /proc/$(pidof sample-base)/task/*/status` twice, a minute apart |
| Needless saves | Leave the device untouched for a minute and check the project file's time does not move |
| Battery life | Same project, same brightness, from full until it switches off: once under the original Melodee, once under the fork. The supply voltage is in `/sys/class/power_supply/mains-power/voltage_now`; logging it once a minute gives the first discharge curve anyone has published for this device |

# How this document is made

The findings and appendices are assembled from the auditors' reports, my verdicts and the fork's status by `.local/audit/build_doc.py` (the `.local` folder is not tracked by Git). To change a verdict or a status, edit `verdicts.tsv` or `status.tsv` there and run the script again.
