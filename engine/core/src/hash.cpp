// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/hash.hpp>

#include <array>
#include <string>

namespace atlas {

std::string to_hex(std::uint64_t value) {
    constexpr std::string_view kDigits = "0123456789abcdef";

    std::string text(16, '0');
    for (std::size_t i = 0; i < 16; ++i) {
        // Most significant nibble first, so the text reads in the usual order and sorts the
        // same way the number does.
        const auto shift = static_cast<unsigned>((15 - i) * 4);
        text[i] = kDigits[(value >> shift) & 0xFULL];
    }
    return text;
}

}  // namespace atlas
