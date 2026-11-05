"""Player 1 can pick any character, and players 2-4 can pick Mario: mkwcat's Gecko code
"Player 1 Can Change Character" (https://github.com/mkwcat/gecko-codes, nsmbw/, USA Rev 1), applied
offline for the translator the way newer_prelink.py applies Newer's hooks.

A Gecko code handler patches memory at boot; here the code it patches is only ever read by the
translator, so:
- 04 / 02 writes go into main.dol and the synthetic REL.
- C2 (insert code) becomes `b stub`, with the stub in a code section of its own, ending in
  `b site+4`. The translator takes both branches as tail calls, as it does Newer's hooks.
- C6 (branch) is a plain `b`.
- Writes into game data can't go here: the game loads its data from the disc. The runtime makes
  those (gameplay_options.cpp, PatchCharacterData).

Three stubs are written out by hand, because the originals keep data inside the code (`bl` over a
table, then `mflr`), which a static translation can't run: the two per-character sound tables become
compare chains, and the 2.0f volume is built on the stack. That volume stub also tests cr0 where the
code's `beq cr7` reads whatever cr7 held on the way in: it follows the `cmpwi r0, 0` right before it.
`lis/ori/mtctr/bctrl` calls become direct `bl`s.

The Gecko code is under this license:

MIT License

Copyright (c) 2024 mkwcat

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
"""
import struct

CODE = """
049027F8 3880FFFF 049028F8 60000000 049029A8 60000000
C2902B7C 0000000B 7FE3FB78 3D808090 618C2BD0 7D8903A6 4E800421 3D208035 81294E60 815F0184 808A0004 548416BA
3C848094 8084CDF0 7C092000 40820014 808A01CC 80840004 909F01D0 48000010 814A0184 2C0A0000 4082FFD0 60000000
04902BE8 3BC00000
C2902D58 00000002 30B7FFFF 7CA5B910 60000000 00000000
C2902D6C 00000002 30B7FFFF 7CA5B910 60000000 00000000
C2902E14 00000005 90640294 80840184 90640294 813F0384 3D408035 814A4E60 554A103A 7D26512E 60000000 00000000
C2909410 00000005 80060000 5400103A 81050384 2C080003 41820008 7C882378 7D03012E 38E50008 60000000 00000000
048EB7D8 3CA08094 048EB7E4 38A5CDF0 04902E78 60000000 04902E84 9BBF0124
047A1C8C 60000000
C219A974 00000002 7C0802A6 90810008 60000000 00000000
047B0770 60000000
C2902ED4 00000002 7C0802A6 90610008 60000000 00000000
C2902F20 00000004 3D208035 81294E60 7C09E000 4082000C 81210008 906901D0 81830000 60000000
028EE0D6 00000000 028EE0E2 00000000 02902D1E 00000000 02902D22 00000000 02902D26 00000000 02902D2E 00000000
C6902FC8 80903020
0290302E 00000000 02903032 00000000 0290303A 00000000 027A008A 00000000 027A008E 00000000 027A0192 00000000
027A018E 00000004 027A11F2 00000000 027A1206 00000000 027A14DA 00000000 027A14DE 00000000 027A151A 00000000
027A152E 00000000 027A1536 00000000 027A162A 00000000 027A162E 00000000 027A1636 00000000 027A165A 00000000
027A165E 00000000 027A15CA 00000000 027A15CE 00000000 027A16DA 00000000 027A16DE 00000000 027A171A 00000000
027A172E 00000000 027A1736 00000000 027A17AE 00000000 027A17B2 00000000
047A068C 60000000 047A0694 60000000 047A069C 60000000 04918B28 38000000 040BB164 90E30384 047A0FE8 60000000
C27A0D24 00000002 7C0802A6 80830074 8884029A 60000000
C27A0D40 00000005 2C040000 40820018 38600000 80010014 7C0803A6 38210010 4E800020 38800900 60000000 00000000
C27A0D68 00000004 38600000 2C040000 41820014 80010014 7C0803A6 38210010 4E800020 60000000
047A12A8 60000000
C28C5DFC 00000004 3D208035 81294E60 81430004 554A073F 7D4A4A78 7D4A0034 554AD97F 60000000
04770A8C 2C030004 047AC8CC 38000001 04918B50 60000000 0476E630 2C000004 0476EAB8 2C030004 0476DDE8 60000000
04770988 5400173A 047709A8 60000000 047709B0 540407FE 0476FE4C 5400173A
C276FF0C 00000003 2C000000 40820008 38000004 38800000 60000000 00000000
C276FE84 00000003 2C000000 40820008 38000004 900302E0 60000000 00000000
C277117C 00000003 388003F2 801F02D4 2C000000 40820008 3880031D 60000000
C2771164 00000003 388003F3 801F02D4 2C000000 40820008 3880031E 60000000
C276FDC4 00000003 2C1F0007 40820008 38800000 7FA3EB78 60000000 00000000
C276FDD4 00000003 2C1F0007 40820008 3BE00008 2C1F0000 60000000 00000000
0476FD8C 7FE628AE
C277359C 0000000C 2C040007 41820024 2C040008 40820048 54A0103A 7CA30214 80A50254 880500BB 60000001 980500BB
386302A4 57E0103A 3C808094 3884F520 7CBE0214 80A50274 38C00000 3D80800B 618C3B60 7D8903A6 4E800421 2C040004
60000000 00000000
"""

