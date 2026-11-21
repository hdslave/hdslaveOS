#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Write the boot film (ui/boot.cpp plays it) from a video: src/melodee/ui/boot_film.h and boot_film*.cpp.

    python3 tools/boot_film.py VIDEO [--part [FILE@]START:END:SPEED[:ZOOM] ...] [--grade purple|none]
                                     [--outro SECONDS] [--fps 30] [--zoom 1.1] [--hold MS] [--quality 4]

The film is its parts one after another: seconds START to END of VIDEO (or of another FILE, a 16:9 video too),
each played SPEED times as fast. Every picture is scaled to cover the screen's 320 px width ZOOM times over
(cropped at the sides, black bars above and below). A part with its own ZOOM starts that many times closer
and eases back to that framing over the part, a camera pulling back (smootherstep, so it starts and lands
gently). The film is resampled to FPS frames a second; nothing of the sound is kept.

--grade purple maps every picture's brightness onto a black, dark violet and lavender ramp: a dark purple shine,
with none of the source's own hues. --outro SECONDS ends the film on a VHS tape's glitch over its last SECONDS:
bands of lines jump sideways and a few tear further, a ghost trails the picture, a noisy tracking bar rolls
down, scanlines and noise grow, while the picture fades through deep purple to black. It works on the
brightness before the grade, so every artefact stays purple.

The default is the hdslave intro's cut: its wide shot (4 to 5.25 s) at 1.25 times with the camera easing back
from 1.8 times closer, so one short shot stands for the film's two close-ups and lands where the badge starts to
form; the formation (5.25 to 7 s) at 1.5 times; the finished badge (7 s to the end) slowed to 0.6 times under a
1 s VHS outro. 3.4 s, purple. The screen then holds the last (black) frame HOLD ms before it fades over the app.

Each frame is kept as a baseline JPEG. The repository keeps source text only, and no file over 2 MB
(base/tools/check_private_files.py), so the frames are Base64 strings spread over as many boot_film_N.cpp files
as they need. ffmpeg runs with bitexact flags, so the frames carry no encoder name, and the same ffmpeg gives the
same files. The film is not covered by the AGPL (THIRD_PARTY.md, Boot film): the files written carry its own
licence line.
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
DEFAULT_PARTS = ["4:5.25:1.25:1.8", "5.25:7:1.5", "7:7.74:0.6"]
# Dark purple shine: brightness to black, dark violet, purple and lavender white (ffmpeg curves, per channel).
GRADES = {
    "purple": "format=gbrp,curves=r='0/0 0.35/0.10 0.6/0.30 0.85/0.62 1/0.92'"
              ":g='0/0 0.35/0.02 0.6/0.10 0.85/0.42 1/0.86':b='0/0 0.35/0.22 0.6/0.55 0.85/0.88 1/1'",
    "none": "",
}


def ffmpeg():
    found = shutil.which("ffmpeg")
    for path in ([found] if found else []) + ["/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg", "/usr/bin/ffmpeg"]:
        if path and Path(path).is_file():
            return path
    raise SystemExit("ffmpeg not found")


def part(text):
    source, _, times = text.rpartition("@")
    try:
        values = [float(v) for v in times.split(":")]
        start, end, speed = values[:3]
        zoom = values[3] if len(values) == 4 else 1.0
        if len(values) not in (3, 4):
            raise ValueError
    except ValueError:
        raise argparse.ArgumentTypeError(f"{text}: [FILE@]START:END:SPEED[:ZOOM]")
    if not 0 <= start < end or speed <= 0 or zoom < 1:
        raise argparse.ArgumentTypeError(f"{text}: START < END, SPEED > 0, ZOOM >= 1")
    return source, start, end, speed, zoom  # source "": the VIDEO argument


def smoother(u):
    """smootherstep of an ffmpeg expression already clipped to 0..1."""
    return f"(({u})*({u})*({u})*(({u})*(({u})*6-15)+10))"


def framing(wide, tall, zoom, length):
    """A part's pictures onto the screen; with zoom > 1 the camera eases back from zoom times closer."""
    pad = f"setsar=1,pad={WIDTH}:{HEIGHT}:0:{(HEIGHT - tall) // 2}:black"
    if zoom == 1:
        return f"scale={wide}:{tall}:flags=lanczos,crop={WIDTH}:{tall},{pad}"
    # Worked at twice the size, so the centred crop moves in half pixels of the screen.
    z = f"(1+{zoom - 1:g}*(1-{smoother(f'clip(t/{length:g},0,1)')}))"
    return (f"scale=w='trunc({2 * wide}*{z}/2)*2':h='trunc({2 * tall}*{z}/2)*2':eval=frame:flags=lanczos,"
            f"crop={2 * WIDTH}:{2 * tall},scale={WIDTH}:{tall}:flags=lanczos,{pad}")


