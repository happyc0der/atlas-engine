// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// The scene: entities, their components, and the tree they form.
///
/// A wrapper around an entity-component library rather than the library itself. All of
/// Atlas's use of it is confined here, which is what makes the pin moveable and the library
/// replaceable, and which is why the library's own types do not appear in this header
/// despite ADR-0004 permitting them.
///
/// **Identity.** Callers refer to entities by `StableId`, never by the library's handle. The
/// handle is reused as entities come and go and means nothing outside one run; the stable
/// identifier is what survives a save and a reload.
///
/// **Ordering.** Anything that can be observed is ordered by stable identifier: drawing,
/// iteration, serialisation. The library stores components in whatever order suits its own
/// compaction, and inheriting that would make a saved file depend on the order things were
/// created. See docs/DETERMINISM.md.
///
/// **Thread affinity.** Main thread only.

#include <atlas/core/result.hpp>
#include <atlas/scene/components.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace atlas::scene {

/// A view of one entity, for iteration and inspection.
struct EntityView {
    StableId id = StableId::None;
    std::string_view name;
    StableId parent = StableId::None;
    std::size_t child_count = 0;
    bool has_sprite = false;
    bool has_camera = false;
};

class Scene {
  public:
    Scene();
    ~Scene();

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    Scene(Scene&& other) noexcept;
    Scene& operator=(Scene&& other) noexcept;

    /// Create an entity with a name and an identity of the scene's choosing.
    [[nodiscard]] StableId create(std::string_view name = {});

    /// Create an entity with a chosen identity, as loading does.
    ///
    /// Fails if the identifier is already in use or is the reserved none value, because
    /// silently renumbering would break every reference in the file being loaded.
    [[nodiscard]] Result<StableId> create_with_id(StableId id, std::string_view name = {});

    /// Destroy an entity and, recursively, its children.
    ///
    /// Children go too rather than being orphaned: a child whose parent no longer exists has
    /// a transform relative to nothing, and leaving that state reachable means every reader
    /// has to handle it.
    bool destroy(StableId id);

    [[nodiscard]] bool contains(StableId id) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    void clear();

    // --- components -------------------------------------------------------------------

    [[nodiscard]] std::string_view name(StableId id) const;
    void set_name(StableId id, std::string_view name);

    [[nodiscard]] const LocalTransform* local_transform(StableId id) const;
    void set_local_transform(StableId id, const LocalTransform& transform);

    /// The composed world transform, valid only after `update_transforms`.
    [[nodiscard]] const WorldTransform* world_transform(StableId id) const;

    [[nodiscard]] const SpriteRenderData* sprite(StableId id) const;
    void set_sprite(StableId id, const SpriteRenderData& sprite);
    void remove_sprite(StableId id);

    [[nodiscard]] const Camera* camera(StableId id) const;
    void set_camera(StableId id, const Camera& camera);
    void remove_camera(StableId id);

    /// What playback has added to this entity, or nullptr when nothing is animating it.
    ///
    /// Written by an animator and by nothing else. It is not part of the authored scene: it is
    /// never serialised, it is not captured or restored by an edit command, and
    /// `update_transforms` composes it on top of the authored transform rather than instead of
    /// it. That separation is what lets an entity be edited while it is moving.
    [[nodiscard]] const AnimationPose* animation_pose(StableId id) const;
    void set_animation_pose(StableId id, const AnimationPose& pose);
    void clear_animation_pose(StableId id);

    /// Every entity carrying a pose, in stable-identifier order.
    [[nodiscard]] std::vector<StableId> animated() const;

    /// The clip this entity plays, or nullptr when it plays none.
    ///
    /// Authored, unlike the pose: edited through the history and saved with the scene.
    [[nodiscard]] const Animator* animator(StableId id) const;
    void set_animator(StableId id, const Animator& animator);
    void remove_animator(StableId id);

    /// Every entity carrying an animator, in stable-identifier order.
    [[nodiscard]] std::vector<StableId> animators() const;

    // --- hierarchy --------------------------------------------------------------------

    /// Attach `child` under `parent`, or detach it when `parent` is None.
    ///
    /// Refuses to create a cycle. A cycle is not merely invalid, it is unwalkable: the
    /// transform update would not terminate, so it is checked at the moment of reparenting
    /// rather than discovered later.
    [[nodiscard]] Status set_parent(StableId child, StableId parent);

    [[nodiscard]] StableId parent(StableId id) const;
    [[nodiscard]] std::span<const StableId> children(StableId id) const;

    /// Entities with no parent, ordered by identifier.
    [[nodiscard]] std::vector<StableId> roots() const;

    /// Recompute every world transform, parents before children.
    ///
    /// Called once after a batch of changes rather than on each one: composing a child's
    /// transform needs its parent's to be current, so doing it eagerly would either
    /// recompute subtrees repeatedly or produce stale results.
    void update_transforms();

    // --- iteration --------------------------------------------------------------------

    /// Every entity, ordered by identifier.
    [[nodiscard]] std::vector<EntityView> entities() const;

    /// Every entity with a sprite, in draw order: by layer, then by identifier.
    ///
    /// Ties broken by identifier rather than by creation order, so that what overlaps what
    /// is the same after a save and reload.
    [[nodiscard]] std::vector<StableId> drawable() const;

    /// The active camera, if one is set.
    [[nodiscard]] std::optional<StableId> active_camera() const;

    /// Depth-first walk from the roots, parents before children, siblings by identifier.
    void visit_depth_first(const std::function<void(StableId, std::size_t depth)>& visitor) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace atlas::scene
