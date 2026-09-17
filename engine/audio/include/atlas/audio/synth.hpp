// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Sounds generated in code, so that a caller with no asset pipeline can still make one.
///
/// Three real callers from the day this file appeared, which is the bar the project sets
/// before something general exists at all: the Strategy Lab, which has no asset registry and
/// deliberately does not want one; the mixer's tests, which need a clip and must not need a
/// file; and the mixing benchmark, which must not measure a decoder.
///
/// **Thread affinity: none.** Pure functions returning new buffers.

#include <atlas/audio/mix.hpp>

#include <cstdint>
#include <vector>

namespace atlas::audio {

/// A short sine tone with a fade at each end, mono, at the mix rate.
///
/// The fades are not decoration. A sine that starts and stops at a non-zero sample has a step
/// discontinuity at each end, and a step is heard as a click over the top of the tone — which
/// is precisely the artefact that makes a generated sound recognisable as a bug. Five
/// milliseconds is enough to remove it and short enough to keep the attack.
///
/// `frequency_hz` and `duration_ms` are clamped to something audible and finite rather than
/// refused: this is presentation, and a caller that asked for zero wants silence, not an error.
[[nodiscard]] std::vector<float> sine_blip(float frequency_hz, std::uint32_t duration_ms,
                                           float amplitude = 0.5F, std::uint32_t fade_ms = 5);

}  // namespace atlas::audio
