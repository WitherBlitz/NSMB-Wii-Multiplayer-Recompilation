// Couch play only: skips EGG::LightTextureManager::drawAndCaptureTexture when nothing it reads has
// changed since the last call. Every frame the game redraws all its light textures (the
// normal-sphere lighting maps courses and the world map light their models with) into a corner of
// the EFB and copies each one out: an ambient quad, then a quad per light with its own TEV setup,
// then the copy, around a save and restore of the EFB it draws over. That is 0.7-1 ms a frame on a
// Pixel 3 XL and twice that on slower devices, for the same pictures frame after frame while the
// lights hold still.
//
// The key covers what the function reads: the manager, each of its light textures, the light set
// they point at (its light objects and ambient colours), and the EGG statics it uses (screen, EFB
// and GX state caches, the light texture layout). Objects are hashed whole as their heap blocks
// (the expanded heap's 16-byte header before each block gives its size), so no field layout is
// assumed. A call whose key matches the last one keeps the copies the last call made (aurora holds
// an EFB copy texture until its destination is written or copied to again) and only sets the
// manager's drawn-this-frame bit, which is all the call leaves behind in what the key covers.
//
// NSMBW_LIGHTTEX=verify always runs the game's code and counts repeated keys whose call wrote
// anything a skip would not have, or produced different GX commands (aurora's command digest; the
// call names the scratch buffer it borrowed, so only calls with the same buffer are compared, and
// the first commands carry over GX state from whatever drew before, which the call then sets for
// itself). =off always runs the game's code.
#include "abi_bridge.h"
#include "det_clock.h"
#include "hle_stubs.h"
#include "memory.h"
#include "runtime_log.h"

#include <dolphin/gx/GXAurora.h>
#include <xxhash.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

extern "C" void func_802CD4B0(CpuContext* ctx);  // the translated drawAndCaptureTexture

namespace {

// EGG::Frustum::sGlobalOffset .. EGG::StateGX::s_optimize. Not EGG::TextureBuffer's list after it:
// the function itself borrows a scratch buffer to save and restore the EFB it draws over, which
// turns the list round every call, and which scratch buffer that is makes no difference.
constexpr uint32_t kStaticsBegin = 0x8042AEC0u;
constexpr uint32_t kStaticsEnd = 0x8042AF58u;
constexpr uint32_t kTextureBufferHead = 0x8042AF58u;  // EGG::TextureBuffer::spHead (verify only)
constexpr uint32_t kMaxBlock = 0x10000u;
constexpr uint32_t kUnknownBlock = 0x100u;  // what is hashed of an object with no heap header

enum class Mode { Cache, Verify, Off };

Mode GetMode() {
    static const Mode mode = [] {
        if (DetClock::Enabled() || std::getenv("NSMBW_NO_COUCH_FASTPATHS") != nullptr) {
            return Mode::Off;  // lockstep play runs the game's own code, like the native reverb
        }
        const char* value = std::getenv("NSMBW_LIGHTTEX");
        if (value != nullptr && std::strcmp(value, "verify") == 0) return Mode::Verify;
        if (value != nullptr && std::strcmp(value, "off") == 0) return Mode::Off;
        return Mode::Cache;
    }();
    return mode;
}

bool Read32(uint32_t address, uint32_t& value) { return Memory::TryRead32(address, value); }

bool Read16(uint32_t address, uint16_t& value) {
    uint32_t word = 0;
    if (!Memory::TryRead32(address & ~3u, word)) return false;
    value = static_cast<uint16_t>((address & 2) ? word : word >> 16);
    return true;
}

// Folds the bytes of the heap block at `object` (or kUnknownBlock bytes of it) into `hash`.
bool HashObject(uint32_t object, uint64_t& hash) {
    if (object < 0x80000010u) return false;
    uint32_t size = kUnknownBlock;
    uint16_t signature = 0;
    uint32_t blockSize = 0;
    if (Read16(object - 0x10, signature) && signature == 0x5544 /* 'UD' */ && Read32(object - 0xC, blockSize) &&
        blockSize != 0 && blockSize <= kMaxBlock) {
        size = blockSize;
    }
    if (!Memory::Contains(object, size)) return false;
    hash = XXH3_64bits_withSeed(Memory::GetPointer(object, size), size, hash ^ object);
    return true;
}

// A call's arguments: the manager (r3) and the four floats (f1-f4), kept apart from the context,
// which the call itself clobbers.
struct Call {
    uint32_t manager = 0;
    uint64_t args[4] = {};
};

Call Arguments(const CpuContext* ctx) {
    Call call;
    call.manager = ctx->gpr[3];
    for (int i = 0; i < 4; ++i) call.args[i] = ctx->fpr[1 + i].raw;
    return call;
}

// The key of one call: false when something is not plain guest memory (then never skipped).
bool ComputeKey(const Call& call, uint64_t& key) {
    const uint32_t manager = call.manager;
    uint64_t hash = XXH3_64bits(call.args, sizeof(call.args));
    if (!Memory::Contains(kStaticsBegin, kStaticsEnd - kStaticsBegin)) return false;
    hash = XXH3_64bits_withSeed(Memory::GetPointer(kStaticsBegin, kStaticsEnd - kStaticsBegin),
                                kStaticsEnd - kStaticsBegin, hash);
    uint32_t screen = 0, textures = 0;
    uint16_t count = 0;
    if (!HashObject(manager, hash) || !Read32(manager + 0xC, screen) || !HashObject(screen, hash) ||
        !Read16(manager + 6, count) || !Read32(manager + 8, textures) || count > 64 || !HashObject(textures, hash)) {
        return false;
    }
    uint32_t lastSet = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t texture = 0, set = 0;
        if (!Read32(textures + 4 * i, texture) || !HashObject(texture, hash) || !Read32(texture + 0x30, set)) {
            return false;
        }
        if (set == lastSet) continue;  // the textures usually share one light set
        lastSet = set;
        uint32_t lights = 0, ambientOwner = 0, ambient = 0;
        uint16_t lightCount = 0;
        if (!HashObject(set, hash) || !Read16(set + 0x14, lightCount) || !Read32(set + 0x18, lights) ||
            lightCount > 256 || !HashObject(lights, hash) || !Read32(set + 0xC, ambientOwner) ||
            !HashObject(ambientOwner, hash) || !Read32(ambientOwner + 0xC, ambient) || !HashObject(ambient, hash)) {
            return false;
        }
        for (uint32_t l = 0; l < lightCount; ++l) {
            uint32_t light = 0;
            if (!Read32(lights + 4 * l, light)) return false;
            if (light != 0 && !HashObject(light, hash)) return false;
        }
    }
    key = hash;
    return true;
}

