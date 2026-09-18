// SPDX-License-Identifier: GPL-3.0-or-later
// What the loader refuses, and what happens when a mod misbehaves anyway.
//
// A mod is untrusted input, so this file is shaped like `test_wav.cpp` and the replay's
// truncation sweep rather than like a feature test: most of it is inputs that must be turned
// away, each built byte by byte so that what is being refused is visible in the test.
//
// It carries the "unit" label, which is what the address sanitiser preset runs — and ASan is
// the whole point of a suite like this, because "refused" and "refused without reading past the
// end of the buffer" are different claims.
#include <atlas/core/assert.hpp>
#include <atlas/script/mod.hpp>
#include <atlas/script/runtime.hpp>

#include "wasm_builder.hpp"
#include <catch2/catch_test_macros.hpp>

#include <vector>

using atlas::ErrorCode;
using atlas::script::Mod;
using atlas::script::ModLimits;
using atlas::script::Runtime;
using atlas::script::test::Behaviour;
using atlas::script::test::ModuleSpec;

namespace {

const bool kMainThreadMarkedForScript = [] {
    atlas::mark_main_thread();
    return true;
}();

/// One runtime and the mods loaded against it, torn down in the right order.
///
/// The order is the point: WAMR's allocator belongs to the runtime, so a mod outliving one
/// would free memory through an allocator that no longer exists. Holding both here means every
/// case gets it right without having to remember to.
struct Fixture {
    Runtime runtime;

    Fixture() : runtime(make()) {}

    [[nodiscard]] static Runtime make() {
        auto created = Runtime::create();
        REQUIRE(created.has_value());
        return *std::move(created);
    }

    [[nodiscard]] atlas::Result<Mod> load(const ModuleSpec& spec, std::string_view name = "test",
                                          const ModLimits& limits = {}) {
        return Mod::load(runtime, atlas::script::test::as_bytes(atlas::script::test::build(spec)),
                         name, limits);
    }
};

}  // namespace

TEST_CASE("a well-formed mod loads, initialises and ticks", "[script][mod]") {
    // The control. Without it every refusal below could be the builder producing rubbish rather
    // than the loader doing its job, and a hostile-input suite whose valid case does not work is
    // a suite that proves nothing.
    Fixture f;
    auto mod = f.load({});
    REQUIRE(mod.has_value());
    CHECK_FALSE(mod->disabled());

    REQUIRE(mod->init().has_value());
    for (std::int64_t tick = 0; tick < 16; ++tick) {
        REQUIRE(mod->tick(tick).has_value());
    }
    CHECK_FALSE(mod->disabled());
    CHECK(mod->disabled_because().empty());
}

TEST_CASE("bytes that are not a module are refused", "[script][mod]") {
    Fixture f;

    SECTION("empty") {
        const auto refused = Mod::load(f.runtime, {}, "empty");
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code() == ErrorCode::ModInvalid);
    }

    SECTION("not WebAssembly at all") {
        const std::vector<std::byte> rubbish(64, std::byte{0xAB});
        const auto refused = Mod::load(f.runtime, rubbish, "rubbish");
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code() == ErrorCode::ModInvalid);
    }

    SECTION("the header alone, with no sections") {
        const std::vector<std::byte> header{std::byte{0x00}, std::byte{0x61}, std::byte{0x73},
                                            std::byte{0x6D}, std::byte{0x01}, std::byte{0x00},
                                            std::byte{0x00}, std::byte{0x00}};
        // A module with no sections is valid WebAssembly and exports nothing, so this is
        // refused for what it lacks rather than for what it is.
        const auto refused = Mod::load(f.runtime, header, "bare");
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code() == ErrorCode::ModExportMissing);
    }
}

TEST_CASE("a module truncated at any length is refused and never read past", "[script][mod]") {
    // The sweep. Every prefix of a valid module is fed in; none may be accepted and none may
    // read off the end, which is what running this under the address sanitiser checks.
    Fixture f;
    const auto whole = atlas::script::test::build({});

    for (std::size_t length = 1; length < whole.size(); ++length) {
        const std::vector<std::uint8_t> prefix(whole.begin(),
                                               whole.begin() + static_cast<std::ptrdiff_t>(length));
        INFO("truncated to " << length << " of " << whole.size());
        const auto refused =
            Mod::load(f.runtime, atlas::script::test::as_bytes(prefix), "truncated");
        REQUIRE_FALSE(refused.has_value());
    }
}

