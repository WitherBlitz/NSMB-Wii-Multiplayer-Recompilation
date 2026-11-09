#pragma once

// A native FxReverbHi::UpdateBuffer for couch play (axfx_reverb_native.cpp). Install, once at
// startup after lockstep (LAN) mode is decided, routes the game's calls to it; in lockstep play it
// does nothing and the translated, bit-exact reverb runs.
namespace AxfxReverbNative {
void Install();
// Runs one reverb block from the ax-mix worker; false if the buffers are not plain RAM.
bool RunWorker(uint32_t self, uint32_t buffers);
} // namespace AxfxReverbNative
