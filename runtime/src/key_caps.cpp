#include "key_caps.h"

#include "keybinds.h"
#include "netplay/game_layout.h"
#include "prompt_style.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "settings_overlay.h"

#include <aurora/imgui.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/pad.h>
#include <imgui.h>
#include <SDL3/SDL_gamepad.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace KeyCaps {
namespace {

using GameLayout::FontPixels;
using GameLayout::Pixels;

constexpr const char* kPictureFont = "mj2d00_PictureFont_32_RGBA8.brfnt";
constexpr const char* kMessageFont = "mj2d00_MessageFont_32_I4.brfnt";

// The picture font's icon characters for the Wii Remote's buttons.
constexpr char16_t kCodeDPad = u'!', kCodeA = u'"', kCodeB = u'#', kCodeHome = u'$', kCodePlus = u'%',
                   kCodeMinus = u'&', kCodeOne = u'\'', kCodeTwo = u'(';

struct Rgba {
    float r = 0, g = 0, b = 0, a = 0;
};

Rgba At(const Pixels& p, int x, int y) {
    x = std::clamp(x, 0, static_cast<int>(p.width) - 1);
    y = std::clamp(y, 0, static_cast<int>(p.height) - 1);
    const uint8_t* c = &p.rgba[(static_cast<size_t>(y) * p.width + x) * 4];
    return {c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, c[3] / 255.0f};
}

// Bilinear, premultiplied so clear texels don't darken edges, clamped to [x0, x1) x [y0, y1) of the source.
Rgba Sample(const Pixels& p, float u, float v, int x0, int y0, int x1, int y1) {
    u = std::clamp(u - 0.5f, static_cast<float>(x0), static_cast<float>(x1 - 1));
    v = std::clamp(v - 0.5f, static_cast<float>(y0), static_cast<float>(y1 - 1));
    const int ix = static_cast<int>(std::floor(u)), iy = static_cast<int>(std::floor(v));
    const float fx = u - ix, fy = v - iy;
    Rgba out;
    const int xs[2] = {ix, std::min(ix + 1, x1 - 1)}, ys[2] = {iy, std::min(iy + 1, y1 - 1)};
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            const Rgba c = At(p, xs[i], ys[j]);
            const float w = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
            out.r += c.r * c.a * w;
            out.g += c.g * c.a * w;
            out.b += c.b * c.a * w;
            out.a += c.a * w;
        }
    }
    if (out.a > 0.0001f) {
        out.r /= out.a;
        out.g /= out.a;
        out.b /= out.a;
    }
    return out;
}

void Blend(Pixels& dst, int x, int y, const Rgba& c) {
    if (x < 0 || y < 0 || x >= static_cast<int>(dst.width) || y >= static_cast<int>(dst.height) || c.a <= 0.0f) {
        return;
    }
    uint8_t* d = &dst.rgba[(static_cast<size_t>(y) * dst.width + x) * 4];
    const float da = d[3] / 255.0f;
    const float a = c.a + da * (1 - c.a);
    const auto mix = [&](float s, uint8_t t) {
        return a <= 0 ? 0.0f : (s * c.a + t / 255.0f * da * (1 - c.a)) / a;
    };
    d[0] = static_cast<uint8_t>(std::clamp(mix(c.r, d[0]), 0.0f, 1.0f) * 255.0f + 0.5f);
    d[1] = static_cast<uint8_t>(std::clamp(mix(c.g, d[1]), 0.0f, 1.0f) * 255.0f + 0.5f);
    d[2] = static_cast<uint8_t>(std::clamp(mix(c.b, d[2]), 0.0f, 1.0f) * 255.0f + 0.5f);
    d[3] = static_cast<uint8_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// Draws source rectangle (sx, sy, sw, sh) of `src` into dst's (dx, dy, dw, dh), output pixels.
void Blit(Pixels& dst, const Pixels& src, float sx, float sy, float sw, float sh, float dx, float dy, float dw,
          float dh, int clipX0, int clipY0, int clipX1, int clipY1, const Rgba* tint = nullptr) {
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) {
        return;
    }
    const int x0 = static_cast<int>(std::floor(dx)), y0 = static_cast<int>(std::floor(dy));
    const int x1 = static_cast<int>(std::ceil(dx + dw)), y1 = static_cast<int>(std::ceil(dy + dh));
    for (int y = y0; y < y1; ++y) {
        const float cy = std::clamp(y + 0.5f, dy, dy + dh);
        const float coverY = std::min(y + 1.0f, dy + dh) - std::max(static_cast<float>(y), dy);
        for (int x = x0; x < x1; ++x) {
            const float cx = std::clamp(x + 0.5f, dx, dx + dw);
            const float coverX = std::min(x + 1.0f, dx + dw) - std::max(static_cast<float>(x), dx);
            Rgba c = Sample(src, sx + (cx - dx) / dw * sw, sy + (cy - dy) / dh * sh, clipX0, clipY0, clipX1, clipY1);
            if (tint != nullptr) {
                c.r *= tint->r;
                c.g *= tint->g;
                c.b *= tint->b;
                c.a *= tint->a;
            }
            c.a *= std::clamp(coverX, 0.0f, 1.0f) * std::clamp(coverY, 0.0f, 1.0f);
            Blend(dst, x, y, c);
        }
    }
}

float Luma(const uint8_t* c) {
    return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
}

// ---------------------------------------------------------------- the disc's pieces
struct CellRect {
    int x = 0, y = 0, w = 0, h = 0;  // in its own image
};

// A glyph cell copied out of the picture font, and the part of it the icon covers.
struct Piece {
    Pixels image;
    CellRect bounds;
};

