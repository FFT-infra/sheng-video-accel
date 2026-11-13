#!/usr/bin/env python3
"""Check the VPP rotation map against FFmpeg's software filters, without a GPU.

The scale kernels rotate the source clockwise and then sample it at swscale's
point positions. Getting the inverse rotation wrong is easy to miss: a square
frame hides a swapped axis, and the target gate only sees it as a mismatch.
This runs the same coordinate map as `unrotate()`, `mirror_at()` and the
`point_*` helpers in scale_probe.c -- keep the two in step -- against
`crop,transpose/vflip,scale=flags=neighbor` over non-square geometries, a crop
and a downscale. Mirroring is the step after the rotation, so each case is a
(rotation, mirror) pair rather than a rotation alone. It checks the formula,
not the driver or the shader.
"""
import os
import shutil
import subprocess
import sys
import tempfile

# The software equivalent of each clockwise quarter turn. ffmpeg 8.x dropped
# the named half-turn from the software filter, so it is two flips here.
CHAINS = {0: "null", 1: "transpose=clock", 2: "hflip,vflip", 3: "transpose=cclock"}

# The software equivalent of each mirror, applied after the rotation. VA
# numbers them as bits: 1 horizontal, 2 vertical, 3 both.
MIRRORS = {0: "", 1: "hflip", 2: "vflip", 3: "hflip,vflip"}


def step(src, dst):
    return ((src << 16) + (dst >> 1)) // dst


def pick(i, inc):
    return (i * inc + (inc >> 1)) >> 16


def unrotate(rot, rx, ry, w, h):
    if rot == 1:
        return ry, h - 1 - rx
    if rot == 2:
        return w - 1 - rx, h - 1 - ry
    if rot == 3:
        return w - 1 - ry, rx
    return rx, ry


def mirror_at(mirror, rx, ry, w, h):
    if mirror & 1:
        rx = w - 1 - rx
    if mirror & 2:
        ry = h - 1 - ry
    return rx, ry


def model(src, w, h, crop, dw, dh, rot, mirror=0):
    cx, cy, cw, ch = crop
    uv = w * h
    rot_w = ch if rot & 1 else cw
    rot_h = cw if rot & 1 else ch
    xinc, yinc = step(rot_w, dw), step(rot_h, dh)
    out = bytearray(dw * dh + 2 * (dw // 2) * (dh // 2))
    for dy in range(dh):
        ry = pick(dy, yinc)
        for dx in range(dw):
            rx, my = mirror_at(mirror, pick(dx, xinc), ry, rot_w, rot_h)
            sx, sy = unrotate(rot, rx, my, cw, ch)
            out[dy * dw + dx] = src[(cy + sy) * w + cx + sx]
    cw2, ch2 = cw // 2, ch // 2
    dw2, dh2 = dw // 2, dh // 2
    rot_w = ch2 if rot & 1 else cw2
    rot_h = cw2 if rot & 1 else ch2
    xinc, yinc = step(rot_w, dw2), step(rot_h, dh2)
    base = dw * dh
    for cy2 in range(dh2):
        ry = pick(cy2, yinc)
        for cx2 in range(dw2):
            rx, my = mirror_at(mirror, pick(cx2, xinc), ry, rot_w, rot_h)
            sx, sy = unrotate(rot, rx, my, cw2, ch2)
            o = base + (cy2 * dw2 + cx2) * 2
            p = uv + (cy // 2 + sy) * w + cx + sx * 2
            out[o] = src[p]
            out[o + 1] = src[p + 1]
    return bytes(out)


def ffmpeg_chain(work, src, w, h, crop, dw, dh, rot, mirror=0):
    cx, cy, cw, ch = crop
    path = os.path.join(work, "in.nv12")
    with open(path, "wb") as f:
        f.write(src)
    chain = f"crop={cw}:{ch}:{cx}:{cy},{CHAINS[rot]}"
    if MIRRORS[mirror]:
        chain += f",{MIRRORS[mirror]}"
    chain += f",scale={dw}:{dh}:flags=neighbor"
    return subprocess.run(
        ["ffmpeg", "-v", "error", "-f", "rawvideo", "-pix_fmt", "nv12", "-s", f"{w}x{h}", "-i", path,
         "-vf", chain, "-frames:v", "1", "-f", "rawvideo", "-"],
        check=True, capture_output=True).stdout


# w, h, crop, dw, dh, rotation. Non-square sources are the point: with a square
# one the two axes could be swapped and every case would still pass.
CASES = [
    (64, 32, (0, 0, 64, 32), 32, 64, 1, 0),
    (64, 32, (0, 0, 64, 32), 32, 64, 3, 0),
    (64, 32, (0, 0, 64, 32), 64, 32, 2, 0),
    (128, 72, (0, 0, 128, 72), 96, 48, 1, 0),
    (128, 72, (16, 16, 96, 40), 96, 48, 1, 0),
    # The mirrors, alone and on top of a rotation: hflip, vflip, clock_flip,
    # cclock_flip, and the two combinations a client can still name.
    (64, 32, (0, 0, 64, 32), 64, 32, 0, 1),
    (64, 32, (0, 0, 64, 32), 64, 32, 0, 2),
    (128, 72, (0, 0, 128, 72), 72, 128, 1, 2),
    (128, 72, (0, 0, 128, 72), 72, 128, 3, 2),
    (64, 32, (0, 0, 64, 32), 64, 32, 0, 3),
    (128, 72, (16, 16, 96, 40), 48, 96, 1, 1),
]


def scratch():
    """Lab scratch before the system default: this tree keeps media off /tmp."""
    for candidate in (os.environ.get("IRIS_TEST_SCRATCH_ROOT"), os.environ.get("TMPDIR"),
                      os.path.expanduser("~/Lab/Bridge/tmp")):
        if candidate and os.path.isdir(candidate):
            return candidate
    return None


def main():
    work = tempfile.mkdtemp(prefix="vpp-rotation.", dir=scratch())
    bad = 0
    for w, h, crop, dw, dh, rot, mirror in CASES:
        src = bytes(((i * 37 + 11) ^ (i >> 5)) & 0xFF for i in range(w * h + w * h // 2))
        want = ffmpeg_chain(work, src, w, h, crop, dw, dh, rot, mirror)
        got = model(src, w, h, crop, dw, dh, rot, mirror)
        ok = want == got
        print(f"{'ok  ' if ok else 'FAIL'} {w}x{h} crop={crop[2]}x{crop[3]}+{crop[0]}+{crop[1]} "
              f"rot={rot} mirror={mirror} -> {dw}x{dh}")
        bad += 0 if ok else 1
    shutil.rmtree(work, ignore_errors=True)
    print(f"{'PASS' if not bad else 'FAIL'} vpp rotation and mirror map cases={len(CASES)} wrong={bad}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
