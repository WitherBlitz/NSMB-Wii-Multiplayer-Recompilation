// Two more nw4r::g3d leaves that the per-node path hits every frame: the resource node
// lookup the bytecode walk uses, and the texture-pattern override selector.
//
// 0x8023B300 ResMdl::GetResNode(u32)
// 0x80245440 AnmObjTexPatOverride::GetResult(TexPatAnmResult*, u32)
#include "hle_stubs.h"

#include "memory.h"

#include <cstdint>

extern "C" void NW4R_G3D_GetResNode_HLE(CpuContext* ctx) {
    const uint32_t mdl = ctx->gpr[3];
    const uint32_t id = ctx->gpr[4];
    uint32_t result = 0;
    const uint32_t data = MemoryInline::FlatRead32(mdl);
    uint32_t arrayOffset = MemoryInline::FlatRead32(data + 20);
    if (arrayOffset != 0) {
        const uint32_t array = data + arrayOffset;
        const uint32_t entry = MemoryInline::FlatRead32(array + (id + 1u) * 16u + 20u);
        result = entry != 0 ? array + entry : 0;
    }
    ctx->gpr[3] = result;
}

PPC_NATIVE_OVERRIDE_VOID(8023B300, NW4R_G3D_GetResNode_HLE, (CpuContext* ctx), (ctx));

extern "C" void NW4R_G3D_TexPatGetResult_HLE(CpuContext* ctx) {
    const uint32_t self = ctx->gpr[3];
    const uint32_t resultPtr = ctx->gpr[4];
    const uint32_t id = ctx->gpr[5];
    uint32_t result = resultPtr;
    bool found = false;
    const int32_t count = static_cast<int32_t>(MemoryInline::FlatRead32(self + 24));
    const uint32_t array = MemoryInline::FlatRead32(self + 28);
    for (int32_t i = count - 1; i >= 0; --i) {
        const uint32_t child = MemoryInline::FlatRead32(array + static_cast<uint32_t>(i) * 4u);
        if (child == 0) {
            continue;
        }
        const uint32_t binding = MemoryInline::FlatRead32(child + 20) + id * 2u;
        if ((MemoryInline::FlatRead16(binding) & 0xC000u) != 0) {
            continue;
        }
        const uint32_t vtable = MemoryInline::FlatRead32(child);
        const uint32_t entryPoint = MemoryInline::FlatRead32(vtable + 56);
        ctx->gpr[3] = child;
        ctx->gpr[4] = resultPtr;
        ctx->gpr[5] = id;
        ctx->gpr[12] = entryPoint;
        ctx->ctr = entryPoint;
        ctx->lr = 0x802454ACu;
        InvokeIndirectCpu(entryPoint, ctx);
        result = ctx->gpr[3];
        if (MemoryInline::FlatRead8(result) != 0 || MemoryInline::FlatRead8(result + 1) != 0) {
            found = true;
            break;
        }
    }
    if (!found) {
        // No child provided the id (or every child's result was empty): the translated body
        // clears the two existence bytes and returns the caller's buffer.
        MemoryInline::FlatWrite8(resultPtr, 0);
        MemoryInline::FlatWrite8(resultPtr + 1, 0);
        result = resultPtr;
    }
    ctx->gpr[3] = result;
}

PPC_NATIVE_OVERRIDE_VOID(80245440, NW4R_G3D_TexPatGetResult_HLE, (CpuContext* ctx), (ctx));