bool CopyCell(const FontPixels& font, char16_t code, Piece& piece) {
    uint32_t sheet = 0, x = 0, y = 0;
    if (!font.Cell(code, sheet, x, y)) {
        return false;
    }
    const Pixels& src = font.sheets[sheet];
    piece.image.width = font.cellWidth;
    piece.image.height = font.cellHeight;
    piece.image.rgba.assign(static_cast<size_t>(font.cellWidth) * font.cellHeight * 4, 0);
    int bx0 = 1 << 30, by0 = 1 << 30, bx1 = -1, by1 = -1;
    for (uint32_t j = 0; j < font.cellHeight; ++j) {
        for (uint32_t i = 0; i < font.cellWidth; ++i) {
            const uint8_t* s = &src.rgba[(static_cast<size_t>(y + j) * src.width + x + i) * 4];
            std::copy(s, s + 4, &piece.image.rgba[(static_cast<size_t>(j) * font.cellWidth + i) * 4]);
            if (s[3] > 128) {
                bx0 = std::min(bx0, static_cast<int>(i));
                by0 = std::min(by0, static_cast<int>(j));
                bx1 = std::max(bx1, static_cast<int>(i));
                by1 = std::max(by1, static_cast<int>(j));
            }
        }
    }
    if (bx1 < bx0) {
        return false;
    }
    piece.bounds = {bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1};
    return true;
}

// The B button with its letter taken out: a blank key cap. The letter is the grey in the middle of
// the cap; each of its texels takes the colour of the cap's face beside it on the same row.
void BlankSquare(Piece& piece) {
    const CellRect& b = piece.bounds;
    Pixels& p = piece.image;
    const int x0 = b.x + 4, x1 = b.x + b.w - 4, y0 = b.y + b.h / 4, y1 = b.y + b.h * 3 / 4 + 1;
    for (int y = y0; y < y1; ++y) {
        const uint8_t* face = &p.rgba[(static_cast<size_t>(y) * p.width + b.x + 3) * 4];
        const float faceLuma = Luma(face);
        uint8_t keep[4];
        std::copy(face, face + 4, keep);
        for (int x = x0; x < x1; ++x) {
            uint8_t* c = &p.rgba[(static_cast<size_t>(y) * p.width + x) * 4];
            if (Luma(c) < faceLuma - 20.0f) {
                std::copy(keep, keep + 4, c);
            }
        }
    }
}

// The 1 button with its digit taken out: a blank round face button. The digit is grey; the face is
// white to blue: each grey texel inside the ring is filled in from the face either side of it.
void BlankRound(Piece& piece) {
    const CellRect& b = piece.bounds;
    Pixels& p = piece.image;
    for (int y = b.y + 3; y < b.y + b.h - 3; ++y) {
        uint8_t* row = &p.rgba[static_cast<size_t>(y) * p.width * 4];
        int first = -1, last = -1;
        for (int x = b.x; x < b.x + b.w; ++x) {
            if (Luma(row + x * 4) > 190.0f) {
                if (first < 0) first = x;
                last = x;
            }
        }
        if (first < 0 || last - first < 4) {
            continue;
        }
        const auto digit = [&](int x) {
            const uint8_t* c = row + x * 4;
            const bool grey = std::abs(static_cast<int>(c[0]) - static_cast<int>(c[2])) < 14;
            return grey && Luma(c) < 205.0f;
        };
        for (int x = first + 1; x < last; ++x) {
            if (!digit(x)) {
                continue;
            }
            int left = x - 1, right = x + 1;
            while (right < last && digit(right)) ++right;
            const uint8_t* a = row + left * 4;
            const uint8_t* c = row + right * 4;
            for (int k = x; k < right; ++k) {
                const float t = static_cast<float>(k - left) / static_cast<float>(right - left);
                for (int ch = 0; ch < 4; ++ch) {
                    row[k * 4 + ch] = static_cast<uint8_t>(a[ch] + (c[ch] - a[ch]) * t + 0.5f);
                }
            }
            x = right;
        }
    }
}

struct Assets {
    bool ok = false;
    Piece square;   // a blank key cap (the B button)
    Piece round;    // a blank face button (the 1 button)
    Piece dpad;     // the D-pad as the game draws it
    FontPixels message;
};

// ---------------------------------------------------------------- drawing caps
constexpr Rgba kLabel{0x31 / 255.0f, 0x31 / 255.0f, 0x31 / 255.0f, 1.0f};  // the B button's letter

// Nine-slice of a piece into dst's (dx, dy, dw, dh): the corners keep their size (shrunk when the
// rectangle is smaller than them), the edges and the middle stretch.
void NineSlice(Pixels& dst, const Piece& piece, float dx, float dy, float dw, float dh, float scale) {
    const CellRect& b = piece.bounds;
    const float l = 5, r = 5, t = 7, bm = 8;  // texels: the border, and the top band and bottom curve of the face
    float kx = scale, ky = scale;
    if ((l + r) * kx > dw) kx = dw / (l + r);
    if ((t + bm) * ky > dh) ky = dh / (t + bm);
    const float sxs[4] = {static_cast<float>(b.x), b.x + l, b.x + b.w - r, static_cast<float>(b.x + b.w)};
    const float sys[4] = {static_cast<float>(b.y), b.y + t, b.y + b.h - bm, static_cast<float>(b.y + b.h)};
    const float dxs[4] = {dx, dx + l * kx, dx + dw - r * kx, dx + dw};
    const float dys[4] = {dy, dy + t * ky, dy + dh - bm * ky, dy + dh};
    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) {
            Blit(dst, piece.image, sxs[i], sys[j], sxs[i + 1] - sxs[i], sys[j + 1] - sys[j], dxs[i], dys[j],
                 dxs[i + 1] - dxs[i], dys[j + 1] - dys[j], b.x, b.y, b.x + b.w, b.y + b.h);
        }
    }
}

