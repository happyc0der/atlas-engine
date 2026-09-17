// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/tools/panels.hpp>

#include <algorithm>
#include <array>

namespace atlas::tools {

bool matches(const log::LogBuffer::Entry& entry, const LogFilter& filter) {
    if (entry.severity < filter.min_severity) {
        return false;
    }
    if (filter.category_substring.empty()) {
        return true;
    }
    return entry.category.contains(filter.category_substring);
}

namespace {

using platform::GamepadButton;
using platform::Key;

/// The bindings, in action order.
///
/// Faster and Slower are new with the gamepad: shoulders are what a person reaches for to
/// step through speeds, and giving them the same meaning on the keyboard costs two keys.
constexpr std::array kBindings = std::to_array<ControlBinding>({
    {ControlAction::TogglePause, Key::Space, GamepadButton::South},
    {ControlAction::SingleStep, Key::Period, GamepadButton::East},
    {ControlAction::SpeedNormal, Key::Num1, GamepadButton::Count},
    {ControlAction::SpeedTimes2, Key::Num2, GamepadButton::Count},
    {ControlAction::SpeedTimes4, Key::Num3, GamepadButton::Count},
    {ControlAction::SpeedTimes8, Key::Num4, GamepadButton::Count},
    {ControlAction::SpeedUnbounded, Key::U, GamepadButton::Count},
    {ControlAction::Faster, Key::Equals, GamepadButton::RightShoulder},
    {ControlAction::Slower, Key::Minus, GamepadButton::LeftShoulder},
    {ControlAction::NextMode, Key::M, GamepadButton::West},
    {ControlAction::ResetView, Key::R, GamepadButton::North},
    {ControlAction::Save, Key::F5, GamepadButton::Count},
    {ControlAction::Load, Key::F9, GamepadButton::Count},
});

/// The speeds Faster and Slower step through, slowest first.
constexpr std::array kSpeedLadder = std::to_array<sim::Speed>({
    sim::Speed::normal(),
    sim::Speed::times(2),
    sim::Speed::times(4),
    sim::Speed::times(8),
    sim::Speed::unbounded(),
});

[[nodiscard]] std::size_t rung_of(sim::Speed speed) noexcept {
    for (std::size_t i = 0; i < kSpeedLadder.size(); ++i) {
        if (kSpeedLadder[i].policy == speed.policy &&
            kSpeedLadder[i].numerator == speed.numerator &&
            kSpeedLadder[i].denominator == speed.denominator) {
            return i;
        }
    }
    // Paused, single-stepping, or a speed nobody can reach from the ladder: stepping up from
    // there means starting at the bottom rather than jumping to wherever a search happened to
    // land.
    return 0;
}

}  // namespace

std::span<const ControlBinding> control_bindings() noexcept {
    return kBindings;
}

std::optional<ControlAction> action_for(platform::Key key) noexcept {
    for (const auto& binding : kBindings) {
        if (binding.key == key) {
            return binding.action;
        }
    }
    return std::nullopt;
}

std::optional<ControlAction> action_for(platform::GamepadButton button) noexcept {
    if (button == platform::GamepadButton::Count) {
        return std::nullopt;
    }
    for (const auto& binding : kBindings) {
        if (binding.button == button) {
            return binding.action;
        }
    }
    return std::nullopt;
}

SimulationControlsRequest request_for(ControlAction action,
                                      const ControlsContext& context) noexcept {
    SimulationControlsRequest request;
    switch (action) {
    case ControlAction::TogglePause:
        request.speed = context.speed.policy == sim::SpeedPolicy::Paused ? sim::Speed::normal()
                                                                         : sim::Speed::paused();
        break;
    case ControlAction::SingleStep: request.single_step = true; break;
    case ControlAction::SpeedNormal: request.speed = sim::Speed::normal(); break;
    case ControlAction::SpeedTimes2: request.speed = sim::Speed::times(2); break;
    case ControlAction::SpeedTimes4: request.speed = sim::Speed::times(4); break;
    case ControlAction::SpeedTimes8: request.speed = sim::Speed::times(8); break;
    case ControlAction::SpeedUnbounded: request.speed = sim::Speed::unbounded(); break;
    case ControlAction::Faster: {
        const std::size_t rung = rung_of(context.speed);
        request.speed = kSpeedLadder[std::min(rung + 1, kSpeedLadder.size() - 1)];
        break;
    }
    case ControlAction::Slower: {
        const std::size_t rung = rung_of(context.speed);
        request.speed = kSpeedLadder[rung == 0 ? 0 : rung - 1];
        break;
    }
    case ControlAction::NextMode:
        if (context.mode_count > 0) {
            request.mode_index = (context.mode_index + 1) % context.mode_count;
        }
        break;
    case ControlAction::ResetView: request.reset_view = true; break;
    case ControlAction::Save: request.save = true; break;
    case ControlAction::Load: request.load = true; break;
    case ControlAction::Count: break;
    }
    return request;
}

std::string_view speed_name(sim::Speed speed) {
    switch (speed.policy) {
    case sim::SpeedPolicy::Paused: return "paused";
    case sim::SpeedPolicy::SingleStep: return "step";
    case sim::SpeedPolicy::Unbounded: return "unbounded";
    case sim::SpeedPolicy::Realtime: break;
    }

    // A fractional speed is realtime with a denominator. Naming it by the numerator alone
    // would report a half-speed run as "1x" on the one row that says how fast time is running.
    if (speed.denominator != 1) {
        return "custom";
    }
    switch (speed.numerator) {
    case 1: return "1x";
    case 2: return "2x";
    case 4: return "4x";
    case 8: return "8x";
    default: return "custom";
    }
}

}  // namespace atlas::tools
