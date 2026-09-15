// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/assert.hpp>
#include <atlas/core/log.hpp>

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace atlas::detail {

void assertion_failed(std::string_view expression, std::string_view message,
                      std::source_location where) {
    // Write to stderr directly as well as through the log. An assertion means the program
    // is in an unknown state, and the logging path may be part of what is broken.
    if (message.empty()) {
        std::fprintf(stderr, "\nAtlas assertion failed: %.*s\n  at %s:%u in %s\n\n",
                     static_cast<int>(expression.size()), expression.data(), where.file_name(),
                     where.line(), where.function_name());
    } else {
        std::fprintf(stderr, "\nAtlas assertion failed: %.*s\n  %.*s\n  at %s:%u in %s\n\n",
                     static_cast<int>(expression.size()), expression.data(),
                     static_cast<int>(message.size()), message.data(), where.file_name(),
                     where.line(), where.function_name());
    }
    std::fflush(stderr);

    if (message.empty()) {
        ATLAS_LOG(log::category::kCore, log::Severity::Fatal, "assertion failed: {}", expression);
    } else {
        ATLAS_LOG(log::category::kCore, log::Severity::Fatal, "assertion failed: {} ({})",
                  expression, message);
    }

    // abort() rather than exit(): a core dump is the most useful artifact here, and no
    // destructor should run while the program's invariants are known to be violated.
    std::abort();
}

}  // namespace atlas::detail