// The message font's letters, `letterHeight` output pixels for a capital, centred on (cx, cy) and no
// wider than maxWidth (smaller then, down to 60%; past that they are squeezed).
void Label(Pixels& dst, const FontPixels& font, const std::u16string& text, float cx, float cy, float letterHeight,
           float maxWidth, const Rgba& color) {
    if (text.empty() || font.height == 0) {
        return;
    }
    // A capital letter is about 0.62 of the font's height.
    float k = letterHeight / (0.62f * font.height);
    float width = 0;
    for (char16_t c : text) {
        uint32_t sheet = 0, x = 0, y = 0;
        FontPixels::Width w;
        if (font.Cell(c, sheet, x, y, &w)) width += w.charWidth;
    }
    float squeeze = 1.0f;
    if (width * k > maxWidth) {
        const float smaller = std::max(maxWidth / (width * k), 0.6f);
        k *= smaller;
        if (width * k > maxWidth) squeeze = maxWidth / (width * k);
    }
    float pen = cx - width * k * squeeze / 2;
    // The capitals' middle sits at (ascent - 0.31 height) below the cell's top.
    const float top = cy - (font.ascent - 0.31f * font.height) * k;
    for (char16_t c : text) {
        uint32_t sheet = 0, x = 0, y = 0;
        FontPixels::Width w;
        if (!font.Cell(c, sheet, x, y, &w)) continue;
        const Pixels& src = font.sheets[sheet];
        Blit(dst, src, static_cast<float>(x), static_cast<float>(y), static_cast<float>(font.cellWidth),
             static_cast<float>(font.cellHeight), pen + w.left * k * squeeze, top, font.cellWidth * k * squeeze,
             font.cellHeight * k, static_cast<int>(x), static_cast<int>(y), static_cast<int>(x + font.cellWidth),
             static_cast<int>(y + font.cellHeight), &color);
        pen += w.charWidth * k * squeeze;
    }
}

// A filled triangle pointing `dir` (0 up, 1 down, 2 left, 3 right), sampled 4x4 per pixel.
void Arrow(Pixels& dst, float cx, float cy, float size, int dir, const Rgba& color) {
    const float h = size / 2;
    float ax, ay, bx, by, qx, qy;
    switch (dir) {
    case 0: ax = cx; ay = cy - h; bx = cx - h; by = cy + h * 0.7f; qx = cx + h; qy = cy + h * 0.7f; break;
    case 1: ax = cx; ay = cy + h; bx = cx + h; by = cy - h * 0.7f; qx = cx - h; qy = cy - h * 0.7f; break;
    case 2: ax = cx - h; ay = cy; bx = cx + h * 0.7f; by = cy + h; qx = cx + h * 0.7f; qy = cy - h; break;
    default: ax = cx + h; ay = cy; bx = cx - h * 0.7f; by = cy - h; qx = cx - h * 0.7f; qy = cy + h; break;
    }
    const auto edge = [](float x0, float y0, float x1, float y1, float px, float py) {
        return (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0);
    };
    const int x0 = static_cast<int>(std::floor(cx - h)) - 1, x1 = static_cast<int>(std::ceil(cx + h)) + 1;
    const int y0 = static_cast<int>(std::floor(cy - h)) - 1, y1 = static_cast<int>(std::ceil(cy + h)) + 1;
    const float area = edge(ax, ay, bx, by, qx, qy);
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            int inside = 0;
            for (int sy = 0; sy < 4; ++sy) {
                for (int sx = 0; sx < 4; ++sx) {
                    const float px = x + (sx + 0.5f) / 4, py = y + (sy + 0.5f) / 4;
                    const float e0 = edge(ax, ay, bx, by, px, py), e1 = edge(bx, by, qx, qy, px, py),
                                e2 = edge(qx, qy, ax, ay, px, py);
                    inside += area > 0 ? (e0 >= 0 && e1 >= 0 && e2 >= 0) : (e0 <= 0 && e1 <= 0 && e2 <= 0);
                }
            }
            if (inside > 0) {
                Rgba c = color;
                c.a *= inside / 16.0f;
                Blend(dst, x, y, c);
            }
        }
    }
}

// ---------------------------------------------------------------- what a cap says
// One thing a cell shows: a key cap with a label or an arrow, or a round face button.
struct Glyph {
    enum Kind { Cap, Round, DPad } kind = Cap;
    std::u16string label;
    int arrow = -1;            // a cap with an arrow (0 up, 1 down, 2 left, 3 right)
    Rgba color = kLabel;
};

// A keyboard key's name (SDL's, or "Mouse L") as a cap: the shortest label that reads as that key. In a
// prompt cell 5 letters at most; `menu`: the runtime's own menus, which have room to tell left and right
// modifiers apart and for up to 8 letters.
Glyph KeyGlyph(const std::string& name, bool menu = false) {
    Glyph g;
    static const std::map<std::string, std::string> kSided = {
        {"Left Shift", "LShift"}, {"Right Shift", "RShift"}, {"Left Ctrl", "LCtrl"}, {"Right Ctrl", "RCtrl"},
        {"Left Alt", "LAlt"},     {"Right Alt", "RAlt"},     {"Left GUI", "LWin"},   {"Right GUI", "RWin"},
    };
    static const std::map<std::string, std::string> kShort = {
        {"Return", "Enter"},     {"Escape", "Esc"},       {"Backspace", "Bksp"},   {"Left Shift", "Shift"},
        {"Right Shift", "Shift"}, {"Left Ctrl", "Ctrl"},  {"Right Ctrl", "Ctrl"},  {"Left Alt", "Alt"},
        {"Right Alt", "Alt"},    {"Left GUI", "Win"},     {"Right GUI", "Win"},    {"CapsLock", "Caps"},
        {"Delete", "Del"},       {"Insert", "Ins"},       {"PageUp", "PgUp"},      {"PageDown", "PgDn"},
        {"Mouse L", "LMB"},      {"Mouse R", "RMB"},      {"Mouse M", "MMB"},      {"Mouse 4", "M4"},
        {"Mouse 5", "M5"},       {"Space", "Space"},
    };
    if (name == "Up") g.arrow = 0;
    else if (name == "Down") g.arrow = 1;
    else if (name == "Left") g.arrow = 2;
    else if (name == "Right") g.arrow = 3;
    if (g.arrow >= 0) {
        return g;
    }
    std::string label = name;
    if (const auto sided = kSided.find(name); menu && sided != kSided.end()) {
        label = sided->second;
    } else if (const auto it = kShort.find(name); it != kShort.end()) {
        label = it->second;
    } else if (label.rfind("Keypad ", 0) == 0) {
        label = "Num" + label.substr(7);
    }
    const size_t most = menu ? 8 : 5;
    if (label.size() > most) {
        label = label.substr(0, most);
    }
    g.label = GameLayout::Utf16(label);
    return g;
}

