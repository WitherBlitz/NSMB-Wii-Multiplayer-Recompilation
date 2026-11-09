// Couch play only: a native FxReverbHi::UpdateBuffer, the AX aux effect that was the hottest
// translated function (a fifth of the game thread on a Pixel 3 XL). The translated version emulates
// every Gekko single-precision operation exactly (25-bit fmuls operand rounding, FPSCR[NI] flushing,
// big-endian loads and stores through the checked flat-memory helpers); this one runs the same
// reverb (AXFXReverbHiExpCallback's network: early reflections, pre-delay, three combs, two shared
// all-passes, damping and a per-channel all-pass, then the crosstalk mix) in plain host float math
// on host pointers. Its rounding differs in the last bits, which nobody can hear but which would
// split two machines in lockstep, so it is installed only outside LAN play (Install, at startup);
// lockstep play keeps the translated original. The effect's state stays in guest memory in the
// game's own layout, so the two never disagree about it.
//
// NSMBW_REVERB_COMPARE=1 runs both on every block (the translated result is kept) and logs how far
// apart they are.
#include "axfx_reverb_native.h"

#include "abi_bridge.h"
#include "ax_dsp.h"
#include "ax_internal.h"
#include "det_clock.h"
#include "memory.h"
#include "runtime_log.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" void func_8026C210(CpuContext* ctx);  // the translated FxReverbHi::UpdateBuffer

