#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct ImDrawList;

// Draws New Super Mario Bros. Wii's own menu graphics on top of the game, so the network-play menus
// look like the rest of the game: nw4r layouts (BRLYT) with their TPL textures and BRFNT fonts,
// read from the player's disc at run time and drawn through ImGui. Covers what the game's menu
// layouts use - picture, text, window and null panes; textures interpolated between a material's
// black and white colours, tinted by vertex colours; alpha inheritance; z rotation - without
// animations (callers place, scale and fade panes themselves).
namespace GameLayout {

// Where the game picture is on screen, in ImGui coordinates: layout units are mapped with the
// layout origin at `centerX/centerY` and `scale` pixels per unit (y up in layout space).
struct View {
    float centerX = 0.0f;
    float centerY = 0.0f;
    float scale = 1.0f;
    // The game's own layouts across: in Fill on a screen wider than 16:9 they stretch their
    // 832-unit 16:9 canvas over the whole width, so their x is scaled by this, not `scale`.
    float gameScaleX = 1.0f;
};
// The game picture's rectangle for the current display mode (4:3 or 16:9 pillar/letterbox,
// stretched for Fill), with 456 layout units across its height like the game's own layouts.
View GameView();

struct Texture {
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t id = 0;  // ImTextureID
    uint8_t wrapS = 0;
    uint8_t wrapT = 0;
};

class Font;

// Per-draw adjustments. Keys are pane names.
struct DrawParams {
    float offsetX = 0.0f;  // added to the drawn pane's translation, layout units
    float offsetY = 0.0f;
    float scale = 1.0f;    // multiplies the drawn pane's scale
    float alpha = 1.0f;    // multiplies everything
    std::unordered_map<std::string, std::u16string> text;
    std::unordered_map<std::string, bool> visible;
    std::unordered_map<std::string, float> paneAlpha;     // replaces the pane's own alpha (0..1)
    std::unordered_map<std::string, float> paneScale;     // replaces the pane's scale (both axes)
    std::unordered_map<std::string, uint32_t> vertexTint; // multiplies the pane's vertex colours (0xRRGGBBAA)
    std::unordered_map<std::string, std::pair<float, float>> paneSize;  // replaces the pane's width and height
    // Recolours the pane's vertex colours: rgb' = m * rgb, row-major (after the tint; a green button made orange).
    std::unordered_map<std::string, std::array<float, 9>> colorMatrix;
    std::unordered_map<std::string, std::pair<float, float>> paneOffset;  // added to the pane's translation
    // Replace the pane's translation (before paneOffset) and its four vertex colours (TL, TR, BL, BR,
    // 0xRRGGBBAA, before the tint): what an Animation sets.
    std::unordered_map<std::string, std::pair<float, float>> paneTranslate;
    std::unordered_map<std::string, std::array<uint32_t, 4>> vertexColors;
    std::unordered_map<std::string, float> textScale;  // multiplies a text pane's font size
    // Text panes show letters only: a mod's icons in place of letters (Newer's parentheses) are the
    // Mario font's letters there, as the runtime's own text has them (Font::plain).
    bool plainText = false;
};

class Layout {
public:
    Layout();
    ~Layout();
    Layout(const Layout&) = delete;
    Layout& operator=(const Layout&) = delete;

    // `arcPath` is a disc path such as "/Layout/yesnoWindow/yesnoWindow.arc"; the archive's first
    // .brlyt is used unless `brlytName` names one.
    bool Load(const std::string& arcPath, const std::string& brlytName = {});
    bool Loaded() const { return m_loaded; }

    // Draws `paneName` and its children (the whole layout for "RootPane").
    void Draw(ImDrawList* list, const View& view, const std::string& paneName, const DrawParams& params) const;
    // Size and translation of a pane as authored, for laying out copies.
    bool PaneRect(const std::string& paneName, float& x, float& y, float& w, float& h) const;
    // Where a pane sits in the whole layout (its centre for a centred origin), as authored.
    bool PaneWorldRect(const std::string& paneName, float& x, float& y, float& w, float& h) const;
    // Grows a row of three slices (left cap, middle, right cap) by dw x dh about its centre: mods such
    // as Newer draw the windows and buttons the base game makes from one stretchable pane this way.
    // False when the layout has no `middle`.
    bool StretchSlices(DrawParams& params, const std::string& left, const std::string& middle,
                       const std::string& right, float dw, float dh) const;
    std::vector<std::string> PaneNames() const;
    // A pane's authored translation and scale.
    bool PaneTransform(const std::string& paneName, float& tx, float& ty, float& sx, float& sy) const;

