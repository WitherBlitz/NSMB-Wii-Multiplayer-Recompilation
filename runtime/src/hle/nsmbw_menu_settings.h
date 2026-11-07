#pragma once

// The pause window's Settings button (hle/nsmbw_menu_settings.cpp) and the settings window
// (netplay/netplay_ui.cpp) hand over to each other through these.
namespace MenuSettings {

// The pause window has the Settings button (its layout is patched).
bool PauseButtonActive() noexcept;
// The button was pressed since the last call: the settings window opens once the pause window has
// closed (PauseWindowClosed).
bool TakeOpenRequest() noexcept;
bool PauseWindowClosed() noexcept;
// The settings window closed: the pause window opens again, Settings selected.
void ReopenPauseWindow() noexcept;
// The world map's + menu has the Settings button; it was picked since the last call (the menu has gone
// out: the settings window opens).
bool MapButtonActive() noexcept;
bool TakeMapOpenRequest() noexcept;
// The settings window opened from the world map's menu has closed: the menu opens again, Settings picked.
void ReopenMapMenu() noexcept;

} // namespace MenuSettings
