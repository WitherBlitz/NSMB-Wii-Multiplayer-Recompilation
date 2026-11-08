#pragma once

#include "texture.hpp"
#include <optional>

namespace aurora::gfx::texture_replacement {
void initialize() noexcept;
void shutdown() noexcept;
void register_tlut(const GXTlutObj* obj, const void* data, GXTlutFmt format, uint16_t entries) noexcept;
void load_tlut(const GXTlutObj* obj, uint32_t idx) noexcept;
std::optional<TextureHandle> find_replacement(const GXTexObj_& obj) noexcept;
std::string build_texture_replacement_name(const GXTexObj_& obj) noexcept;
// Turns replacement on or off (aurora_set_texture_replacements), from any thread: apply_pending makes the
// change at the next frame's start, re-indexing the folder when it turns on.
void request_enabled(bool enabled) noexcept;
void apply_pending() noexcept;
} // namespace aurora::gfx::texture_replacement
