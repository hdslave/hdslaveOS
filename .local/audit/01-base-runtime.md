# Audit: sample-base runtime, drivers, build and boot scripts

## Coverage

**Read fully:**
`base/src/runtime/`: run_app.cpp, controller.cpp, controller.h, commands.cpp, control_socket.cpp, session.h, app.h, screen.cpp, options.cpp, options.h, reports.cpp, inventory.cpp, util.h.
`base/src/drivers/`: audio.cpp, audio.h, display.cpp, display.h, display_damage.h, hardware_midi.cpp, hardware_midi.h, usb_audio.cpp, usb_audio.h, screen_mirror.cpp, screen_mirror.h, mirror_frame.cpp, jpeg_encoder.cpp, platform.cpp, platform.h, mixer.cpp, mixer.h, jacks.cpp, clock_io.cpp, power_button.cpp, system_services.cpp, system_services.h, card_share.cpp, codec_power.cpp, codec_power.h, panel_driver.cpp, kernel_module.cpp, sd_update.cpp, usb_gadget.cpp.
`base/src/dsp/`: frame_ring.h, recorder.h. `base/src/surface/`: leds.h, midi.h, pads.h.
`base/kernel/panel/sample_base_panel.c`.
Build and boot: base/base.mk, base/Makefile, Makefile, build.sh, base/build.sh, release.sh, base/release.sh, base/tools/Dockerfile, tools/plugin_support/Dockerfile, base/install/sd-boot.sh, sd-worker.sh, restore-stock.sh, sd-update.sh, src/main.cpp, tools/make_release.py, base/tools/make_release.py.

**Skimmed:** base/src/surface/controls.h (second half), led_colours.h (first 40 lines); base/device.py (staging and start); base/docs/HARDWARE.md, APPS.md, VALIDATION.md (by grep). The remaining small driver headers (jacks.h, clock_io.h, power_button.h, card_share.h, usb_gadget.h, kernel_module.h, jpeg_encoder.h, mirror_frame.h, sd_update.h) were only grepped for members.
Outside my area, read only to learn how often the base's hooks are used and what they are handed: src/melodee/melodee_app.cpp lines 25–215 and 425–575, src/melodee/shell/input_route.cpp, src/melodee/shell/midi_io.cpp lines 575–631, src/melodee/tools.cpp lines 2870–2928 (the boot self-test), src/melodee/engine/render_pool.cpp lines 40–90.

**Skipped:** base/src/demos, base/src/starter, base/tests, base/kernel/uvc, base/kernel/vepu, JUCE (not checked out), tools/stems and tools/plugin_support/build.py (grepped for compiler flags only).

Nothing was built or run. Every cost below is an estimate from reading the code unless it says otherwise.

## Findings

### Idle wakeup inventory (question 1)

App idle: no notes, static screen, no computer recording, Screen Mirror off, 44.1 kHz. Base threads only; the app's own threads (render pool, pager, loaders, web server) are not counted.

| Thread | Policy | Wakeups per second when idle | What each pass does | Where |
|---|---|---|---|---|
| Audio | FIFO 30 | 344.5 (375 at 48 kHz, 750 at 96 kHz) | 1 capture read, `render()`, 1–3 `snd_pcm_writei` + 1 `snd_pcm_wait` | audio.cpp:117-143 |
| MIDI | FIFO 15 | at least 200 | one non-blocking read per open input port (3 on the device: surface, DIN, USB gadget), `midi_output()`, `lights()`, flush, `ppoll`; once a second a full ALSA port scan | controller.cpp:124-161 |
| Service | normal | about 200 | `poll` 5 ms + 3 more non-blocking reads (sd-bus, jack control, power key), then `App::service()`; four times a second about 15 configfs/sysfs calls | controller.cpp:32-38, 85-100 |
| Screen (main) | normal | 250 input polls + 10 full draws | 3 atomic loads per poll; `App::draw()` and a 307 KB compare per draw | run_app.cpp:127-157 |
| Display worker | normal | 0 (condition variable) | — | display.cpp:119 |
| USB audio out | FIFO 20 | 50, plus 4 ALSA card scans | `sleep_for(20 ms)`; `find_target()` every 250 ms | usb_audio.cpp:287-318 |
| USB audio in | FIFO 20 | 50 + 4 scans, only while the app has USB-C input selected | same loop | usb_audio.cpp:132-156 |
| Screen Mirror worker | normal | 5 when never used; 30 or 60 after the first stream has ended (BASE-13) | `poll` 200 ms | screen_mirror.cpp:533-534 |
| JackDetect, ClockIo, SystemServices, PowerButton, CardShare, mixer | — | no thread of their own | polled by the service thread on every pass | controller.cpp:85-100 |

Total about 1,060 wakeups a second, of which about 345 (audio) are inherent and about 715 are polling. The SoC has no idle state deeper than WFI, so a wakeup costs its active time rather than an expensive idle exit: my estimate is 5–25 µs each (timer interrupt, on PREEMPT_RT a hand-off through the timer softirq thread for normal-priority sleepers, two context switches, the syscalls listed), which puts all the polling together at roughly 1–2 % of one core. Unmeasured. To measure: sample `voluntary_ctxt_switches` in `/proc/<pid>/task/*/status` twice, 10 s apart, and `utime`/`stime` in `/proc/<pid>/task/*/stat`. The base's threads are not named (`pthread_setname_np` is used only by the app's render pool), so naming them first makes this readable.

How each could block instead is in BASE-2 (screen), BASE-6 (service), BASE-7 (MIDI), BASE-9 (USB) and BASE-13 (mirror).

