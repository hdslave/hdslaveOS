# Audit: effects and offline DSP analysis

All paths are under `src/melodee/` unless they start with `base/` or `docs/`.

Every time figure in this report is an **unmeasured estimate** from counting operations in the source. I assumed, for a Cortex-A55 at 1.8 GHz running glibc's float maths: one `sinf`/`exp2f` call about 25 to 40 ns, `powf`/`log10f`/`tanf` about 35 to 50 ns, `tanhf` about 60 ns, one float division or square root about 6 to 7 ns. A 128-frame block lasts 2.9 ms at 44.1 kHz, so 100 ns per stereo frame is 12.8 µs per block, or 0.44 % of one core. The first step before acting on the ranking is a per-effect benchmark on the device (see "Could not determine").

## Coverage

- **Read fully:** `engine/effects.cpp`, `effects.h`, `effects_extra.cpp`, `effect_info.cpp/.h`, `pad_fx_info.cpp/.h`, `master_comp.cpp/.h`, `dsp.h`, `rbj.h`, `fft.h`, `onsets.cpp`, `auto_chop.cpp`, `auto_probe.cpp`, `root_find.cpp`, `loop_find.cpp`, `release_fit.cpp`, `dsp_bench.cpp`, `plugin/idle_gate.h`.
- **Read in part (to learn who calls what, and how often):** `engine/engine_host.cpp` (`run_chain` 1305-1317, `render` 2619-3125, `play_loops` 3336-3371, `run_modulators` 1415-1454, `restore` 1569-1596, the denormal setup 18-25), `engine/render_pool.cpp` 26-33 and 135-136, `engine/sampler.cpp` 1736-1768 (pad lanes), `engine/macros.h/.cpp`, `shell/sample_library.cpp` (the analysis call sites 29-40, 987-1090, 1126-1173, 1309-1322), `shell/sample_memory.h`.
- **Skimmed (searched for loops, maps and maths calls, not read line by line):** `engine/sfz.cpp`, `sf2.cpp`, `exs.cpp`, `dspreset.cpp`, `engine_info.cpp`; `engine/punch.h` and the first 40 lines of `Punch::process`.
- **Skipped:** the bodies of `engine/punch.cpp` and `punch_mpc.cpp` (the Pad FX and Flex Beat DSP; `pad_fx_info.*` only describes them), the sampler voice code, plug-in hosting.
- Nothing in the repository was built, run or changed. One scratch file outside the repository included `effects.h` to print `sizeof(Effect)` (992 bytes) and `sizeof(MasterCompressor)` (4180 bytes).

## Findings

### FX-1: Track, bus, master and loop chains never rest: every effect runs every block, on silence too
- **Where:** `engine/engine_host.cpp:1305` (`run_chain`), called at `:2891` (tracks), `:3047` (buses), `:3108` (master), `:3368` (loop strips). `engine/effects.cpp:613` (`Effect::process`) has no silence test of its own.
- **Thread and frequency:** audio thread and render workers, per block, per effect, always.
- **Now:** a chain is skipped only when it is empty, and an effect only when it is bypassed and faded out (`effects.cpp:622`).
  ```cpp
  void EngineHost::run_chain(unsigned chain, float* left, float* right, unsigned frames) {
      for (unsigned k = 0; k < max_slots; ++k) {
          auto& effect = fx[chain][k];
          if (effect.kind() == EffectKind::None) break; // slots are packed from the front
          ...
          effect.process(left, right, frames);
      }
  }
  ```
  A track's chain runs whenever the track has an engine (`:2843`, `if (slots[order[k]].engine)`), muted or not, since mute is applied later at `:2960`. A bus chain runs whenever its first slot is not empty (`:2926`). The master chain always runs. Delay lines keep advancing, LFOs keep turning and compressors keep taking logarithms of silence.
  The code already rests four other things on the same idea: hosted plug-in effects (`plugin/idle_gate.h`: input and output under -100 dB for 0.5 s), Knob FX (`engine_host.cpp:3063-3077`), a kit pad's own chain (`sampler.cpp:1759-1767`) and a plug-in track's macro effects (`macros.cpp:98-101`). Melodee's own effects in the main chains are the one place left out.
