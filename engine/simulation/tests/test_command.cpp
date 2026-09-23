// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/simulation/command.hpp>

#include "synthetic_tables.hpp"
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <ranges>
#include <vector>

using atlas::ErrorCode;
using atlas::Status;
using atlas::sim::ApplyContext;
using atlas::sim::Command;
using atlas::sim::command_type;
using atlas::sim::CommandHandler;
using atlas::sim::CommandQueue;
using atlas::sim::CommandType;
using atlas::sim::SourceId;
using atlas::sim::World;
using atlas::sim::testing::CounterTable;

namespace {

const CommandType kAdd = command_type("add");

[[nodiscard]] std::vector<std::byte> payload_of(std::uint8_t value) {
    return {static_cast<std::byte>(value)};
}

/// Accepts a single byte and adds it to the counter table.
[[nodiscard]] CommandHandler adding_handler() {
    CommandHandler handler;
    handler.validate = [](std::span<const std::byte> payload) -> atlas::Status {
        if (payload.size() != 1) {
            return std::unexpected(
                atlas::Error(ErrorCode::MalformedData, "expected exactly one byte"));
        }
        return atlas::ok();
    };
    handler.apply = [](World& world, const ApplyContext&,
                       std::span<const std::byte> payload) -> Status {
        auto* table = dynamic_cast<CounterTable*>(world.table(atlas::sim::table_id("counter")));
        if (table != nullptr) {
            table->count += std::to_integer<std::uint64_t>(payload[0]);
        }
        return atlas::ok();
    };
    return handler;
}

struct Fixture {
    World world;
    CommandQueue queue;

    Fixture() {
        REQUIRE(world.add_table("counter", std::make_unique<CounterTable>()).has_value());
        REQUIRE(queue.register_handler(kAdd, adding_handler()).has_value());
    }

    [[nodiscard]] std::uint64_t count() const {
        const auto* table =
            dynamic_cast<const CounterTable*>(world.table(atlas::sim::table_id("counter")));
        return table != nullptr ? table->count : 0;
    }
};

}  // namespace

TEST_CASE("a command type comes from its name", "[sim][command]") {
    CHECK(command_type("add") == command_type("add"));
    CHECK(command_type("add") != command_type("remove"));
    CHECK(command_type("add") != CommandType::Invalid);
}

TEST_CASE("a handler needs both validation and application", "[sim][command]") {
    // Validation is required, not optional: without it an unchecked payload reaches apply,
    // which is where it is too late to refuse.
    CommandQueue queue;

    CommandHandler apply_only;
    apply_only.apply = [](World&, const ApplyContext&, std::span<const std::byte>) {
        return atlas::ok();
    };
    CHECK_FALSE(queue.register_handler(kAdd, std::move(apply_only)).has_value());

    CommandHandler validate_only;
    validate_only.validate = [](std::span<const std::byte>) { return atlas::ok(); };
    CHECK_FALSE(queue.register_handler(kAdd, std::move(validate_only)).has_value());
}

TEST_CASE("registering a type twice is refused", "[sim][command]") {
    Fixture f;
    const auto again = f.queue.register_handler(kAdd, adding_handler());
    REQUIRE_FALSE(again.has_value());
    CHECK(again.error().code() == ErrorCode::AlreadyExists);
}

TEST_CASE("a command with no handler is refused", "[sim][command]") {
    Fixture f;
    const auto status = f.queue.submit(1, SourceId::Local, command_type("unknown"), payload_of(1));
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code() == ErrorCode::NotFound);
}

TEST_CASE("a payload is validated on submission", "[sim][command]") {
    // While the caller still has the context that produced it, rather than several ticks
    // later when only the bytes are left.
    Fixture f;
    const std::array<std::byte, 3> too_long{};
    const auto status = f.queue.submit(1, SourceId::Local, kAdd, too_long);
    REQUIRE_FALSE(status.has_value());
    CHECK(f.queue.pending() == 0);
}

TEST_CASE("an oversized payload is refused", "[sim][command]") {
    Fixture f;
    const std::vector<std::byte> huge(CommandQueue::kMaxPayload + 1, std::byte{0});
    const auto status = f.queue.submit(1, SourceId::Local, kAdd, huge);
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code() == ErrorCode::OutOfRange);
}

TEST_CASE("commands are drained only for the tick they name", "[sim][command]") {
    Fixture f;
    REQUIRE(f.queue.submit(5, SourceId::Local, kAdd, payload_of(1)).has_value());
    REQUIRE(f.queue.submit(7, SourceId::Local, kAdd, payload_of(2)).has_value());
    CHECK(f.queue.pending() == 2);

    CHECK(f.queue.drain(4).empty());
    CHECK(f.queue.pending() == 2);

    const auto at_five = f.queue.drain(5);
    REQUIRE(at_five.size() == 1);
    CHECK(at_five[0].target == 5);
    CHECK(f.queue.pending() == 1);
}

