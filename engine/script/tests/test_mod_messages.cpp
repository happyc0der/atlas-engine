// SPDX-License-Identifier: GPL-3.0-or-later
// `atlas_say`, through a real guest (ADR-0021).
//
// What a mod says is presentation, so none of this is about the simulation; what is checked is
// the boundary. A message arrives with the mod's own namespace in front of it, every malformed
// call is refused without disabling the mod, the budgets drop and count rather than grow, a bad
// pointer traps like any other, and nothing a mod says reaches the command queue.
#include <atlas/core/assert.hpp>
#include <atlas/script/atlas_mod.h>
#include <atlas/script/mod_host.hpp>
#include <atlas/script/runtime.hpp>
#include <atlas/simulation/turn_gate.hpp>

#include "wasm_builder.hpp"
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using atlas::script::check_mod_table;
using atlas::script::mod_key_prefix;
using atlas::script::ModHost;
using atlas::script::ModHostConfig;
using atlas::script::Runtime;
using atlas::sim::CommandQueue;
using atlas::sim::TurnGate;
namespace wasm = atlas::script::test;

namespace {

const bool kMainThreadMarkedForMessages = [] {
    atlas::mark_main_thread();
    return true;
}();

/// Where the arguments sit in the guest's memory, past any key a test writes at offset zero.
constexpr std::int32_t kArgsOffset = 64;

/// Memory holding `key` at offset 0 and `args` as little-endian 64-bit integers at kArgsOffset.
[[nodiscard]] wasm::Bytes memory_with(std::string_view key, const std::vector<std::int64_t>& args) {
    wasm::Bytes data(kArgsOffset + (8 * args.size()), 0);
    for (std::size_t i = 0; i < key.size(); ++i) {
        data[i] = static_cast<std::uint8_t>(key[i]);
    }
    for (std::size_t a = 0; a < args.size(); ++a) {
        const auto raw = static_cast<std::uint64_t>(args[a]);
        for (std::size_t b = 0; b < 8; ++b) {
            data[kArgsOffset + (8 * a) + b] = static_cast<std::uint8_t>((raw >> (8 * b)) & 0xFFU);
        }
    }
    return data;
}

/// One `atlas_say(0, key_len, args_offset, arg_count)` call, its result dropped.
void say(wasm::Bytes& body, std::int32_t key_len, std::int32_t args_offset,
         std::int32_t arg_count) {
    for (const std::int32_t value : {0, key_len, args_offset, arg_count}) {
        body.push_back(wasm::kOpI32Const);
        wasm::put_sleb(body, value);
    }
    body.push_back(wasm::kOpCall);
    wasm::put_uleb(body, 0);
    body.push_back(wasm::kOpDrop);
}

struct Fixture {
    Runtime runtime;
    CommandQueue queue;
    TurnGate gate;

    Fixture() : runtime(make()) {}

    [[nodiscard]] static Runtime make() {
        auto created = Runtime::create();
        REQUIRE(created.has_value());
        return *std::move(created);
    }

    /// A mod whose tick body is `body`, over memory `data`, with `atlas_say` as import 0.
    [[nodiscard]] std::unique_ptr<ModHost> host(wasm::Bytes data, wasm::Bytes body,
                                                std::string_view name = "herald.wasm",
                                                const ModHostConfig& config = {}) {
        wasm::ModuleSpec spec;
        spec.imports = {wasm::imports::say()};
        spec.data = std::move(data);
        spec.tick_body = std::move(body);
        auto made = ModHost::create(runtime, 0, wasm::as_bytes(wasm::build(spec)), name, config);
        REQUIRE(made.has_value());
        REQUIRE((*made)->start().has_value());
        return *std::move(made);
    }
};

}  // namespace

TEST_CASE("a mod's key namespace is its name without the extension", "[script][messages]") {
    CHECK(mod_key_prefix("herald.wasm") == "mod.herald.");
    CHECK(mod_key_prefix("herald") == "mod.herald.");
    // Only a trailing extension is removed, and a name that is nothing but one is kept.
    CHECK(mod_key_prefix("a.wasm.b") == "mod.a.wasm.b.");
    CHECK(mod_key_prefix(".wasm") == "mod..wasm.");
}

TEST_CASE("a mod's table may name only its own keys", "[script][messages]") {
    const atlas::assets::ImportedStringTable own{
        .locale = "en",
        .strings = {{"mod.herald.greeting", "Hello {0}"}, {"mod.herald.farewell", "Goodbye"}}};
    CHECK(check_mod_table(own, "herald.wasm").has_value());

    for (const std::string key :
         {"ui.pause", "mod.other.greeting", "mod.herald", "mod.herald.", "mod.heraldx.greeting"}) {
        INFO(key);
        const atlas::assets::ImportedStringTable foreign{.locale = "en", .strings = {{key, "x"}}};
        const auto refused = check_mod_table(foreign, "herald.wasm");
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code() == atlas::ErrorCode::PermissionDenied);
    }
}

