#include "runtime_product.h"

#ifndef NSMBW_GAME_ID
#define NSMBW_GAME_ID "SMNE01"  // set by runtime/CMakeLists.txt
#endif

namespace RuntimeProduct {
namespace {

constexpr std::string_view kGameId = NSMBW_GAME_ID;
// Newer Super Mario Bros. Wii (projects/nsmbw/recomp-newer.yml): translated from v1.30's code for
// the USA, whose DLCodeUS_1.bin is this size; another version runs other code.
constexpr bool kNewer = kGameId == "SMNE03";

} // namespace

const Descriptor& Active() noexcept {
    static constexpr Descriptor descriptor = kNewer
        ? Descriptor{
              Kind::BaseGame,
              "Newer Super Mario Bros. Wii",
              kGameId,
              "Newer Super Mario Bros. Wii v1.30 for the USA (NTSC, SMNE03)",
              "NewerRes/DLCodeUS_1.bin",
              463252,
          }
        : Descriptor{
              Kind::BaseGame,
              "New Super Mario Bros. Wii",
              kGameId,
          };
    return descriptor;
}

} // namespace RuntimeProduct
