# Audit: LEDs, MIDI input/output and control bindings

## Coverage

Read fully (`src/melodee/shell/`): `lights.cpp`, `pad_lights.cpp`, `pad_lights.h`, `pad_light.h`, `pad_colour.h`, `light_show.h`, `meter_lights.h`, `midi_io.cpp/.h`, `mtc_in.cpp/.h`, `midi_route.cpp/.h`, `midi_ports.h`, `bindings.cpp/.h`, `fader.cpp/.h`, `fader_job.cpp/.h`, `knob_fx.cpp`, `pad_fx.cpp`, `roll.cpp/.h`, `roll_edit.cpp`, `note_repeat.cpp/.h`, `levels_keys.cpp/.h`, `pad_bank.cpp/.h`, `button_jobs.cpp/.h`, `shift_pads.cpp`, `shift_pad_jobs.cpp`, `takeover.h`, `tap_hold.h`, `controls_rules.h`, `pad_context.h`, `autosave.h`, `stream_holds.h`, `sound_list.h`. Base: `surface/leds.h`, `surface/led_colours.h`, `drivers/hardware_midi.cpp/.h`, `runtime/controller.cpp/.h`, the queue and packet types in `surface/midi.h`, `surface/controls.h`. App: `melodee_app.cpp` lines 1-260 and 425-520 (`surface`, `midi`, `midi_output`, `lights`, `draw`, `service`). Engine: `engine/midi_out.h`, `engine/voice_tap.h` (first 80 lines).

Read in part, because `Shell::lights()` or the input path calls them: `shell.h`; `shell.cpp` 60-300 and 556-960 (`surface`, `midi`, `midi_output`, `pot`, `wheel`, `pad`, `hit`, `snapshot`); `context.cpp` (`send`, `autosave`, `save_project_to`, `switch_to`, `reload_tick`, `song_tick`, `free_tick`, `key`, `play_pad`, `pressure`, start of `apply`, `flush_motion`, `take_arp_notes`); `plugin_jobs.cpp` 287-312; `plugin/plugin_rack.cpp` (`sync`, `set_card`, `panel`, `run`); `chain_jobs.cpp` (`chain_tick`); `erase_copy.cpp` (`live_erase_tick`); `looper_jobs.cpp` (`looper_tick`); `capture_jobs.cpp` (`capture_tick`); `chop_mode.cpp` (`chop_tick`); `sample_files.cpp` (`files_landed` start, `browser_tick`); `sample_record.cpp` (`record_tick`); `scene_jobs.cpp` (`follow_tick`); `undo_restore.cpp`, `undo_scope.h`; `model/slot_table.h`, `model/midi_config.h`; `engine_host.cpp` `take_commands` and the render loop around line 2650-2760; `settings.h` 20-110.

Skimmed or not read: `Shell::press` (`shell.cpp` 296-556), `Context::apply` past line 640, `print_tick`, `mpc3_tick`, `stems_tick`, `stage_tick`, `lesson_tick`, `land_instruments`, `stems_landed`, the usage log, `card_follow`. JUCE is absent and was not audited.

One measurement: `sizeof` of a few structs, from a ten-line probe compiled in my scratch directory against the repository's headers (host clang, arm64, libc++; the device's g++/libstdc++ may differ by a few bytes where `std::string` is a member). Nothing in the repository was built, run or changed. Results: `Project` 538,968 bytes, `Track` 32,312, `UiState` 8,160, `Command` 200, `Binding` 20; `MidiPacket` is 272 by its definition. Every CPU-time figure below is an estimate from reading the code, not a measurement.

## Findings

Short answers to the assignment's questions, with the finding that carries the detail:

1. `lights()` recomputes everything from scratch on every call: all sixteen pads, the fader, eight bank lights and about twenty buttons, plus about twenty housekeeping ticks and a block of `status` values. Nothing is gated on change; only the meters (50 ms), stream holds (100 ms) and plug-in card texts (50 ms) are rate-limited. 200 Hz is not needed; 100 Hz would look the same and an adaptive rate is possible (LIGHT-1, LIGHT-2, LIGHT-3).
2. Stopped on a static page: no LED messages. A pad hit and release: about 50-70 SysEx. Playing a drum pattern: a few hundred SysEx a second; the Tracks page or mute mode while playing: up to about 1,400. Fades do send one SysEx per one-step change per tick; nothing quantises or rate-limits beyond exact-value dedupe and 48 messages a pass (LIGHT-8).
3. No global brightness and no idle dim or sleep. Rest states are listed in LIGHT-4, with where to put the multiply.
4. Meters: levels are read every 50 ms from two atomics, turned into a 5-bit mask per strip and sent as CC 83/84 only when the mask changes, so at most 20 messages a second per strip (see Checked and fine).
5. Clock, MTC and CV events are made on the audio thread only while the transport runs and the output is switched on; the MIDI thread sleeps until each event's time (`wake_at`), one wake per pulse. No polling. One bug: a busy loop while a SysEx goes out (LIGHT-5).
6. `surface()` and `midi()` cost little per message: one mutex; pressure allocates nothing; a pad press allocates only for its undo step, on the kit, Roll and Step Edit pages; a short `snprintf` for notes, buttons and knobs. Pressure is not coalesced (LIGHT-9). Two side effects are worse than the per-message cost: the lock they share with the screen thread (LIGHT-7) and the `dirty` flag (LIGHT-6).
7. Dispatch is by `switch` and a page table of function pointers; nothing is rebuilt per event except the three-card `page_bindings()` array (60 bytes, no allocation) (Checked and fine).
8. No light is stuck sending from float noise; compares are made on the rounded 0-127 values. The bugs found are LIGHT-1 (8192-slot walk), LIGHT-5 (busy loop) and LIGHT-6 (needless saves).

