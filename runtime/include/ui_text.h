// ui_text.h - the port's own menus (netplay_ui.cpp) in the player's language. Text is looked up by its
// English wording, as the Android app's Lang.java does; anything without a translation stays English.
#pragma once

#include <string>
#include <string_view>

namespace UiText {

// `english` in the menus' language ([game] language), or `english` itself.
std::string Tr(std::string_view english);

// The menus follow a Language tab change at once (the game's own text only at the next start).
void SetLanguage(const std::string& code);

} // namespace UiText
