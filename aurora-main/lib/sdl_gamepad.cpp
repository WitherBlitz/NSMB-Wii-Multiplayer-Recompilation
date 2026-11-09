// The input thread and the gamepad calls that never wait for it (include/aurora/sdl_gamepad.h).
#include "input.hpp"
#include "internal.hpp"

#include <aurora/sdl_gamepad.h>

#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_stdinc.h>

#include <absl/container/flat_hash_map.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace aurora::input {
namespace {
#if defined(_WIN32)
constexpr bool kInputThread = true;
#else
constexpr bool kInputThread = false;
#endif

// How long a frame waits for its joystick update. A normal update (reading the open controllers)
// took under 0.1 ms in 99.8% of 7800 on PLEXPC (at most 2 ms); a re-scan takes hundreds, and the
// frame goes on without it (once per re-scan, the frame that started it is this much longer).
constexpr auto kUpdateWait = std::chrono::milliseconds(2);
// The input thread also updates on its own when no frame asked for this long (paused, loading).
constexpr auto kIdleUpdate = std::chrono::milliseconds(100);

// Held by the input thread for each SDL_UpdateJoysticks, and by other threads around their SDL
// gamepad calls (try_lock only, so they never wait for an update).
std::recursive_mutex g_gate;

struct UpdateThread {
  std::thread thread;
  std::mutex mutex;
  std::condition_variable kick;
  std::condition_variable done;
  uint64_t requested = 0; // frames that asked for an update
  uint64_t completed = 0; // last request an update finished for
  bool quit = false;
} g_update;

// Gamepads the input thread opened ahead of the game (opening the first one starts SDL's sensor
// subsystem, 5-170 ms); its reference is dropped once the game has opened its own.
absl::flat_hash_map<SDL_JoystickID, SDL_Gamepad*> g_preopened;
std::mutex g_takenMutex;
std::vector<SDL_JoystickID> g_taken;

// Last result of each read, by function, object and argument.
enum class Fn : uint8_t {
  Button, Axis, Joystick, FromPlayer, FromId, Id, Name, Type, Player, Vendor, Product, HasSensor,
  SensorEnabled, SensorData, PadPower, JoyButton, JoyAxis, JoyButtons, JoyAxes, JoyPower,
};
struct Key {
  Fn fn;
  const void* object;
  int64_t arg;
  bool operator==(const Key&) const = default;
  template <typename H>
  friend H AbslHashValue(H h, const Key& key) {
    return H::combine(std::move(h), key.fn, key.object, key.arg);
  }
};
struct Value {
  uint64_t bits = 0;
  int extra = 0;               // percent for power info
  std::array<float, 8> data{}; // sensor data
};
std::mutex g_memoMutex;
absl::flat_hash_map<Key, Value> g_memo;

std::mutex g_writesMutex;
std::vector<std::pair<SDL_Gamepad*, std::function<void()>>> g_writes;

template <typename T>
uint64_t to_bits(T value) {
  static_assert(sizeof(T) <= sizeof(uint64_t));
  uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(T));
  return bits;
}

template <typename T>
T from_bits(uint64_t bits) {
  T value;
  std::memcpy(&value, &bits, sizeof(T));
  return value;
}

void remember(const Key& key, const Value& value) {
  std::lock_guard lock(g_memoMutex);
  g_memo[key] = value;
}

Value recall(const Key& key) {
  std::lock_guard lock(g_memoMutex);
  const auto it = g_memo.find(key);
  return it != g_memo.end() ? it->second : Value{};
}

// The gate is taken only if free; on other platforms it is never contended.
struct Gate {
  bool owned = kInputThread ? g_gate.try_lock() : true;
  Gate() = default;
  Gate(const Gate&) = delete;
  Gate& operator=(const Gate&) = delete;
  ~Gate() {
    if (kInputThread && owned) {
      g_gate.unlock();
    }
  }
  explicit operator bool() const { return owned; }
};

