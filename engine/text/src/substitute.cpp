// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/text/substitute.hpp>

#include <cstddef>

namespace atlas::text {
namespace {

/// The most digits an index may have, which is also the overflow guard.
///
/// A pattern is untrusted, so `{99999999999999999999}` must not be parsed into anything. Nine
/// digits cannot overflow a `std::size_t` on any platform this builds for, and a tenth digit
/// makes the run a literal rather than a placeholder — which is the same outcome an
/// out-of-range index gets, so the rule stays one rule.
constexpr std::size_t kMaxIndexDigits = 9;

[[nodiscard]] constexpr bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

/// Read a placeholder starting at `open`, where `pattern[open]` is known to be `{`.
///
/// Returns whether one is there at all, and if so the index it names and the position just
/// past its closing brace. Anything that is not exactly `{` digits `}` is not a placeholder.
struct Placeholder {
    bool found = false;
    std::size_t index = 0;
    std::size_t end = 0;
};

[[nodiscard]] constexpr Placeholder read_placeholder(std::string_view pattern,
                                                     std::size_t open) noexcept {
    std::size_t cursor = open + 1;
    std::size_t index = 0;
    std::size_t digits = 0;

    while (cursor < pattern.size() && is_digit(pattern[cursor])) {
        if (digits == kMaxIndexDigits) {
            return {};
        }
        index = (index * 10) + static_cast<std::size_t>(pattern[cursor] - '0');
        ++digits;
        ++cursor;
    }

    // No digits is `{}` or `{a`; no closing brace is an unclosed run. Both are literal text.
    if (digits == 0 || cursor >= pattern.size() || pattern[cursor] != '}') {
        return {};
    }
    return {.found = true, .index = index, .end = cursor + 1};
}

}  // namespace

std::string substitute(std::string_view pattern, std::span<const std::string_view> args) {
    std::string out;

    // One allocation for the common case. The true size is bounded by the pattern plus every
    // argument, since each argument is used at most as many times as it appears; reserving the
    // sum of one use each is right for every pattern that mentions an index once, which is all
    // of them so far, and merely a good start for one that does not.
    std::size_t reserve = pattern.size();
    for (const std::string_view arg : args) {
        reserve += arg.size();
    }
    out.reserve(reserve);

    std::size_t cursor = 0;
    while (cursor < pattern.size()) {
        if (pattern[cursor] != '{') {
            out.push_back(pattern[cursor]);
            ++cursor;
            continue;
        }

        const Placeholder placeholder = read_placeholder(pattern, cursor);

        // An index naming an argument that was not supplied is copied through, exactly as a
        // malformed one is. The two are the same kind of mistake from a translator's side, and
        // seeing `{3}` in the interface says which index was wrong.
        if (!placeholder.found || placeholder.index >= args.size()) {
            out.push_back('{');
            ++cursor;
            continue;
        }

        // Appended, never rescanned: an argument that is itself a looked-up string containing
        // `{0}` must appear literally rather than expand again.
        out.append(args[placeholder.index]);
        cursor = placeholder.end;
    }

    return out;
}

}  // namespace atlas::text
