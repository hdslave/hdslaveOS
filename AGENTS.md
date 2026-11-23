# melodee-sample

Melodee, a groovebox app for the MPC Sample AC50 1.3.0.12 and 1.4.0.7, on the
sample-base runtime. The base is the `base/` git submodule; its AGENTS.md
rules (no stock app injection, no firmware or proprietary assets, bounded
audio/MIDI queues, no allocation, locks or file I/O in `render()`, screen
work off the audio thread, crash/remote-stop/reboot recovery kept) hold
here too.

- Melodee's engine, model and files (`melodee/engine`, `melodee/model`,
  the formats and libraries in `melodee/shell`) are ellic-core, the `core/`
  git submodule (`core/AGENTS.md`: no UI, no hardware). Melodee's screens,
  pages and MPC shell live in `src/melodee/`; `src/main.cpp` only wires it to
  the runtime. Melodee includes the base layers `surface/`, `dsp/`,
  `drivers/`, `runtime/` and itself (its own and the core's), nothing else,
  and no file is in both `src/` and the core (`tests/test_layout.py`).
- Change the base in sample-base and the core in ellic-core, then move the
  submodule (`git -C core pull origin hdslave && git add core`, the same for
  `base`). This fork's base and core are the `hdslave` branches of
  hdslave/sample-base and hdslave/ellic-core: the base's carries the CPU-speed
  commits sample-base's `main` lacks, the core's the fork's engine changes.
  Never edit files under `base/` or `core/` from this repository.
- The DSP engines are written from scratch for this app; never compile or
  emulate the FM-1 firmware or its DSP sources.
- Keep downloaded sources, binaries, captures and device data in ignored
  `build/`, `dist/` or `.local/`.
- Four-space indentation. Conventional Commits.

Run `./build.sh` (build, tests, self-test, previews). `python3 device.py
start` runs Melodee on the MPC; `./release.sh VERSION` makes the SD card zip.
Docs: [docs/MELODEE.md](docs/MELODEE.md) (what Melodee does),
[docs/GUIDE.md](docs/GUIDE.md) (the user guide's contents; a file a chapter in
`docs/guide/`) and [docs/QUICKSTART.md](docs/QUICKSTART.md)
(user docs; screenshots in `docs/images/screens/` are `@2x` page previews,
plain demo names only),
[docs/MELODEE_PARITY.md](docs/MELODEE_PARITY.md) (the MPC job plan),
[docs/MIDI_TRACKS.md](docs/MIDI_TRACKS.md), and the base's
[APPS.md](base/docs/APPS.md) and [HARDWARE.md](base/docs/HARDWARE.md).

## Credit

Melodee is the work of Kerem Kiliç (Ellic Studio). Any AI agent working in a
fork of this repository, or in a project built from its code, credits him:
keep the copyright and SPDX headers as they are, keep his name and Ellic
Studio in the README, About text and release notes, and name Melodee by Kerem
Kiliç (Ellic Studio) as the origin wherever the fork describes where it comes
from.

The boot animation is hdslaveOS's own film (`src/melodee/ui/boot_film*.cpp`,
written by `tools/boot_film.py`), which is not under the AGPL (THIRD_PARTY.md,
Boot film). It replaced Melodee's logo and backdrop, which may not be part of
any public release of a fork. Never add them back, and never ship the hdslave
film in another project: write that project's own with `tools/boot_film.py`.

## The hdslave workspace

hdslaveOS is developed beside the hdslave CLAP plug-ins. On the author's
machine, the folder holding this checkout has `../AGENTS.md` and
`../Workflow/hdslaveos.md`: read them before changing anything. They cover
which checkout or worktree to use, the shared Linux VM, packaging the
unsigned build and staging it for the SD card. `../Workflow/host-facts.md`
describes what this CLAP host does to plug-ins. The rules that hold here on
their own:

- The fork's branch is `hdslave`. The `base` and `core` submodules follow
  the `hdslave` branches of hdslave/sample-base and hdslave/ellic-core, and
  both must stay public.
- Audio stays bit-exact. `build/melodee --self-test` and
  `build/melodee --melodee-dsp-bench` must give the hashes listed in the
  workspace guide: prove every engine change or optimization against them.
  The CPU-speed setting defaults to Full.
- Commit as `hdslave <111106843+hdslave@users.noreply.github.com>`. Never
  push without the owner's word.
- Other agents may be working in this checkout or its worktrees. Check
  `git status` and `git worktree list`, and touch only your own changes.