template <typename T, typename Call>
T read(Fn fn, const void* object, int64_t arg, Call&& call) {
  if constexpr (!kInputThread) {
    return call();
  } else {
    const Key key{fn, object, arg};
    if (Gate gate; gate) {
      const T value = call();
      remember(key, Value{.bits = to_bits(value)});
      return value;
    }
    return from_bits<T>(recall(key).bits);
  }
}

// Power state and percentage; an unknown one reads as SDL_POWERSTATE_UNKNOWN, -1 %.
template <typename Call>
SDL_PowerState read_power(Fn fn, const void* object, int* percent, Call&& call) {
  Value value{.bits = to_bits(SDL_POWERSTATE_UNKNOWN), .extra = -1};
  if constexpr (!kInputThread) {
    value.bits = to_bits(call(&value.extra));
  } else if (Gate gate; gate) {
    value.bits = to_bits(call(&value.extra));
    remember(Key{fn, object, 0}, value);
  } else {
    std::lock_guard lock(g_memoMutex);
    if (const auto it = g_memo.find(Key{fn, object, 0}); it != g_memo.end()) {
      value = it->second;
    }
  }
  if (percent != nullptr) {
    *percent = value.extra;
  }
  return from_bits<SDL_PowerState>(value.bits);
}

void apply_queued_writes() {
  std::vector<std::pair<SDL_Gamepad*, std::function<void()>>> writes;
  {
    std::lock_guard lock(g_writesMutex);
    writes.swap(g_writes);
  }
  for (auto& [gamepad, apply] : writes) {
    apply();
  }
}

// Runs a write now if the gate is free (after any queued ones, to keep their order), else queues it.
bool write(SDL_Gamepad* gamepad, std::function<void()> apply) {
  if (gamepad == nullptr) {
    return false;
  }
  if (Gate gate; gate) {
    apply_queued_writes();
    apply();
    return true;
  }
  std::lock_guard lock(g_writesMutex);
  g_writes.emplace_back(gamepad, std::move(apply));
  return true;
}

// Opens gamepads that appeared, drops the input thread's reference to those the game opened or
// that went away. Gate held.
void preopen_gamepads() {
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  const auto present = [&](SDL_JoystickID id) {
    return ids != nullptr && std::find(ids, ids + count, id) != ids + count;
  };
  {
    std::lock_guard lock(g_takenMutex);
    const auto taken = [](SDL_JoystickID id) {
      return std::find(g_taken.begin(), g_taken.end(), id) != g_taken.end();
    };
    for (auto it = g_preopened.begin(); it != g_preopened.end();) {
      if (taken(it->first) || !present(it->first)) {
        SDL_CloseGamepad(it->second);
        g_preopened.erase(it++);
      } else {
        ++it;
      }
    }
    for (int i = 0; i < count; ++i) {
      if (!g_preopened.contains(ids[i]) && !taken(ids[i])) {
        if (SDL_Gamepad* gamepad = SDL_OpenGamepad(ids[i])) {
          g_preopened.emplace(ids[i], gamepad);
        }
      }
    }
    // Forget ids the game took that are gone.
    std::erase_if(g_taken, [&](SDL_JoystickID id) { return !present(id); });
  }
  SDL_free(ids);
}

void update_thread_main() {
  SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH);
  uint64_t served = 0;
  std::unique_lock lock(g_update.mutex);
  while (!g_update.quit) {
    g_update.kick.wait_for(lock, kIdleUpdate, [&] { return g_update.quit || g_update.requested != served; });
    if (g_update.quit) {
      break;
    }
    const uint64_t request = g_update.requested;
    lock.unlock();
    {
      std::lock_guard gate(g_gate);
      SDL_UpdateJoysticks();
      preopen_gamepads();
      apply_queued_writes();
    }
    lock.lock();
    served = request;
    g_update.completed = request;
    g_update.done.notify_all();
  }
}
} // namespace

