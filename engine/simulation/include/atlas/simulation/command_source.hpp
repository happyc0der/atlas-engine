// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Where commands come from, from the simulation's point of view.
///
/// **In `simulation` and not in a networking module, deliberately.** A network peer, a replay, a
/// local input mapper and a sandboxed mod are the same thing to a tick: something outside it
/// that submits stamped commands and says when its turn is done. If this interface lived in a
/// networking module then a mod would depend on networking, or would grow a second interface
/// meaning the same thing — and two interfaces that mean the same thing drift.
/// [ADR-0014](../../../../../docs/adr/0014-deterministic-lockstep.md) records the choice;
/// [ADR-0009](../../../../../docs/adr/0009-scripting-decision.md) decision 2 is the boundary it
/// implements, which has been in force since M9: anything outside the tick changes state only
/// by submitting to the queue, and never executes during a tick.
///
/// Thread affinity: the main thread. A source with a thread of its own owns that thread and
/// hands work across; `poll` runs where the kernel runs.

#include <atlas/core/result.hpp>
#include <atlas/core/time.hpp>
#include <atlas/simulation/command.hpp>
#include <atlas/simulation/turn_gate.hpp>

#include <cstddef>

namespace atlas::sim {

/// What one poll took in.
struct PollReport {
    /// Accepted by the queue.
    std::size_t commands_submitted = 0;

    /// Refused by the queue: an unregistered type, an oversized payload, a full queue, or a
    /// payload its own validator rejected.
    ///
    /// Counted here rather than returned as an error, because one source sending rubbish must
    /// not stop the other sources being polled.
    std::size_t commands_refused = 0;

    /// Turns handed to the gate during this poll.
    std::size_t turns_marked = 0;

    /// The highest tick any accepted command named, for a driver that wants to see how far
    /// ahead a source is running. Zero when nothing was accepted.
    Tick highest_target = 0;

    /// This source will produce nothing further: a peer disconnected, a replay ran out.
    ///
    /// Reported rather than inferred from silence, and that distinction matters more than it
    /// looks. The gate has no timeout by design, so a source that simply stops reporting stalls
    /// every later tick for ever. This is the driver's cue to take it out of the expectation
    /// set, and it is the only way a gate with no clock can be told that waiting is pointless.
    bool closed = false;
};

/// Something outside the simulation that submits commands and completes turns.
class CommandSource {
  public:
    CommandSource() = default;
    virtual ~CommandSource() = default;
    CommandSource(const CommandSource&) = delete;
    CommandSource& operator=(const CommandSource&) = delete;
    CommandSource(CommandSource&&) = delete;
    CommandSource& operator=(CommandSource&&) = delete;

    /// Stable identity: the same index the command queue and the replay use.
    [[nodiscard]] virtual SourceId id() const noexcept = 0;

    /// Take in whatever is already available, and return.
    ///
    /// **Must not block.** Not on a socket, not on a condition variable, not on a file, not on a
    /// lock another thread holds across a system call. This runs in the tick loop, and a source
    /// that waits for its peer turns one slow participant into a frozen application for
    /// everybody — including the participants who were keeping up, whose own turns are then late
    /// for everybody else. A source with a thread of its own drains a bounded, non-blocking
    /// queue here and does its waiting over there.
    ///
    /// **When it is called must not change the result.** Everything it submits is stamped for a
    /// future tick, so polling once a frame, twice a frame, or on an irregular schedule produces
    /// the same simulation. That is what lets a driver poll on whatever schedule suits it, and
    /// it is why `now` is an input rather than something to compare against a clock.
    ///
    /// May: submit commands stamped for `now` or later; mark its own turns through `turns`; log;
    /// reuse its own buffers.
    ///
    /// Must not: call `drain`, `apply`, `clear` or `register_handler` on the queue; touch the
    /// `World`; step the kernel; submit a command stamped before `now`, which would be counted
    /// late and is this source lying about its own timing; or mark a turn for any source but its
    /// own — `turns` is bound to one identifier, so it cannot.
    ///
    /// Failure is reserved for a source that has broken in a way the driver must act on, such as
    /// a transport that cannot be read at all. **Bad data from the far end is a refused command
    /// in the report, not an error**: refusing to poll because one message was malformed hands
    /// anybody who can send a message a way to stop the session.
    [[nodiscard]] virtual Result<PollReport> poll(Tick now, CommandQueue& queue,
                                                  SourceGate& turns) = 0;
};

}  // namespace atlas::sim
