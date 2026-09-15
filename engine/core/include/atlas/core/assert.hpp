// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Assertions for violated programmer invariants.
///
/// An assertion means a bug: an index out of range, a null that the contract said could
/// not be null, a call from the wrong thread. Continuing past one produces undefined
/// behaviour and an unreadable failure later, so an assertion aborts.
///
/// Assertions are never used for recoverable user or data errors. A missing file, a
/// malformed asset, or a failed device creation returns an Error. In particular, untrusted
/// input is validated with real checks that survive a release build, never with
/// ATLAS_ASSERT. See docs/adr/0005-error-and-ownership-model.md.

#include <source_location>
#include <string_view>

namespace atlas::detail {

/// Reports a failed assertion and aborts. Never returns.
[[noreturn]] void assertion_failed(std::string_view expression, std::string_view message,
                                   std::source_location where);

}  // namespace atlas::detail

/// Check a programmer invariant. Compiled out when NDEBUG is defined.
///
/// The expression must have no side effects that the program depends on; use ATLAS_VERIFY
/// when it does.
#ifdef NDEBUG
#define ATLAS_ASSERT(expr) ((void)0)
#define ATLAS_ASSERT_MSG(expr, message) ((void)0)
#else
#define ATLAS_ASSERT(expr)                                                                         \
    ((expr) ? (void)0                                                                              \
            : ::atlas::detail::assertion_failed(#expr, "", std::source_location::current()))
#define ATLAS_ASSERT_MSG(expr, message)                                                            \
    ((expr)                                                                                        \
         ? (void)0                                                                                 \
         : ::atlas::detail::assertion_failed(#expr, (message), std::source_location::current()))
#endif

/// Check an invariant, evaluating the expression in every build.
///
/// Use when the expression has a side effect the program needs. In a release build the
/// expression still runs; only the check is removed.
#ifdef NDEBUG
#define ATLAS_VERIFY(expr) ((void)(expr))
#else
#define ATLAS_VERIFY(expr) ATLAS_ASSERT(expr)
#endif

/// Mark a branch the programmer believes cannot be taken.
#define ATLAS_UNREACHABLE()                                                                        \
    ::atlas::detail::assertion_failed("unreachable", "reached code marked unreachable",            \
                                      std::source_location::current())
