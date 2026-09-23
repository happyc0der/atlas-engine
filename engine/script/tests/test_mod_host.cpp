// SPDX-License-Identifier: GPL-3.0-or-later
// The guest interface, exercised through a real guest.
//
// `atlas_mod.h` is compiled on both sides but it cannot check that the host's implementations
// match its declarations — the host's take an execution environment the guest never sees. So
// these are the tests that enforce it: every import is called from inside a module built byte
// by byte, and what comes back is checked against what the host was told.
#include <atlas/core/assert.hpp>
#include <atlas/script/atlas_mod.h>
#include <atlas/script/mod_host.hpp>
#include <atlas/script/runtime.hpp>
#include <atlas/simulation/turn_gate.hpp>

#include "wasm_builder.hpp"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

using atlas::ErrorCode;
using atlas::script::ModHost;
using atlas::script::ModHostConfig;
using atlas::script::ModView;
using atlas::script::Runtime;
using atlas::sim::CommandHandler;
using atlas::sim::CommandQueue;
using atlas::sim::CommandType;
using atlas::sim::SourceId;
using atlas::sim::TurnGate;
namespace wasm = atlas::script::test;

namespace {

const bool kMainThreadMarkedForHost = [] {
    atlas::mark_main_thread();
    return true;
}();

const CommandType kPoke = atlas::sim::command_type("poke");

/// Accepts a payload of exactly three bytes, and records what it was given.
[[nodiscard]] CommandHandler poke_handler() {
    CommandHandler handler;
    handler.validate = [](std::span<const std::byte> payload) -> atlas::Status {
        if (payload.size() != 3) {
            return std::unexpected(
                atlas::Error(ErrorCode::MalformedData, "expected exactly three bytes"));
        }
        return atlas::ok();
    };
    handler.apply = [](atlas::sim::World&, const atlas::sim::ApplyContext&,
                       std::span<const std::byte>) { return atlas::ok(); };
    return handler;
}

/// A runtime, a queue with one registered type, and a gate nobody should touch.
struct Fixture {
    Runtime runtime;
    CommandQueue queue;
    TurnGate gate;

    Fixture() : runtime(make()) { REQUIRE(queue.register_handler(kPoke, poke_handler())); }

    [[nodiscard]] static Runtime make() {
        auto created = Runtime::create();
        REQUIRE(created.has_value());
        return *std::move(created);
    }

    [[nodiscard]] atlas::Result<std::unique_ptr<ModHost>> host(const wasm::ModuleSpec& spec,
                                                               std::string_view name = "test",
                                                               const ModHostConfig& config = {},
                                                               std::uint32_t index = 0) {
        return ModHost::create(runtime, index, wasm::as_bytes(wasm::build(spec)), name, config);
    }
};

/// `mod_tick` body: call import 0 with no arguments and throw the result away.
[[nodiscard]] wasm::Bytes call_and_drop(std::uint32_t import_index) {
    wasm::Bytes body;
    body.push_back(wasm::kOpCall);
    wasm::put_uleb(body, import_index);
    body.push_back(wasm::kOpDrop);
    return body;
}

}  // namespace

TEST_CASE("a mod may import what the host provides and nothing else", "[script][host]") {
    Fixture f;

    SECTION("a provided import is accepted") {
        wasm::ModuleSpec spec;
        spec.imports = {wasm::imports::tick()};
        spec.tick_body = call_and_drop(0);
        const auto host = f.host(spec, "ticker");
        REQUIRE(host.has_value());
    }

    SECTION("one that only looks provided is refused") {
        // Spelled almost right, which is the realistic mistake and the one a lazily-resolved
        // import would turn into a trap deep inside a guest instead of a refusal at load.
        wasm::ModuleSpec spec;
        spec.imports = {{.field = "atlas_ticks", .results = {wasm::kValI64}}};
        spec.tick_body = call_and_drop(0);
        const auto refused = f.host(spec, "typo");
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code() == ErrorCode::ModImportRefused);
    }
}

