// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// The fixed scenario's recorded hashes, as constants rather than as literals in a test.
///
/// These two numbers are the engine's determinism claim reduced to something comparable. They
/// have been measured identical on arm64, x86_64 and MSVC since M7, and `docs/DETERMINISM.md`
/// records them beside the hash algorithm version that produced them.
///
/// **Why they are here and not only in `test_golden.cpp`, where they lived until M14.** A
/// lockstep handshake refuses a peer whose build cannot be compared with this one, and the
/// cheapest honest probe of that is to exchange these values: two builds that produce the same
/// golden hashes agree about the simulation, whatever else differs between them. The charter
/// permits a session between different builds only on that condition. A module cannot include a
/// test, so a constant a test owns is a constant nothing else can use.
///
/// **Changing either of these is a decision, not a refactor.** They change when the hash
/// algorithm changes, and then only after checking that the *state* is still bit-identical and
/// only the function reducing it to a number moved — which is what M8 did when
/// `kHashAlgorithmVersion` became 2, and what the comment at the assertion records.

#include <cstdint>

namespace atlas::sim {

/// The scenario: seed `0x0A71A50000000001`, 64 rows, three systems, 500 ticks.
inline constexpr std::uint64_t kGoldenSeed = 0x0A71'A5'0000'0001ULL;
inline constexpr std::size_t kGoldenRows = 64;
inline constexpr std::uint64_t kGoldenTicks = 500;

/// The state hash after the last tick.
inline constexpr std::uint64_t kGoldenFinalState = 0xAA82'430D'E232'1AFFULL;

/// Every tick's hash folded together, so a divergence in the middle that converges again is
/// still caught.
inline constexpr std::uint64_t kGoldenAllTicks = 0x2603'546C'5687'E95EULL;

}  // namespace atlas::sim
