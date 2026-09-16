// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Turning command-line logging options into a configured logger, and tearing it down.
///
/// Thread affinity: main thread, at startup and shutdown.

#include <atlas/core/log.hpp>
#include <atlas/core/result.hpp>

#include <string_view>

namespace atlas::app {

/// A severity by its command-line name.
///
/// Failure: InvalidArgument naming the unknown level and listing the valid ones.
[[nodiscard]] Result<log::Severity> parse_severity(std::string_view name);

/// Set the minimum severity, attach the console sink, and optionally a file sink.
///
/// Failure: whatever parse_severity or the file sink reports, with "configuring logging"
/// as context.
[[nodiscard]] Status configure_logging(std::string_view level, std::string_view log_file);

/// Tears down the process-wide logging configuration when the run ends.
///
/// Declared first in a composition root so it is destroyed last, after everything that
/// might still log during its own teardown.
class LogSession {
  public:
    LogSession() = default;
    LogSession(const LogSession&) = delete;
    LogSession& operator=(const LogSession&) = delete;
    LogSession(LogSession&&) = delete;
    LogSession& operator=(LogSession&&) = delete;

    ~LogSession() { log::shutdown(); }
};

}  // namespace atlas::app
