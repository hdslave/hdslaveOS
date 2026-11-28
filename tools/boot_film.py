#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Write the boot film (ui/boot.cpp plays it) from a video: src/melodee/ui/boot_film.h and boot_film*.cpp.

    python3 tools/boot_film.py VIDEO [--part [FILE@]START:END:SPEED[:ZOOM] ...] [--grade purple|none]
                                     [--outro SECONDS] [--fps 30] [--zoom 1.1] [--hold MS] [--quality 4]

The film is its parts one after another: seconds START to END of VIDEO (or of another FILE, a 16:9 video too),
each played SPEED times as fast. A PNG part holds that still for the specified interval. Every picture is scaled to cover the screen's 320 px width ZOOM times over
(cropped at the sides, black bars above and below). A part with its own ZOOM starts that many times closer
and eases back to that framing over the part, a camera pulling back (smootherstep, so it starts and lands
gently). The film is resampled to FPS frames a second; nothing of the sound is kept.

--grade purple maps every picture's brightness onto a black, dark violet and lavender ramp: a dark purple shine,
with none of the source's own hues. --outro SECONDS ends the film on a VHS tape failing over its last SECONDS:
a mistracking band sweeps through the badge, bending the two fields and exposing streaks of RF noise.
Short RF dropouts borrow the last intact line. Purple bleeds behind the bright detail, wavers as the heads
briefly regain tracking, then loses lock before the luma does. The head switch tears partway across a lower
scanline. The tracking band splits when the head slips. After one vertical roll the badge drops out,
leaving moving monochrome snow for a beat before it fades to black. All of this is baked into the JPEGs.

The default is the hdslave intro's cut: its wide shot (4 to 5.25 s) at 1.25 times with the camera easing back
from 1.8 times closer, so one short shot stands for the film's two close-ups and lands where the badge starts to
form; the formation (5.25 to 7 s) at 1.5 times; the finished badge (7 s to the end) slowed to 0.6 times under a
1 s VHS outro. 3.4 s, purple. The screen then holds the last (black) frame HOLD ms before it fades over the app.
The hdslaveOS cut begins on its own rendered camera slide. The crystal fronts develop during the approach,
and the finished badge holds before tracking fails. The exact sources and speeds are listed in boot_film.h.

Each frame is kept as a baseline JPEG. The repository keeps source text only, and no file over 2 MB
(base/tools/check_private_files.py), so the frames are Base64 strings spread over as many boot_film_N.cpp files
as they need. ffmpeg runs with bitexact flags, so the frames carry no encoder name, and the same ffmpeg gives the
same files. The film is not covered by the AGPL (THIRD_PARTY.md, Boot film): the files written carry its own
licence line.
"""
from pathlib import Path
import argparse
import base64
import math
import random
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


def tape_floor():
    """The existing slide's faint noise below the head switch, before the purple grade."""
    grain = "mod(abs(sin(floor(X/2)*12.9898+Y*78.233+N*37.719)*43758.5453),1)"
    return f"geq=lum='clip(p(X-gt(Y,H-10)*5,Y)+({grain}-0.5)*gt(Y,H-10)*140,0,255)'"


def ease(start, end, t):
    u = max(0.0, min(1.0, (t - start) / (end - start)))
    return u * u * (3 - 2 * u)


def luma_line(line, dx, gain, wear, noise, rng):
    """Sample the time-base error between pixels and pass the luma and RF noise through the deck."""
    width = len(line)
    start = math.floor(-dx)
    fraction = -dx - start
    result = bytearray(width)
    previous = filtered = grain = 0.0
    for x in range(width):
        sx = x + start
        left = line[sx] if 0 <= sx < width else 0
        right = line[sx + 1] if 0 <= sx + 1 < width else 0
        value = left + fraction * (right - left)
        filtered = value + 0.46 * wear * (filtered - value)
        sample = (filtered + 0.15 * wear * (value - previous)) * gain
        grain = 0.32 * grain + 0.68 * (rng.random() - 0.45)
        result[x] = min(255, max(0, round(sample + grain * noise)))
        previous = value
    return result


