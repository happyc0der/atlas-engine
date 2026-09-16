// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/app/run_bounds.hpp>

namespace atlas::app {

Status validate_headless_bound(bool headless, std::uint64_t max_frames, std::uint64_t max_ticks) {
    if (headless && max_frames == 0 && max_ticks == 0) {
        return std::unexpected(
            Error(ErrorCode::InvalidArgument,
                  "a headless run has no way to be asked to quit; pass --frames or --ticks to "
                  "bound it"));
    }
    return ok();
}

}  // namespace atlas::app
