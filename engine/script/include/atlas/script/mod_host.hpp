// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// One mod, wired to the command queue as a `sim::CommandSource`.
///
/// **This is the boundary M14 looked for and put in the wrong place.** M14 built a command
/// source bound to a single identifier so that "speak only for yourself" was unrepresentable,
/// discovered that a lockstep session must speak for every peer it is connected to, and removed
/// it — recording that the constraint belongs with the untrusted producer rather than in the
/// interface every producer shares. This is that producer. A mod never supplies a source,
/// because `atlas_submit` has no parameter for one; the host stamps its own identifier and the
/// target tick.
///
/// **One host, one mod, one identifier.** An application with three mods makes three of these.
/// That is what keeps the identifier a property of the object rather than a parameter somebody
/// could pass the wrong value to.
///
/// **A mod never marks the turn gate**, and is handed one only because the interface has one.
/// Mods do not take turns: every peer runs the same mods and each produces the same commands
/// locally, so there is nothing to wait for and nothing to announce. A mod whose commands had
/// to arrive over a link would be a peer, and a mod that gated ticks would stall every peer
/// that did not load it.
///
/// Ownership: move-only, and must not outlive the `Runtime` it was built against.
/// Thread affinity: the main thread, with the kernel.

#include <atlas/assets/importer.hpp>
#include <atlas/core/error.hpp>
#include <atlas/core/result.hpp>
#include <atlas/script/atlas_mod.h>
#include <atlas/script/limits.hpp>
#include <atlas/script/runtime.hpp>
#include <atlas/simulation/command.hpp>
#include <atlas/simulation/command_source.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace atlas::script {

/// A flat run of bytes the application publishes for mods to read.
///
/// **Not the presentation snapshot** (ADR-0015 D5). That is latest-wins, published once a frame,
/// and in the only application that has one it is not published at all when there is no graphics
/// device — so it is absent headless and its age depends on frame rate, which under lockstep is
/// divergence. A view is recomputed from the world at the tick boundary and is identical on
/// every peer.
///
/// **Bytes rather than a type**, because the engine has no game state to describe: `sim::Table`
/// exposes a row count and how to hash and serialise itself, and no accessor at all. What the
/// bytes mean is the application's, per ADR-0010 D7.
///
/// Lifetime: the span must stay valid from `set_views` until `poll` returns. It is not copied —
/// a view over a million cells is four megabytes and copying it once per tick per mod would
/// cost more than every mod put together.
struct ModView {
    /// For logs and for a mod author reading the application's documentation. The guest
    /// addresses views by index, not by name: a name would have to be passed across the
    /// boundary on every call to buy nothing.
    std::string_view name;
    std::span<const std::byte> bytes;
};

/// What the host does on the mod's behalf, and what it will not do more than so often.
struct ModHostConfig {
    /// The simulation's seed, so `atlas_random` draws what every other peer draws.
    std::uint64_t seed = 0;

    /// How far ahead a mod's commands are stamped.
    ///
    /// Must be at least one: a command stamped for the tick currently being decided would be
    /// drained before the mod that produced it had finished producing. Under lockstep this
    /// should match the session's agreed input delay, so a mod's commands and a peer's arrive
    /// on the same schedule.
    Tick input_delay = 1;

    /// Commands one mod may submit in one tick.
    ///
    /// A ceiling that disables a runaway rather than a budget to spend, like every other limit
    /// here. The command queue has its own bound at a million pending; this one exists so that
    /// reaching it is one mod's problem rather than the simulation's.
    std::uint32_t max_commands_per_tick = 1024;

    /// Bytes of log one mod may produce in one tick, after which lines are dropped and counted.
    /// A mod that logs every cell it looks at should not be able to fill a disk.
    std::size_t max_log_bytes_per_tick = 4096;
    /// Messages one mod may say in one tick, after which `atlas_say` refuses (ADR-0021 D4).
    /// A person reads perhaps one a second; a mod saying more than a few in a single tick has
    /// gone wrong, so this disables a runaway rather than rationing a conversation.
    std::uint32_t max_messages_per_tick = 4;
    /// Messages held for the application between drains. Past this a message is dropped and
    /// counted rather than queued, so an application that never drains cannot be made to grow
    /// without bound by a mod that talks.
    std::size_t max_queued_messages = 64;
};

