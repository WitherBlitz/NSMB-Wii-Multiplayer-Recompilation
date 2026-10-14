"""Prepare the translator's inputs from New Super Mario Bros. Wii (SMNE01 rev 1) extracted by Dolphin.

    python unpack_game.py <extracted game folder> [<output folder>]

The extracted folder is what Dolphin's Extract Entire Disc makes (files/ and sys/, or a DATA folder
holding them). This copies sys/main.dol and unpacks the four boot RELs (files/rels/*.rel.LZ, LZ77
type 0x11) into the output folder: by default the "game" folder next to this repository, where
recomp.yml looks for them. Then run prelink_rels.py. Standard library only.
"""
import hashlib
import shutil
import struct
import sys
from pathlib import Path

RELS = ("d_profileNP", "d_basesNP", "d_enemiesNP", "d_en_bossNP")
# The release author's DOL and a second SMNE01 rev 1 disc extraction have distinct
# SHA-256 hashes. Both still require identical section layout and boot revision.
# The alternative below was verified from a privately supplied image in October 2026;
# this does NOT establish that both game variants are byte-for-byte equivalent.
MAIN_DOL_SHA256 = "6d755ec24c395fee600a856a85785e0e99c0b7ecb680d573a6cdd1aa6f7beee7"
ALTERNATE_MAIN_DOL_SHA256 = "166eb6b7cca21418e316a6cfbf1bfbf0a213eb8f6396d42126a1482bb764a955"


def check_dol_layout(data: bytes) -> None:
    """Reject inputs that match the disc header but not the expected execution map."""
    if len(data) < 0x100:
        raise ValueError("main.dol is truncated")
    text_offsets = struct.unpack_from(">7I", data, 0)
    data_offsets = struct.unpack_from(">11I", data, 0x1c)
    text_addrs = struct.unpack_from(">7I", data, 0x48)
    data_addrs = struct.unpack_from(">11I", data, 0x64)
    sizes = struct.unpack_from(">18I", data, 0x90)
    entry_point = struct.unpack_from(">I", data, 0xe0)[0]
    if entry_point != 0x80004050 or text_offsets[0] != 0x100 or text_addrs[0] != 0x80004000:
        raise ValueError("DOL entry point or code layout does not match SMNE01 rev 1")
    if text_addrs[1] != 0x80006780 or data_addrs[2] != 0x802ed9e0:
        raise ValueError("DOL section addresses differ from the recompilation address map")
    for offset, addr, size in zip(text_offsets + data_offsets, text_addrs + data_addrs, sizes):
        if size and (offset < 0x100 or offset + size > len(data) or not 0x80000000 <= addr < 0x81800000):
            raise ValueError("DOL has an invalid or out-of-bounds section")

DEFAULT_OUT = Path(__file__).resolve().parents[3].parent / "game"


def lz11(data: bytes) -> bytes:
    if data[0] != 0x11:
        raise ValueError("not LZ77 type 0x11 data")
    size = data[1] | data[2] << 8 | data[3] << 16
    pos = 4
    if size == 0:
        size = struct.unpack_from("<I", data, 4)[0]
        pos = 8
    out = bytearray()
    while len(out) < size:
        flags = data[pos]
        pos += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if not flags & (0x80 >> bit):
                out.append(data[pos])
                pos += 1
                continue
            b0 = data[pos]
            if b0 >> 4 == 0:
                b1, b2 = data[pos + 1], data[pos + 2]
                length = ((b0 & 0xF) << 4 | b1 >> 4) + 0x11
                disp = ((b1 & 0xF) << 8 | b2) + 1
                pos += 3
            elif b0 >> 4 == 1:
                b1, b2, b3 = data[pos + 1], data[pos + 2], data[pos + 3]
                length = ((b0 & 0xF) << 12 | b1 << 4 | b2 >> 4) + 0x111
                disp = ((b2 & 0xF) << 8 | b3) + 1
                pos += 4
            else:
                b1 = data[pos + 1]
                length = (b0 >> 4) + 1
                disp = ((b0 & 0xF) << 8 | b1) + 1
                pos += 2
            start = len(out) - disp
            for i in range(length):  # the source may overlap what is being written
                out.append(out[start + i])
    return bytes(out[:size])


def find_root(path: Path) -> Path:
    for candidate in (path, path / "DATA"):
        if (candidate / "files").is_dir() and (candidate / "sys" / "main.dol").is_file():
            return candidate
    sys.exit(f"{path} is not an extracted game: it needs files/ and sys/ (Dolphin's Extract Entire Disc)")


def main() -> None:
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    root = find_root(Path(sys.argv[1]))
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_OUT
    header = (root / "sys" / "boot.bin").read_bytes()[:8]
    if header[:6] != b"SMNE01" or header[7] != 1:
        sys.exit(f"this is {header[:6].decode('ascii', 'replace')} revision {header[7]}; "
                 "the port needs New Super Mario Bros. Wii SMNE01 revision 1")
    dol = (root / "sys" / "main.dol").read_bytes()
    digest = hashlib.sha256(dol).hexdigest()
    if digest not in (MAIN_DOL_SHA256, ALTERNATE_MAIN_DOL_SHA256):
        sys.exit("sys/main.dol is not among the two verified SMNE01 rev 1 layouts: "
                 f"SHA-256={digest}. Refusing to translate an unknown executable.")
    try:
        check_dol_layout(dol)
    except ValueError as error:
        sys.exit(f"Unsupported DOL layout: {error}")
    if digest == ALTERNATE_MAIN_DOL_SHA256:
        print("NOTE: alternate verified SMNE01 rev 1 executable. "
              "Translator must use a matching local project hash; "
              "compatibility with the original function map is not yet verified.")
    out.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(root / "sys" / "main.dol", out / "main.dol")
    (out / "main.dol.sha256").write_text(digest + "\n")
    for name in RELS:
        rel = lz11((root / "files" / "rels" / f"{name}.rel.LZ").read_bytes())
        (out / f"{name}.rel").write_bytes(rel)
        print(f"{name}.rel  {len(rel)} bytes")
    print(f"main.dol and the RELs are in {out}; next: prelink_rels.py")


if __name__ == "__main__":
    main()
