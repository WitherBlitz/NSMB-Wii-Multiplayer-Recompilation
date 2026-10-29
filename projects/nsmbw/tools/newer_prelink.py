"""Offline Newer Super Mario Bros. Wii loader for the translator (NSMBW SMNE01 rev 1 base).

    python newer_prelink.py <extracted Newer folder> [<clean game folder>] [<output folder>]

Newer's disc patch redirects the boot scene's dylink init phase (0x80328130) to a small loader it
put in main.dol's .data. At boot that loader reads /NewerRes/SystemUS_1.bin (raw writes into game
memory), loads DLCodeUS_1.bin into a heap block and links it with DLRelocsUS_1.bin (relocations
inside the blob plus the hooks and pointers it writes into game code and data). A static
recompilation never runs patched code, so this script does the same work offline:

- main.dol: Newer's main.dol with the System writes and the hooks that land in it applied.
- nsmbw_linked.rel: prelink_rels.py's synthetic REL with the hooks into the RELs applied and the
  linked blob appended at the heap address the loader gets on hardware (DLCODE_ADDR).
- MAP.txt: the base function map plus the loader and every function start in the blob.

The loader still runs at runtime, so the guest heap must hand it DLCODE_ADDR: its "DLCode @"
OSReport line in the log shows where it really went. Run prelink_rels.py on the clean game first.
"""
import hashlib
import json
import struct
import sys
from pathlib import Path

from unpack_game import RELS, find_root, lz11

REPO = Path(__file__).resolve().parents[3]
CLEAN = Path(sys.argv[2]) if len(sys.argv) > 2 else REPO.parent / "game"
OUT = Path(sys.argv[3]) if len(sys.argv) > 3 else REPO.parent / "game-newer"

# Where the loader's EGG::Heap::alloc puts the blob at boot: the runtime's log line
# "[OSReport] DLCode @ 0x80df8fc0" (the guest heap layout is deterministic, as for the RELs).
DLCODE_ADDR = 0x80DF8FC0
# The loader's region check: halfword at 0x80768D50 (d_profileNP) is 0x6CA1 on SMNE01 rev 1.
VARIANT = "US_1"
LOADER = (0x803482C0, 0x80348410, 0x80348850, 0x80348B40, 0x80348BE8)
PHASE_SLOT, ORIGINAL_PHASE = 0x80328130, 0x8015BB20

R_ADDR32, R_ADDR16_LO, R_ADDR16_HI, R_ADDR16_HA, R_REL24 = 1, 4, 5, 6, 10


def rd(b, o):
    return struct.unpack_from(">I", b, o)[0]


class Dol:
    def __init__(self, data):
        self.data = bytearray(data)
        offs = struct.unpack(">18I", data[0x00:0x48])
        addrs = struct.unpack(">18I", data[0x48:0x90])
        sizes = struct.unpack(">18I", data[0x90:0xD8])
        self.sections = [(i < 7, offs[i], addrs[i], sizes[i]) for i in range(18) if sizes[i]]

    def locate(self, addr):
        for text, off, a, size in self.sections:
            if a <= addr < a + size:
                return text, off + addr - a
        return None


class Image:
    """main.dol + the synthetic REL + the blob, as one writable guest address space."""

    def __init__(self, dol, rel, rel_base, blob):
        self.dol, self.rel, self.rel_base, self.blob = dol, rel, rel_base, blob
        self.writes = {"dol_text": 0, "dol_data": 0, "rel": 0, "blob": 0}

    def _where(self, addr):
        hit = self.dol.locate(addr)
        if hit:
            return self.dol.data, hit[1], "dol_text" if hit[0] else "dol_data"
        if self.rel_base <= addr < self.rel_base + len(self.rel):
            return self.rel, addr - self.rel_base, "rel"
        if DLCODE_ADDR <= addr < DLCODE_ADDR + len(self.blob):
            return self.blob, addr - DLCODE_ADDR, "blob"
        sys.exit(f"Newer writes to 0x{addr:08X}, outside main.dol, the RELs and the blob")

    def rd32(self, addr):
        buf, off, _ = self._where(addr)
        return rd(buf, off)

    def wr(self, addr, data, count=True):
        buf, off, kind = self._where(addr)
        buf[off:off + len(data)] = data
        if count:
            self.writes[kind] += 1

    def wr32(self, addr, v):
        self.wr(addr, struct.pack(">I", v & 0xFFFFFFFF))

    def wr16(self, addr, v):
        self.wr(addr, struct.pack(">H", v & 0xFFFF))


