// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Positional substitution into a pattern that came from a file.
///
/// **This exists because `std::format` cannot be used here and `std::vformat` may not be.** The
/// engine formats with a compile-time checked format string on purpose — `core/log.hpp` takes
/// `std::format_string<Args...>`, which is consteval-constructed from a literal, so a
/// `std::string` read out of a translation file will not convert. The standard escape hatch,
/// `std::vformat`, throws `std::format_error` on a malformed pattern, and ADR-0005 forbids an
/// exception crossing a module boundary. That is enforced rather than asked for: the exception
/// allow-list in `tools/check_module_deps.py` has one entry, and a `try` here would need a
/// second one with a written reason.
///
/// So this is forty lines that cannot throw instead. ADR-0016 records the trade.
///
/// **Positional, not sequential.** `{0}`, `{1}`, and so on, because word order differs between
/// languages and `std::format`'s `{}` in sequence cannot express that. A translator reordering
/// a sentence moves the indices; the call site does not change.

#include <span>
#include <string>
#include <string_view>

namespace atlas::text {

/// Replace `{0}`, `{1}` … in `pattern` with `args`, and never fail.
///
/// **One rule: `{` followed by one or more decimal digits followed by `}` is a placeholder, and
/// every other `{` is a literal `{`.** An unclosed brace, a brace followed by a letter, and an
/// index with no argument are all copied to the output exactly as they appear. A translation
/// file is untrusted input like any other, and the failure that shows a translator their own
/// mistake in the interface is better than the one that stops the frame.
///
/// **There is no escape sequence, so a literal `{0}` cannot be produced.** That is a real
/// limitation and it is written down rather than discovered: adding `{{` would mean a second
/// rule, a second thing to get wrong, and an ambiguity about what `{{0}}` means. Interface
/// strings have not needed one.
///
/// **An argument is never rescanned.** Text that arrives through `args` is appended, not
/// searched for placeholders, so an argument that is itself a looked-up string containing
/// `{0}` appears literally. This is a single pass over `pattern` and nothing else, which is the
/// difference between a substituter and a macro expander.
///
/// Ownership: the result owns its bytes. `pattern` and `args` need outlive only the call.
/// Thread affinity: none; it touches nothing but its arguments.
/// Failure: cannot fail, and cannot throw except by allocation, which ADR-0005 treats as fatal.
[[nodiscard]] std::string substitute(std::string_view pattern,
                                     std::span<const std::string_view> args);

}  // namespace atlas::text
