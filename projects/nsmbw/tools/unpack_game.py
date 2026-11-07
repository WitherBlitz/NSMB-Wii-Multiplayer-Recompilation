"""Prepare the translator's inputs from New Super Mario Bros. Wii extracted by Dolphin.

    python unpack_game.py <extracted game folder> [<output folder>] [--variant <v>] [--check]

The extracted folder is what Dolphin's Extract Entire Disc makes (files/ and sys/, or a DATA folder
holding them). This copies sys/main.dol and unpacks the four boot RELs (files/rels/*.rel.LZ, LZ77
type 0x11) into the output folder: by default the variant's folder next to this repository ("game"
for SMNE01 rev 1, where recomp.yml looks for them). Then run prelink_rels.py. Standard library only.

--variant picks the game version (projects/nsmbw/variants/variants.toml; default E1, SMNE01 rev 1):
the disc's ID, revision and main.dol checksum must be that version's. --check only checks the disc
and unpacks the RELs in memory: it writes nothing.
"""
import hashlib
import shutil
import struct
import sys
from pathlib import Path

RELS = ("d_profileNP", "d_basesNP", "d_enemiesNP", "d_en_bossNP")


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


def check_disc(root: Path, variant: dict, known: dict) -> bytes:
    """Exits unless the extract is the variant's clean disc; returns its main.dol."""
    header = (root / "sys" / "boot.bin").read_bytes()[:8]
    game_id, revision = header[:6].decode("ascii", "replace"), header[7]
    want = f"{variant['game_id']} revision {variant['revision']}"
    if game_id != variant["game_id"] or revision != variant["revision"]:
        other = next((v["name"] for v in known.values()
                      if v["game_id"] == game_id and v["revision"] == revision), None)
        hint = f" (that is variant {other}: pass --variant {other})" if other else ""
        sys.exit(f"this is {game_id} revision {revision}{hint}; "
                 f"the port needs New Super Mario Bros. Wii {want}")
    dol = (root / "sys" / "main.dol").read_bytes()
    if hashlib.sha256(dol).hexdigest() != variant["main_dol_sha256"]:
        sys.exit(f"sys/main.dol differs from the clean {want} executable (modified game?)")
    fst = struct.unpack_from(">I", (root / "sys" / "boot.bin").read_bytes(), 0x428)[0] * 4
    if "fst_size" in variant and fst != variant["fst_size"]:
        sys.exit(f"the disc's FST is 0x{fst:X} bytes, not {want}'s 0x{variant['fst_size']:X} (modified game?)")
    return dol


def main() -> None:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = sys.argv[1:]
    variant_name = "E1"
    if "--variant" in flags:
        at = flags.index("--variant")
        if at + 1 >= len(flags):
            sys.exit(__doc__)
        variant_name = flags[at + 1]
        args.remove(variant_name)
    check_only = "--check" in flags
    if not args:
        sys.exit(__doc__)
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import variants  # noqa: E402  (tomllib: Python 3.11+, only needed here)
    known = variants.load_variants()
    if variant_name not in known:
        sys.exit(f"unknown variant {variant_name}; variants.toml has {', '.join(sorted(known))}")
    variant = known[variant_name]
    root = find_root(Path(args[0]))
    out = Path(args[1]) if len(args) > 1 else variants.default_game_dir(variant)
    check_disc(root, variant, known)
    rels = {name: lz11((root / "files" / "rels" / f"{name}.rel.LZ").read_bytes()) for name in RELS}
    if check_only:
        for name, rel in rels.items():
            print(f"{name}.rel  {len(rel)} bytes")
        print(f"{variant['game_id']} revision {variant['revision']}: clean ({variant_name}); nothing written")
        return
    out.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(root / "sys" / "main.dol", out / "main.dol")
    for name in RELS:
        rel = rels[name]
        (out / f"{name}.rel").write_bytes(rel)
        print(f"{name}.rel  {len(rel)} bytes")
    nxt = "prelink_rels.py" if variant_name == "E1" else f"prelink_rels.py {out} --variant {variant_name}"
    print(f"main.dol and the RELs are in {out}; next: {nxt}")


if __name__ == "__main__":
    main()