def apply_system(img, data):
    """The loader's patcher (0x80348B40): [addr|0x80000000, value] or [count, dest, words...]."""
    pos, records = 0, 0
    while True:
        head = rd(data, pos)
        pos += 4
        if head == 0xFFFFFFFF:
            return records
        if head & 0x80000000:
            img.wr32(head, rd(data, pos))
            pos += 4
        else:
            dest = rd(data, pos)
            pos += 4
            img.wr(dest, data[pos:pos + 4 * head])
            pos += 4 * head
        records += 1


def apply_relocs(img, data):
    """The loader's linker (0x80348BE8). Each 8-byte entry is (type << 24 | symbol index, target);
    symbols and targets with the top bit clear are offsets into the blob."""
    if data[:8] != b"NewerREL":
        sys.exit("DLRelocs file has no NewerREL header")
    table = rd(data, 8)
    counts, hooks = {}, []
    for pos in range(12, table, 8):
        info, target = rd(data, pos), rd(data, pos + 4)
        typ, value = info >> 24, rd(data, table + (info & 0xFFFFFF) * 4)
        sym_in_blob, dst_in_blob = not value & 0x80000000, not target & 0x80000000
        if sym_in_blob:
            value += DLCODE_ADDR
        if dst_in_blob:
            target += DLCODE_ADDR
        if typ == R_ADDR32:
            img.wr32(target, value)
        elif typ == R_ADDR16_LO:
            img.wr16(target, value)
        elif typ == R_ADDR16_HI:
            img.wr16(target, value >> 16)
        elif typ == R_ADDR16_HA:
            img.wr16(target, (value >> 16) + (1 if value & 0x8000 else 0))
        elif typ == R_REL24:
            img.wr32(target, (img.rd32(target) & ~0x03FFFFFC) | ((value - target) & 0x03FFFFFC))
        else:
            sys.exit(f"unhandled Newer relocation type {typ} at 0x{target:08X}")
        counts[typ] = counts.get(typ, 0) + 1
        hooks.append((typ, target, value, sym_in_blob, dst_in_blob))
    return counts, hooks


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def branch_target(word, addr):
    """Target of a b/bl (opcode 18) or bc (16), or None."""
    op = word >> 26
    if op == 18:
        disp = sext(word & 0x03FFFFFC, 26)
    elif op == 16:
        disp = sext(word & 0xFFFC, 16)
    else:
        return None
    return (disp if word & 2 else addr + disp) & 0xFFFFFFFF


def encode_b(src, dst, link=False):
    disp = dst - src
    assert -0x2000000 <= disp < 0x2000000, (hex(src), hex(dst))
    return 0x48000000 | (disp & 0x03FFFFFC) | (1 if link else 0)


def encode_bc(word, src, dst):
    """word's BO/BI and LK, retargeted (bc or bclr alike) as a relative bc."""
    disp = dst - src
    assert -0x8000 <= disp < 0x8000, (hex(src), hex(dst))
    return (16 << 26) | (word & 0x03FF0000) | (disp & 0xFFFC) | (word & 1)


BLR, MFLR_MASK, MFLR, MTLR = 0x4E800020, 0xFC1FFFFF, 0x7C0802A6, 0x7C0803A6


