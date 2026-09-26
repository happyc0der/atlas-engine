// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Loading a sandboxed mod, and saying what it did, the same way in every application.
///
/// Lifted out of the lab when chess became the second application to run a mod (M26,
/// ADR-0023). What moved is what did not depend on the lab: reading a module and its string
/// table through a validated path, bringing the runtime up, printing what a mod says in the
/// application's language, and the one line summarising a run. What each application
/// publishes as views, and when it polls, stays with the application, because those are what a
/// view means.
///
/// Thread affinity: the main thread, like the runtime and the host.

#include <atlas/assets/importer.hpp>
#include <atlas/core/result.hpp>
#include <atlas/script/mod_host.hpp>
#include <atlas/script/runtime.hpp>
#include <atlas/text/catalog.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace atlas::app {

/// A mod's module, its words, and the runtime to run it in. Empty when no mod was asked for.
struct LoadedMod {
    std::optional<script::Runtime> runtime;
    std::vector<std::byte> bytes;
    /// The mod's own words, from `<name>.strings.json` beside it, already checked to name only
    /// keys under the mod's namespace (ADR-0021). Absent when the mod ships none, or when the one
    /// it ships was refused, and either way the mod runs and its keys show as themselves.
    std::optional<assets::ImportedStringTable> strings;
    std::string name;

    [[nodiscard]] bool wanted() const noexcept { return runtime.has_value(); }
};

/// Read `name` from `mods_dir` and bring the runtime up, or do nothing when `name` is empty.
///
/// The bytes come through `assets::VirtualPath` and a mounted root, exactly as a save file does,
/// because a mod is untrusted input in the same way: the name from a command line cannot climb
/// out of the directory. Fails when the directory cannot be mounted, the name is not a valid
/// path, the file cannot be read, or the runtime refuses to start. A string table that cannot be
/// used is logged and ignored rather than failing, because words are presentation.
[[nodiscard]] Result<LoadedMod> open_mod(std::string_view name, std::string_view mods_dir,
                                         bool unsafe_debug_imports);

/// Print and log what a mod said this tick, resolved through `catalog` in the application's
/// language (ADR-0021 D5). A null catalog shows keys, which is what a missing key shows
/// everywhere. `who` prefixes each line, for an application running several peers.
void say_messages(script::ModHost& host, const text::Catalog* catalog, std::string_view who);

/// One log line saying what a mod did, so an integration case has something to assert on.
void report_mod(const script::ModHost& host, std::string_view who);

}  // namespace atlas::app
