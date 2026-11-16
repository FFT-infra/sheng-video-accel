#!/usr/bin/env python3
"""Every VADriverVTable slot is either assigned or one libva null-checks.

libva dispatches a slot with or without a null check depending on the
function, and the difference is not visible from the driver. An empty slot
that libva calls unguarded dereferences NULL inside the client: the process
dies with no driver-side trace. vaQuerySurfaceError was that slot.

This reads the installed va_backend.h, takes every member of VADriverVTable,
and requires each one to be either assigned in src/va/driver.cc or listed
below as guarded. A libva upgrade that adds a slot fails here, which is the
point: the driver has to decide about the new slot before it ships.

The guarded list is read off va/va.c, not guessed. Each entry names the
function that checks the pointer.
"""
import pathlib
import re
import sys

# Slots libva 2.23 calls behind `if (!ctx->vtable->X)`. Keep the libva
# function name beside each so a reader can re-check it against the source.
GUARDED = {
    "vaCopy": "vaCopy",
    "vaCreateBuffer2": "vaCreateBuffer2",
    "vaCreateMFContext": "vaCreateMFContext",
    "vaGetSurfaceAttributes": "vaGetSurfaceAttributes",
    "vaMFAddContext": "vaMFAddContext",
    "vaMFReleaseContext": "vaMFReleaseContext",
    "vaMFSubmit": "vaMFSubmit",
    "vaMapBuffer2": "vaMapBuffer2",
    "vaQueryProcessingRate": "vaQueryProcessingRate",
    "vaSyncSurface2": "vaSyncSurface2",
}


def members(header: pathlib.Path):
    text = header.read_text(errors="replace")
    start = text.index("struct VADriverVTable {")
    # The struct ends at the first line that is exactly "};" after start.
    end = text.index("\n};", start)
    body = text[start:end]
    # Members are declared as `VAStatus(*vaName)(` or `VAStatus (*vaName) (`.
    return sorted(set(re.findall(r"\(\s*\*\s*(va[A-Za-z0-9_]+)\s*\)", body)))


def assigned(driver: pathlib.Path):
    text = driver.read_text(errors="replace")
    return set(re.findall(r"vtable->(va[A-Za-z0-9_]+)\s*=", text))


def main() -> int:
    root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    header = pathlib.Path(sys.argv[2] if len(sys.argv) > 2 else "/usr/include/va/va_backend.h")
    if not header.is_file():
        print(f"FAIL va_backend.h not found: {header}", file=sys.stderr)
        return 2
    driver = root / "src/va/driver.cc"
    if not driver.is_file():
        print(f"FAIL driver source not found: {driver}", file=sys.stderr)
        return 2

    slots = members(header)
    if len(slots) < 50:
        print(f"FAIL parsed only {len(slots)} VADriverVTable members; the parser broke", file=sys.stderr)
        return 2
    filled = assigned(driver)

    unknown = sorted(s for s in slots if s not in filled and s not in GUARDED)
    if unknown:
        print("FAIL these vtable slots are neither assigned nor known to be null-checked by libva:",
            file=sys.stderr)
        for name in unknown:
            print("  " + name, file=sys.stderr)
        print("Either assign it in src/va/driver.cc or add it to GUARDED here with the libva"
              " function that checks it.", file=sys.stderr)
        return 1

    stale = sorted(s for s in filled if s not in slots)
    if stale:
        print("FAIL src/va/driver.cc assigns names that are not VADriverVTable members:",
            file=sys.stderr)
        for name in stale:
            print("  " + name, file=sys.stderr)
        return 1

    # A guarded slot that is also assigned is harmless but means the list is
    # out of date; report it rather than silently accepting it.
    both = sorted(s for s in filled if s in GUARDED)
    if both:
        print("FAIL these slots are assigned and also listed as guarded; drop them from GUARDED:",
            file=sys.stderr)
        for name in both:
            print("  " + name, file=sys.stderr)
        return 1

    print(f"PASS vtable surface: {len(slots)} members, {len(filled)} assigned,"
          f" {len(GUARDED)} guarded by libva")
    return 0


if __name__ == "__main__":
    sys.exit(main())
