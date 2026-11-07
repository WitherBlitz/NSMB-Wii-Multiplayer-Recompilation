"""Every guest address the runtime's C++ uses, for gen_variant.py and verify_variant.py.

Scans runtime/src and runtime/include (not third_party) the way the compiler sees them: lines in an
`#if 0` branch are skipped up to its #else/#elif/#endif (the translator's NativeSourceParsing does
the same), and comments are kept apart. Each site is one of:
  hook:<MACRO>    a registration or override macro's address (PPC_NATIVE_OVERRIDE[_VOID],
                  REGISTER_NATIVE_FUNCTION[_AS], REGISTER_TRANSLATED_FUNCTION, MKW_KNOWN_TYPED_NATIVE,
                  MKW_KNOWN_NATIVE_CPU_CALL, GX_FATAL_STUB); `token` is the bare hex spelling the
                  macro pastes (PPC_NATIVE_OVERRIDE(801A1138, ...)), or "" for a 0x literal
  named-constant  0x8xxxxxxx assigned to a named constant
  inline-literal  any other 0x8xxxxxxx in code
  lowmem-literal  0x80000000-0x80003FFF (the same on every version)
  comment-only    an address in a comment
"""
import re
from pathlib import Path

LOWMEM_END = 0x80004000
SUFFIXES = (".cpp", ".h", ".hpp", ".inc", ".c", ".mm")
HOOK = re.compile(r"\b(PPC_NATIVE_OVERRIDE(?:_VOID)?|REGISTER_NATIVE_FUNCTION(?:_AS)?|REGISTER_TRANSLATED_FUNCTION|"
                  r"MKW_KNOWN_TYPED_NATIVE|MKW_KNOWN_NATIVE_CPU_CALL|GX_FATAL_STUB)\s*\(\s*(0x)?([0-9A-Fa-f]{8})u?\b")
LIT = re.compile(r"\b0x(8[01][0-9A-Fa-f]{6})u?\b", re.I)
CONST = re.compile(r"(?:constexpr|const|static|#define)\b[^=;()]*?\b(k\w+|[A-Z_][A-Z0-9_]{3,})\s*(?:=|\s)\s*0x(8[01][0-9A-Fa-f]{6})", re.I)


def source_files(runtime_dir):
    runtime_dir = Path(runtime_dir)
    files = sorted(list((runtime_dir / "src").rglob("*")) + list((runtime_dir / "include").rglob("*")))
    return [f for f in files if f.suffix in SUFFIXES and "third_party" not in f.parts and f.is_file()]


def live_lines(text):
    """(line number, line) for every line the compiler sees past `#if 0` branches."""
    frames = []  # (zero_if, active)
    for n, line in enumerate(text.splitlines(), 1):
        s = line.lstrip()
        visible_before = all(active for _z, active in frames)
        if s.startswith("#"):
            d = s[1:].lstrip()
            if d.startswith(("ifdef", "ifndef")):
                frames.append((False, True))
            elif d.startswith("if"):
                cond = d[2:].lstrip()
                zero = cond.startswith("0") and (len(cond) == 1 or not (cond[1].isalnum() or cond[1] == "_"))
                frames.append((zero, not zero))
            elif d.startswith(("else", "elif")) and frames:
                if frames[-1][0]:
                    frames[-1] = (True, True)  # the branch after "#if 0" is live
            elif d.startswith("endif") and frames:
                frames.pop()
        if visible_before and all(active for _z, active in frames):
            yield n, line


def scan(runtime_dir):
    runtime_dir = Path(runtime_dir)
    rows = []
    for f in source_files(runtime_dir):
        rel = str(f.relative_to(runtime_dir.parent)).replace("\\", "/")
        in_block_comment = False
        for ln, line in live_lines(f.read_text(encoding="utf-8", errors="replace")):
            s = line.strip()
            code, comment = line, ""
            if in_block_comment:
                if "*/" in line:
                    in_block_comment = False
                    comment, code = line.split("*/", 1)
                else:
                    comment, code = line, ""
            if "/*" in code and "*/" not in code.split("/*", 1)[1]:
                code, c2 = code.split("/*", 1)
                comment += c2
                in_block_comment = True
            if "//" in code:
                code, c2 = code.split("//", 1)
                comment += c2
            code = re.sub(r'"(?:[^"\\]|\\.)*"', '""', code)  # string literals hold no addresses
            hooked = set()
            for m in HOOK.finditer(code):
                a = int(m.group(3), 16)
                hooked.add(m.start(3))
                rows.append(dict(file=rel, line=ln, addr=a, kind="hook:" + m.group(1),
                                 token="" if m.group(2) else m.group(3), name="", code=s[:160]))
            consts = {int(m.group(2), 16): m.group(1) for m in CONST.finditer(code)}
            for m in LIT.finditer(code):
                if m.start(1) in hooked:
                    continue
                a = int(m.group(1), 16)
                if a < LOWMEM_END:
                    kind = "lowmem-literal"
                elif a in consts:
                    kind = "named-constant"
                else:
                    kind = "inline-literal"
                rows.append(dict(file=rel, line=ln, addr=a, kind=kind, token="", name=consts.get(a, ""), code=s[:160]))
            for m in LIT.finditer(comment):
                rows.append(dict(file=rel, line=ln, addr=int(m.group(1), 16), kind="comment-only", token="",
                                 name="", code=s[:160]))
    return rows
