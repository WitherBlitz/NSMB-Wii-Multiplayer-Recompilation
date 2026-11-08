#pragma once

#include <aurora/aurora.h>
#include <aurora/event.h>

#include <string>
#include <vector>

namespace settings_overlay {
// Apply persistent controller settings once Aurora has discovered host devices.
void InitializeRuntimeSettings() noexcept;
// Draw the F10 settings bar before each Aurora present.
void HandleEvents(const AuroraEvent* events) noexcept;
void Draw() noexcept;
bool StartupScreenVisible() noexcept;
void NotifyStrapInputAccepted() noexcept;
void AdvancePresentedFrame() noexcept;
// Before the game starts: when aurora holds its cached pipelines back (drivers that stall a present
// until every shader handed to them is compiled), shows a "Loading shaders" screen and feeds them to
// the driver a batch at a time, returning once they are all built. Does nothing otherwise.
void RunShaderLoadingScreen() noexcept;
// Put host controllers back to a neutral state before the process ends.
void ReleaseControllers() noexcept;
// The Performance Monitor: the FPS counter and a frame-time graph in the top right, off by
// default. Saved to Config.toml ([video] frame_graph; the old show_fps also turns it on).
void SetFrameGraph(bool show) noexcept;
bool FrameGraph() noexcept;
// The Performance Monitor's size in percent, 5-200 (50 is the graph's original size), its text
// included. `save` writes Config.toml (a slider being dragged saves when it is let go).
void SetFrameGraphSize(int percent, bool save) noexcept;
// The Keybinds tab with a controller connected (Controller Bindings): its rows are the keyboard's
// actions (Keybinds::Action), bound to that controller's buttons through the [controller] mapping.
bool ControllerConnected() noexcept;
std::string ControllerBindingLabel(int action);
// The controller buttons (aurora's PAD native encoding) of the action's first binding: its button, else
// its first combination's members; none without a controller. The button prompts show these.
std::vector<uint32_t> ControllerFirstBinding(int action);
bool ControllerActionRebindable(int action) noexcept;
// Waits for the next controller button pressed and binds it to `action`; Escape or 10 s cancel.
void BeginControllerCapture(int action) noexcept;
int ControllerCapturing() noexcept;  // the action waiting for a button, or -1
std::string ControllerCaptureLabel();  // the buttons held so far in it, "LB / L1 + A / Cross", or ""
bool PollControllerCapture() noexcept;  // true once the capture has ended
void ResetControllerBindings() noexcept;
// A fresh press of the controller's L (-1) or R (+1) shoulder button since the last call, else 0:
// the settings window steps between its tabs with them.
int ControllerShoulderPressed() noexcept;
// Android's on-screen controls (the app shows or hides them; the Keybinds tab's checkbox asks it to).
void SetTouchControls(bool on) noexcept;
bool TouchControls() noexcept;
int FrameGraphSize() noexcept;
// The renderer: [video] graphics_api as chosen for the next start ("auto", "vulkan", "d3d12", "d3d11",
// "opengles"...), and the one this run uses ("Vulkan", "Direct3D 11"...).
std::string GraphicsApiChoice();
void SetGraphicsApiChoice(const std::string& name) noexcept;
const char* RunningGraphicsApi() noexcept;
// V-Sync: presents wait for the display, and the game's clock follows its refresh (hle/vi.cpp).
// Off by default. Saved to Config.toml.
void SetVSync(bool enabled) noexcept;
bool VSync() noexcept;
// Battery Saver: the game keeps its 60 steps a second but draws and presents every other one, so the
// screen shows 30 FPS (DisplaySettings' dSys_c::execute port). Frame interpolation is off while it is
// on. Off by default. Saved to Config.toml.
void SetBatterySaver(bool enabled) noexcept;
bool BatterySaver() noexcept;
// Battery Saver on and not paused: lockstep (LAN) play draws every frame, as every peer must run the
// same guest code.
bool BatterySaverActive() noexcept;
// A frame went undrawn (its draw lists left out): the next display copy (GXCopyDisp) would show an empty
// picture, so it is dropped and the screen keeps the last one.
void MarkUndrawnFrame() noexcept;
bool TakeUndrawnFrame() noexcept;
// Fullscreen: 0 off (a window), 1 borderless, 2 exclusive (the display's own mode). Applied at once;
// saved to Config.toml. Desktop only: Android is always fullscreen.
void SetFullscreen(int mode) noexcept;
int Fullscreen() noexcept;
// One of the game's own system sounds (SndAudioMgr::startSystemSe; 121 is the "decide" the file
// select plays for a save slot), played from the game's main thread at its next controller read.
// Not in lockstep (LAN) play, where it would change one machine's game state.
constexpr uint32_t kGameSoundCursor = 120;   // SE_SYS_CURSOR: a move, or a value changed
constexpr uint32_t kGameSoundDecide = 121;   // SE_SYS_DECIDE
constexpr uint32_t kGameSoundBack = 122;     // SE_SYS_BACK: closed, cancelled, cleared
constexpr uint32_t kGameSoundInvalid = 123;  // SE_SYS_INVALID: a choice that can't be taken
constexpr uint32_t kGameSoundPage = 125;     // SE_SYS_PAGE: another tab
// Queued: up to 8 between two reads, each id once.
void PlayGameSound(uint32_t id) noexcept;
// KPADReadEx (hle/input/kpad.cpp), on the main thread: plays the sound PlayGameSound queued.
void FlushGameSound() noexcept;
// Animated tiles (spinning coins, ? blocks...), on by default. Saved to Config.toml.
void SetSpinningCoins(bool enabled) noexcept;
bool SpinningCoins() noexcept;
// Depth Blur, on by default: the courses' depth of field and heat-haze / underwater shimmer
// (hle/gx/depth_blur.cpp). Render-only, applied at once. Saved as [video] disabled_post_processing_paths.
void SetDepthBlur(bool enabled) noexcept;
bool DepthBlur() noexcept;
// Render scale: 1 = 480 lines, 0 = the window's (screen's) own resolution. Saved to Config.toml.
void SetRenderScale(float scale) noexcept;
float RenderScale() noexcept;
// DisplaySettings' aspect (4:3, 16:9, Fill), including a change not yet applied. Saved to Config.toml.
void SetAspect(int32_t mode) noexcept;
int32_t Aspect() noexcept;
// The volumes in percent, 0 to 100: everything (the speakers' master volume), the music, the
// sound effects and the menus' sounds. Applied at once; save writes them to Config.toml (a slider
// being dragged saves when it is let go).
enum class Volume { Master, Music, SoundEffects, MenuSounds };
void SetVolumePercent(Volume which, int percent, bool save) noexcept;
int VolumePercent(Volume which) noexcept;
} // namespace settings_overlay
