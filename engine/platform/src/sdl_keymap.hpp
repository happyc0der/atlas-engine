// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Mapping between Atlas key identifiers and SDL scancodes.
///
/// Private to the platform implementation. Exposed as a header only so that the mapping can
/// be unit-tested directly: a silent hole in the table would otherwise show up as one key
/// that mysteriously does nothing.

#include <atlas/platform/key.hpp>

#include <SDL3/SDL_scancode.h>

namespace atlas::platform::detail {

/// Scancode for an Atlas key, or SDL_SCANCODE_UNKNOWN for Key::Unknown and Key::Count.
[[nodiscard]] SDL_Scancode to_sdl_scancode(Key key) noexcept;

/// Atlas key for a scancode, or Key::Unknown for anything not in the table.
[[nodiscard]] Key from_sdl_scancode(SDL_Scancode scancode) noexcept;

}  // namespace atlas::platform::detail
