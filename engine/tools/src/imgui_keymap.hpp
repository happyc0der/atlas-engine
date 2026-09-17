// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/// \file
/// Atlas's key identifiers, mapped to the overlay library's own.
///
/// Private to the overlay, but in a header so the mapping can be walked by a test. The same
/// arrangement as the platform's `sdl_keymap.hpp`, and for the same reason: a table that is
/// wrong in one entry stops one key working, which nobody notices until they reach for it.
///
/// The alternative to this table was the library's own window-system backend, which would have
/// required a third-party window type inside this module and therefore a boundary violation.
/// Driving the overlay from Atlas's own event types costs this file and keeps the boundary.
///
/// **Every key maps, not only the navigation ones.** The overlay's text fields need letters,
/// digits and punctuation to receive shortcuts: characters arrive separately as `TextInput`,
/// but Ctrl+A, Ctrl+C and Ctrl+Z are key events, and a table covering only arrows and modifiers
/// silently makes every one of them dead.

#include <atlas/platform/key.hpp>

#include <imgui.h>

namespace atlas::tools::detail {

/// The overlay library's identifier for `key`, or `ImGuiKey_None` for one it does not model.
[[nodiscard]] ImGuiKey to_imgui_key(platform::Key key) noexcept;

/// The overlay library's index for `button`, or -1 for one it does not model.
[[nodiscard]] int to_imgui_button(platform::MouseButton button) noexcept;

}  // namespace atlas::tools::detail