/// One thing a mod said, for the application to resolve through its catalogue and show.
///
/// The key is complete — the host has prefixed the mod's namespace — and the arguments are the
/// integers the mod passed, to be formatted and substituted by `text::substitute`. Nothing here
/// is text a person reads until the application has looked it up.
struct ModMessage {
    Tick tick = 0;
    std::string key;
    std::array<std::int64_t, ATLAS_MOD_MAX_SAY_ARGS> args{};
    std::uint8_t arg_count = 0;

    [[nodiscard]] std::span<const std::int64_t> arguments() const noexcept {
        return std::span(args).first(arg_count);
    }
};

/// The namespace every key a mod says or ships lives under: `mod.<stem>.`, where the stem is the
/// mod's name without a `.wasm` extension. Computed in one place so what the host prefixes and
/// what a table is checked against cannot disagree.
[[nodiscard]] std::string mod_key_prefix(std::string_view mod_name);

/// Whether a mod's own string table may be added to an application's catalogue (ADR-0021 D3).
///
/// Fails, naming the first offender, when any key does not begin with `mod_key_prefix`: a mod
/// may add words under its own name and nobody else's. The application refuses the whole table
/// on failure and loads the mod anyway, whose keys then show as themselves.
[[nodiscard]] Status check_mod_table(const assets::ImportedStringTable& table,
                                     std::string_view mod_name);

/// What one mod did, cumulatively. For statistics, for tests, and for a report.
struct ModHostStats {
    std::uint64_t commands_submitted = 0;
    std::uint64_t commands_refused = 0;
    std::uint64_t log_lines_dropped = 0;
    std::uint64_t ticks_run = 0;
    /// Messages queued for the application, and messages refused for the budget or dropped for
    /// a full queue.
    std::uint64_t messages_said = 0;
    std::uint64_t messages_dropped = 0;
};

class ModHost final : public sim::CommandSource {
  public:
    /// Load a mod and wire it to an identifier.
    ///
    /// `mod_index` is this mod's position among the mods this machine has loaded; the identifier
    /// becomes `sim::mod_source(mod_index)`, which has bit 31 set and can therefore never
    /// collide with a peer. It is local to this machine and never sent.
    ///
    /// Fails for anything `Mod::load` fails for, and for an `input_delay` of zero.
    /// Handed out by pointer because a `CommandSource` is deliberately neither copyable nor
    /// movable, exactly as `net::Session` is: whatever holds a reference to one must be able to
    /// rely on it staying where it was put.
    [[nodiscard]] static Result<std::unique_ptr<ModHost>>
    create(Runtime& runtime, std::uint32_t mod_index, std::span<const std::byte> bytes,
           std::string_view name, const ModHostConfig& config = {}, const ModLimits& limits = {});

    ModHost(const ModHost&) = delete;
    ModHost& operator=(const ModHost&) = delete;
    ModHost(ModHost&&) = delete;
    ModHost& operator=(ModHost&&) = delete;
    ~ModHost() override;

    /// Publish what the mod may read for the coming tick.
    ///
    /// Replaces whatever was published before. Called by the driver after recomputing its views
    /// from the world and before `poll`; a mod polled with no views simply reads none, which is
    /// a mod with nothing to decide on rather than an error.
    void set_views(std::span<const ModView> views);

    /// This mod's identifier. Always has `sim::kModSourceBit` set.
    [[nodiscard]] sim::SourceId id() const noexcept override;

    /// Run the mod for one tick and collect what it submitted.
    ///
    /// The turn gate is not touched; see the note at the top of this file. A mod that has been
    /// disabled polls successfully and produces nothing, because that is what a mod which is no
    /// longer there looks like from the simulation's side.
    ///
    /// Never fails because the mod misbehaved: a trap, an exhausted budget or a refused command
    /// disables the mod and is reported in the `PollReport`, not as an error. An error here
    /// would stop the driver polling every other source, which is precisely what one bad mod
    /// must not be able to do.
    [[nodiscard]] Result<sim::PollReport> poll(Tick now, sim::CommandQueue& queue,
                                               sim::TurnGate& turns) override;

    /// Call `mod_init`. Must be called once before the first `poll`.
    [[nodiscard]] Status start();

    [[nodiscard]] bool disabled() const noexcept;
    [[nodiscard]] std::string_view disabled_because() const noexcept;
    [[nodiscard]] std::string_view name() const noexcept;
    [[nodiscard]] ModHostStats stats() const noexcept;

    /// Everything the mod has said since the last call, oldest first, and the queue emptied.
    /// Called by the application once a frame; what it does with them is its own (ADR-0021 D5).
    [[nodiscard]] std::vector<ModMessage> take_messages();

  private:
    ModHost();

    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace atlas::script
