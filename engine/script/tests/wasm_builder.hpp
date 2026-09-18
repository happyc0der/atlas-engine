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
/// This is deliberately not a general assembler. It builds the handful of shapes the suite
/// needs and nothing else; a second consumer would be the point at which it should grow.

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
};

inline constexpr std::uint8_t kValI32 = 0x7F;
inline constexpr std::uint8_t kValI64 = 0x7E;

inline constexpr std::uint8_t kOpEnd = 0x0B;
inline constexpr std::uint8_t kOpLoop = 0x03;
inline constexpr std::uint8_t kOpBr = 0x0C;
inline constexpr std::uint8_t kOpCall = 0x10;
inline constexpr std::uint8_t kOpDrop = 0x1A;
inline constexpr std::uint8_t kOpI32Const = 0x41;
inline constexpr std::uint8_t kOpI64Const = 0x42;
inline constexpr std::uint8_t kOpI32Load = 0x28;
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
    /// An import to declare, as `module.field`. Empty means none.
    std::string_view import_module;
    std::string_view import_field;
};

/// Build a module. The result is a complete, valid binary unless the spec asks otherwise.
[[nodiscard]] inline Bytes build(const ModuleSpec& spec = {}) {
    Bytes out{0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};  // "\0asm", version 1

    const bool has_import = !spec.import_module.empty();
    // Imported functions occupy the low indices, so everything defined here shifts up by one.
    const std::uint32_t first_local_func = has_import ? 1U : 0U;

    // Three types: () -> i32, (i64) -> (), () -> ().
    {
        Bytes payload;
        put_uleb(payload, 3);
        put_all(payload, {0x60, 0x00, 0x01, kValI32});
        put_all(payload, {0x60, 0x01, kValI64, 0x00});
        put_all(payload, {0x60, 0x00, 0x00});
        put_section(out, Section::Type, payload);
    }

    if (has_import) {
        Bytes payload;
        put_uleb(payload, 1);
        put_name(payload, spec.import_module);
        put_name(payload, spec.import_field);
        payload.push_back(0x00);  // a function
        put_uleb(payload, 2);     // of type () -> ()
        put_section(out, Section::Import, payload);
    }

    // Three functions: mod_init, mod_tick, mod_shutdown.
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
        put_all(init_code, {kOpI32Const});
        put_uleb(init_code, spec.behaviour == Behaviour::InitRefuses ? 7 : 0);

        Bytes tick_code;
        switch (spec.behaviour) {
        case Behaviour::TickLoopsForever:
            // loop { br 0 } — the branch targets the loop itself, so it never ends.
            put_all(tick_code, {kOpLoop, kBlockVoid, kOpBr, 0x00, kOpEnd});
            break;
        case Behaviour::TickRecursesForever:
            // `mod_tick` takes an i64, so the argument has to be pushed before the recursive
            // call or the module does not validate — which is a different failure from the one
            // this case is about.
            put_all(tick_code, {kOpI64Const, 0x00, kOpCall});
            put_uleb(tick_code, first_local_func + 1);  // itself
            break;
        case Behaviour::TickReadsOutOfBounds:
            tick_code.push_back(kOpI32Const);
            put_uleb(tick_code, 0x00FF'FFFFU);
            put_all(tick_code, {kOpI32Load, 0x02, 0x00, kOpDrop});
            break;
        case Behaviour::Wellbehaved:
        case Behaviour::InitRefuses: break;
        }

        Bytes payload;
        put_uleb(payload, 3);
        for (const auto& code : {init_code, tick_code, Bytes{}}) {
            const auto encoded = body(code);
            payload.insert(payload.end(), encoded.begin(), encoded.end());
        }
        put_section(out, Section::Code, payload);
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

}  // namespace atlas::script::test