def stub_flow(img, entry, text_lo, text_hi, starts):
    """Walk a hook stub (not into its calls) and the jumps it takes inside Newer's code. LR is "orig"
    (the hook's return address, or saved with mflr first) or "replaced" (mtlr of something else, the
    hooked function's own epilogue that returns for it). Returns {addr: lr} and the exits as
    (addr, kind, lr) with kind blr/bclr/bctr/tail (a b to a function start)."""
    state, exits, work = {}, [], [(entry, "orig")]
    while work:
        a, lr = work.pop()
        while True:
            if not text_lo <= a < text_hi:
                sys.exit(f"hook stub at 0x{entry:08X} runs out of Newer's code at 0x{a:08X}")
            if a in state:
                if state[a] != lr:
                    sys.exit(f"hook stub at 0x{entry:08X} reaches 0x{a:08X} both with and without the hook's LR")
                break
            state[a] = lr
            w = img.rd32(a)
            op = w >> 26
            if w & MFLR_MASK == MFLR:
                lr = "saved" if lr == "orig" else lr
            elif w & MFLR_MASK == MTLR and lr != "saved":
                lr = "replaced"
            if w == BLR:
                exits.append((a, "blr", lr))
                break
            if op == 19 and (w >> 1) & 0x3FF in (16, 528) and not w & 1:
                kind = "blr" if (w >> 1) & 0x3FF == 16 else "bctr"
                if (w >> 21) & 0x14 == 0x14:
                    exits.append((a, kind, lr))
                    break
                exits.append((a, "bclr" if kind == "blr" else "bcctr", lr))
                a += 4
                continue
            t = branch_target(w, a)
            if t is not None and not w & 1:
                if op == 18 and (t in starts or not text_lo <= t < text_hi):
                    exits.append((a, "tail", lr))
                    break
                if op == 18 or (w >> 21) & 0x14 == 0x14:
                    a = t
                    continue
                work.append((t, lr))
            a += 4
    return state, exits


def plain_call(exits):
    return all(lr != "replaced" for _a, _k, lr in exits)


def build_trampoline(img, entry, hook, base, state, exits):
    """A copy of the stub for `b` instead of `bl` at `hook`: every exit that returned to hook+4 now
    branches there (calls stay calls), exits that return for the hooked function stay as they are."""
    order = sorted(state)
    exit_kind = {a: (k, lr) for a, k, lr in exits}

    def size_of(a):
        k, lr = exit_kind.get(a, (None, None))
        return 8 if lr in ("orig", "saved") and k in ("blr", "tail", "bctr") else 4

    def falls_through(a):
        k, _lr = exit_kind.get(a, (None, None))
        if k in ("blr", "tail", "bctr"):
            return False
        w = img.rd32(a)
        if w >> 26 == 18 and not w & 1:
            return False
        return not (w >> 26 == 16 and (w >> 21) & 0x14 == 0x14 and not w & 1)

    # Each copied instruction becomes one or two words; a gap in the original order needs a jump.
    layout, pos = {}, base
    for i, a in enumerate(order):
        layout[a] = pos
        pos += size_of(a)
        if falls_through(a) and (i + 1 == len(order) or order[i + 1] != a + 4):
            pos += 4
    slot = pos  # shared "b hook+4" for conditional normal returns
    words = []

    def emit(addr, w):
        words.append((addr, w))

    for i, a in enumerate(order):
        w = img.rd32(a)
        at = layout[a]
        k, lr = exit_kind.get(a, (None, None))
        normal = lr in ("orig", "saved")
        t = branch_target(w, a)
        if k == "blr" and normal:
            emit(at, encode_b(at, hook + 4))
            emit(at + 4, 0x60000000)
        elif k == "tail" and normal:
            emit(at, encode_b(at, t, link=True))
            emit(at + 4, encode_b(at + 4, hook + 4))
        elif k == "bctr" and normal:
            emit(at, 0x4E800421)  # bctrl
            emit(at + 4, encode_b(at + 4, hook + 4))
        elif k == "bclr" and normal:
            emit(at, encode_bc(w, at, slot))
        elif k == "bcctr" and normal:
            sys.exit(f"hook stub at 0x{entry:08X}: conditional bctr at 0x{a:08X} is not supported")
        elif t is not None and not (w & 1):
            emit(at, (encode_b(at, layout.get(t, t)) if w >> 26 == 18 else encode_bc(w, at, layout[t])))
        elif t is not None:
            if w >> 26 != 18:
                sys.exit(f"conditional call in hook stub at 0x{a:08X} is not supported")
            emit(at, encode_b(at, t, link=True))
        else:
            emit(at, w)
        if falls_through(a) and (i + 1 == len(order) or order[i + 1] != a + 4):
            emit(at + size_of(a), encode_b(at + size_of(a), layout[a + 4]))
    emit(slot, encode_b(slot, hook + 4))
    return words, layout[entry], slot + 4


