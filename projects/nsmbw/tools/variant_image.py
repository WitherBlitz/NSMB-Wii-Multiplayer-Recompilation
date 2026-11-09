"""A game version's boot image for the variant tools: main.dol plus the four boot RELs linked at
their heap addresses (prelink_rels.link, before p1_character's patches), as one flat MEM1 image.

Reads the folder unpack_game.py fills, or a Dolphin extract directly (RELs decompressed in memory):
nothing is written.
"""
import bisect
import struct

import prelink_rels

BASE, SIZE = 0x80000000, 0x01800000


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


class Dol:
    def __init__(self, data):
        self.data = data
        offs = struct.unpack_from(">18I", data, 0x00)
        addrs = struct.unpack_from(">18I", data, 0x48)
        sizes = struct.unpack_from(">18I", data, 0x90)
        self.sections = [(i, offs[i], addrs[i], sizes[i]) for i in range(18) if sizes[i]]
        self.bss, self.bss_size, self.entry = struct.unpack_from(">3I", data, 0xD8)

    def text(self):
        return [(a, a + s) for i, _o, a, s in self.sections if i < 7]

    def data_sections(self):
        return [(a, a + s) for i, _o, a, s in self.sections if i >= 7]

    def sda_bases(self):
        """(_SDA_BASE_, _SDA2_BASE_): .sdata and .sdata2 (the last two data sections) + 0x8000."""
        data = self.data_sections()
        return data[-2][0] + 0x8000, data[-1][0] + 0x8000

    def init_registers(self):
        """The constants __init_registers puts in r1, r2 and r13: lis rN, hi / ori rN, rN, lo in .init."""
        _i, off, addr, size = self.sections[0]
        words = struct.unpack_from(f">{size // 4}I", self.data, off)
        found = {}
        for k in range(len(words) - 1):
            w, x = words[k], words[k + 1]
            rd = (w >> 21) & 31
            if w >> 26 == 15 and (w >> 16) & 31 == 0 and rd in (1, 2, 13) and x >> 26 == 24 and \
                    (x >> 21) & 31 == rd and (x >> 16) & 31 == rd:
                found.setdefault(rd, set()).add((w & 0xFFFF) << 16 | x & 0xFFFF)
        return {rd: (v.pop() if len(v) == 1 else None) for rd, v in found.items()}


class Image:
    """main.dol + the linked RELs. word(a) is None outside what was loaded."""

    def __init__(self, dol_bytes, rel_raws, va=None):
        self.dol = Dol(dol_bytes)
        if va is None:
            self.linked = prelink_rels.link(rel_raws, log=lambda _line: None)
        else:
            self.linked = prelink_rels.link(rel_raws, va(prelink_rels.IMAGE_BASE[1]), None, log=lambda _line: None)
        self.flat = bytearray(SIZE)
        spans = []  # (start, end, kind)
        for i, off, addr, size in self.dol.sections:
            self.flat[addr - BASE:addr - BASE + size] = dol_bytes[off:off + size]
            spans.append((addr, addr + size, "text" if i < 7 else "data"))
        lk = self.linked
        self.flat[lk.base - BASE:lk.end - BASE] = lk.mem
        rel_spans = []
        for m in lk.mods.values():
            for idx, (o, exe, size) in enumerate(m.sections):
                if not size:
                    continue
                a = m.section_addr(idx)
                rel_spans.append((a, a + size, "text" if exe else ("bss" if a == m.bss_addr else "data")))
        rel_spans.sort()
        # The REL area outside every section (headers, alignment) is loaded but belongs to none.
        cursor = lk.base
        for a, b, k in rel_spans:
            if a > cursor:
                spans.append((cursor, a, "rel-other"))
            spans.append((a, b, k))
            cursor = max(cursor, b)
        if cursor < lk.end:
            spans.append((cursor, lk.end, "rel-other"))
        # The DOL's bss range also covers .sdata/.sdata2, which are loaded sections.
        cursor, bss_end = self.dol.bss, self.dol.bss + self.dol.bss_size
        for _i, _o, addr, size in sorted(self.dol.sections, key=lambda s: s[2]):
            if addr + size <= cursor or addr >= bss_end:
                continue
            if addr > cursor:
                spans.append((cursor, addr, "bss"))
            cursor = max(cursor, addr + size)
        if cursor < bss_end:
            spans.append((cursor, bss_end, "bss"))
        spans.sort()
        self.spans = spans
        self._starts = [s[0] for s in spans]

    def kind(self, a):
        i = bisect.bisect_right(self._starts, a) - 1
        if i >= 0 and a < self.spans[i][1]:
            return self.spans[i][2]
        return None

    def word(self, a):
        k = self.kind(a)
        if k is None or k == "bss" or self.kind(a + 3) is None:
            return None
        return struct.unpack_from(">I", self.flat, a - BASE)[0]

    def exec_ranges(self):
        return [(a, b) for a, b, k in self.spans if k == "text"]

    def rel_layout(self):
        return {m.name: (m.image, m.bss_addr, m.end) for m in self.linked.mods.values()}

    def rel_entries(self):
        """REL prolog / epilog / unresolved addresses, as prelink_rels.py lists them."""
        out = []
        for m in self.linked.mods.values():
            out += [m.section_addr(m.prolog_sec) + m.prolog, m.section_addr(m.epilog_sec) + m.epilog,
                    m.section_addr(m.unres_sec) + m.unres]
        return out