TEST_CASE("a module larger than the limit is refused before it is looked at", "[script][mod]") {
    Fixture f;
    ModLimits tiny;
    tiny.max_module_bytes = 32;

    const auto refused = f.load({}, "big", tiny);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code() == ErrorCode::OutOfRange);
    // The message says which limit and by how much, because a mod author who cannot see the
    // number cannot do anything about it.
    CHECK(refused.error().message().contains("32"));
}

TEST_CASE("an import the host does not provide is refused", "[script][mod]") {
    Fixture f;

    SECTION("from outside the atlas table") {
        // The one that matters. A module importing WASI would, on a runtime built with it,
        // receive a filesystem and a clock; this build has neither, but the refusal must not
        // depend on that — it is the import list that is the authority, not what happens to be
        // compiled in.
        ModuleSpec spec;
        spec.import_module = "wasi_snapshot_preview1";
        spec.import_field = "fd_write";
        const auto refused = f.load(spec, "wasi");
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code() == ErrorCode::ModImportRefused);
        CHECK(refused.error().message().contains("wasi_snapshot_preview1"));
    }

    SECTION("from inside it, but not offered") {
        ModuleSpec spec;
        spec.import_module = "atlas";
        spec.import_field = "atlas_clock_ns";
        const auto refused = f.load(spec, "clock");
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code() == ErrorCode::ModImportRefused);
    }
}

TEST_CASE("a module missing a required export is refused, and the message names them all",
          "[script][mod]") {
    Fixture f;

    SECTION("one missing") {
        ModuleSpec spec;
        spec.omit_exports = {"mod_tick"};
        const auto refused = f.load(spec, "no_tick");
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code() == ErrorCode::ModExportMissing);
        CHECK(refused.error().message().contains("mod_tick"));
    }

    SECTION("several missing are all named at once") {
        // Named together rather than one per attempt: somebody porting a mod wants the list,
        // and finding it an item at a time is four builds instead of one.
        ModuleSpec spec;
        spec.omit_exports = {"mod_init", "mod_shutdown"};
        const auto refused = f.load(spec, "half");
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message().contains("mod_init"));
        CHECK(refused.error().message().contains("mod_shutdown"));
    }

    SECTION("memory missing") {
        ModuleSpec spec;
        spec.omit_exports = {"memory"};
        const auto refused = f.load(spec, "no_memory");
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code() == ErrorCode::ModExportMissing);
    }
}

TEST_CASE("a module asking for more memory than the limit is refused", "[script][mod]") {
    Fixture f;
    ModLimits small;
    small.max_linear_memory_bytes = std::size_t{64} * 1024;  // one page

    SECTION("its initial size is over") {
        ModuleSpec spec;
        spec.memory_pages = 4;
        const auto refused = f.load(spec, "greedy", small);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code() == ErrorCode::OutOfRange);
        CHECK(refused.error().message().contains("an initial"));
    }

    SECTION("it starts inside the limit but may grow past it") {
        // The case that matters more, because it looks harmless at load. A module that may grow
        // past the ceiling is refused at the ceiling it declares, not at the one it starts on.
        ModuleSpec spec;
        spec.memory_pages = 1;
        spec.memory_max_pages = 64;
        const auto refused = f.load(spec, "patient", small);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code() == ErrorCode::OutOfRange);
        CHECK(refused.error().message().contains("a maximum"));
    }

    SECTION("it declares no maximum at all") {
        // Refused rather than capped: a mod that does not say how far it may grow has not been
        // written with a limit in mind, and a silent cap becomes a failed grow nobody can explain.
        ModuleSpec spec;
        spec.memory_without_maximum = true;
        const auto refused = f.load(spec, "unbounded", small);
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().code() == ErrorCode::ModInvalid);
        CHECK(refused.error().message().contains("no maximum"));
    }
}

