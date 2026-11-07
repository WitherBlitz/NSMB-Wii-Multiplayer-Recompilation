"""Convert NSMBW addresses between game versions using NSMBW-Updated's address-map.txt.

Semantics follow Kamek's address mapper: a version block lists ranges over its parent's address
space ("extend X"); an address is first mapped through the parent, then shifted by the first range
containing it, and passed through unchanged when no range contains it.

The map's author treats addresses outside every range as non-portable, so the strict forms
(strict_forward, strict_inverse, convert(..., strict=True), range_table) give None for them instead
of passing them through. The variant tools (gen_variant.py, verify_variant.py, prelink_rels.py) use
only the strict forms.

Where the map is: load(path), else the NSMBW_ADDRESS_MAP environment variable, else the copy
vendored in projects/nsmbw/variants/ (MIT, see LICENSE-address-map.txt there).
"""
import os
import re
from functools import lru_cache
from pathlib import Path

DEFAULT_MAP_PATH = Path(__file__).resolve().parents[1] / "variants" / "address-map.txt"
MEM1 = (0x80000000, 0x81800000)  # the window range_table covers (end exclusive)

_state = {"path": None, "blocks": None}


def map_path():
    if _state["path"] is not None:
        return _state["path"]
    env = os.environ.get("NSMBW_ADDRESS_MAP")
    return Path(env) if env else DEFAULT_MAP_PATH


def load(path=None):
    """(Re)loads the map from path, or from map_path(). Returns the parsed blocks."""
    if path is not None:
        _state["path"] = Path(path)
    p = map_path()
    if not p.is_file():
        raise SystemExit(f"address map not found: {p} (pass --map, or set NSMBW_ADDRESS_MAP)")
    _state["blocks"] = _parse(p.read_text())
    inverse.cache_clear()
    strict_inverse.cache_clear()
    _pieces.cache_clear()
    return _state["blocks"]


def _parse(text):
    blocks, cur = {}, None
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        m = re.fullmatch(r"\[(\w+)\]", line)
        if m:
            cur = blocks.setdefault(m.group(1), {"parent": None, "ranges": []})
            continue
        if cur is None:
            continue
        if line.startswith("extend"):
            cur["parent"] = line.split()[1]
            continue
        m = re.fullmatch(r"([0-9a-fA-F]+)-([0-9a-fA-F]+|\*)\s*:\s*([+-])0x([0-9a-fA-F]+)", line)
        if m:
            start = int(m.group(1), 16)
            end = 0xFFFFFFFF if m.group(2) == "*" else int(m.group(2), 16)
            delta = int(m.group(4), 16) * (1 if m.group(3) == "+" else -1)
            cur["ranges"].append((start, end, delta))
    return blocks


def _blocks():
    if _state["blocks"] is None:
        load()
    return _state["blocks"]


def __getattr__(name):  # BLOCKS stays readable for older callers
    if name == "BLOCKS":
        return _blocks()
    raise AttributeError(name)


def versions():
    return sorted(_blocks())


def forward(version, p1):
    """PAL v1 address -> address in `version`."""
    if version == "P1":
        return p1
    block = _blocks()[version]
    a = forward(block["parent"], p1) if block["parent"] else p1
    for start, end, delta in block["ranges"]:
        if start <= a <= end:
            return (a + delta) & 0xFFFFFFFF
    return a


def strict_forward(version, p1):
    """As forward, but None when the address falls in no explicit range at any level."""
    if version == "P1":
        return p1
    block = _blocks()[version]
    a = strict_forward(block["parent"], p1) if block["parent"] else p1
    if a is None:
        return None
    for start, end, delta in block["ranges"]:
        if start <= a <= end:
            return (a + delta) & 0xFFFFFFFF
    return None


@lru_cache(maxsize=None)
def inverse(version, addr):
    """Address in `version` -> PAL v1 address, or None if it has no unambiguous preimage."""
    if version == "P1":
        return addr
    block = _blocks()[version]
    candidates = [addr - d for s, e, d in block["ranges"] if s <= addr - d <= e]
    if not any(s <= addr <= e for s, e, _ in block["ranges"]):
        candidates.append(addr)  # uncovered addresses pass through unchanged
    results = set()
    for parent_addr in candidates:
        p1 = inverse(block["parent"], parent_addr) if block["parent"] else parent_addr
        if p1 is not None and forward(version, p1) == addr:
            results.add(p1)
    if len(results) > 1:
        # e.g. [C] folds 0x9xxxxxxx down onto MEM1; code and static data live in MEM1.
        results = {r for r in results if 0x80000000 <= r < 0x81800000}
    return results.pop() if len(results) == 1 else None