    struct Material;
    struct Pane;

private:
    void DrawPane(ImDrawList* list, const View& view, const Pane& pane, const float* parentMtx, float parentAlpha,
                  bool parentInfluences, const DrawParams& params, bool root) const;

    bool m_loaded = false;
    std::vector<std::unique_ptr<Pane>> m_panes;  // m_panes[0] is the root
    std::unordered_map<std::string, const Pane*> m_byName;
    std::vector<std::unique_ptr<Material>> m_materials;
    std::vector<std::shared_ptr<Font>> m_fonts;
};

// One of a layout archive's animations (BRLAN), the way the game plays its menus: per pane, Hermite
// keys for translation, rotation and scale (RLPA), the vertex colours and alpha (RLVC) and visibility
// (RLVI). Apply writes the values at a frame into DrawParams; frames outside the keys hold the end values.
class Animation {
public:
    Animation();
    ~Animation();
    Animation(const Animation&) = delete;
    Animation& operator=(const Animation&) = delete;
    // `brlanName` names the animation file, e.g. "pauseMenu_16_onButton.brlan" (a unique part is enough).
    bool Load(const std::string& arcPath, const std::string& brlanName);
    bool Loaded() const { return m_loaded; }
    // The animation's length in frames (the game plays it from 0 to Frames()).
    float Frames() const { return m_frames; }
    bool Animates(const std::string& paneName) const;
    // The values at `frame` for every pane it animates, `layout` giving what an animation leaves as
    // authored. `only`: just these panes (and the panes a pane's name maps to, for a copy: from -> to).
    void Apply(float frame, const Layout& layout, DrawParams& params,
               const std::vector<std::pair<std::string, std::string>>& only = {}) const;

    struct PaneAnim;

private:
    bool m_loaded = false;
    float m_frames = 0;
    std::vector<std::unique_ptr<PaneAnim>> m_panes;
};

// The game's text, measured and drawn with one of its fonts ("mj2d00_MessageFont_32_I4.brfnt",
// "mj2d01_marioFont_64_IA4.brfnt", "mj2d00_PictureFont_32_RGBA8.brfnt"). This is the runtime's own
// text: characters a mod's font draws as icons come from the Mario font (layout panes keep them).
std::shared_ptr<Font> LoadFont(const std::string& name);
bool FontReady(const Font* font);
float MeasureText(const Font& font, const std::u16string& text, float fontSize);
// Breaks `text` at spaces into lines no wider than `maxWidth` layout units.
std::vector<std::u16string> WrapText(const Font& font, const std::u16string& text, float fontSize, float maxWidth);
// The height of one line of `font` at `fontSize`, in layout units.
float LineHeight(const Font& font, float fontSize);
void DrawText(ImDrawList* list, const View& view, const Font& font, const std::u16string& text, float x, float y,
              float fontSize, uint32_t topColor, uint32_t bottomColor, float alpha, int originH = 1, int originV = 1);

std::u16string Utf16(const std::string& utf8);

// The disc's files as pixels, for images made on the CPU (runtime/src/key_caps.cpp).
struct Pixels {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;  // RGBA8, rows top down
};
// A file from the active game folder (base game or Newer), as the disc path names it.
bool ReadGameFile(const std::string& dvdPath, std::vector<uint8_t>& out);
// The file in a U8 archive whose path ends in `name`, e.g. "im_graButton_up_24_00.tpl".
bool ArchiveFile(const std::vector<uint8_t>& arc, const std::string& name, std::vector<uint8_t>& out);
bool DecodeTpl(const std::vector<uint8_t>& tpl, Pixels& out);
// A BRFNT: its glyph cells (TGLP), widths (CWDH) and character map (CMAP), each sheet decoded, and
// where the sheets' bytes are in the file (what the game hands the GPU).
struct FontPixels {
    uint32_t cellWidth = 0, cellHeight = 0, perRow = 0, perColumn = 0;
    uint32_t sheetWidth = 0, sheetHeight = 0, sheetFormat = 0, sheetSize = 0, sheetData = 0;
    int ascent = 0, height = 0, width = 0;
    std::vector<Pixels> sheets;
    struct Width {
        int left = 0, glyphWidth = 0, charWidth = 0;
    };
    std::vector<Width> widths;                    // by glyph index
    std::unordered_map<uint16_t, uint16_t> map;   // character -> glyph index
    // The glyph's sheet and cell (top-left texel), false without one.
    bool Cell(char16_t code, uint32_t& sheet, uint32_t& x, uint32_t& y, Width* width = nullptr) const;
};
bool DecodeFont(const std::vector<uint8_t>& brfnt, FontPixels& out);

} // namespace GameLayout