void start_update_thread() noexcept {
  if constexpr (kInputThread) {
    // SDL_PumpEvents (the game thread) no longer updates joysticks; the input thread does.
    SDL_SetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "0");
    g_update.thread = std::thread(update_thread_main);
  }
}

void stop_update_thread() noexcept {
  if constexpr (kInputThread) {
    if (!g_update.thread.joinable()) {
      return;
    }
    {
      std::lock_guard lock(g_update.mutex);
      g_update.quit = true;
    }
    g_update.kick.notify_all();
    g_update.thread.join();
    std::lock_guard gate(g_gate);
    apply_queued_writes();
    for (auto& [id, gamepad] : g_preopened) {
      SDL_CloseGamepad(gamepad);
    }
    g_preopened.clear();
    SDL_SetHint(SDL_HINT_AUTO_UPDATE_JOYSTICKS, "1");
  }
}

void sync_update() noexcept {
  if constexpr (kInputThread) {
    if (!g_update.thread.joinable()) {
      return;
    }
    std::unique_lock lock(g_update.mutex);
    if (g_update.completed != g_update.requested) {
      return; // still busy with an earlier request (a re-scan): don't wait for it
    }
    const uint64_t request = ++g_update.requested;
    g_update.kick.notify_one();
    g_update.done.wait_for(lock, kUpdateWait, [&] { return g_update.completed >= request; });
  }
}

bool try_lock_gate() noexcept { return kInputThread ? g_gate.try_lock() : true; }

void unlock_gate() noexcept {
  if constexpr (kInputThread) {
    g_gate.unlock();
  }
}

void note_opened(SDL_JoystickID instance) noexcept {
  if constexpr (kInputThread) {
    std::lock_guard lock(g_takenMutex);
    if (std::find(g_taken.begin(), g_taken.end(), instance) == g_taken.end()) {
      g_taken.push_back(instance);
    }
  }
}

void forget_gamepad(SDL_Gamepad* gamepad) noexcept {
  if constexpr (kInputThread) {
    const void* joystick = SDL_GetGamepadJoystick(gamepad); // gate held by the caller
    {
      std::lock_guard lock(g_memoMutex);
      absl::erase_if(g_memo, [&](const auto& entry) {
        return entry.first.object == gamepad || entry.first.object == joystick ||
               ((entry.first.fn == Fn::FromPlayer || entry.first.fn == Fn::FromId) &&
                from_bits<SDL_Gamepad*>(entry.second.bits) == gamepad);
      });
    }
    std::lock_guard lock(g_writesMutex);
    std::erase_if(g_writes, [&](const auto& write) { return write.first == gamepad; });
  }
}
} // namespace aurora::input

