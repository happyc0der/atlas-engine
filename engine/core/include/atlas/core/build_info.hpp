// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Identity of this build.
///
/// Every save file, replay, benchmark result, and bug report needs to say which build
/// produced it. These values are generated at configure time from build_info.cpp.in.

#include <string_view>

namespace atlas::build_info {

[[nodiscard]] std::string_view version() noexcept;
[[nodiscard]] std::string_view git_commit() noexcept;
[[nodiscard]] bool git_dirty() noexcept;
[[nodiscard]] std::string_view compiler() noexcept;
[[nodiscard]] std::string_view build_type() noexcept;
[[nodiscard]] std::string_view system() noexcept;
[[nodiscard]] std::string_view configured_at() noexcept;

/// Whether this build has the Tracy client compiled in.
[[nodiscard]] bool profiling_enabled() noexcept;

/// Which sanitizer, if any, this build was compiled with: "none", "address", "thread".
[[nodiscard]] std::string_view sanitizer() noexcept;

/// One-line summary suitable for --version output and log headers.
[[nodiscard]] std::string_view summary() noexcept;

}  // namespace atlas::build_info