// The function's own early-outs (light textures off, already drawn this frame, none to draw), in
// which case it does nothing worth skipping. Otherwise sets the manager's drawn-this-frame bit, as
// the function does first thing.
bool MarkDrawn(uint32_t manager) {
    uint32_t flags = 0, screen = 0, drawn = 0, screenWord = 0;
    uint16_t count = 0;
    if (!Read32(manager + 4, flags) || ((flags >> 24) & 0x10) == 0 || !Read32(manager + 0xC, screen) ||
        !Read32((screen + 0x1D) & ~3u, screenWord)) {
        return false;
    }
    const uint32_t screenId = (screenWord >> (8 * (3 - ((screen + 0x1D) & 3)))) & 0xFF;
    const uint32_t bit = screenId < 32 ? 1u << screenId : 0;
    if (!Read32(manager + 0x10, drawn) || (drawn & bit) != 0 || !Read16(manager + 6, count) || count == 0) {
        return false;
    }
    Memory::Write32(manager + 0x10, drawn | bit);
    return true;
}

struct State {
    bool valid = false;
    uint64_t key = 0;
    uint32_t calls = 0, repeats = 0, skips = 0, outputMismatches = 0, writeMismatches = 0;
};

// Per manager: m3d keeps one per scene layer.
State& StateFor(uint32_t manager) {
    static uint32_t managers[4] = {};
    static State states[4];
    for (int i = 0; i < 4; ++i) {
        if (managers[i] == manager || managers[i] == 0) {
            managers[i] = manager;
            return states[i];
        }
    }
    return states[3];
}

void Report(State& s) {
    if (++s.calls % 1200 == 0) {
        RT_LOGF(RT_TAG_RUNTIME,
                "light textures: %u calls, %u with the last call's key, %u skipped; verify: %u repeats with other "
                "GX output, %u with other writes\n",
                s.calls, s.repeats, s.skips, s.outputMismatches, s.writeMismatches);
    }
}

// NSMBW_LIGHTTEX=verify: runs the call and checks a repeated key against what skipping would do.
void RunVerified(CpuContext* ctx, const Call& call, State& s, bool keyed, uint64_t key, bool repeat) {
    // What a skip would leave in memory: the call's state with the drawn bit set.
    uint64_t skipped = 0;
    bool skippedKeyed = false;
    uint32_t drawnBefore = 0;
    if (keyed && Read32(call.manager + 0x10, drawnBefore) && MarkDrawn(call.manager)) {
        skippedKeyed = ComputeKey(call, skipped);
        Memory::Write32(call.manager + 0x10, drawnBefore);
    }
    uint32_t buffer = 0;
    Read32(kTextureBufferHead, buffer);
    AuroraBeginCommandDigest();
    func_802CD4B0(ctx);
    const uint64_t output = AuroraEndCommandDigest();
    if (!repeat || !skippedKeyed) return;
    uint64_t after = 0;
    if (!ComputeKey(call, after) || after != skipped) ++s.writeMismatches;
    static std::vector<std::pair<std::pair<uint64_t, uint32_t>, uint64_t>> seen;  // (key, buffer) -> output
    for (const auto& [id, earlier] : seen) {
        if (id.first == key && id.second == buffer) {
            if (earlier != output) ++s.outputMismatches;
            return;
        }
    }
    if (seen.size() >= 64) seen.erase(seen.begin());
    seen.push_back({{key, buffer}, output});
}

} // namespace

extern "C" void LightTextureDrawAndCapture_802cd4b0(CpuContext* ctx) {
    const Mode mode = GetMode();
    if (mode == Mode::Off) {
        func_802CD4B0(ctx);
        return;
    }
    const Call call = Arguments(ctx);
    State& s = StateFor(call.manager);
    uint64_t key = 0;
    const bool keyed = ComputeKey(call, key);
    const bool repeat = keyed && s.valid && key == s.key;
    s.repeats += repeat ? 1 : 0;
    if (mode == Mode::Verify) {
        RunVerified(ctx, call, s, keyed, key, repeat);
    } else if (repeat && MarkDrawn(call.manager)) {
        ++s.skips;
    } else {
        func_802CD4B0(ctx);
    }
    s.valid = keyed;
    s.key = key;
    Report(s);
}

REGISTER_NATIVE_FUNCTION_AS(0x802CD4B0, LightTextureDrawAndCapture_802cd4b0, "LightTextureDrawAndCapture_802cd4b0");
