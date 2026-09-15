// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// What an asset is doing.
///
/// Loading is asynchronous, so at any moment an asset is somewhere in a small, explicit
/// state machine rather than simply present or absent. Callers that ask for something not
/// yet ready get a fallback and can carry on; see docs/ARCHITECTURE.md.

#include <cstdint>
#include <string_view>

namespace atlas::assets {

enum class AssetState : std::uint8_t {
    /// Known about, nothing done yet.
    Unloaded,
    /// Waiting for a worker to pick it up.
    Queued,
    /// A worker is reading and decoding it.
    Loading,
    /// Decoded, waiting for the main thread to finish it. A texture's pixels exist but its
    /// graphics resource does not, because only the main thread may create one.
    Decoded,
    /// Usable.
    Ready,
    /// Failed. The recorded error says why, and the asset resolves to a fallback.
    Failed,
};

[[nodiscard]] std::string_view to_string(AssetState state) noexcept;

/// Whether a state can still become Ready without another request.
[[nodiscard]] constexpr bool is_in_progress(AssetState state) noexcept {
    return state == AssetState::Queued || state == AssetState::Loading ||
           state == AssetState::Decoded;
}

/// Whether the state is final until something asks again.
[[nodiscard]] constexpr bool is_settled(AssetState state) noexcept {
    return state == AssetState::Ready || state == AssetState::Failed;
}

}  // namespace atlas::assets
