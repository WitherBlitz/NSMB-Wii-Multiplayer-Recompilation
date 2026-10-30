// Native AXNextFrame for the stereo command list. Mirrors
// ../wii-ipl/libs/RVL_SDK/src/ax/AXCL.c; command bytes are compared against the
// translated original before the native path is enabled.
#include "ax_next_frame_native.h"

#include "memory.h"

#include <cstdint>

namespace AxNextFrameNative {
namespace {

// Small-data offsets used by the translated AX library.
constexpr uint32_t kPbs = 20600;
constexpr uint32_t kClWrite = 20756;
constexpr uint32_t kMode = 20760;
constexpr uint32_t kCycles = 20764;
constexpr uint32_t kCompressor = 20768;
constexpr uint32_t kCompressorTable = 20772;
constexpr uint32_t kCompressorRelease = 20776;
constexpr uint32_t kMasterVolume = 20778;
constexpr uint32_t kAuxAVolume = 20780;
constexpr uint32_t kAuxBVolume = 20782;
constexpr uint32_t kAuxCVolume = 20784;
constexpr uint32_t kAuxAFlag = 20800;
constexpr uint32_t kAuxBFlag = 20804;
constexpr uint32_t kAuxCFlag = 20808;
constexpr uint32_t kAuxInIndex = 20848;
constexpr uint32_t kAuxOutIndex = 20852;
constexpr uint32_t kStudio = 0x8037CDA0u;
constexpr uint32_t kAuxBase = 0x80378D20u;
constexpr uint32_t kAuxStride = 1536u;
constexpr uint32_t kListBytes = 128u;

uint32_t Sda32(CpuContext* ctx, uint32_t offset) {
    return Memory::Read32(ctx->gpr[13] - offset);
}
void SetSda32(CpuContext* ctx, uint32_t offset, uint32_t value) {
    Memory::Write32(ctx->gpr[13] - offset, value);
}
uint16_t Sda16(CpuContext* ctx, uint32_t offset) {
    return Memory::Read16(ctx->gpr[13] - offset);
}

void Cmd16(CpuContext* ctx, uint16_t value) {
    const uint32_t at = Sda32(ctx, kClWrite);
    Memory::Write16(at, value);
    SetSda32(ctx, kClWrite, at + 2u);
}
void Cmd32(CpuContext* ctx, uint32_t value) {
    Cmd16(ctx, static_cast<uint16_t>(value >> 16));
    Cmd16(ctx, static_cast<uint16_t>(value));
}
void AddCycles(CpuContext* ctx, uint32_t value) {
    SetSda32(ctx, kCycles, Sda32(ctx, kCycles) + value);
}

uint32_t AuxInput(CpuContext* ctx, uint32_t flagOffset) {
    if (Sda32(ctx, flagOffset) == 0) {
        return 0;  // inactive
    }
    return kAuxBase + Sda32(ctx, kAuxInIndex) * kAuxStride;
}
uint32_t AuxOutput(CpuContext* ctx) {
    return kAuxBase + Sda32(ctx, kAuxOutIndex) * kAuxStride;
}
void Aux(CpuContext* ctx, uint32_t flagOffset, uint32_t volumeOffset, uint16_t command) {
    const uint32_t input = AuxInput(ctx, flagOffset);
    if (input == 0) {
        return;
    }
    Cmd16(ctx, command);
    Cmd16(ctx, Sda16(ctx, volumeOffset));
    Cmd32(ctx, input);
    Cmd32(ctx, AuxOutput(ctx));
    AddCycles(ctx, 2235);
}

}  // namespace

bool Run(CpuContext* ctx) {
    if (Sda32(ctx, kMode) != 0) {
        return false;  // stereo only; DPL2/surround stay translated
    }
    SetSda32(ctx, kCycles, 7811);
    Cmd16(ctx, 0);  // COMMAND_SETUP
    Cmd32(ctx, kStudio);
    AddCycles(ctx, 4126);
    Cmd16(ctx, 1);  // COMMAND_ADD_TO_LR
    Cmd32(ctx, ctx->gpr[3]);
    AddCycles(ctx, 733);
    Cmd16(ctx, 4);  // COMMAND_PROCESS
    Cmd32(ctx, Sda32(ctx, kPbs));
    Aux(ctx, kAuxAFlag, kAuxAVolume, 5);
    Aux(ctx, kAuxBFlag, kAuxBVolume, 6);
    Aux(ctx, kAuxCFlag, kAuxCVolume, 7);
    if (Sda32(ctx, kCompressor) != 0) {
        Cmd16(ctx, 10);  // COMMAND_COMPRESSOR
        Cmd16(ctx, 32768);
        Cmd16(ctx, Sda16(ctx, kCompressorRelease));
        Cmd32(ctx, Sda32(ctx, kCompressorTable));
        AddCycles(ctx, 1850);
    }
    Cmd16(ctx, 13);  // COMMAND_WM_OUTPUT
    const uint32_t rmt = ctx->gpr[5];
    for (uint32_t i = 0; i < 4; ++i) {
        Cmd32(ctx, Memory::Read32(rmt + i * 4u));
    }
    AddCycles(ctx, 409);
    Cmd16(ctx, 11);  // COMMAND_OUTPUT
    Cmd16(ctx, Sda16(ctx, kMasterVolume));
    Cmd32(ctx, ctx->gpr[3]);
    Cmd32(ctx, ctx->gpr[4]);
    AddCycles(ctx, 1172);
    Cmd16(ctx, 14);  // COMMAND_END
    AddCycles(ctx, 30);
    return true;
}

}  // namespace AxNextFrameNative
