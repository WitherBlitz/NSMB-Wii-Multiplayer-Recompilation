#include "runtime_config.h"

#include <iostream>
#include <sstream>

namespace {
bool Require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}
} // namespace

int main() {
    {
        std::istringstream input("[video]\nmetalfx_spatial_upscaling = true\n");
        const RuntimeUserConfig config = RuntimeConfigFile::ParseConfig(input, "enabled.toml");
        if (!Require(config.metalFxSpatialUpscaling == std::optional<bool>{true},
                     "valid MetalFX setting was not loaded")) return 1;
    }
    {
        std::istringstream input("[video]\nmetalfx_spatial_upscaling = false\n");
        const RuntimeUserConfig config = RuntimeConfigFile::ParseConfig(input, "disabled.toml");
        if (!Require(config.metalFxSpatialUpscaling == std::optional<bool>{false},
                     "false MetalFX setting was not loaded")) return 1;
    }
    {
        std::istringstream input("[video]\nmetalfx_spatial_upscaling = \"yes\"\n");
        const RuntimeUserConfig config = RuntimeConfigFile::ParseConfig(input, "invalid.toml");
        if (!Require(!config.metalFxSpatialUpscaling, "invalid MetalFX setting was accepted")) return 1;
    }
    {
        std::istringstream input(
            "[controller]\ntilt_left = \"left_trigger\"\ntilt_right = \"right_trigger\"\nshake = \"unmapped\"\n");
        const RuntimeUserConfig config = RuntimeConfigFile::ParseConfig(input, "remote-actions.toml");
        if (!Require(config.remoteActions[0] == std::optional<std::string>{"left_trigger"},
                     "left tilt binding was not loaded")) return 1;
        if (!Require(config.remoteActions[1] == std::optional<std::string>{"right_trigger"},
                     "right tilt binding was not loaded")) return 1;
        if (!Require(config.remoteActions[2] == std::optional<std::string>{"unmapped"},
                     "shake binding was not loaded")) return 1;
    }
    return 0;
}
