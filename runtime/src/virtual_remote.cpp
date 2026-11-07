#include "virtual_remote.h"

#include "input_bindings.h"
#include "keybinds.h"
#include "netplay/game_menus.h"
#include "netplay_session.h"
#include "pad_combos.h"
#include "prompt_style.h"
#include "runtime_log.h"
#include "settings_overlay.h"
#include "wii_remote_input.h"

#include <dolphin/pad.h>
#include <aurora/sdl_gamepad.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_sensor.h>
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>

namespace VirtualRemote {
namespace {

// WPAD_BUTTON_* bits as the game reads them from KPADStatus.hold.
constexpr uint32_t kWpadLeft = 0x0001, kWpadRight = 0x0002, kWpadDown = 0x0004, kWpadUp = 0x0008,
                   kWpadPlus = 0x0010, kWpadTwo = 0x0100, kWpadOne = 0x0200, kWpadA = 0x0800,
                   kWpadMinus = 0x1000;

// The remote's D-pad by screen direction. Held sideways with the D-pad under the left thumb, the
// pad's "up" arrow points left, "right" points up, "down" points right and "left" points down.
constexpr uint32_t kScreenLeft = kWpadUp, kScreenRight = kWpadDown, kScreenUp = kWpadRight,
                   kScreenDown = kWpadLeft;

// What the game's motion code looks for (dGameKeyCore_c::setShakeY and the tilt code after
// setConfigKey): a shake is acc.y changing by at least 0.28 g between consecutive frames on four
// frames (gaps of up to two allowed), then a five-frame cool-down; the tilt angle is -acc.z, through
// KPAD's acc_vertical, at 16384 angle units per g. A shake request swings y by +-1.2 g around rest
// for at least kShakeFrames reads, enough for one detection, and holding it keeps shaking.
constexpr float kShakeSwingG = 1.2f;
constexpr int kShakeFrames = 10;
constexpr float kMaxTiltRadians = 0.7853982f;  // 45 degrees
constexpr float kTiltFollow = 0.3f;            // share of the way to the requested tilt per frame
constexpr float kTiltDeadZone = 0.25f;         // of the right stick's travel
constexpr int kStickThreshold = 48;            // of aurora's +-127 stick range
constexpr float kDiagonalSlope = 0.4142136f;   // tan(22.5 degrees): eight-way stick sectors
constexpr int kTriggerThreshold = 128;         // of 255
// aurora's PADRead polls SDL; the game probes and reads all four channels each frame.
constexpr uint64_t kPadRefreshNs = 4'000'000;

struct Intent {
    uint32_t buttons = 0;  // WPAD bits, already in the sideways layout
    bool shake = false;
    float tilt = 0.0f;     // -1 counter-clockwise (left) .. 1 clockwise (right)
};

struct Motion {
    int shakeFramesLeft = 0;
    uint32_t shakePhase = 0;
    float tilt = 0.0f;
};

// Written by the Android UI thread (SetTouchState), read on the guest thread. The *SinceRead
// latches keep a press shorter than one 60 Hz input read (a quick tap) visible for one read.
std::atomic<uint32_t> g_touchButtons{0};
std::atomic<uint32_t> g_touchButtonsSinceRead{0};
std::atomic<bool> g_touchShake{false};
std::atomic<bool> g_touchShakeSinceRead{false};
std::atomic<float> g_touchTilt{0.0f};

std::mutex g_mutex;
std::array<PADStatus, PAD_MAX_CONTROLLERS> g_pads{};
uint64_t g_padsReadNs = 0;
bool g_keyboardOnAPort = false;
std::array<Motion, PAD_MAX_CONTROLLERS> g_motion{};
std::array<bool, PAD_MAX_CONTROLLERS> g_announced{};

void RefreshPadsLocked() {
    const uint64_t now = SDL_GetTicksNS();
    if (g_padsReadNs != 0 && now - g_padsReadNs < kPadRefreshNs) {
        return;
    }
    g_padsReadNs = now;
    PADRead(g_pads.data());
    PadCombos::Apply(g_pads.data());  // combinations first: they take their members' own buttons
    InputBindings::Apply(g_pads.data());
    g_keyboardOnAPort = false;
    for (uint32_t port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        uint32_t count = 0;
        if (PADGetKeyButtonBindings(port, &count) != nullptr) {
            g_keyboardOnAPort = true;
        }
    }
}

// NSMBW_SMART_SPIN_LOG: what Space Smart Spin QoL made of each press.
void SmartSpinLog(const char* what) {
    static const bool log = std::getenv("NSMBW_SMART_SPIN_LOG") != nullptr;
    if (log) {
        RT_LOGF(RT_TAG_RUNTIME, "smart spin: %s\n", what);
    }
}

// Mouse Gyro QoL (keybinds.h): the mouse as the remote's motion while no controller or Wii Remote is
// connected. A sideways swing (1% of the window's width in one read) tilts the remote that way for
// 5 s, long enough to ride a tilting platform across; a swing back the other way within 0.3 s is a
// shake, which also ends the tilt, so wiggling the mouse spins without leaving the remote tilted.
void ApplyMouseGyro(Intent& intent) {
    static float lastX = 0.0f;
    static bool haveLast = false;
    static int tiltDir = 0, swingDir = 0;
    static uint64_t tiltUntilNs = 0, swingNs = 0;
    SDL_Window* window = SDL_GetMouseFocus();
    bool realRemote = false;
    for (uint32_t chan = 0; chan < PAD_MAX_CONTROLLERS; ++chan) {
        realRemote = realRemote || WiiRemoteInput::HasRealRemote(chan);
    }
    if (!Keybinds::MouseGyro() || window == nullptr || realRemote || settings_overlay::ControllerConnected()) {
        haveLast = false;
        tiltDir = swingDir = 0;
        return;
    }
    float x = 0.0f;
    SDL_GetMouseState(&x, nullptr);
    int width = 0;
    SDL_GetWindowSize(window, &width, nullptr);
    const uint64_t now = SDL_GetTicksNS();
    const float threshold = std::max(4.0f, static_cast<float>(width) * 0.01f);
    if (haveLast && std::fabs(x - lastX) >= threshold) {
        const int dir = x < lastX ? -1 : 1;
        if (swingDir != 0 && dir != swingDir && now - swingNs < 300'000'000) {
            intent.shake = true;
            tiltDir = 0;
        } else {
            tiltDir = dir;
            tiltUntilNs = now + 5'000'000'000;
        }
        swingDir = dir;
        swingNs = now;
    }
    lastX = x;
    haveLast = true;
    if (intent.tilt == 0.0f && tiltDir != 0 && now < tiltUntilNs) {
        intent.tilt = static_cast<float>(tiltDir);
    }
}

// Smart Spin QoL: a press of the jump button it's set on (Space, or on the on-screen remote its 2) while
// the player is jumping or falling is a shake, a spin, rather than 2; the button then counts as no button
// until it is let go. Falling, a press within a small Mario's height (16 units) of the ground is meant
// as the next jump: the press waits that long at the current fall speed (2 to 7 frames), and becomes 2 if
// the player lands meanwhile, else the spin. On a springboard a press is always 2, and a spin's button
// still held when he lands on one becomes 2 held. The player's state is looked at on a press, during that
// wait and while a spin's button is held, never otherwise.
class SmartSpinButton {
public:
    void Update(bool down, bool applies, Intent& intent) {
        const uint64_t now = SDL_GetTicksNS();
        const auto player = [] { return NetplaySession::Active() ? NetplaySession::GetStatus().localSlot : 0; };
        if (down && !m_wasDown && Keybinds::SmartSpin() && applies) {
            float moving = 0.0f;
            m_is = Is::Two;
            if (GameMenus::PlayerInSpinnableAir(player(), &moving)) {
                if (moving >= 0.0f) {
                    m_is = Is::Spin;  // still rising: nowhere near the ground
                    intent.shake = true;
                    SmartSpinLog("rising: spin");
                } else {
                    constexpr float kSmallMarioHeight = 16.0f, kFrameNs = 16'666'667.0f;
                    // +1: the player's state shows the landing a frame after he touches down.
                    const float frames = std::clamp(kSmallMarioHeight / -moving + 1.0f, 2.0f, 7.0f);
                    m_is = Is::Waiting;
                    m_waitUntilNs = now + static_cast<uint64_t>(frames * kFrameNs);
                    SmartSpinLog("falling: waiting to see if he lands");
                }
            }
        } else if (m_is == Is::Spin && down) {
            // Held through the spin onto a springboard: it is 2 held there again, for the high
            // bounce holding the jump button gives.
            bool onSpringboard = false;
            GameMenus::PlayerInSpinnableAir(player(), nullptr, &onSpringboard);
            if (onSpringboard) {
                m_is = Is::Two;
                SmartSpinLog("held onto a springboard: 2 (the high bounce)");
            }
        } else if (m_is == Is::Waiting) {
            if (!GameMenus::PlayerInSpinnableAir(player())) {
                m_is = Is::Two;  // landed (or grabbed a wall...): the press was the next jump
                m_twoUntilNs = now + 40'000'000;  // a 2 press long enough to be read, held or not
                SmartSpinLog("landed in the margin: 2 (a jump)");
            } else if (now >= m_waitUntilNs) {
                m_is = Is::Spin;
                intent.shake = true;
                SmartSpinLog("still in the air: spin");
            }
        }
        if (!down && m_is != Is::Waiting) {
            m_is = Is::Two;
        }
        m_wasDown = down;
    }
    // Whether the button counts as 2 now; TwoPulse: 2 for a moment after a press that landed.
    bool IsTwo() const { return m_is == Is::Two; }
    bool TwoPulse() const { return SDL_GetTicksNS() < m_twoUntilNs; }

private:
    enum class Is { Two, Spin, Waiting };
    bool m_wasDown = false;
    Is m_is = Is::Two;
    uint64_t m_waitUntilNs = 0, m_twoUntilNs = 0;
};

// Actions held through combination bindings (1 << Keybinds::Action), as the remote.
void AddComboActions(uint32_t actions, Intent& intent) {
    using namespace Keybinds;
    const auto has = [actions](int action) { return (actions & (1u << action)) != 0; };
    if (has(kLeft)) intent.buttons |= kScreenLeft;
    if (has(kRight)) intent.buttons |= kScreenRight;
    if (has(kUp)) intent.buttons |= kScreenUp;
    if (has(kDown)) intent.buttons |= kScreenDown;
    if (has(kJump)) intent.buttons |= kWpadTwo;
    if (has(kRun)) intent.buttons |= kWpadOne;
    if (has(Keybinds::kA)) intent.buttons |= kWpadA;
    if (has(kPlus)) intent.buttons |= kWpadPlus;
    if (has(kMinus)) intent.buttons |= kWpadMinus;
    intent.shake = intent.shake || has(kShake);
    if (has(kTiltLeft) != has(kTiltRight)) intent.tilt = has(kTiltRight) ? 1.0f : -1.0f;
}

// The built-in layout for port 1, used while no port has aurora's keyboard switched on.
Intent KeyboardIntent() {
    Intent intent;
    if (SDL_GetKeyboardFocus() == nullptr && !Keybinds::Scripted()) {
        Keybinds::StepCombos(false);
        return intent;
    }
    // The keys come from the game's Keybinds tab (Config.toml [keyboard]); none count while one
    // is being picked there, or until the key picked is let go.
    if (Keybinds::Suppressed()) {
        Keybinds::StepCombos(false);
        return intent;
    }
    // Combinations first: Held leaves out the keys they take or keep waiting.
    Keybinds::StepCombos(true);
    using namespace Keybinds;
    if (Held(kLeft)) intent.buttons |= kScreenLeft;
    if (Held(kRight)) intent.buttons |= kScreenRight;
    if (Held(kUp)) intent.buttons |= kScreenUp;
    if (Held(kDown)) intent.buttons |= kScreenDown;
    static SmartSpinButton space;
    int keyCount = 0;
    const bool* keys = SDL_GetKeyboardState(&keyCount);
    const bool spaceDown = (keys != nullptr && SDL_SCANCODE_SPACE < keyCount && keys[SDL_SCANCODE_SPACE] &&
                            !Silenced(SDL_SCANCODE_SPACE)) ||
                           Tapped(SDL_SCANCODE_SPACE);
    space.Update(spaceDown, Bound(kJump, SDL_SCANCODE_SPACE), intent);
    const bool two = space.IsTwo() ? Held(kJump) || space.TwoPulse() : HeldIgnoring(kJump, SDL_SCANCODE_SPACE);
    if (two) intent.buttons |= kWpadTwo;
    if (Held(kRun)) intent.buttons |= kWpadOne;
    if (Held(Keybinds::kA)) intent.buttons |= kWpadA;
    if (Held(kPlus)) intent.buttons |= kWpadPlus;
    if (Held(kMinus)) intent.buttons |= kWpadMinus;
    intent.shake = intent.shake || Held(kShake);
    intent.tilt = (Held(kTiltRight) ? 1.0f : 0.0f) - (Held(kTiltLeft) ? 1.0f : 0.0f);
    ApplyMouseGyro(intent);
    // NSMBW_LOG_KEY_INTENT: the keyboard's remote whenever it changes (combination tests).
    static const bool log = std::getenv("NSMBW_LOG_KEY_INTENT") != nullptr;
    static uint32_t last = 0xFFFFFFFFu;
    const uint32_t now = intent.buttons | (intent.shake ? 0x10000u : 0u) |
                         (intent.tilt > 0.0f ? 0x20000u : intent.tilt < 0.0f ? 0x40000u : 0u);
    if (log && now != last) {
        last = now;
        RT_LOGF(RT_TAG_RUNTIME, "keyboard remote: buttons 0x%04X%s%s\n", intent.buttons, intent.shake ? " shake" : "",
                intent.tilt != 0.0f ? (intent.tilt > 0.0f ? " tilt right" : " tilt left") : "");
    }
    return intent;
}

// The on-screen remote and phone motion set through SetTouchState.
Intent TouchIntent() {
    Intent intent;
    const uint32_t touch = g_touchButtons.load(std::memory_order_relaxed) |
                           g_touchButtonsSinceRead.exchange(0, std::memory_order_relaxed);
    if (touch & kTouchLeft) intent.buttons |= kScreenLeft;
    if (touch & kTouchRight) intent.buttons |= kScreenRight;
    if (touch & kTouchUp) intent.buttons |= kScreenUp;
    if (touch & kTouchDown) intent.buttons |= kScreenDown;
    if (touch & kTouchOne) intent.buttons |= kWpadOne;
    static SmartSpinButton two;
    two.Update((touch & kTouchTwo) != 0, true, intent);
    if (two.IsTwo() ? (touch & kTouchTwo) != 0 || two.TwoPulse() : false) intent.buttons |= kWpadTwo;
    if (touch & kTouchA) intent.buttons |= kWpadA;
    if (touch & kTouchPlus) intent.buttons |= kWpadPlus;
    if (touch & kTouchMinus) intent.buttons |= kWpadMinus;
    if (touch & kTouchB) intent.buttons |= 0x0400;     // WPAD_BUTTON_B
    if (touch & kTouchHome) intent.buttons |= 0x8000;  // WPAD_BUTTON_HOME
    intent.shake = g_touchShake.load(std::memory_order_relaxed) ||
                   g_touchShakeSinceRead.exchange(false, std::memory_order_relaxed);
    intent.tilt = std::clamp(g_touchTilt.load(std::memory_order_relaxed), -1.0f, 1.0f);
    return intent;
}

// A GameCube-style pad state (after the user's remapping) as the sideways remote.
Intent PadIntent(const PADStatus& pad) {
    Intent intent;
    const uint32_t b = pad.button;
    if (b & PAD_BUTTON_LEFT) intent.buttons |= kScreenLeft;
    if (b & PAD_BUTTON_RIGHT) intent.buttons |= kScreenRight;
    if (b & PAD_BUTTON_UP) intent.buttons |= kScreenUp;
    if (b & PAD_BUTTON_DOWN) intent.buttons |= kScreenDown;
    const int sx = pad.stickX;
    const int sy = pad.stickY;  // +y up
    if (sx * sx + sy * sy >= kStickThreshold * kStickThreshold) {
        if (std::abs(sx) >= kDiagonalSlope * std::abs(sy)) intent.buttons |= sx < 0 ? kScreenLeft : kScreenRight;
        if (std::abs(sy) >= kDiagonalSlope * std::abs(sx)) intent.buttons |= sy < 0 ? kScreenDown : kScreenUp;
    }
    if (b & PAD_BUTTON_A) intent.buttons |= kWpadTwo;
    if (b & (PAD_BUTTON_B | PAD_BUTTON_X)) intent.buttons |= kWpadOne;
    if (b & PAD_BUTTON_Y) intent.buttons |= kWpadA;
    if (b & PAD_BUTTON_START) intent.buttons |= kWpadPlus;
    if ((pad.extButton & PAD_BUTTON_BACK) != 0 || (b & PAD_TRIGGER_L) != 0 || pad.triggerLeft >= kTriggerThreshold) {
        intent.buttons |= kWpadMinus;
    }
    intent.shake = (b & (PAD_TRIGGER_Z | PAD_TRIGGER_R)) != 0 || pad.triggerRight >= kTriggerThreshold;
    const float x = static_cast<float>(pad.substickX) / 127.0f;
    intent.tilt = std::fabs(x) < kTiltDeadZone ? 0.0f : std::clamp(x, -1.0f, 1.0f);
    return intent;
}

// A controller's own motion sensors as the remote's: turned like a steering wheel it tilts the remote,
// at the same angles as a phone (MotionInput.java), and a sharp jolt is a shake. SDL gives the
// accelerometer with gravity pulling +y on a controller lying flat and x to its right, so the turn
// shows up on x, however far the controller is pitched towards the player.
constexpr float kPadGravityFollow = 0.25f;     // share of each sample that enters gravity
constexpr float kPadShakeThreshold = 13.0f;    // m/s^2 of motion beyond gravity
constexpr uint64_t kPadShakeHoldMs = 180;      // long enough for the game's 4-frame test
constexpr float kPadTiltDeadZoneDeg = 4.0f;    // a slightly crooked grip does nothing
constexpr float kPadTiltFullDeg = 30.0f;       // the controller's angle that is a full remote tilt

struct PadMotion {
    SDL_JoystickID sensorsFor = 0;
    bool haveGravity = false;
    float gravity[3] = {};
    uint64_t shakeUntilMs = 0;
};
std::array<PadMotion, PAD_MAX_CONTROLLERS> g_padMotion{};
std::atomic<bool> g_p1ControllerMotion{false};

// Adds the sensors' tilt and shake to the intent; false if the port's controller has none.
bool ApplyControllerMotion(uint32_t chan, Intent& intent) {
    PadMotion& motion = g_padMotion[chan];
    const s32 index = PADGetIndexForPort(chan);
    SDL_Gamepad* gamepad = index >= 0 ? PADGetSDLGamepadForIndex(static_cast<u32>(index)) : nullptr;
    if (gamepad == nullptr || !aurora::sdl::GamepadHasSensor(gamepad, SDL_SENSOR_ACCEL)) {
        motion = {};
        return false;
    }
    const SDL_JoystickID id = aurora::sdl::GetGamepadID(gamepad);
    if (motion.sensorsFor != id) {
        if (!aurora::sdl::SetGamepadSensorEnabled(gamepad, SDL_SENSOR_ACCEL, true)) {
            return false;
        }
        motion = {};
        motion.sensorsFor = id;
    }
    float a[3] = {};
    if (!aurora::sdl::GetGamepadSensorData(gamepad, SDL_SENSOR_ACCEL, a, 3) || (a[0] == 0.0f && a[1] == 0.0f && a[2] == 0.0f)) {
        return true;  // nothing yet, but the controller has the sensors
    }
    if (!motion.haveGravity) {
        std::copy(a, a + 3, motion.gravity);
        motion.haveGravity = true;
    }
    for (int i = 0; i < 3; ++i) motion.gravity[i] += (a[i] - motion.gravity[i]) * kPadGravityFollow;
    const float lx = a[0] - motion.gravity[0], ly = a[1] - motion.gravity[1], lz = a[2] - motion.gravity[2];
    const uint64_t now = SDL_GetTicks();
    if (lx * lx + ly * ly + lz * lz > kPadShakeThreshold * kPadShakeThreshold) {
        motion.shakeUntilMs = now + kPadShakeHoldMs;
    }
    intent.shake = intent.shake || now < motion.shakeUntilMs;
    const float length = std::sqrt(motion.gravity[0] * motion.gravity[0] + motion.gravity[1] * motion.gravity[1] +
                                   motion.gravity[2] * motion.gravity[2]);
    if (length > 1.0f && intent.tilt == 0.0f) {  // the right stick, when it's in use, comes first
        // Turned clockwise, the right side drops and the reading on x goes negative.
        const float degrees = std::asin(std::clamp(-motion.gravity[0] / length, -1.0f, 1.0f)) * 57.29578f;
        const float magnitude = std::max(0.0f, std::fabs(degrees) - kPadTiltDeadZoneDeg);
        intent.tilt = std::copysign(std::min(1.0f, magnitude / (kPadTiltFullDeg - kPadTiltDeadZoneDeg)), degrees);
    }
    return true;
}

// Accelerometer for the requested motion, in KPAD's frame (rest: y = -1). Tilting clockwise lowers
// the right end and raises the left one, where the remote's pointer end is, so gravity shows up on
// the remote's long axis (KPAD z) as +sin(angle).
void ApplyMotion(Motion& motion, const Intent& intent, WiiRemoteInput::KpadSample& sample) {
    if (intent.shake) {
        motion.shakeFramesLeft = std::max(motion.shakeFramesLeft, kShakeFrames);
    }
    motion.tilt += (intent.tilt - motion.tilt) * kTiltFollow;
    if (std::fabs(motion.tilt) < 0.001f) {
        motion.tilt = 0.0f;
    }
    const float angle = motion.tilt * kMaxTiltRadians;
    sample.acc[0] = 0.0f;
    sample.acc[1] = -std::cos(angle);
    sample.acc[2] = std::sin(angle);
    if (motion.shakeFramesLeft > 0) {
        --motion.shakeFramesLeft;
        sample.acc[1] += (motion.shakePhase++ & 1u) != 0 ? kShakeSwingG : -kShakeSwingG;
    }
}

void AnnounceLocked(uint32_t chan, bool pad) {
    if (g_announced[chan]) {
        return;
    }
    g_announced[chan] = true;
    const char* source = "keyboard";
    if (pad) {
        const s32 index = PADGetIndexForPort(chan);
        SDL_Gamepad* gamepad = index >= 0 ? PADGetSDLGamepadForIndex(static_cast<u32>(index)) : nullptr;
        const char* name = gamepad != nullptr ? aurora::sdl::GetGamepadName(gamepad) : nullptr;
        source = name != nullptr ? name : g_keyboardOnAPort ? "keyboard (settings bindings)" : "controller";
    }
    RT_LOGF(RT_TAG_RUNTIME, "player %u: %s as a sideways Wii Remote\n", chan + 1, source);
}

} // namespace

void SetTouchState(uint32_t touchButtons, bool shake, float tilt) {
    g_touchButtons.store(touchButtons, std::memory_order_relaxed);
    g_touchButtonsSinceRead.fetch_or(touchButtons, std::memory_order_relaxed);
    g_touchShake.store(shake, std::memory_order_relaxed);
    if (shake) {
        g_touchShakeSinceRead.store(true, std::memory_order_relaxed);
    }
    g_touchTilt.store(std::isfinite(tilt) ? tilt : 0.0f, std::memory_order_relaxed);
}

bool ControllerMotion() {
    return g_p1ControllerMotion.load(std::memory_order_relaxed);
}

bool Present(uint32_t chan) {
    if (chan >= PAD_MAX_CONTROLLERS) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    RefreshPadsLocked();
    return chan == 0 || g_pads[chan].err == PAD_ERR_NONE;
}

bool Sample(uint32_t chan, WiiRemoteInput::KpadSample& sample) {
    if (chan >= PAD_MAX_CONTROLLERS) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    RefreshPadsLocked();
    const bool pad = g_pads[chan].err == PAD_ERR_NONE;
    if (chan != 0 && !pad) {
        return false;
    }
    AnnounceLocked(chan, pad);
    Intent intent;
    bool padMotion = false;
    if (pad) {
        intent = PadIntent(g_pads[chan]);
        AddComboActions(PadCombos::Actions(chan), intent);
        PromptStyle::NoteController(chan, intent.buttons);  // the prompts show this controller's buttons
        padMotion = ApplyControllerMotion(chan, intent);
    }
    if (chan == 0) {
        g_p1ControllerMotion.store(padMotion, std::memory_order_relaxed);
    }
    const auto merge = [&intent](const Intent& other) {
        intent.buttons |= other.buttons;
        intent.shake = intent.shake || other.shake;
        if (other.tilt != 0.0f) {
            intent.tilt = other.tilt;
        }
    };
    if (chan == 0 && !g_keyboardOnAPort) {
        const Intent keyboard = KeyboardIntent();
        PromptStyle::NoteKeyboard(keyboard.buttons);  // the prompts show the keys
        merge(keyboard);
    }
    if (chan == 0) {
        Intent touch = TouchIntent();
        PromptStyle::NoteWiiRemote(PromptStyle::kTouchSource, touch.buttons);  // the on-screen remote is a Wii Remote
        if (padMotion) {
            touch.tilt = 0.0f;  // the controller's own sensors, over the phone's (ControllerMotion)
        }
        merge(touch);
    }
    // Reverse Tilt QoL: whatever the tilt came from (motion sensors, a stick, keys), mirrored.
    if (Keybinds::ReverseTilt()) {
        intent.tilt = -intent.tilt;
    }
    if (InputBindings::InputBlocked()) {
        intent = {};  // the settings overlay owns input; the remote stays connected at rest
    }
    sample = {};
    sample.hold = intent.buttons;
    ApplyMotion(g_motion[chan], intent, sample);
    return true;
}

} // namespace VirtualRemote
