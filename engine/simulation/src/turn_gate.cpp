// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/assert.hpp>
#include <atlas/simulation/turn_gate.hpp>

#include <algorithm>
#include <format>

namespace atlas::sim {
namespace {

/// Where a bit lives in the lookahead window.
struct BitPosition {
    std::size_t word;
    std::uint64_t mask;
};

[[nodiscard]] constexpr BitPosition bit_at(std::uint32_t offset) noexcept {
    return BitPosition{
        .word = offset / 64U,
        .mask = std::uint64_t{1} << (offset % 64U),
    };
}

}  // namespace

Status TurnGate::expect_sources(std::span<const SourceId> sources) {
    if (sources.size() > kMaxSources) {
        return std::unexpected(Error(ErrorCode::OutOfRange,
                                     std::format("a session of {} sources is past the limit of {}",
                                                 sources.size(), kMaxSources)));
    }

    std::vector<SourceId> wanted(sources.begin(), sources.end());
    std::ranges::sort(wanted);
    // Refused rather than collapsed. A repeated identifier means whatever assembled this list
    // is confused about who is playing, and quietly deduplicating would hide that while making
    // the gate wait for one peer twice.
    if (std::ranges::adjacent_find(wanted) != wanted.end()) {
        return std::unexpected(Error(ErrorCode::InvalidArgument,
                                     "the same source appears twice in the expectation set"));
    }

    std::vector<Source> next;
    next.reserve(wanted.size());
    for (const SourceId id : wanted) {
        // A source already expected keeps everything it has completed. One peer joining must
        // not un-complete the turns the peers already agreed had finished.
        const auto existing = std::ranges::find(m_expected, id, &Source::id);
        if (existing != m_expected.end()) {
            next.push_back(*existing);
            continue;
        }
        // A source arriving now starts at the floor, which is the first tick it could have an
        // opinion about; everything below it has already run.
        next.push_back(Source{.id = id, .complete_before = m_floor, .ahead = {}});
    }

    m_expected = std::move(next);
    m_ids = std::move(wanted);
    return {};
}

std::span<const SourceId> TurnGate::expected_sources() const noexcept {
    return m_ids;
}

bool TurnGate::expects(SourceId source) const noexcept {
    return std::ranges::binary_search(m_ids, source);
}

Status TurnGate::mark_complete(SourceId source, Tick tick) {
    const auto at = std::ranges::lower_bound(m_expected, source, {}, &Source::id);
    if (at == m_expected.end() || at->id != source) {
        return std::unexpected(Error(
            ErrorCode::InvalidArgument,
            std::format("source {} is not expected in this session, so it cannot complete a turn",
                        static_cast<std::uint32_t>(source))));
    }

    // Already run. A duplicate of a turn from before the floor, which is ordinary on a
    // transport that retransmits.
    if (tick < m_floor) {
        return {};
    }
    // Already recorded. Idempotent rather than an error, for the same reason.
    if (tick < at->complete_before) {
        return {};
    }

    // Unsigned, and safe: the comparison above establishes that `tick` is not below the
    // frontier, so this cannot wrap at any tick value.
    const std::uint64_t distance = tick - at->complete_before;
    if (distance >= kLookahead) {
        return std::unexpected(Error(
            ErrorCode::OutOfRange,
            std::format("source {} claims to have completed tick {} while its first incomplete "
                        "turn is {}, which is {} ahead of a limit of {}",
                        static_cast<std::uint32_t>(source), tick, at->complete_before, distance,
                        kLookahead)));
    }

    const auto position = bit_at(static_cast<std::uint32_t>(distance));
    at->ahead[position.word] |= position.mask;

    // Absorb every consecutive completed turn into the frontier, so that bit 0 is clear again
    // and the window always describes turns strictly ahead of it. A run of marks arriving in
    // order costs one shift each; a run arriving backwards costs one shift at the end.
    while ((at->ahead[0] & 1U) != 0) {
        ++at->complete_before;
        for (std::size_t word = 0; word + 1 < at->ahead.size(); ++word) {
            at->ahead[word] = (at->ahead[word] >> 1U) | (at->ahead[word + 1] << 63U);
        }
        at->ahead.back() >>= 1U;
    }

    return {};
}

bool TurnGate::ready(Tick tick) const noexcept {
    // Written out rather than left to fall out of the loop below, which would also answer true
    // for an empty set because `all_of` over nothing is true. The branch is for the reader: the
    // solo guarantee is the reason the golden hashes are unaffected by any of this, and a
    // property that important should be visible at the top of the function rather than implied
    // by a standard-library convention. Removing it changes no answer, and a mutation that does
    // so survives for exactly that reason.
    if (m_expected.empty()) {
        return true;
    }
    if (tick < m_floor) {
        return true;
    }
    return std::ranges::all_of(
        m_expected, [tick](const Source& source) { return source.complete_before > tick; });
}

Tick TurnGate::ready_horizon() const noexcept {
    // Explicit for the same reason, and equally not load-bearing: the fold below starts at the
    // unbounded horizon and never lowers it when there is nothing to lower it with.
    if (m_expected.empty()) {
        return kUnboundedHorizon;
    }
    Tick horizon = kUnboundedHorizon;
    for (const Source& source : m_expected) {
        horizon = std::min(horizon, source.complete_before);
    }
    return horizon;
}

void TurnGate::waiting_on(Tick tick, std::vector<SourceId>& out) const {
    out.clear();
    for (const Source& source : m_expected) {
        if (source.complete_before <= tick) {
            out.push_back(source.id);
        }
    }
}

void TurnGate::retire_before(Tick tick) noexcept {
    ATLAS_ASSERT_MSG(tick <= ready_horizon(),
                     "retiring a tick no source has completed: the caller is claiming to have "
                     "run a tick this gate never permitted");
    m_floor = std::max(m_floor, tick);
}

std::size_t TurnGate::footprint_bytes() const noexcept {
    return (m_expected.capacity() * sizeof(Source)) + (m_ids.capacity() * sizeof(SourceId));
}

}  // namespace atlas::sim
