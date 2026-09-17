// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/tools/panels.hpp>

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