TEST_CASE("draining takes everything at or before the tick", "[sim][command]") {
    // A command stamped for a tick already past must still come out, so the kernel can
    // record that it was late rather than have it vanish.
    Fixture f;
    REQUIRE(f.queue.submit(1, SourceId::Local, kAdd, payload_of(1)).has_value());
    REQUIRE(f.queue.submit(2, SourceId::Local, kAdd, payload_of(2)).has_value());
    REQUIRE(f.queue.submit(3, SourceId::Local, kAdd, payload_of(3)).has_value());

    const auto taken = f.queue.drain(2);
    CHECK(taken.size() == 2);
    CHECK(f.queue.pending() == 1);
}

TEST_CASE("drained commands are ordered by source then sequence", "[sim][command]") {
    // The total order two machines can agree on without agreeing on timing. Submitted here
    // in a deliberately scrambled order.
    Fixture f;
    const auto a = SourceId{7};
    const auto b = SourceId{2};

    REQUIRE(f.queue.submit(1, a, kAdd, payload_of(1)).has_value());
    REQUIRE(f.queue.submit(1, b, kAdd, payload_of(2)).has_value());
    REQUIRE(f.queue.submit(1, a, kAdd, payload_of(3)).has_value());
    REQUIRE(f.queue.submit(1, b, kAdd, payload_of(4)).has_value());

    const auto taken = f.queue.drain(1);
    REQUIRE(taken.size() == 4);

    CHECK(taken[0].source == b);
    CHECK(taken[0].sequence == 0);
    CHECK(taken[1].source == b);
    CHECK(taken[1].sequence == 1);
    CHECK(taken[2].source == a);
    CHECK(taken[2].sequence == 0);
    CHECK(taken[3].source == a);
    CHECK(taken[3].sequence == 1);
}

TEST_CASE("two commands sharing a source and sequence still drain in one fixed order",
          "[sim][command]") {
    // `submit` cannot produce this, but `submit_stamped` keeps whatever it is given, so an
    // untrusted producer can. `net::Session` now refuses such a turn, and this is the second
    // half of that fix: the order must be defined even if a collision reaches the queue,
    // because `sort` is not stable and two machines would otherwise pick different answers and
    // diverge silently.
    //
    // Fed to two queues in opposite orders, which is what a reordering link produces. Stability
    // would give these two different answers; comparing the content gives the same one.
    //
    // Eight colliding commands rather than two, and deliberately so. With a pair, whether an
    // unsorted comparator happens to swap them is up to the standard library's small-range
    // path — so a two-element version of this case passes or fails by luck of the platform,
    // which is a test that only looks like one. Eight is past every implementation's insertion
    // -sort threshold, so removing the tie-break reorders them on any of the three.
    constexpr std::uint8_t kColliding = 8;
    Fixture forwards;
    Fixture backwards;

    const auto source = SourceId{4};
    std::vector<Command> commands;
    commands.reserve(kColliding);
    for (std::uint8_t i = 0; i < kColliding; ++i) {
        commands.push_back(Command{
            .target = 1, .source = source, .sequence = 9, .type = kAdd, .payload = payload_of(i)});
    }

    for (const auto& command : commands) {
        REQUIRE(forwards.queue.submit_stamped(command).has_value());
    }
    for (const auto& command : std::ranges::reverse_view(commands)) {
        REQUIRE(backwards.queue.submit_stamped(command).has_value());
    }

    const auto one = forwards.queue.drain(1);
    const auto other = backwards.queue.drain(1);
    REQUIRE(one.size() == kColliding);
    REQUIRE(other.size() == kColliding);

    for (std::size_t i = 0; i < kColliding; ++i) {
        INFO("position " << i);
        // The same answer from opposite arrival orders, which is the property two peers need.
        CHECK(one[i].payload == other[i].payload);
        // And that answer is the content's own order, not either arrival order.
        CHECK(one[i].payload == payload_of(static_cast<std::uint8_t>(i)));
    }
}

