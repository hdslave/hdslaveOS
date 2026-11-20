# Audit: screen pages (`src/melodee/pages/`)

## Coverage

**Read fully** (every draw function line by line; the pad / wheel / push / soft handlers only far enough to tell them from draw code): `stage_page.cpp`, `step_edit_page.cpp`, `roll_page.cpp`, `patterns_page.cpp`, `tracks_page.cpp` with `track_grid.cpp`, `pads_page.cpp`, `song_page.cpp`, `looper_page.cpp`, `chop_page.cpp`, `sampler_pages.cpp` with `sampler_common.cpp/.h`, `samples_page.cpp`, `sounds_page.cpp`, `record_page.cpp` with `input_meter.cpp`, `edit_page.cpp`, `keys_page.cpp`, `scenes_page.cpp`, `sends_page.cpp`, `chain_page.cpp`, `compressor_page.cpp`, `knob_fx_page.cpp`, `fader_page.cpp`, `mod_page.cpp`, `motion_page.cpp`, `place_bar.cpp`.

**Skimmed** (draw function read, the rest grepped for loops, clocks, live data, file access and locks): `project_page.cpp`, `plugins_page.cpp`, `plugin_presets_page.cpp`, `input_config_page.cpp`, `helper_pages.cpp` (Tempo), `instrument_map.cpp` (`zone_tiles`, `map_picture`, `instrument_picture`, `facts_row`, `problems`), `punch_page.cpp`, `time_correct_page.cpp`, `settings_page.cpp`, `auto_sample_page.cpp`, `seq_sheet.cpp`, `overlays.cpp` (`draw_overlays`, `draw_lesson`, `draw_loading_sheet`), `plugin_view.cpp`, `print_page.cpp`, `stems_page.cpp`, `card_access_page.cpp`, `midi_config_page.cpp`, `flex_beat_page.cpp`, `fx_drawing.cpp`.

**Skipped:** `filter_env_page.cpp`, `pad_filter_page.cpp`, `pad_mix_page.cpp` (under 1 KB each, wrappers round `kit_page`), `menu_sheet.cpp`, `ask_sheet.cpp`, `name_sheet.cpp`, and `common.cpp` except `header_of`, `cards_of`, `region_wave`, `pad_name`, `name_case` (another auditor has it). JUCE is not in the tree.

**Read outside the area, only to confirm who calls what and how often:** `base/src/runtime/run_app.cpp:119-158`, `base/src/drivers/display.cpp:84-105`, `src/melodee/melodee_app.cpp:432-468`, `src/melodee/shell/shell.cpp:797-958` (the snapshot), `shell/sample_library.cpp:60-260`, `engine/engine_host.cpp:3185-3203` and `1413-1446`, `plugin/preset_rows.h`, `plugin/plugin_panel.h`, `ui/paint.cpp`, `ui/waveform.cpp`.

### How often a page's draw function runs (verified, it sets the weight of everything below)

- `MelodeeApp::draw` calls `shell.snapshot(*frame, now)`, then `page_spec(frame->ui.page).draw(p, *frame)`, then `draw_overlays(p, *frame)` (`melodee_app.cpp:437, 465-466`). Every draw function named below is reached from that `draw` pointer unless I say otherwise.
- A frame counts as changed when any row differs under `memcmp` (`display.cpp:93`). After a changed frame the loop draws every 16.7 ms until 500 ms have passed without one (`run_app.cpp:127, 151`). So **one changed pixel buys about 30 more full redraws**, and anything that changes more often than twice a second holds the page at 60 fps.
- The place bar is drawn last on every page (`overlays.cpp:92`) and its fill moves by whole pixels while the transport runs (`place_bar.cpp:53-59`). A four-bar pattern at 120 BPM moves it a pixel about every 27 ms. So **while playing, every page is redrawn in full 60 times a second**, whatever the page shows; stopped, a still page is redrawn 10 times a second.

A page's per-frame cost is therefore paid 60 times a second while playing and 10 times a second while stopped, and 60 times a second while stopped if anything on it moves.