### BASE-1: No CPU power policy: the cluster sits at 1800 MHz and 1.10 V all the time
- **Where:** nowhere, which is the finding. `grep -rniE "cpufreq|governor|scaling_(max|min|gov)|schedutil"` over the repository (without `.git`, `.local`) returns nothing. The place a policy fits: `base/src/drivers/platform.cpp:108-124` (`Platform::~Platform`, `Platform::brightness`), `base/install/restore-stock.sh:99-104`.
- **Thread and frequency:** whole system, always.
- **Now:** the kernel's default `performance` governor is never changed. The existing pattern for a setting the runtime changes and puts back:
  ```cpp
  Platform::~Platform() {
      if (!changed_backlight.empty()) write_text(changed_backlight / "brightness", saved_brightness);
  }
  ```
- **Cost:** all four cores share one rail, and it stays at 1.10 V even while every core is in WFI. Energy per unit of DSP work scales with V²: 0.90² / 1.10² = 0.67, so the same project costs about a third less CPU energy at the 0.90 V points, and leakage (paid in WFI too) falls with the voltage as well. 1416 MHz is the fastest 0.90 V point; every point below it has the same voltage, so going slower saves nothing more per unit of work. Throughput loss at 1416 MHz: 21 %. Unmeasured: compare supply current with `scaling_max_freq` at 1800000 and 1416000, idle and with a heavy project playing; confirm with `cpufreq/policy0/stats/time_in_state`.
- **Proposal:** a `Platform` method, called from the service thread, restored on exit and in the recovery script:
  ```cpp
  bool Platform::cpu_max_khz(unsigned khz) {            // platform.cpp
      auto file = sys / "devices/system/cpu/cpufreq/policy0/scaling_max_freq";
      if (saved_cpu_max.empty()) saved_cpu_max = read_text(file);
      return !saved_cpu_max.empty() && write_text(file, std::to_string(khz));
  }   // ~Platform(): write saved_cpu_max back
  ```
  ```sh
  # restore-stock.sh, beside the pmdown_time lines
  for P in /sys/devices/system/cpu/cpufreq/policy*; do
      cat "$P/cpuinfo_max_freq" > "$P/scaling_max_freq" 2>/dev/null || true
  done
  ```
  Keep the `performance` governor so the frequency is fixed and no transition happens under the audio thread. The app starts at 1416000 and steps to 1608000 / 1800000 from its own measured render load (it already has one), with hysteresis: up after a few blocks above about 60 % or on any xrun, down after about 10 s below about 35 %. Do not use `schedutil`: by default it requests the maximum frequency whenever a real-time task is runnable, and the FIFO audio and render threads wake 344 times a second, so it would sit at 1800 MHz or flap; and its transitions run in a deadline-class kernel thread that preempts FIFO threads while it talks to the regulator.
- **Gain:** battery High (the largest software lever on SoC power I found).
- **Sound or behaviour change:** bit-exact.
- **Risk:** Medium. 21 % less headroom until the step up; a sudden jump in load can xrun before the step. `platform.h:38` says "No raw GPIO, I2C, regulator ... writes" and HARDWARE.md:60 says "no radio, network or thermal policy changes", so this crosses a boundary the base set for itself and should be opt-in by the app, as brightness is.
- **Effort:** S for a fixed cap, M with the adaptive step.
- **Confidence:** Medium. Not checked: that the vendor kernel lets root write `scaling_max_freq` (standard), how long a voltage step takes, the actual current saved.

### BASE-2: The idle screen loop polls at 250 Hz and redraws the whole screen 10 times a second; busy is always 60 fps
- **Where:** `base/src/runtime/run_app.cpp:127-157` (`run_app`)
- **Thread and frequency:** screen; 250 polls and 10 draws a second idle, 60 draws a second busy.
- **Now:**
  ```cpp
  constexpr std::chrono::microseconds busy_interval {16667}, idle_interval {100000}, idle_after {500000}, input_poll {4000};
  ...
  bool idle = !display || start - last_change > idle_after;
  next = std::max(next + (idle ? idle_interval : busy_interval), ScreenClock::now() - busy_interval);
  if (!idle) { std::this_thread::sleep_until(next); continue; }
  auto seen = inputs();
  while (ScreenClock::now() < next && inputs() == seen && session.running.load() && !interrupted && !session.screenshot.load())
      std::this_thread::sleep_for(input_poll);
  ```
  The app has no way to say "nothing to draw": whether a frame changed is found only by drawing all of it and comparing all of it (`Display::present`).
