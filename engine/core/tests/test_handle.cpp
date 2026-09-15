// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/handle.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

using atlas::ErrorCode;
using atlas::Handle;
using atlas::HandlePool;

namespace {

struct TestTag;
struct OtherTag;

using TestHandle = Handle<TestTag>;
using TestPool = HandlePool<std::string, TestTag>;

/// Counts its own lifetime, so that a pool leaking or double-destroying is visible.
struct Tracked {
    static inline int alive = 0;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

    int value = 0;

    explicit Tracked(int v) : value(v) { ++alive; }

    Tracked(const Tracked& other) : value(other.value) { ++alive; }

    Tracked(Tracked&& other) noexcept : value(other.value) { ++alive; }

    Tracked& operator=(const Tracked&) = default;
    Tracked& operator=(Tracked&&) = default;

    ~Tracked() { --alive; }
};

}  // namespace

TEST_CASE("a default handle is null and never resolves", "[core][handle]") {
    const TestHandle handle;
    CHECK_FALSE(handle.valid());
    CHECK_FALSE(static_cast<bool>(handle));

    TestPool pool;
    CHECK(pool.get(handle) == nullptr);
    CHECK_FALSE(pool.contains(handle));
    CHECK_FALSE(pool.destroy(handle));
}

TEST_CASE("an inserted value resolves through its handle", "[core][handle]") {
    TestPool pool;
    const auto handle = pool.insert("first");

    REQUIRE(handle.has_value());
    CHECK(handle->valid());
    REQUIRE(pool.get(*handle) != nullptr);
    CHECK(*pool.get(*handle) == "first");
    CHECK(pool.size() == 1);
}

TEST_CASE("a destroyed handle stops resolving", "[core][handle]") {
    TestPool pool;
    const auto handle = pool.insert("doomed");
    REQUIRE(handle.has_value());

    CHECK(pool.destroy(*handle));

    // The whole point: the handle still exists, and it no longer resolves. A raw pointer
    // here would still look usable.
    CHECK(pool.get(*handle) == nullptr);
    CHECK_FALSE(pool.contains(*handle));
    CHECK(pool.size() == 0);
}

TEST_CASE("destroying twice is safe and reported", "[core][handle]") {
    TestPool pool;
    const auto handle = pool.insert("once");
    REQUIRE(handle.has_value());

    CHECK(pool.destroy(*handle));
    CHECK_FALSE(pool.destroy(*handle));
}

TEST_CASE("a reused slot does not resolve the old handle", "[core][handle]") {
    // The case generations exist for. Without them the stale handle would resolve to the
    // new value, and the bug would look like data corruption rather than a lifetime error.
    TestPool pool;

    const auto first = pool.insert("first");
    REQUIRE(first.has_value());
    const std::uint32_t index = first->index();

    REQUIRE(pool.destroy(*first));

    const auto second = pool.insert("second");
    REQUIRE(second.has_value());

    CHECK(second->index() == index);  // the slot was reused
    CHECK(second->generation() != first->generation());
    CHECK(pool.get(*first) == nullptr);  // and the old handle is dead
    REQUIRE(pool.get(*second) != nullptr);
    CHECK(*pool.get(*second) == "second");
}

TEST_CASE("handles of different tags are different types", "[core][handle]") {
    // Compile-time property: a Handle<OtherTag> cannot be passed where Handle<TestTag> is
    // wanted. Asserting the type relationship is the closest a runtime test can get.
    STATIC_REQUIRE_FALSE(std::is_same_v<Handle<TestTag>, Handle<OtherTag>>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<TestHandle>);
}

TEST_CASE("equality compares index and generation", "[core][handle]") {
    TestPool pool;
    const auto first = pool.insert("a");
    const auto second = pool.insert("b");
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    CHECK(*first == *first);
    CHECK_FALSE(*first == *second);
    CHECK(TestHandle{} == TestHandle{});
}

TEST_CASE("slots are reused rather than growing without bound", "[core][handle]") {
    TestPool pool;

    for (int round = 0; round < 100; ++round) {
        const auto handle = pool.insert("churn");
        REQUIRE(handle.has_value());
        REQUIRE(pool.destroy(*handle));
    }

    // One slot, reused a hundred times. A pool that grew here would leak indices in any
    // workload that creates and destroys resources every frame.
    CHECK(pool.slot_count() == 1);
    CHECK(pool.size() == 0);
}