// A controller button (aurora's PAD encoding) with its Xbox label.
Glyph PadGlyph(uint32_t native) {
    Glyph g;
    const auto round = [&](const char* label, uint32_t rgb) {
        g.kind = Glyph::Round;
        g.label = GameLayout::Utf16(label);
        g.color = {((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, 1.0f};
    };
    const auto cap = [&](const char* label) { g.label = GameLayout::Utf16(label); };
    if (PADIsAxisButton(native)) {
        switch (PADAxisButtonAxis(native)) {
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: cap("LT"); break;
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: cap("RT"); break;
        case SDL_GAMEPAD_AXIS_LEFTX:
        case SDL_GAMEPAD_AXIS_LEFTY: round("LS", 0x313131); break;
        default: round("RS", 0x313131); break;
        }
        return g;
    }
    switch (native) {
    case SDL_GAMEPAD_BUTTON_SOUTH: round("A", 0x3C9C2A); break;  // Xbox's green A, red B, blue X, yellow Y
    case SDL_GAMEPAD_BUTTON_EAST: round("B", 0xC8302A); break;
    case SDL_GAMEPAD_BUTTON_WEST: round("X", 0x2A64C8); break;
    case SDL_GAMEPAD_BUTTON_NORTH: round("Y", 0xC89600); break;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: cap("LB"); break;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: cap("RB"); break;
    case SDL_GAMEPAD_BUTTON_BACK: cap("View"); break;
    case SDL_GAMEPAD_BUTTON_START: cap("Menu"); break;
    case SDL_GAMEPAD_BUTTON_GUIDE: cap("Xbox"); break;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: round("LS", 0x313131); break;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: round("RS", 0x313131); break;
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: g.kind = Glyph::DPad; break;
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1: cap("P3"); break;
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2: cap("P4"); break;
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1: cap("P1"); break;
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2: cap("P2"); break;
    default: cap("?"); break;
    }
    return g;
}

// Draws one glyph into dst's rectangle (output pixels); `scale` output pixels per font texel.
void DrawGlyph(Pixels& dst, const Assets& a, const Glyph& g, float x, float y, float w, float h, float scale) {
    if (g.kind == Glyph::DPad) {
        const CellRect& b = a.dpad.bounds;
        const float s = std::min(w / b.w, h / b.h);
        Blit(dst, a.dpad.image, static_cast<float>(b.x), static_cast<float>(b.y), static_cast<float>(b.w),
             static_cast<float>(b.h), x + (w - b.w * s) / 2, y + (h - b.h * s) / 2, b.w * s, b.h * s, b.x, b.y,
             b.x + b.w, b.y + b.h);
        return;
    }
    if (g.kind == Glyph::Round) {
        const CellRect& b = a.round.bounds;
        const float s = std::min(w / b.w, h / b.h);
        const float rx = x + (w - b.w * s) / 2, ry = y + (h - b.h * s) / 2;
        Blit(dst, a.round.image, static_cast<float>(b.x), static_cast<float>(b.y), static_cast<float>(b.w),
             static_cast<float>(b.h), rx, ry, b.w * s, b.h * s, b.x, b.y, b.x + b.w, b.y + b.h);
        Label(dst, a.message, g.label, rx + b.w * s / 2, ry + b.h * s * 0.47f, b.h * s * 0.42f, b.w * s * 0.62f,
              g.color);
        return;
    }
    NineSlice(dst, a.square, x, y, w, h, scale);
    const float faceTop = y + std::min(3.0f * scale, h * 0.15f), faceBottom = y + h - std::min(5.0f * scale, h * 0.2f);
    const float cy = (faceTop + faceBottom) / 2;
    if (g.arrow >= 0) {
        Arrow(dst, x + w / 2, cy, std::min(w, faceBottom - faceTop) * 0.62f, g.arrow, g.color);
        return;
    }
    const float letter = std::min((faceBottom - faceTop) * 0.62f, 13.0f * scale);
    Label(dst, a.message, g.label, x + w / 2, cy, letter, w - 5.0f * scale, g.color);
}

// A cell's contents, `glyphs` stacked top to bottom (a combination), or a cluster of four (the D-pad
// as four keys: one on top, three under it).
void DrawCell(Pixels& dst, const Assets& a, const std::vector<Glyph>& glyphs, bool cluster, float cx0, float cy0,
              float cell, float scale) {
    const float pad = scale;  // a texel of air round the cell's contents
    if (cluster && glyphs.size() == 4) {
        const float w = (cell - 2 * pad) / 3, h = (cell - 2 * pad) / 2;
        DrawGlyph(dst, a, glyphs[0], cx0 + pad + w, cy0 + pad, w, h, scale * 0.5f);
        DrawGlyph(dst, a, glyphs[2], cx0 + pad, cy0 + pad + h, w, h, scale * 0.5f);
        DrawGlyph(dst, a, glyphs[1], cx0 + pad + w, cy0 + pad + h, w, h, scale * 0.5f);
        DrawGlyph(dst, a, glyphs[3], cx0 + pad + 2 * w, cy0 + pad + h, w, h, scale * 0.5f);
        return;
    }
    const size_t n = std::min<size_t>(glyphs.size(), 3);
    if (n == 1) {
        const Glyph& g = glyphs[0];
        // A one-letter key is as square as the game's own B button; a word gets the cell's width.
        const bool narrow = g.kind == Glyph::Cap && (g.arrow >= 0 || g.label.size() <= 1);
        const float w = narrow ? a.square.bounds.w * scale : cell - 2 * pad;
        DrawGlyph(dst, a, g, cx0 + (cell - w) / 2, cy0 + a.square.bounds.y * scale, w,
                  a.square.bounds.h * scale, scale);
        return;
    }
    const float h = (cell - 2 * pad) / static_cast<float>(n);
    for (size_t i = 0; i < n; ++i) {
        DrawGlyph(dst, a, glyphs[i], cx0 + pad, cy0 + pad + h * i, cell - 2 * pad, h, scale * (n == 2 ? 0.55f : 0.4f));
    }
}

// ---------------------------------------------------------------- the prompt cells
struct CellContent {
    char16_t code;
    std::vector<Glyph> glyphs;  // empty: the game's own icon stays
    bool cluster = false;
};

bool IsModifier(const std::string& name) {
    return name.find("Ctrl") != std::string::npos || name.find("Shift") != std::string::npos ||
           name.find("Alt") != std::string::npos || name.find("GUI") != std::string::npos;
}

std::vector<Glyph> KeyGlyphs(int action) {
    std::vector<std::string> names = Keybinds::FirstBinding(action);
    // Modifiers on top, as a combination is read ("Ctrl + X").
    std::stable_sort(names.begin(), names.end(), [](const std::string& l, const std::string& r) {
        return IsModifier(l) && !IsModifier(r);
    });
    std::vector<Glyph> glyphs;
    for (const auto& name : names) glyphs.push_back(KeyGlyph(name));
    return glyphs;
}

std::vector<Glyph> PadGlyphs(int action) {
    std::vector<Glyph> glyphs;
    for (const uint32_t native : settings_overlay::ControllerFirstBinding(action)) glyphs.push_back(PadGlyph(native));
    // - with nothing bound to its button is still the controller's View button (virtual_remote.cpp PadIntent).
    if (glyphs.empty() && action == Keybinds::kMinus && settings_overlay::ControllerConnected()) {
        glyphs.push_back(PadGlyph(SDL_GAMEPAD_BUTTON_BACK));
    }
    return glyphs;
}

std::vector<CellContent> Contents(PromptStyle::Style style) {
    std::vector<CellContent> cells = {{kCodeTwo, {}}, {kCodeOne, {}}, {kCodeA, {}}, {kCodePlus, {}}, {kCodeMinus, {}},
                                      {kCodeDPad, {}}};
    const int actions[5] = {Keybinds::kJump, Keybinds::kRun, Keybinds::kA, Keybinds::kPlus, Keybinds::kMinus};
    if (style == PromptStyle::Style::Keyboard) {
        for (int i = 0; i < 5; ++i) cells[i].glyphs = KeyGlyphs(actions[i]);
        // The D-pad: the four direction keys as a cluster, when each is one key.
        std::vector<Glyph> dirs;
        for (const int action : {Keybinds::kUp, Keybinds::kDown, Keybinds::kLeft, Keybinds::kRight}) {
            const auto g = KeyGlyphs(action);
            if (g.size() != 1) {
                dirs.clear();
                break;
            }
            dirs.push_back(g[0]);
        }
        if (dirs.size() == 4) {
            cells[5].glyphs = dirs;
            cells[5].cluster = true;
        }
    } else if (style == PromptStyle::Style::Controller) {
        for (int i = 0; i < 5; ++i) cells[i].glyphs = PadGlyphs(actions[i]);
        // The D-pad stays the game's own cross: a controller's D-pad (and stick) moves as it shows.
    }
    return cells;
}

std::string Signature(const std::vector<CellContent>& cells) {
    std::string s;
    for (const auto& cell : cells) {
        s += static_cast<char>(cell.code);
        for (const auto& g : cell.glyphs) {
            s += static_cast<char>('0' + g.kind) + std::to_string(g.arrow);
            for (char16_t c : g.label) s += static_cast<char>(c);
            s += '|';
        }
        s += cell.cluster ? "c;" : ";";
    }
    return s;
}

// ---------------------------------------------------------------- the font, per language folder
struct Sheet {
    std::string folder;
    std::vector<uint8_t> file;  // the BRFNT
    FontPixels font;
    uint32_t sheetIndex = 0;    // the sheet holding the button icons
    uint32_t slot = 0;          // aurora's override slot
};

// A prompt the game draws as a texture of its own rather than a font cell: the strap screen's and the
// letters' "2" (os_2btn_00) and the world map guides' D-pad (ImIconjujiButton). The same bytes in
// several archives are one icon.
struct Icon {
    char16_t code = 0;            // the font cell whose contents it shows (kCodeTwo, kCodeDPad)
    std::vector<uint8_t> source;  // the texture's bytes, as the game hands them to the GPU
    uint32_t width = 0, height = 0, format = 0;
    CellRect bounds;              // where the art is (alpha over half), texels
    uint32_t slot = 0;
};

struct State {
    bool loaded = false;
    Assets assets;
    std::vector<Sheet> sheets;
    std::vector<Icon> icons;
    // The runtime's own menus' key caps (DrawKey): per key name and scale, the texture and its aspect.
    std::map<std::string, std::pair<uint64_t, float>> keyTextures;
    std::string signature;     // what the overrides show now ("" the game's own)
    int scale = 0;
    uint32_t checkFrame = 0;
    // The runtime's own menus: the sheet as shown now, as an ImGui texture (cached per look).
    std::map<std::string, uint64_t> uiTextures;
    uint64_t uiTexture = 0;
    uint32_t uiWidth = 0, uiHeight = 0;
};
State g_state;

// aurora's override slots (AuroraSetTextureOverride): the font sheets first, then the icons.
constexpr uint32_t kOverrideSlots = 8;

// A TPL's only image: its bytes, size and format; false for anything else (several images, a palette
// format, mipmaps), which the override can't stand in for.
bool TplImage(const std::vector<uint8_t>& tpl, std::vector<uint8_t>& bytes, uint32_t& width, uint32_t& height,
              uint32_t& format) {
    const auto be32 = [&](size_t at) {
        return at + 4 <= tpl.size() ? static_cast<uint32_t>(tpl[at]) << 24 | static_cast<uint32_t>(tpl[at + 1]) << 16 |
                                          static_cast<uint32_t>(tpl[at + 2]) << 8 | tpl[at + 3]
                                    : 0u;
    };
    if (tpl.size() < 12 || be32(0) != 0x0020AF30u || be32(4) != 1) {
        return false;
    }
    const uint32_t header = be32(be32(8));
    if (header == 0 || header + 0x24 > tpl.size()) {
        return false;
    }
    height = be32(header) >> 16;
    width = be32(header) & 0xFFFFu;
    format = be32(header + 4);
    const uint32_t data = be32(header + 8);
    const uint8_t maxLod = tpl[header + 0x22];
    // Texels of 8 bits (I8, IA4: 8x4 blocks), 16 (IA8, RGB565, RGB5A3: 4x4) or 32 (RGBA8: 4x4).
    const uint32_t bits = format == 3 || format == 4 || format == 5 ? 16 : format == 1 || format == 2 ? 8
                          : format == 6                             ? 32 : 0;
    if (bits == 0 || maxLod != 0 || width == 0 || height == 0) {
        return false;
    }
    const uint32_t blockW = bits == 8 ? 8 : 4, blockH = 4;
    const size_t size = static_cast<size_t>((width + blockW - 1) / blockW * blockW) *
                        ((height + blockH - 1) / blockH * blockH) * bits / 8;
    if (static_cast<size_t>(data) + size > tpl.size()) {
        return false;
    }
    bytes.assign(tpl.begin() + data, tpl.begin() + static_cast<long>(data + size));
    return true;
}

void LoadIcons() {
    struct Where {
        const char* arc;
        const char* tpl;
        char16_t code;
    };
    static constexpr Where kIcons[] = {
        {"/Layout/controllerInformation/controllerInformation.arc", "os_2btn_00.tpl", kCodeTwo},
        {"/Layout/letterWindow/letterWindow.arc", "os_2btn_00.tpl", kCodeTwo},
        {"/Layout/MultiCourseSelect/MultiCourseSelect.arc", "ImIconjujiButton.tpl", kCodeDPad},
        {"/Layout/worldSelectMapGuide/worldSelectMapGuide.arc", "ImIconjujiButton.tpl", kCodeDPad},
    };
    for (const Where& where : kIcons) {
        std::vector<uint8_t> arc, tpl;
        Icon icon;
        icon.code = where.code;
        Pixels pixels;
        if (!GameLayout::ReadGameFile(where.arc, arc) || !GameLayout::ArchiveFile(arc, where.tpl, tpl) ||
            !TplImage(tpl, icon.source, icon.width, icon.height, icon.format) || !GameLayout::DecodeTpl(tpl, pixels) ||
            pixels.width != icon.width || pixels.height != icon.height) {
            continue;
        }
        if (std::any_of(g_state.icons.begin(), g_state.icons.end(),
                        [&](const Icon& other) { return other.source == icon.source; })) {
            continue;
        }
        int bx0 = 1 << 30, by0 = 1 << 30, bx1 = -1, by1 = -1;
        for (uint32_t y = 0; y < pixels.height; ++y) {
            for (uint32_t x = 0; x < pixels.width; ++x) {
                if (pixels.rgba[(static_cast<size_t>(y) * pixels.width + x) * 4 + 3] > 128) {
                    bx0 = std::min(bx0, static_cast<int>(x));
                    by0 = std::min(by0, static_cast<int>(y));
                    bx1 = std::max(bx1, static_cast<int>(x));
                    by1 = std::max(by1, static_cast<int>(y));
                }
            }
        }
        const uint32_t slot = static_cast<uint32_t>(g_state.sheets.size() + g_state.icons.size());
        if (bx1 < bx0 || slot >= kOverrideSlots) {
            continue;
        }
        icon.bounds = {bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1};
        icon.slot = slot;
        g_state.icons.push_back(std::move(icon));
    }
}

void LoadOnce() {
    if (g_state.loaded) {
        return;
    }
    g_state.loaded = true;
    for (const char* folder : {"EngUS", "SpaUS", "FraUS"}) {
        Sheet sheet;
        sheet.folder = folder;
        if (!GameLayout::ReadGameFile(std::string("/US/") + folder + "/Font/" + kPictureFont, sheet.file) ||
            !GameLayout::DecodeFont(sheet.file, sheet.font)) {
            continue;
        }
        uint32_t index = 0, x = 0, y = 0;
        if (!sheet.font.Cell(kCodeTwo, index, x, y)) {
            continue;
        }
        sheet.sheetIndex = index;
        // The same font in another folder needs no slot of its own.
        const size_t at = sheet.font.sheetData + static_cast<size_t>(index) * sheet.font.sheetSize;
        const bool duplicate = std::any_of(g_state.sheets.begin(), g_state.sheets.end(), [&](const Sheet& other) {
            const size_t oat = other.font.sheetData + static_cast<size_t>(other.sheetIndex) * other.font.sheetSize;
            return other.font.sheetSize == sheet.font.sheetSize && other.font.sheetFormat == sheet.font.sheetFormat &&
                   std::equal(sheet.file.begin() + at, sheet.file.begin() + at + sheet.font.sheetSize,
                              other.file.begin() + oat);
        });
        if (!duplicate) {
            sheet.slot = static_cast<uint32_t>(g_state.sheets.size());
            g_state.sheets.push_back(std::move(sheet));
        }
    }
    if (g_state.sheets.empty()) {
        RT_LOGF(RT_TAG_RUNTIME, "key caps: no picture font in the game folder, prompts stay as the game draws them\n");
        return;
    }
    Assets& a = g_state.assets;
    const FontPixels& font = g_state.sheets[0].font;
    std::vector<uint8_t> message;
    a.ok = CopyCell(font, kCodeB, a.square) && CopyCell(font, kCodeOne, a.round) && CopyCell(font, kCodeDPad, a.dpad) &&
           GameLayout::ReadGameFile(std::string("/US/EngUS/Font/") + kMessageFont, message) &&
           GameLayout::DecodeFont(message, a.message);
    if (a.ok) {
        BlankSquare(a.square);
        BlankRound(a.round);
    } else {
        RT_LOGF(RT_TAG_RUNTIME, "key caps: the picture or message font's cells are not as expected, prompts stay\n");
        return;
    }
    LoadIcons();
}

// The sheet holding the button icons, `scale` times larger, with `cells` repainted.
Pixels Paint(const Sheet& sheet, const std::vector<CellContent>& cells, int scale) {
    const FontPixels& font = sheet.font;
    const Pixels& src = font.sheets[sheet.sheetIndex];
    Pixels out;
    out.width = src.width * scale;
    out.height = src.height * scale;
    if (scale == 1) {
        out.rgba = src.rgba;
    } else {
        out.rgba.assign(static_cast<size_t>(out.width) * out.height * 4, 0);
        // Each cell on its own, so no cell bleeds into the next.
        const uint32_t pitchX = font.cellWidth + 1, pitchY = font.cellHeight + 1;
        for (uint32_t cy = 0; cy + font.cellHeight <= src.height; cy += pitchY) {
            for (uint32_t cx = 0; cx + font.cellWidth <= src.width; cx += pitchX) {
                Blit(out, src, static_cast<float>(cx), static_cast<float>(cy), static_cast<float>(font.cellWidth),
                     static_cast<float>(font.cellHeight), static_cast<float>(cx * scale), static_cast<float>(cy * scale),
                     static_cast<float>(font.cellWidth * scale), static_cast<float>(font.cellHeight * scale),
                     static_cast<int>(cx), static_cast<int>(cy), static_cast<int>(cx + font.cellWidth),
                     static_cast<int>(cy + font.cellHeight));
            }
        }
    }
    for (const CellContent& cell : cells) {
        uint32_t index = 0, x = 0, y = 0;
        if (cell.glyphs.empty() || !font.Cell(cell.code, index, x, y) || index != sheet.sheetIndex) {
            continue;
        }
        // Clear the cell, then draw.
        for (uint32_t j = y * scale; j < (y + font.cellHeight) * scale; ++j) {
            std::fill(&out.rgba[(static_cast<size_t>(j) * out.width + x * scale) * 4],
                      &out.rgba[(static_cast<size_t>(j) * out.width + (x + font.cellWidth) * scale) * 4], 0);
        }
        DrawCell(out, g_state.assets, cell.glyphs, cell.cluster, static_cast<float>(x * scale),
                 static_cast<float>(y * scale), static_cast<float>(font.cellWidth * scale), static_cast<float>(scale));
    }
    return out;
}

// An icon (Icon) `scale` times larger: what its font cell shows now, fitted to the square where the
// game's own art is; clear elsewhere.
Pixels PaintIcon(const Icon& icon, const CellContent& cell, int scale) {
    Pixels out;
    out.width = icon.width * scale;
    out.height = icon.height * scale;
    out.rgba.assign(static_cast<size_t>(out.width) * out.height * 4, 0);
    const float side = static_cast<float>(std::min(icon.bounds.w, icon.bounds.h));
    const float cellTexels = static_cast<float>(g_state.sheets[0].font.cellWidth);
    const float x0 = icon.bounds.x + (icon.bounds.w - side) / 2, y0 = icon.bounds.y + (icon.bounds.h - side) / 2;
    // DrawCell lays out one font cell (cellTexels across) at `scale`: here the same, grown to the art.
    DrawCell(out, g_state.assets, cell.glyphs, cell.cluster, x0 * scale, y0 * scale, side * scale,
             scale * side / cellTexels);
    return out;
}

// Output pixels per font texel: the picture's lines over 456 (the layouts' height), rounded, at most 4
// (2 on a phone): sharp at the render scale, never sampled down past what the GPU filters cleanly.
int WantedScale() {
    float scale = settings_overlay::RenderScale();
    if (scale <= 0.0f) {
        scale = ImGui::GetIO().DisplaySize.y / 480.0f;
    }
#if defined(__ANDROID__)
    constexpr int kMax = 2;
#else
    constexpr int kMax = 4;
#endif
    return std::clamp(static_cast<int>(std::lround(scale)), 1, kMax);
}

uint64_t UploadUi(const std::string& key, const Pixels& pixels) {
    if (const auto it = g_state.uiTextures.find(key); it != g_state.uiTextures.end()) {
        return it->second;
    }
    const uint64_t id = static_cast<uint64_t>(aurora_imgui_add_texture(pixels.width, pixels.height, pixels.rgba.data()));
    g_state.uiTextures[key] = id;
    return id;
}

} // namespace

void Update() {
    LoadOnce();
    if (!g_state.assets.ok) {
        return;
    }
    // Bindings change rarely: looked at twice a second, and at once when the style or scale changes.
    static uint32_t s_style = 0xFF;
    static uint32_t s_generation = 0xFFFFFFFFu;
    const PromptStyle::Style style = PromptStyle::Current();
    const int scale = WantedScale();
    // Any binding written (Settings' Keybinds or Controller tab, F10 > Controls, Reset Defaults, a
    // combination taken or cleared, a controller connecting with Config.toml's buttons) bumps
    // Keybinds::BindingsGeneration: the prompts follow at once. The look every 30 frames stays as a
    // fallback (which controller is the one shown, after a disconnect).
    static uint32_t s_bindings = 0xFFFFFFFFu;
    const uint32_t bindings = Keybinds::BindingsGeneration();
    const bool changed = static_cast<uint32_t>(style) != s_style || PromptStyle::Generation() != s_generation ||
                         scale != g_state.scale || bindings != s_bindings;
    if (!changed && ++g_state.checkFrame % 30 != 0) {
        return;
    }
    s_style = static_cast<uint32_t>(style);
    s_generation = PromptStyle::Generation();
    s_bindings = bindings;
    const std::vector<CellContent> cells = Contents(style);
    const bool any = std::any_of(cells.begin(), cells.end(), [](const CellContent& c) { return !c.glyphs.empty(); });
    const std::string signature = any ? Signature(cells) : std::string();
    if (signature == g_state.signature && scale == g_state.scale) {
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    g_state.signature = signature;
    g_state.scale = scale;
    for (const Icon& icon : g_state.icons) {
        const auto cell = std::find_if(cells.begin(), cells.end(), [&](const CellContent& c) { return c.code == icon.code; });
        if (!any || cell == cells.end() || cell->glyphs.empty()) {
            AuroraSetTextureOverride(icon.slot, nullptr, 0, 0, 0, 0, nullptr, 0, 0);
            continue;
        }
        const Pixels painted = PaintIcon(icon, *cell, scale);
        AuroraSetTextureOverride(icon.slot, icon.source.data(), static_cast<uint32_t>(icon.source.size()), icon.width,
                                 icon.height, icon.format, painted.rgba.data(), painted.width, painted.height);
    }
    for (const Sheet& sheet : g_state.sheets) {
        const size_t at = sheet.font.sheetData + static_cast<size_t>(sheet.sheetIndex) * sheet.font.sheetSize;
        if (!any) {
            AuroraSetTextureOverride(sheet.slot, nullptr, 0, 0, 0, 0, nullptr, 0, 0);
            continue;
        }
        const Pixels painted = Paint(sheet, cells, scale);
        AuroraSetTextureOverride(sheet.slot, sheet.file.data() + at, sheet.font.sheetSize, sheet.font.sheetWidth,
                                 sheet.font.sheetHeight, sheet.font.sheetFormat, painted.rgba.data(), painted.width,
                                 painted.height);
        if (&sheet == &g_state.sheets[0]) {
            g_state.uiTexture = UploadUi(signature + "@" + std::to_string(scale), painted);
            g_state.uiWidth = painted.width;
            g_state.uiHeight = painted.height;
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    RT_LOGF(RT_TAG_RUNTIME, "key caps: %s prompts at %dx (%.1f ms)\n",
            style == PromptStyle::Style::Keyboard ? "keyboard" : style == PromptStyle::Style::Controller ? "controller" : "Wii",
            scale, ms);
}

float DrawKey(ImDrawList* list, float x, float y, float height, const std::string& name, float alpha) {
    LoadOnce();
    const Assets& a = g_state.assets;
    if (!a.ok || height <= 0.0f) {
        return 0.0f;
    }
    const int scale = WantedScale();
    const std::string key = name + "@" + std::to_string(scale);
    auto it = g_state.keyTextures.find(key);
    if (it == g_state.keyTextures.end()) {
        // As tall as the game's B button; as wide as it for a letter or an arrow, else as the label needs.
        const Glyph g = KeyGlyph(name, true);
        const CellRect& b = a.square.bounds;
        float w = static_cast<float>(b.w);
        if (g.arrow < 0 && g.label.size() > 1) {
            // DrawGlyph's letter size at one output pixel a texel.
            const float faceTop = std::min(3.0f, b.h * 0.15f), faceBottom = b.h - std::min(5.0f, b.h * 0.2f);
            const float letter = std::min((faceBottom - faceTop) * 0.62f, 13.0f);
            const float k = letter / (0.62f * static_cast<float>(std::max(1, a.message.height)));
            float text = 0;
            for (char16_t c : g.label) {
                uint32_t sheet = 0, cx = 0, cy = 0;
                FontPixels::Width cw;
                if (a.message.Cell(c, sheet, cx, cy, &cw)) text += static_cast<float>(cw.charWidth) * k;
            }
            w = std::max(w, std::ceil(text + 11.0f));
        }
        Pixels image;
        image.width = static_cast<uint32_t>(w) * static_cast<uint32_t>(scale);
        image.height = static_cast<uint32_t>(b.h) * static_cast<uint32_t>(scale);
        image.rgba.assign(static_cast<size_t>(image.width) * image.height * 4, 0);
        DrawGlyph(image, a, g, 0, 0, static_cast<float>(image.width), static_cast<float>(image.height),
                  static_cast<float>(scale));
        const uint64_t id = static_cast<uint64_t>(aurora_imgui_add_texture(image.width, image.height, image.rgba.data()));
        it = g_state.keyTextures.emplace(key, std::make_pair(id, w / static_cast<float>(b.h))).first;
    }
    const float width = height * it->second.second;
    if (list != nullptr) {
        list->AddImage(static_cast<ImTextureID>(it->second.first), ImVec2(x, y - height / 2),
                       ImVec2(x + width, y + height / 2), ImVec2(0, 0), ImVec2(1, 1),
                       IM_COL32(255, 255, 255, static_cast<int>(255 * std::clamp(alpha, 0.0f, 1.0f))));
    }
    return width;
}

float Draw(ImDrawList* list, float x, float y, float height, Button button, float alpha) {
    LoadOnce();
    if (g_state.sheets.empty()) {
        return 0.0f;
    }
    const Sheet& sheet = g_state.sheets[0];
    static const char16_t kCodes[] = {kCodeTwo, kCodeOne, kCodeA, kCodePlus, kCodeMinus, kCodeDPad, kCodeB, kCodeHome};
    uint32_t index = 0, cx = 0, cy = 0;
    FontPixels::Width w;
    if (!sheet.font.Cell(kCodes[static_cast<int>(button)], index, cx, cy, &w)) {
        return 0.0f;
    }
    const float k = height / static_cast<float>(sheet.font.cellHeight);
    const float width = static_cast<float>(sheet.font.cellWidth) * k;
    if (list == nullptr) {
        return width;
    }
    // The repainted sheet while one is shown, else the game's own (uploaded once).
    uint64_t texture = 0;
    uint32_t tw = 0, th = 0;
    int scale = 1;
    if (!g_state.signature.empty() && g_state.uiTexture != 0 && index == sheet.sheetIndex) {
        texture = g_state.uiTexture;
        tw = g_state.uiWidth;
        th = g_state.uiHeight;
        scale = g_state.scale;
    } else {
        const Pixels& original = sheet.font.sheets[index];
        texture = UploadUi("original" + std::to_string(index), original);
        tw = original.width;
        th = original.height;
    }
    const ImVec2 uv0(static_cast<float>(cx * scale) / tw, static_cast<float>(cy * scale) / th);
    const ImVec2 uv1(static_cast<float>((cx + sheet.font.cellWidth) * scale) / tw,
                     static_cast<float>((cy + sheet.font.cellHeight) * scale) / th);
    list->AddImage(static_cast<ImTextureID>(texture), ImVec2(x, y - height / 2), ImVec2(x + width, y + height / 2), uv0,
                   uv1, IM_COL32(255, 255, 255, static_cast<int>(255 * std::clamp(alpha, 0.0f, 1.0f))));
    return width;
}

} // namespace KeyCaps