TEST_CASE("a mod that refuses to initialise is disabled and never ticks", "[script][mod]") {
    Fixture f;
    auto mod = f.load({.behaviour = Behaviour::InitRefuses}, "refuses");
    REQUIRE(mod.has_value());

    const auto failed = mod->init();
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code() == ErrorCode::ModInvalid);
    CHECK(mod->disabled());
    CHECK(mod->disabled_because().contains("mod_init returned 7"));

    // Ticking a disabled mod succeeds and does nothing. It is not an error once per tick for
    // the rest of the session: the mod is simply not there any more.
    CHECK(mod->tick(1).has_value());
    CHECK(mod->disabled());
}

TEST_CASE("a mod that will not stop is stopped by the instruction budget", "[script][mod]") {
    // The case that has to work or none of the rest matters: without a budget counted in
    // instructions, a mod with an infinite loop hangs every peer for ever.
    Fixture f;
    ModLimits metered;
    metered.max_instructions_per_tick = 100'000;

    auto mod = f.load({.behaviour = Behaviour::TickLoopsForever}, "spinner", metered);
    REQUIRE(mod.has_value());
    REQUIRE(mod->init().has_value());

    const auto stopped = mod->tick(0);
    REQUIRE_FALSE(stopped.has_value());
    CHECK(stopped.error().code() == ErrorCode::ModBudgetExhausted);
    CHECK(mod->disabled());
}

TEST_CASE("a mod that traps is disabled rather than taking the process with it", "[script][mod]") {
    Fixture f;

    SECTION("reading outside its own memory") {
        auto mod = f.load({.behaviour = Behaviour::TickReadsOutOfBounds}, "wanderer");
        REQUIRE(mod.has_value());
        REQUIRE(mod->init().has_value());

        const auto trapped = mod->tick(0);
        REQUIRE_FALSE(trapped.has_value());
        CHECK(trapped.error().code() == ErrorCode::ModTrapped);
        CHECK(mod->disabled());
    }

    SECTION("recursing until the stack is gone") {
        // Hits the guest's own operand stack, which is bounded by `ModLimits::max_stack_bytes`,
        // rather than the host's — which is the difference between a trap and a crash.
        auto mod = f.load({.behaviour = Behaviour::TickRecursesForever}, "recurser");
        REQUIRE(mod.has_value());
        REQUIRE(mod->init().has_value());

        const auto trapped = mod->tick(0);
        REQUIRE_FALSE(trapped.has_value());
        CHECK(mod->disabled());
    }
}

TEST_CASE("one mod misbehaving leaves the others running", "[script][mod]") {
    // The engine never stops because a mod did, and neither does another mod. Stated as a test
    // because "other mods continue" is the half of the failure policy that is easy to write
    // down and easy not to implement.
    Fixture f;
    ModLimits metered;
    metered.max_instructions_per_tick = 100'000;

    auto good = f.load({}, "good");
    auto bad = f.load({.behaviour = Behaviour::TickLoopsForever}, "bad", metered);
    REQUIRE(good.has_value());
    REQUIRE(bad.has_value());
    REQUIRE(good->init().has_value());
    REQUIRE(bad->init().has_value());

    CHECK_FALSE(bad->tick(0).has_value());
    CHECK(bad->disabled());

    for (std::int64_t tick = 0; tick < 8; ++tick) {
        REQUIRE(good->tick(tick).has_value());
    }
    CHECK_FALSE(good->disabled());
}

TEST_CASE("a disabled mod gives its memory back", "[script][mod]") {
    // A mod that has gone wrong holding sixteen megabytes until shutdown is a leak with an
    // explanation attached. Measured through the runtime's own counter rather than assumed.
    Fixture f;
    ModLimits metered;
    metered.max_instructions_per_tick = 100'000;

    auto mod = f.load({.behaviour = Behaviour::TickLoopsForever}, "spinner", metered);
    REQUIRE(mod.has_value());
    REQUIRE(mod->init().has_value());
    const auto while_running = f.runtime.stats().bytes_in_use;

    CHECK_FALSE(mod->tick(0).has_value());
    CHECK(mod->disabled());
    CHECK(f.runtime.stats().bytes_in_use < while_running);
}
