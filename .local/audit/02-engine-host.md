# Audit: the audio render graph (engine host, render pool, app hooks)

## Coverage

**Read fully:** `src/melodee/melodee_app.cpp`, `melodee_app.h`; `engine/engine_host.h`; `engine/engine_host.cpp` lines 1–430, 1068–1760, 1880–2300 and 2396–3435 (all of `render()` and what it calls in the host every block: engine calls, chains, modulators, keys, delay compensation, MIDI out, freeze/print, the arp clock, `tick()`, loops, taps); `engine/render_pool.cpp/.h`; `engine.h`; `compensation.h`; `voice_tap.h`; `bounce.h`; `recall_ring.h`; `master_comp.cpp/.h`; `macros.cpp/.h`; `dsp.h`; `rbj.h`; `dsp_bench.cpp`; `plugin/idle_gate.h`; `plugin/plugin_runner.h`; `Makefile`, `base/base.mk` (flags).

**Skimmed (only as far as cost and call frequency):** `engine_host.cpp` 431–1067 (`apply()`, one command at a time), 1760–1880 and 2300–2396 (per bar or per tick: follow actions, song clips, MTC locate, pattern punches); `command.h`; `effects.h` and the entry of `Effect::process` (`effects.cpp` 613–660) plus the echo's line reach; `punch.h` and the frame loop of `Punch::process`; the start of `Looper::run`; the idle path and lane tail rule of `SamplerEngine::render_stereo`; `stream_pager.h` and its `open/close/block`; `take_recorder.h` `process`; `midi_out.h` (`AudioClock`, `SyncOut::run`); `midi_engine.h`; `plugin_engine.h`; base: `runtime/run_app.cpp` render hook, `drivers/audio.cpp` run loop, `drivers/usb_audio.cpp` `push`, `surface/midi.h` `MidiQueue`, `install/sd-worker.sh`; `docs/ROADMAP.md` P2 entries.

**Skipped:** effect DSP bodies, sampler voices, Looper internals, plug-in rack, model, shell (lights, MIDI out, snapshot), pages/ui (what `draw()` paints), JUCE.

**Not done:** nothing was compiled or run. Every time figure below is an estimate from reading the code, at 1.8 GHz (the `performance` governor). Blocks a second: 344.5 at 44.1 kHz, 375 at 48 kHz, 750 at 96 kHz; a block's budget is 2902, 2667 and 1333 µs.

## Answers to the assignment's questions

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

## Findings

### HOST-1: Track, bus, loop and master chains never rest; a silent track costs its full strip every block
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
- **Where:** `src/melodee/engine/engine_host.cpp:3109`, `:3118-3119`, `:2286-2289` (`run_live`); `src/melodee/engine/punch.cpp:97-107`, `:227-230`; defaults `engine_host.h:743`, `:748`, `:774`.
- **Thread and frequency:** audio thread, per frame, ten times a block.
- **Now:** the song's punch, Flex Beat, four live slots and four kept slots all default to target 255, the master:
  ```cpp
  if (punch_target == 255) punch.process(master_l, master_r, frames);
  flex.process(master_l, master_r, frames); // held or not: its history always holds what just played (run_live())
  run_live(255, master_l, master_r, frames);
  ```
  and each `Punch::process` walks the frames with its checks before storing one sample a side:
  ```cpp
  float amount = held ? depth * float(std::min(1.0, double(begun + 1) / ease_in)) : fading * depth;
  if (amount <= 0 && frozen) frozen = false; // the release is over: the history runs again
  ...
  if (!frozen && !lines) { left[write] = dry_l; right[write] = dry_r; }
  ```
  With no punch held the ten histories (1.5 MB each, 15 MB in all) receive the same signal.
- **Cost:** unmeasured: 10 × 128 frames × about 10 cycles is 13 000 cycles, 7 µs a block, plus twenty separate write streams of 512 bytes each through a 32 kB L1 every block. About 0.3 % of a core at 44.1 kHz, 0.7 % at 96 kHz, always.
- **Proposal:** a block path at the top of `Punch::process`: when `!held && fading <= 0`, set `frozen = false`, copy the block into the history with one or two `memcpy` (wrap), advance `write` and `fresh`, return. With HOST-1 step 3 the copies stop once the history holds only silence. A larger change, not proposed now: one shared history per target for the slots that are off.
- **Gain:** CPU Low; battery Low.
- **Sound or behaviour change:** bit-exact (the same samples reach the history; no other state moves when `amount` is 0).
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** High on the count of ten (defaults read, and `punch.h:57` says a load clears ten); Medium on the time.

### HOST-7: Fixed work repeated every block that can be kept or skipped
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

## Checked and fine

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

## Could not determine

- All times. On the device: `status` (`melodee_render_load`, `melodee_render_parallel`), `melodee-freeze status` (`track_load=`), About's core loads, `melodee-render serial|parallel`, and the bench. The bench needs a stopped mode and the stems/input/USB arguments to cover HOST-1, HOST-7 and HOST-8.
- Whether the loops of HOST-4 are vectorised by the project's real compiler (`-fopt-info-vec-missed` answers it in one compile).
- Whether `clock_gettime(CLOCK_MONOTONIC)` is served by the vDSO on this kernel. If it is a real system call, the 68 reads a block of HOST-7 cost about 1 % of a block instead of 0.1 %.
- How long `TrackLoad` (`engine_host.cpp:559-579`, `load_track` at `:343-392`: 128 pads set, the chain and any pad effects rebuilt) holds the audio thread on a track paste or undo.
- The idle cost of `SamplerEngine::render_stereo` with no voice, and whether it can report "nothing written this block" for HOST-1 step 1 (sampler auditor).
- Whether the 32-bit VST2 bridge process should set FZ in FPSCR (plug-in auditor); and the size of the subnormal penalty on the A55 in either mode.
- The power the ADC path draws by staying routed for Audio Recall (`shell/input_route.h:19`); a Settings switch for Audio Recall could let it power down. Hardware measurement needed.
- Whether rarely used shared-library pages on the eMMC (libm's `tanhf`, used only above -1 dBFS) can be evicted and fault on the audio thread; there is no `mlockall` in `base/src` or `src`.
- Cache sizes of the RK3566 (I assumed 32 kB L1D per core and a small shared L3 from memory, not from a source in the repository).