@lru_cache(maxsize=None)
def strict_inverse(version, addr):
    """Address in `version` -> PAL v1 address through explicit ranges only, or None."""
    if version == "P1":
        return addr
    block = _blocks()[version]
    results = set()
    for s, e, d in block["ranges"]:
        parent_addr = (addr - d) & 0xFFFFFFFF
        if not s <= parent_addr <= e:
            continue
        p1 = strict_inverse(block["parent"], parent_addr) if block["parent"] else parent_addr
        if p1 is not None and strict_forward(version, p1) == addr:
            results.add(p1)
    return results.pop() if len(results) == 1 else None


def convert(src, dst, addr, strict=False):
    if strict:
        p1 = strict_inverse(src, addr)
        return None if p1 is None else strict_forward(dst, p1)
    p1 = inverse(src, addr)
    return None if p1 is None else forward(dst, p1)


# ---------------------------------------------------------------- piecewise tables
def _subtract(intervals, lo, hi):
    """intervals minus [lo, hi] (all inclusive)."""
    out = []
    for a, b in intervals:
        if b < lo or a > hi:
            out.append((a, b))
            continue
        if a < lo:
            out.append((a, lo - 1))
        if b > hi:
            out.append((hi + 1, b))
    return out


@lru_cache(maxsize=None)
def _pieces(version):
    """strict_forward(version) as [(p1_lo, p1_hi, delta)] (inclusive, sorted, disjoint)."""
    if version == "P1":
        return ((0, 0xFFFFFFFF, 0),)
    block = _blocks()[version]
    parent = _pieces(block["parent"]) if block["parent"] else ((0, 0xFFFFFFFF, 0),)
    out = []
    for lo, hi, d in parent:
        # The parent piece covers [lo+d, hi+d] in the parent's space; the first containing range wins.
        free = [(lo + d, hi + d)]
        for s, e, dr in block["ranges"]:
            for a, b in free:
                x, y = max(a, s), min(b, e)
                if x <= y:
                    out.append((x - d, y - d, d + dr))
            free = _subtract(free, s, e)
            if not free:
                break
    out.sort()
    return tuple(out)


def range_table(src, dst, window=MEM1):
    """The strict src -> dst mapping as [(src_start, src_end_exclusive, dst_start)], sorted and
    disjoint, inside window. Addresses of src that have no single strict preimage in P1, or whose
    P1 address has no strict image in dst, are left out (unmapped)."""
    lo_w, hi_w = window[0], window[1] - 1
    src_p, dst_p = _pieces(src), _pieces(dst)
    # src space intervals and the P1 address they come from
    cand = []
    for plo, phi, ds in src_p:
        for qlo, qhi, dd in dst_p:
            x, y = max(plo, qlo), min(phi, qhi)
            if x > y:
                continue
            a, b = x + ds, y + ds  # in src space
            a2, b2 = max(a, lo_w), min(b, hi_w)
            if a2 > b2:
                continue
            cand.append((a2, b2, x + (a2 - a) + dd))
    # Two P1 ranges landing on the same src address make it ambiguous: drop every overlap.
    cand.sort()
    bounds = sorted({a for a, _b, _t in cand} | {b + 1 for _a, b, _t in cand})
    out = []
    for i in range(len(bounds) - 1):
        a, b = bounds[i], bounds[i + 1] - 1
        hits = [(s, t) for s, e, t in cand if s <= a and b <= e]
        if len(hits) != 1:
            continue
        s, t = hits[0]
        tgt = t + (a - s)
        if out and out[-1][1] == a and out[-1][2] + (out[-1][1] - out[-1][0]) == tgt:
            out[-1] = (out[-1][0], b + 1, out[-1][2])
        else:
            out.append((a, b + 1, tgt))
    return out


if __name__ == "__main__":
    for name, c_addr in [("OSLink", 0x801B4280), ("OSCreateThread", 0x801B7490), ("GXBegin", 0x801C78D0),
                         ("ARCOpen", 0x801A1A60), ("memcpy", 0x80004364)]:
        p1 = inverse("C", c_addr)
        print(f"{name:16} C 0x{c_addr:08X} -> P1 0x{p1:08X} -> E1 0x{forward('E1', p1):08X}")
