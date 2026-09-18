// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Which sources have reported for which tick, and therefore which ticks may run.
///
/// Deterministic lockstep is one idea: a tick runs only when every participant has said what it
/// is doing on that tick. Every peer then applies the same commands in the same order and
/// reaches the same state **without exchanging any state at all**. This is the bookkeeping for
/// that one idea and nothing else. It holds no commands, no transport and no peers — a
/// `SourceId` here is the same index the command queue and the replay already use — and it is
/// in `simulation` rather than in a networking module because nothing about it is networking.
/// See [ADR-0014](../../../../../docs/adr/0014-deterministic-lockstep.md).
///
/// **Readiness depends only on which sources have reported. It never depends on time.** This
/// class reads no clock, holds no deadline, and has no timeout. Every answer below is a pure
/// function of the marks it has been given.
///
/// That is the invariant the whole milestone rests on. The moment readiness could turn on
/// elapsed time, two machines running at different frame rates would run different ticks with
/// different commands, which is exactly the failure that stamping a command with its target
/// tick was introduced to prevent. Deciding what to do about a peer that has stopped reporting
/// is a policy for whoever owns the transport — drop it from the expectation set, or end the
/// session — and that decision reaches the simulation only as a call to `expect_sources`, never
/// as a timer in here. A timeout is the obvious thing to reach for, which is why its absence is
/// written down rather than left to be noticed.
///
/// **An empty expectation set is always ready.** A solo run therefore takes exactly the path it
/// took before M14: every `ready` is true, `ready_horizon` is unbounded, and no tick is ever
/// refused. That is what keeps the golden hashes identical, and it is asserted rather than
/// assumed.
///
/// **Not saved.** A gate is session state, not world state: two peers resuming from a save
/// renegotiate their expectation set from whatever brought them together. Putting it in the
/// save file would change the save's bytes and prove nothing about the world.
///
/// Thread affinity: the main thread, with the command queue and the kernel. A mark arriving on
/// another thread is handed across before being recorded, exactly as a command is.

#include <atlas/core/result.hpp>
#include <atlas/core/time.hpp>
#include <atlas/simulation/command.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace atlas::sim {

/// Which turns each expected source has completed.
class TurnGate {
  public:
    /// Most sources one gate will track.
    ///
    /// An expectation set is assembled from whatever put a session together, so it is untrusted
    /// input like any other. The bound is also what makes `footprint_bytes` a bound at all:
    /// without it, a list of a million sources is a million records.
    static constexpr std::size_t kMaxSources = 256;

    /// How far past a source's own frontier a mark may reach.
    ///
    /// A lockstep turn delay is a handful of ticks, enough to cover a round trip. This is far
    /// past any legitimate delay, and it is the line at which a mark stops being early and
    /// starts being a claim. Refusing beyond it is what stops a peer announcing that it has
    /// completed tick four billion — which would either need unbounded storage, or, far worse,
    /// be taken to mean every tick in between was complete too.
    static constexpr std::uint32_t kLookahead = 256;

    /// What `ready_horizon` reports when nothing is expected: every tick is ready, so there is
    /// no first unready one.
    ///
    /// A sentinel rather than an optional so that arithmetic on the horizon needs no special
    /// case — `ready_horizon() - current_tick()` is always a safe subtraction.
    static constexpr Tick kUnboundedHorizon = std::numeric_limits<Tick>::max();

    TurnGate() = default;

    /// Declare which sources must report before a tick may run.
    ///
    /// Replaces the set. A source in both the old set and the new **keeps its progress**,
    /// because one peer joining must not un-complete the turns the others already finished; a
    /// source only in the old set is forgotten; a source only in the new starts at the current
    /// floor, which is the first tick it could have an opinion about.
    ///
    /// An empty span expects nobody and makes every tick ready. That is the solo configuration
    /// and it is the default.
    ///
    /// Failure: `InvalidArgument` for a repeated identifier — a duplicate means whatever
    /// assembled the list is confused about who is playing, and collapsing it silently would
    /// hide that while making the gate wait for one peer twice. `OutOfRange` past `kMaxSources`.
    [[nodiscard]] Status expect_sources(std::span<const SourceId> sources);

    /// Every expected source, in ascending identifier order.
    ///
    /// Ordered rather than merely enumerated, so that a message naming what a tick is waiting on
    /// reads identically on every machine. Two peers comparing their stall logs are comparing
    /// text, and text assembled from an unordered container is not comparable.
    [[nodiscard]] std::span<const SourceId> expected_sources() const noexcept;

    [[nodiscard]] bool expects(SourceId source) const noexcept;