namespace aurora::sdl {
using input::Fn;
using input::read;

bool GetGamepadButton(SDL_Gamepad* gamepad, SDL_GamepadButton button) noexcept {
  return read<bool>(Fn::Button, gamepad, button, [&] { return SDL_GetGamepadButton(gamepad, button); });
}

Sint16 GetGamepadAxis(SDL_Gamepad* gamepad, SDL_GamepadAxis axis) noexcept {
  return read<Sint16>(Fn::Axis, gamepad, axis, [&] { return SDL_GetGamepadAxis(gamepad, axis); });
}

SDL_Joystick* GetGamepadJoystick(SDL_Gamepad* gamepad) noexcept {
  return read<SDL_Joystick*>(Fn::Joystick, gamepad, 0, [&] { return SDL_GetGamepadJoystick(gamepad); });
}

SDL_Gamepad* GetGamepadFromPlayerIndex(int playerIndex) noexcept {
  return read<SDL_Gamepad*>(Fn::FromPlayer, nullptr, playerIndex,
                            [&] { return SDL_GetGamepadFromPlayerIndex(playerIndex); });
}

SDL_Gamepad* GetGamepadFromID(SDL_JoystickID instance) noexcept {
  return read<SDL_Gamepad*>(Fn::FromId, nullptr, instance, [&] { return SDL_GetGamepadFromID(instance); });
}

SDL_JoystickID GetGamepadID(SDL_Gamepad* gamepad) noexcept {
  return read<SDL_JoystickID>(Fn::Id, gamepad, 0, [&] { return SDL_GetGamepadID(gamepad); });
}

const char* GetGamepadName(SDL_Gamepad* gamepad) noexcept {
  return read<const char*>(Fn::Name, gamepad, 0, [&] { return SDL_GetGamepadName(gamepad); });
}

SDL_GamepadType GetGamepadType(SDL_Gamepad* gamepad) noexcept {
  return read<SDL_GamepadType>(Fn::Type, gamepad, 0, [&] { return SDL_GetGamepadType(gamepad); });
}

int GetGamepadPlayerIndex(SDL_Gamepad* gamepad) noexcept {
  // An unknown gamepad reads as "no player" (-1), not port 0.
  if constexpr (input::kInputThread) {
    const input::Key key{Fn::Player, gamepad, 0};
    if (input::Gate gate; gate) {
      const int value = SDL_GetGamepadPlayerIndex(gamepad);
      input::remember(key, input::Value{.bits = input::to_bits(value)});
      return value;
    }
    std::lock_guard lock(input::g_memoMutex);
    const auto it = input::g_memo.find(key);
    return it != input::g_memo.end() ? input::from_bits<int>(it->second.bits) : -1;
  } else {
    return SDL_GetGamepadPlayerIndex(gamepad);
  }
}

Uint16 GetGamepadVendor(SDL_Gamepad* gamepad) noexcept {
  return read<Uint16>(Fn::Vendor, gamepad, 0, [&] { return SDL_GetGamepadVendor(gamepad); });
}

Uint16 GetGamepadProduct(SDL_Gamepad* gamepad) noexcept {
  return read<Uint16>(Fn::Product, gamepad, 0, [&] { return SDL_GetGamepadProduct(gamepad); });
}

bool GamepadHasSensor(SDL_Gamepad* gamepad, SDL_SensorType type) noexcept {
  return read<bool>(Fn::HasSensor, gamepad, type, [&] { return SDL_GamepadHasSensor(gamepad, type); });
}

bool GamepadSensorEnabled(SDL_Gamepad* gamepad, SDL_SensorType type) noexcept {
  return read<bool>(Fn::SensorEnabled, gamepad, type, [&] { return SDL_GamepadSensorEnabled(gamepad, type); });
}

bool GetGamepadSensorData(SDL_Gamepad* gamepad, SDL_SensorType type, float* data, int numValues) noexcept {
  if constexpr (!input::kInputThread) {
    return SDL_GetGamepadSensorData(gamepad, type, data, numValues);
  } else {
    numValues = std::clamp(numValues, 0, 8);
    const input::Key key{Fn::SensorData, gamepad, type};
    if (input::Gate gate; gate) {
      input::Value value;
      const bool ok = SDL_GetGamepadSensorData(gamepad, type, value.data.data(), numValues);
      value.bits = ok;
      input::remember(key, value);
      std::copy_n(value.data.begin(), numValues, data);
      return ok;
    }
    const input::Value value = input::recall(key);
    std::copy_n(value.data.begin(), numValues, data);
    return value.bits != 0;
  }
}

SDL_PowerState GetGamepadPowerInfo(SDL_Gamepad* gamepad, int* percent) noexcept {
  return input::read_power(Fn::PadPower, gamepad, percent,
                           [&](int* p) { return SDL_GetGamepadPowerInfo(gamepad, p); });
}

bool GetJoystickButton(SDL_Joystick* joystick, int button) noexcept {
  return read<bool>(Fn::JoyButton, joystick, button, [&] { return SDL_GetJoystickButton(joystick, button); });
}

Sint16 GetJoystickAxis(SDL_Joystick* joystick, int axis) noexcept {
  return read<Sint16>(Fn::JoyAxis, joystick, axis, [&] { return SDL_GetJoystickAxis(joystick, axis); });
}

int GetNumJoystickButtons(SDL_Joystick* joystick) noexcept {
  return read<int>(Fn::JoyButtons, joystick, 0, [&] { return SDL_GetNumJoystickButtons(joystick); });
}

int GetNumJoystickAxes(SDL_Joystick* joystick) noexcept {
  return read<int>(Fn::JoyAxes, joystick, 0, [&] { return SDL_GetNumJoystickAxes(joystick); });
}

SDL_PowerState GetJoystickPowerInfo(SDL_Joystick* joystick, int* percent) noexcept {
  return input::read_power(Fn::JoyPower, joystick, percent,
                           [&](int* p) { return SDL_GetJoystickPowerInfo(joystick, p); });
}

bool RumbleGamepad(SDL_Gamepad* gamepad, Uint16 lowFrequency, Uint16 highFrequency, Uint32 durationMs) noexcept {
  if constexpr (!input::kInputThread) {
    return SDL_RumbleGamepad(gamepad, lowFrequency, highFrequency, durationMs);
  } else {
    return input::write(gamepad, [=] { SDL_RumbleGamepad(gamepad, lowFrequency, highFrequency, durationMs); });
  }
}

bool SetGamepadLED(SDL_Gamepad* gamepad, Uint8 red, Uint8 green, Uint8 blue) noexcept {
  if constexpr (!input::kInputThread) {
    return SDL_SetGamepadLED(gamepad, red, green, blue);
  } else {
    return input::write(gamepad, [=] { SDL_SetGamepadLED(gamepad, red, green, blue); });
  }
}

bool SetGamepadSensorEnabled(SDL_Gamepad* gamepad, SDL_SensorType type, bool enabled) noexcept {
  if constexpr (!input::kInputThread) {
    return SDL_SetGamepadSensorEnabled(gamepad, type, enabled);
  } else {
    // The caller learns of a failure now when the gate is free (as before); a queued one is retried
    // by callers that check SDL_GamepadSensorEnabled.
    if (input::Gate gate; gate) {
      input::apply_queued_writes();
      return SDL_SetGamepadSensorEnabled(gamepad, type, enabled);
    }
    return input::write(gamepad, [=] { SDL_SetGamepadSensorEnabled(gamepad, type, enabled); });
  }
}

bool SetGamepadPlayerIndex(SDL_Gamepad* gamepad, int playerIndex) noexcept {
  if constexpr (!input::kInputThread) {
    return SDL_SetGamepadPlayerIndex(gamepad, playerIndex);
  } else {
    if (input::Gate gate; gate) {
      input::apply_queued_writes();
      const bool ok = SDL_SetGamepadPlayerIndex(gamepad, playerIndex);
      input::remember(input::Key{Fn::Player, gamepad, 0}, input::Value{.bits = input::to_bits(playerIndex)});
      return ok;
    }
    input::remember(input::Key{Fn::Player, gamepad, 0}, input::Value{.bits = input::to_bits(playerIndex)});
    return input::write(gamepad, [=] { SDL_SetGamepadPlayerIndex(gamepad, playerIndex); });
  }
}

bool SendGamepadEffect(SDL_Gamepad* gamepad, const void* data, int size) noexcept {
  if constexpr (!input::kInputThread) {
    return SDL_SendGamepadEffect(gamepad, data, size);
  } else {
    if (input::Gate gate; gate) {
      input::apply_queued_writes();
      return SDL_SendGamepadEffect(gamepad, data, size);
    }
    const auto* begin = static_cast<const uint8_t*>(data);
    std::vector<uint8_t> bytes(begin, begin + std::max(size, 0));
    return input::write(gamepad, [=, bytes = std::move(bytes)] {
      SDL_SendGamepadEffect(gamepad, bytes.data(), static_cast<int>(bytes.size()));
    });
  }
}

void FlushWrites() noexcept {
  if constexpr (input::kInputThread) {
    std::lock_guard gate(input::g_gate);
    input::apply_queued_writes();
  }
}
} // namespace aurora::sdl
