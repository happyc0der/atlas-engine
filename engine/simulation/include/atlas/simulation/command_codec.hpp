// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// One encoder and one decoder for a `Command`, so that a recording and a peer cannot disagree
/// about what a command is.
///
/// Lifted out of `replay.cpp`'s anonymous namespace in M14, unchanged. It had been private to
/// that translation unit since M6, which was correct while the replay was the only thing that
/// wrote a command down. A lockstep turn is the second, and a second encoder written for the
/// wire would be a second opinion — one that agrees today and drifts on the first field anybody
/// adds.
///
/// **The layout is fixed by the replay format**, whose version it shares:
///
///     u64 target | u32 source | u64 sequence | u32 type | u64 payload length | payload
///
/// Thirty-two bytes plus the payload, little-endian. `tests/integration/lab_checks.py` unpacks
/// it independently to tamper with a recording, so changing it here means changing that too and
/// bumping `kReplayFormatVersion` — this is a file format, not an implementation detail.
///
/// **The payload is bytes the codec does not interpret.** What they mean is decided by the
/// handler registered for `type`, which is what makes this a tagged union that a peer, a
/// recording and a mod can all put their own commands through.
///
/// Thread affinity: none. These are pure functions over a buffer.

#include <atlas/core/result.hpp>
#include <atlas/simulation/command.hpp>
#include <atlas/simulation/save_stream.hpp>

#include <cstddef>

namespace atlas::sim {

/// Smallest a command can encode to: target, source, sequence, type, and a payload length.
inline constexpr std::size_t kCommandOverhead = 8 + 4 + 8 + 4 + 8;

/// Append one command to a buffer.
///
/// Cannot fail, and deliberately does not check the payload against `CommandQueue::kMaxPayload`
/// — a `Command` that reached here came through `submit` or `submit_stamped`, both of which
/// already refuse an oversized payload. A caller assembling one by hand is the case that needs
/// checking, and it is the caller that knows what limit applies to it.
void write_command(SaveWriter& writer, const Command& command);

/// Read one command back.
///
/// The payload length goes through `read_count` against `CommandQueue::kMaxPayload`, so a
/// document claiming a four-gigabyte payload is refused before anything is reserved for it
/// rather than after.
///
/// Failure: whatever the reader reports, with the field that failed named.
[[nodiscard]] Result<Command> read_command(SaveReader& reader);

}  // namespace atlas::sim
