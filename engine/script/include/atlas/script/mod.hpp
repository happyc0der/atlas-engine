// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// One loaded mod: validated, instantiated, budgeted, and disabled the moment it misbehaves.
///
/// **A mod is untrusted input, exactly like a save file.** Every refusal below happens before
/// the thing it refuses can do anything, and the order matters: size before reading, validation
/// before inspection, imports and exports before instantiation, instantiation before any guest
/// code runs at all. A check that happens after the guest has started is not a check.
///
/// **The import list is the whole of a guest's authority.** WebAssembly has no ambient
/// anything: a module can reach only what it imports and what is inside its own linear memory.
/// So refusing an unexpected import is not defence in depth, it *is* the defence, and it is why
/// the loader refuses a module rather than resolving imports lazily and hoping.
///
/// Failure policy, from [ADR-0015](../../../../../docs/adr/0015-sandboxed-mods.md) decision 8:
/// a trap, an exhausted budget, a refused allocation or a non-zero `mod_init` **disables this
/// mod for the session and logs once**. Other mods carry on and the engine never stops. It is
/// never re-enabled and never retried, because under lockstep "try it again" is a decision one
/// peer might take and another might not — device loss is the model, not recovery.
///
/// Ownership: move-only, and must not outlive the `Runtime` it was loaded against.
/// Thread affinity: the main thread, with the kernel.

#include <atlas/core/error.hpp>
#include <atlas/core/result.hpp>
#include <atlas/script/atlas_mod.h>
#include <atlas/script/limits.hpp>
#include <atlas/script/runtime.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace atlas::script {

/// The WebAssembly module name every host import lives under.
///
/// A module importing from anywhere else is refused outright rather than having that one import
/// left unresolved, because "unresolved" is a state a guest can probe and "refused" is not.
///
/// Taken from `atlas_mod.h` rather than spelled again, so the name the loader checks and the
/// name a mod author writes cannot drift apart.
inline constexpr std::string_view kImportModule = ATLAS_IMPORT_MODULE;

/// What a mod must export, or it is not a mod.
///
/// Checked as a set rather than one at a time so the error names everything missing at once:
/// somebody porting a mod wants the whole list, not the first item on it.
inline constexpr std::string_view kExportInit = "mod_init";
inline constexpr std::string_view kExportTick = "mod_tick";
inline constexpr std::string_view kExportShutdown = "mod_shutdown";
inline constexpr std::string_view kExportMemory = "memory";

class Mod {
  public:
    /// Validate and instantiate a module from bytes already in memory.
    ///
    /// `debug_name` appears in every message about this mod and is never used to find anything.
    ///
    /// Failures, each before the guest runs: `OutOfRange` for a module larger than the limit;
    /// `ModInvalid` for bytes the runtime will not accept; `ModImportRefused` for an import
    /// outside `kImportModule` or one the host does not provide; `ModExportMissing` for an
    /// absent required export; `Exhausted` if instantiating would pass the runtime's budget.
    [[nodiscard]] static Result<Mod> load(Runtime& runtime, std::span<const std::byte> bytes,
                                          std::string_view debug_name,
                                          const ModLimits& limits = {});

    ~Mod();
    Mod(const Mod&) = delete;
    Mod& operator=(const Mod&) = delete;
    Mod(Mod&& other) noexcept;
    Mod& operator=(Mod&& other) noexcept;

    /// Call `mod_init`. A non-zero return, a trap or an exhausted budget disables the mod.
    ///
    /// Returns the error that disabled it, so the caller can log it with its own context; the
    /// mod is already disabled by the time this returns, and calling again does nothing.
    [[nodiscard]] Status init();

    /// Call `mod_tick` for one simulation tick, with the instruction budget armed.
    ///
    /// Does nothing and succeeds if the mod is already disabled: a disabled mod is not an error
    /// every tick for the rest of the session, it is a mod that is not there.
    [[nodiscard]] Status tick(std::int64_t tick_index);

    [[nodiscard]] bool disabled() const noexcept;

    /// Why it was disabled, empty while it is running. Kept so a panel or a report can say what
    /// happened long after the log line scrolled past.
    [[nodiscard]] std::string_view disabled_because() const noexcept;

    [[nodiscard]] std::string_view name() const noexcept;

  private:
    Mod();

    /// Install the context the host imports read, for the duration of one call into the guest.
    ///
    /// Private, and reachable only by `ModHost`, because it is the mechanism behind the rule
    /// rather than part of the mod interface: a caller who could set this could give a mod
    /// another mod's identity and command budget, which is the whole thing the boundary exists
    /// to make impossible.
    friend class ModHost;
    void set_call_context(void* context) noexcept;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace atlas::script
