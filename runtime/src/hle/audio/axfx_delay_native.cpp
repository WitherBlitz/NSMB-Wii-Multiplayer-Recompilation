// Native AXFXDelayExpCallback. The translated effect is the fixed-point AXFXDelayExp
// network: a one-pole input filter, a three-channel delay line, feedback, a wet gain and
// an optional send bus. The state stays in guest memory. The native path is only installed
// outside lockstep play and falls back to the translated original whenever a range is not
// plain RAM or the state is invalid.
#include "axfx_delay_native.h"

#include "abi_bridge.h"
#include "det_clock.h"
#include "memory.h"
#include "runtime_log.h"

#include "ax_dsp.h"
#include "ax_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" void func_8026B560(CpuContext* ctx);  // FxDelay::UpdateBuffer (non-Dpl2)
extern "C" void func_8026BB60(CpuContext* ctx);  // FxDelay::UpdateBuffer

namespace AxfxDelayNative {
namespace {

constexpr uint32_t kSamples = 96;
constexpr uint32_t kStateBytes = 0x50;
constexpr uint32_t kMaxDelaySamples = 0x400000u;

enum class Mode { Off, Inline, Worker };

Mode g_mode = Mode::Off;

uint8_t* Resolve(uint32_t address, size_t bytes) {
    if (address == 0 || bytes == 0) {
        return nullptr;
    }
    if (AxDspHle::t_onMixWorker) {
        return AxDspHle::MixResolveRange(address, bytes);
    }
    if (!Memory::Contains(address, bytes)) {
        return nullptr;
    }
    return Memory::GetPointer(address, bytes);
}

uint32_t Load32(const uint8_t* p) {
    uint32_t value;
    std::memcpy(&value, p, 4);
    return __builtin_bswap32(value);
}

void Store32(uint8_t* p, uint32_t value) {
    value = __builtin_bswap32(value);
    std::memcpy(p, &value, 4);
}

int32_t LoadS32(const uint8_t* p) {
    return static_cast<int32_t>(Load32(p));
}

void StoreS32(uint8_t* p, int32_t value) {
    Store32(p, static_cast<uint32_t>(value));
}

int32_t Mul(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
}

int32_t Add(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}

// PPC srawi: arithmetic shift right, rounding toward negative infinity.
int32_t Sar7(int32_t value) {
    return static_cast<int32_t>((static_cast<uint32_t>(value) >> 7) |
                                (value < 0 ? 0xFE000000u : 0u));
}

struct Layout {
    int32_t flags = 0;
    int32_t length = 0;
    int32_t index = 0;
    int32_t filter = 0;
    int32_t feedback = 0;
    int32_t outputGain = 0;
    int32_t extraGain = 0;
    int32_t filterState[3]{};
    uint8_t* state = nullptr;
    uint8_t* delay[3]{};
    uint8_t* input[3]{};
    uint8_t* output[3]{};
    uint8_t* extra[3]{};
    bool hasExtra = false;
};

bool BuildLayout(uint32_t stateAddress, uint32_t buffers, Layout& layout) {
    layout.state = Resolve(stateAddress, kStateBytes);
    if (layout.state == nullptr) {
        return false;
    }
    layout.flags = LoadS32(layout.state + 0x34);
    if (layout.flags != 0) {
        return true;
    }
    layout.length = LoadS32(layout.state + 0x10);
    layout.index = LoadS32(layout.state + 0x0C);
    if (layout.length <= 0 || layout.length > static_cast<int32_t>(kMaxDelaySamples) ||
        layout.index < 0 || layout.index >= layout.length) {
        return false;
    }

    const size_t delayBytes = static_cast<size_t>(layout.length) * 4;
    for (int channel = 0; channel < 3; ++channel) {
        layout.delay[channel] = Resolve(Load32(layout.state + channel * 4), delayBytes);
        if (layout.delay[channel] == nullptr) {
            return false;
        }
    }

    uint8_t* bufferPointers = Resolve(buffers, 3 * 4);
    if (bufferPointers == nullptr) {
        return false;
    }
    for (int channel = 0; channel < 3; ++channel) {
        layout.output[channel] = Resolve(Load32(bufferPointers + channel * 4), kSamples * 4);
        if (layout.output[channel] == nullptr) {
            return false;
        }
    }

    const uint32_t inputPointers = Load32(layout.state + 0x48);
    if (inputPointers != 0) {
        uint8_t* pointers = Resolve(inputPointers, 3 * 4);
        if (pointers == nullptr) {
            return false;
        }
        for (int channel = 0; channel < 3; ++channel) {
            layout.input[channel] = Resolve(Load32(pointers + channel * 4), kSamples * 4);
            if (layout.input[channel] == nullptr) {
                return false;
            }
        }
    } else {
        for (int channel = 0; channel < 3; ++channel) {
            layout.input[channel] = layout.output[channel];
        }
    }

    const uint32_t extraPointers = Load32(layout.state + 0x4C);
    if (extraPointers != 0) {
        uint8_t* pointers = Resolve(extraPointers, 3 * 4);
        if (pointers == nullptr) {
            return false;
        }
        layout.hasExtra = true;
        for (int channel = 0; channel < 3; ++channel) {
            layout.extra[channel] = Resolve(Load32(pointers + channel * 4), kSamples * 4);
            if (layout.extra[channel] == nullptr) {
                return false;
            }
        }
    }

    layout.filter = LoadS32(layout.state + 0x28);
    layout.feedback = LoadS32(layout.state + 0x18);
    layout.outputGain = LoadS32(layout.state + 0x2C);
    layout.extraGain = LoadS32(layout.state + 0x30);
    for (int channel = 0; channel < 3; ++channel) {
        layout.filterState[channel] = LoadS32(layout.state + 0x1C + channel * 4);
    }
    return true;
}

void Process(Layout& layout) {
    const int32_t wet = 128 - layout.filter;
    int32_t filterState[3] = {layout.filterState[0], layout.filterState[1], layout.filterState[2]};
    int32_t index = layout.index;

    for (uint32_t sample = 0; sample < kSamples; ++sample) {
        int32_t delayed[3];
        int32_t filtered[3];
        for (int channel = 0; channel < 3; ++channel) {
            delayed[channel] = LoadS32(layout.delay[channel] + static_cast<size_t>(index) * 4);
            const int32_t input = LoadS32(layout.input[channel] + sample * 4);
            filtered[channel] = Sar7(Add(Mul(input, wet), Mul(filterState[channel], layout.filter)));
            filterState[channel] = filtered[channel];
        }
        for (int channel = 0; channel < 3; ++channel) {
            const int32_t written = Add(filtered[channel], Sar7(Mul(delayed[channel], layout.feedback)));
            StoreS32(layout.delay[channel] + static_cast<size_t>(index) * 4, written);
            StoreS32(layout.output[channel] + sample * 4, Sar7(Mul(delayed[channel], layout.outputGain)));
            if (layout.hasExtra) {
                StoreS32(layout.extra[channel] + sample * 4, Sar7(Mul(delayed[channel], layout.extraGain)));
            }
        }
        ++index;
        if (static_cast<uint32_t>(index) >= static_cast<uint32_t>(layout.length)) {
            index = 0;
        }
    }

    for (int channel = 0; channel < 3; ++channel) {
        StoreS32(layout.state + 0x1C + channel * 4, filterState[channel]);
    }
    StoreS32(layout.state + 0x0C, index);
}

Mode ReadMode() {
    const char* value = std::getenv("NSMBW_AX_DELAY");
    if (value == nullptr || value[0] == '\0') {
        // Default couch-play path: the delay state is processed on the AX worker so the
        // guest DSP callback does not carry it.
        return Mode::Worker;
    }
    if (std::strcmp(value, "off") == 0) {
        return Mode::Off;
    }
    if (std::strcmp(value, "inline") == 0) {
        return Mode::Inline;
    }
    if (std::strcmp(value, "worker") == 0) {
        return Mode::Worker;
    }
    RT_LOGF(RT_TAG_RUNTIME, "audio: unknown NSMBW_AX_DELAY=%s; native delay disabled\n", value);
    return Mode::Off;
}

}  // namespace

bool Run(uint32_t stateAddress, uint32_t buffers) {
    Layout layout;
    if (!BuildLayout(stateAddress, buffers, layout)) {
        return false;
    }
    if (layout.flags != 0) {
        StoreS32(layout.state + 0x34, layout.flags & ~2);
        return true;
    }
    Process(layout);
    return true;
}

void UpdateBufferAt(CpuContext* ctx, uint32_t stateOffset, void (*fallback)(CpuContext*)) {
    const uint32_t self = ctx->gpr[3];
    const uint32_t buffers = ctx->gpr[5];
    const uint32_t outputMode = ctx->gpr[8];
    if (g_mode == Mode::Off || outputMode == 2) {
        fallback(ctx);
        return;
    }
    uint8_t* object = Resolve(self, 0x0D);
    if (object == nullptr) {
        fallback(ctx);
        return;
    }
    if (object[0x0C] == 0) {
        return;
    }
    const uint32_t state = self + stateOffset;
    if (g_mode == Mode::Worker && !AxDspHle::t_onMixWorker) {
        if (AxDspHle::RunDelayJob(state, buffers)) {
            return;
        }
    }
    if (!Run(state, buffers)) {
        fallback(ctx);
    }
}

void UpdateBuffer44(CpuContext* ctx) {
    UpdateBufferAt(ctx, 0x2C, &func_8026B560);
}

void UpdateBuffer48(CpuContext* ctx) {
    UpdateBufferAt(ctx, 0x30, &func_8026BB60);
}

const RawDispatchRecord kRecord44{0x8026B560u, &UpdateBuffer44, 0, false};
const RawDispatchRecord kRecord48{0x8026BB60u, &UpdateBuffer48, 0, false};

void Install() {
    if (DetClock::Enabled() || std::getenv("NSMBW_NO_COUCH_FASTPATHS") != nullptr) {
        g_mode = Mode::Off;
        return;
    }
    g_mode = ReadMode();
    if (g_mode == Mode::Off) {
        return;
    }
    TranslatedFunctionRegistry::SetRuntimeRedirect(&kRecord44);
    TranslatedFunctionRegistry::SetRuntimeRedirect(&kRecord48);
    static const char* const names[] = {"off", "inline", "worker", "compare"};
    RT_LOGF(RT_TAG_RUNTIME, "audio: native FxDelay (%s, couch play)\n",
            names[static_cast<int>(g_mode)]);
}

}  // namespace AxfxDelayNative
