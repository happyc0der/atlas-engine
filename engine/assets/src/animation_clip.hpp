// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/// \file
/// Reading an animation clip document.
///
/// Private to the assets module, but in a header so the parser can be driven directly by a
/// test. The same arrangement `wav.hpp`, `sdl_keymap.hpp` and `text_split.hpp` already use, and
/// for the same reason: a hostile-input test that has to go through a registry and two worker
/// threads to reach the code under test is testing the wrong thing.
///
/// **What "hostile input" means for a document parser, which is not what it means for the
/// audio reader.** That reader checks a declared size against the bytes actually present before
/// it allocates. This one cannot: by the time any count inside the document is readable, the
/// whole document has already been parsed into memory. So the bound that matters here is on the
/// **input text itself, before parsing begins**, and everything after the parse is about
/// refusing a document that means something impossible rather than about refusing an
/// allocation.

#include <atlas/assets/importer.hpp>
#include <atlas/core/result.hpp>

#include <cstddef>
#include <span>
#include <string_view>

namespace atlas::assets::detail {

/// Parse an animation clip document.
///
/// Failure is `MalformedData` saying what was wrong and where, never a partial clip. The
/// document names and versions itself, and a newer version is refused rather than
/// half-understood, exactly as the scene file is.
[[nodiscard]] Result<ImportedAnimationClip> parse_animation_clip(std::span<const std::byte> bytes,
                                                                 std::string_view debug_name);

}  // namespace atlas::assets::detail
