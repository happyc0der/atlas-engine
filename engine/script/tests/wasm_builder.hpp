// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Builds WebAssembly modules byte by byte, for tests.
///
/// **No toolchain, on purpose**, and for two reasons that both come out of M13. Depending on
/// `wat2wasm` would make these tests skip on any machine without it, which is how a hostile-
/// input suite quietly stops running; and bytes produced by a third-party compiler are bytes
/// this repository does not decide, which is the trap the shader currency check sat in from M2
/// to M13. Everything here is written out explicitly, so a test that says "truncated" produces
/// exactly the truncation it names.
///
/// It also follows the house convention for test inputs — the image tests embed a PNG as a byte
/// array, the WAV tests assemble RIFF chunks — and the convention exists because an input you
/// can read in the test is an input whose failure you can explain.
///
/// This is deliberately not a general assembler. It builds the shapes the suite needs: three
/// required exports, a declared memory, any list of imports, and whatever body a test wants for
/// `mod_tick`.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <vector>

namespace atlas::script::test {

/// Section identifiers from the specification.
enum class Section : std::uint8_t {
    Type = 1,
    Import = 2,
    Function = 3,
    Memory = 5,
    Export = 7,
    Code = 10,
    Data = 11,
};

inline constexpr std::uint8_t kValI32 = 0x7F;
inline constexpr std::uint8_t kValI64 = 0x7E;

inline constexpr std::uint8_t kOpEnd = 0x0B;
inline constexpr std::uint8_t kOpLoop = 0x03;
inline constexpr std::uint8_t kOpBr = 0x0C;
inline constexpr std::uint8_t kOpCall = 0x10;
inline constexpr std::uint8_t kOpDrop = 0x1A;
inline constexpr std::uint8_t kOpLocalGet = 0x20;
inline constexpr std::uint8_t kOpI32Const = 0x41;
inline constexpr std::uint8_t kOpI64Const = 0x42;
inline constexpr std::uint8_t kOpI32Load = 0x28;
inline constexpr std::uint8_t kOpI32Store = 0x36;
inline constexpr std::uint8_t kOpI32WrapI64 = 0xA7;
inline constexpr std::uint8_t kBlockVoid = 0x40;

using Bytes = std::vector<std::uint8_t>;

/// Unsigned LEB128, which is how every length and index in the format is written.
inline void put_uleb(Bytes& out, std::uint64_t value) {
    do {
        auto byte = static_cast<std::uint8_t>(value & 0x7FU);
        value >>= 7U;
        if (value != 0) {
            byte |= 0x80U;
        }
        out.push_back(byte);
    } while (value != 0);
}

/// Signed LEB128, which is what `i32.const` and `i64.const` take.
inline void put_sleb(Bytes& out, std::int64_t value) {
    bool more = true;
    while (more) {
        auto byte = static_cast<std::uint8_t>(static_cast<std::uint64_t>(value) & 0x7FU);
        value >>= 7;
        const bool sign_set = (byte & 0x40U) != 0;
        if ((value == 0 && !sign_set) || (value == -1 && sign_set)) {
            more = false;
        } else {
            byte |= 0x80U;
        }
        out.push_back(byte);
    }
}

inline void put_name(Bytes& out, std::string_view name) {
    put_uleb(out, name.size());
    out.insert(out.end(), name.begin(), name.end());
}

inline void put_all(Bytes& out, std::initializer_list<std::uint8_t> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

/// A section, whose length prefix is written once the payload is known.
inline void put_section(Bytes& out, Section id, const Bytes& payload) {
    out.push_back(static_cast<std::uint8_t>(id));
    put_uleb(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
}

/// One host function a module asks for, with the shape it expects.
struct ImportSpec {
    std::string_view module_name = "atlas";
    std::string_view field;
    std::vector<std::uint8_t> params;
    std::vector<std::uint8_t> results;
};

/// What the three required functions do, so a test can name a misbehaviour rather than spell it.
enum class Behaviour {
    /// Returns immediately. `mod_init` returns zero.
    Wellbehaved,
    /// `mod_init` returns a non-zero value: the mod declining to run.
    InitRefuses,
    /// `mod_tick` loops for ever. Only the instruction budget ends it.
    TickLoopsForever,
    /// `mod_tick` recurses with no base case, until the operand stack is gone.
    TickRecursesForever,
    /// `mod_tick` reads far outside linear memory.
    TickReadsOutOfBounds,
};

/// How the module is built, so one function covers every case the suite needs.
struct ModuleSpec {
    Behaviour behaviour = Behaviour::Wellbehaved;
    /// Initial linear memory, in 64 KiB pages.
    std::uint32_t memory_pages = 1;
    /// Declared maximum, in pages. Zero means "the same as the initial".
    std::uint32_t memory_max_pages = 0;
    /// Declare the memory with no maximum at all, which the loader refuses.
    bool memory_without_maximum = false;
    /// Exports to leave out, by name. Everything not named here is exported.
    std::vector<std::string_view> omit_exports;
    /// Host functions to import. Their indices are 0, 1, 2 … in this order.
    std::vector<ImportSpec> imports;
    /// Bytes placed at offset 0 of linear memory, for a mod that submits a fixed payload.
    Bytes data;
    /// Body for `mod_tick`, without the trailing `end`. When set, `behaviour` is ignored for
    /// that function: a test writing its own body is being specific on purpose.
    Bytes tick_body;
};

/// Build a module. The result is a complete, valid binary unless the spec asks otherwise.
[[nodiscard]] inline Bytes build(const ModuleSpec& spec = {}) {
    Bytes out{0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};  // "\0asm", version 1

    const auto import_count = static_cast<std::uint32_t>(spec.imports.size());
    // Imported functions occupy the low indices, so everything defined here shifts up by them.
    const std::uint32_t first_local_func = import_count;

    // Types 0, 1 and 2 are the three required functions; one more per import, in order. Not
    // deduplicated: a duplicate type entry is legal, and this is a test builder rather than a
    // compiler.
    {
        Bytes payload;
        put_uleb(payload, 3 + import_count);
        put_all(payload, {0x60, 0x00, 0x01, kValI32});  // () -> i32     mod_init
        put_all(payload, {0x60, 0x01, kValI64, 0x00});  // (i64) -> ()   mod_tick
        put_all(payload, {0x60, 0x00, 0x00});           // () -> ()      mod_shutdown
        for (const auto& import : spec.imports) {
            payload.push_back(0x60);
            put_uleb(payload, import.params.size());
            payload.insert(payload.end(), import.params.begin(), import.params.end());
            put_uleb(payload, import.results.size());
            payload.insert(payload.end(), import.results.begin(), import.results.end());
        }
        put_section(out, Section::Type, payload);
    }

    if (import_count != 0) {
        Bytes payload;
        put_uleb(payload, import_count);
        std::uint32_t type_index = 3;
        for (const auto& import : spec.imports) {
            put_name(payload, import.module_name);
            put_name(payload, import.field);
            payload.push_back(0x00);  // a function
            put_uleb(payload, type_index++);
        }
        put_section(out, Section::Import, payload);
    }

    {
        Bytes payload;
        put_uleb(payload, 3);
        put_uleb(payload, 0);
        put_uleb(payload, 1);
        put_uleb(payload, 2);
        put_section(out, Section::Function, payload);
    }

    {
        Bytes payload;
        put_uleb(payload, 1);
        if (spec.memory_without_maximum) {
            payload.push_back(0x00);  // a minimum and nothing else
            put_uleb(payload, spec.memory_pages);
        } else {
            payload.push_back(0x01);  // a minimum and a maximum
            put_uleb(payload, spec.memory_pages);
            put_uleb(payload,
                     spec.memory_max_pages != 0 ? spec.memory_max_pages : spec.memory_pages);
        }
        put_section(out, Section::Memory, payload);
    }

    {
        const auto omitted = [&spec](std::string_view name) {
            return std::ranges::find(spec.omit_exports, name) != spec.omit_exports.end();
        };

        struct Entry {
            std::string_view name;
            std::uint8_t kind;
            std::uint32_t index;
        };

        const std::vector<Entry> all{
            {"memory", 0x02, 0},
            {"mod_init", 0x00, first_local_func + 0},
            {"mod_tick", 0x00, first_local_func + 1},
            {"mod_shutdown", 0x00, first_local_func + 2},
        };
        Bytes payload;
        std::vector<const Entry*> kept;
        for (const auto& entry : all) {
            if (!omitted(entry.name)) {
                kept.push_back(&entry);
            }
        }
        put_uleb(payload, kept.size());
        for (const auto* entry : kept) {
            put_name(payload, entry->name);
            payload.push_back(entry->kind);
            put_uleb(payload, entry->index);
        }
        put_section(out, Section::Export, payload);
    }

    {
        const auto body = [](const Bytes& code) {
            Bytes wrapped;
            put_uleb(wrapped, 0);  // no local declarations
            wrapped.insert(wrapped.end(), code.begin(), code.end());
            wrapped.push_back(kOpEnd);
            Bytes sized;
            put_uleb(sized, wrapped.size());
            sized.insert(sized.end(), wrapped.begin(), wrapped.end());
            return sized;
        };

        Bytes init_code;
        init_code.push_back(kOpI32Const);
        put_sleb(init_code, spec.behaviour == Behaviour::InitRefuses ? 7 : 0);

        Bytes tick_code;
        if (!spec.tick_body.empty()) {
            tick_code = spec.tick_body;
        } else {
            switch (spec.behaviour) {
            case Behaviour::TickLoopsForever:
                // loop { br 0 } — the branch targets the loop itself, so it never ends.
                put_all(tick_code, {kOpLoop, kBlockVoid, kOpBr, 0x00, kOpEnd});
                break;
            case Behaviour::TickRecursesForever:
                // `mod_tick` takes an i64, so the argument has to be pushed before the recursive
                // call or the module does not validate — which is a different failure from the
                // one this case is about.
                tick_code.push_back(kOpI64Const);
                put_sleb(tick_code, 0);
                tick_code.push_back(kOpCall);
                put_uleb(tick_code, first_local_func + 1);  // itself
                break;
            case Behaviour::TickReadsOutOfBounds:
                tick_code.push_back(kOpI32Const);
                put_sleb(tick_code, 0x00FF'FFFF);
                put_all(tick_code, {kOpI32Load, 0x02, 0x00, kOpDrop});
                break;
            case Behaviour::Wellbehaved:
            case Behaviour::InitRefuses: break;
            }
        }

        Bytes payload;
        put_uleb(payload, 3);
        for (const auto& code : {init_code, tick_code, Bytes{}}) {
            const auto encoded = body(code);
            payload.insert(payload.end(), encoded.begin(), encoded.end());
        }
        put_section(out, Section::Code, payload);
    }

    if (!spec.data.empty()) {
        Bytes payload;
        put_uleb(payload, 1);
        put_uleb(payload, 0);  // memory 0, active
        payload.push_back(kOpI32Const);
        put_sleb(payload, 0);  // at offset zero
        payload.push_back(kOpEnd);
        put_uleb(payload, spec.data.size());
        payload.insert(payload.end(), spec.data.begin(), spec.data.end());
        put_section(out, Section::Data, payload);
    }

    return out;
}

/// The bytes as the loader wants them.
[[nodiscard]] inline std::vector<std::byte> as_bytes(const Bytes& in) {
    std::vector<std::byte> out;
    out.reserve(in.size());
    for (const auto value : in) {
        out.push_back(static_cast<std::byte>(value));
    }
    return out;
}

/// The imports the host provides, with the shapes `atlas_mod.h` declares.
///
/// A pointer is an i32 in WebAssembly, which is why every one of these takes i32 where the C
/// header says `const void*`.
namespace imports {

[[nodiscard]] inline ImportSpec tick() {
    return {.field = "atlas_tick", .results = {kValI64}};
}

[[nodiscard]] inline ImportSpec submit() {
    return {.field = "atlas_submit", .params = {kValI32, kValI32, kValI32}, .results = {kValI32}};
}

[[nodiscard]] inline ImportSpec command_type() {
    return {.field = "atlas_command_type", .params = {kValI32, kValI32}, .results = {kValI32}};
}

[[nodiscard]] inline ImportSpec random() {
    return {.field = "atlas_random", .params = {kValI32, kValI64}, .results = {kValI64}};
}

[[nodiscard]] inline ImportSpec log() {
    return {.field = "atlas_log", .params = {kValI32, kValI32, kValI32}, .results = {}};
}

[[nodiscard]] inline ImportSpec view_count() {
    return {.field = "atlas_view_count", .results = {kValI32}};
}

[[nodiscard]] inline ImportSpec view_size() {
    return {.field = "atlas_view_size", .params = {kValI32}, .results = {kValI32}};
}

[[nodiscard]] inline ImportSpec view_read() {
    return {.field = "atlas_view_read",
            .params = {kValI32, kValI32, kValI32, kValI32},
            .results = {kValI32}};
}

}  // namespace imports

}  // namespace atlas::script::test
