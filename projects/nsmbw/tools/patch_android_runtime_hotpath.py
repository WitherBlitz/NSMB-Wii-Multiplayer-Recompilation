#!/usr/bin/env python3
"""Exact-build experimental ARM64 patch for Aurora draw stats and worker count.

Aurora increments a 64-bit atomic counter in its two hot draw entry points
even when NSMBW_LOG_FPS is disabled. The counter is only read for diagnostics.
Cap mobile pipeline compiler workers at three (from six on 8-core Android)
to reduce shader prewarm contention with the game thread. The persistent
Vulkan pipeline cache is not cleared and no shaders/effects are disabled.
The official v0.1.1 Android game binary remains otherwise byte-identical.

This is NOT a full recompilation. No gameplay speedup is asserted until a
like-for-like Galaxy A15 benchmark confirms one.
"""
import argparse
import hashlib
import struct
from pathlib import Path

EXPECTED_SHA256 = "7342582a3db13a01844cb1975fcc2ac5bc5e84c8a638c8f7095cd03635c0f3c8"
PATCHED_SHA256 = "fcbf70754401836b31402f2bfea7b8f8fde2ede861f0f72ecacd454c598041a2"
VADDR_TO_FILE_OFFSET = 0x4000
ARM64_NOP = bytes.fromhex("1f2003d5")

# (Virtual address, expected original instruction, replacement instruction, purpose)
# Original two calls update diagnostic-only counters; final instruction is the
# worker-count result from pipeline_worker_count() in the pinned release.
# arm64 "mov x19, #3" = 0xd2800073 (73 00 80 d2 little endian).
PATCHES = (
    (0x068D9AEC, bytes.fromhex("49663494"), ARM64_NOP, "Aurora GX FIFO draw counter"),
    (0x068DCDAC, bytes.fromhex("99593494"), ARM64_NOP, "Aurora GX raw draw counter"),
    (0x068BE22C, bytes.fromhex("33159f9a"), bytes.fromhex("730080d2"),
     "Aurora pipeline workers 6 to 3 on Galaxy A15"),
)


def patch(data):
    digest = hashlib.sha256(data).hexdigest()
    if digest != EXPECTED_SHA256:
        raise ValueError(
            "Expected unmodified official v0.1.1 libmain.so SHA-256 "
            + EXPECTED_SHA256 + ", received " + digest
        )
    if data[:4] != b"\x7fELF" or data[4] != 2 or struct.unpack_from("<H", data, 18)[0] != 183:
        raise ValueError("Input is not an ELF64 AArch64 shared library")
    for vaddr, expected, replacement, purpose in PATCHES:
        offset = vaddr - VADDR_TO_FILE_OFFSET
        if data[offset:offset + 4] != expected:
            raise ValueError("Unexpected ARM64 instruction at " + hex(vaddr) + " (" + purpose + ")")
    for vaddr, _, replacement, purpose in PATCHES:
        offset = vaddr - VADDR_TO_FILE_OFFSET
        data[offset:offset + 4] = replacement
        print("Applied experimental", purpose, "at", hex(vaddr))
    result_sha = hashlib.sha256(data).hexdigest()
    if result_sha != PATCHED_SHA256:
        raise ValueError("Patched binary SHA-256 did not match expected value: " + result_sha)
    return result_sha


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("source", type=Path)
    cli.add_argument("destination", type=Path)
    args = cli.parse_args()
    if args.source.resolve() == args.destination.resolve():
        cli.error("Never overwrite the original binary; use a distinct destination")
    raw = bytearray(args.source.read_bytes())
    digest = patch(raw)
    args.destination.parent.mkdir(parents=True, exist_ok=True)
    args.destination.write_bytes(raw)
    print("Patched libmain.so SHA-256:", digest)
    print("WARNING: diagnostic GX draw counts will be 0 in this experimental runtime.")


if __name__ == "__main__":
    main()
