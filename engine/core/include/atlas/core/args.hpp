// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Minimal command-line parsing for Atlas applications.
///
/// Deliberately small: applications need a handful of flags, and a dependency for that
/// would be an abstraction without a call site. It handles `--flag`, `--option value`, and
/// `--option=value`, and rejects anything it does not recognise rather than ignoring it,
/// because a silently ignored typo in a benchmark invocation wastes a run.

#include <atlas/core/result.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace atlas {

class Args {
  public:
    /// Parse argv. Returns an error naming the offending argument on failure.
    [[nodiscard]] static Result<Args> parse(int argc, const char* const* argv);

    /// Whether a boolean flag was present.
    [[nodiscard]] bool has(std::string_view name) const;

    /// Value of an option, or nullopt when absent.
    [[nodiscard]] Result<std::string_view> value(std::string_view name) const;

    /// Value of an option as an unsigned integer, or `fallback` when absent.
    [[nodiscard]] Result<std::uint64_t> value_or(std::string_view name,
                                                 std::uint64_t fallback) const;

    /// Value of an option, or `fallback` when absent.
    [[nodiscard]] std::string_view value_or(std::string_view name, std::string_view fallback) const;

    /// Reject any argument that was not queried. Call after reading every known option, so
    /// that an unrecognised flag is reported rather than silently ignored.
    [[nodiscard]] Status reject_unknown() const;

    /// The program name, as invoked.
    [[nodiscard]] std::string_view program() const noexcept { return m_program; }

  private:
    struct Entry {
        std::string name;
        std::string value;
        bool is_flag = false;
        mutable bool queried = false;
    };

    std::string m_program;
    std::vector<Entry> m_entries;

    [[nodiscard]] const Entry* find(std::string_view name) const;
};

}  // namespace atlas
