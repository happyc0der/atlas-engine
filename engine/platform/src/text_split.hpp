// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/// \file
/// Cutting a committed string into `TextInput` events without splitting a character.
///
/// Private to the platform implementation, but in a header so it can be unit-tested. The same
/// arrangement as `sdl_keymap.hpp`, and for the same reason: this is the part with the
/// interesting behaviour, and the part that cannot be reached through the public interface
/// without a window system delivering real text.
///
/// Testing it here rather than through `SDL_PushEvent` is deliberate. A text event's string is
/// owned by the window-system library, which releases it after delivery; pushing one that
/// points at a caller's buffer is not the safe pattern the key-event tests use. So the logic
/// lives where a test can reach it, and the translation that calls this stays one line.

#include <atlas/platform/event.hpp>

#include <cstddef>
#include <string_view>
#include <vector>

namespace atlas::platform::detail {

/// Whether `byte` continues a multi-byte character rather than starting one.
///
/// UTF-8 continuation bytes are `10xxxxxx`. A cut may not fall on one.
[[nodiscard]] constexpr bool is_utf8_continuation(char byte) noexcept {
    return (static_cast<unsigned char>(byte) & 0xC0U) == 0x80U;
}

/// The largest prefix of `text` that is at most `limit` bytes and ends on a character boundary.
///
/// Backs off from `limit` while the byte at the cut continues a character. That terminates
/// within three steps, because a UTF-8 character is at most four bytes long. Returns zero only
/// for empty input or for a single character longer than `limit`, which cannot happen while
/// `limit` is 63 and a character is at most four bytes; the caller must still not loop forever
/// on a zero, and `append_text_input` does not.
[[nodiscard]] constexpr std::size_t utf8_prefix_length(std::string_view text,
                                                       std::size_t limit) noexcept {
    // Load-bearing, and not only as a shortcut: everything below indexes at `limit`, which is
    // in range only once `text` is known to be longer than it. Weakening this to `<` reads one
    // byte past the end. That is undefined, and it is invisible to both the tests and the
    // address sanitizer here, because a view over a std::string has a readable byte at size().
    // So the guard cannot be defended by a test and is defended by this comment instead.
    if (text.size() <= limit) {
        return text.size();
    }
    std::size_t cut = limit;
    while (cut > 0 && is_utf8_continuation(text[cut])) {
        --cut;
    }
    return cut;
}

/// Append `text` to `events` as one `TextInput` event, or as several if it does not fit.
///
/// Every event is cut on a character boundary, so concatenating their `text()` in order
/// reproduces `text` exactly. Empty input appends nothing.
void append_text_input(std::vector<Event>& events, std::string_view text);

}  // namespace atlas::platform::detail
