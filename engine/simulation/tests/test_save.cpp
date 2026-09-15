// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/core/assert.hpp>
#include <atlas/simulation/save.hpp>

#include "synthetic_systems.hpp"
#include <catch2/catch_test_macros.hpp>

#include <vector>

using atlas::ErrorCode;
using atlas::sim::Kernel;
using atlas::sim::KernelConfig;
using atlas::sim::kSaveFormatVersion;
using atlas::sim::load;
using atlas::sim::read_header;
using atlas::sim::save;
using atlas::sim::SourceId;
using atlas::sim::testing::Harness;
using atlas::sim::testing::increment_values;
using atlas::sim::testing::sum_into_counter;

namespace {

const bool kMainThreadMarked = [] {
    atlas::mark_main_thread();
    return true;
}();

/// A harness that has already run a few ticks, so its state is not the starting one.
struct Ran {
    Harness h;
    Kernel kernel;

    Ran() : kernel(h.world, h.schedule, h.commands, KernelConfig{.seed = 987}) {
        REQUIRE(h.schedule.add(increment_values(h.values)).has_value());
        REQUIRE(h.schedule.add(sum_into_counter(h.values, h.counter)).has_value());
        REQUIRE(h.schedule.finalise(h.world).has_value());
        REQUIRE(kernel.run(17).has_value());
    }
};

}  // namespace

TEST_CASE("a save round-trips the state, tick and seed", "[sim][save]") {
    Ran source;
    const auto bytes = save(source.h.world, source.kernel, source.h.commands);
    REQUIRE(bytes.has_value());

    const std::uint64_t expected_hash = source.h.world.hash();
    const auto expected_tick = source.kernel.current_tick();

    // A separate, freshly built world, as loading in a new process would have.
    Harness target;
    REQUIRE(target.schedule.finalise(target.world).has_value());
    Kernel kernel(target.world, target.schedule, target.commands, KernelConfig{});

    REQUIRE(load(target.world, kernel, target.commands, *bytes).has_value());

    CHECK(target.world.hash() == expected_hash);
    CHECK(kernel.current_tick() == expected_tick);
    CHECK(kernel.seed() == 987);
}

TEST_CASE("a loaded simulation continues identically", "[sim][save]") {
    // The property a save exists for. Resuming from a file must be the same as never having
    // stopped, not merely similar.
    Ran source;
    const auto bytes = save(source.h.world, source.kernel, source.h.commands);
    REQUIRE(bytes.has_value());

    const auto continued = source.kernel.run(20);
    REQUIRE(continued.has_value());

    Harness target;
    REQUIRE(target.schedule.add(increment_values(target.values)).has_value());
    REQUIRE(target.schedule.add(sum_into_counter(target.values, target.counter)).has_value());
    REQUIRE(target.schedule.finalise(target.world).has_value());
    Kernel kernel(target.world, target.schedule, target.commands, KernelConfig{});
    REQUIRE(load(target.world, kernel, target.commands, *bytes).has_value());

    const auto resumed = kernel.run(20);
    REQUIRE(resumed.has_value());

    REQUIRE(continued->size() == resumed->size());
    for (std::size_t i = 0; i < continued->size(); ++i) {
        INFO("tick " << i);
        CHECK((*continued)[i].tick == (*resumed)[i].tick);
        CHECK((*continued)[i].state_hash == (*resumed)[i].state_hash);
    }
}

TEST_CASE("the header can be read without loading", "[sim][save]") {
    Ran source;
    const auto bytes = save(source.h.world, source.kernel, source.h.commands);
    REQUIRE(bytes.has_value());

    const auto header = read_header(*bytes);
    REQUIRE(header.has_value());
    CHECK(header->format_version == kSaveFormatVersion);
    CHECK(header->tick == source.kernel.current_tick());
    CHECK(header->seed == 987);
    CHECK(header->state_hash == source.h.world.hash());
}

TEST_CASE("saving the same state twice produces the same bytes", "[sim][save]") {
    Ran source;
    const auto first = save(source.h.world, source.kernel, source.h.commands);
    const auto second = save(source.h.world, source.kernel, source.h.commands);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(*first == *second);
}

TEST_CASE("something that is not a save is refused", "[sim][save]") {
    Harness h;
    REQUIRE(h.schedule.finalise(h.world).has_value());
    Kernel kernel(h.world, h.schedule, h.commands, KernelConfig{});

    const std::vector<std::byte> junk(64, std::byte{0x41});
    const auto status = load(h.world, kernel, h.commands, junk);
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code() == ErrorCode::MalformedData);
}

TEST_CASE("an empty buffer is refused", "[sim][save]") {
    Harness h;
    REQUIRE(h.schedule.finalise(h.world).has_value());
    Kernel kernel(h.world, h.schedule, h.commands, KernelConfig{});
    CHECK_FALSE(load(h.world, kernel, h.commands, {}).has_value());
}