def chroma_lines(blue, red, delay, persistence, saturation, angle):
    """Both colour differences share a low-pass filter and a burst reference which can slip in phase."""
    width = len(blue)
    u, v = bytearray(width), bytearray(width)
    sine, cosine = saturation * math.sin(angle), saturation * math.cos(angle)
    trail_u = trail_v = 0.0
    for x in range(width):
        sx = x - delay
        cb = blue[sx] - 128 if 0 <= sx < width else 0
        cr = red[sx] - 128 if 0 <= sx < width else 0
        trail_u = cb + persistence * (trail_u - cb)
        trail_v = cr + persistence * (trail_v - cr)
        u[x] = max(0, min(255, round(128 + cosine * trail_u - sine * trail_v)))
        v[x] = max(0, min(255, round(128 + sine * trail_u + cosine * trail_v)))
    return u, v


def dropout(line, previous, start, length, strength, rng):
    """The dropout compensator repeats the preceding good line from the same field across the RF gap."""
    end = min(len(line), start + length)
    for x in range(max(0, start), end):
        k = (x - start) / length
        # The leading burst of noise gives way to delayed video, then blends back into the current line.
        tip = max(0, 1 - 4 * k)
        mix = strength * min(1, (1 - k) * 5)
        replacement = previous[x] * (1 - tip) + rng.uniform(65, 240) * tip
        line[x] = round(line[x] + mix * (replacement - line[x]))