TEST_CASE("a mod's identifier can never be mistaken for a peer's", "[sim][command]") {
    // ADR-0014 asserted this scheme and nothing implemented it until M15. Split by a bit rather
    // than by a range so the two cannot be confused by arithmetic, and so a source arriving
    // over a link can be recognised as impossible rather than merely unexpected.
    CHECK(atlas::sim::is_mod_source(atlas::sim::mod_source(0)));
    CHECK(atlas::sim::is_mod_source(atlas::sim::mod_source(7)));
    CHECK(atlas::sim::mod_source(7) != atlas::sim::mod_source(8));

    // Peers come from link indices, which are small and dense, and `kMaxPeers` is sixteen.
    CHECK_FALSE(atlas::sim::is_mod_source(SourceId::Local));
    for (std::uint32_t peer = 0; peer < 64; ++peer) {
        INFO("peer " << peer);
        CHECK_FALSE(atlas::sim::is_mod_source(SourceId{peer}));
    }

    // The two ranges do not overlap anywhere, which is the property the bit buys and the one
    // worth checking at the boundary rather than in the middle.
    CHECK_FALSE(atlas::sim::is_mod_source(SourceId{atlas::sim::kModSourceBit - 1}));
    CHECK(atlas::sim::is_mod_source(SourceId{atlas::sim::kModSourceBit}));
}

TEST_CASE("sequence numbers are per source and assigned by the queue", "[sim][command]") {
    // Assigned rather than supplied, so a caller cannot reuse one and make the order
    // ambiguous.
    Fixture f;
    REQUIRE(f.queue.submit(1, SourceId{1}, kAdd, payload_of(1)).has_value());
    REQUIRE(f.queue.submit(1, SourceId{1}, kAdd, payload_of(1)).has_value());
    REQUIRE(f.queue.submit(1, SourceId{2}, kAdd, payload_of(1)).has_value());

    CHECK(f.queue.next_sequence(SourceId{1}) == 2);
    CHECK(f.queue.next_sequence(SourceId{2}) == 1);
    CHECK(f.queue.next_sequence(SourceId{3}) == 0);
}

TEST_CASE("applying a command changes the world", "[sim][command]") {
    Fixture f;
    REQUIRE(f.queue.submit(1, SourceId::Local, kAdd, payload_of(7)).has_value());

    const auto taken = f.queue.drain(1);
    REQUIRE(taken.size() == 1);
    const auto outcome = f.queue.apply(f.world, taken[0]);
    REQUIRE(outcome.has_value());
    CHECK(outcome->applied());
    CHECK(f.count() == 7);
}

TEST_CASE("applying revalidates rather than trusting", "[sim][command]") {
    // A command may have been queued before a load replaced the state it referred to, and
    // checking again costs a function call.
    Fixture f;
    Command hand_made;
    hand_made.target = 1;
    hand_made.type = kAdd;
    hand_made.payload = {std::byte{1}, std::byte{2}};  // two bytes; the handler wants one

    const auto status = f.queue.apply(f.world, hand_made);
    REQUIRE_FALSE(status.has_value());
    CHECK(f.count() == 0);
}

TEST_CASE("a stamped command keeps its recorded sequence", "[sim][command]") {
    // What a replay needs: the order in the recording is the thing being reproduced, so the
    // queue must not renumber it.
    Fixture f;
    Command recorded;
    recorded.target = 3;
    recorded.source = SourceId{4};
    recorded.sequence = 99;
    recorded.type = kAdd;
    recorded.payload = payload_of(5);

    REQUIRE(f.queue.submit_stamped(recorded).has_value());

    const auto taken = f.queue.drain(3);
    REQUIRE(taken.size() == 1);
    CHECK(taken[0].sequence == 99);
}

TEST_CASE("a stamped command moves the source's counter past it", "[sim][command]") {
    // Otherwise a command submitted after a replay could reuse a sequence number that
    // already appears in the log, and the order would stop being total.
    Fixture f;
    Command recorded;
    recorded.target = 1;
    recorded.source = SourceId{4};
    recorded.sequence = 50;
    recorded.type = kAdd;
    recorded.payload = payload_of(1);

    REQUIRE(f.queue.submit_stamped(recorded).has_value());
    CHECK(f.queue.next_sequence(SourceId{4}) == 51);

    REQUIRE(f.queue.submit(1, SourceId{4}, kAdd, payload_of(1)).has_value());
    const auto taken = f.queue.drain(1);
    REQUIRE(taken.size() == 2);
    CHECK(taken[0].sequence == 50);
    CHECK(taken[1].sequence == 51);
}

TEST_CASE("a stamped command with a bad payload is refused", "[sim][command]") {
    Fixture f;
    Command recorded;
    recorded.target = 1;
    recorded.type = kAdd;
    recorded.payload = {std::byte{1}, std::byte{2}};

    CHECK_FALSE(f.queue.submit_stamped(std::move(recorded)).has_value());
}

TEST_CASE("clearing empties the queue and its counters", "[sim][command]") {
    Fixture f;
    REQUIRE(f.queue.submit(1, SourceId{1}, kAdd, payload_of(1)).has_value());
    f.queue.clear();

    CHECK(f.queue.pending() == 0);
    CHECK(f.queue.next_sequence(SourceId{1}) == 0);
    // Handlers survive: they describe the application, not the state.
    CHECK(f.queue.has_handler(kAdd));
}
