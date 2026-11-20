# Audit: screen drawing framework and the per-frame pipeline

## Coverage

Read fully: `base/src/runtime/run_app.cpp`, `base/src/drivers/display.cpp`, `display.h`, `display_damage.h`, `base/src/runtime/app.h`; `MelodeeApp::draw()` and the rest of `src/melodee/melodee_app.cpp`; `Shell::snapshot()` (`src/melodee/shell/shell.cpp:804-958`, which `draw()` calls first) and `shell/snapshot.h`; `src/melodee/ui/`: `paint.cpp/.h`, `chrome.cpp/.h`, `knobs.cpp/.h`, `widgets.cpp/.h`, `waveform.cpp/.h`, `boot.cpp/.h`, `tokens.h`, `shades.h`, `palettes.h`, `faces.h`, `font_data.h`, `card_rows.h`, the code of `glyphs.cpp/.h`; `src/melodee/pages/`: `common.cpp/.h`, `pages.cpp/.h`, `page.h`, `overlays.cpp`, `menu_sheet.cpp`, `ask_sheet.cpp`, `name_sheet.cpp`, `seq_sheet.cpp`, `place_bar.cpp`, `track_grid.cpp`, `input_meter.cpp/.h`, `fx_drawing.cpp`, `sampler_common.cpp/.h`; `shell/place_bar.h`, `shell/zoom_view.h`, `shell/comp_trail.h`.

Skimmed: `shell/ui_state.h` (fields and time stamps), `shell/render.cpp` (it is the audio Resample/Mixdown job, not screen rendering), `shell/playheads.h` (`HeadTrail::update`), `shell/sample_library.cpp` (`slot`, `thumbnail`, `detail`, `files`), `shell/bindings.cpp` (only for loops over events), `model/project.h`, `model/slot_table.h`, `base/src/drivers/screen_mirror.cpp/.h` (what the screen thread touches and the worker's wait), `base/src/runtime/controller.cpp:186-198`, `base/src/surface/midi.h` (the two parsers), `base/kernel/panel/sample_base_panel.c` (which helper it uses), `pads_page.cpp` and `keys_page.cpp` draw functions (as samples of a page's cost only).

Skipped: the icon path strings in `glyphs.cpp`, `boot_backdrop.cpp`, `boot_logo.cpp`, the font files, every other page (another auditor), JUCE itself (not checked out; statements about JUCE internals below are from memory of JUCE 7/8 and marked as such).

Struct sizes below come from a syntax-only compile of the model headers outside the repository (`clang++ -std=c++17 -fsyntax-only` on a scratch file with `static_assert(sizeof(T) == 0)`, arm64 macOS, libc++). Nothing in the repository was built, run or changed. On the device `std::string` is 8 bytes larger; the structs measured hold almost none.

## Findings

Short answers to the eight questions, with the finding that carries the detail:

1. Cost of one `draw()`: a 0.8 MB state copy under a lock, a full background fill, then (counted by reading a few pages, not measured) roughly 80-150 flat fills, most of them rounded rectangles, which go through path rasterisation, 5-20 stroked icon paths and 150-300 cached glyphs. No gradients, drop shadows, transparency layers or image rescaling anywhere in `pages/` or `ui/` (grep). My estimate for `draw_us` is 2-5 ms on ordinary pages and 5-10 ms on the densest; unmeasured. See UI-1, UI-2, UI-9, UI-11.
2. Caching: typefaces, `juce::Font`, string widths and glyph layouts are cached (`ui/paint.cpp:18-154`); icon shapes are parsed once. Not cached: static chrome, stroked icon outlines, effect and filter curves, the state copy, list views. See UI-2, UI-6, UI-9, UI-12.
3. What keeps the screen busy: UI-4 and UI-8 list the animated elements and which can be quantised.
4. Skipping the redraw: there is no dirty flag or state hash; the only test is the pixel compare after the frame is drawn. Design in UI-5.
5. Present path: UI-10.
6. Boot animation and transitions: UI-13. Shared code has no page transitions.
7. Text: UI-12.
8. Screen Mirror off: nothing measurable per frame (Checked and fine).

