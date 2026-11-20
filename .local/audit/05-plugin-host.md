# Audit: plug-in hosting (`src/melodee/plugin/`, PLUG)

## Coverage

**Read fully:** every file in `src/melodee/plugin/` (`plugin_rack.cpp/.h`, `clap_instance.cpp/.h`, `vst2_instance.cpp/.h`, `vst2_bridge_instance.cpp/.h`, `vst2_bridge_protocol.h`, `vst2_abi.h`, `bridge32/vst2_bridge32.cpp`, `bridge32/support_pack.S`, `plugin_runner.h`, `idle_gate.h`, `plugin_sleep.h`, `rate_bridge.h`, `polyphase.h`, `host_time.h`, `nam.cpp/.h`, `plugin_files.cpp/.h`, `plugin_support.cpp/.h`, `drum_map.h`, `plugin_panel.h`, `preset_rows.h`), `src/melodee/engine/plugin_engine.h`, `src/melodee/shell/plugin_jobs.cpp`, `tools/plugin_support/build.py`, `make_stubs.py`, `Dockerfile`.

**Skimmed (call sites only):** `engine/engine_host.cpp` (`run_chain` 1305, `play_calls` 1260, `render_engine` 1247, `plan_compensation` 1506, `take_plugins` 1550, the clock at 2687–2699, the engine jobs at 2776–2786), `engine/effects.h` (`EffectClock`), `shell/lights.cpp` (the 5 ms tick), `shell/shell.cpp` 848–853 (snapshot), `shell/delete_jobs.cpp`, `shell/print_engine.cpp` 151–157, `shell/context.cpp` 72–88 (save), `model/project.h` (`PluginRef`, `plugin_used_by`), `base/src/dsp/frame_ring.h` (kernel constants only), `docs/MELODEE.md` (Plug-ins, Idle plug-ins), `tools/vst_probe/README.md` (bridge measurements), `tools/plugin_support/SURVEY.md`, the idle-gate tests in `tests/melodee.cpp` 11985–12020.

**JUCE:** the brief says JUCE is not checked out, but `build/deps/JUCE` (9.0.5, `d60b7a7`) is present on this machine. I did not audit it. I opened it only to verify five facts that the questions about threads and allocation depend on; each is cited with its JUCE path where used.

**Built artefacts, read-only:** I disassembled the two spin loops with `objdump` (`build/plugin-support/bridge32/vst2_bridge32`, `build/obj/melodee/plugin/vst2_bridge_instance.o`) to count instructions per iteration. Nothing was built or run.

**Skipped:** `tools/vst_probe/vst_probe.cpp`, the CLAP headers, the rest of JUCE, and the plug-ins themselves (Neural Amp Modeler, TONE3000, Open MPC binaries are not in this repository).

All CPU figures below are estimates from reading code, not measurements, unless a source is given.

## Answers to the area questions

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

## Findings

### PLUG-1: An instrument plug-in never sleeps while the song plays
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

## Checked and fine

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

## Could not determine

- Real spin and wake times on the device (PLUG-2), and how long real armv7 plug-ins take in `setProgram`/`getChunk` (PLUG-3).
- Wakeups from threads that plug-ins start themselves (a JUCE-built plug-in brings its own timer thread, and its messages wake the host's message thread). Count per loaded plug-in with `/proc/<pid>/task/*/status`.
- Whether armv7 plug-ins slow down on denormals: the host sets `FPCR.FZ` on the audio thread and workers (`engine_host.cpp:18–24`, `render_pool.cpp:26–31`), the bridge never sets `FPSCR.FZ`. An instrument kept awake by playback (PLUG-1) can sit on denormal state indefinitely. Setting the bit in the bridge would be a rounding-only change.
- Neural Amp Modeler's own DSP (scalar or NEON, direct or partitioned convolution for the IR): it is in the plug-in's fork, not here. The documented 17 % for an 8192-tap IR is consistent with direct convolution.
- How long `malloc_trim(0)` (`plugin_rack.cpp:1231`) holds glibc's arena locks with a large heap, and whether the MIDI thread waits on it when a plug-in is freed.
- Sizes of real bundles' data (PLUG-7) and libraries (PLUG-6).
- For the engine-host auditor: `PluginHandle::asleep` is not visible through `PluginRunner`, and `PluginEngine::sounding()` returns true whenever a plug-in is attached (`plugin_engine.h:87`); nothing reads either. If the host ever skips a silent track's chain and mix, the gate's state would have to be exposed.
