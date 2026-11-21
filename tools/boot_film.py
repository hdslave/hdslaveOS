#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Write the boot film (ui/boot.cpp plays it) from a video: src/melodee/ui/boot_film.h and boot_film*.cpp.

    python3 tools/boot_film.py VIDEO [--part [FILE@]START:END:SPEED ...] [--fps 30] [--zoom 1.1] [--hold 800] [--quality 4]

The film is its parts one after another: seconds START to END of VIDEO (or of another FILE, a 16:9 video too),
each played SPEED times as fast. Every picture is scaled to cover the screen's 320 px width ZOOM times over
(cropped at the sides, black bars above and below), resampled to FPS frames a second, and each frame kept as a
baseline JPEG. Nothing of the sound is kept. The default cut is the hdslave intro's film alone: its liquid chrome
(0 to 4 s) at 2.5 times, the badge forming (4 s to the end) at 1.5 times, about 4.1 s. The screen holds the last
frame for HOLD ms before it fades over the app.

The repository keeps source text only, and no file over 2 MB (base/tools/check_private_files.py), so the frames
are Base64 strings spread over as many boot_film_N.cpp files as they need. ffmpeg runs with bitexact flags, so
the frames carry no encoder name, and the same ffmpeg gives the same files. The film is not covered by the AGPL
(THIRD_PARTY.md, Boot film): the files written carry its own licence line.
"""
from pathlib import Path
import argparse
import base64
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
UI = ROOT / "src/melodee/ui"
WIDTH, HEIGHT = 320, 240
PART_BYTES = 1_200_000  # Base64 a file, well under the 2 MB limit
LICENCE = ("// SPDX-License-Identifier: LicenseRef-hdslave-Boot-Film\n"
           "// Copyright (C) 2026 hdslave. All rights reserved.\n"
           "// The boot film is not covered by the AGPL; see THIRD_PARTY.md, Boot film.\n")
DEFAULT_PARTS = ["0:4:2.5", "4:7.74:1.5"]


def ffmpeg():
    found = shutil.which("ffmpeg")
    for path in ([found] if found else []) + ["/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg", "/usr/bin/ffmpeg"]:
        if path and Path(path).is_file():
            return path
    raise SystemExit("ffmpeg not found")


def part(text):
    source, _, times = text.rpartition("@")
    try:
        start, end, speed = (float(v) for v in times.split(":"))
    except ValueError:
        raise argparse.ArgumentTypeError(f"{text}: [FILE@]START:END:SPEED")
    if not 0 <= start < end or speed <= 0:
        raise argparse.ArgumentTypeError(f"{text}: START < END, SPEED > 0")
    return source, start, end, speed  # source "": the VIDEO argument


def frames(sources, parts, fps, zoom, quality, folder):
    wide = round(WIDTH * zoom / 2) * 2
    tall = round(wide * 9 / 16 / 2) * 2
    if tall > HEIGHT:
        raise SystemExit("--zoom too large: the picture would be cut at the top and bottom")
    graph, taken = [], [0] * len(sources)
    uses = [sum(1 for p in parts if p[0] == n) for n in range(len(sources))]
    for n in range(len(sources)):  # each source framed alike, then split once a part it gives
        graph.append(f"[{n}:v]scale={wide}:{tall}:flags=lanczos,crop={WIDTH}:{tall},setsar=1,"
                     f"pad={WIDTH}:{HEIGHT}:0:{(HEIGHT - tall) // 2}:black,format=yuv444p,split={uses[n]}"
                     + "".join(f"[s{n}_{k}]" for k in range(uses[n])))
    for n, (source, start, end, speed) in enumerate(parts):
        graph.append(f"[s{source}_{taken[source]}]trim={start}:{end},setpts=(PTS-STARTPTS)/{speed}[p{n}]")
        taken[source] += 1
    graph.append("".join(f"[p{n}]" for n in range(len(parts))) + f"concat=n={len(parts)}:v=1:a=0,fps={fps},format=yuvj420p[out]")
    inputs = [arg for path in sources for arg in ("-i", str(path))]
    subprocess.run([ffmpeg(), "-v", "error", "-y", *inputs, "-an", "-filter_complex", ";".join(graph),
                    "-map", "[out]", "-c:v", "mjpeg", "-q:v", str(quality), "-fflags", "+bitexact", "-flags", "+bitexact",
                    str(folder / "f%04d.jpg")], check=True)
    out = sorted(folder.glob("f*.jpg"))
    if not out:
        raise SystemExit("ffmpeg wrote no frames")
    for path in out:
        data = path.read_bytes()
        if not data.startswith(b"\xff\xd8\xff"):
            raise SystemExit(f"{path.name}: not a JPEG")
        yield data


def literal(name, data):
    # A namespace-scope const is internal to its file unless declared extern first: boot_film.cpp's table needs it.
    text = base64.b64encode(data).decode()
    lines = [f'    "{text[n:n + 120]}"' for n in range(0, len(text), 120)]
    return f"extern const char {name}[];\nconst char {name}[] =\n" + "\n".join(lines) + ";\n", len(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("video")
    parser.add_argument("--part", type=part, action="append", help="START:END:SPEED (seconds of the source)")
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--zoom", type=float, default=1.1)
    parser.add_argument("--hold", type=int, default=800, help="ms the last frame stays before the fade")
    parser.add_argument("--quality", type=int, default=4, help="ffmpeg's JPEG scale, 2 (best) to 31")
    arguments = parser.parse_args()
    if not 10 <= arguments.fps <= 60:
        raise SystemExit("--fps: 10 to 60")
    sources = [Path(arguments.video)]  # the inputs, each once; a part names its source by its place here
    parts = []
    for name, start, end, speed in arguments.part or [part(p) for p in DEFAULT_PARTS]:
        path = Path(name) if name else sources[0]
        if path not in sources:
            sources.append(path)
        parts.append((sources.index(path), start, end, speed))
    for path in sources:
        if not path.is_file():
            raise SystemExit(f"{path}: no such file")

    with tempfile.TemporaryDirectory() as temp:
        jpegs = list(frames(sources, parts, arguments.fps, arguments.zoom, arguments.quality, Path(temp)))

    for old in UI.glob("boot_film*.cpp"):
        old.unlink()
    files, body, size, names = [], "", 0, []
    for n, data in enumerate(jpegs):
        text, length = literal(f"boot_film_{n:03d}", data)
        if body and size + length > PART_BYTES:
            files.append(body)
            body, size = "", 0
        body += text
        size += length
        names.append(f"boot_film_{n:03d}")
    files.append(body)

    cut = ", ".join(f"{sources[n].name} {s:g}-{e:g} s at {v:g}x" for n, s, e, v in parts)
    what = (f"// {cut}; {arguments.fps} fps, zoom {arguments.zoom:g}, JPEG quality {arguments.quality}, hold {arguments.hold} ms.\n"
            "// Written by tools/boot_film.py; run it again rather than editing this file.\n")
    length_ms = len(jpegs) * 1000 // arguments.fps
    (UI / "boot_film.h").write_text(
        "// SPDX-License-Identifier: AGPL-3.0-only\n" + what + "#pragma once\n#include <cstdint>\n\n"
        "// The boot film (ui/boot.cpp): 320 x 240 frames as Base64 JPEG, shown one after another at `fps`; the last\n"
        "// stays `hold_ms` before the film fades over the app. The frames themselves are not covered by the AGPL\n"
        "// (boot_film*.cpp; THIRD_PARTY.md, Boot film).\n"
        "namespace melodee::ui::boot_film {\n"
        f"constexpr unsigned count = {len(jpegs)}, fps = {arguments.fps};\n"
        f"constexpr uint32_t length_ms = {length_ms}, hold_ms = {arguments.hold}; // length: the last frame's end\n"
        "extern const char* const frames[count];\n"
        "} // namespace melodee::ui::boot_film\n")
    externs = "".join(f"extern const char {name}[];\n" for name in names)
    table = "".join(f"    {name},\n" for name in names)
    (UI / "boot_film.cpp").write_text(
        LICENCE + what + '#include "melodee/ui/boot_film.h"\n\nnamespace melodee::ui::boot_film {\n\n' + externs
        + "\nconst char* const frames[count] = {\n" + table + "};\n\n} // namespace melodee::ui::boot_film\n")
    for n, body in enumerate(files, 1):
        (UI / f"boot_film_{n}.cpp").write_text(
            LICENCE + what + f"// Frames, part {n} of {len(files)}.\n\nnamespace melodee::ui::boot_film {{\n\n"
            + body + "\n} // namespace melodee::ui::boot_film\n")
    total = sum(len(d) for d in jpegs)
    print(f"boot film: {len(jpegs)} frames, {length_ms} ms + {arguments.hold} ms hold, {total} bytes of JPEG in {len(files)} part(s)")


if __name__ == "__main__":
    sys.exit(main())
