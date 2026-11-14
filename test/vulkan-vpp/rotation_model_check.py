#!/usr/bin/env python3
"""Check the VPP rotation map against FFmpeg's software filters, without a GPU.

The scale kernels rotate the source clockwise and then sample it at swscale's
point positions. Getting the inverse rotation wrong is easy to miss: a square
frame hides a swapped axis, and the target gate only sees it as a mismatch.
This runs the same coordinate map as `unrotate()`, `mirror_at()` and the
`point_*` helpers in scale_probe.c -- keep the two in step -- against
`crop,transpose/vflip,scale=flags=neighbor` over non-square geometries, a crop
and a downscale. Mirroring is the step after the rotation, so each case is a
(rotation, mirror) pair rather than a rotation alone. A second list runs the
bilinear map against `flags=bilinear` on upscales, where swscale uses the same
four taps; a downscale is a different filter there by design, because
swscale's support widens with the ratio. It checks the formula, not the driver
or the shader.
"""
import math
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


def axis_pos(d, dst, n):
    """The centre of destination sample `d` mapped back into an `n`-sample
    axis, clamped to that axis. Bilinear's counterpart to pick()."""
    return min(max((d + 0.5) * (n / dst) - 0.5, 0.0), float(n - 1))


def mirror_pos(v, n, on):
    """VA mirrors in the rotated plane, so the reflection applies to the
    continuous position and not to an already-rounded index."""
    return (n - 1) - v if on else v


def blend(getter, nw, nh, dw, dh, dx, dy, mirror=0):
    """The kernel's four-tap blend: the two neighbours on each axis, the edge
    sample repeated when the position lands on the last one."""
    x = mirror_pos(axis_pos(dx, dw, nw), nw, mirror & 1)
    y = mirror_pos(axis_pos(dy, dh, nh), nh, mirror & 2)
    x0, y0 = int(math.floor(x)), int(math.floor(y))
    x1, y1 = min(x0 + 1, nw - 1), min(y0 + 1, nh - 1)
    fx, fy = x - x0, y - y0
    v = ((1 - fx) * (1 - fy) * getter(x0, y0) + fx * (1 - fy) * getter(x1, y0)
         + (1 - fx) * fy * getter(x0, y1) + fx * fy * getter(x1, y1))
    return int(min(max(v + 0.5, 0.0), 255.0))


def model(src, w, h, crop, dw, dh, rot, mirror=0, interp=0):
    cx, cy, cw, ch = crop
    uv = w * h
    rot_w = ch if rot & 1 else cw
    rot_h = cw if rot & 1 else ch
    out = bytearray(dw * dh + 2 * (dw // 2) * (dh // 2))
    if interp:
        def luma(rx, ry):
            sx, sy = unrotate(rot, rx, ry, cw, ch)
            return src[(cy + sy) * w + cx + sx]
        for dy in range(dh):
            for dx in range(dw):
                out[dy * dw + dx] = blend(luma, rot_w, rot_h, dw, dh, dx, dy, mirror)
        xinc, yinc = step(rot_w, dw), step(rot_h, dh)
    else:
        xinc, yinc = step(rot_w, dw), step(rot_h, dh)
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
    base = dw * dh
    if interp:
        def chroma(rx, ry):
            sx, sy = unrotate(rot, rx, ry, cw2, ch2)
            p = uv + (cy // 2 + sy) * w + cx + sx * 2
            return src[p], src[p + 1]
        for cy2 in range(dh2):
            for cx2 in range(dw2):
                o = base + (cy2 * dw2 + cx2) * 2
                for comp in (0, 1):
                    out[o + comp] = blend(lambda rx, ry, c=comp: chroma(rx, ry)[c],
                                          rot_w, rot_h, dw2, dh2, cx2, cy2, mirror)
        return bytes(out)
    xinc, yinc = step(rot_w, dw2), step(rot_h, dh2)
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


def ffmpeg_chain(work, src, w, h, crop, dw, dh, rot, mirror=0, interp=0):
    cx, cy, cw, ch = crop
    path = os.path.join(work, "in.nv12")
    with open(path, "wb") as f:
        f.write(src)
    chain = f"crop={cw}:{ch}:{cx}:{cy},{CHAINS[rot]}"
    if MIRRORS[mirror]:
        chain += f",{MIRRORS[mirror]}"
    chain += f",scale={dw}:{dh}:flags={'bilinear' if interp else 'neighbor'}"
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

# Upscales, where swscale's bilinear and this kernel's four taps are the same
# filter. They are the cases the comparison above can be exact about.
BILINEAR_CASES = [
    (64, 32, (0, 0, 64, 32), 200, 100, 0, 0),
    (64, 32, (0, 0, 64, 32), 96, 48, 0, 0),
    (64, 32, (0, 0, 64, 32), 96, 48, 0, 1),
    (32, 64, (0, 0, 32, 64), 96, 48, 1, 2),
    (64, 32, (0, 0, 64, 32), 128, 64, 0, 3),
    (62, 34, (0, 0, 62, 34), 124, 68, 0, 0),
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
    # The bilinear path, over the geometries where the two filters are
    # comparable. swscale widens the tent when it downsamples -- its support
    # grows with the ratio, while this kernel always takes the two neighbours
    # on each axis -- so a downscale is a different filter by design and not a
    # map to check here. Upscales and identities use the same four taps, and
    # there the comparison is exact up to rounding, which is enough to catch a
    # swapped axis, a wrong clamp or a mirror applied after rounding: each
    # moves samples by tens of codes.
    worst = 0
    for w, h, crop, dw, dh, rot, mirror in BILINEAR_CASES:
        src = bytes(((i * 37 + 11) ^ (i >> 5)) & 0xFF for i in range(w * h + w * h // 2))
        want = ffmpeg_chain(work, src, w, h, crop, dw, dh, rot, mirror, interp=1)
        got = model(src, w, h, crop, dw, dh, rot, mirror, interp=1)
        diff = max(abs(a - b) for a, b in zip(want, got))
        worst = max(worst, diff)
        print(f"{'ok  ' if diff <= 1 else 'FAIL'} bilinear {w}x{h} rot={rot} mirror={mirror} -> {dw}x{dh} "
              f"max_diff={diff}")
        bad += 0 if diff <= 1 else 1
    shutil.rmtree(work, ignore_errors=True)
    print(f"{'PASS' if not bad else 'FAIL'} vpp rotation and mirror map cases={len(CASES)} "
          f"bilinear_cases={len(BILINEAR_CASES)} bilinear_max_diff={worst} wrong={bad}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
