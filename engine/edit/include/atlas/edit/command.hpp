// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/// \file
/// Undoable edits to a scene.
///
/// **Not `sim::Command`.** A simulation command is a stamped payload of bytes: ordered by
/// `(source, sequence)`, applied at a named tick, written into the replay log, and folded into
/// the state hash. Undoing one would not change state, it would rewrite a recording, after
/// which the replay no longer reproduces the run and every later hash differs. Going backwards
/// in a simulation is a load, which already exists (ADR-0008). An edit command is a different
/// thing wearing a similar name: an authoring step outside the tick, applied immediately to
/// presentation state that is neither hashed nor replayed, and reversible. The two share one
/// rule and nothing else — nothing reaches the state except through one of them.
///
/// **A command captures what it needs to reverse itself, the first time it is applied.** Not
/// a snapshot of the scene, which would copy every entity to destroy one leaf; and not a
/// separate inverse object, because the inverse of a destroy needs data only the pre-destroy
/// scene has, so it must be captured at apply time anyway. Letting the command own both
/// directions keeps the capture next to the only code that knows what to capture.
///
/// **Never partly applied.** Each command validates, captures, and then makes exactly one
/// `Scene` call, and every `Scene` mutator is itself all-or-nothing. That is the same
/// invariant the simulation's queue states, for the same reason: half a command is a state no
/// author ever reasoned about. `Destroy::revert` is the one multi-call operation, and its
/// failure mode is described there.
///
/// **Validation is this layer's job, not the scene's.** Most `Scene` setters return `void` and
/// silently do nothing for an entity that does not exist, so a command built from a stale
/// selection would otherwise "succeed" and enter the history with nothing to undo. Every
/// command here checks first and fails with `NotFound`.
///
/// Ownership: a command is owned by exactly one `History`, which is why it is neither copyable
/// nor movable. Thread affinity: main thread, like the scene it edits.

#include <atlas/core/result.hpp>
#include <atlas/scene/components.hpp>
#include <atlas/scene/scene.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace atlas::edit {

/// One reversible change to a scene.
class Command {
  public:
    virtual ~Command() = default;

    Command(const Command&) = delete;
    Command& operator=(const Command&) = delete;
    Command(Command&&) = delete;
    Command& operator=(Command&&) = delete;

    /// Validate, capture the before-image if this is the first application, then mutate once.
    ///
    /// Called again by a redo, which must reproduce the same after-image without disturbing
    /// the before-image captured the first time.
    ///
    /// Failure: nothing in the scene changed, and `History` discards the command.
    [[nodiscard]] virtual Status apply(scene::Scene& scene) = 0;

    /// Restore the before-image captured by `apply`.
    ///
    /// Failure means something wrote to the scene behind the history's back, because a
    /// command's own before-image is always restorable into the scene it came from. `History`
    /// treats it as an invariant violation rather than as an ordinary error.
    [[nodiscard]] virtual Status revert(scene::Scene& scene) = 0;

    /// A catalogue key naming what this command did, such as `edit.command.rename`.
    ///
    /// **A key, not a display string**, since ADR-0016. It was both at once — the overlay put
    /// it in a button, `History` puts it in a log line, and `merge` distinguishes commands by
    /// it — and a string doing all three drifts the moment one of them wants different words.
    /// Now the overlay resolves it through `text::Catalog` and the log prints an identifier,
    /// which is more greppable than prose was.
    ///
    /// `edit` therefore needs no dependency on `atlas::text`: it produces keys and somebody
    /// else decides what they say. Never empty.
    [[nodiscard]] virtual std::string_view label() const noexcept = 0;

    /// Absorb a later command into this one, so a drag becomes one undo step.
    ///
    /// Not `noexcept`: adopting a later command's after-image copies it, and a name is a
    /// string. Allocation failure is fatal by ADR-0005 and would not be caught here, but
    /// claiming `noexcept` over an allocating copy is a lie the analyser rightly objects to.
    ///
    /// Returns false by default, and by default is never asked: `History` only offers a merge
    /// when the caller passed `Coalesce::WithPrevious`. An implementation accepts only when
    /// `later` edits the same property of the same entity, and then adopts `later`'s
    /// after-image while keeping its own before-image.
    [[nodiscard]] virtual bool merge(const Command& later);

