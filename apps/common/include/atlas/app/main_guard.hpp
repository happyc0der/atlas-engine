// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// The outermost boundary of a process.
///
/// ADR-0005 forbids exceptions crossing module or thread boundaries and treats allocation
/// failure as fatal. This is the one place a catch is permitted, so that such a failure exits
/// with a diagnostic rather than through std::terminate. Every application's main is this
/// template applied to its own run function.

#include <atlas/core/log.hpp>
#include <atlas/core/result.hpp>

#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace atlas::app {

/// Exit code for a graphics device that stopped working underneath the process.
///
/// Distinct from the general failure code so that an operator, or a script running a headless
/// job, can tell "the graphics processor went away" from "the arguments were wrong" without
/// reading the log. Atlas never recovers from device loss; it reports it and stops.
inline constexpr int kExitDeviceLost = 2;

/// Run `run`, report its Status, and turn anything that escapes into an exit code.
///
/// Returns 0 on success and 1 on any failure. `program` is what appears before the message
/// on stderr, so a person running several Atlas binaries can tell which one spoke.
template <typename Run> [[nodiscard]] int guarded_main(std::string_view program, Run&& run) {
    try {
        const Status status = std::forward<Run>(run)();
        if (!status) {
            const std::string message = status.error().to_string();
            std::fprintf(stderr, "%.*s: %s\n", static_cast<int>(program.size()), program.data(),
                         message.c_str());
            ATLAS_LOG_ERROR(log::category::kApp, "exiting with failure: {}", message);
            return status.error().code() == ErrorCode::DeviceLost ? kExitDeviceLost : 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%.*s: unhandled exception: %s\n", static_cast<int>(program.size()),
                     program.data(), error.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "%.*s: unhandled exception of unknown type\n",
                     static_cast<int>(program.size()), program.data());
        return 1;
    }
}

}  // namespace atlas::app