def tape_frame(source, t, clock, rng, width=WIDTH, height=HEIGHT, *, field_step=1/58, field_seconds=1/60):
    """Record two successive head passes into one full-range 4:2:0 picture."""
    area = width * height
    result = bytearray(area) + bytearray([128]) * (area // 2)
    if t <= 0:
        return source
    if t >= 1:
        return result

    source = memoryview(source)
    xs, ys = width / WIDTH, height / HEIGHT
    half, quarter = width // 2, area // 4
    raster = height + 2 * round(8 * ys)  # vertical blanking travels with the recorded picture
    for field in (0, 1):
        phase = clock + field * field_seconds
        at = min(1.0, t + field * field_step)
        wear = ease(0.0, 0.31, at)
        # A brief return of tracking restores the colour before the vertical sync drops out.
        catch = ease(0.37, 0.42, at) * (1 - ease(0.45, 0.50, at))
        # On two short stretches the head wanders far enough to pick up the edge of the adjacent track.
        slip = (ease(0.23, 0.26, at) * (1 - ease(0.32, 0.39, at))
                + ease(0.46, 0.49, at) * (1 - ease(0.57, 0.65, at)))
        roll = ease(0.47, 0.73, at)
        roll_motion = 4 * roll * (1 - roll)
        top = 2 * round(roll * raster / 2)
        picture = 1 - ease(0.55, 0.73, at)
        # Snow takes over as the picture disappears. The gate stays open briefly before it closes.
        snow = ease(0.58, 0.72, at)
        mute = 1 - ease(0.875, 1.0, at)
        burst_lock = (1 - ease(0.45, 0.67, at)) * (1 - 0.30 * wear + 0.30 * catch)
        # The weaker head loses contact first. Its companion can still read the other field.
        weak_head = field * ease(0.25, 0.42, at) * (1 - 0.92 * catch)
        travel = ease(0.08, 0.80, at) - 0.10 * catch
        centre = HEIGHT * (1.17 - 1.33 * travel) + 3 * math.sin(phase * 27)
        centre += slip * (8 * math.sin(phase * 43) + 5 * field)
        fringe_offset = 18 + 5 * math.sin(phase * 31 + field)
        switch_y = HEIGHT - 3 - wear * (5.5 + 2.8 * math.sin(phase * 23) + field)
        switch_x = round(width * (0.50 + 0.34 * math.sin(phase * 19 + 1.3 * field)))
        previous_good = bytes(width)
        flutter = 0.0
        for y in range(field, height, 2):
            sy = (y - top) % raster
            if sy >= height:
                continue
            # Read faults belong to the tape scanline, so they also roll across the display.
            scan = sy / ys
            distance = scan - centre
            band = max(0.0, 1 - abs(distance) / (5 + wear * (14 + 6 * field)))
            fringe = slip * max(0.0, 1 - abs(distance + fringe_offset) / (4 + 2 * field))
            hook = math.exp(-distance / 17) if distance >= 0 else 0
            switch = scan - switch_y
            switched = switch >= 0
            head_noise = math.exp(-switch / 3.4) if switched else 0.0
            loss = min(1.0, band * (0.70 + 0.37 * field) * (1 - 0.85 * catch)
                       + 0.84 * fringe + 0.88 * head_noise)
            flutter = 0.70 * flutter + rng.uniform(-1, 1) * (0.8 + (7 + 4 * slip) * loss)
            bend = 1.6 * math.sin(scan * 0.04 + phase * 14) + flutter
            bend += 25 * math.exp(-scan / 30) * math.sin(phase * 26)
            bend += hook * (38 + 15 * field) * (1 - 0.8 * catch)
            bend += slip * hook * (22 + 10 * field) - fringe * (27 + 9 * field)
            # RF falls sharply inside a mistracked field. The noise gate closes after the picture goes.
            gain = picture * (1 - 0.72 * wear * loss) * (1 - weak_head * (0.23 + 0.56 * roll_motion))
            noise = wear * mute * (3 + 260 * loss**2 + 68 * weak_head + 146 * snow)
            line = source[sy * width:(sy + 1) * width]
            dx = xs * wear * bend
            luma = luma_line(line, dx, gain, wear, noise, rng)

            if switched:
                # A head change can occur partway across a line. Read the suffix with the next head's timing.
                begin = switch_x if switch < 2 / ys else 0
                skew = dx - xs * wear * (12 + 44 * head_noise)
                switched_line = luma_line(line, skew, gain, wear, noise + 95 * wear * mute * head_noise, rng)
                luma[begin:] = switched_line[begin:]
            if loss > 0.18 and rng.random() < loss * 0.84:
                scar = width * (0.40 + 0.31 * math.sin(phase * 17))
                start = round(scar + xs * (distance * 7.1 + rng.uniform(-9, 9)))
                dropout(luma, previous_good, start, round(xs * rng.randint(25, 99)), mute * wear, rng)
            elif rng.random() < wear * (0.009 + 0.08 * weak_head):
                dropout(luma, previous_good, rng.randrange(width), round(xs * rng.randint(8, 41)), mute * wear, rng)
            result[y * width:(y + 1) * width] = luma
            if loss < 0.38:
                previous_good = luma

            # The even head supplies the shared 4:2:0 colour row. Chroma trails the luma, briefly swings
            # in hue at the edge of a tear, and falls back to neutral when the burst reference disappears.
            if field == 0:
                src = area + sy // 2 * half
                dest = area + y // 2 * half
                delay = round(dx / 2 + xs * wear * (2.8 + 4.2 * band + 2 * head_noise))
                angle = wear * (0.20 * math.sin(phase * 29 + scan * 0.023) + 1.26 * hook) * (1 - catch)
                u, v = chroma_lines(source[src:src + half], source[src + quarter:src + quarter + half],
                                    delay, 0.80 * wear, picture * burst_lock * (1 - loss * wear), angle)
                result[dest:dest + half] = u
                result[dest + quarter:dest + quarter + half] = v
    return result


def record_outro(path, seconds, fps):
    """Process just the tail of the raw spool, timed against the frames actually emitted by the cut."""
    frame_bytes = WIDTH * HEIGHT * 3 // 2
    count, remainder = divmod(path.stat().st_size, frame_bytes)
    if not count or remainder:
        raise SystemExit("ffmpeg wrote an incomplete film")
    affected = max(1, round(min(seconds, count / fps) * fps))
    # The first sample of the ramp is the clean picture. Even a one-frame outro ends in black.
    first = 1 if affected > 1 else 0
    with path.open("r+b") as spool:
        for i in range(first, affected):
            offset = (count - affected + i) * frame_bytes
            spool.seek(offset)
            frame = spool.read(frame_bytes)
            t = i / (affected - 1) if affected > 1 else 1.0
            recorded = tape_frame(frame, t, i / fps, random.Random(0x564853 + i),
                                  field_step=0.5 / max(1, affected - 1), field_seconds=0.5 / fps)
            spool.seek(offset)
            spool.write(recorded)


def frames(sources, parts, fps, zoom, grade, outro, quality, folder):
    wide = round(WIDTH * zoom / 2) * 2
    tall = round(wide * 9 / 16 / 2) * 2
    if tall > HEIGHT:
        raise SystemExit("--zoom too large: the picture would be cut at the top and bottom")
    graph, taken = [], [0] * len(sources)
    uses = [sum(1 for p in parts if p[0] == n) for n in range(len(sources))]
    for n, path in enumerate(sources):
        hold = "loop=loop=-1:size=1:start=0,settb=1/60,setpts=N," if path.suffix.lower() == ".png" else ""
        graph.append(f"[{n}:v]{hold}split={uses[n]}" + "".join(f"[s{n}_{k}]" for k in range(uses[n])))
    for n, (source, start, end, speed, closer) in enumerate(parts):
        seconds = (end - start) / speed
        graph.append(f"[s{source}_{taken[source]}]trim={start}:{end},setpts=(PTS-STARTPTS)/{speed},fps=60,"
                     f"{framing(wide, tall, closer, seconds)},format=yuv444p[p{n}]")
        taken[source] += 1
    chain = [f"concat=n={len(parts)}:v=1:a=0", f"fps={fps}"]
    if grade != "none" or outro:
        chain.append("format=gray")
    if outro:
        chain.append(tape_floor())
    if GRADES[grade]:
        chain.append(GRADES[grade])
    chain.append("format=yuvj420p")
    graph.append("".join(f"[p{n}]" for n in range(len(parts))) + ",".join(chain) + "[out]")
    inputs = [arg for path in sources for arg in ("-i", str(path))]
    command = [ffmpeg(), "-v", "error", "-y"]
    video = [*inputs, "-an", "-filter_complex", ";".join(graph), "-map", "[out]"]
    if outro:
        spool = folder / "film.yuv"
        subprocess.run(command + video + ["-f", "rawvideo", str(spool)], check=True)
        record_outro(spool, outro, fps)
        video = ["-f", "rawvideo", "-pixel_format", "yuvj420p", "-video_size", f"{WIDTH}x{HEIGHT}",
                 "-framerate", str(fps), "-i", str(spool), "-vf", "setsar=1"]
    subprocess.run(command + video + ["-c:v", "mjpeg", "-q:v", str(quality), "-fflags", "+bitexact",
                                      "-flags", "+bitexact", str(folder / "f%04d.jpg")], check=True)
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
    if not math.isfinite(arguments.outro) or arguments.outro < 0:
        raise SystemExit("--outro: a finite, non-negative duration")
    if arguments.outro and arguments.grade == "none":
        raise SystemExit("--outro needs a --grade")
    sources = []  # the inputs the parts use, each once (VIDEO only if a part does); a part names its source by its place
    parts = []
    for name, start, end, speed, closer in arguments.part or [part(p) for p in DEFAULT_PARTS]:
        path = Path(name) if name else Path(arguments.video)
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