- **Cost:** each always-on effect costs about 3 to 33 µs per block (128 frames at 20 to 260 ns, see the inventory), which is 0.1 to 1.1 % of one core. The project that `dsp_bench.cpp` builds (32 track effects, 4 bus effects, the master compressor) would spend roughly 2.8 µs per frame on effects, about a tenth of one core, with the transport stopped and nothing sounding. Measure: open a project with effects, stop, wait for the tails, and read the DSP load in `status` or Settings AUDIO; or add a mode to `--melodee-dsp-bench` that does not send `Play`.
- **Proposal:** apply the plug-in rule per chain in `run_chain`. Keep one quiet counter per chain. Before the loop, test the input block against `IdleGate::quiet_level` (1e-5). If the input is quiet and the counter has reached its limit, skip the Melodee effects of that chain (plug-in slots keep their own gate). Otherwise run the chain, test the output, and reset or advance the counter.
  ```cpp
  bool fed = loud(left, right, frames);                 // as IdleGate::loud()
  if (!fed && chain_quiet[chain] >= chain_rest[chain]) { run_plugins_only(...); return; }
  ... the loop as now ...
  chain_quiet[chain] = fed || loud(left, right, frames) ? 0 : chain_quiet[chain] + frames;
  ```
  Points to settle: (1) the quiet time must be longer than the longest delay in the chain, so a late repeat is not lost; a synced echo can be `DelayPool::half - 64` frames long (3.3 s at 44.1 kHz), so use that for chains holding a delay-memory kind and 0.5 s otherwise. (2) Do not rest a chain with a keyed slot (`fx_key`) or a Pumper in the first version: their detector or gain state follows something other than the input. (3) Vinyl, Tape emulator and a Looper holding a loop make sound from silence, so their output never goes quiet and they never rest, as now. Do not copy the 30 s forced stop that Knob FX and pad lanes use: on a track chain it would silence those three.
- **Gain:** battery Medium: about a tenth of one core for a project with many effects, whenever the transport is stopped, and a share of it for every track that is silent while others play. It is the only saving in the effect code that applies while nothing sounds. No gain in the worst-case block.
- **Sound or behaviour change:** not bit-exact. Tails are cut once they are under -100 dBFS, the rule hosted plug-in effects already follow. A free-running LFO resumes where it stopped instead of where it would have been.
- **Risk:** Medium. It sits in the main mix path; latency compensation and the keyed run must keep working; the host tests that compare output sample for sample will need a look.
- **Effort:** M.
- **Confidence:** High that nothing gates these chains today (read every caller of `run_chain`). Medium on the size of the saving. I did not check how the shell's meters or the freeze logic react to a skipped chain.

### FX-2: The slot Compressor calls `log10f` and `powf` on every sample, also when it is not compressing
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
- **Where:** `shell/sample_library.cpp:36-40` (`analyse_sample`), called at `:1084` for every sample read into memory and at `:681`, `:915`; `:1170` (`stream_shape`) for every long sample. The work is `engine/onsets.cpp:76` (`analyse`), `:234` (`track_beats`) and `engine/auto_chop.cpp:71` (`read_auto`).
- **Thread and frequency:** loader pool (three threads at nice 5), once per sample on every project open and every sample load.
- **Now:**
  ```cpp
  void analyse_sample(const SampleData& data, LoadedSample& loaded, bool find_note = true) {
      const auto mono = model_mono(data);
      loaded.hit_count = find_hits_in(mono, loaded.hits, loaded.beats, &loaded.autos);
      if (find_note) loaded.root = found_root(mono);
  }
  ```
  The note is found once and then kept in the project (`find_note` is false when `SampleRef::root` is known, `shell/lights.cpp:318`). The hits, beats and auto-chop reading are not kept anywhere, yet only Chop (`shell/context.cpp:1847-1858`, `shell/chop_mode.cpp:48`, `pages/chop_page.cpp`), the record-chop snap (`shell/sample_files.cpp:345`) and the Stage page's hit marks (`pages/stage_page.cpp:635`) read them. For a sample over 10 s, `stream_shape` reads the whole file from the card again and, when memory allows, analyses all of it.
