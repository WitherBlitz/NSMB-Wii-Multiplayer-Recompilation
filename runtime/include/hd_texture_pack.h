#pragma once

#include <string>

// The Settings window's Mods tab: "New Super Mario Bros. Wii HD Texture Pack" v2.81 by Bighead (with
// razius, jaco8, CDAMJC and Amon1995), its PNG edition. It is downloaded from the pack's own links (Google
// Drive, else its MediaFire mirror), checked, unpacked with the LZMA SDK's 7z decoder into the folder
// aurora reads Dolphin-style replacement textures from (<user data>/texture_replacements), and switched on
// and off with [video] texture_replacements, at once.
namespace HdTexturePack {

enum class State {
    NotInstalled,
    Downloading,
    Installing,  // unpacking
    Installed,
    Failed,      // message says why; Download tries again
};

struct Status {
    State state = State::NotInstalled;
    float progress = -1;   // 0..1 while downloading, -1 unknown
    std::string message;   // why it failed, in English (UiText::Tr)
};

Status Get();
// Downloads and installs the pack in the background (again after a failure).
void Download();
// True once after an install finished: the game thread turns the pack on (SetEnabled), as the config
// file is the game thread's.
bool TakeJustInstalled();
bool Enabled();
// On / Off: saved in Config.toml and applied from the next frame (aurora_set_texture_replacements).
void SetEnabled(bool on);

} // namespace HdTexturePack
