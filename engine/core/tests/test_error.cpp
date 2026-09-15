// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/error.hpp>
#include <atlas/core/result.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>

using atlas::Error;
using atlas::ErrorCode;
using atlas::ErrorDomain;
using atlas::Result;
using atlas::Status;

TEST_CASE("error domain is derived from the code block", "[core][error]") {
    CHECK(atlas::error_domain(ErrorCode::InvalidArgument) == ErrorDomain::Generic);
    CHECK(atlas::error_domain(ErrorCode::PlatformInitFailed) == ErrorDomain::Platform);
    CHECK(atlas::error_domain(ErrorCode::GpuUnavailable) == ErrorDomain::Gpu);
    CHECK(atlas::error_domain(ErrorCode::AssetNotFound) == ErrorDomain::Asset);
    CHECK(atlas::error_domain(ErrorCode::VersionMismatch) == ErrorDomain::Serialization);
}

TEST_CASE("every error code has a name", "[core][error]") {
    // A code without a name would print as a number in a log, which is useless to whoever
    // is reading it at the time. Spot-check one from each block plus the boundaries.
    CHECK(atlas::to_string(ErrorCode::Unknown) == "Unknown");
    CHECK(atlas::to_string(ErrorCode::Internal) == "Internal");
    CHECK(atlas::to_string(ErrorCode::DisplayUnavailable) == "DisplayUnavailable");
    CHECK(atlas::to_string(ErrorCode::DeviceLost) == "DeviceLost");
    CHECK(atlas::to_string(ErrorCode::AssetDecodeFailed) == "AssetDecodeFailed");
    CHECK(atlas::to_string(ErrorCode::MalformedData) == "MalformedData");
}

TEST_CASE("an error carries code, message, and source location", "[core][error]") {
    const Error error{ErrorCode::NotFound, "no such thing"};

    CHECK(error.code() == ErrorCode::NotFound);
    CHECK(error.message() == "no such thing");
    CHECK(error.native_code() == 0);
    // The location is where the Error was constructed, which is this file.
    CHECK(std::string_view{error.where().file_name()}.find("test_error.cpp") !=
          std::string_view::npos);
}

TEST_CASE("an error constructed from a code alone uses the code name as its message",
          "[core][error]") {
    const Error error{ErrorCode::Exhausted};
    CHECK(error.message() == "Exhausted");
}

TEST_CASE("context accumulates outermost-first", "[core][error]") {
    Error error{ErrorCode::MalformedData, "truncated"};
    error.context("decoding PNG").context("loading terrain.png");

    // Reads as a narrowing path from what was attempted down to what actually failed.
    CHECK(error.message() == "loading terrain.png: decoding PNG: truncated");
}

TEST_CASE("context on an rvalue returns the error for chaining", "[core][error]") {
    const Error error = Error{ErrorCode::NotFound, "missing"}.context("while starting up");
    CHECK(error.message() == "while starting up: missing");
}

TEST_CASE("a native code is reported when present", "[core][error]") {
    const Error without{ErrorCode::GpuUnavailable, "no device"};
    CHECK(without.to_string() == "no device [GpuUnavailable]");

    const Error with = Error{ErrorCode::GpuUnavailable, "no device"}.with_native_code(-3);
    CHECK(with.native_code() == -3);
    CHECK(with.to_string() == "no device [GpuUnavailable] (native -3)");
}

TEST_CASE("an error formats through std::format", "[core][error]") {
    const Error error{ErrorCode::NotSupported, "not on this backend"};
    CHECK(std::format("{}", error) == "not on this backend [NotSupported]");
}

namespace {

Result<int> succeed() {
    return 7;
}

Result<int> fail_with(ErrorCode code) {
    return std::unexpected(Error{code, "deliberate"});
}

Status void_success() {
    return atlas::ok();
}

}  // namespace

TEST_CASE("a Result carries either a value or an error", "[core][result]") {
    const auto good = succeed();
    REQUIRE(good.has_value());
    CHECK(*good == 7);

    const auto bad = fail_with(ErrorCode::Cancelled);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().code() == ErrorCode::Cancelled);
}

TEST_CASE("Status represents a fallible operation with no value", "[core][result]") {
    const auto status = void_success();
    CHECK(status.has_value());
}

TEST_CASE("forward_error changes the value type and adds context", "[core][result]") {
    auto source = fail_with(ErrorCode::AssetNotFound);
    const Result<std::string> forwarded =
        atlas::forward_error<std::string>(std::move(source), "importing a texture");

    REQUIRE_FALSE(forwarded.has_value());
    CHECK(forwarded.error().code() == ErrorCode::AssetNotFound);
    CHECK(forwarded.error().message() == "importing a texture: deliberate");
}

TEST_CASE("fail builds a failed Result without naming the type twice", "[core][result]") {
    const Result<int> result = atlas::fail<int>(ErrorCode::OutOfRange, "index 5 of 3");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().code() == ErrorCode::OutOfRange);
    CHECK(result.error().message() == "index 5 of 3");
}