- **Cost:** the analysis is a 2048-point FFT for every 20 ms of audio, plus per FFT about 2,050 complex magnitudes (`std::abs`, a `cabsf` call each, `onsets.cpp:105-106`) and about 3,000 float divisions in `band_sum` (`onsets.cpp:69`). I estimate 10 to 20 ms of CPU per second of audio. A kit of one-shots costs well under a second. A five-minute stem costs about 3 to 6 s of CPU plus reading about 50 MB from the card, on every project open. The app already prints the real figure: `load: ... threads: read X, analyse Y ms` (`sample_library.cpp:1318`).
- **Proposal:** in order of value.
  1. Do not repeat the work: keep each file's overview, hits, beats and auto reading in the card's `Melodee/Cache` (keyed by path, size and modification time), or compute hits lazily when Chop first asks. For long files this also removes the full read of the file.
  2. In `analyse`, compute the triangle weights of `band_sum` once per band instead of dividing per bin per frame (the weights do not depend on the frame). Bit-exact if the same expression fills the table.
  3. In `track_beats`, the inner loops call `std::log` (`onsets.cpp:295`) and `std::exp` (`:286`) with values that depend only on the loop offset; a small table per call removes about 40 to 230 `log` calls per 10 ms frame.
- **Gain:** load time Medium (High for projects with long files), battery Low to Medium.
- **Sound or behaviour change:** none (1 changes when the work happens; 2 and 3 are bit-exact).
- **Risk:** Medium for a cache (stale entries, card writes), Low for 2 and 3.
- **Effort:** M for 1, S for 2 and 3.
- **Confidence:** High that nothing is cached (searched `shell/sample_cache.*` and the call sites). Medium on the time. The call sites are in the shell, outside my area. The Stage page draws the hits, so a lazy version must still deliver them soon after a load.

### FX-6: The master compressor does its full work on silence, and Color adds a `sin` and five divisions per sample
- **Where:** `engine/master_comp.cpp:64-88` (`process`), `:37-50` (`colour`), `:90-107` (`duck`); called every block at `engine/engine_host.cpp:3120`.
- **Thread and frequency:** audio per sample while the compressor is on.
- **Now:** the gain computer runs every fourth sample whatever the level, and Color runs a 0.6 Hz wobble through `std::sin` per sample:
  ```cpp
  if ((f & 3) == 0) { // the gain computer every fourth sample, the gain ramping between
      float over = 20 * std::log10(std::max(level, 1e-6f)) - threshold;
      float reduce = over > 0 ? over * (1 - 1 / ratio) : 0;
      most = std::max(most, reduce);
      step = (target(reduce) - gain) * 0.25f;     // target() calls std::pow
  }
  ...
  float back = (40 + 6 * std::sin(2 * dsp::pi * wobble)) * stretch; // 0.8-1 ms
  ```
- **Cost:** about 30 ns per frame without Color and about 110 ns with it (one `sinf`, `0.6f / fs()`, two `soft_clip` divisions, two `/ 1.1f`), so 4 to 14 µs per block, all the time the compressor is on.
- **Proposal:** (a) the same shortcut as FX-2: when `level` is under a guarded linear threshold, `reduce` is 0 and `target(0)` is exactly `makeup` (or 1 with Sidechain), so skip `log10f` and `powf`. (b) Take Color's wobble from the rotation of FX-3 and hoist `0.6f / fs()`. (c) Optional: return early when the input block is all zero, the envelope is under the threshold, the gain has settled and Color's 512-sample line and low-pass state are empty.
- **Gain:** CPU Low; battery Low (it is one instance).
- **Sound or behaviour change:** (a) bit-exact, (b) rounding only, (c) rounding only (the wobble's phase and the envelope's decay must be stepped over the skipped time).
- **Risk:** Low for (a) and (b), Medium for (c).
- **Effort:** S for (a) and (b).
- **Confidence:** High.

### FX-7: Stereo filter chains run as two scalar passes although both sides use the same coefficients
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

## Answers to the questions asked

### 1. Inventory of the 41 effects

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

