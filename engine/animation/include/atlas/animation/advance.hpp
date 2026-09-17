// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Moving every animated entity on by a step, and the clips they play.
///
/// **Determinism, stated plainly.** This is presentation. It is **not** deterministic across
/// machines, because the frame time it is driven by is not: two machines run different numbers
/// of frames per second and so hand it different steps. Nothing it produces may be hashed, may
/// enter simulation state, or may reach a saved file — and none of that is enforced by good
/// intentions. The module cannot link `simulation`, the pose it writes is not serialised, and
/// the Strategy Lab's simulation library is fenced at configure time to link nothing but the
/// kernel.
///
/// What it **is**, and what the tests rest on: bit-exact given the same sequence of steps on
/// one build. It reads no clock of its own — the step is whatever the caller says — which is
/// what lets a test drive a thousand frames in no time and get the same answer twice.
///
/// **Thread affinity: the main thread.** It writes scene components, and the scene asserts it.

#include <atlas/animation/clip.hpp>
#include <atlas/assets/asset_id.hpp>
#include <atlas/core/result.hpp>
#include <atlas/scene/scene.hpp>

#include <cstdint>
#include <memory>
#include <unordered_map>

namespace atlas::animation {

/// The clips available to play, by the asset they came from.
///
/// Insert-only on purpose. A clip that is replaced while an entity is playing it would change
/// the meaning of that entity's clock mid-stride; a reload therefore replaces the whole entry
/// and the next `advance` picks it up from wherever the clock had reached, which is the same
/// answer a person would expect from editing a file while watching it.
class ClipCache {
  public:
    /// Store a clip under an asset identifier, replacing any clip already there.
    void insert(assets::AssetId id, Clip clip);

    /// The clip for an asset, or nullptr when there is none.
    [[nodiscard]] const Clip* find(assets::AssetId id) const noexcept;

    [[nodiscard]] std::size_t size() const noexcept { return m_clips.size(); }

    void clear() noexcept { m_clips.clear(); }

  private:
    /// Looked up by identifier and never iterated, which is what makes an unordered container
    /// permitted here: nothing this produces is ordered, hashed, or written down.
    std::unordered_map<assets::AssetId, std::shared_ptr<const Clip>> m_clips;
};

/// What one call to `advance` did, for statistics and for noticing a missing clip.
struct AdvanceReport {
    /// Entities carrying an animator that were moved on.
    std::size_t advanced = 0;
    /// Animators naming a clip the cache does not have. Counted rather than logged, because a
    /// clip that is still loading is the ordinary case for the first few frames.
    std::size_t missing_clips = 0;
    /// Animators that reached the end of a clip that does not loop.
    std::size_t finished = 0;
};

/// Move every animated entity on by `dt_ns` and write the poses that result.
///
/// Writes `scene::AnimationPose` and nothing else. It never touches an authored component, so
/// an entity can be edited through the history in the same frame it is animated.
///
/// An animator naming a clip the cache does not have leaves the entity at the identity pose
/// rather than at its last one, so a clip that fails to load looks like no animation instead of
/// like a frozen one.
///
/// `dt_ns` is the caller's to choose. Both applications clamp it, so that a debugger pause does
/// not advance every clip by the length of the pause.
AdvanceReport advance(scene::Scene& scene, const ClipCache& clips, std::uint64_t dt_ns);

}  // namespace atlas::animation