TEST_CASE("a mod's identifier is its own and is not a peer's", "[script][host]") {
    Fixture f;
    const auto host = f.host({}, "quiet", {}, 3);
    REQUIRE(host.has_value());

    const auto id = (*host)->id();
    CHECK(atlas::sim::is_mod_source(id));
    CHECK(id == atlas::sim::mod_source(3));
    // The whole point of the bit: a mod can never be mistaken for a peer, and peers come from
    // link indices which are small and dense.
    CHECK_FALSE(atlas::sim::is_mod_source(SourceId::Local));
    CHECK_FALSE(atlas::sim::is_mod_source(SourceId{15}));
}

TEST_CASE("atlas_tick gives the tick being decided", "[script][host]") {
    // Checked by having the guest submit the tick it was told, because a mod's only way to
    // report anything is to submit a command — which is the point of the design and also makes
    // this test exercise two imports rather than assert on one in isolation.
    Fixture f;

    wasm::ModuleSpec spec;
    spec.imports = {wasm::imports::tick(), wasm::imports::submit()};
    spec.data = {0x00, 0x00, 0x00};  // three bytes at offset 0, which is what poke accepts

    wasm::Bytes body;
    // store the low word of atlas_tick() at offset 0, then submit those three bytes
    body.push_back(wasm::kOpI32Const);
    wasm::put_sleb(body, 0);
    body.push_back(wasm::kOpCall);
    wasm::put_uleb(body, 0);  // atlas_tick -> i64
    body.push_back(wasm::kOpI32WrapI64);
    wasm::put_all(body, {wasm::kOpI32Store, 0x02, 0x00});
    body.push_back(wasm::kOpI32Const);
    wasm::put_sleb(body, static_cast<std::int64_t>(static_cast<std::uint32_t>(kPoke)));
    body.push_back(wasm::kOpI32Const);
    wasm::put_sleb(body, 0);  // payload pointer
    body.push_back(wasm::kOpI32Const);
    wasm::put_sleb(body, 3);  // payload length
    body.push_back(wasm::kOpCall);
    wasm::put_uleb(body, 1);  // atlas_submit
    body.push_back(wasm::kOpDrop);
    spec.tick_body = body;

    auto host = f.host(spec, "reporter", {.input_delay = 2});
    REQUIRE(host.has_value());
    REQUIRE((*host)->start().has_value());

    const auto report = (*host)->poll(41, f.queue, f.gate);
    REQUIRE(report.has_value());
    CHECK(report->commands_submitted == 1);

    // Stamped for the tick the host chose, not for the one the guest was deciding.
    CHECK(report->highest_target == 43);

    const auto drained = f.queue.drain(43);
    REQUIRE(drained.size() == 1);
    CHECK(drained[0].source == (*host)->id());
    CHECK(drained[0].target == 43);
    // The three bytes the guest wrote are the tick it was told, little-endian.
    REQUIRE(drained[0].payload.size() == 3);
    CHECK(std::to_integer<int>(drained[0].payload[0]) == 41);
}