namespace AxfxReverbNative {
namespace {

constexpr uint32_t kUpdateBuffer = 0x8026C210u;  // UpdateBuffer__Q34nw4r3snd10FxReverbHi...
constexpr uint32_t kSamplesPerBlock = 96;         // one 3 ms AX frame at 32 kHz
constexpr uint32_t kStateBytes = 0x148;           // the AXFX_REVERBHI_EXP fields used here
// The callback's float constants in .sdata2 (read from the game, not assumed).
constexpr uint32_t kZeroAddr = 0x8042DF3Cu, kOneAddr = 0x8042DF40u, kCrossAddr = 0x8042DF44u,
                   kMixAddr = 0x8042DF48u;

uint32_t Load32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}
void Store32(uint8_t* p, uint32_t v) {
    v = __builtin_bswap32(v);
    std::memcpy(p, &v, 4);
}
float LoadF(const uint8_t* p) {
    const uint32_t bits = Load32(p);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
void StoreF(uint8_t* p, float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    Store32(p, bits);
}
// fctiwz: toward zero, saturating, NaN to INT32_MIN.
int32_t ToInt(float v) {
    if (!(v > -2147483648.0f)) return INT32_MIN;  // also NaN
    if (v >= 2147483648.0f) return INT32_MAX;
    return static_cast<int32_t>(v);
}

// A guest array of `count` 32-bit entries as a host pointer, or nullptr.
uint8_t* Host(uint32_t guest, uint32_t count) {
    if (guest == 0 || count == 0 || !Memory::Contains(guest, static_cast<size_t>(count) * 4)) return nullptr;
    return Memory::GetPointer(guest, static_cast<size_t>(count) * 4);
}

struct Constants {
    float zero = 0, one = 1, cross = 0, mix = 0;
    bool loaded = false;
};
Constants g_k;

bool LoadConstants() {
    if (g_k.loaded) return true;
    uint32_t bits[4];
    if (!Memory::TryRead32(kZeroAddr, bits[0]) || !Memory::TryRead32(kOneAddr, bits[1]) ||
        !Memory::TryRead32(kCrossAddr, bits[2]) || !Memory::TryRead32(kMixAddr, bits[3])) {
        return false;
    }
    std::memcpy(&g_k.zero, &bits[0], 4);
    std::memcpy(&g_k.one, &bits[1], 4);
    std::memcpy(&g_k.cross, &bits[2], 4);
    std::memcpy(&g_k.mix, &bits[3], 4);
    g_k.loaded = true;
    return true;
}

// Everything one block reads and writes, as host pointers into guest RAM.
struct Buffers {
    uint8_t* state = nullptr;
    uint8_t* in[3] = {};
    uint8_t* busIn[3] = {};
    uint8_t* busOut[3] = {};
    uint32_t earlyLen = 0, preLen = 0, combLen[3] = {}, apLen[2] = {}, lastLen[3] = {};
    uint8_t *early[3] = {}, *pre[3] = {}, *comb[3][3] = {}, *ap[3][2] = {}, *last[3] = {};

    // For NSMBW_REVERB_COMPARE: every range a block may write, with its kind (0 int, 1 float).
    template <typename F>
    void ForEachRange(F&& f) const {
        f(state, kStateBytes, 2);
        for (int ch = 0; ch < 3; ++ch) {
            f(in[ch], kSamplesPerBlock * 4, 0);
            if (busOut[ch] != nullptr) f(busOut[ch], kSamplesPerBlock * 4, 0);
            f(early[ch], earlyLen * 4, 1);
            if (pre[ch] != nullptr) f(pre[ch], preLen * 4, 1);
            for (int k = 0; k < 3; ++k) f(comb[ch][k], combLen[k] * 4, 1);
            for (int k = 0; k < 2; ++k) f(ap[ch][k], apLen[k] * 4, 1);
            f(last[ch], lastLen[ch] * 4, 1);
        }
    }
};

// False when anything a block would touch is not plain guest RAM, or an index is out of range: the
// caller then runs the translated original instead.
bool Gather(uint32_t r, const uint32_t inGuest[3], Buffers& b) {
    uint8_t* R = Memory::Contains(r, kStateBytes) ? Memory::GetPointer(r, kStateBytes) : nullptr;
    if (R == nullptr) return false;
    b.state = R;
    for (int ch = 0; ch < 3; ++ch) {
        if ((b.in[ch] = Host(inGuest[ch], kSamplesPerBlock)) == nullptr) return false;
    }
    const uint32_t busInGuest = Load32(R + 0x138), busOutGuest = Load32(R + 0x13C);
    for (int ch = 0; ch < 3; ++ch) {
        uint32_t p = 0;
        if (busInGuest != 0 && (!Memory::TryRead32(busInGuest + 4 * ch, p) || (b.busIn[ch] = Host(p, kSamplesPerBlock)) == nullptr)) return false;
        if (busOutGuest != 0 && (!Memory::TryRead32(busOutGuest + 4 * ch, p) || (b.busOut[ch] = Host(p, kSamplesPerBlock)) == nullptr)) return false;
    }
    b.earlyLen = Load32(R + 0x18);
    b.preLen = Load32(R + 0x3C);
    for (int k = 0; k < 3; ++k) b.combLen[k] = Load32(R + 0x74 + 4 * k);
    for (int k = 0; k < 2; ++k) b.apLen[k] = Load32(R + 0xB8 + 4 * k);
    for (int ch = 0; ch < 3; ++ch) {
        b.lastLen[ch] = Load32(R + 0xE0 + 4 * ch);
        if ((b.early[ch] = Host(Load32(R + 0x00 + 4 * ch), b.earlyLen)) == nullptr) return false;
        if (b.preLen != 0 && (b.pre[ch] = Host(Load32(R + 0x2C + 4 * ch), b.preLen)) == nullptr) return false;
        for (int k = 0; k < 3; ++k) {
            if ((b.comb[ch][k] = Host(Load32(R + 0x44 + 12 * ch + 4 * k), b.combLen[k])) == nullptr) return false;
        }
        for (int k = 0; k < 2; ++k) {
            if ((b.ap[ch][k] = Host(Load32(R + 0x98 + 8 * ch + 4 * k), b.apLen[k])) == nullptr) return false;
        }
        if ((b.last[ch] = Host(Load32(R + 0xC8 + 4 * ch), b.lastLen[ch])) == nullptr) return false;
        if (Load32(R + 0xD4 + 4 * ch) >= b.lastLen[ch]) return false;
    }
    for (int k = 0; k < 3; ++k) {
        if (Load32(R + 0x0C + 4 * k) >= b.earlyLen || Load32(R + 0x68 + 4 * k) >= b.combLen[k]) return false;
    }
    if (Load32(R + 0xB0) >= b.apLen[0] || Load32(R + 0xB4) >= b.apLen[1]) return false;
    if (b.preLen != 0 && Load32(R + 0x38) >= b.preLen) return false;
    return true;
}

void Run(const Buffers& b) {
    uint8_t* R = b.state;
    uint32_t earlyIdx[3] = {Load32(R + 0x0C), Load32(R + 0x10), Load32(R + 0x14)};
    uint32_t preIdx = Load32(R + 0x38);
    uint32_t combIdx[3] = {Load32(R + 0x68), Load32(R + 0x6C), Load32(R + 0x70)};
    uint32_t apIdx[2] = {Load32(R + 0xB0), Load32(R + 0xB4)};
    uint32_t lastIdx[3];
    float lowpass[3];
    for (int ch = 0; ch < 3; ++ch) {
        lastIdx[ch] = Load32(R + 0xD4 + 4 * ch);
        lowpass[ch] = LoadF(R + 0xFC + 4 * ch);
    }
    const float tap[3] = {LoadF(R + 0x20), LoadF(R + 0x24), LoadF(R + 0x28)};
    const float combCoef[3] = {LoadF(R + 0x8C), LoadF(R + 0x90), LoadF(R + 0x94)};
    const float allpass = LoadF(R + 0xF8);
    const float damping = LoadF(R + 0x108), undamped = g_k.one - damping;
    const float cross = g_k.cross * LoadF(R + 0x134);
    const float mix = g_k.mix * LoadF(R + 0x12C);
    const float gain = LoadF(R + 0x140), busGain = LoadF(R + 0x144);

    for (uint32_t i = 0; i < kSamplesPerBlock; ++i) {
        float out[3];
        for (int ch = 0; ch < 3; ++ch) {
            uint32_t sample = Load32(b.in[ch] + 4 * i);
            if (b.busIn[ch] != nullptr) sample += Load32(b.busIn[ch] + 4 * i);
            const float x = static_cast<float>(static_cast<int32_t>(sample));
            uint8_t* e = b.early[ch];
            const float e0 = LoadF(e + 4 * earlyIdx[0]), e1 = LoadF(e + 4 * earlyIdx[1]), e2 = LoadF(e + 4 * earlyIdx[2]);
            StoreF(e + 4 * earlyIdx[2], x);
            const float reflections = tap[2] * e2 + (tap[0] * e0 + tap[1] * e1);
            float delayed = x;
            if (b.pre[ch] != nullptr) {
                delayed = LoadF(b.pre[ch] + 4 * preIdx);
                StoreF(b.pre[ch] + 4 * preIdx, x);
            }
            float acc = g_k.zero;
            for (int k = 0; k < 3; ++k) {
                uint8_t* slot = b.comb[ch][k] + 4 * combIdx[k];
                const float v = LoadF(slot);
                acc += v;
                StoreF(slot, delayed + v * combCoef[k]);
            }
            float a = acc;
            for (int k = 0; k < 2; ++k) {
                uint8_t* slot = b.ap[ch][k] + 4 * apIdx[k];
                const float v = LoadF(slot);
                const float t = a + v * allpass;
                StoreF(slot, t);
                a = v - t * allpass;
            }
            lowpass[ch] = undamped * a + damping * lowpass[ch];
            uint8_t* slot = b.last[ch] + 4 * lastIdx[ch];
            const float v = LoadF(slot);
            const float t = lowpass[ch] + v * allpass;
            StoreF(slot, t);
            if (++lastIdx[ch] >= b.lastLen[ch]) lastIdx[ch] = 0;
            out[ch] = (v - t * allpass) * cross + reflections;
        }
        const float mixed[3] = {out[0] + (out[1] + out[2]) * mix, out[1] + (out[0] + out[2]) * mix,
                                out[2] + (out[0] + out[1]) * mix};
        for (int ch = 0; ch < 3; ++ch) {
            Store32(b.in[ch] + 4 * i, static_cast<uint32_t>(ToInt(mixed[ch] * gain)));
            if (b.busOut[ch] != nullptr) Store32(b.busOut[ch] + 4 * i, static_cast<uint32_t>(ToInt(mixed[ch] * busGain)));
        }
        for (int k = 0; k < 3; ++k) {
            if (++earlyIdx[k] >= b.earlyLen) earlyIdx[k] = 0;
            if (++combIdx[k] >= b.combLen[k]) combIdx[k] = 0;
        }
        if (b.preLen != 0 && ++preIdx >= b.preLen) preIdx = 0;
        for (int k = 0; k < 2; ++k) {
            if (++apIdx[k] >= b.apLen[k]) apIdx[k] = 0;
        }
    }

    for (int k = 0; k < 3; ++k) {
        Store32(R + 0x0C + 4 * k, earlyIdx[k]);
        Store32(R + 0x68 + 4 * k, combIdx[k]);
    }
    Store32(R + 0x38, preIdx);
    Store32(R + 0xB0, apIdx[0]);
    Store32(R + 0xB4, apIdx[1]);
    for (int ch = 0; ch < 3; ++ch) {
        Store32(R + 0xD4 + 4 * ch, lastIdx[ch]);
        StoreF(R + 0xFC + 4 * ch, lowpass[ch]);
    }
}

// NSMBW_REVERB_COMPARE: run the native block on a snapshot, put the snapshot back, run the
// translated one, and log the largest differences (outputs in sample units, state relative).
void Compare(const Buffers& b, CpuContext* ctx) {
    std::vector<std::vector<uint8_t>> before, native;
    b.ForEachRange([&](uint8_t* p, uint32_t n, int) { before.emplace_back(p, p + n); });
    Run(b);
    b.ForEachRange([&](uint8_t* p, uint32_t n, int) { native.emplace_back(p, p + n); });
    size_t i = 0;
    b.ForEachRange([&](uint8_t* p, uint32_t n, int) { std::memcpy(p, before[i++].data(), n); });
    func_8026C210(ctx);
    static double s_maxOut = 0, s_maxState = 0;
    static uint32_t s_blocks = 0, s_indexMismatches = 0;
    i = 0;
    b.ForEachRange([&](uint8_t* p, uint32_t n, int kind) {
        const std::vector<uint8_t>& mine = native[i++];
        for (uint32_t o = 0; o + 4 <= n; o += 4) {
            if (kind == 0) {
                const double d = std::fabs(double(int32_t(Load32(p + o))) - double(int32_t(Load32(mine.data() + o))));
                s_maxOut = std::max(s_maxOut, d);
            } else if (kind == 1 || (kind == 2 && (o == 0xFC || o == 0x100 || o == 0x104))) {
                const float a = LoadF(p + o), c = LoadF(mine.data() + o);
                const double scale = std::max(1.0, double(std::fabs(a)));
                s_maxState = std::max(s_maxState, std::fabs(double(a) - double(c)) / scale);
            } else if (Load32(p + o) != Load32(mine.data() + o)) {
                ++s_indexMismatches;  // the indices and lengths must agree exactly
            }
        }
    });
    if (++s_blocks % 2000 == 0) {
        RT_LOGF(RT_TAG_RUNTIME,
                "reverb compare: %u blocks, largest output difference %.0f (of +-8388608), state %.2e, "
                "index mismatches %u\n",
                s_blocks, s_maxOut, s_maxState, s_indexMismatches);
        s_maxOut = s_maxState = 0;
    }
}

// FxReverbHi::UpdateBuffer(this, channels, buffers, size, format, sampleRate, outputMode): runs the
// reverb on the three aux buffers unless the effect is off or the output is Dolby Pro Logic II (2).
void UpdateBuffer(CpuContext* ctx) {
    static const bool compare = std::getenv("NSMBW_REVERB_COMPARE") != nullptr;
    const uint32_t self = ctx->gpr[3], buffers = ctx->gpr[5], outputMode = ctx->gpr[8];
    uint32_t active = 0, in[3] = {};
    if (!Memory::TryRead32(self + 0xC, active) || !Memory::TryRead32(buffers, in[0]) ||
        !Memory::TryRead32(buffers + 4, in[1]) || !Memory::TryRead32(buffers + 8, in[2]) || !LoadConstants()) {
        func_8026C210(ctx);
        return;
    }
    if ((active >> 24) == 0 || outputMode == 2) return;
    uint32_t flags = 0;
    if (!Memory::TryRead32(self + 0x44 + 0x10C, flags)) {
        func_8026C210(ctx);
        return;
    }
    if (flags != 0) {
        Memory::Write32(self + 0x44 + 0x10C, flags & ~2u);  // the effect was just switched: skip a block
        return;
    }
    Buffers b;
    if (!Gather(self + 0x44, in, b)) {
        func_8026C210(ctx);  // not plain RAM somewhere: the translated original handles it
        return;
    }
    if (compare) {
        Compare(b, ctx);
        return;
    }
    static const bool worker = [] {
        const char* value = std::getenv("NSMBW_REVERB_WORKER");
        return value != nullptr &&
               (std::strcmp(value, "on") == 0 || std::strcmp(value, "1") == 0);
    }();
    if (worker && AxDspHle::RunReverbJob(self, buffers)) {
        return;
    }
    Run(b);
}

const RawDispatchRecord kRecord{kUpdateBuffer, &UpdateBuffer, 0, false};

} // namespace

bool RunWorker(uint32_t self, uint32_t buffers) {
    uint32_t in[3] = {};
    if (!Memory::TryRead32(buffers, in[0]) || !Memory::TryRead32(buffers + 4u, in[1]) ||
        !Memory::TryRead32(buffers + 8u, in[2])) {
        return false;
    }
    Buffers b;
    if (!Gather(self + 0x44u, in, b)) {
        return false;
    }
    Run(b);
    return true;
}

void Install() {
    if (DetClock::Enabled() || std::getenv("NSMBW_NO_COUCH_FASTPATHS") != nullptr) {
        return;  // lockstep play keeps the translated, bit-exact reverb (and the switch, for A/B tests)
    }
    TranslatedFunctionRegistry::SetRuntimeRedirect(&kRecord);
    RT_LOGF(RT_TAG_RUNTIME, "audio: native FxReverbHi (couch play)\n");
}

} // namespace AxfxReverbNative
