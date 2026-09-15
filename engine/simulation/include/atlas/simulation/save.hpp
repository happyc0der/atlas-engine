// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Writing a simulation out and reading it back.
///
/// A save holds everything needed to resume: the tick, the seed, the command sequence
/// numbers, and every table's contents. It does not hold anything derived, because a file
/// that stored both a value and something computed from it could contradict itself.
///
/// **A save file is untrusted input.** It may come from a mod, a download, or a colleague,
/// and the charter says to treat all three as hostile. Every length is checked before it is
/// used, every table is matched against one the world actually has, and a file is refused
/// rather than partly believed.
///
/// **A failed load changes nothing.** The state is read into place only after every table has
/// been accepted. A half-loaded world would be a state no author reasoned about, and every
/// reader would have to handle it.
///
/// **The format names and versions itself.** A file from a newer build is refused rather than
/// read, because reading it would silently drop whatever that build added and the first sign
/// would be data disappearing on the next save.
///
/// The format itself is recorded in docs/adr/0008-numeric-and-save-policy.md.

#include <atlas/core/result.hpp>
#include <atlas/simulation/command.hpp>
#include <atlas/simulation/kernel.hpp>
#include <atlas/simulation/world.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace atlas::sim {

/// Identifies the file. Chosen to be recognisable in a hex dump and invalid as text.
inline constexpr std::uint64_t kSaveMagic = 0x53'4C'54'41'56'41'53'00ULL;  // "\0SAVATLS"

/// Bumped whenever the layout changes. A file from a later version is refused.
inline constexpr std::uint32_t kSaveFormatVersion = 1;

/// What a save records besides the tables.
struct SaveHeader {
    std::uint32_t format_version = kSaveFormatVersion;

    /// The hash algorithm the stored hash was produced with, so a change of algorithm is
    /// detected rather than mistaken for a change of content.
    std::uint32_t hash_algorithm_version = kHashAlgorithmVersion;

    Tick tick = 0;
    std::uint64_t seed = 0;

    /// The state hash at the moment of saving, checked on load.
    std::uint64_t state_hash = 0;
};

/// Serialise the world and the kernel's position into bytes.
[[nodiscard]] Result<std::vector<std::byte>> save(const World& world, const Kernel& kernel,
                                                  const CommandQueue& commands);

/// Read a save back into an existing world.
///
/// The world must already hold the same tables, by name, that the file was written from. The
/// kernel's tick and seed and the queue's sequence numbers are restored too, so the
/// simulation resumes rather than restarts.
///
/// The tables are matched by identifier, so a file written when the tables were registered in
/// a different order still reads. A table in the file that the world does not have is an
/// error, and so is a table in the world that the file does not mention: either means the
/// save and the build disagree about what the state is.
[[nodiscard]] Status load(World& world, Kernel& kernel, CommandQueue& commands,
                          std::span<const std::byte> bytes);

/// Read only the header, to inspect a file without loading it.
[[nodiscard]] Result<SaveHeader> read_header(std::span<const std::byte> bytes);

}  // namespace atlas::sim
