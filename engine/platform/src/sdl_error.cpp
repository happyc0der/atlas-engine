// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdl_error.hpp"

#include <SDL3/SDL_error.h>

#include <format>
#include <string_view>

namespace atlas::platform::detail {

Error sdl_error(ErrorCode code, std::string_view what) {
    const char* message = SDL_GetError();
    const std::string_view detail =
        (message != nullptr) ? std::string_view{message} : std::string_view{};

    // Leaving the message in place would let an unrelated later failure pick it up and
    // report something that did not happen.
    SDL_ClearError();

    if (detail.empty()) {
        return {code, std::string{what}};
    }
    return {code, std::format("{}: {}", what, detail)};
}

}  // namespace atlas::platform::detail
