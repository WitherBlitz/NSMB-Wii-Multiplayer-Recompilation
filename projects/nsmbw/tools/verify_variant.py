"""The build-time proof that the runtime, written against USA rev 1 (E1) addresses, is right on
another game version.

    python verify_variant.py <variant> [--map <address-map.txt>] [--game <dir>]
                             [--canonical-game <dir>] [--out <dir>]

--game is the variant's unpack_game.py folder (default: its game_dir next to this repository) or a
Dolphin extract, read in memory; --canonical-game the same for E1 (default "game"). Nothing is
written except the reports in --out (default: the current folder).

Checks, each a build failure unless the variant's overrides.tsv gives the address a reason:
  1. every function in MAP.txt is compared word by word with the variant's code at its mapped
     address, both images linked at their heap addresses; differences the address map explains
     (b/bl targets, r13/r2-relative and lis-built absolute addresses) count as equivalent
  2. every hook (registration/override macro) lands on an identical or equivalent function
  3. every runtime address maps; a code address lies in identical/equivalent code; an
     initialized-data constant has the same first 16 bytes (pointers compared through the map)
  4. every word p1_character.py patches holds E1's instruction, inside identical/equivalent code
  5. the REL chain matches the address map, and vi.cpp's expected module table maps onto it
  6. the SDA bases come from the variant's main.dol and agree with its __init_registers
Reports: function_equivalence_<v>.csv, hooks_<v>.csv, runtime_addresses_<v>.csv,
feature_coverage_<v>.csv (what still needs a playtest) and verify_<v>.txt.
"""
import bisect
import csv
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

import p1_character
import prelink_rels
import runtime_scan
import variants
from variant_image import Image, s16

MAX_WORDS = 0x4000
PROVEN = ("identical", "equivalent")
GPR_LOADS = {32, 34, 40, 42, 46, 14, 15, 7, 8, 12, 13}   # write rD (bits 21-25)
UPDATE_FORMS = {33, 35, 41, 43, 37, 39, 45, 49, 51, 53, 55, 57, 61}  # write rA (load: also rD)
LOGICAL_IMM = {20, 21, 23, 24, 25, 26, 27, 28, 29}        # write rA (bits 16-20)
NO_GPR = {4, 10, 11, 16, 17, 18, 19, 36, 38, 44, 47, 48, 50, 52, 54, 56, 59, 60, 63}


def gpr_writes(w):
    """The GPRs an instruction may write (op 31 and unknown opcodes: both register fields)."""
    op, rt, ra = w >> 26, (w >> 21) & 31, (w >> 16) & 31
    if op in NO_GPR:
        return ()
    if op in GPR_LOADS:
        return (rt,)
    if op in UPDATE_FORMS:
        return (ra, rt) if op in (33, 35, 41, 43) else (ra,)
    if op in LOGICAL_IMM:
        return (ra,)
    return (rt, ra)


