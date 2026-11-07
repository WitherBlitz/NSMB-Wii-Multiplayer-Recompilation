#include "hle/storage/layout_patch.h"

#include "det_clock.h"
#include "runtime_config.h"
#include "runtime_product.h"
#include "runtime_log.h"
#include "ui_text.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <vector>

namespace LayoutPatch {
namespace {

namespace fs = std::filesystem;

std::atomic<bool> g_pausePatched{false};
std::atomic<bool> g_mapPatched{false};
std::atomic<float> g_pausePitch{70.0f};

uint16_t BE16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
uint32_t BE32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3];
}
void Put16(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}
void Put32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}
float GetF(const uint8_t* p) {
    const uint32_t v = BE32(p);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
void PutF(uint8_t* p, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    Put32(p, v);
}

constexpr uint32_t Tag(const char (&s)[5]) {
    return static_cast<uint32_t>(s[0]) << 24 | static_cast<uint32_t>(s[1]) << 16 | static_cast<uint32_t>(s[2]) << 8 |
           static_cast<uint32_t>(s[3]);
}

// ---------------------------------------------------------------- U8
struct U8Node {
    uint8_t type = 0;
    uint32_t nameOffset = 0;
    uint32_t a = 0, b = 0;  // file: data offset and size; folder: parent and next
    std::string path;
    std::vector<uint8_t> data;
};

bool ReadU8(const std::vector<uint8_t>& arc, std::vector<U8Node>& nodes, std::vector<uint8_t>& strings) {
    if (arc.size() < 0x20 || BE32(arc.data()) != 0x55AA382Du) {
        return false;
    }
    const uint32_t root = BE32(&arc[4]);
    if (root + 12 > arc.size()) {
        return false;
    }
    const uint32_t total = BE32(&arc[root + 8]);
    const size_t stringTable = root + static_cast<size_t>(total) * 12;
    const uint32_t headerSize = BE32(&arc[8]);
    if (stringTable > arc.size() || root + headerSize > arc.size() || stringTable > root + headerSize) {
        return false;
    }
    strings.assign(arc.begin() + stringTable, arc.begin() + root + headerSize);
    std::vector<std::pair<std::string, uint32_t>> dirs{{"", total}};
    for (uint32_t i = 0; i < total; ++i) {
        const uint8_t* n = &arc[root + i * 12];
        U8Node node;
        node.type = n[0];
        node.nameOffset = BE32(n) & 0xFFFFFFu;
        node.a = BE32(n + 4);
        node.b = BE32(n + 8);
        std::string name;
        for (size_t k = node.nameOffset; k < strings.size() && strings[k] != 0; ++k) {
            name.push_back(static_cast<char>(strings[k]));
        }
        while (i > 0 && dirs.size() > 1 && i >= dirs.back().second) {
            dirs.pop_back();
        }
        for (const auto& d : dirs) {
            if (!d.first.empty()) node.path += d.first + "/";
        }
        node.path += name;
        if (node.type == 1) {
            if (i > 0) dirs.emplace_back(name, node.b);
        } else {
            if (static_cast<size_t>(node.a) + node.b > arc.size()) return false;
            node.data.assign(arc.begin() + node.a, arc.begin() + node.a + node.b);
        }
        nodes.push_back(std::move(node));
    }
    return true;
}

std::vector<uint8_t> WriteU8(const std::vector<U8Node>& nodes, const std::vector<uint8_t>& strings) {
    const uint32_t root = 0x20;
    const uint32_t headerSize = static_cast<uint32_t>(nodes.size() * 12 + strings.size());
    uint32_t dataStart = (root + headerSize + 0x1F) & ~0x1Fu;
    std::vector<uint8_t> out(dataStart, 0);
    Put32(&out[0], 0x55AA382Du);
    Put32(&out[4], root);
    Put32(&out[8], headerSize);
    Put32(&out[12], dataStart);
    std::copy(strings.begin(), strings.end(), out.begin() + root + nodes.size() * 12);
    for (size_t i = 0; i < nodes.size(); ++i) {
        const U8Node& node = nodes[i];
        uint8_t* n = &out[root + i * 12];
        Put32(n, static_cast<uint32_t>(node.type) << 24 | node.nameOffset);
        if (node.type == 1) {
            Put32(n + 4, node.a);
            Put32(n + 8, node.b);
            continue;
        }
        const uint32_t at = static_cast<uint32_t>(out.size());
        Put32(n + 4, at);
        Put32(n + 8, static_cast<uint32_t>(node.data.size()));
        out.insert(out.end(), node.data.begin(), node.data.end());
        out.resize((out.size() + 0x1F) & ~size_t{0x1F}, 0);
    }
    return out;
}

// ---------------------------------------------------------------- BRLYT / BRLAN sections
struct Section {
    uint32_t kind = 0;
    std::vector<uint8_t> bytes;  // with its 8-byte header
};

bool ReadSections(const std::vector<uint8_t>& file, uint32_t magic, std::vector<uint8_t>& header,
                  std::vector<Section>& sections) {
    if (file.size() < 16 || BE32(file.data()) != magic) {
        return false;
    }
    const uint16_t headerSize = BE16(&file[12]);
    const uint16_t count = BE16(&file[14]);
    header.assign(file.begin(), file.begin() + headerSize);
    size_t pos = headerSize;
    for (uint16_t i = 0; i < count; ++i) {
        if (pos + 8 > file.size()) return false;
        const uint32_t size = BE32(&file[pos + 4]);
        if (size < 8 || pos + size > file.size()) return false;
        sections.push_back({BE32(&file[pos]), std::vector<uint8_t>(file.begin() + pos, file.begin() + pos + size)});
        pos += size;
    }
    return true;
}

std::vector<uint8_t> WriteSections(std::vector<uint8_t> header, const std::vector<Section>& sections) {
    std::vector<uint8_t> out = std::move(header);
    for (const Section& s : sections) out.insert(out.end(), s.bytes.begin(), s.bytes.end());
    Put32(&out[8], static_cast<uint32_t>(out.size()));
    Put16(&out[14], static_cast<uint32_t>(sections.size()));
    return out;
}

bool IsPane(uint32_t kind) {
    return kind == Tag("pan1") || kind == Tag("pic1") || kind == Tag("txt1") || kind == Tag("wnd1") ||
           kind == Tag("bnd1");
}

std::string PaneName(const Section& s) {
    return std::string(reinterpret_cast<const char*>(&s.bytes[0x0C]), strnlen(reinterpret_cast<const char*>(&s.bytes[0x0C]), 16));
}

void SetPaneName(Section& s, const std::string& name) {
    std::fill(&s.bytes[0x0C], &s.bytes[0x0C] + 16, 0);
    std::memcpy(&s.bytes[0x0C], name.data(), std::min<size_t>(name.size(), 16));
}

int FindPane(const std::vector<Section>& sections, const std::string& name) {
    for (size_t i = 0; i < sections.size(); ++i) {
        if (IsPane(sections[i].kind) && PaneName(sections[i]) == name) return static_cast<int>(i);
    }
    return -1;
}

// The last section of a pane's subtree: the pane, its user data, and its children's pas1 ... pae1.
size_t SubtreeEnd(const std::vector<Section>& sections, size_t pane) {
    size_t end = pane;
    while (end + 1 < sections.size() && sections[end + 1].kind == Tag("usd1")) ++end;
    if (end + 1 < sections.size() && sections[end + 1].kind == Tag("pas1")) {
        int depth = 0;
        for (size_t i = end + 1; i < sections.size(); ++i) {
            if (sections[i].kind == Tag("pas1")) ++depth;
            if (sections[i].kind == Tag("pae1") && --depth == 0) return i;
        }
    }
    return end;
}

void MovePane(std::vector<Section>& sections, const std::string& name, float dy) {
    const int i = FindPane(sections, name);
    if (i < 0) return;
    uint8_t* ty = &sections[static_cast<size_t>(i)].bytes[0x28];
    const float y = GetF(ty);
    if (std::fabs(y) < 1000.0f) {  // Newer parks the panes it doesn't use at 9999
        PutF(ty, y + dy);
    }
}

void GrowPane(std::vector<Section>& sections, const std::string& name, float dh) {
    const int i = FindPane(sections, name);
    if (i < 0) return;
    uint8_t* h = &sections[static_cast<size_t>(i)].bytes[0x48];
    PutF(h, GetF(h) + dh);
}

// A text pane's text set to `text` (its buffer room for at least 32 letters).
void SetPaneText(Section& s, const std::u16string& text) {
    if (s.kind != Tag("txt1") || s.bytes.size() < 0x74) return;
    const uint32_t stringOffset = BE32(&s.bytes[0x58]);
    if (stringOffset < 0x74 || stringOffset > s.bytes.size()) return;
    s.bytes.resize(stringOffset);
    for (char16_t c : text) {
        s.bytes.push_back(static_cast<uint8_t>(c >> 8));
        s.bytes.push_back(static_cast<uint8_t>(c));
    }
    s.bytes.push_back(0);
    s.bytes.push_back(0);
    const uint32_t length = static_cast<uint32_t>((text.size() + 1) * 2);
    while (s.bytes.size() % 4 != 0) s.bytes.push_back(0);
    Put16(&s.bytes[0x4C], std::max<uint32_t>(length, 64));  // the buffer
    Put16(&s.bytes[0x4E], length);
    Put32(&s.bytes[4], static_cast<uint32_t>(s.bytes.size()));
}

std::u16string Utf16(const std::string& utf8) {
    std::u16string out;
    for (size_t i = 0; i < utf8.size();) {
        const uint8_t c = static_cast<uint8_t>(utf8[i]);
        uint32_t cp = c;
        int extra = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : 3;
        if (extra > 0) cp = c & (0x3F >> extra);
        ++i;
        for (int k = 0; k < extra && i < utf8.size(); ++k, ++i) cp = (cp << 6) | (static_cast<uint8_t>(utf8[i]) & 0x3F);
        out.push_back(cp < 0x10000 ? static_cast<char16_t>(cp) : u'?');
    }
    return out;
}

// The copy's pane names: Exit's with "settings" in them, unique and within nw4r's 16 letters.
std::string CopyName(const std::string& name) {
    if (name == "P_SBBase_02") return "P_SBBase_03";
    if (const size_t at = name.find("modoru"); at != std::string::npos) {
        return name.substr(0, at) + "setting" + name.substr(at + 6);
    }
    if (name.size() > 3 && name.compare(name.size() - 3, 3, "_02") == 0) return name.substr(0, name.size() - 3) + "_03";
    if (name.size() > 2 && name.compare(name.size() - 2, 2, "_2") == 0) return name.substr(0, name.size() - 2) + "_3";
    if (name.rfind("Btn2", 0) == 0) return "Btn3" + name.substr(4);
    return name + "S";
}

// The copy's animation group. m2d keeps the name the game registers a group under, so it has to be a
// string that stays in the game's memory: the yes/no window's group name, in main.dol's constant data
// (hle/nsmbw_menu_settings.cpp finds it there). No group of the pause window's has that name.
constexpr const char* kGroupName = "B00_yesButton";

// The group `from`, copied as `to` right after it with its members renamed (members not copied left out).
bool CopyGroup(std::vector<Section>& sections, const char* from, const char* to,
               const std::map<std::string, std::string>& renames) {
    for (size_t i = 0; i < sections.size(); ++i) {
        Section& s = sections[i];
        if (s.kind != Tag("grp1") || s.bytes.size() < 0x1C ||
            std::string(reinterpret_cast<const char*>(&s.bytes[8]), strnlen(reinterpret_cast<const char*>(&s.bytes[8]), 16)) != from) {
            continue;
        }
        Section group;
        group.kind = Tag("grp1");
        group.bytes.assign(0x1C, 0);
        std::memcpy(&group.bytes[0], "grp1", 4);
        std::memcpy(&group.bytes[8], to, std::strlen(to));
        const uint16_t count = BE16(&s.bytes[0x18]);
        uint16_t kept = 0;
        for (uint16_t m = 0; m < count && 0x1C + (m + 1) * 16u <= s.bytes.size(); ++m) {
            const std::string member(reinterpret_cast<const char*>(&s.bytes[0x1C + m * 16]),
                                     strnlen(reinterpret_cast<const char*>(&s.bytes[0x1C + m * 16]), 16));
            const auto it = renames.find(member);
            if (it == renames.end()) continue;
            group.bytes.resize(group.bytes.size() + 16, 0);
            std::memcpy(&group.bytes[group.bytes.size() - 16], it->second.data(), it->second.size());
            ++kept;
        }
        Put16(&group.bytes[0x18], kept);
        Put32(&group.bytes[4], static_cast<uint32_t>(group.bytes.size()));
        sections.insert(sections.begin() + static_cast<long>(i) + 1, group);
        return kept > 0;
    }
    return false;
}

// The world map's + menu (corseSelectMenu_11.brlyt): Quick Save's button copied as Settings under it,
// the separator and Title Screen moved down a button, the window grown and the menu raised so it stays
// clear of its Back hint. Copies are named ..._06 (the layout already has a T_corseSelect_05).
constexpr const char* kMapGroupName = "B01_noButton";  // like kGroupName: a name the game's data holds

bool PatchAnimation(std::vector<uint8_t>& file, const std::map<std::string, std::string>& renames,
                    const char* fromGroup, const char* toGroup);

bool PatchMapLayout(std::vector<uint8_t>& file, std::map<std::string, std::string>& renames) {
    std::vector<uint8_t> header;
    std::vector<Section> sections;
    if (!ReadSections(file, Tag("RLYT"), header, sections)) return false;
    const int save = FindPane(sections, "P_SBBase_03"), above = FindPane(sections, "P_SBBase_02");
    if (save < 0 || above < 0 || FindPane(sections, "P_SBBase_04") < 0 || FindPane(sections, "P_SBBase_06") >= 0 ||
        FindPane(sections, "W_corseSelect_01") < 0 || FindPane(sections, "N_corseSelect_01") < 0 ||
        FindPane(sections, "P_winbg_01") < 0) {
        return false;
    }
    const float pitch = GetF(&sections[static_cast<size_t>(above)].bytes[0x28]) -
                        GetF(&sections[static_cast<size_t>(save)].bytes[0x28]);
    if (!(pitch > 20.0f && pitch < 200.0f)) return false;
    std::set<std::string> existing;
    for (const Section& s : sections) {
        if (IsPane(s.kind)) existing.insert(PaneName(s));
    }
    const size_t end = SubtreeEnd(sections, static_cast<size_t>(save));
    std::vector<Section> copy(sections.begin() + save, sections.begin() + static_cast<long>(end) + 1);
    const std::u16string label = Utf16(UiText::Tr("Settings"));
    for (Section& s : copy) {
        if (!IsPane(s.kind)) continue;
        const std::string from = PaneName(s);
        std::string to = from;
        if (to.size() >= 2 && to.compare(to.size() - 2, 2, "03") == 0) to = to.substr(0, to.size() - 2) + "06";
        else to += "S";
        if (to.size() > 16 || existing.count(to) != 0) return false;
        renames[from] = to;
        SetPaneName(s, to);
        if (s.kind == Tag("txt1")) SetPaneText(s, label);
    }
    PutF(&copy.front().bytes[0x28], GetF(&copy.front().bytes[0x28]) - pitch);
    sections.insert(sections.begin() + static_cast<long>(end) + 1, copy.begin(), copy.end());
    // The buttons move up half a button, Title Screen down half, the separator and its band with it.
    MovePane(sections, "N_corseSelect_01", pitch / 2);
    MovePane(sections, "P_SBBase_04", -pitch);
    MovePane(sections, "P_line_00", -pitch / 2);
    MovePane(sections, "P_winbg_00", -pitch / 2);
    GrowPane(sections, "P_winbg_01", pitch);
    GrowPane(sections, "W_corseSelect_01", pitch);
    // The window's frame then ends clear of "Back (1)" (N_back, at -183).
    const int window = FindPane(sections, "W_corseSelect_01");
    const float half = GetF(&sections[static_cast<size_t>(window)].bytes[0x48]) / 2;
    MovePane(sections, "N_corseSelect_00", std::max(0.0f, half - 168.0f));
    if (!CopyGroup(sections, "B03_Button", kMapGroupName, renames)) return false;
    file = WriteSections(header, sections);
    return true;
}

bool PatchMapArchive(std::vector<uint8_t>& arc) {
    std::vector<U8Node> nodes;
    std::vector<uint8_t> strings;
    if (!ReadU8(arc, nodes, strings)) return false;
    std::map<std::string, std::string> renames;
    bool layout = false;
    for (U8Node& node : nodes) {
        if (node.type == 0 && node.path.size() > 6 && node.path.compare(node.path.size() - 6, 6, ".brlyt") == 0 &&
            node.path.find("corseSelectMenu_11") != std::string::npos) {
            layout = PatchMapLayout(node.data, renames);
        }
    }
    if (!layout) return false;
    int animations = 0;
    for (U8Node& node : nodes) {
        if (node.type != 0 || node.path.size() < 6 || node.path.compare(node.path.size() - 6, 6, ".brlan") != 0) {
            continue;
        }
        if (!PatchAnimation(node.data, renames, "B03_Button", kMapGroupName)) return false;
        ++animations;
    }
    if (animations < 4) return false;
    arc = WriteU8(nodes, strings);
    return true;
}

// pauseMenu_16.brlyt with the Settings button. `renames`: Exit's pane -> its copy's.
bool PatchPauseLayout(std::vector<uint8_t>& file, std::map<std::string, std::string>& renames, float& pitch) {
    std::vector<uint8_t> header;
    std::vector<Section> sections;
    if (!ReadSections(file, Tag("RLYT"), header, sections)) return false;
    const int cont = FindPane(sections, "P_SBBase_00"), exit = FindPane(sections, "P_SBBase_02");
    if (cont < 0 || exit < 0 || FindPane(sections, "P_SBBase_03") >= 0 || FindPane(sections, "W_N_pauseMenu_00") < 0 ||
        FindPane(sections, "N_pauseMenu_01") < 0) {
        return false;
    }
    pitch = GetF(&sections[static_cast<size_t>(cont)].bytes[0x28]) - GetF(&sections[static_cast<size_t>(exit)].bytes[0x28]);
    if (!(pitch > 20.0f && pitch < 200.0f)) return false;
    std::set<std::string> existing;
    for (const Section& s : sections) {
        if (IsPane(s.kind)) existing.insert(PaneName(s));
    }
    // Exit's subtree, without the grey cover that marks it unavailable.
    const size_t end = SubtreeEnd(sections, static_cast<size_t>(exit));
    std::vector<Section> copy;
    for (size_t i = static_cast<size_t>(exit); i <= end; ++i) {
        if (IsPane(sections[i].kind) && PaneName(sections[i]) == "P_shadowBlack") {
            i = SubtreeEnd(sections, i);
            continue;
        }
        copy.push_back(sections[i]);
    }
    const std::u16string label = Utf16(UiText::Tr("Settings"));
    for (Section& s : copy) {
        if (!IsPane(s.kind)) continue;
        const std::string from = PaneName(s), to = CopyName(from);
        if (to.size() > 16 || existing.count(to) != 0) {
            RT_LOGF(RT_TAG_DVD, "layout patch: no room for the copy of %s, the pause window stays as it is\n", from.c_str());
            return false;
        }
        renames[from] = to;
        SetPaneName(s, to);
        if (s.kind == Tag("txt1")) SetPaneText(s, label);
        // Newer colours Exit's three slices red: Settings takes Continue's (Btn1_*) instead.
        if (s.kind == Tag("pic1") && from.rfind("Btn2", 0) == 0 && s.bytes.size() >= 0x5E) {
            const int continueSlice = FindPane(sections, "Btn1" + from.substr(4));
            if (continueSlice >= 0) {
                std::memcpy(&s.bytes[0x5C], &sections[static_cast<size_t>(continueSlice)].bytes[0x5C], 2);
            }
        }
    }
    // Settings takes Exit's place; Exit moves a button lower; the window grows by one button and
    // everything in it moves up by half of that, so it stays centred.
    MovePane(sections, "P_SBBase_02", -pitch);
    sections.insert(sections.begin() + static_cast<long>(end) + 1, copy.begin(), copy.end());
    for (const char* grow : {"P_bg_00", "W_N_pauseMenu_00", "BG_Left", "BG_Middle", "BG_Right"}) {
        GrowPane(sections, grow, pitch);
    }
    for (const char* up : {"N_pauseMenu_01", "N_worldText_00", "N_osusumeText_00", "N_multiText_00", "LevelNumShadow",
                           "LevelNameShadow"}) {
        MovePane(sections, up, pitch / 2);
    }
    // The copy's animation group, beside Exit's.
    for (size_t i = 0; i < sections.size(); ++i) {
        Section& s = sections[i];
        if (s.kind != Tag("grp1") || s.bytes.size() < 0x1C ||
            std::string(reinterpret_cast<const char*>(&s.bytes[8]), strnlen(reinterpret_cast<const char*>(&s.bytes[8]), 16)) !=
                "B02_menuButton") {
            continue;
        }
        Section group;
        group.kind = Tag("grp1");
        group.bytes.assign(0x1C, 0);
        std::memcpy(&group.bytes[0], "grp1", 4);
        std::memcpy(&group.bytes[8], kGroupName, std::strlen(kGroupName));
        const uint16_t count = BE16(&s.bytes[0x18]);
        uint16_t kept = 0;
        for (uint16_t m = 0; m < count && 0x1C + (m + 1) * 16u <= s.bytes.size(); ++m) {
            const std::string member(reinterpret_cast<const char*>(&s.bytes[0x1C + m * 16]),
                                     strnlen(reinterpret_cast<const char*>(&s.bytes[0x1C + m * 16]), 16));
            const auto it = renames.find(member);
            if (it == renames.end()) continue;
            group.bytes.resize(group.bytes.size() + 16, 0);
            std::memcpy(&group.bytes[group.bytes.size() - 16], it->second.data(), it->second.size());
            ++kept;
        }
        Put16(&group.bytes[0x18], kept);
        Put32(&group.bytes[4], static_cast<uint32_t>(group.bytes.size()));
        sections.insert(sections.begin() + static_cast<long>(i) + 1, group);
        file = WriteSections(header, sections);
        return kept > 0;
    }
    return false;
}

// A BRLAN with each animated pane Exit's copy has also animated under the copy's name.
bool PatchAnimation(std::vector<uint8_t>& file, const std::map<std::string, std::string>& renames,
                    const char* fromGroup, const char* toGroup) {
    std::vector<uint8_t> header;
    std::vector<Section> sections;
    if (!ReadSections(file, Tag("RLAN"), header, sections)) return false;
    for (Section& s : sections) {
        // m2d binds an animation to the groups its pat1 lists: Exit's group gets the copy's beside it.
        if (s.kind == Tag("pat1") && s.bytes.size() >= 0x1C) {
            std::vector<uint8_t>& b = s.bytes;
            const uint16_t count = BE16(&b[0x0A]);
            const uint32_t groups = BE32(&b[0x10]);
            if (groups + count * 20u != b.size()) return false;  // the groups are its end, as authored
            for (uint16_t g = 0; g < count; ++g) {
                const uint8_t* entry = &b[groups + g * 20u];
                if (std::string(reinterpret_cast<const char*>(entry), strnlen(reinterpret_cast<const char*>(entry), 16)) !=
                    fromGroup) {
                    continue;
                }
                std::vector<uint8_t> copy(entry, entry + 20);
                std::fill(copy.begin(), copy.begin() + 16, 0);
                std::memcpy(copy.data(), toGroup, std::strlen(toGroup));
                b.insert(b.end(), copy.begin(), copy.end());
                Put16(&b[0x0A], count + 1u);
                Put32(&b[4], static_cast<uint32_t>(b.size()));
                break;
            }
            continue;
        }
        if (s.kind != Tag("pai1")) continue;
        std::vector<uint8_t>& b = s.bytes;
        if (b.size() < 0x14 || BE16(&b[0x0C]) != 0) return false;  // texture patterns: not expected here
        const uint16_t count = BE16(&b[0x0E]);
        const uint32_t table = BE32(&b[0x10]);
        if (table + count * 4u > b.size()) return false;
        std::vector<std::vector<uint8_t>> entries;
        for (uint16_t e = 0; e < count; ++e) {
            const uint32_t at = BE32(&b[table + e * 4]);
            const uint32_t next = e + 1 < count ? BE32(&b[table + (e + 1) * 4]) : static_cast<uint32_t>(b.size());
            if (at >= next || next > b.size()) return false;
            entries.emplace_back(b.begin() + at, b.begin() + next);
        }
        const size_t original = entries.size();
        for (size_t e = 0; e < original; ++e) {
            const std::string name(reinterpret_cast<const char*>(entries[e].data()),
                                   strnlen(reinterpret_cast<const char*>(entries[e].data()), 20));
            const auto it = renames.find(name);
            if (it == renames.end()) continue;
            std::vector<uint8_t> copy = entries[e];
            std::fill(copy.begin(), copy.begin() + 20, 0);
            std::memcpy(copy.data(), it->second.data(), it->second.size());
            entries.push_back(std::move(copy));
        }
        std::vector<uint8_t> out(b.begin(), b.begin() + table);
        Put16(&out[0x0E], static_cast<uint32_t>(entries.size()));
        const size_t tableAt = out.size();
        out.resize(out.size() + entries.size() * 4, 0);
        for (size_t e = 0; e < entries.size(); ++e) {
            Put32(&out[tableAt + e * 4], static_cast<uint32_t>(out.size()));
            out.insert(out.end(), entries[e].begin(), entries[e].end());
        }
        Put32(&out[4], static_cast<uint32_t>(out.size()));
        b = std::move(out);
    }
    file = WriteSections(header, sections);
    return true;
}

bool PatchPauseArchive(std::vector<uint8_t>& arc, float& pitch) {
    std::vector<U8Node> nodes;
    std::vector<uint8_t> strings;
    if (!ReadU8(arc, nodes, strings)) return false;
    std::map<std::string, std::string> renames;
    bool layout = false;
    for (U8Node& node : nodes) {
        if (node.type == 0 && node.path.size() > 6 && node.path.compare(node.path.size() - 6, 6, ".brlyt") == 0 &&
            node.path.find("pauseMenu_16") != std::string::npos) {
            layout = PatchPauseLayout(node.data, renames, pitch);
        }
    }
    if (!layout) return false;
    int animations = 0;
    for (U8Node& node : nodes) {
        if (node.type != 0 || node.path.size() < 6 || node.path.compare(node.path.size() - 6, 6, ".brlan") != 0) {
            continue;
        }
        // All of them: onButton, idleButton, offButon (sic) and hitButton for the copy's group, and the
        // window's own, which also hold tracks for Exit's panes.
        if (!PatchAnimation(node.data, renames, "B02_menuButton", kGroupName)) return false;
        ++animations;
    }
    if (animations < 4) return false;
    arc = WriteU8(nodes, strings);
    return true;
}

} // namespace