def blob_text_end(blob, hooks):
    """Code comes first in the blob; its data starts at the first pointer-sized relocation target."""
    data_start = min(t for typ, t, _v, _s, in_blob in hooks if in_blob and typ == R_ADDR32)
    return data_start - DLCODE_ADDR


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    root = find_root(Path(sys.argv[1]))
    res = root / "files" / "NewerRes"
    for name in ("SystemUS_1.bin", "DLCodeUS_1.bin", "DLRelocsUS_1.bin"):
        if not (res / name).is_file():
            sys.exit(f"{res / name} is missing: is this Newer Super Mario Bros. Wii?")
    for name in RELS:
        newer = lz11((root / "files" / "rels" / f"{name}.rel.LZ").read_bytes())
        if newer != (CLEAN / f"{name}.rel").read_bytes():
            sys.exit(f"{name}.rel differs from the clean game's; only main.dol may be patched")

    dol = Dol((root / "sys" / "main.dol").read_bytes())
    text, off = dol.locate(PHASE_SLOT)
    loader_hook = rd(dol.data, off)
    if loader_hook != LOADER[0]:
        sys.exit(f"0x{PHASE_SLOT:08X} holds 0x{loader_hook:08X}, not Newer's loader at 0x{LOADER[0]:08X}")
    rel = bytearray((CLEAN / "nsmbw_linked.rel").read_bytes())
    layout = json.loads((CLEAN / "nsmbw_rel_layout.json").read_text())
    rel_base = int(layout["image_start"], 16)
    if struct.unpack_from(">H", rel, 0x80768D52 - rel_base)[0] != 0x6CA1:
        sys.exit("d_profileNP does not carry the SMNE01 rev 1 marker the loader checks")
    if DLCODE_ADDR < rel_base + len(rel):
        sys.exit(f"DLCODE_ADDR 0x{DLCODE_ADDR:08X} overlaps the RELs (end 0x{rel_base + len(rel):08X})")

    blob = bytearray((res / f"DLCode{VARIANT}.bin").read_bytes())
    img = Image(dol, rel, rel_base, blob)
    records = apply_system(img, (res / f"System{VARIANT}.bin").read_bytes())
    counts, hooks = apply_relocs(img, (res / f"DLRelocs{VARIANT}.bin").read_bytes())
    text_end = blob_text_end(blob, hooks)
    print(f"System{VARIANT}.bin: {records} patch records; DLRelocs: " +
          ", ".join(f"type {t}={n}" for t, n in sorted(counts.items())))
    print("writes by region: " + ", ".join(f"{k}={v}" for k, v in img.writes.items()))

    # Function starts in the blob: its entry (the boot phase hook calls offset 0), every branch
    # target and every code pointer (vtables, profile tables, lis/addi pairs) that lands in its code.
    text_lo, text_hi = DLCODE_ADDR, DLCODE_ADDR + text_end
    starts = {DLCODE_ADDR}
    for typ, _target, value, sym_in_blob, _dst in hooks:
        if sym_in_blob and text_lo <= value < text_hi and typ in (R_REL24, R_ADDR32, R_ADDR16_LO):
            starts.add(value)

    # A bl hook whose stub returns for the hooked function (its epilogue: mtlr + blr past the bl)
    # works on hardware but not as a translated call, which comes back and runs that epilogue again.
    # Such hooks become b: straight to the stub when it never returns to the hook, or to a copy of
    # it whose returns to the hook branch there instead (code only the translator reads).
    tramp_base = tramp_pos = (DLCODE_ADDR + len(blob) + 0x1F) & ~0x1F
    tramp_words, rewritten = [], []
    for typ, target, value, sym_in_blob, dst_in_blob in hooks:
        if typ != R_REL24 or dst_in_blob or not sym_in_blob or not img.rd32(target) & 1:
            continue
        state, exits = stub_flow(img, value, text_lo, text_hi, starts)
        if plain_call(exits):
            continue
        if all(lr == "replaced" for _a, _k, lr in exits):
            img.wr32(target, encode_b(target, value))
            rewritten.append(f"0x{target:08X} -> b 0x{value:08X}")
            continue
        words, copy, end = build_trampoline(img, value, target, tramp_pos, state, exits)
        tramp_words += words
        img.wr32(target, encode_b(target, copy))
        starts.add(copy)
        rewritten.append(f"0x{target:08X} -> b 0x{copy:08X} (copy of 0x{value:08X})")
        tramp_pos = (end + 0x1F) & ~0x1F
    tramps = bytearray(tramp_pos - tramp_base)
    for addr, w in tramp_words:
        struct.pack_into(">I", tramps, addr - tramp_base, w)
    print(f"{len(rewritten)} bl hook(s) that return for the hooked function rewritten as b")

    # The title demo loads each player's replay into a unit of a 4-unit heap (dSys_c::create).
    # Newer's replays are LZ-compressed, and decompressing takes a second unit for the read chunk
    # while the replay holds its own, so the fourth player of a 4-player demo gets no chunk and
    # keeps raw compressed data: garbage header, garbage power-up, crash. Five units of 0x60000
    # (the biggest replay, otehon/08-24_N_1.bin, is 0x5EC24) instead of four of 0x70800.
    for addr, old, new in ((0x800E4AE4, 0x3C80001C, 0x3C80001E),   # lis r4, 0x1C -> 0x1E
                           (0x800E4AEC, 0x38842000, 0x38840000),   # addi r4, r4, 0x2000 -> 0
                           (0x800E4AF0, 0x38A00004, 0x38A00005)):  # li r5, 4 -> 5
        if img.rd32(addr) != old:
            sys.exit(f"0x{addr:08X} holds 0x{img.rd32(addr):08X}, not dSys_c::create's replay heap size")
        img.wr32(addr, new)

    base_map = (REPO / "projects" / "nsmbw" / "MAP.txt").read_text()
    lines = [base_map.rstrip("\n"), "# Newer Super Mario Bros. Wii loader (main.dol .data) and blob"]
    lines += [f"{a:08x} {a:08x}" for a in LOADER]
    lines += [f"{a:08x} newer_{a - DLCODE_ADDR:05x}" for a in sorted(starts)]

    # Synthetic REL: the gap up to the blob, then the blob as a code section and a data section.
    nsec, secoff = struct.unpack_from(">II", rel, 0x0C)
    table = [struct.unpack_from(">II", rel, secoff + i * 8) for i in range(nsec)]
    pad = DLCODE_ADDR - (rel_base + len(rel))
    blob_off = len(rel) + pad
    table += [(blob_off | 1, text_end), (blob_off + text_end, len(blob) - text_end)]
    if tramps:
        table.append(((blob_off + tramp_base - DLCODE_ADDR) | 1, len(tramps)))
    hdr_size = 0x4C + 8 * len(table)
    first_payload = min(o & ~1 for o, s in table if s)
    if hdr_size > first_payload:
        sys.exit(f"synthetic header (0x{hdr_size:X}) would overwrite payload at 0x{first_payload:X}")
    # The original header's section table sits inside it; the new one is two entries longer.
    out_rel = rel + bytes(pad) + blob + bytes(tramp_base - DLCODE_ADDR - len(blob)) + tramps
    size = len(out_rel)
    struct.pack_into(">II", out_rel, 0x0C, len(table), 0x4C)
    struct.pack_into(">II", out_rel, 0x24, size, size)  # relocation/import offsets: none, at the end
    struct.pack_into(">I", out_rel, 0x48, size)  # fixSize
    for i, (o, s) in enumerate(table):
        struct.pack_into(">II", out_rel, 0x4C + i * 8, o, s)

    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "main.dol").write_bytes(dol.data)
    (OUT / "nsmbw_linked.rel").write_bytes(out_rel)
    (OUT / "MAP.txt").write_text("\n".join(lines) + "\n")
    layout["newer"] = {
        "dlcode": f"0x{DLCODE_ADDR:08X}", "dlcode_size": f"0x{len(blob):X}",
        "dlcode_text_end": f"0x{DLCODE_ADDR + text_end:08X}", "variant": VARIANT,
        "function_starts": len(starts),
        "rewritten_hooks": rewritten,
        "hooks_into_game": [f"0x{t:08X}" for typ, t, _v, _s, in_blob in hooks if not in_blob],
    }
    (OUT / "nsmbw_rel_layout.json").write_text(json.dumps(layout, indent=2))
    print(f"blob at 0x{DLCODE_ADDR:08X} (code 0x{text_end:X}, data 0x{len(blob) - text_end:X} bytes), "
          f"{len(starts)} function starts")
    print(f"main.dol sha256 {hashlib.sha256(dol.data).hexdigest()}")
    print(f"wrote main.dol, nsmbw_linked.rel, MAP.txt and nsmbw_rel_layout.json to {OUT}")


if __name__ == "__main__":
    main()
