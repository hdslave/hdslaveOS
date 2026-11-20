# Audit brief: Melodee performance and battery life on the MPC Sample

You are one of several auditors, each covering a different part of this repository. Read this brief first, then audit only your assigned area. The lead engineer merges every report into one document and then re-checks each finding against the code, so precision matters more than volume: a wrong finding wastes more time than a missing one.

## The device (verified from the vendor's public kernel sources)

- **SoC:** Rockchip RK3566, 4 × Cortex-A55 (ARMv8.2-A: NEON/ASIMD, FP16, dot product; no SVE). One cluster, one shared clock and voltage rail, so one cpufreq policy for all four cores.
- **CPU operating points:** 408, 600, 816, 1104 and 1416 MHz at 0.90 V; 1608 MHz at 0.975 V; 1800 MHz at 1.10 V.
- **Kernel:** Linux 6.12.60, `PREEMPT_RT`, `HZ=1000`, `NO_HZ_IDLE`. Default cpufreq governor is `performance`; `schedutil` is the only other governor built in. The device tree defines no CPU idle states beyond WFI. No swap. GPU and the DRM display subsystem are disabled.
- **Power:** battery powered. There is no battery gauge, only a supply voltage from an ADC.
- **Display:** 240 × 320 SPI panel at 25 MHz, software-rendered by JUCE into a 320 × 240 ARGB image. A full-screen push takes about 54 ms; a small damaged region takes under 5 ms. Backlight has seven levels (PWM).
- **Audio:** AK4558 codec over I2S through ALSA. Fixed 128-frame stereo S32 blocks at 44.1, 48 or 96 kHz (2.9 ms per block at 44.1 kHz).
- **Control surface:** a microcontroller on a 4 Mbaud UART, seen as raw MIDI. Pad and button LEDs are set by MIDI/SysEx messages.
- **Storage:** eMMC plus a microSD card (exFAT/FAT). The app runs from the card; samples can stream from the card.
- **Radios:** the board has a BCM43456 Wi-Fi/Bluetooth module.

## The software

- `base/` is the **sample-base** runtime (git submodule): drivers in `base/src/drivers/`, the app host in `base/src/runtime/`. It defines the `App` interface in `base/src/runtime/app.h`.
- `src/melodee/` is the **Melodee** app: `engine/` (audio DSP), `model/` (project data), `shell/` (controls, jobs, files, lights, MIDI), `pages/` and `ui/` (screen drawing), `plugin/` (VST3/LV2/CLAP/VST2 hosting), `web/` (the built-in web page).
- **Threads the runtime creates** (`base/src/runtime/run_app.cpp`, `controller.cpp`, `base/src/drivers/audio.cpp`):
  - Audio: `SCHED_FIFO` 30. Calls `App::render()` once per 128-frame block, then pushes the block to the USB audio output.
  - MIDI: `SCHED_FIFO` 15. Sleeps in `ppoll`, wakes on input, and at least every 5 ms to call `App::lights()`; also calls `App::midi_output()`.
  - Service: loops every 5 ms (`server.serve(5, …)`), polls jacks, card, power key, then calls `App::service()`.
  - Screen: calls `App::draw()` for the whole screen every 16.7 ms while the picture keeps changing. After 500 ms with no changed frame it draws every 100 ms and polls input counters every 4 ms.
  - Others: USB audio output thread, optional Screen Mirror threads, plus whatever the app starts itself (render pool, disk streaming, loaders, web server).
- **Build:** g++ 12 in a Debian bookworm arm64 container (native aarch64). Flags are `-std=c++17 -O2 -pthread -DNDEBUG`, with `-Wall -Wextra -Werror` on base and app code. There is no `-mcpu`/`-mtune`, no LTO and no `-ffast-math`. JUCE is not checked out here (`build/deps` is absent), so do not audit JUCE itself.

## Rules the project sets for itself (they still hold)

- No allocation, locks or file I/O in `render()`. Bounded audio/MIDI queues. Screen work stays off the audio thread.
- The synth and effect engines must keep their sound. For every proposal, say whether the audio output stays **bit-exact**, changes by **rounding only**, or **changes audibly**.

## What to look for

Two goals, in this order of interest:

1. **Battery life:** CPU time spent when nothing is happening (stopped transport, silent tracks, static screen), periodic wakeups and polling loops, work repeated every tick/frame/block that could be cached or skipped, unnecessary SD-card or eMMC writes, LEDs and backlight, radios or services left running, threads that spin or poll instead of blocking.
2. **Performance headroom:** cost per sample and per block on the audio thread, cost per frame on the screen thread, multi-core use, memory copies of large structures, cache behaviour, missed vectorisation, denormals, repeated transcendental maths, lock contention, load times.

Also report plain bugs you find on the way if they waste CPU or power, or risk audio dropouts.

Do not report: style, naming, generic advice with no code location, micro-optimisations in cold code (run once at load, on a button press, in tests or tools), or anything you have not read the code for.

## How to work

- Read-only. Do not modify, build or run anything in the repository. The only file you write is your report.
- Use `grep -n` to find hot paths and call sites, then read the hot code in full. Skim cold code. Say in your report what you read fully, what you skimmed and what you skipped.
- Check before you claim. If a proposal depends on who calls a function or how often, find the callers. If the code already does the optimisation somewhere (there is an idle gate for plug-ins, a render pool, a display damage planner), say so instead of proposing it again.
- Line numbers must be real: quote the lines you cite.

## Report format

Write your report to the path given in your assignment. Use this structure:

```markdown
# Audit: <area>

## Coverage
Read fully: … / Skimmed: … / Skipped: …

## Findings
(Best first. Up to about 15. Fewer strong findings beat many weak ones.)

### <PREFIX>-1: <short title>
- **Where:** `path/to/file.cpp:123` (`function_name`)
- **Thread and frequency:** audio per sample | audio per block | screen per frame | MIDI every 5 ms | service every 5 ms | on load | …
- **Now:** what the code does today, with a short quote (8 lines at most).
- **Cost:** why this matters, with a rough estimate and how you would measure it. Say plainly that an estimate is unmeasured.
- **Proposal:** the specific change. Sketch the code if it is short.
- **Gain:** battery | CPU | latency | memory | load time, rated High / Medium / Low.
- **Sound or behaviour change:** bit-exact | rounding only | audible | visible | none.
- **Risk:** Low / Medium / High, and why.
- **Effort:** S (under 30 lines) | M | L.
- **Confidence:** High / Medium / Low, and what you did not check.

## Checked and fine
Things that look expensive but are not, or are already optimised. One line each with a file reference.

## Could not determine
Open questions that need a measurement on the device or code outside your area.
```

Your final reply to the lead should be a summary under 300 words: your top five findings (ID, one line each) and the report path.
