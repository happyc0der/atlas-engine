// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// The host side of `atlas_mod.h`, private to this module.
///
/// Private but a header, for the same reason `sdl_keymap.hpp` is: the loader has to know which
/// imports exist in order to refuse the ones that do not, and a test has to be able to ask the
/// same question without going through a runtime and a guest.
///
/// **One list, two readers.** `host_import_names()` is what `Mod::load` checks a module's
/// imports against, and the same table is what gets registered with the runtime. If they were
/// two lists, a module could be accepted for an import the host does not actually provide, and
/// the failure would arrive as a trap inside a guest rather than as a refusal at load.

#include <atlas/script/mod_host.hpp>
#include <atlas/simulation/command.hpp>
#include <atlas/simulation/rng.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string_view>

namespace atlas::script {

/// Every function a mod may import, sorted, for the loader to check against.
///
/// What it returns depends on whether the live runtime offered the unsafe debug imports, which
/// is why it is a function rather than a constant: the loader must refuse an import the runtime
/// did not register, and accept one it did, and those are the same list seen from two sides.
[[nodiscard]] std::span<const std::string_view> host_import_names();

/// Register the host imports with the runtime. Called once, by `Runtime::create`, after the
/// runtime is up. Returns false if the runtime refused them, which is a programmer error here
/// rather than bad data.
///
/// `with_debug` adds the clock a mod must not have. See `RuntimeConfig::unsafe_debug_imports`.
[[nodiscard]] bool register_host_imports(bool with_debug);

/// Everything the imports need, for the duration of one `mod_tick` call.
///
/// Installed on the mod's execution environment before the call and cleared after, so an import
/// reached outside a tick finds nothing and refuses rather than reading a stale queue. That is
/// worth the two assignments: `mod_init` and `mod_shutdown` run without one, and a mod calling
/// `atlas_submit` from `mod_init` would otherwise be submitting into whatever the last tick left
/// behind.
struct HostCall {
    /// The tick being decided. What `atlas_tick` returns.
    Tick tick = 0;
    /// Where this mod's commands are stamped for: `tick + input_delay`.
    Tick target = 0;
    sim::SourceId source = sim::SourceId::Local;
    sim::CommandQueue* queue = nullptr;
    std::span<const ModView> views;

    std::uint64_t seed = 0;
    std::string_view mod_name;

    /// Ordered rather than hashed. Nothing here is observable in a hash, but the queue's own
    /// handler table made the same choice for the same reason: a container whose order depends
    /// on a hash seed is one fewer thing that has to be argued about later.
    std::map<std::uint32_t, sim::RngStream> streams;

    std::uint32_t commands_left = 0;
    std::size_t log_bytes_left = 0;

    std::uint64_t submitted = 0;
    std::uint64_t refused = 0;
    std::uint64_t log_dropped = 0;
    Tick highest_target = 0;
};

}  // namespace atlas::script
