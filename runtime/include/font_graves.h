// font_graves.h - letters with a grave accent that the disc's fonts lack, made from ones they have.
#pragma once

#include <cstdint>
#include <vector>

namespace FontGraves {

// Adds à è ì ò ù À È Ì Ò Ù to an RFNT font (IA4 sheets) wherever it lacks them: the base letter (i
// without its dot) with the grave accent of a letter that has one (è, à or ù; for capitals À, È or Ù,
// else the small one raised over the capital), in free glyph cells. Returns how many it added; 0
// leaves the font as it was.
int Add(std::vector<uint8_t>& font);

} // namespace FontGraves
