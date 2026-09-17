// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/// \file
/// Panel inputs and outputs that are worth testing without a graphics device.
///
/// The overlay itself needs a device, so everything in `DebugUi` can only be exercised under
/// the `gpu` label. The decisions those panels make — which log lines a filter admits, what a
/// key press means, whether a request is empty — do not need one, and are here so they can be
/// tested directly.
///
/// **Panels emit intent; the application applies it.** A control panel returns a request and
/// changes nothing itself. That is not ceremony: `tools` cannot depend on an application, so it
/// could not call the lab's functions even if it wanted to, and it keeps the blueprint's rule
/// that a user-interface callback must not reach past validation. The scene panel is the one
/// exception and takes an `edit::History`, because a history *is* the validated path.

#include <atlas/core/log.hpp>
#include <atlas/simulation/tick_accumulator.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace atlas::tools {

/// Which records a log console shows.
struct LogFilter {
    /// Records below this are hidden.
    log::Severity min_severity = log::Severity::Trace;
    /// Case-sensitive substring the category must contain. Empty matches every category.
    std::string category_substring;
};

/// Whether one record passes a filter.
///
/// Pure, and separate from the panel that uses it, because this is the part that can be wrong
/// in a way nobody sees: a filter that silently hides errors looks exactly like a quiet system.
[[nodiscard]] bool matches(const log::LogBuffer::Entry& entry, const LogFilter& filter);

/// What a log console did this frame.
struct LogConsoleReport {
    /// Records displayed, and records held back by the filter.
    std::size_t shown = 0;
    std::size_t hidden = 0;
    /// The user asked for the buffer to be emptied. The panel does not empty it: it holds the
    /// buffer by const reference, and whoever owns it decides.
    bool clear_requested = false;
};

/// What a simulation-controls panel shows. The application fills this in.
struct SimulationControlsView {
    sim::Speed speed;
    Tick tick = 0;
    /// Names of the application's display modes, in order. Empty when it has none.
    std::span<const std::string_view> modes;
    std::size_t mode_index = 0;
    /// Whether the application was given somewhere to save to and load from.
    bool can_save = false;
    bool can_load = false;
    /// Whether applied commands are being recorded, and how many so far. Status only: recording
    /// is chosen when the kernel is built, so that a replay always covers a whole run.
    bool recording = false;
    std::uint64_t recorded_commands = 0;
};

/// What the user asked a simulation-controls panel for.
///
/// Every field is "no request" by default, so a frame in which nothing was clicked produces a
/// request that changes nothing. The application applies this through the same path its
/// keyboard shortcuts use, so a button and a key cannot drift apart.
struct SimulationControlsRequest {
    std::optional<sim::Speed> speed;
    bool single_step = false;
    std::optional<std::size_t> mode_index;
    bool reset_view = false;
    bool save = false;
    bool load = false;

    [[nodiscard]] bool empty() const noexcept {
        return !speed.has_value() && !single_step && !mode_index.has_value() && !reset_view &&
               !save && !load;
    }
};

/// A short name for a speed, for a panel or a status row.
[[nodiscard]] std::string_view speed_name(sim::Speed speed);

}  // namespace atlas::tools
