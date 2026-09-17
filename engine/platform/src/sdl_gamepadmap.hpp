// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/// \file
/// Atlas's gamepad identifiers, mapped to the window system's, in both directions.
///
/// Private to the platform, but in a header so the bijection can be walked by a test. The
/// same arrangement as `sdl_keymap.hpp`, for the same reason: a table that is wrong in one
/// entry stops one button working, and nobody notices until they press it.

#include <atlas/platform/gamepad.hpp>

#include <SDL3/SDL_gamepad.h>

namespace atlas::platform::detail {

[[nodiscard]] SDL_GamepadButton to_sdl_button(GamepadButton button) noexcept;
[[nodiscard]] GamepadButton from_sdl_button(SDL_GamepadButton button) noexcept;
[[nodiscard]] SDL_GamepadAxis to_sdl_axis(GamepadAxis axis) noexcept;
[[nodiscard]] GamepadAxis from_sdl_axis(SDL_GamepadAxis axis) noexcept;

}  // namespace atlas::platform::detail
