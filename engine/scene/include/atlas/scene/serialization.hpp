// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Writing a scene out and reading it back.
///
/// **Canonical.** Entities are written in identifier order and each entity's components in a
/// fixed order, so that saving a scene, loading it, and saving it again produces byte-for-byte
/// the same file. Without that, a version-control diff of a scene would be noise, and any
/// future hash of scene state would depend on the order things happened to be created.
///
/// **Versioned.** Every file records the schema version it was written with. A file from a
/// newer version is refused rather than half-understood. Migration is written when the first
/// real schema change happens; the policy is recorded now, the code is not, because a
/// migration with nothing to migrate is untested by construction.
///
/// **Untrusted.** A scene file may come from anywhere. Counts, lengths, references and
/// numbers are validated before anything is allocated or indexed, and parsing never throws
/// across this boundary.

#include <atlas/core/result.hpp>
#include <atlas/scene/scene.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace atlas::scene {

/// The schema version this build writes.
///
/// Incremented whenever the meaning or the shape of the format changes. See
/// docs/adr/0007-scene-file-format.md.
inline constexpr std::uint32_t kSceneFormatVersion = 1;

/// Serialise to text.
///
/// Deterministic: the same scene always produces the same bytes.
[[nodiscard]] Result<std::string> to_text(const Scene& scene);

/// Parse text into `scene`, replacing whatever it held.
///
/// On failure the scene is left empty rather than half-populated, because a partially loaded
/// scene is harder to reason about than an empty one and every caller would have to handle
/// it.
[[nodiscard]] Status from_text(Scene& scene, std::string_view text);

}  // namespace atlas::scene
