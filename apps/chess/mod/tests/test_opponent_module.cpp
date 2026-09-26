// SPDX-License-Identifier: GPL-3.0-or-later
// The committed chess opponent, loaded into the engine's own runtime (ADR-0023).
//
// What this proves is the toolchain rather than the chess: that a module clang compiled and
// wasm-ld linked, with the features `tools/build_mods.py` pins, is accepted by the loader and runs
// under the limits every mod gets. It reads the committed file — the one a player would load —
// rather than building one, because this test is not the place a compiler is available.
#include <atlas/core/assert.hpp>
#include <atlas/script/mod_host.hpp>
#include <atlas/script/runtime.hpp>
#include <atlas/simulation/command.hpp>
#include <atlas/simulation/turn_gate.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace {

const bool kMainThreadMarked = [] {
    atlas::mark_main_thread();
    return true;
}();

[[nodiscard]] std::vector<std::byte> committed_module() {
    // The working directory is the project root under CTest (AtlasModule.cmake).
    const std::filesystem::path path = "assets/mods/chess_opponent.wasm";
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file.good());
    const std::vector<char> raw{std::istreambuf_iterator<char>(file),
                                std::istreambuf_iterator<char>()};
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

}  // namespace

TEST_CASE("the compiled opponent loads and runs under the default limits", "[chess][mod]") {
    REQUIRE(kMainThreadMarked);
    const auto bytes = committed_module();
    REQUIRE_FALSE(bytes.empty());

    auto runtime = atlas::script::Runtime::create();
    REQUIRE(runtime.has_value());
    auto host = atlas::script::ModHost::create(*runtime, 0, bytes, "chess_opponent.wasm");
    INFO((host ? std::string{} : host.error().message()));
    REQUIRE(host.has_value());
    const auto started = (*host)->start();
    INFO((started ? std::string{} : started.error().message()));
    REQUIRE(started.has_value());

    atlas::sim::CommandQueue queue;
    atlas::sim::TurnGate gate;
    for (atlas::Tick tick = 0; tick < 5; ++tick) {
        const auto report = (*host)->poll(tick, queue, gate);
        REQUIRE(report.has_value());
        CHECK_FALSE(report->closed);
    }
    CHECK_FALSE((*host)->disabled());
    CHECK((*host)->stats().ticks_run == 5);
    CHECK((*host)->stats().commands_submitted == 0);
}
