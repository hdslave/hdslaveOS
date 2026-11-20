# Audit: project model and sequencing data

## Coverage

**Read fully**
- `src/melodee/model/`: `limits.h`, `trig.h`, `lane_value.h`, `pattern.h`, `pattern.cpp`, `project.h`, `sampler.h`, `slot_table.h`, `pad_params.h`, `keygroup_track.h`, `midi_track.h`, `audio_track.h`, `looper.h`, `mod_matrix.h`, `modulators.h`, `tempo_lane.h`, `song_flatten.h`, `follow.h`, `progression_data.h`, `progression.h`, `chords.h`, `scales.h`, `generators.h/.cpp`, `note_edits.h/.cpp`, `print.h/.cpp`, `fader.h`, `knob_fx.h`, `pad_fx.h`, `compressor.h`, `input_config.h`, `midi_config.h`, `chop_slices.h`, `slice_banks.h`, `card_folders.h`.
- `src/melodee/shell/`: `step_edit.cpp/.h`, `seq_layer.cpp/.h`, `sequences.h`, `scene_jobs.cpp`, `chain_jobs.cpp`, `note_jobs.cpp`, `erase_copy.cpp`, `track_copy.cpp`, `track_swap.h`, `step_lane.h`, `note_copy.h`, `record_place.h`, `free_length.h`, `spare_bar.h`, `place_bar.h`, `sampler_map.h`, `pad_context.h`, `mod_jobs.cpp`.
- `src/melodee/engine/command.h`.
- `src/melodee/engine/engine_host.cpp`, the sequencing parts only: lines 225-345 (tempo, `take_commands`, `place_at`, `skipped`), 431-470 and 868-900 (`apply`, `PatternData`), 1096-1160, 1337-1460 (modulators), 1569-1735 (`restore`, `play_motion`, lanes, `tick_frame`), 1737-1932 (`bar_start`, follow actions, song clips, MTC place, arp write), 1979-2033, 2300-2575 (punches, `play_event`, `tick`), 2619-2790 (start of `render`). `engine_host.h` lines 25-110 and 370-500.

**Read to trace callers (outside the assignment)**: `shell/context.cpp` (`sync_pattern`, `sync_motion`, `record`, `begin_edit`/`commit_edit`, `set_signature`, `free_tick`, `sync_song`), `shell/context.h` 150-330, `shell/history.h`, `shell/undo_restore.cpp` 1-125, `shell/feel_jobs.cpp`, `shell/snapshot.h`, `shell/shell.cpp` 88-132 and 800-958, `shell/lights.cpp` 170-420, `melodee_app.cpp` 425-470, `base/src/runtime/run_app.cpp` 100-172, `base/src/surface/midi.h` (`MidiQueue`), `stream_holds()` in `shell/stream_holds.h`.

**Skimmed**: `shell/stage_map.h` (first 60 of 145 lines: page tables, no sequencing).

**Skipped**: `model/project_file.cpp/.h`, `model/sound_file.cpp/.h`, `model/smf.cpp/.h` (another auditor); the mixing, effect, render-pool and plug-in parts of `engine_host.cpp`.

**How sizes were obtained**: `sizeof` through a syntax-only compile (`clang++ -std=c++17 -fsyntax-only`, arm64 LP64) of a probe file kept outside the repository; nothing in the repository was built, run or changed. Layout rules are the same as the device's aarch64 g++; `std::vector` is 24 bytes and `std::unique_ptr` 8 in both standard libraries. `Edit` and `Snapshot` hold `std::string` members (24 bytes in libc++, 32 in libstdc++), so those two are a few dozen bytes larger on the device.

## Answers to the lead's questions

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

## Findings

### MODEL-1: Every screen frame copies the whole Project and rebuilds 2048 pattern sketches under the shell lock
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

## Checked and fine

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

## Could not determine

- The real time of `Shell::snapshot()` on the device, the frame rate while a pattern plays (the SPI push limits it), and how often the screen thread is preempted while it holds `Shell::lock`.
- How fast the pots and the fader deliver values, which decides whether MODEL-2's overflow can happen and how much MODEL-3 costs.
- Which pages read other tracks' kits from `Snapshot::project`, and whether every write to `Track::pads` is followed by `send_pad()`; both decide how MODEL-1's second part can be done.
- How many notes and locks real projects hold; MODEL-3 and MODEL-4 only matter for long, dense patterns.
- Whether the save path writes the fixed arrays whole, and what an autosave three seconds after each edit costs on the card (`model/project_file.cpp`, another auditor).
- The device's RAM size and whether the process locks its memory. A pattern copy above glibc's mmap threshold (128 KB, about 8200 notes) is first served by `mmap` and page faults on the MIDI thread.
