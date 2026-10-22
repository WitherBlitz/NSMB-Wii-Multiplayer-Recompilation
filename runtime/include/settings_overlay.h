#pragma once

#include <aurora/aurora.h>
#include <aurora/event.h>

namespace settings_overlay {
// Apply persistent controller settings once Aurora has discovered host devices.
void InitializeRuntimeSettings() noexcept;
// Draw the F10 settings bar before each Aurora present.
void HandleEvents(const AuroraEvent* events) noexcept;
void Draw() noexcept;
bool StartupScreenVisible() noexcept;
void NotifyStrapInputAccepted() noexcept;
void AdvancePresentedFrame() noexcept;
// Put host controllers back to a neutral state before the process ends.
void ReleaseControllers() noexcept;
// FPS counter on or off (the Android menu sets it through JNI); saved to Config.toml.
void SetShowFps(bool show) noexcept;
bool ShowFps() noexcept;
// A frame-time graph under the FPS counter, off by default. Saved to Config.toml.
void SetFrameGraph(bool show) noexcept;
bool FrameGraph() noexcept;
// The frametime graph's size in percent, 5-200 (50 is its original size). `save` writes Config.toml
// (a slider being dragged saves when it is let go).
void SetFrameGraphSize(int percent, bool save) noexcept;
// Android's on-screen controls (the app shows or hides them; the Keybinds tab's checkbox asks it to).
void SetTouchControls(bool on) noexcept;
bool TouchControls() noexcept;
int FrameGraphSize() noexcept;
// V-Sync: presents wait for the display, and the game's clock follows its refresh (hle/vi.cpp).
// Off by default. Saved to Config.toml.
void SetVSync(bool enabled) noexcept;
bool VSync() noexcept;
// Fullscreen: 0 off (a window), 1 borderless, 2 exclusive (the display's own mode). Applied at once;
// saved to Config.toml. Desktop only: Android is always fullscreen.
void SetFullscreen(int mode) noexcept;
int Fullscreen() noexcept;
// One of the game's own system sounds (SndAudioMgr::startSystemSe; 121 is the "decide" the file
// select plays for a save slot), played from the game's main thread at its next controller read.
// Not in lockstep (LAN) play, where it would change one machine's game state.
constexpr uint32_t kGameSoundDecide = 121;
void PlayGameSound(uint32_t id) noexcept;
// KPADReadEx (hle/input/kpad.cpp), on the main thread: plays the sound PlayGameSound queued.
void FlushGameSound() noexcept;
// Animated tiles (spinning coins, ? blocks...), on by default. Saved to Config.toml.
void SetSpinningCoins(bool enabled) noexcept;
bool SpinningCoins() noexcept;
// Render scale: 1 = 480 lines, 0 = the window's (screen's) own resolution. Saved to Config.toml.
void SetRenderScale(float scale) noexcept;
float RenderScale() noexcept;
// DisplaySettings' aspect (4:3, 16:9, Fill), including a change not yet applied. Saved to Config.toml.
void SetAspect(int32_t mode) noexcept;
int32_t Aspect() noexcept;
// The volumes in percent, 0 to 100: everything (the speakers' master volume), the music, and the
// sound effects. Applied at once; save writes them to Config.toml (a slider being dragged saves
// when it is let go).
enum class Volume { Master, Music, SoundEffects };
void SetVolumePercent(Volume which, int percent, bool save) noexcept;
int VolumePercent(Volume which) noexcept;
} // namespace settings_overlay