# ---------------------------------------------------------------- function equivalence
def compare(e1, tg, m, sda_e, sda_t, e_start, t_start, nwords):
    """-> (status, n_equal, n_explained, n_real, first_real_offset)."""
    hi_e, hi_t = {}, {}       # register -> high half a lis put there (E1, target)
    pending = {}              # register -> index of a lis whose words differ, until a use explains it
    verdict = [None] * nwords
    same = [False] * nwords
    (e13, e2), (t13, t2) = sda_e, sda_t

    def same_place(ae, at):
        """An address inside E1's image must land on its mapped address. Anything else (hardware
        registers, heap, plain constants such as DSP mails) is only the same as itself."""
        return m(ae) == at or (e1.kind(ae) is None and ae == at)

    def forget(reg):
        hi_e.pop(reg, None)
        hi_t.pop(reg, None)
        if reg in pending:
            verdict[pending.pop(reg)] = False  # a moved high half nothing used

    for i in range(nwords):
        ea, ta = e_start + 4 * i, t_start + 4 * i
        we, wt = e1.word(ea), tg.word(ta)
        if we is None or wt is None:
            return ("out-of-image", 0, 0, 0, i * 4)
        same[i] = we == wt
        op_e, op_t = we >> 26, wt >> 26
        rt, ra = (we >> 21) & 31, (we >> 16) & 31
        ok = None
        if op_e == 18 and op_t == 18 and we & 3 == wt & 3:
            de, dt = we & 0x03FFFFFC, wt & 0x03FFFFFC
            de, dt = de - (de & 0x02000000) * 2, dt - (dt & 0x02000000) * 2
            te = (de if we & 2 else ea + de) & 0xFFFFFFFF
            tt = (dt if wt & 2 else ta + dt) & 0xFFFFFFFF
            ok = m(te) == tt
        elif same[i]:
            ok = True
        if op_e == 15 and op_t == 15 and ra == 0 and (we >> 16) == (wt >> 16):
            forget(rt)
            hi_e[rt], hi_t[rt] = (we & 0xFFFF) << 16, (wt & 0xFFFF) << 16
            if ok is None:
                pending[rt] = i
                ok = "pending"
        elif op_e == op_t and (we >> 16) == (wt >> 16) and (op_e in (14, 24) or 32 <= op_e <= 55):
            base = rt if op_e == 24 else ra  # ori rA, rS, imm reads rS
            if op_e == 24:
                ie, it = we & 0xFFFF, wt & 0xFFFF
            else:
                ie, it = s16(we & 0xFFFF), s16(wt & 0xFFFF)
            # The address is checked even where the words are the same: identical code that names
            # an address which moved by a different amount reaches a different object.
            if base in hi_e:
                ae = (hi_e[base] | ie) if op_e == 24 else (hi_e[base] + ie) & 0xFFFFFFFF
                at = (hi_t[base] | it) if op_e == 24 else (hi_t[base] + it) & 0xFFFFFFFF
                addr_ok = same_place(ae, at)
                ok = addr_ok if ok is None else ok and addr_ok
                if base in pending:
                    verdict[pending.pop(base)] = addr_ok
            elif op_e != 24 and base in (13, 2):
                eb, tb = (e13, t13) if base == 13 else (e2, t2)
                addr_ok = same_place((eb + ie) & 0xFFFFFFFF, (tb + it) & 0xFFFFFFFF)
                ok = addr_ok if ok is None else ok and addr_ok
            for reg in gpr_writes(we):
                forget(reg)
        else:
            for reg in gpr_writes(we):
                forget(reg)
        verdict[i] = ok
    for reg in list(pending):
        forget(reg)
    eq = expl = real = 0
    first = None
    for i, ok in enumerate(verdict):
        if ok is True and same[i]:
            eq += 1
        elif ok is True:
            expl += 1
        else:
            real += 1
            if first is None:
                first = i * 4
    status = "identical" if expl == 0 and real == 0 else ("equivalent" if real == 0 else "differs")
    return (status, eq, expl, real, first)


def read_function_map(path):
    starts, names = [], {}
    for line in Path(path).read_text().splitlines():
        if line.strip() and not line.startswith("#"):
            a, name = line.split(None, 1) if " " in line.strip() else (line.strip(), "")
            starts.append(int(a, 16))
            names[int(a, 16)] = name.strip()
    return sorted(set(starts)), names


