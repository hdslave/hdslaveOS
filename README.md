<h1 align="center">hdslaveOS</h1>

<p align="center"><strong>A performance-tuned 16-track groovebox OS for the Akai MPC Sample</strong><br>
hdslave's fork of <a href="https://github.com/keremimo/melodee-sample">Melodee</a> by Kerem Kiliç (Ellic Studio), currently tracking Melodee v0.8.2.</p>

<p align="center">
  <img src="docs/images/hdslave-theme.png" alt="hdslaveOS HDSLAVE purple flare theme on the Palette, Mixer, Step Edit, and Deep Freeze screens" width="760">
  <br><sub>hdslaveOS 0.8.1 screenshots.</sub>
</p>

## What hdslaveOS adds over stock Melodee

**hdslaveOS** (originally `melodee-hdslave`) cuts wasted CPU and display work on the Akai MPC Sample so more headroom goes to your synths and effects and the battery lasts longer — while playing back bit-identical audio to stock Melodee.

- **Performance & power controls**: Removes idle and redundant work across the engine and UI. Settings include **CPU speed** (Full by default), **Display refresh** (a 20, 30 or 60 fps cap for built-in display updates), **Dim after**, and **Pad brightness**. See [optimization.md](optimization.md) for the full breakdown.
- **Custom themes (`HDSLAVE` & `MONO`)**: Settings › **SCREEN** › **Palette** adds **HDSLAVE** (a static purple flare on black with dark violet contours on selected list rows, the default on new cards and legacy `NIGHT` cards) and **MONO** (a crisp black-and-white dark mode), both with sharp square corners.
- **Sidechain keying from plug-in outputs**: The master compressor's **Sidechain** can be keyed directly by a CLAP plug-in's second output (such as [Foundry](https://github.com/hdslave/hdslave-Foundry-Drum-Machine)'s **SC Key** kick-only bus).
- **Hands-on ergonomics**: On **Step Edit**, **SHIFT + encoder** steps the caret a full bar at a time; kit pads assigned any of the 16 colours light more vividly; and a custom **hdslave intro** boot film replaces the default splash.

### Mixer

In 0.8.2, Mixer opens on the channel strips. Hold **+** to open the selected channel’s card and hold **−** to return to the strips. Tap **+** or **−** to move through the selected block.

<p align="center">
  <img src="docs/images/hdslave-mixer.png" alt="Four views of the hdslaveOS mixer showing submix sends, insert effects, channel EQ, and compression" width="760">
  <br><sub>Submix sends, insert effects, channel EQ, and compression.</sub>
</p>

### Palettes & Custom Plug-in Screens

<p align="center">
  <img src="docs/images/hdslave-mono-theme.png" alt="hdslaveOS MONO black-and-white palette across 16 screens, including the Mixer" width="760">
  <br><sub><b>MONO</b> — high-contrast black-and-white dark palette across 16 pages.</sub>
</p>

All eleven plug-ins in the [hdslave MPC Sample Synths & Drum Machines](https://github.com/hdslave/hdslave-MPCSample-Synths-and-Drum-Machines) collection use Melodee’s official `melodee.screen/1` extension to render bespoke hardware-inspired interfaces right on the MPC Sample:

> **Desktop CLAP versions (macOS & Windows):** Each plug-in repository below also includes a separate desktop CLAP build in its `mac-clap/` folder (for FL Studio and other desktop CLAP hosts) with all MPC screens on one interactive panel and MIDI Learn.

<table>
  <tr>
    <td align="center" width="50%">
      <a href="https://github.com/hdslave/hdslave-Foundry-Drum-Machine"><img src="docs/images/plugins/foundry.gif" alt="Foundry drum machine interface on hdslaveOS" width="320"></a><br>
      <b><a href="https://github.com/hdslave/hdslave-Foundry-Drum-Machine">Foundry</a></b> — 16-voice 808/909/606 drum machine
    </td>
    <td align="center" width="50%">
      <a href="https://github.com/hdslave/hdslave-Cortex-Filter"><img src="docs/images/plugins/cortex-filter.gif" alt="Cortex Filter 3D morphing filter interface on hdslaveOS" width="320"></a><br>
      <b><a href="https://github.com/hdslave/hdslave-Cortex-Filter">Cortex Filter</a></b> — 3D Z-plane morphing filter cube
    </td>
  </tr>
  <tr>
    <td align="center" width="50%">
      <a href="https://github.com/hdslave/hdslave-Reverie-Synth"><img src="docs/images/plugins/reverie.gif" alt="Reverie chord and pad synth interface on hdslaveOS" width="320"></a><br>
      <b><a href="https://github.com/hdslave/hdslave-Reverie-Synth">Reverie</a></b> — chord-progression &amp; Motion pad synth
    </td>
    <td align="center" width="50%">
      <a href="https://github.com/hdslave/hdslave-Necronomicon-Synth"><img src="docs/images/plugins/necronomicon.gif" alt="Necronomicon polyrhythmic subharmonic synth interface on hdslaveOS" width="320"></a><br>
      <b><a href="https://github.com/hdslave/hdslave-Necronomicon-Synth">Necronomicon</a></b> — polyrhythmic subharmonic synth
    </td>
  </tr>
</table>

<p align="center">See all 11 animated plug-in interfaces in the <a href="https://github.com/hdslave/hdslave-MPCSample-Synths-and-Drum-Machines"><b>hdslave MPC Sample Synths &amp; Drum Machines</b></a> hub.</p>

#### Opening a plug-in screen

On an instrument track, press **B1** on Stage to move from **PRESETS** to **PANEL** and open the plug-in’s custom screen. **EDIT** shows Melodee’s parameter pages. In an FX chain, select Cortex Filter to see its compact view. Push the encoder to open the full panel. **B3** or **Shift + Push** returns to the chain, and **Shift + B3** opens the slot sheet.

On a plug-in panel, K1–K3 follow the knob takeover setting when you select another step or change which parameters a row controls.

<p align="center">
  <img src="docs/images/plugins/cortex-filter-panel.png" alt="Cortex Filter below the selected Track FX slot, showing the morph cube, filter response and three knobs" width="400">
</p>

## About Melodee

Melodee is a 16-track groovebox for the **Akai MPC Sample**. It runs on the
MPC in place of the stock app, started from the microSD card. Each pad is a
track: a drum kit, a keygroup (a sampled instrument), a plug-in instrument,
a MIDI track for external gear, or an audio track. Tracks play patterns,
patterns make sequences and a song, and every track has a mixer strip and an
effect chain that takes plug-in effects.

Melodee does not change anything on the MPC itself. It runs from files on
the card. Choose **MPC APP** on its Exit screen, or remove its `autorun`
folder from the card, and the stock app runs as before.

Back up your card before you install Melodee.

New to Melodee? Start with the [quickstart](docs/QUICKSTART.md). For every
page and control, read the [user guide](docs/GUIDE.md).

## Features

- **16 tracks**, one per pad. Hold **PAD BANK** and press a pad to select a
  track from any page.
- **VST3, LV2, CLAP and VST2 plug-in host**: load instruments and effects from
  the card. Use a plug-in instrument as a track's sound, and plug-in effects
  on track, bus, master and loop chains. Melodee runs 64-bit ARM Linux builds, Open
  MPC plug-ins, and 32-bit ARM plug-ins through a bridge. It loads each
  plug-in's presets, keeps its state in the project, plays drum machine
  plug-ins on pads, and compensates for plug-in latency.
- **Melodee synths**: three Open MPC instruments of its own, Melodee Poly
  (analog-style polysynth), Melodee PD (phase distortion) and Melodee Chip
  (chip voice), each with factory presets.
- **Kits** of 128 pads in 8 banks, with the stock Sample mode pages (Trim,
  Mix, Amp Env, Tune, Play, Filter, Filt Env) plus Layers, Follow (tempo
  follow), Cloud (granular) and a kit filter. Each pad has its own effects
  and sends to the four buses.
- **Sampling**: Chop (by hits, regions, beats, or by hand as the sample
  plays, up to 128 slices), Sample Record, Audio Recall, Resample and 16
  Levels.
  Samples load from WAV, MP3, FLAC, Ogg Vorbis and AIFF files. A sample
  longer than 10 seconds plays from the card, up to 6 hours; a project holds
  as many samples as fit in memory.
- **Stems**: separate a pad's sample into drums, bass, music and vocals on
  four new pads, on the MPC itself.
- **Sampled instruments**: SFZ, MPC keygroup and drum programs (`.xpm`,
  `.xty`), MPC expansions (`.xpn`), Logic EXS24 (`.exs`), SoundFont (`.sf2`)
  and DecentSampler (`.dspreset`), streamed from the card, on a keygroup
  track or a kit pad. Stock MPC Sample projects (`.xpj`) open too.
- **Auto sampler**: samples a plug-in track, or an external instrument on a
  MIDI track, into an MPC keygroup program on a new keygroup track.
- **Audio tracks**: record the input into a pattern, as on the MPC 3. Each
  take has its level, start and end, fades, reverse and warp.
- **MIDI tracks** for external synths and drum machines over USB-C or the
  MIDI jacks, with program changes, CC knobs and SysEx.
- **Chords**: MPC 3 chord progressions on the pads, on any track, and a
  chord builder for your own.
- **Sequencing**: 128 patterns per track in 8 banks, up to 128 bars each. Step
  sequencer, piano roll, step entry, parameter locks, ratchets and trig
  conditions. Sequences, as on the stock app, sequence chains and follow
  actions.
- **Song**: an arrangement with a lane per track, markers and a tempo lane.
  Export a WAV mixdown, stems, a MIDI file, an MPC 3 project or an Ableton
  Live Set.
- **Looper**: four loops that follow the tempo, with overdub and undo.
- **Effects**: 41 effects. A six-slot chain on every track, four send buses
  and the master. Knob FX, Pad FX, Flex Beat and the master compressor work
  as on the stock app.
- **Freeze**: print a track to audio and play it from the card, or deep
  freeze it to unload its plug-ins too. Ring prints keep delayed repeats, reverb
  tails and kit pad sends, and follow changes to plug-in tails. Long tails stream as they overlap
  across successive passes of a frozen pattern. Changing the selection keeps the previous freeze
  playable if the new selection exceeds the memory available. Deep Freeze preserves
  playback if a new print is incompatible with the handoff or relaunch tempo.
  A later tempo change automatically reloads the plug-ins while the original
  print plays, then returns the track to live playback on its next pattern pass.
- **Sync**: MIDI clock and MTC in and out, and CV clock out.
- **The Menu**: hold the encoder on any page for the page's jobs with their
  shortcuts, and the 16 places laid out as the pads. Four short **lessons**
  run on the MPC itself.
- **Basic and Full**: Basic shows the stock app's pages plus tracks,
  plug-ins, the mixer and Song. Full shows every page. Melodee starts on
  Full.
- **Web page**: connect the MPC to a computer by USB-C and open
  http://melodee.local to install updates and plug-ins, manage the card's
  files and read this guide. The MPC can also show up as a USB webcam with
  Melodee's screen.
- **Updates from the card**: copy a release zip to the card and Melodee
  installs it.

## Requirements

- Akai MPC Sample with firmware **1.3.0.12**, **1.4.0.7** or **1.4.0.8**. On any other
  firmware Melodee does not start and the stock app keeps running.
- On firmware 1.4: **Developer Mode** on, in the stock app's Settings on the
  **DEV ENHANCEMENTS (BETA)** page.
- A microSD card.

## Install

hdslaveOS has no signed releases (Melodee's updater takes only its author's),
so it is installed by hand from an unsigned package
(`hdslaveOS-MPC-Sample-<version>-unsigned.zip`, see
[Build from source](#build-from-source)). Melodee's own
[Releases](https://github.com/keremimo/melodee-sample/releases) install
stock Melodee, not this fork.

1. Back up the card. If the card already has an `autorun` folder at its top
   level, move it somewhere safe.
2. Unzip the package. It holds the `autorun` folder and an `autorun.sh` file.
   Copy both to the top level of the card (or, under a boot menu such as
   SampleBoot, copy the contents of `autorun/` into that menu's folder for it).
   Do not edit or rename anything inside them.
3. Put the card in the MPC and switch it on. The stock app starts first.
   On firmware 1.4 it asks whether to run the card's autorun script: press
   Yes. A few seconds later hdslaveOS starts.

On firmware 1.4, Screen Mirror is not available and hdslaveOS runs at
44.1 kHz only: the firmware no longer lets apps load their own kernel
drivers, and its audio driver takes 44.1 kHz alone.

The [quickstart](docs/QUICKSTART.md) has the full steps, with screenshots.

### Plug-ins

Put plug-ins in `Melodee/Plugins` on the card (the `Plugins` folder inside
the `Melodee` folder at the top level), in any subfolders. Melodee loads:

- VST3 bundles with a `Contents/aarch64-linux/` build
- LV2 bundles (`.lv2` folders) built for 64-bit ARM Linux (aarch64)
- CLAP plug-ins (`.clap` files) built for 64-bit ARM Linux (aarch64)
- [Open MPC](https://open-mpc.github.io) plug-in folders
- VST2 `.so` files built for 64-bit ARM (aarch64) or 32-bit ARM (armv7)

Plug-ins built only for Windows or macOS do not run on the MPC. The
plug-in picker shows them dimmed, with the reason.

The [11-plug-in collection](https://github.com/hdslave/hdslave-MPCSample-Synths-and-Drum-Machines/releases/tag/v0.8.2-slink.2) and the [complete installer](https://github.com/hdslave/hdslaveOS/releases/tag/v0.8.2-hdslave.3) both include **Slink 0.6.1**. To update only that instrument, merge the `Melodee` folder from the [Slink MPC download](https://github.com/hdslave/hdslave-Slink-Synth/releases/tag/v0.6.1) onto your card. Keep the supplied plug-in filename for saved projects. Select **hdslave Slink** and open **Stage → Panel** to see the silver panel below.

<a href="https://github.com/hdslave/hdslave-Slink-Synth/blob/v0.6.1/docs/screen.md#silver-panel-on-mpc"><img src="https://raw.githubusercontent.com/hdslave/hdslave-Slink-Synth/v0.6.1/docs/slink-mpc-pattern.png" width="320" alt="Slink 0.6.1 playing in Stage on its silver MPC panel"></a>

The Melodee synths are free downloads:
[Melodee Poly](https://github.com/keremimo/mpc-vst-melodee-poly/releases/latest), [Melodee PD](https://github.com/keremimo/mpc-vst-melodee-pd/releases/latest) and
[Melodee Chip](https://github.com/keremimo/mpc-vst-melodee-chip/releases/latest). From each release, download the
`-mpc-sample.zip`, unzip it and copy the plug-in folder inside (for example
`Ellic Studio - VST - Melodee Poly`) into `Melodee/Plugins`. The synths also run on
MPC OS devices that load Open MPC plug-ins; those devices use the
`-mpc-aarch64.zip` or `-mpc-armv7.zip`.

## Documentation

| Document | What it covers |
| --- | --- |
| [Quickstart](docs/QUICKSTART.md) | Install, first start, a first beat, saving |
| [User guide](docs/GUIDE.md) | Every page and control, and a chapter for users of the stock app |
| [Reference](docs/MELODEE.md) | The complete control reference, file formats and internals |

The design notes for the [Looper](docs/LOOPER.md) and
[MIDI tracks](docs/MIDI_TRACKS.md) record the choices made while each was
built. They are for developers; the user guide covers both.

## Build from source

hdslaveOS is built for linux/arm64 in a Docker image. You need Docker and
Python 3.

```sh
git clone --recurse-submodules -b hdslave https://github.com/hdslave/hdslaveOS.git
cd hdslaveOS
./build.sh
```

To make an unsigned package from a committed tree, build with the version,
strip as `release.sh` does, and package (`release.sh` itself makes Melodee's
signed releases and needs its author's key):

```sh
MELODEE_VERSION=v0.8.1-hdslave.3 ./build.sh
docker run --rm --platform linux/arm64 -v "$PWD:/work" sample-base-builder \
    sh -c 'strip -o build/melodee.release build/melodee && build/melodee.release --self-test build/release-test.wav'
python3 tools/package_unsigned.py v0.8.1-hdslave.3
```

This writes `dist/hdslaveOS-MPC-Sample-<version>-unsigned.zip`.

`./build.sh` builds `build/melodee`, then runs the tests, a sound
self-test, a monkey test and the page previews (`build/melodee-preview/`).

| Path | Contents |
| --- | --- |
| `src/melodee/` | The app: pages, shell, plug-in hosts, design system |
| `src/main.cpp` | Connects Melodee to the sample-base runtime |
| `base/` | [sample-base](https://github.com/keremimo/sample-base) (git submodule): drivers, runtime, build, install scripts |
| `core/` | [ellic-core](https://github.com/keremimo/ellic-core) (git submodule): the engine, model and shared files, with this fork's engine changes |
| `tests/` | Engine, sequencer and shell tests, layout and release checks |
| `tools/make_release.py`, `release/` | The SD card release zip |

To run a build on an MPC over SSH, use `device.py` (the base's device tool):

```sh
python3 device.py start
```

`start` copies Melodee into RAM and starts it. `status`, `ctl` and
`screenshot` talk to the running app. `install-sd` starts it from the card at
every boot.

Melodee's signed release zip (`./release.sh VERSION`,
`dist/Melodee-MPC-Sample-<version>.zip`) needs its author's key; this fork
makes the unsigned package above instead.

To move the base and the core to their latest commits (this fork's are the
`hdslave` branches of [hdslave/sample-base](https://github.com/hdslave/sample-base),
whose `main` lacks the fork's CPU-speed commits, and of
[hdslave/ellic-core](https://github.com/hdslave/ellic-core), which carries the
fork's engine changes):

```sh
git -C base pull origin hdslave
git -C core pull origin hdslave
git add base core
```

## Support

Melodee is free. If you use it, you can support it on
[Ko-fi](https://ko-fi.com/keremimo).

## Credits and licence

Melodee is made by Kerem Kiliç (Ellic Studio).

Melodee is licensed under AGPL-3.0-only ([LICENSE](LICENSE)). It uses JUCE
under the AGPLv3, and the Rubik typeface and the other screen fonts under the
SIL Open Font License 1.1 ([THIRD_PARTY.md](THIRD_PARTY.md)). hdslaveOS's boot film, the
hdslave intro, is not covered by the AGPL (all rights reserved); it replaces Melodee's own boot art.

Melodee is not affiliated with or endorsed by inMusic or Akai Professional.
MPC and MPC Sample are trademarks of inMusic Brands.