TEST_CASE("many live values coexist", "[core][handle]") {
    TestPool pool;
    std::vector<TestHandle> handles;

    for (int i = 0; i < 1000; ++i) {
        const auto handle = pool.insert(std::to_string(i));
        REQUIRE(handle.has_value());
        handles.push_back(*handle);
    }

    CHECK(pool.size() == 1000);
    for (int i = 0; i < 1000; ++i) {
        REQUIRE(pool.get(handles[static_cast<std::size_t>(i)]) != nullptr);
        CHECK(*pool.get(handles[static_cast<std::size_t>(i)]) == std::to_string(i));
    }
}

TEST_CASE("destroying some values leaves the rest resolvable", "[core][handle]") {
    TestPool pool;
    std::vector<TestHandle> handles;
    for (int i = 0; i < 10; ++i) {
        handles.push_back(*pool.insert(std::to_string(i)));
    }

    for (std::size_t i = 0; i < handles.size(); i += 2) {
        REQUIRE(pool.destroy(handles[i]));
    }

    CHECK(pool.size() == 5);
    for (std::size_t i = 0; i < handles.size(); ++i) {
        INFO("entry " << i);
        if (i % 2 == 0) {
            CHECK(pool.get(handles[i]) == nullptr);
        } else {
            REQUIRE(pool.get(handles[i]) != nullptr);
            CHECK(*pool.get(handles[i]) == std::to_string(i));
        }
    }
}

TEST_CASE("for_each_live visits exactly the live values", "[core][handle]") {
    TestPool pool;
    const auto keep = pool.insert("keep");
    const auto drop = pool.insert("drop");
    REQUIRE(keep.has_value());
    REQUIRE(drop.has_value());
    REQUIRE(pool.destroy(*drop));

    std::vector<std::string> seen;
    pool.for_each_live([&seen](TestHandle, const std::string& value) { seen.push_back(value); });

    REQUIRE(seen.size() == 1);
    CHECK(seen[0] == "keep");
}

TEST_CASE("a pool destroys what it holds", "[core][handle]") {
    // This is what makes the leak report at device shutdown meaningful: if the pool itself
    // leaked, the report would be clean while the resources were not.
    REQUIRE(Tracked::alive == 0);
    {
        HandlePool<Tracked, TestTag> pool;
        for (int i = 0; i < 5; ++i) {
            REQUIRE(pool.insert(Tracked{i}).has_value());
        }
        CHECK(Tracked::alive == 5);
    }
    CHECK(Tracked::alive == 0);
}

TEST_CASE("destroying a value destroys it immediately", "[core][handle]") {
    REQUIRE(Tracked::alive == 0);
    HandlePool<Tracked, TestTag> pool;

    const auto handle = pool.insert(Tracked{7});
    REQUIRE(handle.has_value());
    CHECK(Tracked::alive == 1);

    REQUIRE(pool.destroy(*handle));
    CHECK(Tracked::alive == 0);
}

TEST_CASE("clear destroys everything and invalidates every handle", "[core][handle]") {
    TestPool pool;
    const auto first = pool.insert("a");
    const auto second = pool.insert("b");
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    pool.clear();

    CHECK(pool.size() == 0);
    CHECK(pool.get(*first) == nullptr);
    CHECK(pool.get(*second) == nullptr);

    // And the slots are available again.
    const auto reused = pool.insert("c");
    REQUIRE(reused.has_value());
    CHECK(pool.slot_count() == 2);
}

TEST_CASE("a handle from one pool does not resolve in another", "[core][handle]") {
    // Two pools of the same type have independent slot spaces. A handle from one happens to
    // be a valid index in the other, which is exactly the confusion generations cannot
    // catch, so this documents the limit rather than pretending otherwise.
    TestPool first;
    TestPool second;

    const auto handle = first.insert("in the first pool");
    REQUIRE(handle.has_value());

    // The second pool is empty, so the index is out of range and the lookup fails.
    CHECK(second.get(*handle) == nullptr);
}