TEST_CASE("a mod cannot submit under another source", "[script][host]") {
    // There is nothing to test at the call, because `atlas_submit` has no source parameter —
    // so what is checked is that every command the queue received carries the mod's own
    // identifier. That is the property ADR-0015 D6 makes unrepresentable rather than forbidden.
    Fixture f;

    wasm::ModuleSpec spec;
    spec.imports = {wasm::imports::submit()};
    spec.data = {0x01, 0x02, 0x03};

    wasm::Bytes body;
    for (int i = 0; i < 4; ++i) {
        body.push_back(wasm::kOpI32Const);
        wasm::put_sleb(body, static_cast<std::int64_t>(static_cast<std::uint32_t>(kPoke)));
        body.push_back(wasm::kOpI32Const);
        wasm::put_sleb(body, 0);
        body.push_back(wasm::kOpI32Const);
        wasm::put_sleb(body, 3);
        body.push_back(wasm::kOpCall);
        wasm::put_uleb(body, 0);
        body.push_back(wasm::kOpDrop);
    }
    spec.tick_body = body;

    auto host = f.host(spec, "submitter", {}, 5);
    REQUIRE(host.has_value());
    REQUIRE((*host)->start().has_value());
    REQUIRE((*host)->poll(0, f.queue, f.gate).has_value());

    const auto drained = f.queue.drain(1);
    REQUIRE(drained.size() == 4);
    for (const auto& command : drained) {
        CHECK(command.source == atlas::sim::mod_source(5));
        CHECK(atlas::sim::is_mod_source(command.source));
    }
    // And the sequence numbers are the queue's, so two mods cannot collide on (source, sequence)
    // however many commands either submits.
    CHECK(drained[0].sequence == 0);
    CHECK(drained[3].sequence == 3);
}

TEST_CASE("a mod's command budget is a ceiling it cannot pass", "[script][host]") {
    Fixture f;

    wasm::ModuleSpec spec;
    spec.imports = {wasm::imports::submit()};
    spec.data = {0x01, 0x02, 0x03};

    wasm::Bytes body;
    for (int i = 0; i < 6; ++i) {
        body.push_back(wasm::kOpI32Const);
        wasm::put_sleb(body, static_cast<std::int64_t>(static_cast<std::uint32_t>(kPoke)));
        body.push_back(wasm::kOpI32Const);
        wasm::put_sleb(body, 0);
        body.push_back(wasm::kOpI32Const);
        wasm::put_sleb(body, 3);
        body.push_back(wasm::kOpCall);
        wasm::put_uleb(body, 0);
        body.push_back(wasm::kOpDrop);
    }
    spec.tick_body = body;

    auto host = f.host(spec, "eager", {.max_commands_per_tick = 2});
    REQUIRE(host.has_value());
    REQUIRE((*host)->start().has_value());

    const auto report = (*host)->poll(0, f.queue, f.gate);
    REQUIRE(report.has_value());
    CHECK(report->commands_submitted == 2);
    CHECK(report->commands_refused == 4);
    // Refused, not fatal: a mod that asks for too much gets less, and carries on.
    CHECK_FALSE((*host)->disabled());
    CHECK(f.queue.pending() == 2);
}

TEST_CASE("a command the queue will not take is refused rather than applied", "[script][host]") {
    Fixture f;

    wasm::ModuleSpec spec;
    spec.imports = {wasm::imports::submit()};
    spec.data = {0x01, 0x02, 0x03, 0x04};

    wasm::Bytes body;
    // Four bytes, where the handler validates exactly three.
    body.push_back(wasm::kOpI32Const);
    wasm::put_sleb(body, static_cast<std::int64_t>(static_cast<std::uint32_t>(kPoke)));
    body.push_back(wasm::kOpI32Const);
    wasm::put_sleb(body, 0);
    body.push_back(wasm::kOpI32Const);
    wasm::put_sleb(body, 4);
    body.push_back(wasm::kOpCall);
    wasm::put_uleb(body, 0);
    body.push_back(wasm::kOpDrop);
    spec.tick_body = body;

    auto host = f.host(spec, "malformed");
    REQUIRE(host.has_value());
    REQUIRE((*host)->start().has_value());

    const auto report = (*host)->poll(0, f.queue, f.gate);
    REQUIRE(report.has_value());
    CHECK(report->commands_submitted == 0);
    CHECK(report->commands_refused == 1);
    CHECK(f.queue.pending() == 0);
}

