// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/app/log_options.hpp>

#include <catch2/catch_test_macros.hpp>

using atlas::ErrorCode;
using atlas::app::parse_severity;
using atlas::log::Severity;

TEST_CASE("every level parses by name", "[app][log]") {
    CHECK(parse_severity("trace").value() == Severity::Trace);
    CHECK(parse_severity("debug").value() == Severity::Debug);
    CHECK(parse_severity("info").value() == Severity::Info);
    CHECK(parse_severity("warning").value() == Severity::Warning);
    CHECK(parse_severity("warn").value() == Severity::Warning);
    CHECK(parse_severity("error").value() == Severity::Error);
    CHECK(parse_severity("fatal").value() == Severity::Fatal);
}

TEST_CASE("an unknown level is refused and the message lists the valid ones", "[app][log]") {
    const auto result = parse_severity("shouty");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code() == ErrorCode::InvalidArgument);
    CHECK(result.error().to_string().find("trace") != std::string::npos);
}
