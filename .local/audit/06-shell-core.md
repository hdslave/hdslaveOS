# Audit: shell core, periodic service work, settings, undo and autosave

## Coverage

**Read fully:** `src/melodee/melodee_app.cpp/.h`, `src/melodee/crash_log.cpp/.h`; in `src/melodee/shell/`: `shell.cpp`, `shell.h`, `context.cpp`, `context.h`, `ui_state.h`, `settings.h`, `autosave.h`, `usage_log.h`, `history.h`, `snapshot.h`, `undo_scope.cpp/.h`, `undo_restore.cpp`, `playheads.h`, `features.h`, `page_stack.cpp/.h`, `menu.cpp/.h`, `ask.cpp`, `lesson.h`, `lesson_jobs.cpp`, `update.h` and the `UpdateLink` part of `update.cpp`, and all of `lights.cpp` (it holds `Shell::lights()`, `autosave_tick()` and `update_tick()`, which is where every shell tick is called from; I report only its non-LED work). Also `src/melodee/pages/settings_page.cpp`, `docs/guide/settings.md`, `model/limits.h`, `model/slot_table.h`, and in the base `runtime/run_app.cpp`, `runtime/controller.cpp`, `runtime/control_socket.cpp`, `runtime/app.h`, `runtime/session.h`, `drivers/platform.cpp/.h`, `drivers/card_share.h`.

**Skimmed (only the functions my area calls, to learn cost and frequency):** `shell/project_store.cpp/.h` (`service`, `run`, `scan`, `write_settings`), `shell/sample_library.cpp` (`service`, `scan`, `list_into`, the accessors), `shell/looper_files.cpp`, `shell/midi_track_jobs.cpp` (`midi_audio_tick`), `shell/sample_record.cpp` (`record_tick`), `shell/plugin_jobs.cpp` (`plugins_tick`), `plugin/plugin_rack.cpp` (`sync`, `set_card`, `usage`, the thread loop), `shell/audio_jobs.cpp`, `shell/audio_tracks.cpp` (thread loop), `shell/stream_holds.h`, `shell/free_length.h`, `shell/scene_jobs.cpp` (`follow_tick`), `shell/chain_jobs.cpp`, `shell/capture_jobs.cpp`, `shell/looper_jobs.cpp` (`looper_tick`), `shell/input_route.cpp`, `shell/card_warm.cpp` (`start`/`stop`), `shell/mpc_wav_slices.cpp` (`read_mpc_slices`), `shell/mpc_program.cpp` (`is_drum_program`), `web/web_app.cpp/.h`, `engine/looper.cpp` (`service`), `model/project.h`, `model/pattern.h`, `model/sampler.h`, `model/project_file.cpp` (start of `save_project`), base `drivers/sd_update.cpp` (`find`), and the `poll()` functions of the base drivers the service loop calls.

**Skipped:** the LED code (`pad_lights.cpp`, `light_show.h`, the second half of `Shell::lights()`), `midi_io.cpp`, `bindings.cpp`, `print_engine.cpp`, everything in `engine/` except the two functions above, `pages/` except Settings, `ui/`, `web/web_server.cpp`, `tools.cpp`, tests.

Nothing was built or run. Struct sizes below are counted by hand from the definitions (not compiled); every cost figure is an unmeasured estimate unless it says otherwise.

## Findings

### SHELL-1: Engine-mirror commands mark the project dirty, so some projects autosave about once a second while the device sits idle
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
- **Where:** `src/melodee/shell/looper_files.cpp:70` (`save_loop_files`), `:174-188` (`SampleLibrary::loop_service`), called from `src/melodee/shell/context.cpp:76` (`Context::save_project_to`)
- **Thread and frequency:** queued on the MIDI thread at every save; written on the service thread, one loop per pass. An autosave happens 3 s after the controls go quiet following any change (`autosave.h:12`), and it is not held off while loops merely play (`lights.cpp:70` counts only `looper().recording()` and `armed()`).
- **Now:**
  ```cpp
  // looper_files.cpp:83-85
  SampleLibrary::LoopFiles job;
  job.save = true; job.dir = loop_dir(path); job.mask = mask; job.loops = lp.loops;
  c.library->loop_files(std::move(job));
  // looper_files.cpp:181-186 (service thread, for each loop in the mask)
  adopt(data, std::vector<float>(looper.held_left(n), looper.held_left(n) + frames),
        std::vector<float>(looper.held_right(n), looper.held_right(n) + frames), v.rate);
  looper.release(n);
  fs::create_directories(loop_job.dir, error);
  auto failed = write_wav(file + ".new", data); // at the loop's own rate
  if (failed.empty()) fs::rename(file + ".new", file, error);
  ```
  There is no check that the loop's audio differs from the file already there.