TEST_CASE("atlas_command_type answers only for types somebody handles", "[script][host]") {
    Fixture f;

    const auto build_lookup = [](std::string_view name) {
        wasm::ModuleSpec spec;
        spec.imports = {wasm::imports::command_type(), wasm::imports::submit()};
        spec.data.assign(name.begin(), name.end());
        spec.data.resize(std::max<std::size_t>(spec.data.size(), 3));

        wasm::Bytes body;
        // submit(atlas_command_type(name, len), 0, 3)
        body.push_back(wasm::kOpI32Const);
        wasm::put_sleb(body, 0);
        body.push_back(wasm::kOpI32Const);
        wasm::put_sleb(body, static_cast<std::int64_t>(name.size()));
        body.push_back(wasm::kOpCall);
        wasm::put_uleb(body, 0);
        body.push_back(wasm::kOpI32Const);
        wasm::put_sleb(body, 0);
        body.push_back(wasm::kOpI32Const);
        wasm::put_sleb(body, 3);
        body.push_back(wasm::kOpCall);
        wasm::put_uleb(body, 1);
        body.push_back(wasm::kOpDrop);
        spec.tick_body = body;
        return spec;
    };

    SECTION("a known name resolves and the command lands") {
        auto host = f.host(build_lookup("poke"), "knows");
        REQUIRE(host.has_value());
        REQUIRE((*host)->start().has_value());
        const auto report = (*host)->poll(0, f.queue, f.gate);
        REQUIRE(report.has_value());
        CHECK(report->commands_submitted == 1);
    }

    SECTION("an unknown name resolves to nothing and submits nothing") {
        // A mod written against a game that is not running should do nothing, rather than hash
        // a name into some other game's command type and submit that.
        auto host = f.host(build_lookup("prod"), "guesses");
        REQUIRE(host.has_value());
        REQUIRE((*host)->start().has_value());
        const auto report = (*host)->poll(0, f.queue, f.gate);
        REQUIRE(report.has_value());
        CHECK(report->commands_submitted == 0);
        CHECK(report->commands_refused == 1);
    }
}

TEST_CASE("views are what the application published, and nothing beside them", "[script][host]") {
    Fixture f;

    wasm::ModuleSpec spec;
    spec.imports = {wasm::imports::view_size(), wasm::imports::submit()};
    spec.data = {0x00, 0x00, 0x00};

    wasm::Bytes body;
    // store atlas_view_size(<arg>) at offset 0 for three different view indices, submitting each
    for (const std::int64_t view : {std::int64_t{0}, std::int64_t{1}, std::int64_t{7}}) {
        body.push_back(wasm::kOpI32Const);
        wasm::put_sleb(body, 0);
        body.push_back(wasm::kOpI32Const);
        wasm::put_sleb(body, view);
        body.push_back(wasm::kOpCall);
        wasm::put_uleb(body, 0);  // atlas_view_size
        wasm::put_all(body, {wasm::kOpI32Store, 0x02, 0x00});
        body.push_back(wasm::kOpI32Const);
        wasm::put_sleb(body, static_cast<std::int64_t>(static_cast<std::uint32_t>(kPoke)));
        body.push_back(wasm::kOpI32Const);
        wasm::put_sleb(body, 0);
        body.push_back(wasm::kOpI32Const);
        wasm::put_sleb(body, 3);
        body.push_back(wasm::kOpCall);
        wasm::put_uleb(body, 1);
        body.push_back(wasm::kOpDrop);
    }
    spec.tick_body = body;

    auto host = f.host(spec, "reader");
    REQUIRE(host.has_value());
    REQUIRE((*host)->start().has_value());

    const std::vector<std::byte> first(5, std::byte{0xAA});
    const std::vector<std::byte> second(9, std::byte{0xBB});
    const std::array<ModView, 2> views{
        ModView{.name = "first", .bytes = first},
        ModView{.name = "second", .bytes = second},
    };
    (*host)->set_views(views);

    REQUIRE((*host)->poll(0, f.queue, f.gate).has_value());
    const auto drained = f.queue.drain(1);
    REQUIRE(drained.size() == 3);
    CHECK(std::to_integer<int>(drained[0].payload[0]) == 5);
    CHECK(std::to_integer<int>(drained[1].payload[0]) == 9);
    // View seven does not exist, and asking about it is a negative number rather than a trap or
    // a read of whatever happened to be next in memory.
    CHECK(std::to_integer<int>(drained[2].payload[0]) == 0xFF);  // -1, low byte
}

