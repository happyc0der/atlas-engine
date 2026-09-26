// SPDX-License-Identifier: GPL-3.0-or-later
//
// What hosting a mod costs per tick.
//
// Four scenarios:
//
//   script/tick_empty   — a guest whose `mod_tick` returns immediately. This is the **boundary**:
//                         everything the host does around a call, with nothing inside it. It is
//                         the number that matters, because it is what every mod pays whether or
//                         not it does anything.
//   script/tick_submit  — the same, plus one `atlas_submit` through the command queue. The
//                         difference between this and the above is what one command costs a mod.
//   script/tick_views   — a guest that reads a view before deciding, which is what a real mod
//                         does and what the demonstration mod does.
//   script/load         — validating and instantiating a module. Once per mod per run, so it is
//                         here to be known rather than to be optimised.
//
// The guest is deliberately trivial in all four. A benchmark whose guest did real work would
// measure the guest, and a mod author's code is not the engine's to be fast at.
//
// ---------------------------------------------------------------------------------------
// PREDICTION, written before the first run and committed before the numbers exist.
//
// `script/tick_empty`: **1 to 4 microseconds per mod per tick**. The work is a `HostCall`
// constructed on the stack, two spans copied into a vector that already has capacity, one
// `wasm_runtime_set_instruction_count_limit`, one `wasm_runtime_call_wasm` into an interpreter,
// and two pointer writes to install and clear the call context. Nothing there allocates after
// the first tick. WAMR's call entry does set up an execution frame, which is why this is not
// expected to be in the hundreds of nanoseconds.
//
// `script/tick_submit`: **+1 to 3 µs over tick_empty**, and the cost is expected to be the
// command's payload allocation plus the queue's handler lookup and validation — the same
// per-command allocation M14 predicted for lockstep and found to be the dominant term there.
//
// `script/tick_views`: **within 1 µs of tick_empty**. `atlas_view_read` is a bounds check and a
// `memcpy` of four bytes; if this is materially more than tick_empty, the cost is in the host
// call boundary itself rather than the copy, and that is worth knowing.
//
// `script/load`: **20 to 200 µs**, dominated by instantiating one 64 KiB page of linear memory
// rather than by validating 295 bytes of module.
//
// **The claim this is expected to support is that mod hosting does not need optimising.** At
// sixty ticks a second with eight mods at 4 µs, that is under 0.2% of a 16.6 ms frame. Above
// **50 µs** for `tick_empty` the cause will be something allocating per call — most likely the
// `std::map` of random streams, which is constructed per `HostCall` and should stay empty unless
// a guest draws — and the fix would be to hoist it into the host. Finding that out is the point
// of writing this down.
//
// Being wrong here is fine and is why it is recorded first. Being unable to be wrong is not.
// ---------------------------------------------------------------------------------------

#include <atlas/core/assert.hpp>
#include <atlas/script/mod_host.hpp>
#include <atlas/script/runtime.hpp>
#include <atlas/simulation/command.hpp>
#include <atlas/simulation/turn_gate.hpp>

#include "harness.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace {

using atlas::bench::Result;

/// Abort rather than exit, which is what every other benchmark here does: a benchmark that
/// cannot set itself up has nothing to unwind, and a measurement taken after a failed setup
/// would be a number with no meaning.
[[noreturn]] void die(const std::string& why) {
    std::fprintf(stderr, "benchmark setup failed: %s\n", why.c_str());
    std::abort();
}

const atlas::sim::CommandType kPoke = atlas::sim::command_type("bench mod poke");

[[nodiscard]] atlas::sim::CommandHandler poke_handler() {
    atlas::sim::CommandHandler handler;
    handler.validate = [](std::span<const std::byte> payload) -> atlas::Status {
        if (payload.size() != 3) {
            return std::unexpected(
                atlas::Error(atlas::ErrorCode::MalformedData, "expected three bytes"));
        }
        return atlas::ok();
    };
    handler.apply = [](atlas::sim::World&, const atlas::sim::ApplyContext&,
                       std::span<const std::byte>) { return atlas::ok(); };
    return handler;
}

// --- a minimal WebAssembly encoder, so this benchmark needs no toolchain -------------------
//
// The same reasoning as the tests and `tools/gen_mods.py`: depending on an assembler would make
// this benchmark unbuildable on a machine that has none, and the modules it needs are a handful
// of instructions each.

using Bytes = std::vector<std::uint8_t>;