NOP = 0x60000000
CARD_PORTRAIT = 0x80B8E340  # where the stubs start: the end of the linked RELs, aligned


def _li(rd, value):
    return 0x38000000 | rd << 21 | value & 0xFFFF


def _cmpwi(cr, ra, value):
    return 0x2C000000 | cr << 23 | ra << 16 | value & 0xFFFF


def _by_character(table):
    """r4 = table[r9], r9 the character, leaving cr0 alone."""
    words = [_li(4, table[0])]
    for character in range(1, 4):
        words += [_cmpwi(1, 9, character), 0x40860008, _li(4, table[character])]  # bne cr1, +8
    return words


# Stubs written out by hand (see the top of this file). ("bl", address) is a call.
HAND_STUBS = {
    # The character screen's confirm sound: the character's own (0x324 for Mario when the screen's
    # state at +0x238 is 3, as the game).
    0x807A1C94: [0x3D208035, 0x81294E60] + _by_character((0x323, 0x372, 0x3B9, 0x3FF)) +
                [0x40820010, _cmpwi(0, 9, 0), 0x40820008, _li(4, 0x324), NOP],
    # dStockItem's: the sound of the character at r6.
    0x807B0778: [0x81260000] + _by_character((0x356, 0x39D, 0x3E4, 0x42A)) + [NOP],
    # BasicSound::SetPitch's body, then voices 5 and 7 at twice the volume on a sound that wants it.
    0x8019AA4C: [0xD02300BC, 0x801D00AC, 0x2C000000, 0x4182002C, 0x80010008, 0x2C000005, 0x4182000C, 0x2C000007,
                 0x40820018, 0x3C804000, 0x90810008, 0xC0210008, 0x38800000, ("bl", 0x80267420), NOP],
}

# The code's writes into game data are not here (the runtime makes them: PatchCharacterData): seven
# sound numbers at 0x802F5FC8-0x802F6310 and the 5-byte icon table at 0x80933CF0.


def _encode_b(src, dst, link=False):
    disp = dst - src
    assert -0x2000000 <= disp < 0x2000000, (hex(src), hex(dst))
    return 0x48000000 | (disp & 0x03FFFFFC) | (1 if link else 0)


def _direct_calls(words):
    """lis r12, hi / ori r12, r12, lo / mtctr r12 / bctrl -> a direct call."""
    out = list(words)
    for i in range(len(out) - 3):
        a, b, c, d = out[i:i + 4]
        if isinstance(a, int) and a >> 16 == 0x3D80 and isinstance(b, int) and b >> 16 == 0x618C and \
                c == 0x7D8903A6 and d == 0x4E800421:
            out[i:i + 4] = [NOP, NOP, NOP, ("bl", (a & 0xFFFF) << 16 | b & 0xFFFF)]
    return out


def apply(read32, write32, stub_base):
    """Applies the code through read32(addr) / write32(addr, value); returns the stubs' bytes, to be
    placed at stub_base as a code section."""
    words = [int(w, 16) for w in CODE.split()]
    stubs = bytearray()

    def insert(addr, code):
        at = stub_base + len(stubs)
        write32(addr, _encode_b(addr, at))
        for k, word in enumerate(code + [("b", addr + 4)]):
            if isinstance(word, tuple):
                word = _encode_b(at + 4 * k, word[1], link=word[0] == "bl")
            stubs.extend(struct.pack(">I", word))

    # The stubs' first word: a function of its own that the runtime overrides (CardPortrait in
    # gameplay_options.cpp, at this fixed address). Each card calls it before showing its character
    # (r3 the card, r4 what it shows, r6 which of its two sides): the layout has no portrait of Mario,
    # who could never be picked here before.
    assert stub_base == CARD_PORTRAIT, hex(stub_base)
    stubs.extend(struct.pack(">I", 0x4E800020))  # blr
    insert(0x80773574, [0x7FE6FB78, ("bl", CARD_PORTRAIT), 0x2C040002])  # mr r6, r31; cmpwi r4, 2

    for addr, code in HAND_STUBS.items():
        insert(addr, code)
    i = 0
    while i < len(words):
        head, value = words[i], words[i + 1]
        i += 2
        kind, addr = head >> 24, 0x80000000 | head & 0x01FFFFFF
        if kind == 0x04:
            write32(addr, value)
        elif kind == 0x02:
            shift = 16 if addr & 2 == 0 else 0
            word = addr & ~3
            write32(word, read32(word) & ~(0xFFFF << shift) | (value & 0xFFFF) << shift)
        elif kind == 0xC6:
            write32(addr, _encode_b(addr, value))
        elif kind == 0xC2:
            body = words[i:i + value * 2]
            i += value * 2
            insert(addr, _direct_calls(body[:-1]))
        else:
            raise SystemExit(f"unhandled Gecko code type {kind:02X}")
    return bytes(stubs)


def dol_location(dol, addr):
    """File offset of addr in a main.dol image, or None."""
    offs = struct.unpack_from(">18I", dol, 0x00)
    addrs = struct.unpack_from(">18I", dol, 0x48)
    sizes = struct.unpack_from(">18I", dol, 0x90)
    for off, start, size in zip(offs, addrs, sizes):
        if size and start <= addr < start + size:
            return off + addr - start
    return None