### 2. Idle behaviour
- An **empty** slot ends the chain loop (`engine_host.cpp:1308`): free.
- A **bypassed** effect fades for 10 ms, then `process()` returns after a few branches (`effects.cpp:622`): free. It keeps its pool block.
- A **non-bypassed** effect never rests on its own. On silent input it still advances its delay lines (writing zeros), turns its LFOs and runs its detector. Delay lines are not re-cleared while idle; clearing happens only after a kind change, `clear()` or coming back from bypass, 16,384 floats per call (`effects.cpp:624-627`).
- Resting exists only outside `Effect`, for kit-pad chains, Knob FX, macro effects and hosted plug-ins. Track, bus, master and loop chains have none (FX-1).

### 3. Per-sample waste
See FX-2 (logarithm and power per sample), FX-3 and FX-10 (`sin` for LFOs and windows), FX-4 (block constants recomputed), FX-8 (divisions), FX-9 (member reloads) and FX-7 (stereo as two scalar passes). On the two remaining points: every modulo on a delay index is by a compile-time constant (see "Checked and fine"), and `double` is used per sample only in Pumper's beat position (`effects_extra.cpp:249-250`, two double divisions per frame), where the song clock needs it.

### 4. Denormals
No recursive structure has a guard of its own. The list: the reverb's eight combs with their damping one-poles and four allpasses, and Shimmer's feedback; the feedback lines and tone one-poles of Echo, Tape echo, Ping pong, Diff delay (plus its eight diffusers) and Flanger; Looper's KEEP; the allpass states and feedback of Phaser and Pedal phaser; Spring's two loops, 24 dispersion allpasses, damping and low-cut states; every biquad (EQ, Amp sim) and state-variable filter (Filter, Formant, Auto-wah, Vintage); the tone one-poles of Drive, Fuzz, Pedal chorus, Vinyl and Tape emulator; Tube drive's DC blocker; Vinyl's `pop *= fall[0]`, hiss and rumble; the envelopes and gain smoothers of Compressor, Gate, Transient, Limiter, Tremolo and Pumper; the master compressor's envelope, gain ramp and Color low-pass; `dsp::Ladder` and `dsp::Smooth`.

A global flush-to-zero covers all of them, and it is **already set**: `engine_host.cpp:18-25` sets FPCR bit 24 on the first block of any thread that calls `EngineHost::render` (`:2621-2624`, so print hosts too), and `render_pool.cpp:135-136` sets it on each render worker. It is set once per thread and never re-asserted; see "Could not determine" for hosted plug-ins.

### 5. Vectorisation
- Almost every effect loop is a feedback recurrence (combs, allpasses, biquads, one-poles, envelope followers) and cannot be vectorised across frames whatever the flags.
- g++ 12 at `-O2` runs the vectoriser with its "very cheap" cost model. As I understand it (not checked against this build), that model refuses any loop that needs a scalar remainder or a run-time aliasing test, so loops over a run-time `frames` stay scalar. `-fopt-info-vec-missed` on `effects.cpp` would confirm it.
- Loops that could vectorise with `-O3` or a fixed 128-frame count plus `__restrict`: the dry fade while a block is being cleared (`effects.cpp:630`), the reverb's predelay copy (`:283-289`, once the three `% pre_length` are split at the wrap) and its wet/dry mix (`:346-351`), and Width (`:548-551`). The last two are blocked today by `blend()` (`effects.h:103-107`), which carries the bypass fade from frame to frame through a member; computing the mix once for the block when no fade is running (`on == aim`) removes that and is bit-exact.
- The reverb's allpass loops (`:312-318`) have no dependence shorter than their line (242 frames or more), so splitting each at its wrap point would let them vectorise. The comb loops (`:298-304`) cannot: the damping one-pole is a true recurrence.
- The larger win is across the two channels, not across frames (FX-7).

### 6. The master compressor and metering
- The gain computer (`log10f`, `powf`) runs every fourth sample with a linear ramp between (`master_comp.cpp:75-81`). There is no look-ahead buffer; the only buffer is Color's 512-frame wobble line a side, inside the object, and its `% line` compiles to a mask because 512 is a power of two.
- Metering is cheap: per block one to three relaxed atomic stores and one `log10f` (`master_comp.cpp:52-57, 86-87`). `take_activity` is two atomic exchanges on the screen thread.
- Off, it returns at once (`:65`). On, it costs the same on silence as on signal (FX-6).