  protected:
    Command() = default;
};

/// Set an entity's name.
class Rename final : public Command {
  public:
    Rename(scene::StableId id, std::string name);

    [[nodiscard]] Status apply(scene::Scene& scene) override;
    [[nodiscard]] Status revert(scene::Scene& scene) override;

    [[nodiscard]] std::string_view label() const noexcept override { return "edit.command.rename"; }

    [[nodiscard]] bool merge(const Command& later) override;

  private:
    scene::StableId m_id;
    std::string m_after;
    std::string m_before;
    bool m_captured = false;
};

/// Set an entity's local transform. Merges with a later transform edit of the same entity,
/// which is what makes a drag one undo step rather than one per frame.
class SetLocalTransform final : public Command {
  public:
    SetLocalTransform(scene::StableId id, const scene::LocalTransform& transform);

    [[nodiscard]] Status apply(scene::Scene& scene) override;
    [[nodiscard]] Status revert(scene::Scene& scene) override;

    [[nodiscard]] std::string_view label() const noexcept override { return "edit.command.move"; }

    [[nodiscard]] bool merge(const Command& later) override;

  private:
    scene::StableId m_id;
    scene::LocalTransform m_after;
    scene::LocalTransform m_before;
    bool m_captured = false;
};

/// Add or replace an entity's sprite. Undo removes it again when there was none before.
class SetSprite final : public Command {
  public:
    SetSprite(scene::StableId id, const scene::SpriteRenderData& sprite);

    [[nodiscard]] Status apply(scene::Scene& scene) override;
    [[nodiscard]] Status revert(scene::Scene& scene) override;

    [[nodiscard]] std::string_view label() const noexcept override {
        return "edit.command.set_sprite";
    }

  private:
    scene::StableId m_id;
    scene::SpriteRenderData m_after;
    std::optional<scene::SpriteRenderData> m_before;
    bool m_captured = false;
};

/// Give an entity a clip to play, or change the one it has.
///
/// Merges with a previous `SetAnimator` on the same entity, so scrubbing a start time or
/// dragging a speed is one undo step rather than one per frame of the drag.
class SetAnimator final : public Command {
  public:
    SetAnimator(scene::StableId id, const scene::Animator& animator);

    [[nodiscard]] Status apply(scene::Scene& scene) override;
    [[nodiscard]] Status revert(scene::Scene& scene) override;
    [[nodiscard]] bool merge(const Command& later) override;

    [[nodiscard]] std::string_view label() const noexcept override {
        return "edit.command.set_animator";
    }

  private:
    scene::StableId m_id;
    scene::Animator m_after;
    std::optional<scene::Animator> m_before;
    bool m_captured = false;
};

/// Stop an entity animating. Fails with `NotFound` when it has no animator, rather than
/// recording a no-op whose undo would have to invent one.
class RemoveAnimator final : public Command {
  public:
    explicit RemoveAnimator(scene::StableId id);

    [[nodiscard]] Status apply(scene::Scene& scene) override;
    [[nodiscard]] Status revert(scene::Scene& scene) override;

    [[nodiscard]] std::string_view label() const noexcept override {
        return "edit.command.remove_animator";
    }

  private:
    scene::StableId m_id;
    scene::Animator m_before;
    bool m_captured = false;
};

/// Remove an entity's sprite. Fails with `NotFound` when it has none, rather than recording a
/// command that would undo into a sprite the entity never had.
class RemoveSprite final : public Command {
  public:
    explicit RemoveSprite(scene::StableId id);

    [[nodiscard]] Status apply(scene::Scene& scene) override;
    [[nodiscard]] Status revert(scene::Scene& scene) override;

    [[nodiscard]] std::string_view label() const noexcept override {
        return "edit.command.remove_sprite";
    }

  private:
    scene::StableId m_id;
    scene::SpriteRenderData m_before;
    bool m_captured = false;
};

/// Add or replace an entity's camera.
class SetCamera final : public Command {
  public:
    SetCamera(scene::StableId id, const scene::Camera& camera);

    [[nodiscard]] Status apply(scene::Scene& scene) override;
    [[nodiscard]] Status revert(scene::Scene& scene) override;

