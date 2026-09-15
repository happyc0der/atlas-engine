// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/assert.hpp>
#include <atlas/core/log.hpp>
#include <atlas/core/profile.hpp>
#include <atlas/scene/scene.hpp>

#include <entt/entity/registry.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <unordered_map>
#include <utility>

namespace atlas::scene {
namespace {

constexpr log::Category kScene{"scene"};

/// How deep the hierarchy may go.
///
/// A bound rather than a belief. Serialised scenes are untrusted input, and a deeply nested
/// one would otherwise recurse until the stack ran out; a limit turns that into an error
/// with a message.
constexpr std::size_t kMaxDepth = 256;

}  // namespace

struct Scene::Impl {
    entt::registry registry;

    /// Stable identifier to the library's own handle. The only place the two meet.
    std::unordered_map<StableId, entt::entity> by_id;

    /// Next identifier to hand out. Monotonic, never reused, so a stale reference to a
    /// destroyed entity cannot start referring to a new one.
    std::uint64_t next_id = 1;

    [[nodiscard]] entt::entity lookup(StableId id) const {
        const auto found = by_id.find(id);
        return found == by_id.end() ? entt::null : found->second;
    }

    /// Detach a child from whatever currently holds it.
    void detach(StableId child) {
        const entt::entity handle = lookup(child);
        if (handle == entt::null) {
            return;
        }
        auto* hierarchy = registry.try_get<Hierarchy>(handle);
        if (hierarchy == nullptr || hierarchy->parent == StableId::None) {
            return;
        }

        const entt::entity parent_handle = lookup(hierarchy->parent);
        if (parent_handle != entt::null) {
            if (auto* parent_hierarchy = registry.try_get<Hierarchy>(parent_handle)) {
                std::erase(parent_hierarchy->children, child);
            }
        }
        hierarchy->parent = StableId::None;
    }

    /// Whether `candidate` is `node` or lies beneath it.
    ///
    /// The cycle check. Walking downwards from the proposed parent is bounded by the tree
    /// that already exists, which is known to be acyclic, so this terminates.
    [[nodiscard]] bool is_self_or_descendant(StableId node, StableId candidate) const {
        if (node == candidate) {
            return true;
        }
        const entt::entity handle = lookup(node);
        if (handle == entt::null) {
            return false;
        }
        const auto* hierarchy = registry.try_get<Hierarchy>(handle);
        if (hierarchy == nullptr) {
            return false;
        }
        return std::ranges::any_of(hierarchy->children, [this, candidate](const StableId child) {
            return is_self_or_descendant(child, candidate);
        });
    }

    void destroy_recursive(StableId id) {
        const entt::entity handle = lookup(id);
        if (handle == entt::null) {
            return;
        }

        // Copied, because destroying a child mutates the parent's list.
        std::vector<StableId> children;
        if (const auto* hierarchy = registry.try_get<Hierarchy>(handle)) {
            children = hierarchy->children;
        }
        for (const StableId child : children) {
            destroy_recursive(child);
        }

        detach(id);
        registry.destroy(handle);
        by_id.erase(id);
    }

