#pragma once

#include <cstdint>

// Native AXFXDelayExpCallback. The translated effect is the fixed-point AXFXDelayExp
// network; this version keeps the state in guest memory and runs the same integer
// operations on host pointers. Installed only outside lockstep play.
namespace AxfxDelayNative {

void Install();

// Runs one delay block. Returns false when the effect is not plain RAM or the state is
// invalid; the caller then runs the translated original.
bool Run(uint32_t state, uint32_t buffers);

} // namespace AxfxDelayNative