TEST_CASE("a message arrives in the mod's own namespace, with its integers", "[script][messages]") {
    Fixture f;
    wasm::Bytes body;
    say(body, 8, kArgsOffset, 2);  // "greeting", 7, -3
    auto host = f.host(memory_with("greeting", {7, -3}), body);

    REQUIRE(host->poll(12, f.queue, f.gate).has_value());
    const auto messages = host->take_messages();
    REQUIRE(messages.size() == 1);
    CHECK(messages[0].key == "mod.herald.greeting");
    CHECK(messages[0].tick == 12);
    REQUIRE(messages[0].arguments().size() == 2);
    CHECK(messages[0].arguments()[0] == 7);
    CHECK(messages[0].arguments()[1] == -3);
    CHECK(host->stats().messages_said == 1);

    // Taking empties the queue, and nothing a mod says goes near the simulation.
    CHECK(host->take_messages().empty());
    CHECK(f.queue.pending() == 0);
}

TEST_CASE("a malformed call is refused and the mod carries on", "[script][messages]") {
    Fixture f;
    // Memory: "Bad..key" at 0; a valid "ok" is written by the test at offset 32.
    wasm::Bytes data = memory_with("Bad..key", {1, 2, 3, 4, 5});
    data[32] = 'o';
    data[33] = 'k';

    wasm::Bytes body;
    say(body, 0, kArgsOffset, 0);                          // empty key
    say(body, 3, kArgsOffset, 0);                          // "Bad": upper case
    say(body, 8, kArgsOffset, 0);                          // "Bad..key": and an empty segment
    say(body, ATLAS_MOD_MAX_SAY_KEY + 1, kArgsOffset, 0);  // too long
    // A valid key with too many and with a negative number of arguments. The key is at 32, so
    // these calls are written out by hand rather than through `say`.
    for (const std::int32_t count : {ATLAS_MOD_MAX_SAY_ARGS + 1, -1}) {
        for (const std::int32_t value : {32, 2, kArgsOffset, count}) {
            body.push_back(wasm::kOpI32Const);
            wasm::put_sleb(body, value);
        }
        body.push_back(wasm::kOpCall);
        wasm::put_uleb(body, 0);
        body.push_back(wasm::kOpDrop);
    }
    // And one that is fine, so "carries on" is shown rather than assumed.
    for (const std::int32_t value : {32, 2, kArgsOffset, ATLAS_MOD_MAX_SAY_ARGS}) {
        body.push_back(wasm::kOpI32Const);
        wasm::put_sleb(body, value);
    }
    body.push_back(wasm::kOpCall);
    wasm::put_uleb(body, 0);
    body.push_back(wasm::kOpDrop);

    auto host = f.host(data, body);
    REQUIRE(host->poll(0, f.queue, f.gate).has_value());
    CHECK_FALSE(host->disabled());
    const auto messages = host->take_messages();
    REQUIRE(messages.size() == 1);
    CHECK(messages[0].key == "mod.herald.ok");
    CHECK(messages[0].arguments().size() == ATLAS_MOD_MAX_SAY_ARGS);
    CHECK(messages[0].arguments()[3] == 4);
}

TEST_CASE("a mod's messages are bounded per tick, and the rest are dropped and counted",
          "[script][messages]") {
    Fixture f;
    wasm::Bytes body;
    for (int i = 0; i < 6; ++i) {
        say(body, 2, kArgsOffset, 0);
    }
    auto host = f.host(memory_with("hi", {}), body, "herald.wasm", {.max_messages_per_tick = 2});

    REQUIRE(host->poll(0, f.queue, f.gate).has_value());
    CHECK(host->take_messages().size() == 2);
    CHECK(host->stats().messages_dropped == 4);
    // A new tick is a new budget.
    REQUIRE(host->poll(1, f.queue, f.gate).has_value());
    CHECK(host->take_messages().size() == 2);
    CHECK_FALSE(host->disabled());
}

TEST_CASE("an application that never drains cannot be made to hold more than its bound",
          "[script][messages]") {
    Fixture f;
    wasm::Bytes body;
    say(body, 2, kArgsOffset, 0);
    say(body, 2, kArgsOffset, 0);
    auto host = f.host(memory_with("hi", {}), body, "herald.wasm", {.max_queued_messages = 3});

    for (atlas::Tick tick = 0; tick < 5; ++tick) {
        REQUIRE(host->poll(tick, f.queue, f.gate).has_value());
    }
    CHECK(host->take_messages().size() == 3);
    CHECK(host->stats().messages_said == 3);
    CHECK(host->stats().messages_dropped == 7);
}

TEST_CASE("arguments outside the mod's memory trap, like any bad pointer", "[script][messages]") {
    // ADR-0015's failure policy: a bad pointer disables the mod. The count is of integers and
    // the host checks the range itself, so this is the case that proves it does.
    Fixture f;
    wasm::Bytes body;
    say(body, 2, 65'536 - 8, 2);  // the second integer runs past the single page
    auto host = f.host(memory_with("hi", {}), body);

    const auto report = host->poll(0, f.queue, f.gate);
    REQUIRE(report.has_value());
    CHECK(host->disabled());
    CHECK(host->take_messages().empty());
}
