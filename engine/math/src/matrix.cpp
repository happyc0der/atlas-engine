// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/math/matrix.hpp>

#include <format>
#include <string>

namespace atlas::math {

std::string Mat4::to_string() const {
    std::string text;
    text.reserve(160);
    for (std::size_t row = 0; row < 4; ++row) {
        text += std::format("[{:9.4f} {:9.4f} {:9.4f} {:9.4f}]\n", at(row, 0), at(row, 1),
                            at(row, 2), at(row, 3));
    }
    return text;
}

}  // namespace atlas::math
