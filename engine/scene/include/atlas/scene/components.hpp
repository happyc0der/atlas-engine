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
#include <string>
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

/// A viewpoint attached to an entity.
struct Camera {
    /// Pixels per world unit.
    float zoom = 1.0F;
    /// Only one camera is drawn from; this picks which.
    bool active = false;
};

}  // namespace atlas::scene
