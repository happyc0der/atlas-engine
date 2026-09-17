// SPDX-License-Identifier: GPL-3.0-or-later
#include "text_split.hpp"

#include <algorithm>

namespace atlas::platform::detail {

void append_text_input(std::vector<Event>& events, std::string_view text) {
    while (!text.empty()) {
        const std::size_t take = utf8_prefix_length(text, TextInput::kCapacity);
        if (take == 0) {
            // A single character longer than the capacity, which no valid UTF-8 produces while
            // the capacity is 63. Taking the whole limit rather than looping forever means a
            // malformed input is truncated and the loop still ends, which is the right failure
            // for something that arrives from outside.
            TextInput event;
            const std::size_t fallback = std::min(text.size(), TextInput::kCapacity);
            std::ranges::copy(text.substr(0, fallback), event.bytes.begin());
            event.length = static_cast<std::uint8_t>(fallback);
            events.emplace_back(event);
            text.remove_prefix(fallback);
            continue;
        }

        TextInput event;
        std::ranges::copy(text.substr(0, take), event.bytes.begin());
        event.length = static_cast<std::uint8_t>(take);
        events.emplace_back(event);
        text.remove_prefix(take);
    }
}

}  // namespace atlas::platform::detail
