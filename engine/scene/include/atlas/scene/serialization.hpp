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
/// **Versioned, and since version 2 the rule is written down.** Every file records the schema
/// version it was written with. A file from a newer version is refused rather than
/// half-understood; a file from an older one is read.
///
/// The rule, decided by ADR-0012: **a version that only appends components needs no migration
/// code at all.** A component added at version N+1 is read when its key is present and left
/// absent when it is not, and a writer at version N never produces the key — so an older file
/// is already a valid newer one with some components missing. Migration code is required only
/// for a change that is not a pure append: a renamed key, a changed unit, a split field.
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
/// docs/adr/0007-scene-file-format.md and docs/adr/0012-scene-format-v2.md.
///
/// | Version | Change |
/// |---|---|
/// | 1 | M5. Name, transform, sprite, camera. |
/// | 2 | M13. Appends `animator`. A version 1 file loads unchanged, with no animators. |
inline constexpr std::uint32_t kSceneFormatVersion = 2;

/// The oldest version this build reads.
///
/// One, and it will stay one until a change that is not a pure append makes reading an old
/// file cost something. Refusing an old file is a decision to abandon it, and the bar for that
/// is higher than the bar for adding a key.
inline constexpr std::uint32_t kMinSceneFormatVersion = 1;

/// Serialise to text.
///
/// Deterministic: the same scene always produces the same bytes.
[[nodiscard]] Result<std::string> to_text(const Scene& scene);

/// Parse text into `scene`, replacing whatever it held.
///
/// On failure the scene is left empty rather than half-populated, because a partially loaded
/// scene is harder to reason about than an empty one and every caller would have to handle
/// it.
///
/// A scene file is untrusted input. The text itself is bounded before it is parsed — which is
/// the only bound a document parser can enforce, since every count inside a document can only
/// be read once the whole thing has been allocated — and every count within it is bounded
/// before anything is reserved.
[[nodiscard]] Status from_text(Scene& scene, std::string_view text);

}  // namespace atlas::scene
