#pragma once

// Test seam (test_pad.cpp): NSMBW_PAD_SCRIPT plugs in a virtual controller and presses its buttons.
namespace TestPad {

// Once a frame on the thread that pumps SDL's events (NetplayUi::Draw).
void Update();

} // namespace TestPad
