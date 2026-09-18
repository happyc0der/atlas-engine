// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// The components a scene entity may have.
///
/// Deliberately few, and deliberately about presentation. This is the scene graph: names,
/// transforms, parentage, and what to draw. It is **not** the grand-strategy database, and
/// nothing here should acquire a field because a future game might want one. Provinces,
/// populations, markets and armies will live in structure-of-arrays tables chosen from
/// measured query patterns, for the reasons in docs/adr/0004-scene-ecs-vs-simulation-storage.md.
///
/// Every component here is plainly copyable and free of pointers, because it gets written to
/// a file and read back.

#include <atlas/assets/asset_id.hpp>
#include <atlas/math/matrix.hpp>
#include <atlas/math/vector.hpp>
#include <atlas/rhi/types.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace atlas::scene {

/// An entity's durable identity.
///
/// The value written to a file and used to refer to an entity across a save and reload. The
/// entity handle the underlying library hands out is **not** this: it is reused as entities
/// are destroyed and created, and it means nothing outside one run. Serialising one would
/// produce a file that appeared to load and referred to the wrong things.
///
/// Zero is reserved for "no entity", so a zeroed structure is not a reference to the first
/// one ever created.
enum class StableId : std::uint64_t { None = 0 };

[[nodiscard]] constexpr bool valid(StableId id) noexcept {
    return id != StableId::None;
}

/// A human-readable label. Not an identifier: two entities may share a name.
struct Name {
    std::string value;
};

/// Position, rotation and scale relative to the parent, or to the world when there is none.
struct LocalTransform {
    math::Vec2 position;
    /// Radians, counter-clockwise on screen.
    float rotation = 0.0F;
    math::Vec2 scale{.x = 1.0F, .y = 1.0F};
};

/// The composed transform, in world space.
///
/// Derived, never authored: recomputed from the local transforms whenever the hierarchy
/// changes. It is stored rather than recomputed per use because many things read it and one
/// thing writes it, and it is not serialised because a file that stored both could disagree
/// with itself.
struct WorldTransform {
    math::Mat4 matrix;
};

/// What an animator has made of an entity this frame, and how far through a clip it is.
///
/// **Derived, never authored, and never written to a file** — the same standing as
/// `WorldTransform`, and for a sharper reason. The authored `LocalTransform` is what a person
/// typed, what the edit history owns, and what a save records. This is what playback added on
/// top of it. Keeping them apart is what lets both exist at once: `update_transforms` composes
/// the two, so an entity can be dragged **while** it animates, an undo undoes the drag and
/// never the animation, and the bytes a scene saves are unchanged by having played.
///
/// An offset rather than a replacement, deliberately. A replacement would make the inspector's
/// position field have no visible effect while a clip runs, which is the same confusion as
/// writing the authored value directly with the undo breakage removed and the confusion kept.
///
/// The clock lives here for the same reason the offset does: it is not authored, not saved,
/// and written only by the animator. One component, so the rule about who may write what names
/// one thing rather than two.
struct AnimationPose {
    /// Added to the authored position.
    math::Vec2 position_offset;
    /// Added to the authored rotation, in radians. Unwrapped, so a clip taking a full turn
    /// reads as one turn rather than folding back to zero.
    float rotation_offset = 0.0F;
    /// Multiplied with the authored scale, so an untouched pose is the identity.
    math::Vec2 scale_factor{.x = 1.0F, .y = 1.0F};

    /// How far into the clip this entity is, in nanoseconds. Integer, so a clip that loops for
    /// an hour is exact at the end of it rather than a float sum that has drifted.
    std::uint64_t elapsed_ns = 0;

    /// The region of the texture this frame, when the clip sets one. Preferred over the
    /// sprite's own when present, which is the whole of frame animation: nothing in the
    /// renderer changes, because a sprite already carries a rectangle.
    std::optional<math::Rect> frame_uv;
};

/// Parentage and sibling order.
///
/// Children are listed explicitly rather than only linked from child to parent, so that
/// drawing and serialisation can walk the tree in a stable order without sorting first.
struct Hierarchy {
    StableId parent = StableId::None;
    std::vector<StableId> children;
};

