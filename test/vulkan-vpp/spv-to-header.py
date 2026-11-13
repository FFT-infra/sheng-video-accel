#!/usr/bin/env python3
"""Emit a checked-in SPIR-V module as a C uint32_t array.

The Vulkan VPP backend embeds the shader in the driver .so: libva loads the
driver into arbitrary client processes, so there is no reliable install path
to read a .spv from at runtime. The pinned test/vulkan-vpp/*.spv is the single
source of truth; run-static-checks.sh regenerates this header and compares it,
so a hand-edited binary or array cannot drift.

Usage: spv-to-header.py <module.spv> [output.h]
With no output path the array goes to stdout.
"""
import pathlib
import sys


def main() -> None:
    if len(sys.argv) < 2:
        print("usage: spv-to-header.py <module.spv> [output.h]", file=sys.stderr)
        sys.exit(2)
    data = pathlib.Path(sys.argv[1]).read_bytes()
    if len(data) % 4 or not data:
        print(f"FAIL {sys.argv[1]} is not a non-empty SPIR-V word stream", file=sys.stderr)
        sys.exit(1)
    words = [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]
    out = []
    out.append("// SPDX-License-Identifier: MIT\n")
    out.append("// Generated from test/vulkan-vpp/nv12_scale.spv by spv-to-header.py.\n")
    out.append("// Do not edit by hand; run-static-checks.sh verifies this against the pinned module.\n")
    out.append("#pragma once\n")
    out.append("#include <cstdint>\n")
    out.append("namespace iris {\n")
    out.append("inline constexpr uint32_t kNv12ScaleSpv[] = {\n")
    for i in range(0, len(words), 8):
        out.append("    " + ", ".join(f"0x{w:08x}u" for w in words[i:i + 8]) + ",\n")
    out.append("};\n")
    out.append(f"inline constexpr uint32_t kNv12ScaleSpvWords = {len(words)};\n")
    out.append("} // namespace iris\n")
    text = "".join(out)
    if len(sys.argv) >= 3:
        pathlib.Path(sys.argv[2]).write_text(text)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