    /// Record that `source` has submitted everything it will submit for `tick`.
    ///
    /// **Idempotent.** Marking the same tick twice is not an error and changes nothing. A lossy
    /// transport retransmits, and a retransmission that produced an error would turn one peer's
    /// ordinary retry into a log full of failures on every other peer.
    ///
    /// **Order-tolerant.** Marks for ticks 9, 7 and 8 leave the gate exactly as marks for 7, 8
    /// and 9 would. Messages arrive in whatever order the network chose, and requiring ascending
    /// marks would make readiness depend on arrival order — the one thing this file exists to
    /// rule out.
    ///
    /// Failures, both of which mean a peer is misbehaving rather than merely slow:
    ///   * `InvalidArgument` — `source` is not expected. A source nobody agreed to cannot
    ///     complete a turn on everybody's behalf.
    ///   * `OutOfRange` — `tick` is `kLookahead` or more past this source's own frontier.
    ///
    /// A mark below the retired floor is accepted and does nothing: it is a duplicate of a turn
    /// that has already been run.
    [[nodiscard]] Status mark_complete(SourceId source, Tick tick);

    /// Whether every expected source has completed `tick`.
    ///
    /// True when nothing is expected, and true for a tick below the floor, which has already
    /// run. A pure function of the marks: asking twice with no mark in between gives the same
    /// answer however long the wait was.
    [[nodiscard]] bool ready(Tick tick) const noexcept;

    /// The first tick that is **not** ready, so everything in `[floor(), ready_horizon())` may
    /// run.
    ///
    /// Half-open on purpose: a caller asking how many ticks it may run wants
    /// `ready_horizon() - current_tick()` with no adjustment, and an inclusive bound is where
    /// that calculation grows an off-by-one that only appears when a peer is exactly one turn
    /// ahead.
    [[nodiscard]] Tick ready_horizon() const noexcept;

    /// Append the expected sources that have not completed `tick`, in ascending order.
    ///
    /// Clears `out` first. Takes a buffer rather than returning one because this names the
    /// sources in a refused tick's message, and a stall is asked about once a frame for as long
    /// as it lasts; allocating a vector every frame inside the tick loop to report that nothing
    /// happened is the wrong shape.
    void waiting_on(Tick tick, std::vector<SourceId>& out) const;

    /// Forget everything below `tick`.
    ///
    /// **This reclaims nothing**, and the doc comment says so rather than implying a bound it
    /// does not provide: the representation is a frontier plus a fixed window per source, so it
    /// never grew with the tick count in the first place. What it does is move the line below
    /// which a mark is a duplicate rather than a mark, and give `ready` a defined answer for a
    /// tick from the distant past.
    ///
    /// Asserts `tick <= ready_horizon()`. Retiring a tick no source has completed would be the
    /// caller claiming to have run a tick this gate never permitted — a bug in the caller rather
    /// than bad data, which is what separates an assertion from an error (ADR-0005).
    void retire_before(Tick tick) noexcept;

    /// The first tick this gate still reasons about.
    [[nodiscard]] Tick floor() const noexcept { return m_floor; }

    /// Bytes the containers hold, for the test that proves this does not grow.
    ///
    /// The bound is `kMaxSources` records, and it is independent of the tick count, of how many
    /// marks have been recorded, and of how long a peer has been silent. Exposed because that is
    /// a claim, and a claim with no way to check it is a claim that stops being true during the
    /// refactor after next.
    [[nodiscard]] std::size_t footprint_bytes() const noexcept;

  private:
    /// One source's progress: the first tick it has not completed, plus a fixed window of marks
    /// that arrived ahead of that.
    ///
    /// Bit `i` of `ahead` is the tick `complete_before + i`. **Bit 0 is always clear**, because
    /// a set bit 0 is immediately absorbed by advancing the frontier — which is what makes a run
    /// of consecutive marks cost one shift each rather than a search.
    ///
    /// This is the whole memory story. A source that never marks keeps one small record for
    /// ever, and the mark that would need a larger window is the one `kLookahead` refuses.
    struct Source {
        SourceId id = SourceId::Local;
        Tick complete_before = 0;
        std::array<std::uint64_t, kLookahead / 64> ahead{};
    };

    /// Sorted by identifier, never longer than `kMaxSources`.
    std::vector<Source> m_expected;
    /// The same identifiers, so `expected_sources` can hand out a span without building one.
    std::vector<SourceId> m_ids;
    Tick m_floor = 0;
};

/// The right to mark one source's turns, and no other's.
///
/// A `CommandSource` is handed this rather than the gate itself. With a `TurnGate&` the rule
/// "mark only your own turn" could only be written in a comment — and in M15 the thing on the
/// other side of that comment is a sandboxed mod. Two pointers' worth of state, no allocation,
/// no virtual call, and the rule becomes unrepresentable instead of documented.
class SourceGate {
  public:
    SourceGate(TurnGate& gate, SourceId source) noexcept : m_gate(&gate), m_source(source) {}

    /// Failure: whatever `TurnGate::mark_complete` reports for this source.
    [[nodiscard]] Status mark_complete(Tick tick) { return m_gate->mark_complete(m_source, tick); }

    [[nodiscard]] SourceId source() const noexcept { return m_source; }

  private:
    TurnGate* m_gate;
    SourceId m_source;
};

}  // namespace atlas::sim
