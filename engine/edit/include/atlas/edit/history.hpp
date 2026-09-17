// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/// \file
/// The undo and redo stack, and the only thing permitted to write to an editable scene.
///
/// A panel is handed a `History`, never a mutable `Scene`. There is no method here that
/// returns one, so a widget physically cannot reach past the validation an `edit::Command`
/// performs — the same compiler-enforced arrangement that kept the scene panel read-only from
/// M5 until now, rather than a rule anybody has to remember.
///
/// **Linear.** One stack back, one forward; a new command clears the forward one. Branching
/// histories are a real design with real interface questions, and nothing has asked for one.
///
/// Ownership: holds a reference to a scene that must outlive it, and owns every command it has
/// applied. Thread affinity: main thread.

#include <atlas/core/error.hpp>
#include <atlas/core/result.hpp>
#include <atlas/edit/command.hpp>
#include <atlas/scene/scene.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string_view>

namespace atlas::edit {

/// Whether an applied command may fold into the one before it.
///
/// Decided by the caller rather than by a timer, because the caller is the only one who knows
/// whether this value came from the same interaction as the last. Merging on elapsed time
/// would make undo granularity depend on how fast the machine ran, so the same drag would be
/// one step on a slow machine and several on a fast one.
enum class Coalesce : std::uint8_t {
    /// Its own undo step.
    No,
    /// Fold into the previous command when that command accepts the merge and the group is
    /// still open. A widget passes this while its control is being dragged, and calls
    /// `break_coalescing` when the drag ends.
    WithPrevious,
};

class History {
  public:
    /// Commands kept before the oldest is dropped.
    ///
    /// Bounded because a `Destroy` holds a whole subtree and an editing session is long. An
    /// unbounded history is a memory leak with a user interface.
    static constexpr std::size_t kDefaultCapacity = 256;

    /// `scene` must outlive this history, and nothing else may write to it.
    ///
    /// That second half is a contract with the composition root rather than something this
    /// class can check: it holds a reference, not ownership. An application that animates the
    /// scene directly — as the sandbox's demo does — must say so and must not animate anything
    /// the user can edit at the same time.
    explicit History(scene::Scene& scene, std::size_t capacity = kDefaultCapacity) noexcept;

    History(const History&) = delete;
    History& operator=(const History&) = delete;
    History(History&&) noexcept = default;
    History& operator=(History&&) noexcept = default;
    ~History() = default;

    /// Read access, for panels and for anything that draws.
    ///
    /// Deliberately no mutable overload. This is the whole reason a panel can be given a
    /// history and still be unable to bypass validation.
    [[nodiscard]] const scene::Scene& scene() const noexcept { return *m_scene; }

    /// Apply a command now, and keep it so it can be undone.
    ///
    /// On success the command is pushed and the redo stack is cleared — or, with
    /// `Coalesce::WithPrevious` and a previous command that accepts the merge, the previous
    /// command absorbs this one and nothing is pushed.
    ///
    /// Failure: the scene, both stacks and the revision are unchanged and the command is
    /// destroyed. A refused edit leaves no trace, which is what lets a widget call this every
    /// frame without polluting the history.
    [[nodiscard]] Status apply(std::unique_ptr<Command> command, Coalesce coalesce = Coalesce::No);

    /// Undo the most recent command. False when there is nothing to undo.
    ///
    /// False is not an error: pressing undo on an empty history is an ordinary thing to do.
    /// False also means a failed revert, which is not ordinary — see `last_error`.
    [[nodiscard]] bool undo();

    /// Redo the most recently undone command. False when there is nothing to redo.
    [[nodiscard]] bool redo();

    [[nodiscard]] bool can_undo() const noexcept { return !m_undo.empty(); }

    [[nodiscard]] bool can_redo() const noexcept { return !m_redo.empty(); }

    /// What undo or redo would do, for a menu item or a tooltip. Empty when it would do
    /// nothing.
    [[nodiscard]] std::string_view undo_label() const noexcept;
    [[nodiscard]] std::string_view redo_label() const noexcept;

    [[nodiscard]] std::size_t undo_depth() const noexcept { return m_undo.size(); }

    [[nodiscard]] std::size_t redo_depth() const noexcept { return m_redo.size(); }

    /// Incremented by every successful apply, undo and redo.
    ///
    /// What an owner watches to know the scene changed, without being told what changed:
    /// recompose world transforms, mark the document modified, republish. A counter rather
    /// than a callback, because a callback would run inside a command and could edit the scene
    /// from there.
    [[nodiscard]] std::uint64_t revision() const noexcept { return m_revision; }

    /// End the current coalescing group without applying anything.
    ///
    /// For a widget that reports the end of an interaction separately from its last value.
    void break_coalescing() noexcept { m_coalescing_open = false; }

    /// Forget everything. The scene is not touched.
    void clear() noexcept;

    /// The failure from the last revert that could not be performed, if there was one.
    ///
    /// Set only when `undo` or `redo` failed to restore a command's own before-image, which
    /// means something wrote to the scene outside this history. The history clears itself when
    /// that happens: one that cannot reproduce the scene has no business offering to.
    [[nodiscard]] const std::optional<Error>& last_error() const noexcept { return m_last_error; }

  private:
    scene::Scene* m_scene;
    std::size_t m_capacity;
    std::deque<std::unique_ptr<Command>> m_undo;
    std::deque<std::unique_ptr<Command>> m_redo;
    bool m_coalescing_open = false;
    std::uint64_t m_revision = 0;
    std::optional<Error> m_last_error;
};

}  // namespace atlas::edit
