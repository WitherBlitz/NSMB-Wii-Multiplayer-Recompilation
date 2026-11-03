// font_graves.cpp - see font_graves.h. RFNT layout as game_layout.cpp reads it: FINF points at the
// TGLP sheets (cells of cellWidth x cellHeight, one pixel apart), the CWDH widths and the CMAP code
// maps, each a linked list of blocks; offsets are to a block's data (its header + 8).
#include "font_graves.h"

#include <algorithm>
#include <map>
#include <optional>

namespace FontGraves {
namespace {

uint16_t Get16(const std::vector<uint8_t>& d, size_t at) { return static_cast<uint16_t>(d[at] << 8 | d[at + 1]); }
uint32_t Get32(const std::vector<uint8_t>& d, size_t at) {
    return static_cast<uint32_t>(d[at]) << 24 | d[at + 1] << 16 | d[at + 2] << 8 | d[at + 3];
}
void Put16(std::vector<uint8_t>& d, size_t at, uint32_t v) {
    d[at] = static_cast<uint8_t>(v >> 8);
    d[at + 1] = static_cast<uint8_t>(v);
}
void Put32(std::vector<uint8_t>& d, size_t at, uint32_t v) {
    Put16(d, at, v >> 16);
    Put16(d, at + 2, v & 0xFFFF);
}

constexpr uint8_t kInk = 3;  // an IA4 pixel counts as ink from this alpha (of 15) up

struct Glyph {
    int8_t left = 0;
    uint8_t width = 0;
    int8_t advance = 0;
    std::vector<uint8_t> px;  // cellWidth x cellHeight IA4 bytes
};

struct Font {
    std::vector<uint8_t>& d;
    uint8_t cellW = 0, cellH = 0;
    uint32_t sheetSize = 0, sheetData = 0, perRow = 0, perColumn = 0, sheetWidth = 0, sheetCount = 0;
    uint32_t widthsCount = 0;           // glyph cells that have widths (the ones in use)
    uint32_t lastCwdh = 0, lastCmap = 0;
    std::map<uint16_t, uint16_t> codes;  // character -> glyph index

    size_t PixelAt(uint32_t index, int x, int y) const {
        const uint32_t perSheet = perRow * perColumn;
        const uint32_t sheet = index / perSheet, cell = index % perSheet;
        const uint32_t sx = (cell % perRow) * (cellW + 1u) + static_cast<uint32_t>(x);
        const uint32_t sy = (cell / perRow) * (cellH + 1u) + static_cast<uint32_t>(y);
        // IA4: 8 x 4 pixel tiles of one byte each, alpha in the high nibble.
        return sheetData + sheet * sheetSize + ((sy / 4) * (sheetWidth / 8) + sx / 8) * 32 + (sy % 4) * 8 + sx % 8;
    }

    std::optional<Glyph> Read(uint16_t code) const {
        const auto it = codes.find(code);
        if (it == codes.end() || it->second >= widthsCount) {
            return std::nullopt;
        }
        Glyph g;
        const size_t w = WidthAt(it->second);
        g.left = static_cast<int8_t>(d[w]);
        g.width = d[w + 1];
        g.advance = static_cast<int8_t>(d[w + 2]);
        g.px.resize(static_cast<size_t>(cellW) * cellH);
        for (int y = 0; y < cellH; ++y) {
            for (int x = 0; x < cellW; ++x) {
                g.px[static_cast<size_t>(y) * cellW + x] = d[PixelAt(it->second, x, y)];
            }
        }
        return g;
    }

    size_t WidthAt(uint32_t index) const {
        for (uint32_t block = firstCwdh; block != 0; block = Get32(d, block + 4)) {
            const uint16_t first = Get16(d, block), last = Get16(d, block + 2);
            if (index >= first && index <= last) {
                return block + 8 + (index - first) * 3u;
            }
        }
        return 0;
    }

