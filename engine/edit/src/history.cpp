// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/log.hpp>
#include <atlas/edit/history.hpp>

#include <utility>

namespace atlas::edit {
namespace {

constexpr log::Category kEdit = log::category::kApp;

}  // namespace

History::History(scene::Scene& scene, std::size_t capacity) noexcept
    : m_scene(&scene), m_capacity(capacity == 0 ? 1 : capacity) {}

Status History::apply(std::unique_ptr<Command> command, Coalesce coalesce) {
    if (command == nullptr) {
        return fail(ErrorCode::InvalidArgument, "an edit command must not be null");
    }

    if (auto status = command->apply(*m_scene); !status) {
        // The command destroys itself on the way out. Nothing about the history changed, so a
        // widget may call this every frame and a refused edit leaves no trace.
        return status;
    }

    ++m_revision;

    if (coalesce == Coalesce::WithPrevious && m_coalescing_open && !m_undo.empty() &&
        m_undo.back()->merge(*command)) {
        // Absorbed: the previous command now carries this one's after-image and its own
        // before-image, so one undo returns to where the interaction started. The redo stack
        // is already empty, because whatever opened this group cleared it.
        return ok();
    }

    m_redo.clear();
    m_undo.push_back(std::move(command));
    if (m_undo.size() > m_capacity) {
        // Dropping the oldest never affects redo, which holds only commands undone from the
        // other end.
        m_undo.pop_front();
    }

    m_coalescing_open = coalesce == Coalesce::WithPrevious;
    return ok();
}

bool History::undo() {
    if (m_undo.empty()) {
        return false;
    }

    std::unique_ptr<Command> command = std::move(m_undo.back());
    m_undo.pop_back();

    if (auto status = command->revert(*m_scene); !status) {
        ATLAS_LOG_ERROR(kEdit, "cannot undo '{}': {}. Discarding the edit history",
                        command->label(), status.error());
        m_last_error = status.error();
        clear();
        return false;
    }

    m_redo.push_back(std::move(command));
    m_coalescing_open = false;
    ++m_revision;
    return true;
}

bool History::redo() {
    if (m_redo.empty()) {
        return false;
    }

    std::unique_ptr<Command> command = std::move(m_redo.back());
    m_redo.pop_back();

    if (auto status = command->apply(*m_scene); !status) {
        ATLAS_LOG_ERROR(kEdit, "cannot redo '{}': {}. Discarding the edit history",
                        command->label(), status.error());
        m_last_error = status.error();
        clear();
        return false;
    }

    m_undo.push_back(std::move(command));
    m_coalescing_open = false;
    ++m_revision;
    return true;
}

std::string_view History::undo_label() const noexcept {
    return m_undo.empty() ? std::string_view{} : m_undo.back()->label();
}

std::string_view History::redo_label() const noexcept {
    return m_redo.empty() ? std::string_view{} : m_redo.back()->label();
}

void History::clear() noexcept {
    m_undo.clear();
    m_redo.clear();
    m_coalescing_open = false;
}

}  // namespace atlas::edit
