// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/build_info.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string_view>

namespace info = atlas::build_info;

TEST_CASE("build identity is populated at configure time", "[core][build_info]") {
    // Every save file, replay, and benchmark result records these. An empty field would
    // make an artifact untraceable to the build that produced it.
    CHECK_FALSE(info::version().empty());
    CHECK_FALSE(info::git_commit().empty());
    CHECK_FALSE(info::compiler().empty());
    CHECK_FALSE(info::build_type().empty());
    CHECK_FALSE(info::system().empty());
    CHECK_FALSE(info::configured_at().empty());
    CHECK_FALSE(info::summary().empty());
}

TEST_CASE("the version matches the project version", "[core][build_info]") {
    CHECK(info::version() == "0.0.1");
}

TEST_CASE("the sanitizer field names the configuration actually built", "[core][build_info]") {
    const auto sanitizer = info::sanitizer();
    CHECK((sanitizer == "none" || sanitizer == "address" || sanitizer == "thread"));
}

TEST_CASE("profiling state is reported honestly", "[core][build_info]") {
    // Whatever the value, it must agree with the macro the build was compiled with.
#if ATLAS_TRACY_ENABLED
    CHECK(info::profiling_enabled());
#else
    CHECK_FALSE(info::profiling_enabled());
#endif
}

TEST_CASE("the summary mentions the version and the build type", "[core][build_info]") {
    const std::string_view summary = info::summary();
    CHECK(summary.contains("0.0.1"));
    CHECK(summary.contains(info::build_type()));
}