    [[nodiscard]] std::string_view label() const noexcept override {
        return "edit.command.set_camera";
    }

  private:
    scene::StableId m_id;
    scene::Camera m_after;
    std::optional<scene::Camera> m_before;
    bool m_captured = false;
};

/// Remove an entity's camera. Fails with `NotFound` when it has none.
class RemoveCamera final : public Command {
  public:
    explicit RemoveCamera(scene::StableId id);

    [[nodiscard]] Status apply(scene::Scene& scene) override;
    [[nodiscard]] Status revert(scene::Scene& scene) override;

    [[nodiscard]] std::string_view label() const noexcept override {
        return "edit.command.remove_camera";
    }

  private:
    scene::StableId m_id;
    scene::Camera m_before;
    bool m_captured = false;
};

/// Move an entity to a new parent, or to the root with `StableId::None`.
///
/// `Scene::set_parent` refuses a cycle and a missing entity before it detaches anything, so a
/// refused reparent leaves nothing to roll back and never enters the history.
class Reparent final : public Command {
  public:
    Reparent(scene::StableId child, scene::StableId parent);

    [[nodiscard]] Status apply(scene::Scene& scene) override;
    [[nodiscard]] Status revert(scene::Scene& scene) override;

    [[nodiscard]] std::string_view label() const noexcept override {
        return "edit.command.reparent";
    }

  private:
    scene::StableId m_child;
    scene::StableId m_after;
    scene::StableId m_before = scene::StableId::None;
    bool m_captured = false;
};

/// Create an entity, optionally under a parent.
///
/// A redo recreates the same identifier rather than allocating a new one, because a selection,
/// a later command in the same history, or anything else holding the id must still mean this
/// entity after an undo and redo.
class Create final : public Command {
  public:
    explicit Create(std::string name, scene::StableId parent = scene::StableId::None);

    [[nodiscard]] Status apply(scene::Scene& scene) override;
    [[nodiscard]] Status revert(scene::Scene& scene) override;

    [[nodiscard]] std::string_view label() const noexcept override { return "edit.command.create"; }

    /// The entity created, or `StableId::None` before the first apply.
    [[nodiscard]] scene::StableId id() const noexcept { return m_id; }

  private:
    std::string m_name;
    scene::StableId m_parent;
    scene::StableId m_id = scene::StableId::None;
};

/// Destroy an entity and everything beneath it.
///
/// `Scene::destroy` cascades to children, so undo must restore a subtree rather than one
/// entity: the same identifiers, the same components, and the same child order. Identifiers
/// are never reused, so recreating them is safe; child order falls out of `set_parent` sorting
/// siblings by identifier, which is the same rule that makes iteration and saved files
/// canonical.
class Destroy final : public Command {
  public:
    explicit Destroy(scene::StableId id);

    [[nodiscard]] Status apply(scene::Scene& scene) override;

    /// Recreate the captured subtree, parents before children.
    ///
    /// The one command whose revert makes more than one `Scene` call. Its precondition is that
    /// none of the captured identifiers exist, which holds whenever the history is the scene's
    /// only writer. If it does not hold, the restoration stops at the first identifier it
    /// cannot recreate and returns `AlreadyExists` naming it; `History` responds by discarding
    /// itself, because a history that cannot reproduce the scene has no business offering to.
    [[nodiscard]] Status revert(scene::Scene& scene) override;

    [[nodiscard]] std::string_view label() const noexcept override {
        return "edit.command.destroy";
    }

  private:
    /// One entity's whole state, in the order it must be restored.
    struct Record {
        scene::StableId id = scene::StableId::None;
        scene::StableId parent = scene::StableId::None;
        std::string name;
        scene::LocalTransform transform;
        std::optional<scene::SpriteRenderData> sprite;
        std::optional<scene::Camera> camera;
        /// The clip it was playing. Absent means it was not animating — and the pose it
        /// happened to be in is deliberately not here, because a pose is what playback made of
        /// an entity rather than anything an undo owes it. Restoring the animator restarts the
        /// clip, which is also what loading a file does.
        std::optional<scene::Animator> animator;
    };

    scene::StableId m_id;
    std::vector<Record> m_subtree;
};

}  // namespace atlas::edit
