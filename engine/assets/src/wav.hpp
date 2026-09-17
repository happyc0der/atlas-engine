// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/// \file
/// RIFF/WAVE decoding.
///
/// Private to the assets module, but in a header so the reader can be driven directly by a
/// test. The same arrangement `sdl_keymap.hpp` and `text_split.hpp` already use, and for the
/// same reason: this is where the behaviour that can be wrong lives, and a hostile-input test
/// that has to go through a registry and two threads to reach it is testing the wrong thing.

#include <atlas/assets/importer.hpp>
#include <atlas/core/result.hpp>

#include <cstddef>
#include <span>
#include <string_view>

namespace atlas::assets::detail {

/// Whether the leading bytes are a RIFF/WAVE header.
///
/// Cheap and total: it reads at most twelve bytes and never allocates, so `import_audio` can
/// use it to choose a decoder without trusting the file's name.
[[nodiscard]] bool looks_like_wav(std::span<const std::byte> bytes) noexcept;

/// Decode a RIFF/WAVE file.
///
/// Every declared length is checked against the bytes actually present before anything is
/// allocated on the strength of it. Failure is `MalformedData` saying what was wrong and
/// where, never a partial result.
[[nodiscard]] Result<ImportedAudio> import_wav(std::span<const std::byte> bytes,
                                               std::string_view debug_name);

}  // namespace atlas::assets::detail
