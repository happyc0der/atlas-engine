// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/// \file
/// Reading a string table document (ADR-0016).
///
/// Private to the assets module, but in a header so the parser can be driven directly by a
/// test — the arrangement `animation_clip.hpp`, `wav.hpp` and `text_split.hpp` already use, and
/// for the same reason: a hostile-input test that has to go through a registry and two worker
/// threads to reach the code under test is testing the wrong thing.
///
/// The discipline is the clip reader's, inherited rather than re-derived: **the bound that
/// matters is on the input text itself, before parsing begins**, because by the time any count
/// inside a document can be read the whole document is already in memory. Everything after the
/// parse is about refusing a document that means something impossible.

#include <atlas/assets/importer.hpp>
#include <atlas/core/result.hpp>

#include <cstddef>
#include <span>
#include <string_view>

namespace atlas::assets::detail {

/// Parse a string table document.
///
/// Failure is `MalformedData` saying what was wrong and which entry it was in, never a partial
/// table. The document names and versions itself, and a newer version is refused rather than
/// half-understood, exactly as the scene and clip files are.
[[nodiscard]] Result<ImportedStringTable> parse_string_table(std::span<const std::byte> bytes,
                                                             std::string_view debug_name);

}  // namespace atlas::assets::detail
