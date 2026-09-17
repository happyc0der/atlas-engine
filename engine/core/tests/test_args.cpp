// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/args.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

using atlas::Args;
using atlas::ErrorCode;

namespace {

/// Parse from a literal argument list, as argv would arrive.
template <std::size_t N> [[nodiscard]] auto parse(const std::array<const char*, N>& argv) {
    return Args::parse(static_cast<int>(N), argv.data());
}

}  // namespace

TEST_CASE("an empty command line parses", "[core][args]") {
    const std::array argv{"atlas_sandbox"};
    const auto args = parse(argv);

    REQUIRE(args.has_value());
    CHECK(args->program() == "atlas_sandbox");
    CHECK_FALSE(args->has("headless"));
}

TEST_CASE("a flag is recognised without a value", "[core][args]") {
    const std::array argv{"app", "--headless"};
    const auto args = parse(argv);

    REQUIRE(args.has_value());
    CHECK(args->has("headless"));
    CHECK_FALSE(args->has("verbose"));
}

TEST_CASE("an option takes the following token as its value", "[core][args]") {
    const std::array argv{"app", "--log-level", "debug"};
    const auto args = parse(argv);

    REQUIRE(args.has_value());
    const auto value = args->value("log-level");
    REQUIRE(value.has_value());
    CHECK(*value == "debug");
}

TEST_CASE("an option accepts the equals form", "[core][args]") {
    const std::array argv{"app", "--log-level=trace"};
    const auto args = parse(argv);

    REQUIRE(args.has_value());
    const auto value = args->value("log-level");
    REQUIRE(value.has_value());
    CHECK(*value == "trace");
}

TEST_CASE("a flag followed by another option stays a flag", "[core][args]") {
    const std::array argv{"app", "--headless", "--iterations", "10"};
    const auto args = parse(argv);

    REQUIRE(args.has_value());
    CHECK(args->has("headless"));

    const auto iterations = args->value_or("iterations", std::uint64_t{0});
    REQUIRE(iterations.has_value());
    CHECK(*iterations == 10);
}

TEST_CASE("a numeric option parses, and rejects anything that is not a number", "[core][args]") {
    SECTION("valid") {
        const std::array argv{"app", "--iterations", "250"};
        const auto args = parse(argv);
        REQUIRE(args.has_value());

        const auto value = args->value_or("iterations", std::uint64_t{1});
        REQUIRE(value.has_value());
        CHECK(*value == 250);
    }

    SECTION("absent, so the fallback is used") {
        const std::array argv{"app"};
        const auto args = parse(argv);
        REQUIRE(args.has_value());

        const auto value = args->value_or("iterations", std::uint64_t{99});
        REQUIRE(value.has_value());
        CHECK(*value == 99);
    }

    SECTION("not a number") {
        const std::array argv{"app", "--iterations", "many"};
        const auto args = parse(argv);
        REQUIRE(args.has_value());

        const auto value = args->value_or("iterations", std::uint64_t{1});
        REQUIRE_FALSE(value.has_value());
        CHECK(value.error().code() == ErrorCode::InvalidArgument);
    }

    SECTION("trailing garbage is not silently truncated") {
        const std::array argv{"app", "--iterations", "12abc"};
        const auto args = parse(argv);
        REQUIRE(args.has_value());

        const auto value = args->value_or("iterations", std::uint64_t{1});
        CHECK_FALSE(value.has_value());
    }
}

TEST_CASE("a string fallback is used when the option is absent", "[core][args]") {
    const std::array argv{"app"};
    const auto args = parse(argv);
    REQUIRE(args.has_value());

    CHECK(args->value_or("log-level", std::string_view{"info"}) == "info");
}

TEST_CASE("a missing required option is an error naming the option", "[core][args]") {
    const std::array argv{"app"};
    const auto args = parse(argv);
    REQUIRE(args.has_value());

    const auto value = args->value("scene");
    REQUIRE_FALSE(value.has_value());
    CHECK(value.error().code() == ErrorCode::NotFound);
    CHECK(value.error().message().contains("--scene"));
}

TEST_CASE("asking for a value from a flag is an error", "[core][args]") {
    const std::array argv{"app", "--headless"};
    const auto args = parse(argv);
    REQUIRE(args.has_value());

    const auto value = args->value("headless");
    REQUIRE_FALSE(value.has_value());
    CHECK(value.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("a positional argument is rejected rather than ignored", "[core][args]") {
    const std::array argv{"app", "scene.json"};
    const auto args = parse(argv);

    REQUIRE_FALSE(args.has_value());
    CHECK(args.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("an unqueried option is reported, so a typo is not silently ignored", "[core][args]") {
    const std::array argv{"app", "--headles"};  // deliberate typo
    const auto args = parse(argv);
    REQUIRE(args.has_value());

    // The application asks for the option it knows about, which does not match.
    CHECK_FALSE(args->has("headless"));

    const auto status = args->reject_unknown();
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code() == ErrorCode::InvalidArgument);
    CHECK(status.error().message().contains("--headles"));
}

TEST_CASE("every option being queried leaves nothing unknown", "[core][args]") {
    const std::array argv{"app", "--headless", "--iterations", "5"};
    const auto args = parse(argv);
    REQUIRE(args.has_value());

    CHECK(args->has("headless"));
    CHECK(args->value_or("iterations", std::uint64_t{0}).has_value());
    CHECK(args->reject_unknown().has_value());
}

TEST_CASE("an empty option is rejected", "[core][args]") {
    const std::array argv{"app", "--"};
    const auto args = parse(argv);

    REQUIRE_FALSE(args.has_value());
    CHECK(args.error().code() == ErrorCode::InvalidArgument);
}