    void compose(StableId id, const math::Mat4& parent_matrix, std::size_t depth) {
        if (depth > kMaxDepth) {
            ATLAS_LOG_ERROR(kScene,
                            "hierarchy deeper than {} below entity {}; stopping to avoid "
                            "running out of stack",
                            kMaxDepth, static_cast<std::uint64_t>(id));
            return;
        }

        const entt::entity handle = lookup(id);
        if (handle == entt::null) {
            return;
        }

        math::Mat4 world = parent_matrix;
        if (const auto* local = registry.try_get<LocalTransform>(handle)) {
            // Translate, then rotate, then scale, applied to a point in that reverse order:
            // scale about the entity's own origin, rotate about it, then move. Any other
            // order makes a scaled child rotate about the wrong point.
            const float cosine = std::cos(local->rotation);
            const float sine = std::sin(local->rotation);

            math::Mat4 local_matrix;
            local_matrix.set(0, 0, cosine * local->scale.x);
            local_matrix.set(0, 1, -sine * local->scale.y);
            local_matrix.set(1, 0, sine * local->scale.x);
            local_matrix.set(1, 1, cosine * local->scale.y);
            local_matrix.set(0, 3, local->position.x);
            local_matrix.set(1, 3, local->position.y);

            world = parent_matrix * local_matrix;
        }

        registry.emplace_or_replace<WorldTransform>(handle, WorldTransform{.matrix = world});

        if (const auto* hierarchy = registry.try_get<Hierarchy>(handle)) {
            // Copied: composing a child may add a WorldTransform, which can move the
            // library's storage and invalidate the pointer this loop is reading from.
            const std::vector<StableId> children = hierarchy->children;
            for (const StableId child : children) {
                compose(child, world, depth + 1);
            }
        }
    }
};

Scene::Scene() : m_impl(std::make_unique<Impl>()) {}

Scene::~Scene() = default;
Scene::Scene(Scene&& other) noexcept = default;
Scene& Scene::operator=(Scene&& other) noexcept = default;

StableId Scene::create(std::string_view name) {
    ATLAS_ASSERT_MAIN_THREAD();

    const auto id = static_cast<StableId>(m_impl->next_id++);
    const entt::entity handle = m_impl->registry.create();
    m_impl->by_id.emplace(id, handle);

    m_impl->registry.emplace<Name>(handle, Name{.value = std::string{name}});
    m_impl->registry.emplace<LocalTransform>(handle, LocalTransform{});
    m_impl->registry.emplace<Hierarchy>(handle, Hierarchy{});
    return id;
}

Result<StableId> Scene::create_with_id(StableId id, std::string_view name) {
    ATLAS_ASSERT_MAIN_THREAD();

    if (!valid(id)) {
        return std::unexpected(Error(ErrorCode::InvalidArgument,
                                     "zero is reserved for 'no entity' and cannot name one"));
    }
    if (m_impl->by_id.contains(id)) {
        return std::unexpected(
            Error(ErrorCode::AlreadyExists,
                  std::format("entity {} already exists; renumbering it silently would break every "
                              "reference to it",
                              static_cast<std::uint64_t>(id))));
    }

    const entt::entity handle = m_impl->registry.create();
    m_impl->by_id.emplace(id, handle);
    m_impl->registry.emplace<Name>(handle, Name{.value = std::string{name}});
    m_impl->registry.emplace<LocalTransform>(handle, LocalTransform{});
    m_impl->registry.emplace<Hierarchy>(handle, Hierarchy{});

    // Keep handing out identifiers above anything loaded, so a later create cannot collide.
    m_impl->next_id = std::max(m_impl->next_id, static_cast<std::uint64_t>(id) + 1);
    return id;
}

bool Scene::destroy(StableId id) {
    ATLAS_ASSERT_MAIN_THREAD();
    if (!m_impl->by_id.contains(id)) {
        return false;
    }
    m_impl->destroy_recursive(id);
    return true;
}

bool Scene::contains(StableId id) const noexcept {
    return m_impl->by_id.contains(id);
}

std::size_t Scene::size() const noexcept {
    return m_impl->by_id.size();
}

void Scene::clear() {
    ATLAS_ASSERT_MAIN_THREAD();
    m_impl->registry.clear();
    m_impl->by_id.clear();
    m_impl->next_id = 1;
}

std::string_view Scene::name(StableId id) const {
    const entt::entity handle = m_impl->lookup(id);
    if (handle == entt::null) {
        return {};
    }
    const auto* component = m_impl->registry.try_get<Name>(handle);
    return component != nullptr ? std::string_view{component->value} : std::string_view{};
}

void Scene::set_name(StableId id, std::string_view name) {
    const entt::entity handle = m_impl->lookup(id);
    if (handle != entt::null) {
        m_impl->registry.emplace_or_replace<Name>(handle, Name{.value = std::string{name}});
    }
}

const LocalTransform* Scene::local_transform(StableId id) const {
    const entt::entity handle = m_impl->lookup(id);
    return handle == entt::null ? nullptr : m_impl->registry.try_get<LocalTransform>(handle);
}

void Scene::set_local_transform(StableId id, const LocalTransform& transform) {
    const entt::entity handle = m_impl->lookup(id);
    if (handle != entt::null) {
        m_impl->registry.emplace_or_replace<LocalTransform>(handle, transform);
    }
}

const WorldTransform* Scene::world_transform(StableId id) const {
    const entt::entity handle = m_impl->lookup(id);
    return handle == entt::null ? nullptr : m_impl->registry.try_get<WorldTransform>(handle);
}

const SpriteRenderData* Scene::sprite(StableId id) const {
    const entt::entity handle = m_impl->lookup(id);
    return handle == entt::null ? nullptr : m_impl->registry.try_get<SpriteRenderData>(handle);
}

void Scene::set_sprite(StableId id, const SpriteRenderData& sprite) {
    const entt::entity handle = m_impl->lookup(id);
    if (handle != entt::null) {
        m_impl->registry.emplace_or_replace<SpriteRenderData>(handle, sprite);
    }
}

void Scene::remove_sprite(StableId id) {
    const entt::entity handle = m_impl->lookup(id);
    if (handle != entt::null) {
        m_impl->registry.remove<SpriteRenderData>(handle);
    }
}

const Camera* Scene::camera(StableId id) const {
    const entt::entity handle = m_impl->lookup(id);
    return handle == entt::null ? nullptr : m_impl->registry.try_get<Camera>(handle);
}

void Scene::set_camera(StableId id, const Camera& camera) {
    const entt::entity handle = m_impl->lookup(id);
    if (handle != entt::null) {
        m_impl->registry.emplace_or_replace<Camera>(handle, camera);
    }
}

void Scene::remove_camera(StableId id) {
    const entt::entity handle = m_impl->lookup(id);
    if (handle != entt::null) {
        m_impl->registry.remove<Camera>(handle);
    }
}

Status Scene::set_parent(StableId child, StableId parent) {
    ATLAS_ASSERT_MAIN_THREAD();

    if (!m_impl->by_id.contains(child)) {
        return std::unexpected(
            Error(ErrorCode::NotFound,
                  std::format("entity {} does not exist", static_cast<std::uint64_t>(child))));
    }
    if (parent != StableId::None && !m_impl->by_id.contains(parent)) {
        return std::unexpected(
            Error(ErrorCode::NotFound,
                  std::format("parent {} does not exist", static_cast<std::uint64_t>(parent))));
    }

    // A cycle is not merely invalid, it is unwalkable: the transform update would not
    // terminate. Checked here, at the only place one can be created.
    if (parent != StableId::None && m_impl->is_self_or_descendant(child, parent)) {
        return std::unexpected(Error(
            ErrorCode::InvalidArgument,
            std::format("making {} a child of {} would create a cycle, because {} is already "
                        "beneath {}",
                        static_cast<std::uint64_t>(child), static_cast<std::uint64_t>(parent),
                        static_cast<std::uint64_t>(parent), static_cast<std::uint64_t>(child))));
    }

    m_impl->detach(child);

    if (parent == StableId::None) {
        return ok();
    }

    const entt::entity child_handle = m_impl->lookup(child);
    const entt::entity parent_handle = m_impl->lookup(parent);

    m_impl->registry.get<Hierarchy>(child_handle).parent = parent;

    auto& parent_hierarchy = m_impl->registry.get<Hierarchy>(parent_handle);
    parent_hierarchy.children.push_back(child);

    // Siblings kept ordered by identifier, so that drawing and serialisation do not depend
    // on the order things happened to be attached.
    std::ranges::sort(parent_hierarchy.children);
    return ok();
}

StableId Scene::parent(StableId id) const {
    const entt::entity handle = m_impl->lookup(id);
    if (handle == entt::null) {
        return StableId::None;
    }
    const auto* hierarchy = m_impl->registry.try_get<Hierarchy>(handle);
    return hierarchy != nullptr ? hierarchy->parent : StableId::None;
}

std::span<const StableId> Scene::children(StableId id) const {
    const entt::entity handle = m_impl->lookup(id);
    if (handle == entt::null) {
        return {};
    }
    const auto* hierarchy = m_impl->registry.try_get<Hierarchy>(handle);
    return hierarchy != nullptr ? std::span<const StableId>{hierarchy->children}
                                : std::span<const StableId>{};
}

std::vector<StableId> Scene::roots() const {
    std::vector<StableId> found;
    for (const auto& [id, handle] : m_impl->by_id) {
        const auto* hierarchy = m_impl->registry.try_get<Hierarchy>(handle);
        if (hierarchy == nullptr || hierarchy->parent == StableId::None) {
            found.push_back(id);
        }
    }
    // The map's order is arbitrary; the result is not allowed to be.
    std::ranges::sort(found);
    return found;
}

void Scene::update_transforms() {
    ATLAS_ZONE_NAMED("Scene::update_transforms");
    ATLAS_ASSERT_MAIN_THREAD();

    for (const StableId root : roots()) {
        m_impl->compose(root, math::Mat4::identity(), 0);
    }
}

std::vector<EntityView> Scene::entities() const {
    std::vector<EntityView> found;
    found.reserve(m_impl->by_id.size());

    for (const auto& [id, handle] : m_impl->by_id) {
        const auto* name = m_impl->registry.try_get<Name>(handle);
        const auto* hierarchy = m_impl->registry.try_get<Hierarchy>(handle);
        found.push_back(EntityView{
            .id = id,
            .name = name != nullptr ? std::string_view{name->value} : std::string_view{},
            .parent = hierarchy != nullptr ? hierarchy->parent : StableId::None,
            .child_count = hierarchy != nullptr ? hierarchy->children.size() : 0,
            .has_sprite = m_impl->registry.try_get<SpriteRenderData>(handle) != nullptr,
            .has_camera = m_impl->registry.try_get<Camera>(handle) != nullptr,
        });
    }

    std::ranges::sort(found, [](const EntityView& a, const EntityView& b) { return a.id < b.id; });
    return found;
}

std::vector<StableId> Scene::drawable() const {
    std::vector<std::pair<std::int32_t, StableId>> ordered;

    for (const auto& [id, handle] : m_impl->by_id) {
        const auto* sprite = m_impl->registry.try_get<SpriteRenderData>(handle);
        if (sprite != nullptr && sprite->visible) {
            ordered.emplace_back(sprite->layer, id);
        }
    }

    // Layer first, then identifier. Ties broken by identifier rather than by creation order,
    // so that what overlaps what is the same after a save and a reload.
    std::ranges::sort(ordered);

    std::vector<StableId> found;
    found.reserve(ordered.size());
    for (const auto& [layer, id] : ordered) {
        found.push_back(id);
    }
    return found;
}

std::optional<StableId> Scene::active_camera() const {
    std::optional<StableId> found;
    for (const auto& [id, handle] : m_impl->by_id) {
        const auto* camera = m_impl->registry.try_get<Camera>(handle);
        if (camera == nullptr || !camera->active) {
            continue;
        }

        // Lowest identifier wins, so that two active cameras give a defined answer rather
        // than whichever the map happened to visit first.
        if (!found.has_value() || id < *found) {
            found = id;
        }
    }
    return found;
}

void Scene::visit_depth_first(const std::function<void(StableId, std::size_t)>& visitor) const {
    const auto walk = [this, &visitor](auto&& self, StableId id, std::size_t depth) -> void {
        if (depth > kMaxDepth) {
            return;
        }
        visitor(id, depth);
        for (const StableId child : children(id)) {
            self(self, child, depth + 1);
        }
    };

    for (const StableId root : roots()) {
        walk(walk, root, 0);
    }
}

}  // namespace atlas::scene