### UI-1: The whole pipeline runs at 60 fps whenever anything moves; nothing separates "a hand is on the controls" from "a play head is moving"
- **Where:** `base/src/runtime/run_app.cpp:127` and `:151-153` (the screen loop in `run_app`)
- **Thread and frequency:** screen, per frame, 60 a second while a project plays (the author's measurement, `base/docs/HARDWARE.md:442`)
- **Now:**
  ```cpp
  constexpr std::chrono::microseconds busy_interval {16667}, idle_interval {100000}, idle_after {500000}, input_poll {4000};
  ...
  bool idle = !display || start - last_change > idle_after;
  next = std::max(next + (idle ? idle_interval : busy_interval), ScreenClock::now() - busy_interval);
  if (!idle) { std::this_thread::sleep_until(next); continue; }
  ```
  One changed pixel (the 2 px place bar's leading edge) keeps the full cycle going: `Shell::snapshot()` (UI-2), `g.fillAll`, the page, the overlays, the 307 kB compare in `present()`, a push.
- **Cost:** per frame my estimate is 2-5 ms of drawing plus 0.3-0.8 ms of present and push planning, so 15-35 % of one core for as long as the transport runs, on ordinary pages; more on dense ones. Unmeasured. Measure with `status`: `screen_draw_us` times the rate of `screen_presented`, or `top -H` on the main thread. `screen_presented - screen_pushed` over a time span is the number of frames drawn in full and then found identical.
- **Proposal:** two rates in the busy state. Keep 16.7 ms for 250 ms after any control or external note (the loop already has `inputs()`), and use an app-chosen interval otherwise:
  ```cpp
  if (auto n = inputs(); n != seen_inputs) { seen_inputs = n; last_input = start; }
  bool hands = start - last_input < std::chrono::milliseconds(250);
  auto interval = idle ? idle_interval : hands ? busy_interval : app.watch_interval(); // default 16667; Melodee: 33333
  ```
  `watch_interval()` is a new `App` hook (change in sample-base). Melodee returns 33.3 ms, or a Settings > SCREEN row "Frame rate 60 / 30". The 33 ms wait must still wake on input (use the idle branch's wait, or the eventfd of UI-5). Fix UI-3 first, or MIDI clock counts as a hand.
- **Gain:** battery High, CPU High (about half the screen thread's time during unattended playback).
- **Sound or behaviour change:** bit-exact audio. Visible: play head, place bar and meters step at 30 fps when nobody touches the controls; unchanged under the fingers.
- **Risk:** Low. Timing only; every frame is still a full, correct picture.
- **Effort:** S.
- **Confidence:** High on the mechanism; the size of the gain depends on the unmeasured `draw_us`.

### UI-2: Every frame copies 0.8 MB of state under the lock the MIDI thread needs, and rebuilds the sketches of all 2048 patterns
- **Where:** `src/melodee/shell/shell.cpp:804-958` (`Shell::snapshot`), called from `src/melodee/melodee_app.cpp:437`; the lock is shared with `Shell::lights` (`shell/lights.cpp:180`), `Shell::midi_output`, `Shell::surface`, `Shell::midi` (`shell.cpp:93-128`)
- **Thread and frequency:** screen, per frame (60 a second busy, 10 idle); blocks the MIDI thread (`SCHED_FIFO` 15, at least every 5 ms)
- **Now:**
  ```cpp
  { // The project, without each sample's chop: no page draws one (Chop draws the slices in hand, UiState's).
      SlotTable<SampleChop> chops = std::move(project_->chops);
      out.project = *project_;
      project_->chops = std::move(chops);
  }
  ```
  ```cpp
  for (unsigned t = 0; t < track_count; ++t)
      for (unsigned n = 0; n < pattern_count; ++n) {
          const auto& pattern = patterns_->at[t][n];
          auto& sketch = out.sketches[t][n];
          sketch.length = pattern.length; sketch.keys.fill(0);
  ```
  Sizes (host probe, see Coverage): `Project` 538,968 bytes (16 tracks of 32,312), `Snapshot::sketches` 16 x 128 x 112 = 229,376 bytes written, `PatternBank` 491,520 bytes read (plus each non-empty pattern's events), `UiState` 8,160, `LoadedSample` 6,240; `Snapshot` 793,888 in all. Only nine page files read `sketches` (grep), and Stage and Edit read one entry each.
- **Cost:** about 1.8 MB of memory traffic a frame, more than the A55's private caches hold, so it also evicts what the audio thread and render workers keep in the shared cache. My estimate is 0.5-0.9 ms a frame (3-5 % of a core at 60 fps); unmeasured. Time `snapshot()` alone with a `steady_clock` pair and add it to the status line. The second cost is latency: the lock is a plain `std::mutex` (`shell/shell.h:146`), the holder is a `SCHED_OTHER` thread, and the waiter is the thread that times MIDI out and reads pads "the moment it arrives" (`base/src/runtime/controller.cpp:103-107`). Each frame can hold the MIDI thread for the length of the copy, and for a scheduler slice if the screen thread is preempted while it holds the lock.
- **Proposal:** in this order.
  1. Sketches: rebuild only when something could have changed them. A pattern edit the engine must hear ends in `Context::sync_pattern()` (77 callers; `shell/context.cpp:1225-1231`) or `sync_motion()` (`:1257-1263`). Both already set `dirty`; add a `pattern_serial` they increment, bump it too where a load or an undo replaces patterns wholesale, gate the double loop on it, and rebuild at least once a second as a safety net. The snapshot object persists (`melodee_app.cpp:61`), so the last sketches stay valid. (`Context::sent` cannot serve: `sync_pattern` calls `host.send` directly and does not count.)
  2. Project: copy only when a change is possible. `Context::send()` already tells project-changing commands from the rest (`shell/context.cpp:55-62` sets `dirty` for them) and `dirty = true` is written in 83 places in `shell/` and `pages/` (grep; that one among them); make that one `touch()` that also increments a `model_serial`. Add a counter bumped on entry to `Shell::surface()` and `Shell::midi()` (non-real-time), because some project fields are written with no mark at the place of the write (`octave()`, `pages/common.cpp:889-897`, writes `t.octave` and `t.bank`; I did not trace whether the press's undo scope marks them afterwards), plus `SampleLibrary::generation()`, `ProjectStore::generation()`, `PluginRack::changes()`; and copy at least every 250 ms as a safety net. `shell.cpp:256-258` (`usage_changed`) already uses `sent`, `edits.pushes`, `dirty` and a `memcmp` of `UiState` as its "did anything change" test.
  3. Make `Shell::lock` a priority-inheritance mutex. The codebase already has one (`plugin/vst2_bridge_instance.h:28-46`).
- **Gain:** CPU Medium, latency Medium (MIDI out jitter, pad-to-sound latency), battery Low-Medium.
- **Sound or behaviour change:** bit-exact audio; MIDI timing gets tighter, never looser. Visible: none when the gates are right.
- **Risk:** step 1 Low-Medium (a pattern change that reaches neither sync function would show up to 1 s late in a sketch; such a change would also be one the engine never hears). Step 2 Medium: a project field changed by a timer path with no mark and no generation bump would lag by up to 250 ms. Step 3 Low.
- **Effort:** S for 1 and 3, M for 2.
- **Confidence:** High on the sizes and the lock sharing; Medium on the timings (unmeasured); I did not read the 77 `sync_pattern` callers or the 83 `dirty` sites.

### UI-3: External MIDI clock and Active Sensing count as "input" and push the idle screen back to 60 fps
- **Where:** `base/src/runtime/controller.cpp:195` (`Controller::midi_packet`), `base/src/runtime/run_app.cpp:128` and `:154-157`; real-time bytes arrive as one-byte packets from `MidiStreamParser` (`base/src/surface/midi.h:24-27`, used for the hardware ports at `drivers/hardware_midi.cpp:14`)
- **Thread and frequency:** MIDI thread counts; screen thread reacts: 24 times a quarter note for clock, about every 300 ms for Active Sensing
- **Now:**
  ```cpp
  ++session.external_events;
  if (packet.size == 1 && packet.bytes[0] == 0xf8) ++session.clocks;
  app.midi(port, packet);
  ```
  ```cpp
  auto seen = inputs();
  while (ScreenClock::now() < next && inputs() == seen && session.running.load() && !interrupted && !session.screenshot.load())
      std::this_thread::sleep_for(input_poll);
  if (inputs() != seen) { last_change = ScreenClock::now(); next = last_change; }
  ```
  Setting `last_change` makes the loop "busy" for the next 500 ms (`idle_after`).
- **Cost:** with a clock source connected (a DAW or drum machine that sends clock while stopped) the screen never idles: 60 full draws a second on a still picture instead of 10. With a keyboard that sends Active Sensing the cycle is 500 ms at 60 fps, then at most 300 ms idle: about 40 draws a second instead of 10. By the estimate in UI-1 that is 10-30 % of a core for nothing. Check on the device: connect such a keyboard, leave Melodee stopped, and compare the rate of `screen_presented` with and without it.
- **Proposal:** do not count system real-time bytes as input for the screen:
  ```cpp
  if (!(packet.size == 1 && packet.bytes[0] >= 0xf8)) ++session.external_events;
  ```
  (or keep the counter for `status` and give the screen loop its own). A transport start from MIDI still redraws: the picture changes, and the idle loop draws every 100 ms anyway.
- **Gain:** battery High in that setup, none otherwise.
- **Sound or behaviour change:** none.
- **Risk:** Low. Worst case the first frame after an external Start is up to 100 ms late, as any non-input change is today.
- **Effort:** S (sample-base).
- **Confidence:** High on the code path. Not checked: whether the control surface's own port sends periodic messages that count in `hardware_events` (see Could not determine).

### UI-4: After every change the loop draws 30 more full frames, and slow animations are drawn at 60 fps
- **Where:** `base/src/runtime/run_app.cpp:127`, `:151-153`; the slow animations: `pages/overlays.cpp:98`, `pages/seq_sheet.cpp:43`, `pages/patterns_page.cpp:50`, `pages/scenes_page.cpp:64`, `pages/looper_page.cpp:27`, `:73`, `pages/auto_sample_page.cpp:102`, `:154`
- **Thread and frequency:** screen, 60 a second for 500 ms after each changed frame
- **Now:** `idle_after {500000}` is the only way down from 60 fps (quoted in UI-1). A blink is drawn as
  ```cpp
  bool blink = (s.now / 150) % 2;
  ```
  so the picture changes every 150 ms (250 ms for the lesson's ring, the Looper and Auto Sample) and the loop never reaches idle: 9 to 15 full draws for each change.
- **Cost:** one button press costs about 30 full draws of which the first one or two differ. A lesson, a queued pattern on the Sequence sheet, an armed loop or a count that changes once a second all hold 35-60 draws a second for 1-7 changes a second. A slow place bar does the same: a 16-bar pattern at 120 BPM moves its edge about 8.7 px a second (278 px of bar over 6144 ticks at 192 ticks a second). Unmeasured; `screen_presented - screen_pushed` shows it directly.
- **Proposal:** back off by the number of unchanged frames instead of a fixed 500 ms, with the input wake at every level:
  ```cpp
  still = changed ? 0 : still + 1;
  auto interval = still < 2 ? 16667us : still < 4 ? 33333us : still < 8 ? 50000us : 100000us;
  ```
  A press then costs about 10 draws in its first half second instead of 30. Better, and compatible: let the app say when the picture can next change by time alone (UI-5), so a 250 ms blink costs 4 draws a second and its edge is never late.
- **Gain:** battery Medium, CPU Medium.
- **Sound or behaviour change:** none in the audio. Visible: a change with no input behind it (a blink edge, a job's progress) can show up to 33-50 ms late in the first 300 ms after the last change; today it is on time for 500 ms and then up to 100 ms late.
- **Risk:** Low.
- **Effort:** S (sample-base).
- **Confidence:** High.

### UI-5: A still screen is redrawn in full 10 times a second for ever; there is no "nothing changed" test before drawing
- **Where:** `src/melodee/melodee_app.cpp:432-468` (`MelodeeApp::draw`), `base/src/runtime/run_app.cpp:135-157`
- **Thread and frequency:** screen, every 100 ms when idle, plus a 4 ms poll (250 wake-ups a second)
- **Now:**
  ```cpp
  ui::Paint p(g);
  g.fillAll(ui::colour::bg);
  page_spec(frame->ui.page).draw(p, *frame);
  draw_overlays(p, *frame);
  boot.draw(g, now); // its fade out, over the app; nothing once it has ended
  ```
  `draw()` has no early return. The only "unchanged" test is `Display::present()`'s compare of the finished image.
- **Cost:** 10 x (snapshot + draw + compare), about 3-6 % of a core, plus 250 timer wake-ups a second, whenever the unit is on and untouched. Unmeasured.
- **Proposal:** the cheapest robust design I can see for this codebase. Where the state lives and who writes it:

  | State drawing reads | Lives in | Written by |
  |---|---|---|
  | `UiState` (8 kB, trivially copyable), `Project`, `PatternBank`, `Context` (`dirty`, `sent`, `edits`, prints) | `Shell` | MIDI thread under `Shell::lock`; also the screen thread itself in `snapshot()` (`check_pickup`, `fader_check`, `edit_loop_check`, `shell.cpp:806-809`) |
  | positions, ticks, meters, voice taps, compressor, recorder, bounce, looper | `EngineHost` atomics | audio thread, per block |
  | sample, file, project and plug-in lists | `SampleLibrary`, `ProjectStore`, `PluginRack` | service and loader threads; each already has a generation counter |
  | core loads, card space | `MelodeeApp::machine` atomics | service thread, every 0.5 s and 5 s |
  | xruns, mirror state | `Session` | audio and screen threads |
  | `now` | clock | 33 sites in `pages/` compare it with a `UiState` stamp or take it modulo a blink period |

  1. sample-base: a hook the loop calls before it builds the `juce::Graphics`, for example `virtual Clock::duration next_draw()`: zero means draw now; otherwise skip `draw()` and `present()` and sleep that long or until input. (Without a base change `draw()` could return without drawing, because the loop reuses one `juce::Image`, `run_app.cpp:129`; but the remote prompt draws into the same image, so the explicit hook is safer.)
  2. Melodee answers "draw now" when any of these holds: the transport runs, counts in, records, renders or loads; `session().audio.sounding_blocks` moved since the last frame (the runtime already counts non-silent blocks, `base/src/drivers/audio.cpp:133`), which covers meters and voice heads; a cheap serial moved (the two counters of UI-2, `Context::sent`, `edits.pushes`, a `memcmp` of `UiState` against the copy in the last snapshot, the three generation counters, the machine stats' time stamp); the page is one that shows live input (Record, Input Config, Compressor, Mod, About); or the deadline from the last draw has passed.
  3. The deadline: route the 33 time tests through two helpers on `Paint` that return the same value as today and record when it will next change (`p.within(now, stamp, window)` records `stamp + window`; `p.phase(now, period)` records the next multiple). `MelodeeApp::draw` reads the earliest after drawing. Cap it at 1 s so that anything missed is at most a second stale.
  4. Move `check_pickup`, `fader_check` and `edit_loop_check` out of `snapshot()` into `Shell::lights()` first. `edit_loop_check` ends an editor's loop in the engine (`shell/note_jobs.cpp:176-182`), and today it only runs when the screen draws: 10 times a second idle, never with `--headless`. `HeadTrail` and `CompTrail` (`shell.cpp:932`, `:951`) also sample by the frame; reset them after a gap (`CompTrail` already does after 1 s, `shell/comp_trail.h:26`).
  5. Replace the 4 ms poll with an `eventfd` the MIDI and service threads write when they count an event and the screen thread waits on with a timeout.

  Zero-effort variant: `idle_interval {250000}`. Time-only changes then show up to 250 ms late on a still screen instead of 100 ms.
- **Gain:** battery Medium (it is the device's resting state), CPU Low.
- **Sound or behaviour change:** none in the audio (step 4 moves a check that sends an engine command to a thread that runs more often; same result, sooner). Visible: none if every dependency is covered.
- **Risk:** Medium. A missed dependency is a stale picture; the 1 s cap bounds it. The risky parts are the per-page live readouts, which I did not audit.
- **Effort:** M (S for the zero-effort variant and for step 5).
- **Confidence:** Medium. I read the shared code and grepped the pages for `s.now`; I did not read each page for other sources of motion.

### UI-6: Lists, the sample in hand and zoomed peaks are copied or recomputed every frame although generation counters exist
- **Where:** `src/melodee/shell/shell.cpp:847-849`, `:858-859`, `:864`, `:868-873`, `:875`; `shell/sample_library.cpp:76`, `:186-208`, `:260`
- **Thread and frequency:** screen, per frame, under `Shell::lock` and then the library's or store's lock
- **Now:**
  ```cpp
  out.sample = slot < sample_slots ? library_->slot(slot) : LoadedSample {};
  ...
  if (view.width() && slot < sample_slots && library_->detail(slot, view.from, view.to, out.detail.data(), unsigned(out.detail.size()), sides)) {
  ...
  out.files = library_->files(); out.sample_root = library_->root(); out.files_folder = library_->folder(); out.files_top = library_->top();
  ```
  `slot()` returns a `LoadedSample` by value (6.2 kB, four `std::string`s and a `std::vector<ZoneShape>`), so every frame allocates and frees. `files()` returns the whole `std::vector<SampleFile>` by value (two strings a file) on Samples. `detail()` reads up to 512 x 128 frames of both channels to make zoomed peaks on Trim and Chop. `store_->view()`, `plugins->view()` and `programs()` copy their lists on their pages. `SampleLibrary::generation()` ("Changes each time the list or a loaded sample changes", `shell/sample_library.h:171-172`), `ProjectStore::generation()` and `PluginRack::changes()` exist and the MIDI thread already uses them (`shell/lights.cpp:196`, `:284`, `:306`, `shell/plugin_jobs.cpp:293`).
- **Cost:** a folder of 500 files is 500 to 1000 heap allocations and as many frees a frame while Samples is open, 60 times a second while a preview plays; zoomed Trim or Chop reads up to 130,000 samples a frame. All of it extends the time the MIDI thread can be held (UI-2). Unmeasured; roughly 0.1-0.5 ms a frame on those pages.
- **Proposal:** keep the last generation seen beside each copy in the screen thread's `Snapshot` (it persists) and copy only when it moved; for `detail()` also key on `(slot, view.from, view.to)`. Add `out.sample` to the same gate.
- **Gain:** CPU Low-Medium on the browser and zoom pages, latency Low-Medium.
- **Sound or behaviour change:** none.
- **Risk:** Low: the counters are already trusted for the same data. Not checked: that `detail()`'s result can only change with the library's generation for a sample still streaming in.
- **Effort:** S.
- **Confidence:** High.

### UI-7: A sheet that owns the screen is drawn over a page that was drawn in full first
- **Where:** `src/melodee/melodee_app.cpp:465-466`; `src/melodee/pages/overlays.cpp:63-64`, `:32`, `:231`; partial covers at `overlays.cpp:73`, `:264`, `pages/name_sheet.cpp:24`, `pages/menu_sheet.cpp:84`, `ui/widgets.cpp:232`, `pages/seq_sheet.cpp:45`, `pages/track_grid.cpp:110-111`
- **Thread and frequency:** screen, per frame while a project loads, an update installs, or a sheet is open
- **Now:**
  ```cpp
  page_spec(frame->ui.page).draw(p, *frame);
  draw_overlays(p, *frame);
  ```
  ```cpp
  if (s.ui.update.phase) { draw_update_sheet(p, s); return; } // full screen until the app restarts or B1 closes it
  if (s.ui.loading) { draw_loading_sheet(p, s); return; } // it owns the screen and the controls until it is in
  ```
  Both sheets start with `p.fill(0, 0, zone::width, zone::height, colour::bg)`.
- **Cost:** during a load the progress bar changes, so the loop runs at up to 60 fps and each frame rasterises a page nobody sees, while the loaders want the CPU. The name sheet, the Exit sheet, the MENU and the Sequence sheet cover everything but one zone for as long as they are open. Unmeasured: the hidden page is most of a frame's drawing.
- **Proposal:** put `bool overlay_owns_screen(const Snapshot& s) { return s.ui.update.phase || s.ui.loading; }` beside `draw_overlays`, use it for those two lines, and skip `page.draw` in `MelodeeApp::draw` when it is true. For the partial covers, clip the page to the zone the sheet leaves (`g.reduceClipRegion`) before `page.draw`; `Paint::text` already skips text outside the clip (`ui/paint.cpp:231`).
- **Gain:** CPU Medium while loading, load time Low, battery Low.
- **Sound or behaviour change:** none.
- **Risk:** Low for the two full covers (one shared predicate). Medium for the partial covers: the clip must match each sheet's own fill.
- **Effort:** S.
- **Confidence:** High.

### UI-8: Meters and bars are drawn at sub-pixel sizes, so the picture differs on almost every frame they are on screen
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
- **Where:** `base/src/drivers/display.cpp:84-105` (`Display::present`), `:133-166` (`push`); `base/src/drivers/display_damage.h:54-60` (`plan`)
- **Thread and frequency:** screen per frame (compare), display worker per changed frame (plan and push)
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
  ```cpp
  for (int y = 0; y < height; ++y) {
      if (!std::memcmp(now + size_t(y) * width, was + size_t(y) * width, size_t(width) * 4)) continue;
  ```
  Screen thread: one 307 kB against 307 kB compare on every frame; on a change, the changed rows copied into `latest`, then all 307 kB copied into `pending` under the lock. Worker: `plan()` compares `pending` with `shown` row by row again, scans the changed bands pixel by pixel, copies each rectangle to the DRM buffer and to `shown`, and calls `drmModeDirtyFB` once a rectangle. No pixel format conversion happens in user space: the buffer is 32 bpp and the panel driver is a `mipi_dbi` one (`base/kernel/panel/sample_base_panel.c:31`), which converts each clip to RGB565 in the kernel inside that call. The SPI push is on the worker; the screen thread only waits when the previous push is still going out.
- **Cost:** about 0.15-0.3 ms for an unchanged frame and 0.4-0.8 ms of CPU for a changed one, before the kernel's part; unmeasured. Small beside the drawing.
- **Proposal:** `present()` already knows which rows changed. After a push completes, `latest`, `pending` and `shown` are equal, so those rows are exactly what `plan()` will find. Keep them as a 240-bit set, copy only those rows into `pending`, and let `plan()` start from the set instead of its first pass.
- **Gain:** CPU Low.
- **Sound or behaviour change:** none.
- **Risk:** Low-Medium: it relies on the invariant above; a failed push must mark every row.
- **Effort:** S-M (sample-base).
- **Confidence:** High on what the code does; Medium on the invariant in the error path.

### UI-11: Every small rounded rectangle goes through path rasterisation
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

## Checked and fine

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

## Could not determine

- The real `draw_us`, and how it splits between `Shell::snapshot()`, the page, the overlays and `present()`. The status line has one number for the first three. Adding `snapshot_us` and `present_us` to `ScreenStats` would rank UI-1, UI-2, UI-9 and UI-11 by measurement.
- What JUCE's software renderer costs per glyph, per rounded rectangle and per stroked path on the A55 (JUCE is not checked out).
- Whether the control surface's microcontroller sends anything periodic that counts in `hardware_events`. If it does, the screen never idles (the same mechanism as UI-3). `status` at rest shows it: `hardware_events` should not move.
- How much CPU the kernel spends per push (RGB565 conversion, and whether the SPI controller uses DMA or interrupts at 25 MHz).
- Whether a non-PI `std::mutex` held by the screen thread delays the MIDI thread measurably in practice (UI-2); a histogram of the wait in `Shell::lights()` would show it.
- Cache sizes on the RK3566, so the claim in UI-2 that the copy evicts the audio threads' data is an inference from the sizes, not a measurement.
- Per-page animation and live readouts (another auditor): they decide how much UI-4 and UI-5 save on each page.
