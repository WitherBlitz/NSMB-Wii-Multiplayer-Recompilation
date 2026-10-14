#!/usr/bin/env python3
"""Exact-build experimental ARM64 patch to remove unnecessary GPU telemetry writes.

Aurora increments a 64-bit atomic counter in its two hot draw entry points
even when NSMBW_LOG_FPS is disabled. The counter is only read for diagnostics.
The official v0.1.1 Android game binary remains otherwise byte-identical.

This is NOT a full recompilation. No gameplay speedup is asserted until a
like-for-like Galaxy A15 benchmark confirms one.
"""
import argparse
import hashlib
import struct
from pathlib import Path

EXPECTED_SHA256 = "7342582a3db13a01844cb1975fcc2ac5bc5e84c8a638c8f7095cd03635c0f3c8"
PATCHED_SHA256 = "f1b1ac089dcd8b49efef0c62aad0d55295ce215587a3ac9c8408414ebde418d8"
VADDR_TO_FILE_OFFSET = 0x4000
ARM64_NOP = bytes.fromhex("1f2003d5")

# (Virtual address, old AArch64 BL instruction, purpose).
# These BLs call the compiler's out-of-line atomic increment routine.
PATCHES = (
    (0x068D9AEC, bytes.fromhex("49663494"), "Aurora GX FIFO draw counter"),
    (0x068DCDAC, bytes.fromhex("99593494"), "Aurora GX raw draw counter"),
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
    for vaddr, expected, purpose in PATCHES:
        offset = vaddr - VADDR_TO_FILE_OFFSET
        if data[offset:offset + 4] != expected:
            raise ValueError("Unexpected ARM64 instruction at " + hex(vaddr) + " (" + purpose + ")")
    for vaddr, _, purpose in PATCHES:
        offset = vaddr - VADDR_TO_FILE_OFFSET
        data[offset:offset + 4] = ARM64_NOP
        print("Disabled redundant", purpose, "at", hex(vaddr))
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
