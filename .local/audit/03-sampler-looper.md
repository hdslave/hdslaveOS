# Audit: sample playback voices, streaming and the looper (SMP)

## Coverage

Read fully (all under `src/melodee/engine/`): `sampler.cpp`, `sampler.h`, `sample_data.cpp`, `sample_data.h`, `multisample.h`, `stream_pager.cpp`, `stream_pager.h`, `audio_player.cpp`, `audio_player.h`, `take_recorder.h`, `looper.cpp`, `looper.h`, `punch.cpp`, `punch.h`, `punch_mpc.cpp`, `print_player.cpp`, `print_render.cpp`, `print_render.h`, `external_notes.cpp`, `external_notes.h`, `arp.h`, `note_repeat.h`, `midi_engine.h`, `midi_out.h`, `plugin_engine.h`, `keygroup.h`; and of `dsp.h` lines 10-80 and 140-209 (rate helpers, `soft_clip`, `Envelope`).

Skimmed: `keygroup.cpp` (function list only: a load-time reader of an XML tree, nothing on the audio thread); `print_player.h` (the class, lines 60-110); `engine_host.cpp` only at the call sites of my code (1245-1275, 2284-2289, 2619-2650, 2730-2945, 3085-3134, 3321-3432); `render_pool.cpp` (futex and priority lines); `shell/sample_memory.h`; `shell/sample_library.cpp:440-452`; `docs/LOOPER.md` lines 88-92.

Skipped: `tests/direct_stream.cpp`, `tests/card_warm.cpp`; `engine/recall_ring.h` and the `audio_in` take (both capture every block, neither is on my list); `effects.h` (what a pad chain runs), `macros.h`.

Sizes quoted below come from a syntax-only compile of the headers with clang on arm64, outside the repository (the same LP64 layout as the target): `SamplerEngine::Voice` 648 bytes, `Grain` 104, `Lane` 1,040, `BandRows` 10,048, `SamplerEngine` 92,552 inline plus about 453 kB on the heap, `PadSound` 132, `Punch` 568 plus a 1.536 MB history, `Looper` 4,528.

Every time estimate is **unmeasured** and assumes 1.8 GHz (the `performance` governor). Cycle counts for the Cortex-A55 (single-precision divide about 10-12 cycles, double about 20, neither pipelined) are from memory of Arm's optimisation guide; treat them as rough.

Where the eight questions are answered: Q1 voice loop: SMP-9, SMP-8, Checked. Q2 filters and envelopes: SMP-8, SMP-13, Checked. Q3 idle and silent voices: SMP-5, SMP-6. Q4 per-pad extras: SMP-5, SMP-7, Checked. Q5 streaming: SMP-1, Checked. Q6 looper: SMP-2, SMP-4, SMP-11. Q7 recording paths: SMP-3, Checked. Q8 memory: SMP-10, SMP-12, Checked.

## Findings

### SMP-1: The stream pager's thread wakes 1,000 times a second for as long as the app runs

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

## Checked and fine

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

## Could not determine

- The real cost of the pager's 1 ms wake (SMP-1) and of `LoopStretch::best` (SMP-2) on the device. For the second, `loop_ns` is already there to read.
- This SoC's L2 and L3 sizes, on which the idle-scan estimates in SMP-3 and SMP-5 lean.
- Whether GCC inlines `voice_out` into `run_plain` (five call sites, a large body: probably not, a call a frame for a filtered voice); look at the object code.
- How common envelopes with SUSTAIN at 0 are in users' kits and imported SFZ or keygroup programs (SMP-6).
- The pager runs at FIFO 20, above the MIDI thread (15): a chunk's read and conversion, `mlock`, `madvise`, and the `std::map`/`std::list` nodes allocated in `now_idle()` (`stream_pager.cpp:53-57`) all run ahead of pad and LED handling. Whether a chord on a many-layered streamed instrument delays the MIDI thread audibly needs a measurement.
- A part read in place is held twice for a while: the file's pages in the page cache and the float copy in the part's own memory. The page cache gives way first under pressure; whether `posix_fadvise(POSIX_FADV_DONTNEED)` after a chunk is worth it depends on how tight memory gets.
- `engine/recall_ring.h` and the `audio_in` take write every block (`engine_host.cpp:3134`, `3148`); neither was on my list and I did not read them.
- Whether the host could skip a silent kit altogether is the host auditor's question; SMP-5 item 3 says what the sampler would have to report first.