- **Cost:** The file is 24-bit stereo at the device rate (header comment, `looper_files.cpp:3-6`); a loop may be 64 s (`engine/looper.h:29`). An 8-bar loop at 120 BPM and 48 kHz is about 4.6 MB; four of them about 18 MB; the worst case is about 18 MB per loop. With the Looper on, turning one knob and pausing costs that much card writing plus two full float copies of each loop in RAM, and each write blocks the service thread (jack, power key, sample-load hand-off) for its duration. Measure: bytes written per save from `/proc/self/io` (`write_bytes`) around an autosave with four loops held.
- **Proposal:** Give each loop a content serial and skip the write when the serial matches the one last written and the file exists. The Looper already publishes per-loop state through `Looper::View` (`engine/looper.h:164-176`); add a `uint32_t writes` counter there that the audio thread bumps whenever it finishes writing into a loop (take end, overdub end, undo, clear, fit swap, load). In `loop_service`, keep `std::array<uint32_t, loop_tracks> saved_serial` and `saved_dir`; before `looper.hold(n)` do `if (v.writes == saved_serial[n] && loop_job.dir == saved_dir && fs::exists(file)) { ++loop_at; continue; }`. Save As (another `dir`) still writes everything.
- **Gain:** battery High and card wear High whenever the Looper holds audio; CPU Medium on the service thread.
- **Sound or behaviour change:** none (bit-exact; the files on the card are the same).
- **Risk:** Low to Medium: a missed bump would leave a stale loop file. Bump in one place (where the audio thread publishes the loop's view after a write) rather than per feature.
- **Effort:** M.
- **Confidence:** High that every save rewrites every kept loop; I did not read the Looper engine closely enough to list every place that changes a loop's audio.

### SHELL-3: There is no idle policy at all: nothing dims, slows or powers off, and no "time since last activity" is published
- **Where:** `src/melodee/melodee_app.cpp:529-535` (`MelodeeApp::service`, the only place the backlight is set), `src/melodee/shell/shell.h:177` (`last_input_`), `src/melodee/shell/settings.h:88`
- **Thread and frequency:** always.
- **Now:** The backlight follows one fixed setting and is written only when that setting changes:
  ```cpp
  // melodee_app.cpp:530-535
  unsigned screen = shell.screen();
  if (screen && screen != screen_applied) {
      auto backlight = hardware.platform.backlight();
      if (backlight.maximum > 0) hardware.platform.brightness(unsigned(std::max(1L, backlight.maximum * long(screen) / 100)));
      screen_applied = screen;
  }
  ```
  A search of `src/melodee` and `base/src` for dim, screensaver, auto-off, eco, governor, cpufreq and `supplies(` finds nothing but the `Platform` readers themselves (`platform.cpp:179`) and a base demo. The only idle timer is `Shell::last_input_`, private to the MIDI thread and used only by autosave.
- **Cost:** At idle the device keeps the backlight at the set level, the pad and button LEDs lit, the CPU at the `performance` governor, the screen thread drawing 10 frames a second and every 5 ms loop running, for as long as it is switched on. The backlight is very likely the largest single consumer after the SoC (not measured; measure by reading the supply voltage/current on a bench supply at brightness 1 and 7).
- **Proposal:** Add a **Power** policy with three parts; all the plumbing already exists in the shape `Brightness` uses.
  1. *Activity signal.* In `Shell`, publish `std::atomic<uint32_t> last_input_ms` wherever `last_input_` is written (`shell.cpp:96`, `:130`, `lights.cpp:66`) and `std::atomic<bool> busy` from the `AutosaveFacts` that `autosave_tick` already fills every pass (`lights.cpp:68-76`: `f.playing || f.recording || f.loading`), plus "heard": `host.out_level(0/1)` above a floor. Accessors beside `screen()` in `shell.h:59`.
  2. *Policy on the service thread.* In `MelodeeApp::service()` replace the block above with `unsigned want = idle_for >= dim_ms && !busy ? dim_level : shell.screen();` and keep the existing "write only on change" (`screen_applied`). Count `hardware.power.presses()` and a card lend as activity. Because `Shell::surface()` stamps `last_input_ms` on the MIDI thread, the screen is back within one service pass (5 ms) of any touch. A second, longer timer can call the existing `request_power_off()` path (`melodee_app.cpp:35`) through the Exit sheet's save-then-off sequence (`shell.cpp:427-436`), only on battery if the supply can tell (see Could not determine).
  3. *Settings.* `Settings` (`settings.h:37`): add e.g. `uint8_t dim_after` (Never, 1, 2, 5, 10 min) and `uint8_t off_after` (Never, 30, 60, 120 min); add rows before `Rows` in `enum Row` (`:38-40`; the enum order is not persisted, the file is `key=value`), and the matching entries in `row_name` (`:134`, same order as the enum), `value_text`, `step`, `choices`, `choice`, `set_choice`, `text()` (`:345`) and `read()` (`:374`). On the page, add the rows to a door in `settings_page.cpp:50-62`: the grid is fixed at eight doors and all are used, so put **Dim after** and **Auto off** behind SCREEN (already 7 rows; rows scroll past six) or rename a door. K1 and −/+ then work through the existing `Binding::Kind::SettingRow` → `Context::set_setting` / `change_setting` (`context.cpp:1030-1049`) with no new code. Document in `docs/guide/settings.md`.
  A CPU governor switch (`schedutil` while idle) has no base API today: `Platform` says "No raw GPIO, I2C, regulator, gadget or block-device writes" (`platform.h:37-38`) and has no cpufreq method, so that part is a sample-base change first.
- **Gain:** battery High (unmeasured; depends on the backlight's share).
- **Sound or behaviour change:** visible (the screen dims after the chosen time; off by default if wanted). Audio bit-exact.
- **Risk:** Low for dimming (never while playing, recording, loading, printing or sounding; any touch restores). Medium for auto power-off (must go through the save path and must never fire on mains or mid-job).
- **Effort:** M (dim and settings rows); L with auto-off and a governor API.
- **Confidence:** High that nothing exists today; the gain is not measured.

### SHELL-4: `ProjectStore::scan()` re-reads and fully parses every project in the folder after every request, and at boot before the last project is loaded
- **Where:** `src/melodee/shell/project_store.cpp:500` (`ProjectStore::run`), `:540-542` (`ProjectStore::scan`), `:267-271` (`ProjectStore::service`)
- **Thread and frequency:** service thread; after every autosave, every settings write (each −/+ step, each Palette/Font detent, each debounced K1 change), every usage-log flush (10 s while lines wait), every `opened()`; and once at boot.
- **Now:** `run()` ends with an unconditional `scan();` (line 500; only Load and the MPC 3 export return earlier). `scan()` builds a whole project in memory for each file just to count tracks and read the tempo:
  ```cpp
  // project_store.cpp:540-542
  auto project = std::make_unique<Project>();
  auto patterns = std::make_unique<PatternBank>();
  if (load_project(read_file(e.path), *project, *patterns).empty()) { e.tracks = project->used_tracks(); e.tempo = project->tempo; }
  ```
  It then walks every folder under the root four deep (`:574-584`). At boot the same scan runs before the resume project is even read:
  ```cpp
  // project_store.cpp:267-271
  scan_sounds();
  scan();
  const auto& resume = !start.empty() && fs::exists(start, error) ? start : last;
  if (!started && !resume.empty() && fs::exists(resume, error)) {
      auto* l = new LoadedProject {resume, {}, read_file(resume), false, {}, {}};
  ```
- **Cost:** Per project file: read the whole file (plug-in states included), allocate and construct a `Project` (about 0.54 MB by my count: `Track` is about 32.3 KB × 16) and a `PatternBank` (2048 `Pattern`s), parse, free. With 30 projects that is on the order of 30 MB of allocation and construction plus the parses per scan, probably tens to a few hundred ms of service-thread time each time a setting is stepped or an autosave lands, during which the service loop polls nothing. At boot the files are cold on the card, so start-up time grows with the number and size of projects in `Melodee/Projects` before the one wanted is touched. Measure: time `scan()` with a steady clock and print it once, with 5 and with 50 projects.
- **Proposal:** (a) Split `scan()`: a cheap `publish()` that only copies `root`, `folder`, `start`, `saves` into `known` (that is all the non-Project pages read; `free_path` needs `known.root`), and the expensive listing run only for `List`, `Duplicate`, `Remove`, `Rename`, `Move`, `NewFolder`, a `Save` into the folder in view, and a card return. (b) Cache each entry by path, `last_write_time` and size, and parse only files whose stamp changed. (c) Replace the full `load_project` with a header peek that walks section headers and reads only `PROJ` (tempo) and each `TRAK`'s engine byte. (d) At boot push the resume project before listing.
- **Gain:** CPU Medium (service thread), load time Medium to High at boot with many projects, battery Low to Medium.
- **Sound or behaviour change:** none.
- **Risk:** Low for (b) and (d); Medium for (a) (the Project page and the Exit sheet read `known.entries`; `lights.cpp:306-312` watches `generation()`).
- **Effort:** M.
- **Confidence:** High on what the code does; project counts and sizes on real cards are unknown to me. This file belongs to the file-jobs auditor too; expect an overlapping finding.

### SHELL-5: The screen snapshot copies the whole `Project` and rebuilds 2048 pattern sketches every frame while holding the lock the MIDI thread takes on every pass
- **Where:** `src/melodee/shell/shell.cpp:804-815` and `:878-894` (`Shell::snapshot`)
- **Thread and frequency:** screen thread, once per frame: 60 a second while the picture changes, 10 a second when it stands still (`base/src/runtime/run_app.cpp:127`). `Shell::lock` is also taken by the MIDI thread at least twice per 5 ms pass (`lights`, `midi_output`) and for every control or MIDI event.
- **Now:**
  ```cpp
  // shell.cpp:805, 810-815
  std::lock_guard<std::mutex> guard(lock);
  ...
  out.ui = ui_;
  { // The project, without each sample's chop: no page draws one (Chop draws the slices in hand, UiState's).
      SlotTable<SampleChop> chops = std::move(project_->chops);
      out.project = *project_;
      project_->chops = std::move(chops);
  }
  // shell.cpp:878-883
  for (unsigned t = 0; t < track_count; ++t)
      for (unsigned n = 0; n < pattern_count; ++n) {
          const auto& pattern = patterns_->at[t][n];
          auto& sketch = out.sketches[t][n];
          sketch.length = pattern.length; sketch.keys.fill(0);
  ```
- **Cost:** `Project` is about 0.54 MB (16 × `Track`, each with 128 `PadSound` of 132 bytes and 128 pad chains of 98 bytes), plus 20 KB per 64 samples in use; the sketch table is 2048 × about 112 bytes written and 2048 `Pattern` headers read. That is roughly 1.2 MB of memory traffic per frame, more than the SoC's caches hold, so I estimate 0.5 to 1 ms per frame: 3 to 6 % of one core at 60 fps, 0.5 to 1 % at the idle 10 fps, all of it evicting the audio thread's cache lines. For the whole of it the MIDI thread (SCHED_FIFO 15) can be waiting on a plain `std::mutex` held by the non-real-time screen thread, so a pad hit can be handled late. The app already counts that: `melodee_timed=kept/missed/worst` in `status` (`shell.cpp:976-977`). Measure: a steady-clock pair around `shell.snapshot` in `MelodeeApp::draw` (`melodee_app.cpp:437`), and `melodee_timed` while a page animates.
- **Proposal:** Copy the heavy parts only when they can have changed for the eye.
  1. Add `uint32_t input_epoch` to `Shell`, bumped in `surface()`, `midi()`, `looper_ask()` and `midi_panic()` (every path by which a hand or a message edits the model).
  2. In `snapshot()`: if `input_epoch` moved since the last frame, copy as today. Otherwise copy the small members, the selected track whole, and one other track per frame in rotation (the other fifteen tracks' `pads` / `pad_fx` are then at most 16 frames old, and they only change without input through the ticks). `Track` copies as plain bytes, so this is a loop of `tracks[t] = project_->tracks[t]` over two indices.
  3. Sketches: rebuild the selected track's row every frame (Stage and Edit read `[track][live]`), and the whole table when `input_epoch` moved, when a new `patterns_epoch` bumped in `Context::sync_pattern` (`context.cpp:1225`, the one place every pattern edit passes) and in `switch_to` moved, or a few times a second.
  Idle frames then copy about 65 KB instead of about 0.8 MB.
- **Gain:** CPU Medium (screen thread, and cache pressure on the audio thread), latency Medium (MIDI thread waits less), battery Low to Medium at idle.
- **Sound or behaviour change:** none intended; visible only if an epoch bump is missed (a stale tile for up to a quarter of a second).
- **Risk:** Medium. The model is edited in many places; the design above stays safe because anything not covered by an epoch is still refreshed within 16 frames, but it should be checked page by page for pages that draw another track's kit.
- **Effort:** M.
- **Confidence:** High on what is copied per frame; the size is a hand count and the time an estimate.

### SHELL-6: Remote-`status` mirrors are rebuilt every 5 ms on the MIDI thread, including a walk of all 8192 sample slots
- **Where:** `src/melodee/shell/lights.cpp:371-503` (`Shell::lights`), in particular `:484`, `:383-384`, `:408`, `:491-497`
- **Thread and frequency:** MIDI thread (SCHED_FIFO 15), every 5 ms, under `Shell::lock`.
- **Now:** About 45 atomics and three strings are refreshed on every pass so that `Shell::summary()` can answer a `status` command from another thread:
  ```cpp
  // lights.cpp:483-484
  unsigned used = 0;
  for (const auto& ref : project_->samples) used += ref.used();
  // lights.cpp:383-384
  std::lock_guard<std::mutex> text(text_lock);
  stage_key_text = stage_page_now(ui_, t, instrument).key;
  // lights.cpp:407-409 (on Roll)
  if (ui_.page == PageId::Roll) { // the view as the page draws it
      UiState roll = ui_;
      roll_follow(shown, roll);
  ```
  `project_->samples` iterates `sample_slots` = 8192 entries (`slot_table.h:66-67`, `model/sampler.h:21`). The same file uses the bounded `samples.span()` 300 lines earlier (`lights.cpp:198`), and `slot_table.h:68` says `span()` is "For what runs every tick or frame".
- **Cost:** The slot walk alone is 8192 iterations of a bounds check, a block-pointer load and a byte load: I estimate 20 to 30 µs per pass, about 0.5 % of one core, for one number (`melodee_pad_slots`). The strings add two `text_lock` acquisitions, a `library_->folder()` copy under the library's lock (`:491`) and up to three heap allocations per pass; on Roll a `UiState` (about 8 KB) is copied 200 times a second. None of it is read unless a remote tool asks for `status`. Measure: time the block `lights.cpp:371-505` with a steady clock; compare `melodee` CPU in `top -H` before and after.
- **Proposal:** Refresh the mirrors at 10 Hz, not 200 Hz: wrap lines 372-503 in `if (now - status_at >= 100) { status_at = now; ... }` and keep the one line that is not status, the gesture timeout at `:504`, outside it. Independently, change `:484` to `for (unsigned n = 0, span = unsigned(project_->samples.span()); n < span; ++n) used += project_->samples[n].used();`. Tests that read `status` right after a press need either a flush on demand (set an atomic "status wanted" flag in `summary()` and refresh on the next pass) or the 100 ms allowance.
- **Gain:** CPU Low to Medium (about 0.5 to 1 % of a core, constant), battery Low.
- **Sound or behaviour change:** none (a `status` reply may be up to 100 ms old).
- **Risk:** Low; the self-test and `device.py` scripts that press and then read `status` are the thing to check.
- **Effort:** S.
- **Confidence:** High on the code; the times are estimates. `lights.cpp` is shared with the lights auditor.

### SHELL-7: Shell housekeeping runs at the lights' 5 ms rate and allocates and locks on every pass
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
- **Where:** `src/melodee/shell/shell.cpp:874-876` and `:847` (`Shell::snapshot`); `src/melodee/shell/sample_library.cpp:260`; `src/melodee/shell/project_store.cpp:196`
- **Thread and frequency:** screen thread, every frame while the Samples page (or the Project page or Exit sheet) is open; `Shell::lock` held throughout.
- **Now:**
  ```cpp
  // shell.cpp:874-876
  if (ui_.page == PageId::Samples) {
      out.files = library_->files(); out.sample_root = library_->root(); out.files_folder = library_->folder(); out.files_top = library_->top();
  }
  // sample_library.cpp:260
  std::vector<SampleFile> SampleLibrary::files() const { std::lock_guard<std::mutex> guard(lock); return known.files; }
  ```
  `SampleFile` holds two `std::string`s (`sample_library.h:50-53`) and a folder lists up to 4000 entries (`sample_library.cpp:415`). `store_->view(out.projects)` copies the whole `View` (entries, sounds, folders, settings text) the same way.
- **Cost:** A folder of 500 files is about 500 to 1000 heap allocations and as many frees per frame; at 4000 files about 8000 of each, which I estimate at 1.5 to 3 ms per frame, at up to 60 fps while the cursor moves or a preview plays, with the MIDI thread locked out for that time. That is the page where pads audition samples. Measure as SHELL-5, on a large folder.
- **Proposal:** Publish the list as `std::shared_ptr<const std::vector<SampleFile>>` built once in `SampleLibrary::scan()` and handed out by pointer; `Snapshot::files` becomes the shared pointer. The plug-in presets already do exactly this for the same reason (`plugin/plugin_panel.h:57-58`: "The list shared rather than copied ... the pages take it with every frame"). Same for `ProjectStore::View::entries`, or copy the view only when `store_->generation()` changed since the last frame.
- **Gain:** CPU Medium on those pages, latency Medium (pad response while browsing).
- **Sound or behaviour change:** none.
- **Risk:** Low.
- **Effort:** S to M (the pages that index `s.files` change from `.` to `->`).
- **Confidence:** High on the copy; time is an estimate.

### SHELL-9: `MelodeeApp::service()` re-derives the same facts on every 5 ms pass; the mount table is parsed about 3 times a second from six places
- **Where:** `src/melodee/melodee_app.cpp:506-536` (`service`), `:499`, `:546`, `:698`, `:750`; `src/melodee/shell/project_store.cpp:247-248`; `src/melodee/shell/sample_library.cpp:474-479`; `base/src/drivers/platform.cpp:199-212`
- **Thread and frequency:** service thread, every 5 ms (`base/src/runtime/controller.cpp:37`, `server.serve(5, …)`).
- **Now:** Each caller finds "the card" for itself by calling `Platform::storage()`, which reads `/proc/self/mountinfo` a byte at a time, parses every line with string streams and calls `statvfs()` on every device-backed mount:
  ```cpp
  // melodee_app.cpp:546-547 (every 2 s; every 250 ms with no card)
  for (const auto& space : hardware.platform.storage())
      if (space.path.rfind("/media/", 0) == 0) { card = space.path; break; }
  ```
  The same loop is at `:498-500` (5 s), `:698-699` (2 s), `:750-751` (1 s), `project_store.cpp:248` and `sample_library.cpp:478` (every 600 passes each). `web_service` also rebuilds and republishes its facts on every pass:
  ```cpp
  // melodee_app.cpp:754-756, 766
  melodee::web::Facts facts;
  facts.version = melodee::melodee_version();
  facts.card = web_card;
  ...
  web.publish(facts);
  ```
  `publish` takes a mutex, copies four strings and calls `notify_all` (`web/web_app.cpp:89-95`), followed by `take_note` (mutex) every pass; `card_update` takes `UpdateLink`'s mutex and copies a string every pass (`melodee_app.cpp:644`, `update.cpp:75-80`). `read_machine` parses `/proc/stat` and asks the plug-in rack for its load every 500 ms whether or not About is on screen (`:482-493`).
- **Cost:** Small per pass (a few µs), plus about 3 mountinfo parses and statvfs rounds a second. The comment at `platform.h:50-51` warns "statvfs() can wait on a slow card", and each wait stalls the whole service loop. The larger cost is the loop itself: 200 wake-ups a second in which the base also does five or six non-blocking system calls (`jacks`, `power`, `clock`, `sd_bus_process`, the control socket), about 1200 system calls a second with nothing happening. Measure: `strace -c -f -p` for ten idle seconds, and `perf top` on the service thread.
- **Proposal:** In this repository: (1) one `card_path()` helper on `MelodeeApp` that calls `storage()` at most once a second (at once after `card_returns` moves) and is used by all six sites, with the library and the store taking the path from the app instead of asking the platform; (2) publish web facts only when they differ from the last ones sent or once a second; (3) call `link.take_request` only when an atomic "asked" flag is set; (4) gate `read_machine`'s CPU part on the About page being in view (the page is already mirrored in `Shell::page_track`). In sample-base (not editable here): let the service loop block in one `poll()` over the control socket, the jack control fd, the power-key fd and the sync-in fd with a timeout the app chooses (5 ms while a take, render, preview, load or card transition runs; 50 ms otherwise). If that changes, the two pass counters used as timers must become clock timers: `if (++ticks % 600 == 1)` in `project_store.cpp:247` and `sample_library.cpp:474`, and `hold_passes = 600` in `looper_files.cpp:23`.
- **Gain:** battery Low from the app-side changes alone; Medium with the base change (about 180 fewer wake-ups and 1000 fewer system calls a second).
- **Sound or behaviour change:** none.
- **Risk:** Low for the app side. Medium for the base loop: Sample Record's take is drained to the card from this loop (`sample_library.cpp:442-443`), so the short period must stay while one runs.
- **Effort:** S (app side), M (base).
- **Confidence:** High on the call sites; the per-pass cost and the base polling cost are estimates.

### SHELL-10: The sample library re-lists the browser's folder every 3 s for ever, reading every MPC program file in it
- **Where:** `src/melodee/shell/sample_library.cpp:474-488` (`SampleLibrary::service`), `:411-437` (`list_into`), `:432`; `src/melodee/shell/mpc_program.cpp:155-165`
- **Thread and frequency:** service thread, every 600 passes (about 3 s), on every page, for as long as the app runs.
- **Now:**
  ```cpp
  // sample_library.cpp:474, 486-487
  if (++ticks % 600 == 1 || scan_wanted.load() || browse_wanted) {
      ...
      scan_wanted.store(false);
      scan();
  // sample_library.cpp:432, 434 (per directory entry)
  if (instrument && is_drum_program(f.path)) { f.kit = true; f.instrument = false; }
  if (!folder_here) f.bytes = it->file_size(error);
  // mpc_program.cpp:158-162
  if (!read_bytes(path, head, 16384)) return false;
  if (head.size() >= 2 && uint8_t(head[0]) == 0x1f && uint8_t(head[1]) == 0x8b) { // JSON: read whole (an .xty is a few hundred KB)
      ...
      return read_bytes(path, whole, 8u << 20) && read_program(path, whole, program).empty() && program.type == "Drum";
  ```
  `scan()` lists whatever folder the browser was last in (up to 4000 entries), stats each file for its size, and for each `.xpm` / `.xty` opens it and reads 16 KB, or for a gzipped one reads and parses the whole file, only to end with "same listing, nothing changed" (`:536-538`).
- **Cost:** For a plain folder of WAVs it is a directory read and one `stat` per file every 3 s (served from the kernel's caches, so CPU rather than card I/O). If the browser was left in an expansion folder with, say, 30 programs, that is 30 file opens and reads every 3 s, and for `.xty` programs a full decompress and JSON parse each, which could reach tens of ms per file. All on the service thread, on every page. Measure: time `scan()`; `strace -e openat` for ten idle seconds.
- **Proposal:** Rescan on events only: when the browser asks (`browse_wanted`), on `rescan()` (card return, web upload, Sample Select), and after the library's own writes. Keep the 3 s timer only for finding the card root (`:475-485`) and skip `scan()` there unless the root changed. If a periodic refresh is wanted while the Samples page is open, let the shell ask for it (`library_->rescan()` is already called when the page opens, `shell.cpp:472`). Cache `is_drum_program` by path, size and mtime.
- **Gain:** CPU Low to Medium (depends on the folder), battery Low.
- **Sound or behaviour change:** none; a file copied onto the card behind the app's back shows when the browser is next opened or moved, not within 3 s.
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** High on the code path; the cost depends on the folder. This is the file-jobs auditor's file; expect overlap.

### SHELL-11: During a project load the MIDI thread reads up to 15 sample files per pass from the card while holding the shell lock
- **Where:** `src/melodee/shell/lights.cpp:323-331` (`Shell::lights`), `src/melodee/shell/mpc_wav_slices.cpp:87-97` (`read_mpc_slices`); also `src/melodee/shell/context.cpp:1828`
- **Thread and frequency:** MIDI thread (SCHED_FIFO 15), under `Shell::lock`, once per sample on every project load, in bursts as large as the library's request queue (`MidiQueue<16, Request>`, `sample_library.h:385`).
- **Now:**
  ```cpp
  // lights.cpp:323-330
  while (library_ && context_.samples_to_load < sample_slots) {
      const unsigned slot = context_.samples_to_load;
      follow_moved_sample(*project_, slot, *library_);
      const auto& ref = project_->samples[slot];
      bool real = ref.used() && ref.path[0] == '/';
      if ((real || library_->held(slot)) && !library_->load(slot, real ? ref.path : "", real && ref.root == root_unread)) break;
      if (real && !project_->chops[slot].unwritten) project_->chops.edit(slot) = read_mpc_slices(ref.path); // the file's slices are its chop (an older project's waits to be written)
      ++context_.samples_to_load;
  ```
  `read_mpc_slices` opens the WAV with `std::ifstream`, walks its chunk list to the end of the file and parses a JSON chunk with JUCE.
- **Cost:** A cold open plus a seek to the tail of a file on SD is several ms at best; fifteen in one pass can hold the MIDI thread, and with it pad input, MIDI clock out and the lights, for tens to hundreds of ms, and the screen thread waits on the same lock. This breaks the spirit of "file I/O off the real-time threads" for the thread that timestamps pads and sends sync. Measure: `melodee_timed` and an external MIDI clock's jitter while a project with 100 samples loads.
- **Proposal:** Read the slices where the file is read anyway. The loader already opens each file on the pool; have it return the `SampleChop` with the loaded sample (a field in what `take_loaded()` hands on) and apply it in the library-generation block (`lights.cpp:196-219`), where the MIDI thread already copies each sample's length and root. `context.cpp:1828` (a file put on a pad) can use the same path.
- **Gain:** latency Medium during loads; load time Low.
- **Sound or behaviour change:** none (a sample's chop arrives with its length instead of just before it).
- **Risk:** Medium: the `unwritten` rule (an older project's chop must not be overwritten by the file's) has to move with it.
- **Effort:** M.
- **Confidence:** High that the read happens on the MIDI thread under the lock; the stall length is an estimate.

### SHELL-12: Helper threads that poll on a timer for ever, started whether or not their feature is in use
- **Where:** `src/melodee/shell/audio_jobs.cpp:36-38` (`Context::sync_audio`) with `src/melodee/shell/audio_tracks.cpp:157`; `src/melodee/melodee_app.cpp:737-741` (`web_service`) with `web/web_server.cpp:209` and `web/mdns.cpp:217`; `src/melodee/shell/sample_library.cpp:448` with `src/melodee/engine/stream_pager.cpp:250-251`
- **Thread and frequency:** their own threads: 40 Hz, 4 Hz + 4 Hz, and 1000 Hz, always.
- **Now:**
  ```cpp
  // audio_jobs.cpp:36-37 (first lights pass of every run)
  if (!audio_tracks) {
      audio_tracks = std::make_shared<AudioTracks>(host);
  // audio_tracks.cpp:157
  wake.wait_for(held, std::chrono::milliseconds(25),
  // stream_pager.cpp:250-251 (SCHED_FIFO 20; started from SampleLibrary::service every pass)
  while (running.load(std::memory_order_relaxed))
      if (!pump()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ```
  The audio-tracks thread is created on the first tick of every session and wakes 40 times a second even when the project has no audio track. The web server and the mDNS responder start on the first service pass and each wake every 250 ms only to look at a stop flag. The stream pager sleeps 1 ms between empty pumps, so it wakes about 1000 times a second with nothing streaming.
- **Cost:** About 1050 timer wake-ups a second at idle that do no work, most of them the pager's. Each wake-up of a real-time thread costs a context switch and cold caches (I estimate 10 to 30 µs each, so 1 to 3 % of a core for the pager alone). Measure: `perf sched record` for ten idle seconds, or the per-thread `voluntary_ctxt_switches` in `/proc/<pid>/task/*/status`.
- **Proposal:** Audio tracks: create the thread when the first audio track or take appears, and wait on the condition variable without a timeout except while a take runs (the 25 ms is "to keep up with a take", `audio_tracks.cpp:156`). Web and mDNS: add a wake pipe or `eventfd` to the `poll` set and use an infinite timeout. Stream pager (engine auditor's file; noted here because the shell starts it): block on a semaphore or futex that the audio thread posts when it opens a stream or crosses a read-ahead mark, with a long timeout as a safety net.
- **Gain:** battery Medium (mostly the pager), CPU Low.
- **Sound or behaviour change:** bit-exact if the pager is woken at the same points it would have polled; none for the others.
- **Risk:** Low for audio tracks and web; Medium for the pager (a missed wake is a stream underrun: keep a 5 to 10 ms timeout while any stream is open).
- **Effort:** S each for audio tracks and web; M for the pager.
- **Confidence:** High on the loops as written; I did not read `pump()` to see how much an empty pump costs, nor the rest of the pager.

### SHELL-13: Settings are rewritten in full, unconditionally, once per step, and not atomically
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

## Answers to the assignment's questions

### 1. What runs on each pass, and what could run less often

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

### 2. Locks

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

### 3. Undo, history, snapshots

- **Undo history** (`history.h:171-207`): RAM only, two `std::deque<Edit>` capped at 32 entries each (`depth = 32`), cleared on a project switch (`context.cpp:157`). Never written to the card.
- **An `Edit` is partial, not a copy of the project** (`history.h:139-169`): two inline `PadState`s (about 250 bytes each) and optional heap parts for what the scope covers: one `Pattern`, the `Song` (a few KB), a `Chain`, the mix, a track's sound, the master state, Chop's slices, the tempo. The largest are `TrackState` (a `Track`, about 32 KB, plus 128 patterns; used by track copy, clear and move, and pad-gap close at `context.cpp:1482`) and a `Pattern` with many notes (up to 65 535 events of 16 bytes).
- **When:** a pot's turns are one edit per control: `touch_gesture` takes the "before" once when the gesture opens and returns at once while it continues (`undo_restore.cpp:89`); the "after" is taken when another control is used or after 2 s still (`lights.cpp:504`). A button or pad press on an editing page takes a "before", runs the press, and keeps the edit only if something differs (`PageEdit`, `undo_restore.cpp:182-195`). A Seq Record take is one edit (pattern + song, `context.cpp:2253`).
- **No multi-megabyte copy on control input or on a timer** in the undo path. The per-frame `Project` copy for the screen (SHELL-5) is the only large periodic copy I found.
- Related, not undo: every pattern change sends the audio thread a fresh heap copy of the pattern (`context.cpp:1228`), so a pot sweep on a note card (Step velocity and the like, `context.cpp:634-658`) makes one pattern copy per controller message. Motion recording is throttled to one per 40 ms (`context.h:215`); note-card edits are not. Small for ordinary patterns.

### 4. Writes to the card

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

### 5. Power-related settings that exist today

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

### 6. Signals available for "idle for N minutes"

- **Last input:** `Shell::last_input_` (`shell.h:177`), ms on the steady clock. Stamped by every control-surface message (`shell.cpp:130`), every external MIDI message except real-time ones (`shell.cpp:96`), and every pass while a pad or a modal button is held (`lights.cpp:66`). Private to the MIDI thread today; needs an atomic copy. Not stamped by the power key (the service thread counts it: `hardware.power.presses()`, `melodee_app.cpp:526`) or by web-page activity.
- **Transport and jobs**, already gathered every pass in `Shell::autosave_tick` (`lights.cpp:68-76`): `host.playing()`, `host.song_playing()`, `host.looper().recording()` / `armed()`, `ui.recording`, `ui.rec_armed`, `host.recorder().state()`, `printer->busy()`, the print and auto-sample sheets, `store_->exporting()`, `ui.loading`, `samples_to_load`, an open question, name sheet, exit sheet, card access or update.
- **Sound:** `host.out_level(0/1)` and `host.in_level()` (used for the meters, `lights.cpp:560`), `host.meter(t)` per track, the voice taps (`host.taps`), `host.monitoring()`, each loop's `view(n).sounding`.
- **External clock:** `midi_io.following()` / `following_mtc()` (`shell.cpp:831-832`), `session().clocks`.
- **Counters from the runtime:** `session().hardware_events`, `external_events`, `synthetic_events` (`session.h:37`), the same ones the screen loop uses to wake from its idle rate.
- **USB and card:** `hardware.card.status()` (state, `connected`: a computer has configured the gadget), `hardware.platform.usb_state()`, `input_route.usb_state()`, `session().mirror.state` (a computer is showing the camera), `hardware.usb_output`.
- **Jacks:** `session().plugged(Jack::Headphones / LineOut)`.
- **Supply:** `Platform::supplies()` returns each supply's name, type, `online` and `voltage_now` (`platform.cpp:179-184`). Melodee never calls it. Whether this board reports mains through `online` I could not tell from the code.

### 7. Start-up and project load

- `MelodeeApp::MelodeeApp()` reads the mount table and the settings file before the threads start (`melodee_app.cpp:72-74`), deliberately, so the audio device opens once at the right rate.
- First service pass: `stop_stock_web_server()` spawns `systemctl stop` and waits for it (`melodee_app.cpp:48-56`, `:738`); the card warm starts and holds the library and the store back for up to 5 s (`:540-561`).
- About 250 ms in, `card_update` runs `/bin/sh sd-update.sh check` for each zip in the card's top folder, up to 16, 10 s timeout each, on the service thread (`base/src/drivers/sd_update.cpp:174-192`). It overlaps the card warm, so it only costs time if zips are present.
- `ProjectStore::service` then reads and parses **every** project in `Melodee/Projects` before it reads the one to resume (SHELL-4).
- `switch_to` builds the project pack and asks for every sample again; the MIDI thread reads each sample file's slice chunk itself (SHELL-11). A card warm is started again before each project load (`melodee_app.cpp:556-559`).
- Samples are loaded on the library's pool, not synchronously in the shell (not audited further).

### 8. Bugs looked for

- **Wall-clock timers:** none. All shell timers are `uint32_t` ms on `steady_clock`. Two comparisons are not wrap-safe (`at >= settings_save_at`, `sample_record.cpp:282`; `now >= boot_ms`, `melodee_app.cpp:639`), which matters only after 49 days of uptime.
- **Pass-count timers:** `++ticks % 600` in the store and the library, `hold_passes` in the looper files. Correct at today's 5 ms; they silently stretch if the service period ever changes.
- **Busy loops:** none in my files. Outside them: the stream pager's 1 ms sleep loop (SHELL-12) and `sample_library_instruments.cpp:575`, `:590` (1 ms sleeps, not read).
- **Joins on the service thread:** `card_warm.stop()` joins the warm thread when the card is lent (`melodee_app.cpp:573`, `card_warm.cpp:213-216`; the worker is passed a `stopping` flag, so it should be prompt); `ProjectStore::start_export` joins only a finished exporter (`project_store.cpp:112-114`).
- **Unbounded containers:** none found. History is capped at 32; the crash ring is fixed at 256 notes; the usage log flushes at 4096 bytes and its request is dropped when there is no card; `update_skip` and `slots_waiting` are bounded by what exists.

## Checked and fine

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

## Could not determine

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