void uleb(Bytes& out, std::uint64_t value) {
    do {
        auto byte = static_cast<std::uint8_t>(value & 0x7FU);
        value >>= 7U;
        if (value != 0) {
            byte |= 0x80U;
        }
        out.push_back(byte);
    } while (value != 0);
}

void sleb(Bytes& out, std::int64_t value) {
    while (true) {
        auto byte = static_cast<std::uint8_t>(static_cast<std::uint64_t>(value) & 0x7FU);
        value >>= 7;
        const bool sign_set = (byte & 0x40U) != 0;
        if ((value == 0 && !sign_set) || (value == -1 && sign_set)) {
            out.push_back(byte);
            return;
        }
        out.push_back(static_cast<std::uint8_t>(byte | 0x80U));
    }
}

void put_name(Bytes& out, std::string_view text) {
    uleb(out, text.size());
    out.insert(out.end(), text.begin(), text.end());
}

void put_section(Bytes& out, std::uint8_t id, const Bytes& payload) {
    out.push_back(id);
    uleb(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
}

/// What the guest's `mod_tick` does, which is the only thing that differs between the modules.
enum class Shape : std::uint8_t { Empty, Submit, ReadView };

[[nodiscard]] Bytes build(Shape shape) {
    // Imports, in the order the module declares them. Only what each shape needs, because an
    // unused import still costs a resolution at load.
    std::vector<std::string_view> imports;
    if (shape == Shape::Submit) {
        imports = {"atlas_submit"};
    } else if (shape == Shape::ReadView) {
        imports = {"atlas_view_read"};
    }

    Bytes out{0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};

    {
        Bytes types;
        uleb(types, 3 + imports.size());
        for (const auto& bytes : {Bytes{0x60, 0x00, 0x01, 0x7F},  // () -> i32    mod_init
                                  Bytes{0x60, 0x01, 0x7E, 0x00},  // (i64) -> ()  mod_tick
                                  Bytes{0x60, 0x00, 0x00}}) {     // () -> ()     mod_shutdown
            types.insert(types.end(), bytes.begin(), bytes.end());
        }
        if (shape == Shape::Submit) {
            // (i32, i32, i32) -> i32
            const Bytes submit{0x60, 0x03, 0x7F, 0x7F, 0x7F, 0x01, 0x7F};
            types.insert(types.end(), submit.begin(), submit.end());
        } else if (shape == Shape::ReadView) {
            // (i32, i32, i32, i32) -> i32
            const Bytes read{0x60, 0x04, 0x7F, 0x7F, 0x7F, 0x7F, 0x01, 0x7F};
            types.insert(types.end(), read.begin(), read.end());
        }
        put_section(out, 1, types);
    }

    if (!imports.empty()) {
        Bytes section;
        uleb(section, imports.size());
        for (std::size_t i = 0; i < imports.size(); ++i) {
            put_name(section, "atlas");
            put_name(section, imports[i]);
            section.push_back(0x00);
            uleb(section, 3 + i);
        }
        put_section(out, 2, section);
    }

    {
        Bytes functions;
        uleb(functions, 3);
        uleb(functions, 0);
        uleb(functions, 1);
        uleb(functions, 2);
        put_section(out, 3, functions);
    }

    {
        Bytes memory;
        uleb(memory, 1);
        memory.push_back(0x01);  // a minimum and a maximum, which the loader requires
        uleb(memory, 1);
        uleb(memory, 1);
        put_section(out, 5, memory);
    }

    {
        const auto first_local = static_cast<std::uint32_t>(imports.size());
        Bytes exports;
        uleb(exports, 4);
        put_name(exports, "memory");
        exports.push_back(0x02);
        uleb(exports, 0);
        for (const auto& [name, offset] : std::array<std::pair<std::string_view, std::uint32_t>, 3>{
                 {{"mod_init", 0}, {"mod_tick", 1}, {"mod_shutdown", 2}}}) {
            put_name(exports, name);
            exports.push_back(0x00);
            uleb(exports, first_local + offset);
        }
        put_section(out, 7, exports);
    }

    {
        const auto body = [](const Bytes& code) {
            Bytes wrapped;
            uleb(wrapped, 0);
            wrapped.insert(wrapped.end(), code.begin(), code.end());
            wrapped.push_back(0x0B);
            Bytes sized;
            uleb(sized, wrapped.size());
            sized.insert(sized.end(), wrapped.begin(), wrapped.end());
            return sized;
        };

        Bytes init;
        init.push_back(0x41);
        sleb(init, 0);

        Bytes tick;
        if (shape == Shape::Submit) {
            tick.push_back(0x41);
            sleb(tick, static_cast<std::int64_t>(static_cast<std::uint32_t>(kPoke)));
            tick.push_back(0x41);
            sleb(tick, 0);
            tick.push_back(0x41);
            sleb(tick, 3);
            tick.push_back(0x10);
            uleb(tick, 0);
            tick.push_back(0x1A);
        } else if (shape == Shape::ReadView) {
            for (const std::int64_t argument : {0, 0, 0, 4}) {
                tick.push_back(0x41);
                sleb(tick, argument);
            }
            tick.push_back(0x10);
            uleb(tick, 0);
            tick.push_back(0x1A);
        }

        Bytes code;
        uleb(code, 3);
        for (const auto& piece : {body(init), body(tick), body({})}) {
            code.insert(code.end(), piece.begin(), piece.end());
        }
        put_section(out, 10, code);
    }

    return out;
}

[[nodiscard]] std::vector<std::byte> as_bytes(const Bytes& in) {
    std::vector<std::byte> out;
    out.reserve(in.size());
    for (const auto value : in) {
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

[[nodiscard]] std::vector<Result> run() {
    std::vector<Result> results;

    auto runtime = atlas::script::Runtime::create();
    if (!runtime) {
        die("no script runtime");
    }

    const std::array<std::byte, 8> view_bytes{};
    const std::array<atlas::script::ModView, 1> views{
        atlas::script::ModView{.name = "bench", .bytes = view_bytes}};

    for (const auto [shape, label] : std::array<std::pair<Shape, std::string_view>, 3>{
             {{Shape::Empty, "script/tick_empty"},
              {Shape::Submit, "script/tick_submit"},
              {Shape::ReadView, "script/tick_views"}}}) {
        atlas::sim::CommandQueue queue;
        if (!queue.register_handler(kPoke, poke_handler())) {
            die("handler");
        }
        atlas::sim::TurnGate gate;

        const auto module_bytes = as_bytes(build(shape));
        auto host = atlas::script::ModHost::create(*runtime, 0, module_bytes, "bench");
        if (!host) {
            die(std::format("no host: {}", host.error().message()));
        }
        if (!(*host)->start()) {
            die("start");
        }
        (*host)->set_views(views);

        atlas::Tick tick = 0;
        results.push_back(atlas::bench::measure(label, "mods=1", 20'000, 2'000, [&] {
            if (!(*host)->poll(tick++, queue, gate)) {
                die("poll");
            }
            // Cleared rather than drained: draining sorts, and the sort is the simulation's cost
            // rather than the host's. A queue left to grow would reach its bound and start
            // refusing, which would turn this into a measurement of refusals.
            queue.clear();
        }));
        if ((*host)->disabled()) {
            die(std::format("the guest stopped: {}", (*host)->disabled_because()));
        }
    }

    {
        const auto module_bytes = as_bytes(build(Shape::Empty));
        results.push_back(atlas::bench::measure("script/load", "bytes=trivial", 2'000, 200, [&] {
            const auto host = atlas::script::ModHost::create(*runtime, 0, module_bytes, "bench");
            if (!host) {
                die("load");
            }
        }));
    }

    {
        // A compiled module: the engine's own painter, C built by tools/build_mods.py (ADR-0023),
        // read from the committed file as a player's copy would be. Until M27 this row loaded
        // the chess opponent, 8 KB, which goes with chess to its own repository (ADR-0024);
        // docs/PERFORMANCE.md records what the row measured then and measures now. Skipped, and
        // said, when run from somewhere the file is not.
        std::ifstream file(std::filesystem::path{"assets/mods/painter.wasm"}, std::ios::binary);
        const std::vector<char> raw{std::istreambuf_iterator<char>(file),
                                    std::istreambuf_iterator<char>()};
        if (raw.empty()) {
            std::fprintf(stderr, "bench_script: assets/mods/painter.wasm not found from here, "
                                 "skipping its load\n");
        } else {
            std::vector<std::byte> module_bytes(raw.size());
            for (std::size_t i = 0; i < raw.size(); ++i) {
                module_bytes[i] = static_cast<std::byte>(raw[i]);
            }
            results.push_back(atlas::bench::measure(
                "script/load", std::format("bytes={}", module_bytes.size()), 2'000, 200, [&] {
                    const auto host =
                        atlas::script::ModHost::create(*runtime, 0, module_bytes, "painter.wasm");
                    if (!host) {
                        die("load the painter");
                    }
                }));
        }
    }

    return results;
}

const bool kRegistered = atlas::bench::register_benchmark("script", run);

}  // namespace
