#pragma once

#include "isa/ppc_isa_context.h"

namespace AxNextFrameNative {

// Writes the next command list for stereo output. Returns false when the mode is
// not stereo, in which case the caller must run the translated original.
bool Run(CpuContext* ctx);

}  // namespace AxNextFrameNative
