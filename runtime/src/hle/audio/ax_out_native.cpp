// Native AX output control path. This replaces the translated AXOut AI callback
// and __AXOutNewFrame bookkeeping. The DSP command list and mix still go through
// AxDspHle; the guest user callback and the individual AX helpers stay on the CPU.
#include "ax_out_native.h"

#include "abi_bridge.h"
#include "ax_dsp.h"
#include "ax_internal.h"
#include "det_clock.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_log.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

extern "C" void func_801A00E0(CpuContext* ctx);  // __AXServiceCallbackStack
extern "C" void func_801A08F0(CpuContext* ctx);  // __AXProcessAux
extern "C" void func_801A0E60(CpuContext* ctx);  // __AXGetCommandListAddress
extern "C" void func_801A0E90(CpuContext* ctx);  // __AXNextFrame
extern "C" void func_801A2B10(CpuContext* ctx);  // __AXSyncPBs

extern "C" void AIInitDMA_80123fcc(uint32_t startAddr, uint32_t length);
extern "C" uint32_t AIGetDMABytesLeft_8012405c();

namespace AxOutNative {
namespace {

constexpr uint32_t kAxFrameSize = 384u;
constexpr uint32_t kAxSamplesPerFrame = 96u;
constexpr uint32_t kAxRmtFrameSamples = 18u;
constexpr uint32_t kAxRmtStride = 360u;
constexpr uint32_t kAxLocalProfile = 0x8037BFA0u;
constexpr uint32_t kAxRmtOutBuffer = 0x8037BFE0u;
constexpr uint32_t kAxOutSBuffer = 0x8037C580u;
constexpr uint32_t kAxOutBuffer = 0x8037C880u;
constexpr uint32_t kAxDspTask = AxDspHle::kAxDspTaskAddr;

// Small-data offsets from the NSMBW AXOut globals.
constexpr uint32_t kOutFrame = 0x50D0u;
constexpr uint32_t kDspDoneFlag = 0x50E4u;
constexpr uint32_t kOutDspReady = 0x50E8u;
constexpr uint32_t kAiDmaFrame = 0x50ECu;
constexpr uint32_t kUserFrameCallback = 0x50F4u;
constexpr uint32_t kOutputBufferMode = 0x50F8u;
constexpr uint32_t kRmtBuffLen = 0x50FCu;
constexpr uint32_t kRmtDspPtr = 0x5100u;
constexpr uint32_t kRmtCpuPtr = 0x5104u;
constexpr uint32_t kExceedCallback = 0x5108u;

std::atomic<uint32_t> g_aiCallbackCalls{0};
std::atomic<uint32_t> g_newFrameCalls{0};

uint32_t Sda(const CpuContext* ctx, uint32_t offset) {
    return Memory::Read32(ctx->gpr[13] - offset);
}

void SetSda(const CpuContext* ctx, uint32_t offset, uint32_t value) {
    Memory::Write32(ctx->gpr[13] - offset, value);
}

void NativeAXOutNewFrame(CpuContext* ctx) {
    if (g_newFrameCalls.fetch_add(1, std::memory_order_relaxed) == 0) {
        RT_LOGF(RT_TAG_AUDIO, "audio: native AXOutNewFrame active\n");
    }
    const uint32_t mode = Sda(ctx, kOutputBufferMode);

    // AXOut.c: cycles = (AX_SAMPLES_PER_FRAME - AIGetDMABytesLeft() / 4) * 3797.
    const uint32_t bytesLeft = AIGetDMABytesLeft_8012405c();
    const uint32_t cycles = (kAxSamplesPerFrame - bytesLeft / sizeof(uint32_t)) * 3797u;

    ctx->gpr[3] = mode == 1u ? 0u : 24400u;
    func_801A2B10(ctx);  // __AXSyncPBs

    // __AXPrintStudio is diagnostic output only.

    ctx->gpr[3] = 0;
    func_801A0E60(ctx);  // __AXGetCommandListAddress
    const uint32_t commandList = ctx->gpr[3];
    AxDspHle::SendMailToDSP(AxDspHle::kMailCmdList | 128u);  // AX_CL_SIZE
    AxDspHle::SendMailToDSP(commandList);

    ctx->gpr[3] = 0;
    func_801A00E0(ctx);  // __AXServiceCallbackStack
    ctx->gpr[3] = 0;
    func_801A08F0(ctx);  // __AXProcessAux

    const uint32_t userFrameCallback = Sda(ctx, kUserFrameCallback);
    if (userFrameCallback != 0) {
        InvokeIndirectCpu(userFrameCallback, ctx);
    }

    int32_t rmtDspPtr = static_cast<int32_t>(Sda(ctx, kRmtDspPtr));
    int32_t rmtCpuPtr = static_cast<int32_t>(Sda(ctx, kRmtCpuPtr));
    int32_t rmtBuffLen = static_cast<int32_t>(Sda(ctx, kRmtBuffLen));
    if (rmtBuffLen <= 0) {
        rmtBuffLen = static_cast<int32_t>(kAxRmtFrameSamples * 10u);
    }

    const uint32_t rmtBase = kAxRmtOutBuffer + static_cast<uint32_t>(rmtDspPtr) * 2u;
    const uint32_t rmt[4] = {rmtBase, rmtBase + kAxRmtStride, rmtBase + kAxRmtStride * 2u,
                             rmtBase + kAxRmtStride * 3u};

    int32_t nextDspPtr = rmtDspPtr + static_cast<int32_t>(kAxRmtFrameSamples);
    if (nextDspPtr >= rmtBuffLen) {
        nextDspPtr = 0;
    }
    if (rmtCpuPtr >= rmtDspPtr &&
        rmtCpuPtr < rmtDspPtr + static_cast<int32_t>(kAxRmtFrameSamples)) {
        rmtCpuPtr = nextDspPtr;
    }
    SetSda(ctx, kRmtDspPtr, static_cast<uint32_t>(nextDspPtr));
    SetSda(ctx, kRmtCpuPtr, static_cast<uint32_t>(rmtCpuPtr));

    const uint32_t guestStack = ctx->gpr[1];
    const uint32_t rmtStack = (guestStack - 48u) & ~7u;
    for (uint32_t i = 0; i < 4; ++i) {
        Memory::Write32(rmtStack + i * 4u, rmt[i]);
    }

    const uint32_t outFrame = Sda(ctx, kOutFrame);
    ctx->gpr[1] = rmtStack;
    ctx->gpr[3] = kAxOutSBuffer;
    ctx->gpr[4] = kAxOutBuffer + outFrame * kAxFrameSize;
    ctx->gpr[5] = rmtStack;
    func_801A0E90(ctx);  // __AXNextFrame
    ctx->gpr[1] = guestStack;

    uint32_t nextOutFrame = outFrame + 1u;
    if (mode == 1u) {
        nextOutFrame %= 3u;
    } else {
        nextOutFrame &= 1u;
        AIInitDMA_80123fcc(kAxOutBuffer + nextOutFrame * kAxFrameSize, kAxFrameSize);
    }
    SetSda(ctx, kOutFrame, nextOutFrame);

    (void)kAxLocalProfile;
    ctx->gpr[3] = cycles;
}

void NativeAXOutAiCallback(CpuContext* ctx) {
    if (g_aiCallbackCalls.fetch_add(1, std::memory_order_relaxed) == 0) {
        RT_LOGF(RT_TAG_AUDIO, "audio: native AXOut AI callback active\n");
    }
    if (Sda(ctx, kDspDoneFlag) == 1u) {
        return;
    }

    if (Sda(ctx, kOutDspReady) == 1u) {
        SetSda(ctx, kOutDspReady, 0);
        NativeAXOutNewFrame(ctx);
    } else {
        SetSda(ctx, kOutDspReady, 2);
        AxDspHle::AssertTask(kAxDspTask);
    }

    if (Sda(ctx, kOutputBufferMode) == 1u) {
        const uint32_t dmaFrame = Sda(ctx, kAiDmaFrame);
        const uint32_t outFrame = Sda(ctx, kOutFrame);
        AIInitDMA_80123fcc(kAxOutBuffer + dmaFrame * kAxFrameSize, kAxFrameSize);
        const uint32_t nextDmaFrame = (dmaFrame + 1u) % 3u;
        if (nextDmaFrame != outFrame) {
            SetSda(ctx, kAiDmaFrame, nextDmaFrame);
        }
    }
}

void NativeAXDSPResumeCallback(CpuContext* ctx) {
    if (Sda(ctx, kOutDspReady) == 2u) {
        SetSda(ctx, kOutDspReady, 0);
        NativeAXOutNewFrame(ctx);
        const uint32_t exceedCallback = Sda(ctx, kExceedCallback);
        if (exceedCallback != 0) {
            InvokeIndirectCpu(exceedCallback, ctx);
        }
    } else {
        SetSda(ctx, kOutDspReady, 1);
    }
}

const RawDispatchRecord kAiCallbackRecord{0x801A1BA0u, &NativeAXOutAiCallback, 0, false};
const RawDispatchRecord kNewFrameRecord{0x801A1940u, &NativeAXOutNewFrame, 0, false};
const RawDispatchRecord kResumeCallbackRecord{0x801A1C60u, &NativeAXDSPResumeCallback, 0, false};

}  // namespace

void Install() {
    const char* value = std::getenv("NSMBW_AX_OUT");
    if (value == nullptr || value[0] == '\0' || std::strcmp(value, "off") == 0) {
        return;
    }
    if (std::strcmp(value, "native") != 0 && std::strcmp(value, "on") != 0 &&
        std::strcmp(value, "1") != 0) {
        RT_LOGF(RT_TAG_AUDIO, "audio: unknown NSMBW_AX_OUT=%s; native AXOut disabled\n", value);
        return;
    }
    if (DetClock::Enabled() || std::getenv("NSMBW_NO_COUCH_FASTPATHS") != nullptr) {
        RT_LOGF(RT_TAG_AUDIO, "audio: native AXOut disabled outside couch play\n");
        return;
    }

    TranslatedFunctionRegistry::SetRuntimeRedirect(&kAiCallbackRecord);
    TranslatedFunctionRegistry::SetRuntimeRedirect(&kNewFrameRecord);
    TranslatedFunctionRegistry::SetRuntimeRedirect(&kResumeCallbackRecord);
    RT_LOGF(RT_TAG_AUDIO, "audio: native AXOutNewFrame and AI callback enabled\n");
}

}  // namespace AxOutNative
