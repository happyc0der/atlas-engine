// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Result type used by every fallible Atlas API.
///
/// Result<T> is std::expected<T, Error>: the standard type is used directly rather than a
/// first-party equivalent. See docs/adr/0005-error-and-ownership-model.md.
///
/// Usage:
/// \code
///   Result<Device> create_device(const DeviceDesc& desc);
///
///   auto device = create_device(desc);
///   if (!device) {
///       return std::unexpected(std::move(device).error().context("starting the renderer"));
///   }
/// \endcode

#include <atlas/core/error.hpp>

#include <expected>
#include <utility>

namespace atlas {

/// The result of a fallible operation: a value, or an Error explaining why there is none.
template <typename T> using Result = std::expected<T, Error>;

/// The result of a fallible operation that produces no value.
using Status = Result<void>;

/// A successful Status. Spelled out because `return {};` reads as ambiguous at call sites.
[[nodiscard]] inline Status ok() noexcept {
    return Status{};
}

/// Build a failed Result<T> without naming the type twice.
template <typename T = void, typename... Args> [[nodiscard]] Result<T> fail(Args&&... args) {
    return std::unexpected(Error(std::forward<Args>(args)...));
}

/// Propagate a failure from a Result of a different value type, adding context.
///
/// Useful when forwarding a failure across an API boundary where the value type changes:
/// \code
///   auto bytes = read_file(path);
///   if (!bytes) {
///       return forward_error<Texture>(std::move(bytes), "importing a texture");
///   }
/// \endcode
template <typename To, typename From>
[[nodiscard]] Result<To> forward_error(Result<From>&& source, std::string_view what) {
    return std::unexpected(std::move(source).error().context(what));
}

}  // namespace atlas
