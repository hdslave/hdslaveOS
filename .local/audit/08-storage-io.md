# Audit: storage, sample loading, background jobs, web server and exports (IO)

## Coverage

**Read fully** (under `src/melodee/shell/` unless a folder is given): `sample_library.cpp`, `sample_library.h`, `sample_library_instruments.cpp`, `sample_library_stems.cpp`, `sample_files.cpp`, `sample_cache.cpp/.h`, `sample_decode.cpp/.h`, `sample_memory.h`, `load_pool.cpp/.h`, `card_warm.cpp/.h`, `stream_holds.h`, `web/web_server.cpp/.h`, `web/web_app.cpp/.h`, `web/mdns.cpp/.h`, `web/card_files.cpp`, `update.cpp/.h`, `card_access.cpp/.h`, `stems.cpp/.h`, `stems_jobs.cpp`, `tools/stems/stems_engine.c`, `tools/stems/build.py`, `sample_record.cpp`, `sample_capture.cpp`, `capture_ring.h`, `capture_jobs.cpp`, `audio_tracks.cpp/.h`, `audio_jobs.cpp`, `looper_files.cpp`, `project_store.cpp/.h`, `project_jobs.cpp`, `autosave.h`, `usage_log.h`.

**Read in part:** `model/project_file.cpp` (the Writer and Reader, `save_project` lines 233-330 and 720-792, `load_project` lines 794-830, 1296-1320 and 1500-1524, and the list of sections; the per-section field code was not read line by line), `looper_jobs.cpp` (`looper_tick`, 195-246), `auto_sample.cpp` (80-200), `mpc_program.cpp` (1-165, 282-305), `mpc_wav_slices.cpp` (24-100, 142-262), `model/sound_file.cpp`.

**Skimmed** (no finding unless named below): `mpc_project.cpp`, `xpm_write.cpp`, `mpc3_export.cpp`, `print_engine.cpp` (its thread loop and accessors, 160-345), `print_jobs.cpp` (95-135), `print_sheet.cpp`, `render.cpp`, `expansion_install.cpp`, `song_export.cpp`, `smf_jobs.cpp`, `progression_file.cpp`, `model/smf.cpp`, `project_kits.cpp`, `auto_sample.h`, `looper_jobs.h`, `web/web_bundle.cpp`.

**Read outside the area, because findings depend on who calls what:** `melodee_app.cpp` (40-130, 440-773: `service`, `read_machine`, `warm_card`, `card_access`, `card_update`, `web_service`), `shell/lights.cpp` (50-106, 170-365: the MIDI thread's pass), `shell/context.cpp` (55-135, 170-262, 1820-1834), `shell/chop_mode.cpp` (140-215), `shell/shell.cpp` (250-268, 800-885), `engine/sample_data.h/.cpp`, `engine/recall_ring.h`, `engine/take_recorder.h` (33-113), `engine/onsets.cpp` and `engine/root_find.cpp` (constants and loop structure only), `engine/looper.h` (140-200), `base/src/drivers/platform.cpp` (10-17, 57-70, 199-212), `base/src/runtime/controller.cpp` (95-135).

**Skipped:** `engine/stream_pager.cpp` and the rest of the shell core (other auditors), JUCE.

**Measured, not estimated:** the `sizeof` figures below come from a two-line probe compiled outside the repository on the host (clang, libc++, arm64): `Project` 538,968 B, `PatternBank` 491,520 B (2048 x `Pattern` 240 B), `LoadedSample` 6,240 B, `SampleFile` 64 B, `PlayedNote` 24 B. On the device (libstdc++, `std::string` 32 B) they differ by a few percent. Every time or CPU figure in this report is an estimate from reading the code and is marked so.

## Answers to the assignment's questions

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

## Findings

### IO-1: Every store request re-lists the project folder and fully parses every project in it
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
- **Where:** `src/melodee/engine/sample_data.h:39` (`SampleData`), `src/melodee/engine/sample_data.cpp:90-91` (`wav_frames`), `src/melodee/shell/sample_memory.h:34-37` (`kit_sample_bytes`).
- **Thread and frequency:** on load; the memory is held for the session.
- **Now:**
  ```cpp
  // sample_data.cpp:91: a mono file's one side decoded onto both
      for (unsigned n = 0; n < frames; ++n) { left[n] = sample(n, 0); right[n] = sample(n, std::min(1u, channels - 1)); }
  // sample_memory.h:36
      return std::min<uint64_t>(frames, uint64_t(stream_most_seconds) * rate) * 2 * sizeof(float);
  ```
- **Cost:** 8 bytes a frame whatever the file: a mono one-shot is held twice. A kit of 128 two-second mono hits at 44.1 kHz is 90 MB where 45 MB would do. The device has 2 GB and a 256 MB floor, so this limits large kits and instruments in memory rather than typical projects.
- **Proposal:** for a mono source leave `right` empty and give `SampleData` accessors (`const float* l() const`, `const float* r() const { return right.empty() ? left.data() : right.data(); }`) for the readers; `kit_sample_bytes` takes the channel count. About 100 reads of `.right` across the engine and shell would move to the accessor.
- **Gain:** memory Medium for mono-heavy kits; no CPU change expected.
- **Sound or behaviour change:** bit-exact (the same values are read).
- **Risk:** Medium: many call sites, some of which write both sides (`as_written`, `fade_edges`, `SampleFill`).
- **Effort:** M to L.
- **Confidence:** High on the format; the engine's readers were counted with grep, not read.

### IO-14: The audio-track recorder's thread spins a whole core for as long as a take lasts
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

## Checked and fine

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

## Could not determine

- The read and analyse shares of a project load on the device. The app prints them (`load: ... threads: read X, analyse Y ...`, `sample_library.cpp:1318`); one journal line settles IO-4.
- What one gzip MPC program costs in `read_mpc_json_program` on the A55 (IO-2), and how long `ProjectStore::scan()` takes with a real folder (IO-1).
- Whether `usb0` is up with an address when no computer is attached. If it is not when the first service pass runs, mDNS never starts (`mdns.cpp:172`, `melodee_app.cpp:737-745`).
- Whether the base locks memory (`mlockall`). The recall ring and the take buffer are 66 MB, sized for 96 kHz and zero-filled at start (`engine/recall_ring.h:24,28`, `engine/take_recorder.h:34,40`). Sizing them for the device's rate would save about 36 MB at 44.1 kHz, but needs an allocation off the audio thread when the rate changes; engine code, for that auditor.
- How often the IO-8 stall occurs: it needs a trace of the MIDI thread's longest pass while a freeze prints during a project load.
- Whether `host.playing()` covers the Looper playing alone; if not, autosave (and IO-3's rewrite) can run while loops play.
- How the card is mounted (flush or sync options) and how the kernel's exFAT driver caches negative lookups: they scale the cost of IO-11 and of the `fs::exists` name searches (`free_name`, `TAKE n.wav`), which I left out as cold code.
- `plugin/plugin_rack.cpp:791` waits with a 50 ms timeout (20 wakeups a second). Outside this area; not read.