    uint32_t firstCwdh = 0;
};

bool Ink(uint8_t p) { return (p >> 4) >= kInk; }

// The rows [top, bottom] of a glyph's ink, or nullopt for a blank one.
std::optional<std::pair<int, int>> InkRows(const Font& f, const Glyph& g) {
    int top = -1, bottom = -1;
    for (int y = 0; y < f.cellH; ++y) {
        for (int x = 0; x < f.cellW; ++x) {
            if (Ink(g.px[static_cast<size_t>(y) * f.cellW + x])) {
                if (top < 0) top = y;
                bottom = y;
                break;
            }
        }
    }
    return top < 0 ? std::nullopt : std::optional<std::pair<int, int>>({top, bottom});
}

// The first row of the lowest run of ink rows (a letter's body, under any accent or dot).
int BodyTop(const Font& f, const Glyph& g) {
    const auto rows = InkRows(f, g);
    if (!rows) return 0;
    int y = rows->second;
    const auto rowInk = [&](int r) {
        for (int x = 0; x < f.cellW; ++x) {
            if (Ink(g.px[static_cast<size_t>(r) * f.cellW + x])) return true;
        }
        return false;
    };
    while (y > rows->first && rowInk(y - 1)) --y;
    return y;
}

float InkCentre(const Font& f, const Glyph& g, int fromRow, int toRow) {
    int lo = f.cellW, hi = -1;
    for (int y = fromRow; y <= toRow; ++y) {
        for (int x = 0; x < f.cellW; ++x) {
            if (Ink(g.px[static_cast<size_t>(y) * f.cellW + x])) {
                lo = std::min(lo, x);
                hi = std::max(hi, x);
            }
        }
    }
    return hi < 0 ? f.cellW / 2.0f : (lo + hi) / 2.0f;
}

// The accent of `marked` (its ink above `plain`'s body top), as pixels relative to plain's ink centre.
struct Accent {
    std::vector<std::pair<std::pair<int, int>, uint8_t>> px;  // (dx, y), pixel
    int bottom = 0;  // its lowest row
    int gap = 0;     // rows between it and the body under it
};

std::optional<Accent> TakeAccent(const Font& f, const Glyph& marked, const Glyph& plain) {
    // Cut at the plain letter's top (a row above it, which may hold the curve's soft edge): the
    // marked one's accent can touch its letter through faint pixels.
    const int body = BodyTop(f, plain);
    const float centre = InkCentre(f, marked, body, f.cellH - 1);
    Accent a;
    a.bottom = -1;
    for (int y = 0; y < body - 1; ++y) {
        for (int x = 0; x < f.cellW; ++x) {
            const uint8_t p = marked.px[static_cast<size_t>(y) * f.cellW + x];
            if ((p >> 4) != 0) {
                a.px.push_back({{x - static_cast<int>(centre + 0.5f), y}, p});
                if (Ink(p)) a.bottom = std::max(a.bottom, y);
            }
        }
    }
    if (a.bottom < 0) return std::nullopt;
    a.gap = body - a.bottom;
    return a;
}

// `base` with `accent` over it: its body top is where the accent's source body was, so a small
// accent sits over a small letter as it did; `raise` lifts it (a small accent over a capital).
// xTop: the top of a small letter's body (e's), under which an i keeps its stem.
Glyph Compose(const Font& f, Glyph base, const Accent& accent, bool dropDot, int raise, int xTop) {
    const int body = dropDot ? xTop : BodyTop(f, base);
    if (dropDot) {
        for (int y = 0; y < xTop - 1; ++y) {
            for (int x = 0; x < f.cellW; ++x) base.px[static_cast<size_t>(y) * f.cellW + x] = 0;
        }
    }
    const int centre = static_cast<int>(InkCentre(f, base, body, f.cellH - 1) + 0.5f);
    // Widen the glyph to the left when the accent starts left of it (a narrow i).
    int minX = 0, maxX = base.width - 1;
    for (const auto& [at, p] : accent.px) {
        minX = std::min(minX, centre + at.first);
        maxX = std::max(maxX, centre + at.first);
    }
    const int shift = std::max(0, -minX);
    if (shift > 0 && maxX + shift < f.cellW) {
        std::vector<uint8_t> moved(base.px.size(), 0);
        for (int y = 0; y < f.cellH; ++y) {
            for (int x = 0; x + shift < f.cellW; ++x) {
                moved[static_cast<size_t>(y) * f.cellW + x + shift] = base.px[static_cast<size_t>(y) * f.cellW + x];
            }
        }
        base.px = std::move(moved);
        base.left = static_cast<int8_t>(base.left - shift);
    }
    const int width = std::min<int>(f.cellW, std::max<int>(base.width + shift, maxX + shift + 1));
    base.width = static_cast<uint8_t>(width);
    for (const auto& [at, p] : accent.px) {
        const int x = centre + shift + at.first;
        const int y = at.second - raise;
        if (x < 0 || x >= f.cellW || y < 0 || y >= f.cellH) continue;
        uint8_t& out = base.px[static_cast<size_t>(y) * f.cellW + x];
        if ((p >> 4) > (out >> 4)) out = p;
    }
    return base;
}

} // namespace

int Add(std::vector<uint8_t>& d) {
    if (d.size() < 0x30 || Get32(d, 0) != 0x52464E54u) return 0;  // RFNT
    const size_t finf = Get16(d, 12) + 8;
    if (finf + 20 > d.size()) return 0;
    Font f{d};
    const uint32_t tglp = Get32(d, finf + 8);
    f.firstCwdh = Get32(d, finf + 12);
    uint32_t cmap = Get32(d, finf + 16);
    if (tglp + 24 > d.size()) return 0;
    f.cellW = d[tglp];
    f.cellH = d[tglp + 1];
    f.sheetSize = Get32(d, tglp + 4);
    f.sheetCount = Get16(d, tglp + 8);
    if (Get16(d, tglp + 10) != 2) return 0;  // IA4 sheets only (the message and Mario fonts)
    f.perRow = Get16(d, tglp + 12);
    f.perColumn = Get16(d, tglp + 14);
    f.sheetWidth = Get16(d, tglp + 16);
    f.sheetData = Get32(d, tglp + 20);
    for (uint32_t b = f.firstCwdh; b != 0 && b + 8 <= d.size(); b = Get32(d, b + 4)) {
        f.widthsCount = std::max<uint32_t>(f.widthsCount, Get16(d, b + 2) + 1u);
        f.lastCwdh = b;
    }
    for (uint32_t b = cmap; b != 0 && b + 12 <= d.size(); b = Get32(d, b + 8)) {
        const uint16_t begin = Get16(d, b), end = Get16(d, b + 2), method = Get16(d, b + 4);
        if (method == 0) {
            for (uint32_t c = begin; c <= end; ++c) f.codes[static_cast<uint16_t>(c)] = static_cast<uint16_t>(Get16(d, b + 12) + (c - begin));
        } else if (method == 1) {
            for (uint32_t c = begin; c <= end; ++c) {
                const uint16_t index = Get16(d, b + 12 + (c - begin) * 2);
                if (index != 0xFFFF) f.codes[static_cast<uint16_t>(c)] = index;
            }
        } else if (method == 2) {
            const uint16_t n = Get16(d, b + 12);
            for (uint16_t i = 0; i < n; ++i) f.codes[Get16(d, b + 14 + i * 4u)] = Get16(d, b + 16 + i * 4u);
        }
        f.lastCmap = b;
    }
    if (f.lastCwdh == 0 || f.lastCmap == 0) return 0;
    const uint32_t capacity = f.sheetCount * f.perRow * f.perColumn;

    // The accents: small from è/à/ù over their letter, capital from È/À/Ù.
    const auto accentFrom = [&](std::initializer_list<std::pair<uint16_t, uint16_t>> pairs) -> std::optional<Accent> {
        for (const auto& [marked, plain] : pairs) {
            const auto m = f.Read(marked), p = f.Read(plain);
            if (m && p) {
                if (auto a = TakeAccent(f, *m, *p)) return a;
            }
        }
        return std::nullopt;
    };
    const auto small = accentFrom({{0x00E8, 'e'}, {0x00E0, 'a'}, {0x00F9, 'u'}});
    const auto capital = accentFrom({{0x00C8, 'E'}, {0x00C0, 'A'}, {0x00D9, 'U'}});
    if (!small) return 0;

    struct Target { uint16_t code, base; bool capitalLetter, dot; };
    constexpr Target kTargets[] = {
        {0x00E0, 'a', false, false}, {0x00E8, 'e', false, false}, {0x00EC, 'i', false, true},
        {0x00F2, 'o', false, false}, {0x00F9, 'u', false, false}, {0x00C0, 'A', true, false},
        {0x00C8, 'E', true, false},  {0x00CC, 'I', true, false},  {0x00D2, 'O', true, false},
        {0x00D9, 'U', true, false},
    };
    std::vector<std::pair<uint16_t, Glyph>> made;
    for (const Target& t : kTargets) {
        if (f.codes.count(t.code) != 0) continue;
        const auto base = f.Read(t.base);
        if (!base) continue;
        int raise = 0;
        const Accent* accent = t.capitalLetter && capital ? &*capital : &*small;
        if (t.capitalLetter && !capital) {
            // The small accent over a capital: as far above its top as it sat above the small letter.
            const auto e = f.Read('e');
            raise = (e ? BodyTop(f, *e) : 0) - BodyTop(f, *base);
        } else if (t.capitalLetter) {
            raise = 0;
        }
        const auto e = f.Read('e');
        made.push_back({t.code, Compose(f, *base, *accent, t.dot, raise, e ? BodyTop(f, *e) : 0)});
    }
    const uint32_t first = f.widthsCount;
    if (made.empty() || first + made.size() > capacity) return 0;

    for (size_t i = 0; i < made.size(); ++i) {
        const Glyph& g = made[i].second;
        for (int y = 0; y < f.cellH; ++y) {
            for (int x = 0; x < f.cellW; ++x) d[f.PixelAt(first + static_cast<uint32_t>(i), x, y)] = g.px[static_cast<size_t>(y) * f.cellW + x];
        }
    }
    // New CWDH and CMAP blocks at the end, linked after the last ones.
    const auto align4 = [](size_t n) { return (n + 3) & ~size_t{3}; };
    const size_t cwdhAt = align4(d.size());
    const size_t cwdhSize = align4(8 + 8 + made.size() * 3);
    d.resize(cwdhAt + cwdhSize, 0);
    Put32(d, cwdhAt, 0x43574448u);  // CWDH
    Put32(d, cwdhAt + 4, static_cast<uint32_t>(cwdhSize));
    Put16(d, cwdhAt + 8, first);
    Put16(d, cwdhAt + 10, first + static_cast<uint32_t>(made.size()) - 1);
    for (size_t i = 0; i < made.size(); ++i) {
        d[cwdhAt + 16 + i * 3] = static_cast<uint8_t>(made[i].second.left);
        d[cwdhAt + 17 + i * 3] = made[i].second.width;
        d[cwdhAt + 18 + i * 3] = static_cast<uint8_t>(made[i].second.advance);
    }
    Put32(d, f.lastCwdh + 4, static_cast<uint32_t>(cwdhAt + 8));
    const size_t cmapAt = d.size();
    const size_t cmapSize = align4(8 + 12 + 2 + made.size() * 4);
    d.resize(cmapAt + cmapSize, 0);
    Put32(d, cmapAt, 0x434D4150u);  // CMAP
    Put32(d, cmapAt + 4, static_cast<uint32_t>(cmapSize));
    Put16(d, cmapAt + 8, 0);
    Put16(d, cmapAt + 10, 0xFFFF);
    Put16(d, cmapAt + 12, 2);  // scan
    Put16(d, cmapAt + 20, static_cast<uint32_t>(made.size()));
    for (size_t i = 0; i < made.size(); ++i) {
        Put16(d, cmapAt + 22 + i * 4, made[i].first);
        Put16(d, cmapAt + 24 + i * 4, first + static_cast<uint32_t>(i));
    }
    Put32(d, f.lastCmap + 8, static_cast<uint32_t>(cmapAt + 8));
    Put32(d, 8, static_cast<uint32_t>(d.size()));
    Put16(d, 14, Get16(d, 14) + 2u);
    return static_cast<int>(made.size());
}

} // namespace FontGraves
