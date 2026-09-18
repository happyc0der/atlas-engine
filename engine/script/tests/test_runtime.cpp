// SPDX-License-Identifier: GPL-3.0-or-later
// The runtime's lifetime and its counted allocator.
#include <atlas/core/assert.hpp>
#include <atlas/script/runtime.hpp>

#include <catch2/catch_test_macros.hpp>

using atlas::script::Runtime;

namespace {

const bool kMainThreadMarkedForRuntime = [] {
    atlas::mark_main_thread();
    return true;
}();

}  // namespace

TEST_CASE("a runtime starts, reports itself alive, and stops", "[script][runtime]") {
    REQUIRE_FALSE(Runtime::alive());
    {
        const auto runtime = Runtime::create();
        REQUIRE(runtime.has_value());
        CHECK(Runtime::alive());
    }
    // Destroyed with the scope. Checked because the process-wide teardown is the reason this
    // object exists at all, and a runtime that leaks would make every later case in the binary
    // hit the single-instance assertion instead of running.
    CHECK_FALSE(Runtime::alive());
}

TEST_CASE("a runtime can be created again after the first is gone", "[script][runtime]") {
    {
        const auto first = Runtime::create();
        REQUIRE(first.has_value());
    }
    const auto second = Runtime::create();
    REQUIRE(second.has_value());
    CHECK(Runtime::alive());
}

TEST_CASE("moving a runtime does not tear it down", "[script][runtime]") {
    auto created = Runtime::create();
    REQUIRE(created.has_value());
    Runtime moved = *std::move(created);
    // The moved-from object must not destroy the process-wide runtime when it goes out of
    // scope, or the one that owns it is left holding something already freed.
    CHECK(Runtime::alive());
    CHECK(moved.stats().peak_bytes_in_use > 0);
}

TEST_CASE("a budget too small to hold anything is refused rather than starved",
          "[script][runtime]") {
    // Refused at `create` rather than producing a runtime where every allocation fails: a
    // runtime that cannot allocate looks exactly like one whose mods all happen to be broken.
    const auto refused = Runtime::create(1);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code() == atlas::ErrorCode::InvalidArgument);
    CHECK_FALSE(Runtime::alive());
}
