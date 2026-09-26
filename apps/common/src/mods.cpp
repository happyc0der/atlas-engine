// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/app/mods.hpp>
#include <atlas/assets/filesystem.hpp>
#include <atlas/assets/virtual_path.hpp>
#include <atlas/core/log.hpp>
#include <atlas/text/substitute.hpp>

#include <array>
#include <cstdio>
#include <filesystem>
#include <format>
#include <span>

namespace atlas::app {
namespace {

constexpr log::Category kApp = log::category::kApp;

}  // namespace

Result<LoadedMod> open_mod(std::string_view name, std::string_view mods_dir,
                           bool unsafe_debug_imports) {
    LoadedMod opened;
    if (name.empty()) {
        return opened;
    }
    opened.name = std::string{name};

    assets::FileSystem files;
    if (auto status = files.mount("mods", std::filesystem::path(mods_dir)); !status) {
        return std::unexpected(std::move(status).error().context("mounting the mods directory"));
    }
    auto path = assets::VirtualPath::parse(name);
    if (!path) {
        return std::unexpected(std::move(path).error().context("the mod's name"));
    }
    auto bytes = files.read(*path);
    if (!bytes) {
        return std::unexpected(std::move(bytes).error().context("reading the mod"));
    }
    opened.bytes = *std::move(bytes);

    // The table beside the module, if there is one. Read through the same mount and the same
    // validated path as the module itself. A table naming anything outside the mod's namespace
    // is refused whole — a mod may add words under its own name and nobody else's — and the mod
    // loads regardless, because words are presentation and a missing one shows its key.
    {
        std::string_view stem = name;
        if (stem.ends_with(".wasm")) {
            stem.remove_suffix(std::string_view{".wasm"}.size());
        }
        const std::string table_name = std::format("{}.strings.json", stem);
        if (auto table_path = assets::VirtualPath::parse(table_name);
            table_path && files.exists(*table_path)) {
            auto table_bytes = files.read(*table_path);
            auto table =
                table_bytes
                    ? assets::import_string_table(*table_bytes, table_name)
                    : Result<assets::ImportedStringTable>{std::unexpected(table_bytes.error())};
            if (!table) {
                ATLAS_LOG_WARN(kApp, "mod '{}' string table not used: {}", name, table.error());
            } else if (auto allowed = script::check_mod_table(*table, name); !allowed) {
                ATLAS_LOG_WARN(kApp, "mod '{}' string table refused: {}", name, allowed.error());
            } else {
                opened.strings = *std::move(table);
            }
        }
    }

    if (unsafe_debug_imports) {
        // Said out loud, at warning level, every time. A run that offers a mod a clock is a run
        // whose results mean nothing, and the log is where somebody reading an unexpected
        // divergence will look first.
        ATLAS_LOG_WARN(kApp, "--unsafe-debug-imports: mods are offered a host clock; a mod that "
                             "reads one will diverge under lockstep, which is the only thing "
                             "this flag is for");
    }
    auto runtime = script::Runtime::create({.unsafe_debug_imports = unsafe_debug_imports});
    if (!runtime) {
        return std::unexpected(std::move(runtime).error());
    }
    opened.runtime = *std::move(runtime);
    return opened;
}

void say_messages(script::ModHost& host, const text::Catalog* catalog, std::string_view who) {
    // Printed as well as logged, so the log console shows it in a window and a headless run can
    // be read by a script.
    for (const auto& message : host.take_messages()) {
        std::array<std::string, ATLAS_MOD_MAX_SAY_ARGS> owned;
        std::array<std::string_view, ATLAS_MOD_MAX_SAY_ARGS> arg_views;
        const auto arguments = message.arguments();
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            owned[i] = std::format("{}", arguments[i]);
            arg_views[i] = owned[i];
        }
        const std::string_view pattern =
            catalog != nullptr ? catalog->lookup(message.key) : std::string_view{message.key};
        const std::string text =
            text::substitute(pattern, std::span(arg_views).first(arguments.size()));
        std::printf("%smod '%s' says: %s\n", std::string{who}.c_str(),
                    std::string{host.name()}.c_str(), text.c_str());
        ATLAS_LOG_INFO(kApp, "{}mod '{}' says: {}", who, host.name(), text);
    }
}

void report_mod(const script::ModHost& host, std::string_view who) {
    const auto stats = host.stats();
    ATLAS_LOG_INFO(
        kApp,
        "{}mod '{}': {} submitted, {} refused, {} log line(s) dropped, {} said, {} "
        "unsaid over {} tick(s), {}",
        who, host.name(), stats.commands_submitted, stats.commands_refused, stats.log_lines_dropped,
        stats.messages_said, stats.messages_dropped, stats.ticks_run,
        host.disabled() ? std::string("disabled: ") + std::string(host.disabled_because())
                        : std::string("still running"));
}

}  // namespace atlas::app
