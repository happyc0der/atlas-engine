// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/app/log_options.hpp>

#include <format>
#include <string>

namespace atlas::app {

Result<log::Severity> parse_severity(std::string_view name) {
    using log::Severity;
    if (name == "trace") {
        return Severity::Trace;
    }
    if (name == "debug") {
        return Severity::Debug;
    }
    if (name == "info") {
        return Severity::Info;
    }
    if (name == "warning" || name == "warn") {
        return Severity::Warning;
    }
    if (name == "error") {
        return Severity::Error;
    }
    if (name == "fatal") {
        return Severity::Fatal;
    }
    return std::unexpected(
        Error(ErrorCode::InvalidArgument,
              std::format("unknown log level '{}'; expected one of: trace, debug, info, warning, "
                          "error, fatal",
                          name)));
}

Status configure_logging(std::string_view level, std::string_view log_file) {
    const auto severity = parse_severity(level);
    if (!severity) {
        return std::unexpected(severity.error());
    }
    log::set_min_severity(*severity);
    log::add_sink(log::make_console_sink());

    if (!log_file.empty()) {
        auto sink = log::make_file_sink(std::string{log_file});
        if (!sink) {
            return std::unexpected(std::move(sink).error().context("configuring logging"));
        }
        log::add_sink(std::move(*sink));
    }
    return ok();
}

}  // namespace atlas::app