- **Cost:** idle, forever: 10 × (`App::draw()` of 320 × 240 by JUCE's software renderer + a 307 KB compare) and 250 timer wakeups a second. Busy: 60 full draws a second even when only a play head moves. The draw time is already recorded (`screen_draw_us` in `--ctl status`); if it is 4 ms (unmeasured guess) idle costs 4 % of a core and busy 24 %.
- **Proposal:** three independent steps.
  1. Event-driven wake. Add an eventfd (or a futex word) to `Session`; the MIDI thread signals it where it counts events (controller.cpp:188, 195), the service thread for the power key, `screenshot` and `prompt`. Arm it only while the screen thread waits, so busy MIDI traffic pays no syscall:
     ```cpp
     session.screen_wake.armed.store(true);
     if (inputs() == seen) { pollfd p {session.screen_wake.fd, POLLIN, 0}; poll(&p, 1, ms_until(next)); }
     // signal(): if (armed.exchange(false)) write(fd, &one, 8);
     ```
  2. Let the app report nothing to draw: `virtual bool App::draw_needed()` (default true), checked instead of a full draw on the idle tick, plus a `session().wake_screen()` for app-side changes. Cheaper still, with no API change: `image` is created once (run_app.cpp:129) and keeps the last frame, so an app may return from `draw()` without painting when its own state is unchanged; `present()` then finds nothing changed after one compare. The app must repaint once after a remote prompt (`session().prompting`).
  3. A lower busy cap: 33 ms between frames unless there was input in the last 250 ms (then 16.7 ms, so knob turns stay smooth), or let the app name the interval.
- **Gain:** battery High idle, CPU Medium busy.
- **Sound or behaviour change:** steps 1 and 2 none; step 3 visible (30 fps animation; a full-screen push already takes 54 ms).
- **Risk:** Low to Medium. A wake source that is missed shows as a stale screen until the next safety tick; keep a 250–500 ms tick at first.
- **Effort:** M (step 1 alone is S).
- **Confidence:** High on the mechanism; the draw cost is unmeasured and belongs to the UI audit.

### BASE-3: The backlight never dims or blanks, and the base has no idle signal for it
- **Where:** `base/src/drivers/platform.cpp:111-124` (`Platform::brightness`); used only by `src/melodee/melodee_app.cpp:529-535` and the `brightness` command (commands.cpp:62).
- **Thread and frequency:** service; on a settings change only.
- **Now:**
  ```cpp
  unsigned screen = shell.screen();
  if (screen && screen != screen_applied) {
      auto backlight = hardware.platform.backlight();
      if (backlight.maximum > 0) hardware.platform.brightness(unsigned(std::max(1L, backlight.maximum * long(screen) / 100)));
      screen_applied = screen;
  }
  ```
  The setting is 20–100 %, default 100 % (settings.h:88). I found no idle timer, dimmer or blanking anywhere (grep for dim, blank, idle, standby in src/melodee and base/src). The pad and button LEDs have none either.
- **Cost:** the backlight and the 16 RGB pad LEDs are probably the largest loads after the SoC; a 2.4 inch panel's backlight is typically 0.15–0.25 W at full drive. Unmeasured: read the supply current at brightness 6, 1 and 0, and with `led off`.
- **Proposal:** the mechanism exists; add the policy. In the app's `service()`: when `session().hardware_events` has not moved for N minutes (and the transport is stopped), `platform.brightness(1)`, later 0; restore on the first event. A small base addition makes the LEDs follow: a dim factor in `SurfaceLights::update()` applied to RGB values before they are sent (leds.h:139-141), set by the same timer. The panel keeps its picture from its own memory while dark, so no redraw is needed on wake. A full blank through DRM (disabling the CRTC also switches the backlight off in mipi-dbi) is not needed.
- **Gain:** battery High (estimate).
- **Sound or behaviour change:** visible; make it a setting.
- **Risk:** Low. Whether level 0 switches the LED driver fully off is unknown. `Platform` restores brightness only on a normal exit (platform.cpp:108-110); after a crash a dimmed level would stay until the stock app sets its own, so add it to restore-stock.sh.
- **Effort:** S (backlight), M (with LED dimming).
- **Confidence:** Medium: the absence is certain, the saving is not measured.

### BASE-4: MIDI clock or Active Sensing from an external device keeps the screen at 60 fps
- **Where:** `base/src/runtime/controller.cpp:195-197` (`Controller::midi_packet`), `base/src/runtime/run_app.cpp:128, 154-157`
- **Thread and frequency:** screen; 60 draws a second for as long as the device is connected.
- **Now:** every external packet counts as input, real-time bytes included (`MidiStreamParser::feed` emits each byte ≥ 0xF8 as a packet, surface/midi.h:24-27):
  ```cpp
  ++session.external_events;
  if (packet.size == 1 && packet.bytes[0] == 0xf8) ++session.clocks;
  app.midi(port, packet);
  ```
  ```cpp
  auto inputs = [&] { return session.hardware_events.load() + session.external_events.load() + session.synthetic_events.load(); };
  ...
  if (inputs() != seen) { last_change = ScreenClock::now(); next = last_change; }
  ```
  Setting `last_change` makes the loop "busy" for `idle_after` (500 ms). Active Sensing arrives every 300 ms or less and MIDI clock 48 times a second at 120 BPM, so the idle rate is never reached again: a static screen is drawn 60 times a second (about 40 on average with Active Sensing alone).
- **Cost:** 4–6 times the idle draw work of BASE-2 whenever a keyboard that sends Active Sensing, or any clock source, is plugged in. Unmeasured; check `screen_presented` against time in `--ctl status` with such a device attached.
- **Proposal:** a separate wake counter that leaves out system real-time messages:
  ```cpp
  ++session.external_events;
  if (packet.size != 1 || packet.bytes[0] < 0xf8) ++session.screen_events;   // run_app's inputs() reads this one
  ```
  `external_events` stays as the diagnostic it is in the status line.
- **Gain:** battery Medium (only with such a device attached).
- **Sound or behaviour change:** none.
- **Risk:** Low. An app that shows incoming clock on screen still redraws on the idle tick.
- **Effort:** S.
- **Confidence:** High on the mechanism. Not checked: whether the control surface itself sends anything periodically (see "Could not determine").

### BASE-5: Build flags: no `-mcpu`, math functions keep errno, no eager binding, no section garbage collection
- **Where:** `base/base.mk:21-23, 36`
- **Thread and frequency:** every thread; build time.
- **Now:**
  ```make
  CXXFLAGS := -std=c++17 -O2 -pthread -MMD -MP
  CFLAGS := -O2 -MMD -MP
  ...
  	$(CXX) $(OBJECTS) $(LDLIBS) -o $@
  ```
  There is no `LDFLAGS` at all. tools/stems/build.py:38 already builds the stem tool with `-mcpu=cortex-a55`.
- **Cost:** (a) GCC 12 for generic ARMv8-A tunes for an out-of-order core and calls out-of-line helpers for every atomic read-modify-write (`-moutline-atomics` is the default); the A55 is in-order and has the ARMv8.1 atomics. (b) With errno kept, `lround`/`lrint` are library calls instead of one `fcvtas` instruction, and `sqrt` cannot be hoisted or vectorised; the engine has about 75 `lround`/`lrint` sites and 28 `sqrt` sites (grep count; which are per sample is for the engine audit). (c) GCC 12's `-O2` vectoriser uses the `very-cheap` cost model, which refuses loops that need a runtime alias check or a scalar tail, that is, most `float*` block loops. (d) Unused JUCE code stays in the binary, which lives in RAM (tmpfs) and is hashed three times at boot (BASE-11). Unmeasured; the app's own DSP benchmark (src/melodee/dsp_bench.cpp) and `screen_draw_us` are the measures.
- **Proposal:**
  ```make
  ARCH_FLAGS ?= -mcpu=cortex-a55
  CXXFLAGS := -std=c++17 -O2 $(ARCH_FLAGS) -fno-math-errno -fno-trapping-math -ffunction-sections -fdata-sections -pthread -MMD -MP
  CFLAGS := -O2 $(ARCH_FLAGS) -ffunction-sections -fdata-sections -MMD -MP
  LDFLAGS ?= -Wl,--gc-sections -Wl,-z,relro -Wl,-z,now
  # link: $(CXX) $(OBJECTS) $(LDFLAGS) $(LDLIBS) -o $@
  ```
  Then try `-fvect-cost-model=dynamic` on the engine and `base/src/dsp` objects only, and measure. `-fno-plt` is not worth it once `-z now` is there. Not proposed: `-ffast-math` (reorders arithmetic), `-O3` everywhere (code growth on a 32 KB instruction cache; build.py notes it measured no faster for the stem tool), LTO (see below).
- **Gain:** CPU Medium (a few percent expected; more where `lround` sits in a per-sample loop), memory and load time Low.
- **Sound or behaviour change:** `-fno-math-errno`, `-fno-trapping-math`, `-mcpu`, section GC and `-z now` do not change any arithmetic result by themselves: bit-exact expected. One caveat applies to every codegen change: C++ builds with `-ffp-contract=fast` by default (the ISO-mode exception is for C only), so the compiler already fuses multiply-adds wherever inlining puts them side by side, and a change in inlining can move one. Confirm with a render hash before and after (tests/melodee.cpp already has `print_hash`); if it differs the change is rounding only. `-fvect-cost-model=dynamic`: same expectation, same check. LTO: rounding-only risk is real (cross-file inlining creates new fusions); treat as a separate, measured step.
- **Risk:** Low. `-mcpu=cortex-a55` emits ARMv8.2 instructions, so the self-test that build.sh runs in the container needs an ARMv8.2 host (Apple Silicon is; an older arm64 CI machine is not); `ARCH_FLAGS` can be overridden with `-mtune=cortex-a55` there.
- **Effort:** S.
- **Confidence:** Medium. Nothing was compiled; the `lround` and vectoriser statements are from GCC's documented behaviour, not from this build's assembly.

### BASE-6: The service thread wakes 200 times a second and makes four syscalls each time for sources that could all be waited on
- **Where:** `base/src/runtime/controller.cpp:32-38` (`Controller::run`), `85-100` (`poll_platform`), `base/src/runtime/control_socket.cpp:39-43`
- **Thread and frequency:** service every 5 ms.
- **Now:**
  ```cpp
  while (session.running.load()) {
      record_surface_packets();
      MidiCatalog update;
      while (catalog_updates.pop(update)) catalog = std::move(update);
      poll_platform(session);
      server.serve(5, [&](const std::string& request) { return command(session, request); });
  }
  ```
  Per pass: `poll()` on the control socket with a 5 ms timeout, one non-blocking socket read inside `sd_bus_process()` (system_services.cpp:72), one `snd_ctl_read()` on the jack control (jacks.cpp:56), one `read()` on the power key's input device (power_button.cpp:50), then `App::service()`. Four times a second `CardShare::poll()` adds `opendir`/`readdir`, two or three file reads and directory listings of configfs (card_share.cpp:96-109). `ClockIo::poll()` and the mixer's speaker hold only take a mutex.
- **Cost:** about 200 wakeups and 800 syscalls a second, each wake through the timer softirq thread on PREEMPT_RT because the thread is not real-time. Estimated 0.3–0.6 % of a core before the app's own `service()` work. Unmeasured.
- **Proposal:** one `poll()` over every source, with a wake eventfd for the app and a longer default tick:
  ```cpp
  pollfd fds[] {{server.fd(), POLLIN, 0}, {services.fd(), POLLIN, 0}, {jacks.fd(), POLLIN, 0},
                {power.fd(), POLLIN, 0}, {clock.fd(), POLLIN, 0}, {session.service_wake.fd, POLLIN, 0}};
  poll(fds, 6, app.service_interval_ms());   // 5 today; 50 when the app has nothing queued
  ```
  (`sd_bus_get_fd`, `snd_ctl_poll_descriptors`, the evdev and GPIO event fds are all pollable; poll ignores a negative fd.) Timed duties bound the timeout: the speaker hold's `until`, CardShare's 250 ms follow (1 s would do while the card is home). `App::service_interval_ms()` defaults to 5 so other apps keep today's behaviour; Melodee returns a longer one unless a request is pending, and other threads call `session().wake_service()` when they queue one.
- **Gain:** battery Medium.
- **Sound or behaviour change:** none if the wake is used; otherwise slow requests are picked up to 50 ms later.
- **Risk:** Medium. `App::service()` is documented as "every few milliseconds" (APPS.md:101) and Melodee does a lot there (saves, loads, looper memory, card requests, the power key); each needs the wake or must tolerate the longer tick.
- **Effort:** M.
- **Confidence:** Medium. The syscall inside `sd_bus_process()` is from my knowledge of libsystemd, not verified on the device's version.

### BASE-7: The MIDI thread runs the lights tick every 5 ms and reads every port on every pass
- **Where:** `base/src/runtime/controller.cpp:123-161` (`Controller::run_midi`), `base/src/drivers/hardware_midi.cpp:118-139` (`HardwareMidi::wait`, `read`)
- **Thread and frequency:** MIDI every 5 ms (FIFO 15).
- **Now:**
  ```cpp
  if (now >= lights_due) {
      app.lights(lights, now);
      lights.update(send_surface, 48);
      lights_due = now + std::chrono::milliseconds(5);
  }
  midi.flush();
  auto until = std::min({lights_due, output.wake, release, scan});
  midi.wait(std::chrono::duration_cast<std::chrono::nanoseconds>(until - Clock::now()).count());
  ```
  After each wake `midi.read()` calls `snd_rawmidi_tread()`/`read()` on every open input whether or not `ppoll` reported it ready (the `revents` are thrown away at hardware_midi.cpp:129).
- **Cost:** 200 wakeups a second with 1 `ppoll` + 3 failed reads each on the device's three ports: about 800 syscalls a second, plus `App::lights()` and `App::midi_output()` 200 times a second. LEDs do not need 200 updates a second. Unmeasured.
- **Proposal:** (1) lights at most every 5 ms after input, otherwise every 20 ms: keep `last_lights`, use `spacing = input_this_pass ? 5 ms : 20 ms`. A pad hit then lights as fast as today, and the idle rate falls from 200 to 50 a second. (2) Keep the `pollfd` array from `wait()` and read only ports whose `revents` are set (all of them on the once-a-second scan pass). Timed MIDI output is not affected: the app already schedules it with `wake_at()` (midi_io.cpp:600, 630).
- **Gain:** battery Medium to Low.
- **Sound or behaviour change:** none for sound; LED animation steps at 50 Hz instead of 200 Hz when nothing is touched.
- **Risk:** Low to Medium. APPS.md:98-99 promises `lights()` every 5 ms and `midi_output()` at least every 5 ms, and Melodee also drains its remote-control queues in `lights()` (melodee_app.cpp:166-173), which would then wait up to 20 ms. Document the new contract.
- **Effort:** S.
- **Confidence:** High on the counts; the cost of Melodee's `lights()` itself is outside my area.

### BASE-8: Real-time hazards on the audio thread: lazy symbol binding, no locked memory, priority under every IRQ thread
- **Where:** `base/base.mk:36` (link line), `base/src/drivers/audio.cpp:109-111` (`AudioDevice::run`), whole repository for `mlockall` (absent)
- **Thread and frequency:** audio per block; the hazards are rare events, not steady cost.
- **Now:**
  ```cpp
  sched_param priority {}; priority.sched_priority = 30;
  int scheduling = pthread_setschedparam(pthread_self(), SCHED_FIFO, &priority);
  ```
  (a) The binary is linked without `-z now`, and Debian's GCC does not add it, so each shared-library function is resolved by the dynamic loader on its first call. For the audio thread that means the first `sinf`, `expf` or `snd_pcm_prepare` (the xrun path) walks the libraries' symbol tables from inside `render()` or recovery. (b) `grep mlockall` finds nothing; only the app's stream pager uses `mlock` on its own chunks. The binary runs from tmpfs and cannot be evicted, but the shared libraries' code (libc, libm, libstdc++, libasound) is file-backed on eMMC and may be reclaimed under memory pressure, since without swap file pages are all the kernel can reclaim. A fault on such a page from the audio thread waits for eMMC. (c) No CPU affinity is set in the base (the app's render pool can pin; `render_pinned_default` is false). (d) Audio at FIFO 30, USB audio at 20, MIDI at 15 are all below the kernel's threaded IRQ handlers (50), so USB (audio, MIDI and network on one controller), SD/eMMC, SPI and UART interrupt threads can each preempt the audio thread.
- **Cost:** none in steady state; each is a possible dropout. Unmeasured. Check xruns while copying files over USB networking and while streaming samples from the card, and after loading enough samples to push memory low.
- **Proposal:** (a) `-Wl,-z,relro -Wl,-z,now` (in BASE-5): certain, free. (b) `mlockall(MCL_CURRENT | MCL_ONFAULT)` once the threads and libraries are up. Not `MCL_FUTURE`: it would lock the pager's sample mappings and defeat its `MADV_DONTNEED` (stream_pager.cpp:48). Plain `MCL_CURRENT` without `MCL_ONFAULT` would populate every thread's 8 MB stack. (c) and (d): the priorities are a sound conservative default (the audio thread can never starve an interrupt thread) and I would not change them blind. If xruns follow USB or card activity, raise the audio and render threads above the unrelated IRQ threads and the I2S/DMA IRQ thread above both, as rtirq setups do.
- **Gain:** latency (dropout risk) Medium for (a), Low to Medium for (b).
- **Sound or behaviour change:** bit-exact.
- **Risk:** (a) Low: a few milliseconds more at start. (b) Medium: locks resident library pages (tens of MB at most); plug-in libraries opened later are not covered.
- **Effort:** S.
- **Confidence:** High that binding is lazy and nothing is locked; Low on how often either actually bites.

### BASE-9: The USB audio threads poll every 20 ms and scan the sound cards four times a second while no computer streams
- **Where:** `base/src/drivers/usb_audio.cpp:286-318` (`UsbAudioOutput::run`), `131-156` (`UsbAudioInput::run`), `66-98` (`find_target`, `find_uac2_gadget`, `uac2_rate`)
- **Thread and frequency:** USB audio out, always (it is started unless `--usb-output off`, run_app.cpp:95); USB audio in while the app has USB-C input selected. 50 wakeups and 4 scans a second each.
- **Now:**
  ```cpp
  if (now >= next_check) {
      next_check = now + std::chrono::milliseconds(250);
      Target target = find_target(requested, "Playback Rate");
  ...
  if (!playback.pcm) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }
  ```
  `find_target()` walks every sound card with `snd_ctl_open()` (alsa-lib re-checks its configuration files and builds the device definition on each open), reads the card info, then opens the gadget's control device again to read one value.
- **Cost:** 50 wakeups a second on a FIFO 20 thread that has nothing to do, and about 4 × (5–8 control-device opens) a second; I estimate 0.5–1 ms of CPU a second. Unmeasured.
- **Proposal:** (1) small: while closed, wait until `next_check` on a condition variable that `stop()` notifies, not 20 ms at a time: 4 wakeups a second, and `stop()` still returns at once. (2) better: keep one control handle open on the gadget card, `snd_ctl_subscribe_events()`, and `poll()` its descriptor; mainline u_audio notifies the "Playback Rate"/"Capture Rate" controls when the computer starts or stops. Fall back to a 1 s re-scan when the card is gone (a gadget rebind).
- **Gain:** battery Low to Medium.
- **Sound or behaviour change:** none; recording start is detected at least as fast.
- **Risk:** Low for (1). For (2), the notification must exist in the vendor's 6.12 kernel.
- **Effort:** S for (1), M for (2).
- **Confidence:** High for the polling; Medium for the event (not checked against the vendor kernel source).

### BASE-10: While a computer records, the converter filters all 18 channels, silent stems included
- **Where:** `base/src/dsp/frame_ring.h:143-175` (`SincResampler::feed`), `base/src/drivers/usb_audio.cpp:249, 264-270` (`UsbAudioOutput::run`)
- **Thread and frequency:** USB audio out, per output frame, only while a computer records.
- **Now:**
  ```cpp
  for (unsigned j = 0; j < taps; ++j) {
      double at = std::abs(first - j * scale);
      unsigned point = unsigned(at);
      weights[j] = kernel[point].value + float(at - point) * kernel[point].slope;
      sum += weights[j];
  }
  std::array<float, Channels> mixed {};
  for (unsigned j = 0; j < taps; ++j)
      for (unsigned c = 0; c < Channels; ++c) mixed[c] += weights[j] * window[Channels * j + c];
  ```
  With equal nominal rates `taps` is 64, so each output frame costs 64 weight computations in double precision and 64 × 18 = 1,152 multiply-adds, whatever the gadget carries (`carried` at usb_audio.cpp:270 only limits the encoding) and however many stems are silent.
- **Cost:** about 51 million multiply-adds and 2.8 million weight computations a second at 44.1 kHz; I estimate 5–8 % of one core. Unmeasured.
  What is already cheap: when no computer records, `push()` returns on its first line (`if (!accepting.load(...)) return;`, usb_audio.cpp:230), so the audio thread pays one atomic load, and the output thread only polls (BASE-9). Note for the engine audit: the app still hands `render()` its stem block on every block regardless (melodee_app.cpp:137).
- **Proposal:** skip a channel whose whole window is zero. Count consecutive zero input samples per channel while copying the frame into the history (18 compares a frame); when `quiet[c] >= taps`, leave `mixed[c]` at 0. The filter would have produced exactly 0 too, so the encoded PCM is identical. Also filter only the channels the open stream carries (`min(playback.channels, channels)`), which matters with `--usb-stems off`.
- **Gain:** CPU Medium while recording (proportional to unused tracks); battery none otherwise.
- **Sound or behaviour change:** bit-exact at the encoded output.
- **Risk:** Low.
- **Effort:** M (the per-channel skip breaks the inner loop's simple shape; check it still vectorises).
- **Confidence:** Medium: counts are exact, the CPU share is an estimate.

### BASE-11: Boot: the binary is hashed three times, the stock binary once, and a six-second render self-test runs on every start
- **Where:** `base/install/sd-worker.sh:10-11, 22-25, 44`; the self-test is `src/melodee/tools.cpp:2879-2928` (`sound`)
- **Thread and frequency:** on every boot, before the stock app is stopped.
- **Now:**
  ```sh
  [ "$(sha256sum '/usr/bin/MPC Sample' | cut -d ' ' -f 1)" = "$EXPECTED" ] || { ...
  (cd "$DIR" && sha256sum -c SHA256SUMS) || { ...
  ...
  (cd "$DIR" && cp $FILES "$RELEASE/")
  (cd "$DIR" && sha256sum $FILES) | (cd "$RELEASE" && sha256sum -c -)
  chmod 755 "$RELEASE/sample-base"
  "$RELEASE/sample-base" --self-test "$RELEASE/test.wav" || { ...
  ```
  What is not slow: only the binary and two scripts are copied to RAM. The stems program and its model stay on the card and are not hashed at boot (`SHA256SUMS` covers the five top-level files only, tools/make_release.py:94). The release binary is stripped (release.sh:9).
- **Cost:** three SHA-256 passes over the Melodee binary (card copy, card copy again, RAM copy) and one over the stock binary; a second process start; an offline render of 6 s of audio (2,067 blocks) with its own engine and a 1 MB WAV written to RAM (never removed); a `systemctl daemon-reload`. I cannot put seconds on it: the binary's size is not in this checkout and `sha256sum`'s speed depends on whether the device's build uses the CPU's SHA instructions. Measure with timestamps in `/run/melodee-sd.log` or `journalctl -o short-monotonic -u sample-base-sd-boot -u sample-base`.
- **Proposal:** (1) copy first, then check the RAM copy against the card's `SHA256SUMS` (still verify the two scripts on the card): one hash of the binary for the same guarantee, not three. (2) Run the self-test once per binary: after it passes, write the binary's hash to a stamp file beside it on the card and skip the test while the stamp matches the hash just verified; sd-update.sh already self-tests a new binary before installing it (line 227). (3) `rm` the WAV after the test.
- **Gain:** load time Medium (estimate: one to a few seconds of the Melodee-specific part; the wait for the stock app to boot first is untouched).
- **Sound or behaviour change:** none.
- **Risk:** (1) Low. (2) Medium: it weakens "on any doubt the stock app keeps running" slightly (a binary that passed once is trusted on the same hash), and adds one small card write per version.
- **Effort:** S.
- **Confidence:** Medium: the redundancy is certain, the time is unmeasured.

### BASE-12: A changed frame is compared twice and copied whole
- **Where:** `base/src/drivers/display.cpp:90-94, 101` (`Display::present`), `138` (`Display::push`), `base/src/drivers/display_damage.h:56-60` (`plan`)
- **Thread and frequency:** screen and display worker, per changed frame (up to 60 a second).
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
  then the worker's `plan()` runs `memcmp` over all 240 rows again, `pending` against `shown`. `present()` waits until the worker is neither queued nor busy before it queues, so at that moment `shown` equals the previous `latest`: the worker's row pass finds exactly the rows `present()` just found.
- **Cost:** per changed frame, a second pass over 614 KB and a 307 KB copy under the lock that are not needed; about 0.2–0.5 ms, so up to 1–3 % of a core at 60 fps. An unchanged frame costs only the one compare (about 0.1–0.3 ms, 10 times a second when idle): negligible beside the draw that precedes it, and skipped entirely once the app can say nothing changed (BASE-2). Unmeasured.
- **Proposal:** `present()` records the first and last changed row, copies only those rows into `pending`, and hands the span to the worker; `plan()` takes a row range and scans only it. Full range on the first frame.
- **Gain:** CPU Low.
- **Sound or behaviour change:** none (the same rectangles are sent).
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** High on the redundancy, Medium on the size of the saving.

  The rest of the path, for question 4: the worker copies changed rectangles into a 32-bit dumb buffer (turned to portrait pixel by pixel for the base's own driver) and calls `drmModeDirtyFB` once per rectangle. In the kernel, the stock mipi-dbi helpers convert the clip to RGB565 and send three window commands plus the pixels by SPI at 25 MHz; a whole screen is 153,600 bytes, 49 ms on the wire, which matches the measured 54 ms. Nothing is sent when nothing changed; the panel refreshes from its own memory.

### BASE-13: Screen Mirror's frame timer is never disarmed, so its thread keeps waking 30 or 60 times a second after a stream ends
- **Where:** `base/src/drivers/screen_mirror.cpp:344-346` (`stream_on`), `369-380` (`stream_off`), `252-257` (`close_device`), `533-546` (`run`)
- **Thread and frequency:** Screen Mirror worker; 30 or 60 a second from the first stream until the app exits.
- **Now:**
  ```cpp
  long tick_ns = long(format.interval()) * 100;
  itimerspec every {{0, tick_ns}, {0, tick_ns}};
  timerfd_settime(timer, 0, &every, nullptr);
  ```
  This is the only `timerfd_settime` in the file. `stream_off()`, `close_device()` and `apply()` leave the periodic timer running; the loop then wakes on it, reads it and does nothing because `streaming` is false.
- **Cost:** 30 or 60 pointless wakeups a second after anyone has used Screen Mirror once, even with it switched off again.
- **Proposal:** disarm in `stream_off()` and `close_device()`: `itimerspec none {}; timerfd_settime(timer, 0, &none, nullptr);`. While at it, the 200 ms poll with the mirror off can wait on an eventfd that `set_mode()` writes.
- **Gain:** battery Low.
- **Sound or behaviour change:** none.
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** High.

### BASE-14: The MIDI thread enumerates every sound card once a second
- **Where:** `base/src/runtime/controller.cpp:129-134` (`run_midi`), `base/src/drivers/hardware_midi.cpp:25-65, 83-108` (`enumerate`, `rescan`)
- **Thread and frequency:** MIDI (FIFO 15), once a second.
- **Now:**
  ```cpp
  if (now >= scan) {
      if (midi.rescan()) app.midi_lost();
      require(!surface_required || midi.has_surface(), "Control surface disconnected");
      MidiCatalog update {midi.json(), midi.outputs(), midi.open_ports()}; catalog_updates.push(update);
      scan = now + std::chrono::seconds(1);
  }
  ```
  `rescan()` opens the control device of every card through alsa-lib, queries each raw MIDI device, and the catalogue is rebuilt as JSON and string vectors and copied to the service thread, changed or not.
- **Cost:** an estimated 0.5–2 ms once a second on the thread that reads the pads; a pad hit that arrives during the scan is handled that much later (its kernel time stamp is still right). Unmeasured.
- **Proposal:** add an inotify descriptor for `/dev/snd` (`IN_CREATE | IN_DELETE`) to the `ppoll` set and rescan only when it fires, with a slow safety rescan (every 30 s); push a new catalogue only when `generation()` moved.
- **Gain:** latency Low, battery Low.
- **Sound or behaviour change:** none.
- **Risk:** Low.
- **Effort:** M.
- **Confidence:** Medium (scan time estimated).

### BASE-15: Cross-area bug: the MIDI thread can spin at full speed while a SysEx goes out during playback
- **Where:** `src/melodee/shell/midi_io.cpp:616, 630` together with `base/src/runtime/controller.cpp:160-161` and `base/src/drivers/hardware_midi.cpp:118-119`
- **Thread and frequency:** MIDI (FIFO 15); for the length of a SysEx transfer sent while MIDI-out notes are queued.
- **Now:** due notes are held while a SysEx is active, but the wake request for them is still made:
  ```cpp
  while (!sysex.active && sent < pending_count && pending[sent].due <= now_ns + 200000) {
  ...
  if (pending_count) out.wake_at(Clock::time_point(std::chrono::nanoseconds(pending[0].due)));
  ```
  Once `pending[0].due` is in the past, the base computes a negative wait and `HardwareMidi::wait()` returns at once (`if (timeout_ns <= 0) return;`), so the loop runs again immediately, with its port reads, until the SysEx ends.
- **Cost:** one core at 100 % in a FIFO 15 thread for the whole transfer: on DIN the app paces 320 µs a byte, so a 10 KB dump is about 3 s. Audio (FIFO 30) is not blocked; normal-priority threads on that core are.
- **Proposal:** in the app, `if (pending_count && !sysex.active) out.wake_at(...)`. As a guard in the base, treat a wake time in the past as "next pass in 200 µs" when the pass before it delivered nothing.
- **Gain:** CPU Low (rare).
- **Sound or behaviour change:** none.
- **Risk:** Low.
- **Effort:** S.
- **Confidence:** Medium. Read from code only, and midi_io.cpp is outside my area: the MIDI auditor should confirm.

## Checked and fine

- `UsbAudioOutput::push` does nothing while no computer records: one atomic load, no conversion, no ring write (`usb_audio.cpp:230`).
- The display sends nothing over SPI for an unchanged frame, and its worker blocks on a condition variable (`display.cpp:98, 119`); the damage planner already limits pushes to changed rectangles (`display_damage.h`).
- MIDI input is event-driven (`ppoll` on the raw MIDI descriptors with kernel time stamps, `hardware_midi.cpp:118-130`); only the 5 ms tick is periodic (BASE-7).
- Jack detection subscribes to control events and re-reads the two jacks only on an event (`jacks.cpp:53-62`); sd-bus uses signal matches, with `GetManagedObjects` only on demand (`system_services.cpp:49-55, 89`). Their per-pass cost is the non-blocking read counted in BASE-6.
- The audio device is opened as `hw:` in S32 with no plug or conversion layer (`audio.cpp:44-48, 63`).
- Audio loop per block (`audio.cpp:117-143`): two 1 KB fills, two peak scans, 3–5 syscalls. The peak scan is scalar double precision over 256 samples, twice (`block_peak`, lines 18-22): about 1–2 µs a block by my estimate, 0.05 % of a block. An integer max-abs would be exact and vectorisable, but the gain is negligible. The non-blocking write often writes part of a block and then waits (the queue sits between 3 and 4 periods); that is one extra `ioctl` a block, also negligible.
- On xrun (`audio.cpp:97-104, 139-141`) the stream is prepared again and the next three blocks render back to back to refill the start threshold; no unbounded recovery loop.
- `MidiPacket` is 272 bytes and is copied three or four times per message (`midi.h:12-16`, `hardware_midi.cpp:153`, `controller.cpp:188`): at MIDI rates this is well under 1 MB/s.
- `midi.read()` takes a `std::function` built from a three-reference lambda on every pass (`controller.cpp:125`), which allocates; about 200 small allocations a second from the thread's own cache, no measurable cost.
- `Session` packs flags read by every thread beside counters the audio thread writes each block (`session.h:25-38`): some false sharing, a few hundred cache-line transfers a second, negligible.
- The base writes nothing to the card or eMMC periodically: its state files are in `/run`, screenshots are on request only (`run_app.cpp:149`).
- Screen Mirror makes a picture only when the screen changed (`screen_mirror.cpp:465-471`) and is off unless the app asks for it.
- `CardShare::poll` reads configfs four times a second (`card_share.cpp:96-109`): about 60 syscalls a second, small; folded into BASE-6.
- The release binary is stripped (`release.sh:9`, `tools/make_release.py:211`).
- `sd-boot.sh` returns immediately (`systemd-run --no-block`, line 12); nothing of the ~100 MB package but the binary and two scripts goes to RAM.
- Stopped and left running: the unit stops only `ac50.service` (`sd-worker.sh:34`); the app also stops `az0x-webserver.service` for port 80 (`melodee_app.cpp:48-56`) and `restore-stock.sh:117` starts it again. Every other stock service is left as it was.
- The panel driver is the stock mipi-dbi path with a different address mode; it adds no periodic work (`sample_base_panel.c`).

## Could not determine

- **All absolute numbers.** Draw time (`screen_draw_us`), per-thread CPU and wakeups, and supply current need the device. Recipe: name the threads, then sample `/proc/<pid>/task/*/status` (`voluntary_ctxt_switches`) and `/proc/<pid>/task/*/stat` over 10 s; power from a bench supply or an inline meter.
- **cpufreq on the vendor kernel:** whether `scaling_max_freq` is writable as expected, how long a voltage step takes, whether 1416 MHz carries a heavy project, and whether `CONFIG_UCLAMP_TASK` is set (it decides whether `schedutil` could ever be made usable with real-time threads).
- **Heartbeat LED:** nothing in the repository touches `/sys/class/leds`; whether the device tree defines an LED with a heartbeat trigger needs `ls /sys/class/leds; cat /sys/class/leds/*/trigger` on the device.
- **Wi-Fi and Bluetooth:** nothing in the repository touches the radios, rfkill or their services, and HARDWARE.md:60 says the base makes no radio policy changes. Whether the stock image loads the BCM43456 drivers or runs a supplicant or Bluetooth daemon after `ac50.service` stops needs `rfkill list`, `lsmod`, and `systemctl list-units --state=running` on the device (the binary's `--hardware` mode already lists `/sys/class/net` states, `platform.cpp:170-175`). Whether the web page is meant to be reachable over Wi-Fi decides if they may be blocked.
- **Backlight and LED power:** how much levels 6, 1 and 0 and the pad LEDs draw, and whether level 0 switches the LED driver off.
- **USB gadget rate notification:** whether the vendor's `u_audio.c` notifies the rate controls (BASE-9, step 2).
- **Boot timing:** the binary's size, the speed of the device's `sha256sum`, the self-test's duration, how long `ac50.service` takes to stop, and `daemon-reload`'s cost.
- **Control surface chatter:** whether the microcontroller sends anything when untouched; if `hardware_events` grows on its own, it defeats the idle screen rate the way BASE-4 describes.
- **Interrupt-thread interference:** whether USB networking traffic or card I/O causes xruns with audio at FIFO 30 under IRQ threads at 50 (BASE-8).
- **SPI clock:** the base's panel module could raise `spi->max_speed_hz` before `mipi_dbi_spi_init()` to shorten the 54 ms full-screen push; whether the panel and its cable tolerate more than 25 MHz is a hardware test. No battery effect.
- **Taking cores offline** for light projects would remove their leakage, but CPU hotplug stalls the system briefly; untested and probably not worth the dropout risk.
- **Where the journal goes:** the runtime prints only on state changes, but whether journald stores to eMMC or RAM on this image is unknown.
