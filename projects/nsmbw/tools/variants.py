"""The game versions the port builds for (variants/variants.toml) and their address tables.

E1 (USA rev 1) is the canonical address space: the runtime and the tools are written against E1
addresses, and each variant's table maps an E1 address to that variant's. The table is the
address map's strict E1 -> variant mapping (nsmbw_versions.range_table), then the variant's
hand-written overrides (variants/<v>/overrides.tsv), then the values computed from the binaries
(the p1_character stub area). For E1 it is the identity, and no address map is needed.
"""
import bisect
import re
import tomllib
from pathlib import Path

import nsmbw_versions

TOOLS = Path(__file__).resolve().parent
PROJECT = TOOLS.parent
REPO = PROJECT.parents[1]
VARIANTS_DIR = PROJECT / "variants"
CANONICAL = "E1"
MEM1 = nsmbw_versions.MEM1


def load_variants():
    with (VARIANTS_DIR / "variants.toml").open("rb") as fh:
        table = tomllib.load(fh)
    for name, v in table.items():
        v["name"] = name
    return table


def get(name):
    table = load_variants()
    if name not in table:
        raise SystemExit(f"unknown variant {name}; variants.toml has {', '.join(sorted(table))}")
    return table[name]


def default_game_dir(variant):
    return REPO.parent / variant["game_dir"]


class Override:
    def __init__(self, start, end, target, reason, line):
        self.start, self.end, self.target, self.reason, self.line = start, end, target, reason, line

    def __repr__(self):
        t = "none" if self.target is None else f"0x{self.target:08X}"
        return f"0x{self.start:08X}-0x{self.end - 1:08X} -> {t} ({self.reason})"


def read_overrides(variant):
    """variants/<v>/overrides.tsv: `canonical[-canonical_last]<TAB>target|none<TAB>reason`."""
    path = VARIANTS_DIR / variant["name"] / "overrides.tsv"
    out = []
    if not path.is_file():
        return out
    for n, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.rstrip()
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        cols = [c.strip() for c in line.split("\t") if c.strip()]
        m = re.fullmatch(r"0x([0-9A-Fa-f]{8})(?:-0x([0-9A-Fa-f]{8}))?", cols[0]) if cols else None
        if not m or len(cols) < 3:
            raise SystemExit(f"{path}:{n}: expected `canonical<TAB>target|none<TAB>reason`")
        start = int(m.group(1), 16)
        end = int(m.group(2), 16) + 1 if m.group(2) else start + 1
        target = None if cols[1].lower() == "none" else int(cols[1], 16)
        out.append(Override(start, end, target, cols[2], n))
    return out


class AddressTable:
    """E1 address -> variant address, as sorted disjoint rows (start, end_exclusive, target, source)."""

    def __init__(self, rows):
        rows = sorted(rows)
        for a, b in zip(rows, rows[1:]):
            if a[1] > b[0]:
                raise SystemExit(f"address table rows overlap: {a} {b}")
        self.rows = rows
        self._starts = [r[0] for r in rows]

    @classmethod
    def identity(cls):
        return cls([(MEM1[0], MEM1[1], MEM1[0], "identity")])

    def lookup(self, addr):
        i = bisect.bisect_right(self._starts, addr) - 1
        if i >= 0 and addr < self.rows[i][1]:
            return self.rows[i][2] + addr - self.rows[i][0]
        return None

    def source(self, addr):
        i = bisect.bisect_right(self._starts, addr) - 1
        if i >= 0 and addr < self.rows[i][1]:
            return self.rows[i][3]
        return None

    __call__ = lookup

    def with_ranges(self, ranges):
        """A copy where each (start, end, target|None, source) replaces what the table had there."""
        rows = list(self.rows)
        for start, end, target, source in ranges:
            cut = []
            for a, b, t, s in rows:
                if b <= start or a >= end:
                    cut.append((a, b, t, s))
                    continue
                if a < start:
                    cut.append((a, start, t, s))
                if b > end:
                    cut.append((end, b, t + (end - a), s))
            if target is not None:
                cut.append((start, end, target, source))
            rows = cut
        return AddressTable(rows)

    def is_identity(self):
        return all(t == a for a, _b, t, _s in self.rows)

    def check_injective(self):
        tgt = sorted((t, t + (b - a), a) for a, b, t, _s in self.rows)
        for x, y in zip(tgt, tgt[1:]):
            if x[1] > y[0]:
                raise SystemExit(f"two E1 ranges land on the same addresses: 0x{x[2]:08X} and 0x{y[2]:08X}")


def heap_start():
    """Where E1's static image ends: main.dol, the linked RELs and the p1_character stubs. Above it
    is heap and arena, laid out at run time, where the address map's offsets mean nothing; only
    overrides and computed values map addresses there."""
    import p1_character
    return p1_character.CARD_PORTRAIT + p1_character.stub_size()


def address_table(variant, map_path=None, computed=(), overrides=True):
    """The variant's table. computed: (E1 start, target start, size, what) values worked out from
    the binaries (the p1_character stub area, the MEM1 arena end); they win over everything.
    overrides=False leaves overrides.tsv out (the plain address map, for the function map)."""
    if variant["name"] == CANONICAL:
        return AddressTable.identity()
    if map_path is not None or nsmbw_versions._state["blocks"] is None:
        nsmbw_versions.load(map_path)
    block = variant["address_map_block"]
    limit = heap_start()
    rows = [(a, min(b, limit), t, "map") for a, b, t in nsmbw_versions.range_table(CANONICAL, block) if a < limit]
    table = AddressTable(rows)
    extra = [(o.start, o.end, o.target, f"override:{o.reason}") for o in read_overrides(variant)] if overrides else []
    extra += [(start, start + size, target, f"computed:{what}") for start, target, size, what in computed]
    table = table.with_ranges(extra)
    table.check_injective()
    return table
