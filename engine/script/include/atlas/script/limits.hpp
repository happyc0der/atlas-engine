// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// What a mod is allowed to consume, and why each number is that number.
///
/// Every limit here is a **ceiling that disables a runaway**, not a budget anybody is expected
/// to spend. A mod that reaches one of these has gone wrong; a mod behaving normally should not
/// come close, and if a real one ever does, the number moves in a commit that says why rather
/// than quietly at a call site.
///
/// The shape follows `net::CommandInbox`, which learned it the hard way: **two independent
/// bounds, because either alone is not a bound.** A count of modules with no size limit bounds
/// memory at "however many times whatever a module may be", and a size limit with no count
/// bounds nothing at all.
///
/// See [ADR-0015](../../../../../docs/adr/0015-sandboxed-mods.md) decision 7.

#include <cstddef>
#include <cstdint>

namespace atlas::script {

/// Limits applied to one mod, injectable so a test can reach a bound in a millisecond rather
/// than by actually allocating sixteen megabytes or running ten million instructions.
///
/// The defaults are the shipping values. A test that lowers one is testing the mechanism; a
/// test that needs the real number should say so.
struct ModLimits {
    /// Bytes of WebAssembly a mod file may contain, checked **before the bytes are read** and
    /// again against what was actually read.
    ///
    /// A mod is untrusted input from a mounted directory, so the size has to be refused from
    /// the file's own metadata rather than after loading it into memory — the same discipline
    /// the WAV reader follows, and the reason the clip reader gained a cap in M13.
    std::size_t max_module_bytes = std::size_t{4} * 1024 * 1024;

    /// Bytes of linear memory one mod instance may hold.
    ///
    /// Sixteen megabytes is enough for a scripting language compiled to WebAssembly to live
    /// inside, which is the authoring route ADR-0015 defers rather than forecloses. Eight mods
    /// at the ceiling is still less than one million-cell snapshot, which is the figure worth
    /// holding in mind: this is not the thing that will run a machine out of memory.
    std::size_t max_linear_memory_bytes = std::size_t{16} * 1024 * 1024;

    /// Bytes of WebAssembly operand stack for one instance. Deep recursion hits this and traps,
    /// rather than growing until the host's own stack is gone.
    std::size_t max_stack_bytes = std::size_t{64} * 1024;

    /// Instructions one mod may execute in one tick.
    ///
    /// **Counted in instructions and never in wall time.** A wall-clock watchdog fires after a
    /// different amount of work on a fast machine than on a slow one, so two peers would
    /// disable a mod at different ticks and diverge — a safety mechanism that causes the
    /// failure it exists to prevent. Instruction counts are identical everywhere.
    std::uint32_t max_instructions_per_tick = 10'000'000;
};

}  // namespace atlas::script
