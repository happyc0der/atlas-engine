// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/edit/command.hpp>

#include <format>
#include <utility>

namespace atlas::edit {
namespace {

/// Every command begins here.
///
/// `Scene`'s component setters return void and do nothing for an entity that is not there, so
/// without this a command built from a selection the user has since deleted would report
/// success, enter the history, and undo into nothing.
[[nodiscard]] Status require_present(const scene::Scene& scene, scene::StableId id,
                                     std::string_view what) {
    if (!scene.contains(id)) {
        return fail(ErrorCode::NotFound,
                    std::format("cannot {} entity {}: it does not exist in this scene", what,
                                static_cast<std::uint64_t>(id)));
    }
    return ok();
}

}  // namespace

bool Command::merge(const Command& later) {
    (void)later;
    return false;
}

// --- Rename ---------------------------------------------------------------------------------

Rename::Rename(scene::StableId id, std::string name) : m_id(id), m_after(std::move(name)) {}

Status Rename::apply(scene::Scene& scene) {
    if (auto present = require_present(scene, m_id, "rename"); !present) {
        return present;
    }
    if (!m_captured) {
        m_before = std::string{scene.name(m_id)};
        m_captured = true;
    }
    scene.set_name(m_id, m_after);
    return ok();
}

Status Rename::revert(scene::Scene& scene) {
    if (auto present = require_present(scene, m_id, "restore the name of"); !present) {
        return present;
    }
    scene.set_name(m_id, m_before);
    return ok();
}

bool Rename::merge(const Command& later) {
    const auto* other = dynamic_cast<const Rename*>(&later);
    if (other == nullptr || other->m_id != m_id) {
        return false;
    }
    m_after = other->m_after;
    return true;
}

// --- SetLocalTransform ----------------------------------------------------------------------

SetLocalTransform::SetLocalTransform(scene::StableId id, const scene::LocalTransform& transform)
    : m_id(id), m_after(transform) {}

Status SetLocalTransform::apply(scene::Scene& scene) {
    if (auto present = require_present(scene, m_id, "move"); !present) {
        return present;
    }
    if (!m_captured) {
        const auto* current = scene.local_transform(m_id);
        // An entity always has a transform; the pointer is null only for one that does not
        // exist, which the check above has already ruled out.
        m_before = current != nullptr ? *current : scene::LocalTransform{};
        m_captured = true;
    }
    scene.set_local_transform(m_id, m_after);
    return ok();
}

Status SetLocalTransform::revert(scene::Scene& scene) {
    if (auto present = require_present(scene, m_id, "restore the transform of"); !present) {
        return present;
    }
    scene.set_local_transform(m_id, m_before);
    return ok();
}

bool SetLocalTransform::merge(const Command& later) {
    const auto* other = dynamic_cast<const SetLocalTransform*>(&later);
    if (other == nullptr || other->m_id != m_id) {
        return false;
    }
    // The later value replaces ours; the before-image stays the one captured at the start of
    // the drag, which is what one undo must restore.
    m_after = other->m_after;
    return true;
}

// --- SetSprite ------------------------------------------------------------------------------

SetSprite::SetSprite(scene::StableId id, const scene::SpriteRenderData& sprite)
    : m_id(id), m_after(sprite) {}

Status SetSprite::apply(scene::Scene& scene) {
    if (auto present = require_present(scene, m_id, "set the sprite of"); !present) {
        return present;
    }
    if (!m_captured) {
        const auto* current = scene.sprite(m_id);
        m_before = current != nullptr ? std::optional{*current} : std::nullopt;
        m_captured = true;
    }
    scene.set_sprite(m_id, m_after);
    return ok();
}

Status SetSprite::revert(scene::Scene& scene) {
    if (auto present = require_present(scene, m_id, "restore the sprite of"); !present) {
        return present;
    }
    if (m_before.has_value()) {
        scene.set_sprite(m_id, *m_before);
    } else {
        scene.remove_sprite(m_id);
    }
    return ok();
}

// --- RemoveSprite ---------------------------------------------------------------------------

RemoveSprite::RemoveSprite(scene::StableId id) : m_id(id) {}

Status RemoveSprite::apply(scene::Scene& scene) {
    if (auto present = require_present(scene, m_id, "remove the sprite of"); !present) {
        return present;
    }
    if (!m_captured) {
        const auto* current = scene.sprite(m_id);
        if (current == nullptr) {
            // Refused rather than recorded as a no-op: an undo of this would have to invent a
            // sprite the entity never had.
            return fail(ErrorCode::NotFound, std::format("entity {} has no sprite to remove",
                                                         static_cast<std::uint64_t>(m_id)));
        }
        m_before = *current;
        m_captured = true;
    }
    scene.remove_sprite(m_id);
    return ok();
}

Status RemoveSprite::revert(scene::Scene& scene) {
    if (auto present = require_present(scene, m_id, "restore the sprite of"); !present) {
        return present;
    }
    scene.set_sprite(m_id, m_before);
    return ok();
}

// --- SetAnimator ---------------------------------------------------------------------------

SetAnimator::SetAnimator(scene::StableId id, const scene::Animator& animator)
    : m_id(id), m_after(animator) {}

Status SetAnimator::apply(scene::Scene& scene) {
    if (auto present = require_present(scene, m_id, "set the animator of"); !present) {
        return present;
    }
    if (!m_captured) {
        const auto* current = scene.animator(m_id);
        m_before = current != nullptr ? std::optional{*current} : std::nullopt;
        m_captured = true;
    }
    scene.set_animator(m_id, m_after);
    return ok();
}

Status SetAnimator::revert(scene::Scene& scene) {
    if (auto present = require_present(scene, m_id, "restore the animator of"); !present) {
        return present;
    }
    if (m_before.has_value()) {
        scene.set_animator(m_id, *m_before);
    } else {
        scene.remove_animator(m_id);
    }
    return ok();
}

bool SetAnimator::merge(const Command& later) {
    const auto* other = dynamic_cast<const SetAnimator*>(&later);
    if (other == nullptr || other->m_id != m_id) {
        return false;
    }
    // The later value replaces ours; the before-image stays the one captured at the start of
    // the drag, which is what one undo must restore.
    m_after = other->m_after;
    return true;
}

// --- RemoveAnimator ------------------------------------------------------------------------

RemoveAnimator::RemoveAnimator(scene::StableId id) : m_id(id) {}

Status RemoveAnimator::apply(scene::Scene& scene) {
    if (auto present = require_present(scene, m_id, "remove the animator of"); !present) {
        return present;
    }
    if (!m_captured) {
        const auto* current = scene.animator(m_id);
        if (current == nullptr) {
            // Refused rather than recorded as a no-op: an undo of this would have to invent an
            // animator the entity never had.
            return fail(ErrorCode::NotFound, std::format("entity {} has no animator to remove",
                                                         static_cast<std::uint64_t>(m_id)));
        }
        m_before = *current;
        m_captured = true;
    }
    scene.remove_animator(m_id);
    return ok();
}

Status RemoveAnimator::revert(scene::Scene& scene) {
    if (auto present = require_present(scene, m_id, "restore the animator of"); !present) {
        return present;
    }
    scene.set_animator(m_id, m_before);
    return ok();
}

// --- SetCamera ------------------------------------------------------------------------------

SetCamera::SetCamera(scene::StableId id, const scene::Camera& camera) : m_id(id), m_after(camera) {}

Status SetCamera::apply(scene::Scene& scene) {
    if (auto present = require_present(scene, m_id, "set the camera of"); !present) {
        return present;
    }
    if (!m_captured) {
        const auto* current = scene.camera(m_id);
        m_before = current != nullptr ? std::optional{*current} : std::nullopt;
        m_captured = true;
    }
    scene.set_camera(m_id, m_after);
    return ok();
}

Status SetCamera::revert(scene::Scene& scene) {
    if (auto present = require_present(scene, m_id, "restore the camera of"); !present) {
        return present;
    }
    if (m_before.has_value()) {
        scene.set_camera(m_id, *m_before);
    } else {
        scene.remove_camera(m_id);
    }
    return ok();
}

// --- RemoveCamera ---------------------------------------------------------------------------

RemoveCamera::RemoveCamera(scene::StableId id) : m_id(id) {}

Status RemoveCamera::apply(scene::Scene& scene) {
    if (auto present = require_present(scene, m_id, "remove the camera of"); !present) {
        return present;
    }
    if (!m_captured) {
        const auto* current = scene.camera(m_id);
        if (current == nullptr) {
            return fail(ErrorCode::NotFound, std::format("entity {} has no camera to remove",
                                                         static_cast<std::uint64_t>(m_id)));
        }
        m_before = *current;
        m_captured = true;
    }
    scene.remove_camera(m_id);
    return ok();
}

Status RemoveCamera::revert(scene::Scene& scene) {
    if (auto present = require_present(scene, m_id, "restore the camera of"); !present) {
        return present;
    }
    scene.set_camera(m_id, m_before);
    return ok();
}

// --- Reparent -------------------------------------------------------------------------------

Reparent::Reparent(scene::StableId child, scene::StableId parent)
    : m_child(child), m_after(parent) {}

Status Reparent::apply(scene::Scene& scene) {
    if (auto present = require_present(scene, m_child, "reparent"); !present) {
        return present;
    }
    // Captured before the call, because set_parent is what might refuse. It checks existence
    // and cycles before detaching anything, so a refusal leaves the scene untouched and this
    // command never reaches the history.
    const scene::StableId previous = scene.parent(m_child);
    if (auto status = scene.set_parent(m_child, m_after); !status) {
        return status;
    }
    if (!m_captured) {
        m_before = previous;
        m_captured = true;
    }
    return ok();
}

Status Reparent::revert(scene::Scene& scene) {
    if (auto present = require_present(scene, m_child, "restore the parent of"); !present) {
        return present;
    }
    return scene.set_parent(m_child, m_before);
}

// --- Create ---------------------------------------------------------------------------------

Create::Create(std::string name, scene::StableId parent)
    : m_name(std::move(name)), m_parent(parent) {}

Status Create::apply(scene::Scene& scene) {
    if (scene::valid(m_parent)) {
        if (auto present = require_present(scene, m_parent, "create a child of"); !present) {
            return present;
        }
    }

    if (!scene::valid(m_id)) {
        m_id = scene.create(m_name);
    } else {
        // A redo. The same identifier, not a new one: anything still holding it — a selection,
        // a later command in this history — must mean this entity again.
        auto recreated = scene.create_with_id(m_id, m_name);
        if (!recreated) {
            return std::unexpected(recreated.error());
        }
    }

    if (scene::valid(m_parent)) {
        return scene.set_parent(m_id, m_parent);
    }
    return ok();
}

Status Create::revert(scene::Scene& scene) {
    if (auto present = require_present(scene, m_id, "remove the created"); !present) {
        return present;
    }
    // Destroy cascades. An entity created by this command has no children unless something
    // added them outside the history, and a history that is not the only writer has already
    // lost the ability to reproduce the scene.
    if (!scene.destroy(m_id)) {
        return fail(ErrorCode::NotFound, std::format("could not destroy created entity {}",
                                                     static_cast<std::uint64_t>(m_id)));
    }
    return ok();
}

// --- Destroy --------------------------------------------------------------------------------

Destroy::Destroy(scene::StableId id) : m_id(id) {}

Status Destroy::apply(scene::Scene& scene) {
    if (auto present = require_present(scene, m_id, "destroy"); !present) {
        return present;
    }

    if (m_subtree.empty()) {
        // Breadth-first from the root of the subtree, so a parent is always recorded before
        // its children and can be recreated before them. Children come from Scene::children,
        // which is ordered by identifier, so the capture order is canonical rather than
        // dependent on how the scene was assembled.
        std::vector<scene::StableId> pending{m_id};
        for (std::size_t index = 0; index < pending.size(); ++index) {
            const scene::StableId id = pending[index];
            Record record;
            record.id = id;
            record.parent = scene.parent(id);
            record.name = std::string{scene.name(id)};
            if (const auto* transform = scene.local_transform(id); transform != nullptr) {
                record.transform = *transform;
            }
            if (const auto* sprite = scene.sprite(id); sprite != nullptr) {
                record.sprite = *sprite;
            }
            if (const auto* camera = scene.camera(id); camera != nullptr) {
                record.camera = *camera;
            }
            if (const auto* animator = scene.animator(id); animator != nullptr) {
                record.animator = *animator;
            }
            m_subtree.push_back(std::move(record));

            const auto children = scene.children(id);
            pending.insert(pending.end(), children.begin(), children.end());
        }
    }

    if (!scene.destroy(m_id)) {
        return fail(ErrorCode::NotFound,
                    std::format("could not destroy entity {}", static_cast<std::uint64_t>(m_id)));
    }
    return ok();
}

Status Destroy::revert(scene::Scene& scene) {
    for (const Record& record : m_subtree) {
        auto recreated = scene.create_with_id(record.id, record.name);
        if (!recreated) {
            return fail(ErrorCode::AlreadyExists,
                        std::format("cannot restore entity {}: {}. Something other than the "
                                    "history has written to this scene",
                                    static_cast<std::uint64_t>(record.id),
                                    recreated.error().message()));
        }

        scene.set_local_transform(record.id, record.transform);
        if (record.sprite.has_value()) {
            scene.set_sprite(record.id, *record.sprite);
        }
        if (record.camera.has_value()) {
            scene.set_camera(record.id, *record.camera);
        }
        if (record.animator.has_value()) {
            scene.set_animator(record.id, *record.animator);
        }

        if (scene::valid(record.parent)) {
            // The parent is either outside the subtree and still present, or inside it and
            // already restored, because parents precede children in the capture.
            if (auto status = scene.set_parent(record.id, record.parent); !status) {
                return status;
            }
        }
    }
    return ok();
}

}  // namespace atlas::edit