/// What to draw for this entity.
///
/// Holds an asset identifier rather than a graphics handle. A component that owned a texture
/// would be a component that cannot be serialised, cannot survive a reload, and has to be
/// destroyed in the right order.
struct SpriteRenderData {
    assets::AssetId texture;
    /// Size in world units.
    math::Vec2 size{.x = 1.0F, .y = 1.0F};
    /// Region of the texture to show, normalised.
    math::Rect uv{.position = {.x = 0.0F, .y = 0.0F}, .size = {.x = 1.0F, .y = 1.0F}};
    rhi::Colour tint{.r = 1.0F, .g = 1.0F, .b = 1.0F, .a = 1.0F};
    /// Higher draws later, so higher is in front. Ordering within a layer is by stable
    /// identifier, so that what overlaps what does not depend on creation order.
    std::int32_t layer = 0;
    bool visible = true;
};

/// A clip to play on this entity, and how.
///
/// **Authored**: edited through the history, saved with the scene, and never written by the
/// animator. Its counterpart is `AnimationPose`, which the animator owns and nothing saves.
///
/// Deliberately **no running time here.** The clock lives in the pose, so a saved scene records
/// what the author chose and never how long the application happened to be open. Without that
/// split the sandbox's save-load-save byte check would fail the moment a clip started playing,
/// and the file would change every second for no authored reason.
struct Animator {
    /// The clip asset. An identifier that resolves to nothing means the entity is not animated,
    /// which is also what a clip still loading looks like.
    assets::AssetId clip;
    /// Where playback begins, in milliseconds. What a scrub control sets, and what a save
    /// records; the pose's own clock starts from here.
    std::uint32_t start_ms = 0;
    /// Multiplier on the passage of time. Clamped to [0, 100] and never negative: running a
    /// clip backwards is a different feature with a different name, and letting time go
    /// backwards here would make the clock's integer exactness pointless.
    float speed = 1.0F;
    bool playing = true;
    /// Per instance rather than per clip, so one clip can loop on one entity and play once on
    /// another without being two files.
    std::uint8_t loop = 1;
};

/// The bounds an animator's fields are held to, wherever they are read or offered.
///
/// Beside the component rather than inside the reader, because three places need to agree: the
/// serialiser refuses a file outside them, the animator clamps to them, and the inspector must
/// not offer a value the other two would reject. A widget that let a person type a speed the
/// engine then clamps shows a number the engine does not use.
///
/// A day is not a meaningful limit on authoring — no clip is a day long — but it is a bound on
/// a number that arrives from a file and is narrowed into a smaller field, which is where a
/// silent truncation would otherwise live.
inline constexpr std::uint32_t kMaxAnimatorStartMs = 86'400'000;
inline constexpr float kMaxAnimatorSpeed = 100.0F;

/// The name of a loop mode, and the mode a name spells.
///
/// Declared here, beside the field, because two unrelated places need it and a second copy is
/// how a file comes to say "ping-pong" where a panel says "once". The serialiser writes these
/// names into the scene file and the inspector shows them in a drop-down; there is no third
/// spelling anywhere.
///
/// A value this build does not know reads as "once", which is what the animator does with it
/// too. The two must agree, or a hand-edited file would round-trip into something that plays
/// differently from what it says.
[[nodiscard]] std::string_view animation_loop_name(std::uint8_t loop) noexcept;

/// The stored value for a loop mode's name, or nothing when the name is not one.
[[nodiscard]] std::optional<std::uint8_t> animation_loop_value(std::string_view name) noexcept;

/// Every loop mode's name, in stored order, for a panel that offers the choice.
[[nodiscard]] std::span<const std::string_view> animation_loop_names() noexcept;

/// A viewpoint attached to an entity.
struct Camera {
    /// Pixels per world unit.
    float zoom = 1.0F;
    /// Only one camera is drawn from; this picks which.
    bool active = false;
};

}  // namespace atlas::scene