TEST_CASE("atlas_random draws the same numbers on every machine that runs the mod",
          "[script][host]") {
    // The property lockstep needs: two hosts with the same seed, the same mod name and the same
    // tick produce identical commands. A mod using a generator of its own would fail this on
    // the first draw, which is why the ABI has no way to make one.
    Fixture f;

    wasm::ModuleSpec spec;
    spec.imports = {wasm::imports::random(), wasm::imports::submit()};
    spec.data = {0x00, 0x00, 0x00};

    wasm::Bytes body;
    for (int i = 0; i < 3; ++i) {
        body.push_back(wasm::kOpI32Const);
        wasm::put_sleb(body, 0);
        body.push_back(wasm::kOpI32Const);
        wasm::put_sleb(body, 0);  // stream 0
        body.push_back(wasm::kOpI64Const);
        wasm::put_sleb(body, 251);  // bound
        body.push_back(wasm::kOpCall);
        wasm::put_uleb(body, 0);  // atlas_random -> i64
        body.push_back(wasm::kOpI32WrapI64);
        wasm::put_all(body, {wasm::kOpI32Store, 0x02, 0x00});
        body.push_back(wasm::kOpI32Const);
        wasm::put_sleb(body, static_cast<std::int64_t>(static_cast<std::uint32_t>(kPoke)));
        body.push_back(wasm::kOpI32Const);
        wasm::put_sleb(body, 0);
        body.push_back(wasm::kOpI32Const);
        wasm::put_sleb(body, 3);
        body.push_back(wasm::kOpCall);
        wasm::put_uleb(body, 1);
        body.push_back(wasm::kOpDrop);
    }
    spec.tick_body = body;

    const auto run = [&](CommandQueue& queue) {
        auto host = ModHost::create(f.runtime, 0, wasm::as_bytes(wasm::build(spec)), "dice",
                                    {.seed = 12345});
        REQUIRE(host.has_value());
        REQUIRE((*host)->start().has_value());
        TurnGate gate;
        REQUIRE((*host)->poll(7, queue, gate).has_value());
        std::vector<int> drawn;
        for (const auto& command : queue.drain(8)) {
            drawn.push_back(std::to_integer<int>(command.payload[0]));
        }
        return drawn;
    };

    CommandQueue one;
    CommandQueue other;
    REQUIRE(one.register_handler(kPoke, poke_handler()));
    REQUIRE(other.register_handler(kPoke, poke_handler()));

    const auto first = run(one);
    const auto second = run(other);
    REQUIRE(first.size() == 3);
    CHECK(first == second);
    // Three draws from one stream advance it, rather than repeating the first value — which is
    // what a freshly constructed RngStream per call would have done.
    const bool all_the_same = (first[0] == first[1]) && (first[1] == first[2]);
    CHECK_FALSE(all_the_same);
}