**All costs below are estimates; nothing was measured.** To measure on the device: the status line prints `screen_presented` (frames drawn), `screen_pushed` (frames that changed) and `screen_draw_us` (the last frame's draw time, snapshot included) (`base/src/runtime/reports.cpp:12-25`, `run_app.cpp:47`). Read them twice ten seconds apart on each page, stopped and playing.

Quotes are abridged: `...` marks source lines left out, and a first line such as `// file.cpp:12-20` is my label, not source.

One assumption runs through several findings. `Paint::fill` with a radius calls `g.fillRoundedRectangle`, without one `g.fillRect` (`ui/paint.cpp:245-249`). From JUCE's public source (not checked out here), `fillRoundedRectangle`, `fillEllipse`, `drawLine` and `strokePath` each build a `juce::Path` and rasterise it through an edge table, with heap allocation per call; `fillRect` on an untransformed rectangle is a direct span fill. I assume a small path fill costs several microseconds on a Cortex-A55 and a plain rectangle well under one.

## Findings

### PAGE-1: Readouts and pulses that keep a stopped page at 60 fps
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
- **Where and now:**
  - `src/melodee/pages/project_page.cpp:153-155` (`draw`): `auto v = s.projects; sort_view(v, s.ui.project_sort);` A deep copy of the store's view (entries, the settings text, the user sounds, every folder name) and a `std::stable_sort` over all entries, per frame. The Snapshot's own copy was already made per frame at `shell.cpp:847`.
  - `src/melodee/pages/plugin_presets_page.cpp:32` (`draw`): `const auto rows = preset_rows(v.list, inside, v.current);` `preset_rows` (`plugin/preset_rows.h:39-65`) walks every program, copies each name into a `PresetRow`, and copies the presets again at `:63`. The program list itself is shared, not copied (`plugin_panel.h:57-71`, whose comment names Surge XT's thousands of presets).
  - `src/melodee/shell/shell.cpp:874-876` (`Shell::snapshot`, on Samples): `out.files = library_->files();` copies the folder's `std::vector<SampleFile>` (two `std::string` each) under both locks, per frame.
- **Thread and frequency:** screen, per frame, on those pages.
- **Cost:** for N entries, on the order of N to 2N string copies and allocations a frame for six rows drawn: a few hundred microseconds a frame for thousands of presets or a folder of a thousand samples (unmeasured). The copy on Samples also lengthens the shell lock.
- **Proposal:** the listings are already cached and scanned off the screen thread; only the hand-over is wasteful. All three sources have a change counter: `SampleLibrary::generation()` (`sample_library.h:172`), `ProjectStore::generation()` (`project_store.h:119`), `PluginRack::changes()` (`plugin_rack.h:111`). Hand lists over as `std::shared_ptr<const std::vector<...>>` swapped on change, as `PluginProgramList` already does; sort the project view when it is listed or when K3 changes the order; keep `preset_rows`' result keyed on the list's pointer, the folder and `current`.
- **Gain:** CPU Low to Medium, latency Low.
- **Sound or behaviour change:** none.
- **Risk:** Low.
- **Effort:** M (three places, plus the handlers that also call `view_of`).
- **Confidence:** High.

### PAGE-7: Song draws the parts of long clips that are out of view
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

## Checked and fine

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

## Could not determine

- **What a frame costs on the device**, and how it splits between the snapshot, text and shapes. Every estimate above rests on assumed costs for JUCE's `fillRoundedRectangle`, `strokePath` and `fillRect`. `screen_draw_us` per page would settle it.
- **Whether the input level stands still with nothing plugged in** (the ADC's noise floor against the meter's -96 dB bottom), so whether Record, Input Config and an audio track's Stage INPUT ever reach idle today.
- **Whether "CPU n %" on Tracks flips while stopped.** It depends on how much the block time jitters around an integer boundary.
- **Whether a plug-in's load reaches exactly zero when its idle gate closes** (`plugin_rack.cpp:293` multiplies by 0.95 a block; the panel shows tenths of a percent), so whether Stage of a plug-in without presets idles when stopped.
- **The longest sample kept in memory** before the library streams it, which sets the worst case of PAGE-5.
- **`frame()`, `cards_of()` and the chrome every page calls** (`common.cpp`, `ui/chrome.cpp`, `ui/knobs.cpp`): shared by all pages and probably a large part of each frame; another auditor has them.
- **The frame loop's policy.** Several elements change at a low rate (a pulse at 4 Hz, the step chase at 8 Hz at 120 BPM, a load readout at 20 Hz, the place bar by pixels), yet any change buys 500 ms at 60 fps. A draw rate that follows the rate of change would cut more than any single page fix here; it is outside `pages/`.