fs::path Patch(const std::string& dvdPath, const fs::path& hostPath) {
    std::string lower = dvdPath;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool map = lower == "/layout/corseselectmenu/corseselectmenu.arc";
    if (lower != "/layout/pausemenu/pausemenu.arc" && !map) {
        return {};
    }
    // The world map's code is only known in the disc's own build (hle/nsmbw_menu_settings.cpp); Newer
    // draws its own map, and its + menu.
    if (map && RuntimeProduct::Active().gameId != "SMNE01") {
        return {};
    }
    // NSMBW_NO_MENU_PATCH=1: the pause window as the disc has it (comparisons, tests). NSMBW_MENU_PATCH=1
    // patches it in deterministic mode too (a test seam for reproducible runs; LAN sessions never set it).
    const bool forced = std::getenv("NSMBW_MENU_PATCH") != nullptr;
    if ((DetClock::Enabled() && !forced) || std::getenv("NSMBW_NO_MENU_PATCH") != nullptr) {
        return {};
    }
    std::ifstream in(hostPath, std::ios::binary);
    std::vector<uint8_t> arc((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    float pitch = 70.0f;
    if (map) {
        if (arc.empty() || !PatchMapArchive(arc)) {
            RT_LOGF(RT_TAG_DVD, "layout patch: %s is not the menu this expects; it stays as it is\n", dvdPath.c_str());
            return {};
        }
        const fs::path dir = RuntimeConfigFile::ResolveConfigPath().parent_path() / "Cache" / "LayoutPatch";
        std::error_code ec;
        fs::create_directories(dir, ec);
        const fs::path out = dir / ("corseSelectMenu_" + RuntimeConfigFile::Language() + ".arc");
        std::ofstream file(out, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(arc.data()), static_cast<std::streamsize>(arc.size()));
        if (!file) {
            return {};
        }
        g_mapPatched.store(true);
        RT_LOGF(RT_TAG_DVD, "layout patch: the world map's menu gets its Settings button\n");
        return out;
    }
    if (arc.empty() || !PatchPauseArchive(arc, pitch)) {
        RT_LOGF(RT_TAG_DVD, "layout patch: %s is not the pause window this expects; it stays as it is\n", dvdPath.c_str());
        return {};
    }
    const fs::path dir = RuntimeConfigFile::ResolveConfigPath().parent_path() / "Cache" / "LayoutPatch";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path out = dir / ("pauseMenu_" + RuntimeConfigFile::Language() + ".arc");
    std::ofstream file(out, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(arc.data()), static_cast<std::streamsize>(arc.size()));
    if (!file) {
        return {};
    }
    g_pausePitch.store(pitch);
    g_pausePatched.store(true);
    RT_LOGF(RT_TAG_DVD, "layout patch: the pause window gets its Settings button (pitch %.0f)\n", pitch);
    return out;
}

bool PauseButtonPatched() noexcept {
    return g_pausePatched.load();
}

bool MapButtonPatched() noexcept {
    return g_mapPatched.load();
}

float PausePitch() noexcept {
    return g_pausePitch.load();
}

} // namespace LayoutPatch
