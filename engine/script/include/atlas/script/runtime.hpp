// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// The WebAssembly runtime's process-wide lifetime, owned by one object.
///
/// **This is the second process-wide object in Atlas, and the only one ADR-0015 adds.**
/// CLAUDE.md permits exactly one — the log sink registry — and says so because hidden global
/// state is what makes lifetimes impossible to reason about. WAMR has a process-global
/// initialisation and teardown, which is not negotiable from this side, so the rule bends here
/// in the one place and is named rather than quietly widened. `Runtime` is to WAMR what
/// `platform::Platform` is to SDL: an RAII object the composition root creates once, asserts is
/// unique, and destroys last.
///
/// Ownership: move-only, and the mods loaded against a runtime must be destroyed before it.
/// That ordering is asserted rather than documented, because getting it wrong frees the
/// allocator a mod's memory came from.
///
/// Thread affinity: the main thread, throughout. A mod is polled where the kernel runs, and
/// nothing here is safe to touch from anywhere else.
///
/// Failure: `create()` returns `ScriptRuntimeInitFailed` and never a runtime that does not
/// work. There is no null runtime, deliberately — unlike audio, where a silent device is a
/// legitimate degradation, an application that cannot sandbox has nothing safe to fall back to,
/// and quietly running no mods would look identical to running mods that do nothing.

#include <atlas/core/error.hpp>
#include <atlas/core/result.hpp>
#include <atlas/script/limits.hpp>

#include <cstddef>
#include <memory>

namespace atlas::script {

/// Counts of what the runtime has been asked to do, for statistics and for tests.
struct RuntimeStats {
    /// Bytes currently held by the runtime's allocator, across every mod.
    std::size_t bytes_in_use = 0;
    /// The high-water mark of the above, which is the number worth reporting: the peak is what
    /// a limit has to accommodate, and the instantaneous value is whatever happened to be true
    /// when somebody asked.
    std::size_t peak_bytes_in_use = 0;
    /// Allocations the counted allocator refused because the cap would have been passed.
    std::size_t allocations_refused = 0;
};

class Runtime {
  public:
    /// Bring the runtime up. At most one may exist in a process, and a second `create()` while
    /// one is alive is a programmer error rather than a recoverable one, so it asserts.
    ///
    /// `budget` caps the runtime's own allocator across every mod together. It is separate from
    /// a mod's linear-memory limit and both apply: linear memory is what a guest can address,
    /// and this is what the runtime spends on top of it holding module structures.
    [[nodiscard]] static Result<Runtime> create(std::size_t budget_bytes = kDefaultBudgetBytes);

    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&& other) noexcept;
    Runtime& operator=(Runtime&& other) noexcept;

    /// Total the runtime's allocator may hold across every loaded mod.
    ///
    /// Sized so that the default eight mods at the default linear-memory ceiling fit with room
    /// for the module structures beside them, rather than being a round number.
    static constexpr std::size_t kDefaultBudgetBytes = std::size_t{160} * 1024 * 1024;

    [[nodiscard]] RuntimeStats stats() const noexcept;

    /// Whether a runtime currently exists. For assertions and for tests that must not leave one
    /// behind; not for deciding whether to make one.
    [[nodiscard]] static bool alive() noexcept;

  private:
    Runtime();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace atlas::script