def vhs(seconds, start):
    """The VHS outro on a grey picture: the film's last `seconds`, from `start` on its clock."""
    expression = (
        f"st(0,clip((T-{start:g})/{seconds:g},0,1));"                           # 0..1 over the outro
        "st(1,clip(ld(0)/0.55,0,1));st(1,ld(1)*ld(1)*(3-2*ld(1)));"            # the glitch builds
        "st(2,clip((ld(0)-0.4)/0.6,0,1));st(2,1-ld(2)*ld(2)*(3-2*ld(2)));"     # the picture fades
        "st(3,mod(abs(sin(floor(Y/4)*12.9898+floor(N/2)*78.233))*43758.5453,1));"  # a 4-line band's dice
        "st(5,lt(abs(Y-(mod(T*230,H+60)-30)),7));"                             # the rolling tracking bar
        "st(4,ld(1)*((ld(3)-0.5)*5+gt(ld(3),0.9)*(ld(3)-0.9)*360+2*sin(Y*0.06+T*31)+ld(5)*12));"  # sideways
        "st(6,(mod(abs(sin(X*12.9898+Y*78.233+N*37.719))*43758.5453,1)-0.5)*ld(1)*(40+ld(5)*140));"  # noise
        "st(7,1-0.3*ld(1)*mod(Y,2));"                                          # scanlines
        "st(8,p(X-ld(4),Y)*(1-0.3*ld(1))+p(X-ld(4)-2-7*ld(1),Y)*0.45*ld(1));"  # the picture and its ghost
        "clip((ld(8)*ld(7)+ld(6))*ld(2),0,255)")
    return f"geq=lum='{expression}'"


def frames(sources, parts, fps, zoom, grade, outro, quality, folder):
    wide = round(WIDTH * zoom / 2) * 2
    tall = round(wide * 9 / 16 / 2) * 2
    if tall > HEIGHT:
        raise SystemExit("--zoom too large: the picture would be cut at the top and bottom")
    graph, taken = [], [0] * len(sources)
    uses = [sum(1 for p in parts if p[0] == n) for n in range(len(sources))]
    for n in range(len(sources)):
        graph.append(f"[{n}:v]split={uses[n]}" + "".join(f"[s{n}_{k}]" for k in range(uses[n])))
    length = 0.0
    for n, (source, start, end, speed, closer) in enumerate(parts):
        seconds = (end - start) / speed
        graph.append(f"[s{source}_{taken[source]}]trim={start}:{end},setpts=(PTS-STARTPTS)/{speed},fps=60,"
                     f"{framing(wide, tall, closer, seconds)},format=yuv444p[p{n}]")
        taken[source] += 1
        length += seconds
    chain = [f"concat=n={len(parts)}:v=1:a=0", f"fps={fps}"]
    if grade != "none" or outro:
        chain.append("format=gray")
    if outro:
        chain.append(vhs(outro, max(0.0, length - outro)))
    if GRADES[grade]:
        chain.append(GRADES[grade])
    chain.append("format=yuvj420p")
    graph.append("".join(f"[p{n}]" for n in range(len(parts))) + ",".join(chain) + "[out]")
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
    parser.add_argument("--part", type=part, action="append", help="[FILE@]START:END:SPEED[:ZOOM] (seconds of the source)")
    parser.add_argument("--grade", choices=sorted(GRADES), default="purple")
    parser.add_argument("--outro", type=float, default=1.0, help="seconds of VHS glitch fading to black at the end; 0 none")
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--zoom", type=float, default=1.1)
    parser.add_argument("--hold", type=int, default=0, help="ms the last frame stays before the fade over the app")
    parser.add_argument("--quality", type=int, default=4, help="ffmpeg's JPEG scale, 2 (best) to 31")
    arguments = parser.parse_args()
    if not 10 <= arguments.fps <= 60:
        raise SystemExit("--fps: 10 to 60")
    if arguments.outro and arguments.grade == "none":
        raise SystemExit("--outro works on the picture's brightness: it needs a --grade")
    sources = [Path(arguments.video)]  # the inputs, each once; a part names its source by its place here
    parts = []
    for name, start, end, speed, closer in arguments.part or [part(p) for p in DEFAULT_PARTS]:
        path = Path(name) if name else sources[0]
        if path not in sources:
            sources.append(path)
        parts.append((sources.index(path), start, end, speed, closer))
    for path in sources:
        if not path.is_file():
            raise SystemExit(f"{path}: no such file")

    with tempfile.TemporaryDirectory() as temp:
        jpegs = list(frames(sources, parts, arguments.fps, arguments.zoom, arguments.grade, arguments.outro,
                            arguments.quality, Path(temp)))

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

    cut = ", ".join(f"{sources[n].name} {s:g}-{e:g} s at {v:g}x" + (f" from {z:g}x closer" if z != 1 else "")
                    for n, s, e, v, z in parts)
    what = (f"// {cut}; grade {arguments.grade}, VHS outro {arguments.outro:g} s; {arguments.fps} fps, "
            f"zoom {arguments.zoom:g}, JPEG quality {arguments.quality}, hold {arguments.hold} ms.\n"
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