### 7. Analysis code
- `fft.h` is an iterative, in-place radix-2 FFT on `std::complex<float>`. It does not allocate; the caller passes the twiddle table, built once per analysis (`onsets.cpp:86-87`, `root_find.cpp:140-141, 252-253`). The bit-reversal order is recomputed on each call (about 5 % of a call). `analyse` already packs two real frames into one complex FFT (`onsets.cpp:100-107`). It never runs on the audio thread.
- Onset detection and auto-chop reading run when a sample **loads**, not when Chop opens, so Chop itself is instant: `chop()`, `auto_slices()` and `pick()` work on at most 512 stored hits. The load cost is FX-5.
- `read_auto`'s two median filters (`auto_chop.cpp:119-129`) do two `nth_element` calls of 17 bytes per band per frame, perhaps 0.2 s for a minute of audio; a stride caps the work at 12,000 frames (`:116`).
- `find_root` is bounded to the first 8 s and 48 windows (`root_find.cpp:36-37`), about 0.1 to 0.15 s for a long sample, and runs once per sample ever. Its chord path recomputes the Hann window with a double `cos` per sample for each window (`root_find.cpp:151`: 4,096 calls x up to 48 windows); a table removes about 15 ms.
- `auto_probe.cpp`, `loop_find.cpp` and `release_fit.cpp` are called only from the Auto Sample job (`shell/print_engine.cpp:834-1335`). The slowest case I found is `probe_glide` (`auto_probe.cpp:91-97`): up to 500 calls of an O(window x lags) `probe_pitch`, a few seconds when the pitch never settles. It is a one-off job, so I do not rate it.
- The instrument readers (`sfz.cpp`, `sf2.cpp`, `exs.cpp`, `dspreset.cpp`) parse text or headers only, in linear passes with `std::map`; nothing there costs seconds.

## Checked and fine

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

## Could not determine

- **Real cost per effect on the device.** `--melodee-dsp-bench` times whole blocks of one busy project. That project uses 15 of the 41 kinds (Chorus, Drive, Phaser, Filter, Crush, Echo, Compressor, Width, Flanger, Tape, Tape echo, Ping pong, Reverb, Plate, Shimmer) and none of the thirteen newer ones (`effects_extra.cpp`). A loop over every kind that calls `Effect::process` on noise and on silence and prints nanoseconds per frame would replace every estimate in this report; `Effect` is a public class, so it is about 30 lines beside `dsp_bench`.
- **How much of a busy block is effects.** `docs/ROADMAP.md:763-767` records the bench at 48 kHz as a mean block of 1,376 to 1,487 µs and 187 to 192 % CPU. My estimates put the effects at roughly 5 to 10 % of that CPU time, with the sampler voices (and the workers' waiting) taking most of the rest; that split is not measured.
- **Whether a hosted plug-in can clear flush-to-zero.** The flag is set once per thread. If a plug-in's process call restores a floating-point state without it, nothing sets it again. Re-asserting it at the top of `render()` costs two instructions.
- **Cache sizes and the cost of the memory traffic in FX-11.** I recall Rockchip's data sheets giving 32 kB of L1 data cache per core and one shared 512 kB L3; I did not verify it, and the cost of cold cache lines needs `perf stat` on the device.
- **Which maths library the device runs.** My call costs assume glibc 2.36 as in the Debian bookworm build container.
- **What g++ actually emits** for the points in FX-4, FX-7 and FX-9 and for answer 5 (no cross-compiler here; nothing was built).
- **The Pad FX and Flex Beat DSP** (`punch.cpp`, `punch_mpc.cpp`) were not audited. From the header and the first lines of `Punch::process`: each `Punch` holds two 192,000-float history lines (1.5 MB) and writes them every block also while off (`punch.cpp:107`), and `flex.process` runs on the master every block (`engine_host.cpp:3118`).
- **Sample analysis time on the device:** read it from the `load:` line the app prints (FX-5).
