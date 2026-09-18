// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/simulation/divergence.hpp>

#include <algorithm>
#include <format>

namespace atlas::sim {

Divergence attribute_divergence(Tick tick, std::uint64_t expected_state_hash,
                                std::uint64_t actual_state_hash,
                                std::span<const SystemHash> expected,
                                std::span<const SystemHash> actual, const Schedule& schedule) {
    Divergence divergence;
    divergence.tick = tick;
    divergence.expected_hash = expected_state_hash;
    divergence.actual_hash = actual_state_hash;

    // By identifier rather than by position. Two runs of one schedule should produce the same
    // order, and a run that did not is a difference worth naming rather than a reason to give
    // up comparing.
    for (const SystemHash& reference : expected) {
        const auto at = std::ranges::find(actual, reference.system, &SystemHash::system);
        if (at == actual.end() || at->hash != reference.hash) {
            divergence.first_system = reference.system;
            break;
        }
    }

    const System* system =
        divergence.first_system.has_value() ? schedule.find(*divergence.first_system) : nullptr;

    // Three cases, and the third is the one the code this replaced got wrong: it reported "no
    // per-system hashes were recorded" whenever no system could be blamed, including when
    // hashes were recorded and every one of them matched. That is a different situation with a
    // different cause, and telling somebody the hashes are missing while they are looking at
    // them is worse than saying nothing.
    std::string attribution;
    if (system != nullptr) {
        attribution = std::format("the first system whose writes differ is '{}'", system->name);
    } else if (divergence.first_system.has_value()) {
        // A system was blamed but the schedule does not know it — the two runs are not running
        // the same systems, which is a bigger problem than a divergence.
        attribution =
            std::format("system {} differs and is not in this schedule, so the two runs are not "
                        "running the same systems",
                        static_cast<std::uint32_t>(*divergence.first_system));
    } else if (expected.empty() || actual.empty()) {
        attribution = "no per-system hashes were recorded, so it cannot be attributed to a system";
    } else {
        attribution = std::format(
            "all {} systems that recorded a hash agree, so the difference is in a table no "
            "system declares that it writes",
            expected.size());
    }

    divergence.description =
        std::format("diverged at tick {}: expected state {:#018x}, got {:#018x}; {}",
                    divergence.tick, divergence.expected_hash, divergence.actual_hash, attribution);
    return divergence;
}

}  // namespace atlas::sim
