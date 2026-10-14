"""Audit a private NSMBW rebuild ZIP against the public recompilation function map.

Runs locally without copying copyrighted game data into the repository.
Usage:
    python projects/nsmbw/tools/audit_rebuild_input.py \
        /path/to/NSMBW-private-build-inputs.zip
"""
import hashlib
import re
import struct
import sys
from pathlib import Path
from zipfile import ZipFile

from unpack_game import (
    MAIN_DOL_SHA256,
    ALTERNATE_MAIN_DOL_SHA256,
    check_dol_layout,
    lz11,
)

ROOT = Path(__file__).resolve().parents[1]
FUNCTION_MAP = ROOT / "MAP.txt"
MODULES = (
    ("d_profileNP", 0x807684C0),
    ("d_basesNP", 0x8076D680),
    ("d_enemiesNP", 0x809A2CC0),
    ("d_en_bossNP", 0x80B1C940),
)
MAP_LINE = re.compile(r"^([0-9a-fA-F]{8})(?:[ \t]|$)")


def ranges_from_dol(raw):
    check_dol_layout(raw)
    offsets = struct.unpack_from(">7I", raw, 0)
    addresses = struct.unpack_from(">7I", raw, 0x48)
    sizes = struct.unpack_from(">7I", raw, 0x90)
    return [
        (addr, addr + size, "main.dol")
        for offset, addr, size in zip(offsets, addresses, sizes)
        if size and offset >= 0x100
    ]


def ranges_from_rel(raw, image, name):
    if len(raw) < 0x4c:
        raise ValueError(f"{name}: REL header is truncated")
    section_count, section_offset = struct.unpack_from(">II", raw, 0x0c)
    if not 1 <= section_count <= 32 or section_offset + section_count * 8 > len(raw):
        raise ValueError(f"{name}: invalid section table")
    ranges = []
    for n in range(section_count):
        offset, size = struct.unpack_from(">II", raw, section_offset + 8 * n)
        if offset & 1 and size:
            start = image + (offset & ~1)
            end = start + size
            if offset & ~1 < 0x4c or (offset & ~1) + size > len(raw):
                raise ValueError(f"{name}: executable section extends beyond REL data")
            ranges.append((start, end, name))
    return ranges


def validate_function_map(lines, executable_ranges):
    """Return code-location counts, rejecting unaligned or unmapped function starts."""
    counts = {name: 0 for _, _, name in executable_ranges}
    total = 0
    failures = []
    unique = set()
    for line in lines:
        match = MAP_LINE.match(line)
        if not match:
            continue
        address = int(match.group(1), 16)
        total += 1
        if address in unique:
            failures.append(f"duplicate address 0x{address:08X}")
        unique.add(address)
        where = next((name for lo, hi, name in executable_ranges if lo <= address < hi), None)
        if address & 3 or where is None:
            failures.append(f"unmapped or misaligned 0x{address:08X}")
        else:
            counts[where] += 1
        if len(failures) > 25:
            break
    if not total:
        raise ValueError("The function map is empty or unrecognized")
    if failures:
        raise ValueError("Function map doesn't fit this executable: " + ", ".join(failures))
    return total, counts


def audit(zip_path, map_path=FUNCTION_MAP):
    with ZipFile(zip_path) as archive:
        names = set(archive.namelist())
        required = {"sys/boot.bin", "sys/main.dol"} | {
            f"files/rels/{name}.rel.LZ" for name, _ in MODULES
        }
        if required - names:
            raise ValueError("Missing private build input files: " + ", ".join(sorted(required - names)))
        boot = archive.read("sys/boot.bin")
        if len(boot) < 8 or boot[:6] != b"SMNE01" or boot[7] != 1:
            raise ValueError("Expected SMNE01 USA revision 1")
        dol = archive.read("sys/main.dol")
        digest = hashlib.sha256(dol).hexdigest()
        if digest not in (MAIN_DOL_SHA256, ALTERNATE_MAIN_DOL_SHA256):
            raise ValueError("Unrecognized main.dol SHA-256: " + digest)
        ranges = ranges_from_dol(dol)
        for name, image in MODULES:
            compressed = archive.read(f"files/rels/{name}.rel.LZ")
            raw = lz11(compressed)
            ranges.extend(ranges_from_rel(raw, image, name))
    # This source file is public, but the game inputs never leave this process.
    total, counts = validate_function_map(
        map_path.read_text(encoding="utf-8").splitlines(), ranges
    )
    print(f"Game: SMNE01 rev 1; DOL SHA-256: {digest}")
    print(f"Executable ranges: {len(ranges)}; function map entries: {total}")
    for name, n in counts.items():
        print(f"  {name}: {n}")
    print("Address-map structure: PASS")
    print("NOTE: this does NOT establish instruction-by-instruction equivalence.")
    return total


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    try:
        audit(Path(sys.argv[1]))
    except (OSError, ValueError, IndexError, struct.error) as err:
        sys.exit(f"FAIL: {err}")
