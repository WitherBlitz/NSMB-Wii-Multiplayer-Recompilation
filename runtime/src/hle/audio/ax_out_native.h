#pragma once

namespace AxOutNative {

// Replaces the translated AXOut AI callback and output-frame control path when
// NSMBW_AX_OUT=native. The DSP mix itself stays on the existing AX worker.
void Install();

}  // namespace AxOutNative