### LIGHT-1: The `status` mirror in `Shell::lights()` is rebuilt every 5 ms, with a walk over all 8192 sample slots
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
- **Where:** `src/melodee/shell/shell.h:146`; `src/melodee/shell/shell.cpp:93-94,118-119,127-128` (MIDI side), `:804-815,878-894` (`Shell::snapshot`); `src/melodee/shell/lights.cpp:180`.
- **Thread and frequency:** MIDI per message and per pass; screen per frame (every 16.7 ms while the picture changes, else every 100 ms).
- **Now:**
  ```cpp
  804 void Shell::snapshot(Snapshot& out, uint32_t now) {
  805     std::lock_guard<std::mutex> guard(lock);
  810     out.ui = ui_;
  813         out.project = *project_;
  ```
  The sketches of all 16 x 128 patterns (`:878-894`), the pattern in view and the sample views are built inside the same lock. `Project` is about 539 KB plus 20 KB for each used block of 64 sample slots. The lock is a plain `std::mutex` with no priority inheritance; the MIDI thread is `SCHED_FIFO` 15 and the screen thread is not real-time.
- **Cost:** the MIDI thread can be held for the length of the copy (perhaps 0.3-1 ms, unmeasured) up to 60 times a second. If the screen thread is preempted while it holds the lock, as it can be when the audio thread and render workers are busy, the MIDI thread waits behind a thread of lower priority. A hand-played note is placed by its kernel stamp one block later (`engine_host.h:112-118`), so more than about 2.9 ms of delay makes it late; the engine counts these (`timed_missed()`, `timed_worst_frames()`, `engine_host.h:132-133`), and MIDI clock out reports its lateness in `melodee_midi_late_us`.
- **Proposal:** make `Shell::lock` a priority-inheritance mutex; the repository already has one (`src/melodee/plugin/vst2_bridge_instance.h:28-46`). Shortening the hold (copy the project only when a change counter moved, sketch a pattern only when it changed) belongs to the screen audit.
- **Gain:** latency, Medium.
- **Sound or behaviour change:** none.
- **Risk:** Low (a small wrapper type for the `std::lock_guard` users).
- **Effort:** S for the mutex.
- **Confidence:** Medium. The hold time is an estimate; the inversion follows from the priorities and was not observed.

### LIGHT-8: LED traffic: one SysEx for every one-step colour change, one `write()` for every message
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
- **Where:** `base/src/runtime/controller.cpp:129-134`; `base/src/drivers/hardware_midi.cpp:25-65` (`enumerate`), `:83-108` (`rescan`), `:202-231`.
- **Thread and frequency:** MIDI, once a second for as long as the app runs.
- **Now:**
  ```cpp
  129            if (now >= scan) {
  130                if (midi.rescan()) app.midi_lost();
  132                MidiCatalog update {midi.json(), midi.outputs(), midi.open_ports()}; catalog_updates.push(update);
  133                scan = now + std::chrono::seconds(1);
  134            }
  ```
  `enumerate()` opens every sound card's control device (`snd_ctl_open("hw:N")`), asks each rawmidi device for both directions and builds strings. Then `json()` (an `ostringstream`), `outputs()`, `open_ports()` and the copy into the queue allocate again, whether or not anything changed.
- **Cost:** perhaps 0.3-1.5 ms each second at real-time priority (alsa-lib's name lookup, several opens and ioctls a card, dozens of allocations): little CPU, but a pad or MIDI message that arrives meanwhile waits that long. Unmeasured; time `midi.rescan()` with two clock reads.
- **Proposal (sample-base):** rescan on change. Add an inotify watch on `/dev/snd` to the `ppoll` set in `HardwareMidi::wait`; keep the rescan after a read or write error (the port is already closed there, `hardware_midi.cpp:140-147`) and a slow fallback every 30 s. Build the JSON and the lists only when `generation()` moved.
- **Gain:** battery Low; latency Low to Medium.
- **Sound or behaviour change:** none; a new USB device is seen as fast or faster.
- **Risk:** Low to Medium (hot-plug needs a test on the device).
- **Effort:** M.
- **Confidence:** Medium (the cost is an estimate).

### LIGHT-11: The idle MIDI loop reads every port and allocates a `std::function` on each pass
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

## Checked and fine
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

## Could not determine
- What `Shell::lights()` and a whole pass of the MIDI loop cost on the device. It needs per-thread CPU time (`perf stat -t`, or `/proc/<pid>/task/<tid>/stat`) with and without each change.
- LED current: per drive step for the pads, for a button at Dim and at Bright, and whether the microcontroller's dimming is linear at the bottom. It decides whether LIGHT-4 is Medium or High.
- How fast the surface sends pressure for a held pad, and whether it repeats unchanged values.
- What a mid-block run boundary costs in the engine with one-dispatch blocks on, and whether the engines smooth pressure (LIGHT-9).
- The longest hold of `SampleLibrary::lock`, `PluginRack::lock` and `UpdateLink::lock` by the service and loader threads; `lights()` takes each several times a tick. `PluginRack::panel()` asks the plug-in for parameter names and value texts under the rack's lock on the MIDI thread, 20 times a second while a plug-in's cards are in view (`plugin_rack.cpp:617-650`); a bridged VST2 may make that slow.
- How long `Shell::snapshot()` holds `Shell::lock` (LIGHT-7), and how often `timed_missed()` rises in normal use.
- Whether the commands sent at start-up (`MidiIo::configure`, `record_tick`, `send_repeat_velocity()` at `lights.cpp:298`) leave an untouched project dirty and so cause one save on every start (LIGHT-6).
- What alsa-lib's enumeration costs on the device (LIGHT-10).
- Whether remote test scripts rely on `status` being fresher than 100 ms (LIGHT-1).