TEST_CASE("a save from a newer build is refused", "[sim][save]") {
    // Reading it would silently drop whatever the newer version added, and the first sign
    // would be data disappearing on the next save.
    Ran source;
    auto bytes = save(source.h.world, source.kernel, source.h.commands);
    REQUIRE(bytes.has_value());

    // The format version sits immediately after the eight-byte magic.
    (*bytes)[8] = static_cast<std::byte>(kSaveFormatVersion + 1);

    const auto header = read_header(*bytes);
    REQUIRE_FALSE(header.has_value());
    CHECK(header.error().code() == ErrorCode::VersionMismatch);
}

TEST_CASE("every truncation of a save is refused", "[sim][save]") {
    // Not only the obvious ones. A file cut at any point must fail rather than read past its
    // end or half-populate the world.
    Ran source;
    const auto bytes = save(source.h.world, source.kernel, source.h.commands);
    REQUIRE(bytes.has_value());

    for (std::size_t length = 0; length < bytes->size(); ++length) {
        Harness target;
        REQUIRE(target.schedule.finalise(target.world).has_value());
        Kernel kernel(target.world, target.schedule, target.commands, KernelConfig{});

        INFO("truncated to " << length << " of " << bytes->size() << " bytes");
        CHECK_FALSE(load(target.world, kernel, target.commands,
                         std::span<const std::byte>{*bytes}.subspan(0, length))
                        .has_value());
    }
}

TEST_CASE("a corrupted save fails its integrity check", "[sim][save]") {
    // The stored hash is a statement about the state, so a flipped byte anywhere in the
    // tables is caught even though the file is still structurally valid.
    Ran source;
    auto bytes = save(source.h.world, source.kernel, source.h.commands);
    REQUIRE(bytes.has_value());

    // Well past the header, inside the table data.
    const std::size_t at = bytes->size() - 12;
    (*bytes)[at] = static_cast<std::byte>(std::to_integer<int>((*bytes)[at]) ^ 0xFF);

    Harness target;
    REQUIRE(target.schedule.finalise(target.world).has_value());
    Kernel kernel(target.world, target.schedule, target.commands, KernelConfig{});

    const auto status = load(target.world, kernel, target.commands, *bytes);
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code() == ErrorCode::IntegrityCheckFailed);
}

TEST_CASE("a failed load leaves the world empty rather than half-populated", "[sim][save]") {
    // An empty world is a state a caller can recognise and recover from. A world holding the
    // file's rows in some tables and the previous state in others is not.
    Ran source;
    auto bytes = save(source.h.world, source.kernel, source.h.commands);
    REQUIRE(bytes.has_value());

    const std::size_t at = bytes->size() - 12;
    (*bytes)[at] = static_cast<std::byte>(std::to_integer<int>((*bytes)[at]) ^ 0xFF);

    Ran target;  // has real state in it
    REQUIRE(target.h.world.table(target.h.values)->row_count() > 0);

    REQUIRE_FALSE(load(target.h.world, target.kernel, target.h.commands, *bytes).has_value());

    CHECK(target.h.world.table(target.h.values)->row_count() == 0);
    CHECK(target.h.world.table_count() == 2);  // the tables themselves stay registered
}

TEST_CASE("a save whose tables the build does not have is refused", "[sim][save]") {
    Ran source;
    const auto bytes = save(source.h.world, source.kernel, source.h.commands);
    REQUIRE(bytes.has_value());

    // A world with only one of the two tables.
    atlas::sim::World target;
    REQUIRE(target.add_table("counter", std::make_unique<atlas::sim::testing::CounterTable>())
                .has_value());
    atlas::sim::Schedule schedule;
    REQUIRE(schedule.finalise(target).has_value());
    atlas::sim::CommandQueue queue;
    Kernel kernel(target, schedule, queue, KernelConfig{});

    const auto status = load(target, kernel, queue, *bytes);
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code() == ErrorCode::MalformedData);
}

TEST_CASE("command sequence numbers survive a save", "[sim][save]") {
    // So a command submitted after a load cannot reuse a number that already appeared, which
    // would make the command order stop being total.
    Ran source;
    source.h.commands.set_next_sequence(SourceId{3}, 42);
    source.h.commands.set_next_sequence(SourceId{9}, 7);

    const auto bytes = save(source.h.world, source.kernel, source.h.commands);
    REQUIRE(bytes.has_value());

    Harness target;
    REQUIRE(target.schedule.finalise(target.world).has_value());
    Kernel kernel(target.world, target.schedule, target.commands, KernelConfig{});
    REQUIRE(load(target.world, kernel, target.commands, *bytes).has_value());

    CHECK(target.commands.next_sequence(SourceId{3}) == 42);
    CHECK(target.commands.next_sequence(SourceId{9}) == 7);
}

TEST_CASE("trailing bytes are refused", "[sim][save]") {
    Ran source;
    auto bytes = save(source.h.world, source.kernel, source.h.commands);
    REQUIRE(bytes.has_value());
    bytes->push_back(std::byte{0});

    Harness target;
    REQUIRE(target.schedule.finalise(target.world).has_value());
    Kernel kernel(target.world, target.schedule, target.commands, KernelConfig{});

    const auto status = load(target.world, kernel, target.commands, *bytes);
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code() == ErrorCode::MalformedData);
}