TEST_CASE("a mod never marks the turn gate", "[script][host]") {
    // Mods take no turns: every peer runs the same mods and each produces the same commands
    // locally, so there is nothing to announce. Stated as a test because a host that marked the
    // gate would stall every peer that had not loaded the same mod, and nothing else would
    // notice until two machines disagreed.
    Fixture f;

    const std::array<SourceId, 2> peers{SourceId{0}, SourceId{1}};
    REQUIRE(f.gate.expect_sources(peers));
    const auto before = f.gate.ready_horizon();

    wasm::ModuleSpec spec;
    spec.imports = {wasm::imports::tick()};
    spec.tick_body = call_and_drop(0);

    auto host = f.host(spec, "silent");
    REQUIRE(host.has_value());
    REQUIRE((*host)->start().has_value());

    const auto report = (*host)->poll(0, f.queue, f.gate);
    REQUIRE(report.has_value());
    CHECK(report->turns_marked == 0);
    CHECK(f.gate.ready_horizon() == before);
    CHECK_FALSE(f.gate.ready(0));

    // And again with the gate expecting the mod itself.
    //
    // The assertions above pass whether or not the host marks, because a gate refuses a source
    // it was never told to expect — so on their own they check that the gate is well behaved
    // rather than that the host is. A mutation making the host mark survived exactly that gap.
    // Putting the mod in the expectation set removes the gate's protection and leaves only the
    // host's restraint, which is the thing this case is named after.
    TurnGate expecting_the_mod;
    const std::array<SourceId, 2> with_mod{SourceId{0}, (*host)->id()};
    REQUIRE(expecting_the_mod.expect_sources(with_mod));

    REQUIRE((*host)->poll(0, f.queue, expecting_the_mod).has_value());
    std::vector<SourceId> waiting;
    expecting_the_mod.waiting_on(0, waiting);
    CHECK(waiting.size() == 2);
    CHECK(std::ranges::find(waiting, (*host)->id()) != waiting.end());
}

