// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Translation of SDL failures into Atlas errors.
///
/// SDL reports failure by returning false or null and leaving a message in a thread-local
/// slot. That message is the most useful part of the failure and it is easy to lose, so
/// every SDL call site that can fail goes through here.

#include <atlas/core/error.hpp>

#include <string_view>

namespace atlas::platform::detail {

/// Build an error from `what` plus whatever SDL last reported.
///
/// Clears SDL's error slot, so a later unrelated failure cannot inherit this message.
[[nodiscard]] Error sdl_error(ErrorCode code, std::string_view what);

}  // namespace atlas::platform::detail