def equivalence(e1, tg, m, sda_e, sda_t, starts, names):
    results, rows = {}, []
    for idx, s in enumerate(starts):
        if e1.kind(s) != "text":
            continue
        nxt = starts[idx + 1] if idx + 1 < len(starts) else s + 4
        n = max(1, min((nxt - s) // 4, MAX_WORDS))
        ts = m(s)
        if ts is None:
            st = ("unmapped", 0, 0, 0, None)
        else:
            te = m(s + 4 * n - 4)
            if te is None or te - ts != 4 * n - 4:
                st = ("length/map-split", 0, 0, 0, None)
            else:
                st = compare(e1, tg, m, sda_e, sda_t, s, ts, n)
        results[s] = st
        rows.append(dict(e1=f"0x{s:08X}", target="" if ts is None else f"0x{ts:08X}", words=n, status=st[0],
                         equal=st[1], explained=st[2], real=st[3],
                         first_real_off="" if st[4] is None else hex(st[4]), name=names.get(s, "")))
    return results, rows


# ---------------------------------------------------------------- the proof
class Proof:
    def __init__(self):
        self.failures, self.notes = [], []

    def fail(self, msg):
        self.failures.append(msg)

    def note(self, msg):
        self.notes.append(msg)


def parse_args(argv):
    opts, rest = {}, []
    i = 0
    while i < len(argv):
        if argv[i] in ("--map", "--game", "--canonical-game", "--out"):
            if i + 1 >= len(argv):
                sys.exit(__doc__)
            opts[argv[i]] = argv[i + 1]
            i += 2
        else:
            rest.append(argv[i])
            i += 1
    if len(rest) != 1:
        sys.exit(__doc__)
    return rest[0], opts


def load_images(variant, opts):
    e1_game = Path(opts.get("--canonical-game", variants.REPO.parent / "game"))
    game = Path(opts["--game"]) if "--game" in opts else variants.default_game_dir(variant)
    e1_dol, e1_rels = prelink_rels.read_unpacked(e1_game)
    tg_dol, tg_rels = prelink_rels.read_unpacked(game)
    return (e1_dol, e1_rels), (tg_dol, tg_rels)


MEM1_END = 0x81800000


def arena_hi(variant):
    """The game's MEM1 arena end: MEM1 end minus the disc's FST, 32-aligned (system_bridge.cpp)."""
    return MEM1_END - prelink_rels.align_up(variant["fst_size"], 0x20)


def computed_values(variant, target_image):
    """What the address map can't give, worked out from the binaries, for variants.address_table:
    p1_character's stub section (where prelink_rels.py puts it) and the MEM1 arena end."""
    e1 = variants.get(variants.CANONICAL)
    return [(p1_character.CARD_PORTRAIT, prelink_rels.align_up(target_image.linked.end, 0x20),
             p1_character.stub_size(), "p1_character stub area (prelink_rels.py)"),
            (arena_hi(e1), arena_hi(variant), 1, "MEM1 arena hi = MEM1 end - the disc's FST")]


def run(variant_name, opts, log=print):
    variant = variants.get(variant_name)
    if variant_name == variants.CANONICAL:
        log("E1 is the canonical address space: its table is the identity; nothing to prove")
        return 0
    proof = Proof()
    (e1_dol, e1_rels), (tg_dol, tg_rels) = load_images(variant, opts)
    base_table = variants.address_table(variant, opts.get("--map"))
    e1 = Image(e1_dol, e1_rels)
    tg = Image(tg_dol, tg_rels, base_table)
    computed = computed_values(variant, tg)
    stubs = computed[0][:3]
    table = variants.address_table(variant, None, computed)
    m = table.lookup
    heap = variants.heap_start()
    out = Path(opts.get("--out", "."))
    out.mkdir(parents=True, exist_ok=True)
    v = variant_name
    summary = []

    # 6. SDA bases and entry
    sda_e, sda_t = e1.dol.sda_bases(), tg.dol.sda_bases()
    for label, img, sda in (("E1", e1, sda_e), (v, tg, sda_t)):
        init = img.dol.init_registers()
        if (init.get(13), init.get(2)) != sda:
            proof.fail(f"{label}: __init_registers sets r13 {init.get(13)} r2 {init.get(2)}, the DOL sections say "
                       f"0x{sda[0]:08X} / 0x{sda[1]:08X}")
    via_map = (base_table.lookup(sda_e[0]), base_table.lookup(sda_e[1]))
    summary.append(f"SDA bases: E1 r13 0x{sda_e[0]:08X} r2 0x{sda_e[1]:08X}; {v} r13 0x{sda_t[0]:08X} "
                   f"r2 0x{sda_t[1]:08X} (from main.dol, agrees with __init_registers)")
    for reg, mapped, real in (("r13", via_map[0], sda_t[0]), ("r2", via_map[1], sda_t[1])):
        if mapped != real:
            summary.append(f"  {reg}: the address map would give "
                           f"{'nothing' if mapped is None else f'0x{mapped:08X}'}; corrected to 0x{real:08X} "
                           f"(_SDA_BASE_ is a linker symbol the map does not cover)")
    summary.append(f"entry: E1 0x{e1.dol.entry:08X}, {v} 0x{tg.dol.entry:08X}")

    # 5. REL chain
    layout = tg.rel_layout()
    e1_layout = e1.rel_layout()
    for name, (image, bss, end) in e1_layout.items():
        t_image, t_bss, _t_end = layout[name]
        if m(image) != t_image:
            proof.fail(f"REL {name}: chained to 0x{t_image:08X}, the address map predicts {m(image)}")
        if m(bss) != t_bss:
            proof.fail(f"REL {name}: bss 0x{t_bss:08X}, the address map predicts {m(bss)}")
    vi = variants.REPO / "runtime" / "src" / "hle" / "vi.cpp"
    expected = re.findall(r'\{(\d), "(\w+)", 0x([0-9A-Fa-f]{8})u, 0x([0-9A-Fa-f]{8})u\}', vi.read_text(encoding="utf-8"))
    if len(expected) != 4:
        proof.fail("vi.cpp: LogGuestModulesOnce's kExpected table not found (4 modules)")
    for _id, name, image, bss in expected:
        if m(int(image, 16)) != layout[name][0] or m(int(bss, 16)) != layout[name][1]:
            proof.fail(f"vi.cpp kExpected {name}: 0x{image}/0x{bss} do not map onto the chained layout")
    summary.append("REL images: " + ", ".join(f"{n} 0x{layout[n][0]:08X} (bss 0x{layout[n][1]:08X})" for n in layout) +
                   f"; p1_character stubs 0x{stubs[1]:08X}+0x{stubs[2]:X} (E1 0x{stubs[0]:08X})")
    summary.append(f"MEM1 arena hi: E1 0x{computed[1][0]:08X}, {v} 0x{computed[1][1]:08X} (FST 0x{variant['fst_size']:X} bytes)")

    # 1. function equivalence
    starts, names = read_function_map(variants.PROJECT / "MAP.txt")
    results, frows = equivalence(e1, tg, m, sda_e, sda_t, starts, names)
    write_csv(out / f"function_equivalence_{v}.csv", frows)
    c_dol = Counter(r["status"] for r in frows if int(r["e1"], 16) < e1.linked.base)
    c_rel = Counter(r["status"] for r in frows if int(r["e1"], 16) >= e1.linked.base)
    summary.append(f"functions in MAP.txt: {len(frows)}; main.dol {dict(c_dol)}; RELs {dict(c_rel)}")
    proven_total = sum(1 for r in frows if r["status"] in PROVEN)
    summary.append(f"  identical or equivalent: {proven_total} ({100.0 * proven_total / len(frows):.2f} %)")
    write_csv(out / f"functions_not_equivalent_{v}.csv", [r for r in frows if r["status"] not in PROVEN])

    def containing(a):
        i = bisect.bisect_right(starts, a) - 1
        while i >= 0 and starts[i] not in results:
            i -= 1
        return (starts[i], results[starts[i]][0]) if i >= 0 else (None, None)

    overrides = {o.start: o for o in variants.read_overrides(variant)}

    def override_for(a):
        for o in overrides.values():
            if o.start <= a < o.end:
                return o
        return None

    in_stubs = lambda a: stubs[0] <= a < stubs[0] + stubs[2]  # noqa: E731

    # 2 + 3. runtime addresses
    rows = runtime_scan.scan(variants.REPO / "runtime")
    arows, hooks = [], {}
    for r in rows:
        if r["kind"] == "comment-only":
            continue
        a = r["addr"]
        target = m(a)
        o = override_for(a)
        kind_e1 = e1.kind(a)
        verdict, detail = "proven", ""
        if r["kind"] == "lowmem-literal":
            detail = "low memory (the same on every version)"
        elif o is not None and o.target is None:
            verdict, detail = "waived", f"override none: {o.reason}"
        elif target is None and a >= heap:
            verdict, detail = "FAIL", ("heap/arena address: the address map does not apply there; give it a "
                                       "per-version value (an override or a computed value)")
        elif target is None:
            verdict, detail = "FAIL", "no mapping"
        elif a >= heap:
            src = table.source(a)
            verdict, detail = "explained", src.replace("override:", "override: ").replace("computed:", "computed: ")
        elif in_stubs(a):
            verdict, detail = "explained", "p1_character stub area (computed by prelink; patch sites checked below)"
        elif r["kind"].startswith("hook:"):
            st = results.get(a, (None,))[0]
            if st in PROVEN:
                detail = st
            elif o is not None:
                verdict, detail = "explained", f"override: {o.reason}"
            else:
                verdict, detail = "FAIL", f"hook on a function that is {st or 'not a MAP.txt start'}"
        elif kind_e1 == "text":
            start, st = containing(a)
            if st in PROVEN:
                detail = f"code inside {st} 0x{start:08X}"
            elif o is not None:
                verdict, detail = "explained", f"override: {o.reason}"
            else:
                verdict, detail = "FAIL", f"code address inside 0x{start:08X}, which {st}"
        elif kind_e1 == "data":
            same = True
            for k in range(4):
                we, wt = e1.word(a + 4 * k), tg.word(target + 4 * k)
                if we is None or wt is None:
                    break
                if we != wt and not (0x80000000 <= we < 0x81800000 and m(we) == wt):
                    same = False
            if same:
                detail = "initialized data: first 16 bytes match (pointers through the map)"
            elif o is not None:
                verdict, detail = "explained", f"override: {o.reason}"
            else:
                verdict, detail = "FAIL", "initialized data differs"
            r = dict(r, data_constant=same)
        else:
            verdict, detail = "mapped", f"{kind_e1 or 'heap/arena'}: address maps (contents not checkable statically)"
        row = dict(file=r["file"], line=r["line"], kind=r["kind"], e1=f"0x{a:08X}",
                   target="" if target is None else f"0x{target:08X}", verdict=verdict, detail=detail,
                   code=r["code"])
        arows.append(row)
        if r["kind"].startswith("hook:"):
            hooks.setdefault(a, row)
        if verdict == "FAIL":
            proof.fail(f"{r['file']}:{r['line']} 0x{a:08X}: {detail}")
        if "data_constant" in r:
            row["data_constant"] = "match" if r["data_constant"] else "differs"
    write_csv(out / f"runtime_addresses_{v}.csv", arows)
    write_csv(out / f"hooks_{v}.csv", [r for r in arows if r["kind"].startswith("hook:")])
    hv = Counter(r["verdict"] for r in hooks.values())
    hd = Counter(r["detail"] for r in hooks.values() if r["verdict"] == "proven")
    summary.append(f"hooks: {sum(1 for r in arows if r['kind'].startswith('hook:'))} sites, {len(hooks)} addresses: "
                   f"{hv.get('proven', 0)} proven ({hd.get('identical', 0)} identical, {hd.get('equivalent', 0)} "
                   f"equivalent), {hv.get('explained', 0)} explained, {hv.get('waived', 0)} waived, "
                   f"{hv.get('FAIL', 0)} failed")
    for a, r in sorted(hooks.items()):
        if r["verdict"] != "proven":
            summary.append(f"  0x{a:08X} -> {r['target'] or '-'}: {r['verdict']}: {r['detail']}")
    consts = {}
    for r in arows:
        if "data_constant" in r:
            consts[r["e1"]] = r["data_constant"]
    in_dol = sum(1 for a in consts if int(a, 16) < e1.linked.base)
    summary.append(f"initialized-data constants: {len(consts)} distinct ({in_dol} in main.dol, "
                   f"{len(consts) - in_dol} in the RELs), {sum(1 for x in consts.values() if x == 'match')} match")
    lit = Counter(r["verdict"] for r in arows if not r["kind"].startswith("hook:"))
    summary.append(f"other runtime address sites: {dict(lit)}")
    for r in arows:
        if r["verdict"] in ("waived", "explained") and not r["kind"].startswith("hook:"):
            summary.append(f"  {r['file']}:{r['line']} {r['e1']}: {r['verdict']}: {r['detail']}")

    # 4. p1_character patch sites
    problems = p1_character.check_sites(e1.word, tg.word, m)
    for site in p1_character.patch_sites():
        start, st = containing(site)
        if st not in PROVEN:
            problems.append(f"0x{site:08X} lies in 0x{start:08X}, which {st}")
    for p in problems:
        proof.fail(f"p1_character: {p}")
    summary.append(f"p1_character: {len(p1_character.patch_sites())} patch sites, {len(problems)} problems")

    # Coverage per runtime file (the playtest list)
    per = defaultdict(lambda: dict(points=0, proven=0, mapped_only=0, review=[]))
    for r in arows:
        f = per[r["file"]]
        f["points"] += 1
        if r["verdict"] in ("proven", "explained"):
            f["proven"] += 1
        elif r["verdict"] == "mapped":
            f["mapped_only"] += 1
        else:
            f["review"].append(f"{r['e1']} {r['detail']}")
    crows = []
    for name, x in sorted(per.items()):
        status = "review" if x["review"] else ("proven" if x["mapped_only"] == 0 else "proven code; data mapped")
        crows.append(dict(file=name, touch_points=x["points"], code_proven=x["proven"], data_mapped=x["mapped_only"],
                          status=status, review="; ".join(x["review"])))
    write_csv(out / f"feature_coverage_{v}.csv", crows)
    summary.append("runtime files: " + str(dict(Counter(r["status"] for r in crows))))

    summary.append("RESULT: " + ("PROVEN" if not proof.failures else f"FAILED ({len(proof.failures)})"))
    summary += [f"FAIL {x}" for x in proof.failures]
    text = "\n".join(summary) + "\n"
    (out / f"verify_{v}.txt").write_text(text, encoding="utf-8")
    log(text.rstrip())
    return 1 if proof.failures else 0


def write_csv(path, rows):
    keys = []
    for r in rows:
        for k in r:
            if k not in keys:
                keys.append(k)
    with path.open("w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=keys or ["empty"])
        w.writeheader()
        w.writerows(rows)


def main():
    name, opts = parse_args(sys.argv[1:])
    sys.exit(run(name, opts))


if __name__ == "__main__":
    main()