TEST_CASE("a mod that breaks mid-tick is reported closed rather than as an error",
          "[script][host]") {
    // The failure policy's other half. An error out of `poll` would stop the driver polling
    // every other source, which is exactly what one bad mod must not be able to do.
    Fixture f;

    wasm::ModuleSpec spec;
    spec.behaviour = wasm::Behaviour::TickLoopsForever;

    auto host = ModHost::create(f.runtime, 0, wasm::as_bytes(wasm::build(spec)), "spinner", {},
                                {.max_instructions_per_tick = 100'000});
    REQUIRE(host.has_value());
    REQUIRE((*host)->start().has_value());

    const auto report = (*host)->poll(0, f.queue, f.gate);
    REQUIRE(report.has_value());
    CHECK(report->closed);
    CHECK((*host)->disabled());

    // Polled again it succeeds and does nothing, because that is what a mod which is no longer
    // there looks like from the simulation's side.
    const auto after = (*host)->poll(1, f.queue, f.gate);
    REQUIRE(after.has_value());
    CHECK(after->commands_submitted == 0);
}

TEST_CASE("an input delay of zero is refused", "[script][host]") {
    Fixture f;
    const auto refused = f.host({}, "immediate", {.input_delay = 0});
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code() == ErrorCode::InvalidArgument);
}

TEST_CASE("the committed demonstration mod loads and does what it says", "[script][host]") {
    // The real artifact, read from the tree rather than built in the test. Everything else in
    // this file checks the host against modules written for the occasion; this checks that the
    // bytes actually shipped are a mod, which is the only way `assets/mods/synthetic.wasm` is
    // covered by anything faster than an integration case.
    //
    // The command type is registered here under the same name the mod looks up. Its meaning is
    // the lab's; what the engine needs to know is that a five-byte payload arrives.
    Fixture f;
    const auto set_color = atlas::sim::command_type("set_color_index");
    CommandHandler handler;
    handler.validate = [](std::span<const std::byte> payload) -> atlas::Status {
        if (payload.size() != 5) {
            return std::unexpected(atlas::Error(ErrorCode::MalformedData, "expected five bytes"));
        }
        return atlas::ok();
    };
    handler.apply = [](atlas::sim::World&, const atlas::sim::ApplyContext&,
                       std::span<const std::byte>) { return atlas::ok(); };
    REQUIRE(f.queue.register_handler(set_color, std::move(handler)));

    std::ifstream file("assets/mods/synthetic.wasm", std::ios::binary);
    REQUIRE(file.is_open());
    const std::vector<char> raw((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
    REQUIRE_FALSE(raw.empty());
    std::vector<std::byte> bytes;
    bytes.reserve(raw.size());
    for (const char value : raw) {
        bytes.push_back(static_cast<std::byte>(value));
    }

    auto host = ModHost::create(f.runtime, 0, bytes, "synthetic", {.seed = 99});
    REQUIRE(host.has_value());
    REQUIRE((*host)->start().has_value());

    // A grid of 64 cells, as four little-endian bytes: the mod has no idea how big the world is
    // until this says so, which is what stops it from being written against one grid.
    const std::array<std::byte, 4> layout{std::byte{64}, std::byte{0}, std::byte{0}, std::byte{0}};
    const std::array<ModView, 1> views{ModView{.name = "layout", .bytes = layout}};
    (*host)->set_views(views);

    for (atlas::Tick tick = 0; tick < 4; ++tick) {
        const auto report = (*host)->poll(tick, f.queue, f.gate);
        REQUIRE(report.has_value());
        CHECK(report->commands_submitted == 1);
    }
    CHECK_FALSE((*host)->disabled());

    const auto drained = f.queue.drain(8);
    REQUIRE(drained.size() == 4);
    for (const auto& command : drained) {
        CHECK(command.source == atlas::sim::mod_source(0));
        REQUIRE(command.payload.size() == 5);
        // Inside the grid it was told about, and inside the colour range it was built with.
        const auto cell = static_cast<std::uint32_t>(command.payload[0]) |
                          (static_cast<std::uint32_t>(command.payload[1]) << 8U) |
                          (static_cast<std::uint32_t>(command.payload[2]) << 16U) |
                          (static_cast<std::uint32_t>(command.payload[3]) << 24U);
        CHECK(cell < 64);
        CHECK(std::to_integer<int>(command.payload[4]) < 8);
    }
}

TEST_CASE("the interface has no clock, and that is enforced rather than documented",
          "[script][host]") {
    // The guarantee `atlas_mod.h` opens with. A mod that can read a clock decides differently on
    // a slower machine, which under lockstep is a divergence — so the import exists only to
    // demonstrate that, and only for a host that asked for it by a name with "unsafe" in it.
    //
    // **This case fails if anybody ever adds a clock to the interface for real**, which is the
    // reason it is written as two halves rather than one.
    wasm::ModuleSpec spec;
    spec.imports = {{.field = "atlas_debug_clock_ns", .results = {wasm::kValI64}}};
    spec.tick_body = call_and_drop(0);
    const auto bytes = wasm::as_bytes(wasm::build(spec));

    SECTION("an ordinary runtime refuses it") {
        auto runtime = Runtime::create();
        REQUIRE(runtime.has_value());
        const auto refused = ModHost::create(*runtime, 0, bytes, "clock");
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code() == ErrorCode::ModImportRefused);
        CHECK(refused.error().message().contains("atlas_debug_clock_ns"));
    }

    SECTION("a runtime that opted in provides it") {
        // Proving the refusal above is the flag's doing and not a misspelling: the same bytes,
        // the same loader, one setting different.
        auto runtime = Runtime::create({.unsafe_debug_imports = true});
        REQUIRE(runtime.has_value());
        const auto loaded = ModHost::create(*runtime, 0, bytes, "clock");
        REQUIRE(loaded.has_value());
    }

    SECTION("and every other import is unaffected either way") {
        // A flag that widened the list by more than one would pass both halves above.
        auto runtime = Runtime::create({.unsafe_debug_imports = true});
        REQUIRE(runtime.has_value());
        wasm::ModuleSpec invented;
        // Shaped so the module itself validates — no arguments, one result for the
        // `drop` to take — because a module refused for being malformed would prove
        // nothing about the import list.
        invented.imports = {{.field = "atlas_open_file", .results = {wasm::kValI32}}};
        invented.tick_body = call_and_drop(0);
        const auto refused =
            ModHost::create(*runtime, 0, wasm::as_bytes(wasm::build(invented)), "hopeful");
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code() == ErrorCode::ModImportRefused);
    }
}
