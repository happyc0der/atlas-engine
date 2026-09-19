// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Strategy Lab: the composition root that drives the M6 kernel with a window, a device,
// a field of cells and an overlay, or headless with none of them. Nothing in here is a game.
//
// The kernel's tick is authoritative. The accumulator only decides how many ticks to run this
// frame; it is committed with exactly what the kernel ran, re-synchronised after a load, and
// compared against the kernel once per frame under a debug assertion.

#include <atlas/app/frame_counters.hpp>
#include <atlas/app/log_options.hpp>
#include <atlas/app/main_guard.hpp>
#include <atlas/app/ppm.hpp>
#include <atlas/app/run_bounds.hpp>
#include <atlas/assets/filesystem.hpp>
#include <atlas/assets/virtual_path.hpp>
#include <atlas/audio/device.hpp>
#include <atlas/audio/synth.hpp>
#include <atlas/core/args.hpp>
#include <atlas/core/assert.hpp>
#include <atlas/core/build_info.hpp>
#include <atlas/core/hash.hpp>
#include <atlas/core/log.hpp>
#include <atlas/core/profile.hpp>
#include <atlas/core/time.hpp>
#include <atlas/lab/cell_field.hpp>
#include <atlas/lab/cell_id_pass.hpp>
#include <atlas/lab/commands.hpp>
#include <atlas/lab/generate.hpp>
#include <atlas/lab/mod_views.hpp>
#include <atlas/lab/snapshot.hpp>
#include <atlas/lab/systems.hpp>
#include <atlas/net/loopback.hpp>
#include <atlas/net/session.hpp>
#include <atlas/platform/platform.hpp>
#include <atlas/rhi/device.hpp>
#include <atlas/script/mod_host.hpp>
#include <atlas/script/runtime.hpp>
#include <atlas/simulation/kernel.hpp>
#include <atlas/simulation/replay.hpp>
#include <atlas/simulation/save.hpp>
#include <atlas/simulation/snapshot.hpp>
#include <atlas/simulation/tick_accumulator.hpp>
#include <atlas/simulation/turn_gate.hpp>
#include <atlas/tasks/worker_pool.hpp>
#include <atlas/tools/debug_ui.hpp>
#include <atlas/tools/panels.hpp>

#include "file_bytes.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace {

constexpr atlas::log::Category kApp = atlas::log::category::kApp;

#ifdef NDEBUG
constexpr bool kDebugBuild = false;
#else
constexpr bool kDebugBuild = true;
#endif

struct Options {
    bool headless = false;
    bool unbounded = false;
    bool no_overlay = false;
    bool no_render = false;
    bool no_gamepad = false;
    bool no_audio = false;
    bool start_paused = false;
    std::string_view video_driver;
    std::string_view audio_driver;
    std::string_view log_level;
    std::string_view log_file;
    std::string_view shader_dir;
    /// A mod to load from the `mods` mount, or empty for none.
    std::string_view mod;
    std::string_view mods_dir;
    /// Offer mods a host clock. Unsafe, and named so: see --help.
    bool unsafe_debug_imports = false;
    std::string_view screenshot;
    std::string_view save_path;
    std::string_view load_path;
    std::string_view record_path;
    std::string_view play_path;
    std::string_view pick;
    atlas::lab::MapMode map_mode = atlas::lab::MapMode::ColorIndex;
    std::uint32_t grid = 1024;
    std::uint32_t chunk = 32;
    std::uint32_t ticks_per_second = 60;
    std::uint32_t commands_per_tick = 0;
    /// Zero means no loopback session; two or more runs that many peers in this process.
    std::uint32_t loopback_peers = 0;
    std::uint32_t input_delay = 2;
    std::uint32_t link_latency = 0;
    std::uint64_t link_reorder = 0;
    std::string_view loopback_fault;
    /// Polls without progress before a held message is released. Counted in polls rather than
    /// ticks because a stalled run's tick is the thing that stops advancing.
    std::uint64_t loopback_release_after = 0;
    std::uint32_t max_ticks_per_frame = 8;
    std::size_t workers = 0;
    bool workers_set = false;
    std::uint64_t seed = 1;
    std::uint64_t max_frames = 0;
    std::uint64_t max_ticks = 0;
};

void print_usage() {
    std::puts(R"(atlas_lab - the Strategy Lab: a synthetic grid driven by the simulation kernel

Options:
  --grid N               Cells per side of a square grid. Default 1024 (a million cells).
  --chunk N              Cells per side of a chunk; N must divide --grid. Default 32.
  --seed S               Generation and kernel seed. Default 1.
  --tps N                Ticks per second. Default 60.
  --unbounded            Run ticks as fast as possible.
  --paused               Start paused.
  --loopback-peers N     Run N peers in this process over an in-memory link and check that
                         they agree hash for hash. 2..8. Default 0, meaning off.
  --input-delay N        Ticks between stamping a command and running it. 1..16. Default 2.
  --link-latency N       Delivery delay, in receiver polls rather than ticks. 0..16.
  --link-reorder SEED    Reorder messages within a poll, driven by SEED. 0 is off.
  --loopback-fault SPEC  drop:T, corrupt:T or hold:T — one fault from peer 1 to peer 0 at
                         tick T.
  --loopback-release-after N  Release held messages once the run has made no progress for N
                         polls. Counted in polls rather than ticks because a stalled run's
                         tick is exactly what stops advancing. Use with hold:T.
  --commands-per-tick N  Submit N deterministic set_color_index commands each tick.
  --mod NAME             Load a sandboxed WebAssembly mod from the mods directory and run it
                         once per tick, before that tick. Its commands are stamped with its own
                         identifier and never sent to peers: every peer runs the same mod and
                         produces the same commands, which is what the hash check verifies.
  --mods-dir PATH        Where --mod looks. Default: assets/mods.
  --unsafe-debug-imports Offer mods a host clock, which the interface deliberately does not
                         have. A mod that reads one decides differently on a slower machine,
                         so under lockstep this produces a divergence — which is the only
                         reason it exists. Never use it for anything but that demonstration.
  --workers N            Worker threads beside this one for the simulation's compute phase.
                         Default: one per hardware thread beyond this one. Zero runs everything
                         on the calling thread. The state hash is the same at every setting,
                         which apps/lab/sim/tests/test_parallel.cpp checks.
  --max-ticks-per-frame N
                         Catch-up limit; ticks beyond it are dropped and counted. Default 8,
                         the tick scheduler's own. It was 2 while a million-cell tick cost
                         tens of milliseconds; at 2 ms a lower limit only stops the simulation
                         catching up after a slow frame, which measurably drops more ticks.
  --ticks N              Stop after exactly N ticks.
  --frames N             Stop after N frames.
  --headless             No window, no device, no snapshot. Needs --ticks or --frames.
  --map-mode NAME        region | owner | population | colour. Default colour.
  --save PATH            Save at exit; F5 saves to the same path during a run.
  --load PATH            Load before the first tick; F9 reloads it during a run.
  --record PATH          Record a replay and write it at exit.
  --play PATH            Play a replay against a fresh world and report; exits 1 on divergence.
  --screenshot PATH      Write the last frame as a PPM.
  --pick X,Y             Pick the cell under logical window point (X, Y) on the third frame,
                         as a click would, and log what the identifier pass and the
                         projection's inverse each say. For the integration checks.
  --video-driver NAME    SDL video driver, e.g. dummy.
  --shader-dir PATH      Where the cooked shaders are. Default assets/cooked/shaders.
  --no-overlay           Do not create the debug overlay.
  --no-render            Open the window but create no graphics device; for the dummy driver.
  --no-gamepad           Do not enumerate gamepads. On by default when there is a window.
  --no-audio             Open no audio device. On by default when there is a window.
  --audio-driver NAME    SDL audio driver, e.g. dummy.
  --log-level LEVEL      trace | debug | info | warning | error | fatal. Default info.
  --log-file PATH        Also write the log to a file.
  --version              Print build identity and exit.
  --help                 Print this message and exit.

Keys: Space pause, Period single step, 1-4 speeds, U unbounded, M map mode, R reset camera,
F5 save, F9 load, Escape quit. Right-drag pans, wheel zooms, left-click recolours a cell.)");
}

[[nodiscard]] atlas::Result<atlas::lab::MapMode> parse_map_mode(std::string_view name) {
    using atlas::lab::MapMode;
    if (name == "region") {
        return MapMode::RegionValue;
    }
    if (name == "owner") {
        return MapMode::OwnerIndex;
    }
    if (name == "population") {
        return MapMode::PopulationValue;
    }
    if (name == "colour" || name == "color") {
        return MapMode::ColorIndex;
    }
    return std::unexpected(atlas::Error(
        atlas::ErrorCode::InvalidArgument,
        std::format("unknown map mode '{}'; expected region, owner, population or colour", name)));
}

[[nodiscard]] atlas::Result<std::uint64_t> bounded(const atlas::Args& args, std::string_view name,
                                                   std::uint64_t fallback, std::uint64_t low,
                                                   std::uint64_t high) {
    const auto value = args.value_or(name, fallback);
    if (!value) {
        return std::unexpected(value.error());
    }
    if (*value < low || *value > high) {
        return std::unexpected(
            atlas::Error(atlas::ErrorCode::InvalidArgument,
                         std::format("--{} must be in [{}, {}], got {}", name, low, high, *value)));
    }
    return *value;
}

[[nodiscard]] atlas::Result<Options> read_options(const atlas::Args& args) {
    Options options;
    options.headless = args.has("headless");
    options.unbounded = args.has("unbounded");
    options.no_overlay = args.has("no-overlay");
    options.no_render = args.has("no-render");
    options.no_gamepad = args.has("no-gamepad");
    options.no_audio = args.has("no-audio");
    options.unsafe_debug_imports = args.has("unsafe-debug-imports");
    options.start_paused = args.has("paused");
    options.video_driver = args.value_or("video-driver", std::string_view{});
    options.audio_driver = args.value_or("audio-driver", std::string_view{});
    options.log_level = args.value_or("log-level", std::string_view{"info"});
    options.log_file = args.value_or("log-file", std::string_view{});
    options.shader_dir = args.value_or("shader-dir", std::string_view{"assets/cooked/shaders"});
    options.mod = args.value_or("mod", std::string_view{});
    options.mods_dir = args.value_or("mods-dir", std::string_view{"assets/mods"});
    options.screenshot = args.value_or("screenshot", std::string_view{});
    options.save_path = args.value_or("save", std::string_view{});
    options.load_path = args.value_or("load", std::string_view{});
    options.record_path = args.value_or("record", std::string_view{});
    options.play_path = args.value_or("play", std::string_view{});
    options.pick = args.value_or("pick", std::string_view{});

    auto mode = parse_map_mode(args.value_or("map-mode", std::string_view{"colour"}));
    if (!mode) {
        return std::unexpected(mode.error());
    }
    options.map_mode = *mode;

    const auto grid = bounded(args, "grid", 1024, 1, 2048);
    if (!grid) {
        return std::unexpected(grid.error());
    }
    options.grid = static_cast<std::uint32_t>(*grid);
    const auto chunk = bounded(args, "chunk", 32, 1, atlas::lab::GridLayout::kMaxChunkSize);
    if (!chunk) {
        return std::unexpected(chunk.error());
    }
    options.chunk = static_cast<std::uint32_t>(*chunk);
    const auto seed = args.value_or("seed", std::uint64_t{1});
    if (!seed) {
        return std::unexpected(seed.error());
    }
    options.seed = *seed;
    const auto tps = bounded(args, "tps", 60, 1, atlas::sim::TickAccumulator::kMaxTicksPerSecond);
    if (!tps) {
        return std::unexpected(tps.error());
    }
    options.ticks_per_second = static_cast<std::uint32_t>(*tps);
    const auto per_tick = bounded(args, "commands-per-tick", 0, 0, 10'000);
    if (!per_tick) {
        return std::unexpected(per_tick.error());
    }
    options.commands_per_tick = static_cast<std::uint32_t>(*per_tick);

    // Zero is off. One peer is a solo run, which is what this binary does without the option at
    // all, so it is refused rather than quietly meaning the same thing.
    const auto peers = bounded(args, "loopback-peers", 0, 0, 8);
    if (!peers) {
        return std::unexpected(peers.error());
    }
    if (*peers == 1) {
        return std::unexpected(atlas::Error(
            atlas::ErrorCode::InvalidArgument,
            "--loopback-peers 1 is a solo run, which is what this binary does without the "
            "option; use 2 or more, or leave it out"));
    }
    options.loopback_peers = static_cast<std::uint32_t>(*peers);

    const auto delay = bounded(args, "input-delay", 2, 1, 16);
    if (!delay) {
        return std::unexpected(delay.error());
    }
    options.input_delay = static_cast<std::uint32_t>(*delay);

    const auto latency = bounded(args, "link-latency", 0, 0, 16);
    if (!latency) {
        return std::unexpected(latency.error());
    }
    options.link_latency = static_cast<std::uint32_t>(*latency);

    const auto reorder = bounded(args, "link-reorder", 0, 0, ~std::uint64_t{0});
    if (!reorder) {
        return std::unexpected(reorder.error());
    }
    options.link_reorder = *reorder;

    options.loopback_fault = args.value_or("loopback-fault", std::string_view{});
    const auto release_at = bounded(args, "loopback-release-after", 0, 0, ~std::uint64_t{0});
    if (!release_at) {
        return std::unexpected(release_at.error());
    }
    options.loopback_release_after = *release_at;
    if (args.has("workers")) {
        const auto workers = bounded(args, "workers", 0, 0, atlas::tasks::WorkerPool::kMaxWorkers);
        if (!workers) {
            return std::unexpected(workers.error());
        }
        options.workers = static_cast<std::size_t>(*workers);
        options.workers_set = true;
    }
    const auto per_frame = bounded(args, "max-ticks-per-frame", 8, 1, 64);
    if (!per_frame) {
        return std::unexpected(per_frame.error());
    }
    options.max_ticks_per_frame = static_cast<std::uint32_t>(*per_frame);
    const auto frames = args.value_or("frames", std::uint64_t{0});
    if (!frames) {
        return std::unexpected(frames.error());
    }
    options.max_frames = *frames;
    const auto ticks = args.value_or("ticks", std::uint64_t{0});
    if (!ticks) {
        return std::unexpected(ticks.error());
    }
    options.max_ticks = *ticks;

    if (const auto status = args.reject_unknown(); !status) {
        return std::unexpected(status.error());
    }
    // A playback is its own bound: it runs the recorded tick count and stops.
    if (options.play_path.empty()) {
        if (auto bound = atlas::app::validate_headless_bound(options.headless, options.max_frames,
                                                             options.max_ticks);
            !bound) {
            return std::unexpected(std::move(bound).error());
        }
    }
    return options;
}

// ------------------------------------------------------------------------------ simulation

/// Everything the kernel needs, owned together so the references the kernel holds outlive it.
struct Simulation {
    atlas::lab::LabWorld lab;
    atlas::sim::Schedule schedule;
    atlas::sim::CommandQueue commands;
    std::shared_ptr<atlas::lab::CellBound> bound = std::make_shared<atlas::lab::CellBound>(0);
    std::unique_ptr<atlas::sim::Kernel> kernel;  ///< Always set once make_simulation returns.
};

/// The script runtime, and the mod's bytes, read once for however many peers want them.
///
/// The runtime is process-wide — WAMR's initialisation is, so `script::Runtime` is too — which
/// is why a loopback run with four peers has one of these and four hosts rather than four of
/// everything. A host must not outlive it: WAMR's allocator belongs to the runtime, so a mod
/// freed afterwards would hand memory back to something that no longer exists.
struct ModRuntime {
    std::optional<atlas::script::Runtime> runtime;
    std::vector<std::byte> bytes;

    [[nodiscard]] bool wanted() const noexcept { return runtime.has_value(); }
};

/// One peer's mod, and the view buffer it reads.
struct ModPlayer {
    std::unique_ptr<atlas::script::ModHost> host;
    /// Owned here because a view is a span over somebody else's bytes, and these are the bytes.
    atlas::lab::LayoutView layout;

    [[nodiscard]] bool loaded() const noexcept { return host != nullptr; }
};

/// Read `--mod` and bring the runtime up, or do nothing at all when it was not given.
///
/// The bytes come through `assets::VirtualPath` and a mounted root, exactly as a save file does,
/// because a mod is untrusted input in the same way. The lab has never had a filesystem and said
/// so at the audio call site — "giving it a filesystem and a mount to play one click would be a
/// larger change than the audio module itself" — and that reasoning was about proportion. A mod
/// changes the proportion: `VirtualPath` is the only validated way to turn a name from the
/// command line into a file inside a directory, and rolling that by hand for untrusted input is
/// precisely what this project does not do.
[[nodiscard]] atlas::Result<ModRuntime> open_mod_runtime(const Options& options) {
    ModRuntime opened;
    if (options.mod.empty()) {
        return opened;
    }

    atlas::assets::FileSystem files;
    if (auto status = files.mount("mods", std::filesystem::path(options.mods_dir)); !status) {
        return std::unexpected(std::move(status).error().context("mounting the mods directory"));
    }
    auto path = atlas::assets::VirtualPath::parse(options.mod);
    if (!path) {
        return std::unexpected(std::move(path).error().context("the mod's name"));
    }
    auto bytes = files.read(*path);
    if (!bytes) {
        return std::unexpected(std::move(bytes).error().context("reading the mod"));
    }
    opened.bytes = *std::move(bytes);

    if (options.unsafe_debug_imports) {
        // Said out loud, at warning level, every time. A run that offers a mod a clock is a run
        // whose results mean nothing, and the log is where somebody reading an unexpected
        // divergence will look first.
        ATLAS_LOG_WARN(kApp, "--unsafe-debug-imports: mods are offered a host clock; a mod that "
                             "reads one will diverge under lockstep, which is the only thing "
                             "this flag is for");
    }
    auto runtime =
        atlas::script::Runtime::create({.unsafe_debug_imports = options.unsafe_debug_imports});
    if (!runtime) {
        return std::unexpected(std::move(runtime).error());
    }
    opened.runtime = *std::move(runtime);
    return opened;
}

/// Give one simulation its own instance of the mod.
///
/// **Every peer uses the same index and the same seed**, unlike the synthetic command stream,
/// which deliberately differs per peer. A mod's commands are never sent: each peer runs the mod
/// itself and must reach the same answer, so anything that differs between peers here would be
/// a divergence rather than a variation.
[[nodiscard]] atlas::Result<ModPlayer> attach_mod(ModRuntime& shared, const Options& options,
                                                  const Simulation& sim) {
    ModPlayer player;
    // Tested on the member rather than through `wanted()`, so that both a reader and the
    // analyser can see that the dereference below is guarded.
    if (!shared.runtime.has_value()) {
        return player;
    }

    auto host = atlas::script::ModHost::create(
        *shared.runtime, 0, shared.bytes, options.mod,
        {.seed = options.seed,
         .input_delay = options.loopback_peers > 1 ? options.input_delay : 1});
    if (!host) {
        return std::unexpected(std::move(host).error());
    }
    player.host = *std::move(host);
    if (auto status = player.host->start(); !status) {
        return std::unexpected(std::move(status).error().context("starting the mod"));
    }
    player.layout = atlas::lab::layout_view(sim.lab.layout);
    ATLAS_LOG_INFO(kApp, "mod '{}' loaded as source {}", options.mod,
                   static_cast<std::uint32_t>(player.host->id()));
    return player;
}

/// Publish this tick's views and let the mod decide, before the tick runs.
///
/// **Once per tick and never once per frame.** Peers run different numbers of frames per tick,
/// so a per-frame call would make the number of mod invocations depend on frame rate, and
/// therefore the commands, and therefore the state.
void run_mod_for_tick(ModPlayer& player, Simulation& sim, atlas::Tick tick,
                      atlas::sim::TurnGate& gate) {
    if (!player.loaded()) {
        return;
    }
    const std::array<atlas::script::ModView, 2> views{
        atlas::script::ModView{.name = "layout", .bytes = player.layout.bytes},
        atlas::script::ModView{.name = "colour",
                               .bytes = atlas::lab::color_view(sim.lab.world, sim.lab.ids)},
    };
    player.host->set_views(views);
    if (const auto report = player.host->poll(tick, sim.commands, gate); !report) {
        // Cannot happen by design — a mod that misbehaves is reported closed rather than as an
        // error — but a driver that ignored it would be assuming that on the reader's behalf.
        ATLAS_LOG_ERROR(kApp, "polling the mod failed: {}", report.error());
    }
}

/// One line saying what a mod did, so an integration case has something to assert on.
void report_mod(const ModPlayer& player, std::string_view who) {
    if (!player.loaded()) {
        return;
    }
    const auto stats = player.host->stats();
    ATLAS_LOG_INFO(kApp,
                   "{}mod '{}': {} submitted, {} refused, {} log line(s) dropped over {} "
                   "tick(s), {}",
                   who, player.host->name(), stats.commands_submitted, stats.commands_refused,
                   stats.log_lines_dropped, stats.ticks_run,
                   player.host->disabled()
                       ? std::string("disabled: ") + std::string(player.host->disabled_because())
                       : std::string("still running"));
}

/// `gate` is borrowed and optional: null is a solo run, which is every run but a loopback one.
/// A kernel given no gate never refuses a tick, which is the path this binary has always taken.
[[nodiscard]] atlas::Result<std::unique_ptr<Simulation>>
make_simulation(const Options& options, atlas::tasks::WorkerPool* pool,
                const atlas::sim::TurnGate* gate = nullptr) {
    auto simulation = std::make_unique<Simulation>();
    auto generated = atlas::lab::generate({.width = options.grid,
                                           .height = options.grid,
                                           .chunk_size = options.chunk,
                                           .seed = options.seed});
    if (!generated) {
        return std::unexpected(std::move(generated).error().context("generating the world"));
    }
    simulation->lab = std::move(*generated);
    simulation->bound->store(simulation->lab.layout.cell_count());
    if (auto status = atlas::lab::add_lab_systems(simulation->schedule, simulation->lab.ids);
        !status) {
        return std::unexpected(std::move(status).error().context("adding the lab systems"));
    }
    if (auto status = simulation->schedule.finalise(simulation->lab.world); !status) {
        return std::unexpected(std::move(status).error().context("finalising the schedule"));
    }
    if (auto status = atlas::lab::register_lab_commands(simulation->commands, simulation->lab.ids,
                                                        simulation->bound);
        !status) {
        return std::unexpected(std::move(status).error().context("registering commands"));
    }
    simulation->kernel = std::make_unique<atlas::sim::Kernel>(
        simulation->lab.world, simulation->schedule, simulation->commands,
        atlas::sim::KernelConfig{
            .seed = options.seed,
            // Per-system hashes exist to attribute a replay divergence
            // to a system, so they are recorded only when recording.
            // Measured at a million cells: the world hash is 11.8 ms of
            // a 13.4 ms tick, and the per-system hashes add about as
            // much again. See docs/PERFORMANCE.md.
            // And for a loopback run, whatever the recording options say: a divergence
            // between peers that cannot be attributed to a system is exactly what the hash
            // checks exist to avoid.
            .record_system_hashes = !options.record_path.empty() || options.loopback_peers > 0,
            .record_applied_commands = !options.record_path.empty(),
            .pool = pool,
            .gate = gate,
        });
    return simulation;
}

/// What sim::load cannot do on its own: check the tables belong together, move the bound and
/// the accumulator to the loaded state, and rebuild geometry if the layout changed. One named
/// function, because the last two steps are the easy ones to forget.
[[nodiscard]] atlas::Result<atlas::lab::GridLayout>
apply_loaded_state(Simulation& simulation, atlas::sim::TickAccumulator& accumulator,
                   atlas::lab::CellField* field) {
    auto layout = atlas::lab::validate_world(simulation.lab.world, simulation.lab.ids);
    if (!layout) {
        return std::unexpected(std::move(layout).error().context("the loaded world"));
    }
    simulation.bound->store(layout->cell_count());
    accumulator.set_tick(simulation.kernel->current_tick());
    if (*layout != simulation.lab.layout) {
        ATLAS_LOG_INFO(kApp, "the loaded world is {}x{} in chunks of {}; rebuilding geometry",
                       layout->width(), layout->height(), layout->chunk_size());
        simulation.lab.layout = *layout;
        if (field != nullptr) {
            if (auto s = field->set_layout(*layout); !s) {
                ATLAS_LOG_ERROR(kApp, "resizing the cell field failed: {}", s.error());
            }
        }
    }
    return *layout;
}

/// Load from a file with the application-level guarantee that a failed load changes nothing.
/// sim::load promises that for its own parsing; the cross-table check runs after it and can
/// still refuse, so the state is serialised first and restored if it does.
[[nodiscard]] atlas::Status load_guarded(const std::filesystem::path& path, Simulation& simulation,
                                         atlas::sim::TickAccumulator& accumulator,
                                         atlas::lab::CellField* field) {
    auto backup = atlas::sim::save(simulation.lab.world, *simulation.kernel, simulation.commands);
    if (!backup) {
        return std::unexpected(
            std::move(backup).error().context("preserving the current state before a load"));
    }
    if (auto status = atlas::lab::load_world_from(path, simulation.lab.world, *simulation.kernel,
                                                  simulation.commands);
        !status) {
        return std::unexpected(std::move(status).error().context("loading"));
    }
    auto applied = apply_loaded_state(simulation, accumulator, field);
    if (!applied) {
        if (auto restored = atlas::sim::load(simulation.lab.world, *simulation.kernel,
                                             simulation.commands, *backup);
            !restored) {
            ATLAS_LOG_ERROR(kApp, "restoring the state after a refused load failed: {}",
                            restored.error());
        }
        return std::unexpected(std::move(applied).error());
    }
    ATLAS_LOG_INFO(kApp, "loaded '{}': tick {} hash {:#018x}", path.string(),
                   simulation.kernel->current_tick(), simulation.lab.world.hash());
    return atlas::ok();
}

/// Run several peers in this process and check that they agree.
///
/// This is M14's whole-program proof. Everything the engine tests in isolation — the gate, the
/// codec, the inbox, the link, the session — is wired together here over the lab's real
/// simulation, and the binary itself refuses to exit zero unless every peer reached the same
/// state. A script comparing two printed numbers is a fine second opinion; the binary refusing
/// is what makes the property hold for every loopback run anybody ever does.
///
/// All of it lives here rather than in `lab_sim`, which is fenced at configure time to link
/// nothing but `atlas::simulation` so that the headless benchmark stays independent of anything
/// that draws.
[[nodiscard]] atlas::Status run_loopback(const Options& options) {
    const std::size_t peer_count = options.loopback_peers;

    // Parse the fault before building anything, so a typo fails immediately rather than after a
    // world has been generated for every peer.
    std::optional<atlas::net::LinkFault> fault;
    atlas::Tick fault_at = 0;
    if (!options.loopback_fault.empty()) {
        const auto colon = options.loopback_fault.find(':');
        if (colon == std::string_view::npos) {
            return atlas::fail(atlas::ErrorCode::InvalidArgument,
                               "--loopback-fault wants drop:TICK, corrupt:TICK or hold:TICK");
        }
        const std::string_view kind = options.loopback_fault.substr(0, colon);
        if (kind == "drop") {
            fault = atlas::net::LinkFault::Drop;
        } else if (kind == "corrupt") {
            fault = atlas::net::LinkFault::Corrupt;
        } else if (kind == "hold") {
            fault = atlas::net::LinkFault::Hold;
        } else {
            return atlas::fail(atlas::ErrorCode::InvalidArgument,
                               std::format("'{}' is not a fault this build has; use drop, "
                                           "corrupt or hold",
                                           kind));
        }
        const auto tick_text = options.loopback_fault.substr(colon + 1);
        std::uint64_t parsed = 0;
        for (const char digit : tick_text) {
            if (digit < '0' || digit > '9') {
                return atlas::fail(atlas::ErrorCode::InvalidArgument,
                                   "--loopback-fault wants a whole tick after the colon");
            }
            parsed = (parsed * 10) + static_cast<std::uint64_t>(digit - '0');
        }
        fault_at = parsed;
    }

    auto hub = atlas::net::LoopbackHub::create({
        .peer_count = peer_count,
        .latency_polls = options.link_latency,
        .reorder = options.link_reorder != 0,
        .reorder_seed = options.link_reorder,
    });
    if (!hub) {
        return std::unexpected(std::move(hub).error());
    }

    struct Participant {
        std::unique_ptr<Simulation> sim;
        atlas::sim::TurnGate gate;
        std::unique_ptr<atlas::net::Session> session;
        /// Each peer runs its own instance of the same mod, with the same identifier and the
        /// same seed. Its commands are never sent: the whole proof is that two machines running
        /// the same mod produce the same commands without exchanging them.
        ModPlayer mod;
        atlas::Tick next_turn = 0;
        std::uint64_t last_hash = 0;
        std::size_t applied = 0;
    };

    auto mod_runtime = open_mod_runtime(options);
    if (!mod_runtime) {
        return std::unexpected(std::move(mod_runtime).error());
    }

    std::vector<std::unique_ptr<Participant>> peers;
    peers.reserve(peer_count);
    std::uint64_t initial_hash = 0;

    for (std::size_t i = 0; i < peer_count; ++i) {
        auto participant = std::make_unique<Participant>();
        // No worker pool. One pool per peer would spawn N times the hardware's worth of
        // threads and one shared pool would make the peers contend; neither changes results,
        // because the hash is worker-count independent, and both make the timing meaningless.
        auto made = make_simulation(options, nullptr, &participant->gate);
        if (!made) {
            return std::unexpected(std::move(made).error());
        }
        participant->sim = *std::move(made);

        auto player = attach_mod(*mod_runtime, options, *participant->sim);
        if (!player) {
            return std::unexpected(std::move(player).error());
        }
        participant->mod = *std::move(player);

        // Per-system hashes on, whatever the recording options say: a divergence that cannot be
        // attributed to a system is exactly what the hash checks exist to avoid.

        const std::uint64_t hash = participant->sim->lab.world.hash();
        if (i == 0) {
            initial_hash = hash;
        } else if (hash != initial_hash) {
            // Checked before the handshake would catch it, so that a generation bug is reported
            // as a generation bug rather than blamed on the network.
            return atlas::fail(
                atlas::ErrorCode::Internal,
                std::format("peer {} generated a different world from peer 0 ({:#018x} against "
                            "{:#018x}); nothing has been sent yet, so this is world generation "
                            "and not the link",
                            i, hash, initial_hash));
        }

        auto session = atlas::net::Session::create(
            (*hub)->end(i), {
                                .input_delay = options.input_delay,
                                .hash_check_interval = 16,
                                .seed = options.seed,
                                .start_tick = 0,
                                .initial_state_hash = hash,
                                .tick_rate = options.ticks_per_second,
                                .build_id = std::string{atlas::build_info::summary()},
                            });
        if (!session) {
            return std::unexpected(std::move(session).error());
        }
        participant->session = *std::move(session);
        participant->session->set_schedule(&participant->sim->schedule);
        peers.push_back(std::move(participant));
    }

    ATLAS_LOG_INFO(kApp, "loopback: {} peers, delay {}, latency {} poll(s), reorder {}", peer_count,
                   options.input_delay, options.link_latency, options.link_reorder);

    // Settle the handshake before anything is announced. Every peer has to have agreed the
    // session — and learned its own identifier — before it can stamp a command with it, and
    // under latency that takes a few polls rather than one. Bounded, so a handshake that cannot
    // complete says so instead of hanging.
    {
        constexpr int kMaxHandshakePolls = 256;
        int polls = 0;
        bool agreed = false;
        while (!agreed && polls < kMaxHandshakePolls) {
            for (auto& peer : peers) {
                auto report = peer->session->poll(0, peer->sim->commands, peer->gate);
                if (!report) {
                    return std::unexpected(std::move(report).error().context("the handshake"));
                }
            }
            agreed = std::ranges::all_of(peers,
                                         [](const auto& peer) { return peer->session->running(); });
            ++polls;
        }
        if (!agreed) {
            return atlas::fail(atlas::ErrorCode::Unavailable,
                               std::format("the loopback handshake did not complete in {} polls",
                                           kMaxHandshakePolls));
        }
        ATLAS_LOG_INFO(kApp, "loopback handshake agreed in {} poll(s), input delay {}", polls,
                       peers.front()->session->agreed_delay());
    }

    // A stall makes no progress by design, so bounding by --ticks alone would let an unreleased
    // hold run until the harness killed it — a timeout with no message rather than a failure
    // that says what is waiting for what.
    constexpr std::uint64_t kMaxPollsWithoutProgress = 10'000;
    std::uint64_t idle_polls = 0;
    bool released = options.loopback_release_after == 0;
    std::uint64_t divergences = 0;
    std::uint64_t stalls = 0;

    const auto poll_all = [&] -> atlas::Status {
        for (auto& peer : peers) {
            auto report = peer->session->poll(peer->sim->kernel->current_tick(),
                                              peer->sim->commands, peer->gate);
            if (!report) {
                return std::unexpected(std::move(report).error());
            }
        }
        return atlas::ok();
    };

    while (true) {
        atlas::Tick lowest = peers.front()->sim->kernel->current_tick();
        for (const auto& peer : peers) {
            lowest = std::min(lowest, peer->sim->kernel->current_tick());
        }
        if (options.max_ticks != 0 && lowest >= options.max_ticks) {
            break;
        }

        if (fault.has_value() && lowest >= fault_at) {
            if (auto status = (*hub)->arm_fault(1, 0, *fault); !status) {
                return status;
            }
            fault.reset();
        }

        if (auto status = poll_all(); !status) {
            for (std::size_t i = 0; i < peers.size(); ++i) {
                if (peers[i]->session->divergence().has_value()) {
                    ++divergences;
                    // The wording matches what a replay divergence prints, so one expression
                    // reads both.
                    ATLAS_LOG_ERROR(kApp, "loopback peer {} {}", i,
                                    peers[i]->session->divergence()->description);
                }
            }
            return std::unexpected(std::move(status).error().context("a loopback peer"));
        }

        // Announce every turn each peer owes, tracking its own tick rather than a frame count:
        // a peer that has stalled must stop announcing, or its partner runs ahead of it.
        for (std::size_t i = 0; i < peers.size(); ++i) {
            auto& peer = *peers[i];
            const atlas::Tick horizon = peer.sim->kernel->current_tick() + options.input_delay;
            while (peer.next_turn <= horizon) {
                std::vector<atlas::sim::Command> turn;
                if (options.commands_per_tick > 0 && peer.next_turn >= options.input_delay) {
                    // Built rather than submitted-and-asked-for: a peer sends the same list it
                    // applies, so it has to hold it. The seed differs per peer so the streams
                    // genuinely differ rather than being the same commands under two names,
                    // which is what makes the agreement worth anything.
                    const auto source = peer.session->self();
                    for (auto& payload : atlas::lab::synthetic_payloads(
                             peer.next_turn, options.commands_per_tick, options.seed + i,
                             peer.sim->lab.layout.cell_count())) {
                        atlas::sim::Command command{
                            .target = peer.next_turn,
                            .source = source,
                            .sequence = peer.sim->commands.next_sequence(source),
                            .type = atlas::lab::kSetColorIndex,
                            .payload = std::move(payload),
                        };
                        if (auto status = peer.sim->commands.submit_stamped(command); !status) {
                            return std::unexpected(std::move(status).error());
                        }
                        turn.push_back(std::move(command));
                    }
                }
                if (auto status = peer.session->send_turn(peer.next_turn, turn, peer.gate);
                    !status) {
                    return std::unexpected(std::move(status).error());
                }
                ++peer.next_turn;
            }
        }

        if (auto status = poll_all(); !status) {
            return std::unexpected(std::move(status).error().context("a loopback peer"));
        }

        bool progressed = false;
        for (auto& peer : peers) {
            while (peer->sim->kernel->ready()) {
                // Before the tick this peer is about to run, and on this peer's own copy of the
                // mod. Nothing about it crosses the link.
                run_mod_for_tick(peer->mod, *peer->sim, peer->sim->kernel->current_tick(),
                                 peer->gate);
                auto report = peer->sim->kernel->step();
                if (!report) {
                    return std::unexpected(std::move(report).error());
                }
                progressed = true;
                peer->last_hash = report->state_hash;
                peer->applied += report->commands_applied;
                peer->gate.retire_before(report->tick);
                if (auto status = peer->session->send_hash_check(report->tick, report->state_hash,
                                                                 report->system_hashes);
                    !status) {
                    return std::unexpected(std::move(status).error());
                }
            }
        }

        if (progressed) {
            idle_polls = 0;
        } else {
            ++stalls;
            if (!released && idle_polls >= options.loopback_release_after) {
                (*hub)->release_held();
                released = true;
                ATLAS_LOG_INFO(kApp, "releasing held messages after {} poll(s) without progress",
                               idle_polls);
            }
            if (++idle_polls > kMaxPollsWithoutProgress) {
                std::string waiting;
                std::vector<atlas::sim::SourceId> outstanding;
                for (std::size_t i = 0; i < peers.size(); ++i) {
                    peers[i]->gate.waiting_on(peers[i]->sim->kernel->current_tick(), outstanding);
                    for (const auto source : outstanding) {
                        waiting += std::format("peer {} waiting on source {}; ", i,
                                               static_cast<std::uint32_t>(source));
                    }
                }
                return atlas::fail(atlas::ErrorCode::Unavailable,
                                   std::format("the loopback made no progress for {} polls: {}",
                                               kMaxPollsWithoutProgress, waiting));
            }
        }
    }

    std::uint64_t agreed = 0;
    for (std::size_t i = 0; i < peers.size(); ++i) {
        const auto& peer = *peers[i];
        agreed += peer.session->stats().hash_checks_agreed;
        std::printf("peer %zu: final tick=%llu state hash=%#018llx commands applied=%zu source=%u "
                    "turns sent=%llu turns received=%llu\n",
                    i, static_cast<unsigned long long>(peer.sim->kernel->current_tick()),
                    static_cast<unsigned long long>(peer.last_hash), peer.applied,
                    static_cast<unsigned>(peer.session->self()),
                    static_cast<unsigned long long>(peer.session->stats().turns_sent),
                    static_cast<unsigned long long>(peer.session->stats().turns_received));
        report_mod(peer.mod, std::format("peer {} ", i));
    }

    // Every peer's mod must have produced the same number of commands. The hashes agreeing
    // already implies it, but this says which half of the proof failed when one does: a mod
    // disabled on one peer and not another is a different problem from two mods that ran fully
    // and disagreed.
    for (std::size_t i = 1; i < peers.size(); ++i) {
        if (!peers[i]->mod.loaded()) {
            break;
        }
        const auto mine = peers[i]->mod.host->stats().commands_submitted;
        const auto first = peers[0]->mod.host->stats().commands_submitted;
        if (mine != first) {
            return atlas::fail(atlas::ErrorCode::IntegrityCheckFailed,
                               std::format("peer {}'s mod submitted {} commands and peer 0's "
                                           "submitted {}; the same mod on the same seed must "
                                           "reach the same answer",
                                           i, mine, first));
        }
    }
    std::printf("loopback: peers=%zu delay=%u latency=%u reorder=%llu agreed hashes=%llu "
                "divergences=%llu stalls=%llu\n",
                peer_count, options.input_delay, options.link_latency,
                static_cast<unsigned long long>(options.link_reorder),
                static_cast<unsigned long long>(agreed),
                static_cast<unsigned long long>(divergences),
                static_cast<unsigned long long>(stalls));

    // The binary asserts this itself rather than leaving it to whoever reads the output. A
    // property checked only by a script holds only when somebody runs the script.
    for (std::size_t i = 1; i < peers.size(); ++i) {
        if (peers[i]->last_hash != peers[0]->last_hash) {
            return atlas::fail(atlas::ErrorCode::IntegrityCheckFailed,
                               std::format("peer {} finished at {:#018x} and peer 0 at {:#018x}", i,
                                           peers[i]->last_hash, peers[0]->last_hash));
        }
        if (peers[i]->sim->kernel->current_tick() != peers[0]->sim->kernel->current_tick()) {
            return atlas::fail(atlas::ErrorCode::IntegrityCheckFailed,
                               std::format("peer {} ran {} ticks and peer 0 ran {}", i,
                                           peers[i]->sim->kernel->current_tick(),
                                           peers[0]->sim->kernel->current_tick()));
        }
    }
    return atlas::ok();
}

[[nodiscard]] atlas::Status play_replay(const Options& options) {
    auto bytes = atlas::lab::read_file_bytes(std::filesystem::path{options.play_path});
    if (!bytes) {
        return std::unexpected(std::move(bytes).error().context("reading the replay"));
    }
    auto replay = atlas::sim::Replay::from_bytes(*bytes);
    if (!replay) {
        return std::unexpected(std::move(replay).error().context("decoding the replay"));
    }
    auto simulation = make_simulation(options, nullptr);
    if (!simulation) {
        return std::unexpected(simulation.error());
    }
    Simulation& sim = **simulation;
    if (!options.load_path.empty()) {
        // A recording made after a load starts from that loaded state, not a fresh one.
        auto accumulator = atlas::sim::TickAccumulator::create({});
        if (!accumulator) {
            return std::unexpected(accumulator.error());
        }
        if (auto status =
                load_guarded(std::filesystem::path{options.load_path}, sim, *accumulator, nullptr);
            !status) {
            return status;
        }
    }
    ATLAS_LOG_INFO(kApp, "playing '{}': {} ticks from tick {}, {} commands, {} checkpoints",
                   options.play_path, replay->tick_count, replay->first_tick,
                   replay->commands.size(), replay->checkpoints.size());
    auto result = atlas::sim::play(*replay, sim.lab.world, sim.schedule, sim.commands);
    if (!result) {
        return std::unexpected(std::move(result).error().context("playing the replay"));
    }
    // Bound to a local before the check: the older clang-tidy on the lint lane does not
    // follow has_value() through the expected's operator->, and it is clearer this way.
    const std::optional<atlas::sim::Divergence>& divergence = result->divergence;
    if (divergence.has_value()) {
        // The description already carries the tick, both hashes and the system to blame. It
        // used to be appended to a sentence that repeated the first three, which meant a reader
        // was told the tick twice and the hashes twice before reaching the part they needed.
        return std::unexpected(atlas::Error(atlas::ErrorCode::IntegrityCheckFailed,
                                            std::format("replay {}", divergence->description)));
    }
    ATLAS_LOG_INFO(kApp, "replay matched: {} ticks, {} checkpoints checked, final hash {:#018x}",
                   result->ticks_run, result->checkpoints_checked, sim.lab.world.hash());
    return atlas::ok();
}

// ------------------------------------------------------------------------------------ run

/// Processor-side phase times for the last frame, in microseconds. Not a profiler: a way
/// for a person to see which phase is largest without a profiling build.
struct Phases {
    std::uint64_t events = 0;
    std::uint64_t simulation = 0;
    /// Acquiring a swapchain image and presenting. Reported apart from the drawing because
    /// both wait for the display: with vertical sync on, this is where the frame's idle time
    /// goes, and counting it as drawing made a million cells and four thousand cells look
    /// equally expensive. They are not; see docs/PERFORMANCE.md.
    std::uint64_t present = 0;
    std::uint64_t snapshot = 0;
    std::uint64_t draw = 0;
    /// Mixing and pushing. Small by design and reported anyway: ADR-0011 spends a frame's
    /// worth of headroom on this, and a number nobody can see is a budget nobody can check.
    std::uint64_t audio = 0;
};

[[nodiscard]] std::uint64_t micros_since(std::chrono::steady_clock::time_point start) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now() - start)
                                          .count());
}

/// Display-mode names in enumeration order, for the controls panel.
///
/// Built from to_string so the panel and the status row cannot disagree about what a mode is
/// called, and sized from MapMode::Count so adding one fails to compile here rather than
/// quietly leaving a button missing.
const std::array<std::string_view, static_cast<std::size_t>(atlas::lab::MapMode::Count)>
    kMapModeNames{
        atlas::lab::to_string(atlas::lab::MapMode::RegionValue),
        atlas::lab::to_string(atlas::lab::MapMode::OwnerIndex),
        atlas::lab::to_string(atlas::lab::MapMode::PopulationValue),
        atlas::lab::to_string(atlas::lab::MapMode::ColorIndex),
};

[[nodiscard]] std::string_view speed_name(const atlas::sim::Speed& speed) {
    using atlas::sim::SpeedPolicy;
    switch (speed.policy) {
    case SpeedPolicy::Paused: return "paused";
    case SpeedPolicy::SingleStep: return "step";
    case SpeedPolicy::Unbounded: return "unbounded";
    case SpeedPolicy::Realtime: break;
    }
    switch (speed.numerator) {
    case 1: return "1x";
    case 2: return "2x";
    case 4: return "4x";
    case 8: return "8x";
    default: return "custom";
    }
}

[[nodiscard]] atlas::Status run(int argc, const char* const* argv) {
    auto args = atlas::Args::parse(argc, argv);
    if (!args) {
        return std::unexpected(std::move(args).error());
    }
    if (args->has("help")) {
        print_usage();
        return atlas::ok();
    }
    if (args->has("version")) {
        std::printf("%s\n", std::string{atlas::build_info::summary()}.c_str());
        return atlas::ok();
    }
    const auto options = read_options(*args);
    if (!options) {
        return std::unexpected(options.error());
    }

    const atlas::app::LogSession logging;
    // Owned here, so it dies with this function and the sink registry's weak reference to
    // it simply stops resolving. Four thousand records is a few seconds of a busy frame loop
    // and about a megabyte, which is worth having when something goes wrong once.
    const auto log_buffer = std::make_shared<atlas::log::LogBuffer>(4096);

    if (const auto status =
            atlas::app::configure_logging(options->log_level, options->log_file, log_buffer);
        !status) {
        return status;
    }
    // The kernel asserts it runs on the main thread. Marked here, in the composition root,
    // rather than as a side effect of creating the platform, because a replay is played
    // before there is any platform.
    atlas::mark_main_thread();
    ATLAS_THREAD_NAME("main");
    ATLAS_LOG_INFO(kApp, "startup: {}", atlas::build_info::summary());

    if (options->loopback_peers > 0) {
        return run_loopback(*options);
    }

    if (!options->play_path.empty()) {
        return play_replay(*options);
    }

    // Declared before the simulation so it outlives the kernel that borrows it.
    const std::size_t worker_count =
        options->workers_set ? options->workers : atlas::tasks::WorkerPool::default_worker_count();
    auto pool = atlas::tasks::WorkerPool::create(worker_count);
    if (!pool) {
        return std::unexpected(std::move(pool).error().context("starting the worker pool"));
    }

    auto simulation = make_simulation(*options, &*pool);
    if (!simulation) {
        return std::unexpected(simulation.error());
    }
    Simulation& sim = **simulation;

    auto mod_runtime = open_mod_runtime(*options);
    if (!mod_runtime) {
        return std::unexpected(std::move(mod_runtime).error());
    }
    auto mods = attach_mod(*mod_runtime, *options, sim);
    if (!mods) {
        return std::unexpected(std::move(mods).error());
    }
    // A solo run has nothing to gate on, and a mod never marks a gate in any case. Declared
    // here so the reference `poll` takes outlives every call to it.
    atlas::sim::TurnGate mod_gate;

    ATLAS_LOG_INFO(kApp, "world: {}x{} cells in {} chunks of {}, seed {}, initial hash {:#018x}",
                   sim.lab.layout.width(), sim.lab.layout.height(), sim.lab.layout.chunk_count(),
                   sim.lab.layout.chunk_size(), options->seed, sim.lab.world.hash());

    // The gamepad subsystem follows the window: a run with no window has nobody to aim a
    // camera, and enumerating input devices on a headless machine is work with no consumer
    // that can also raise a permission prompt. Every headless test therefore behaves exactly
    // as it did before this line existed.
    auto platform = atlas::platform::Platform::create({
        .video = !options->headless,
        .video_driver = options->video_driver,
        .app_name = "Atlas lab",
        .gamepad = !options->headless && !options->no_gamepad,
        .audio = !options->headless && !options->no_audio,
        .audio_driver = options->audio_driver,
    });
    if (!platform) {
        return std::unexpected(std::move(platform).error().context("starting the platform"));
    }
    // Declared after the platform, so it is destroyed before it. The platform's destructor
    // shuts down every window-system subsystem at once, including the audio one this device's
    // stream lives on, and a device closed after that is a device closed too late.
    std::optional<atlas::audio::AudioDevice> audio;
    atlas::audio::ClipHandle click_clip;
    if (platform->has_audio_support()) {
        auto opened = atlas::audio::AudioDevice::create();
        if (!opened) {
            // The fallback is the application's decision and is logged as one. If create()
            // had quietly returned a silent device instead, nobody could tell a working
            // machine from a broken one without reading the source.
            ATLAS_LOG_WARN(kApp, "no audio device ({}); continuing without sound", opened.error());
            audio = atlas::audio::AudioDevice::null();
        } else {
            audio = std::move(*opened);
        }

        // Generated, not loaded. The lab has no asset registry and deliberately does not want
        // one: giving it a filesystem and a mount to play one click would be a larger change
        // than the audio module itself.
        const auto samples = atlas::audio::sine_blip(880.0F, 40, 0.35F);
        if (auto clip = audio->create_clip({.samples = samples, .debug_name = "click"}); !clip) {
            ATLAS_LOG_WARN(kApp, "the click could not be created: {}", clip.error());
        } else {
            click_clip = *clip;
        }
    }

    /// Play the click, if there is anything to play it on. Called from the places a person
    /// did something, never from inside a tick: audio is presentation, and `atlas_lab_sim`
    /// is fenced at configure time so that a system could not reach this even by mistake.
    const auto play_click = [&audio, &click_clip](float pan) {
        if (audio.has_value() && click_clip.valid()) {
            (void)audio->play(click_clip, {.pan = pan});
        }
    };

    atlas::platform::Window window;
    if (!options->headless) {
        auto created = platform->create_window({.title = "Atlas lab"});
        if (!created) {
            return std::unexpected(std::move(created).error().context("opening a window"));
        }
        window = std::move(*created);
    }

    std::optional<atlas::rhi::Device> device;
    std::optional<atlas::lab::CellField> field;
    std::optional<atlas::lab::CellIdPass> id_pass;
    std::optional<atlas::tools::DebugUi> overlay;
    if (!options->headless && !options->no_render && window.valid()) {
        auto created = atlas::rhi::Device::create({.debug = kDebugBuild}, window);
        if (!created) {
            return std::unexpected(
                std::move(created).error().context("creating the graphics device"));
        }
        device = std::move(*created);
        auto cells =
            atlas::lab::CellField::create(*device, {.shader_directory = options->shader_dir});
        if (!cells) {
            return std::unexpected(std::move(cells).error().context("building the cell field"));
        }
        field = std::move(*cells);
        field->resize(window.pixel_size().width, window.pixel_size().height);
        if (auto s = field->set_layout(sim.lab.layout); !s) {
            return std::unexpected(std::move(s).error().context("sizing the cell field"));
        }
        auto ids =
            atlas::lab::CellIdPass::create(*device, {.shader_directory = options->shader_dir});
        if (!ids) {
            return std::unexpected(std::move(ids).error().context("building the id pass"));
        }
        id_pass = std::move(*ids);
        if (auto s = id_pass->resize(window.pixel_size().width, window.pixel_size().height); !s) {
            return std::unexpected(std::move(s).error());
        }
        if (!options->no_overlay) {
            auto ui = atlas::tools::DebugUi::create(*device, window);
            if (!ui) {
                ATLAS_LOG_WARN(kApp, "the debug overlay is unavailable: {}", ui.error());
            } else {
                overlay = std::move(*ui);
            }
        }
    }

    auto accumulator = atlas::sim::TickAccumulator::create({
        .ticks_per_second = options->ticks_per_second,
        .max_ticks_per_frame = options->max_ticks_per_frame,
    });
    if (!accumulator) {
        return std::unexpected(
            std::move(accumulator).error().context("configuring the tick scheduler"));
    }
    if (options->unbounded) {
        accumulator->set_speed(atlas::sim::Speed::unbounded());
    }
    if (options->start_paused) {
        accumulator->set_speed(atlas::sim::Speed::paused());
    }

    if (!options->load_path.empty()) {
        if (auto status = load_guarded(std::filesystem::path{options->load_path}, sim, *accumulator,
                                       field.has_value() ? &*field : nullptr);
            !status) {
            return status;
        }
    }

    // The recorder starts from wherever the run starts, after any load.
    std::optional<atlas::sim::ReplayRecorder> recorder;
    if (!options->record_path.empty()) {
        recorder.emplace(sim.kernel->seed(), sim.kernel->current_tick(), sim.lab.world.hash());
    }

    std::optional<atlas::math::Vec2> scripted_pick;
    if (!options->pick.empty()) {
        const std::string text{options->pick};
        char* after_x = nullptr;
        const float x = std::strtof(text.c_str(), &after_x);
        char* after_y = nullptr;
        const float y =
            after_x != nullptr && *after_x == ',' ? std::strtof(after_x + 1, &after_y) : 0.0F;
        const bool well_formed = after_x != nullptr && *after_x == ',' && after_y != nullptr &&
                                 after_y != after_x + 1 && *after_y == '\0' &&
                                 after_x != text.c_str();
        if (!well_formed) {
            return std::unexpected(
                atlas::Error(atlas::ErrorCode::InvalidArgument,
                             std::format("--pick wants X,Y in logical window coordinates, got '{}'",
                                         options->pick)));
        }
        scripted_pick = atlas::math::Vec2{x, y};
    }

    atlas::sim::SnapshotChannel<atlas::lab::CellSnapshot> channel;
    std::uint64_t last_hash = sim.lab.world.hash();
    std::uint64_t snapshot_us = 0;
    const auto publish = [&] {
        const auto start = std::chrono::steady_clock::now();
        channel.publish(atlas::lab::build_snapshot(sim.lab.world, sim.lab.ids, sim.lab.layout,
                                                   {.tick = sim.kernel->current_tick(),
                                                    .state_hash = last_hash,
                                                    .generation = channel.published() + 1}));
        snapshot_us = micros_since(start);
    };
    if (device.has_value()) {
        publish();
    }

    ATLAS_LOG_INFO(kApp,
                   "running: headless={} tps={} unbounded={} frames={} ticks={} commands/tick={}",
                   options->headless, options->ticks_per_second, options->unbounded,
                   options->max_frames, options->max_ticks, options->commands_per_tick);

    atlas::SteadyClock clock;
    atlas::app::FrameCounters counters;
    const auto run_start = std::chrono::steady_clock::now();
    const atlas::Tick first_tick = sim.kernel->current_tick();
    std::uint64_t frame_index = 0;
    bool quit = false;
    bool captured = false;
    // From the last tick of the most recent frame, for the determinism rows. Rejected is the
    // one worth watching: a command that fails validation is logged and dropped, and a run
    // quietly dropping every command looks exactly like a run nobody is sending any.
    std::size_t last_applied = 0;
    std::size_t last_rejected = 0;

    atlas::lab::MapMode mode = options->map_mode;
    atlas::sim::Speed resume_speed = accumulator->speed();

    // Keys and buttons both produce a request and both come through here. Two code paths that
    // did the same things separately would drift, and the one that drifted would be the one
    // nobody tested.
    const auto controls_context = [&] {
        return atlas::tools::ControlsContext{
            .speed = accumulator->speed(),
            .mode_index = static_cast<std::size_t>(mode),
            .mode_count = static_cast<std::size_t>(atlas::lab::MapMode::Count)};
    };

    const auto apply_controls = [&](const atlas::tools::SimulationControlsRequest& request) {
        if (request.empty()) {
            return;
        }
        if (request.speed.has_value()) {
            // Remember what to come back to before pausing, so resuming restores the speed
            // rather than dropping to 1x. The panel cannot do this: it does not know the
            // previous speed, which is why it asks for normal and is corrected here.
            if (request.speed->policy == atlas::sim::SpeedPolicy::Paused) {
                resume_speed = accumulator->speed();
                accumulator->set_speed(atlas::sim::Speed::paused());
            } else if (accumulator->speed().policy == atlas::sim::SpeedPolicy::Paused &&
                       request.speed->policy == atlas::sim::SpeedPolicy::Realtime &&
                       request.speed->numerator == 1 && request.speed->denominator == 1) {
                accumulator->set_speed(resume_speed);
            } else {
                accumulator->set_speed(*request.speed);
            }
        }
        if (request.single_step) {
            accumulator->set_speed(
                atlas::sim::Speed{.policy = atlas::sim::SpeedPolicy::SingleStep});
            accumulator->request_single_step();
        }
        if (request.mode_index.has_value() &&
            *request.mode_index < static_cast<std::size_t>(atlas::lab::MapMode::Count)) {
            mode = static_cast<atlas::lab::MapMode>(*request.mode_index);
        }
        if (request.reset_view && field.has_value()) {
            field->reset_camera();
        }
        if (request.save) {
            if (options->save_path.empty()) {
                ATLAS_LOG_WARN(kApp, "save requested with no --save path");
            } else if (auto s = atlas::lab::save_world_to(std::filesystem::path{options->save_path},
                                                          sim.lab.world, *sim.kernel, sim.commands);
                       !s) {
                ATLAS_LOG_ERROR(kApp, "save failed: {}", s.error());
            } else {
                play_click(0.0F);
                ATLAS_LOG_INFO(kApp, "saved '{}' at tick {}", options->save_path,
                               sim.kernel->current_tick());
            }
        }
        if (request.load) {
            if (options->load_path.empty() && options->save_path.empty()) {
                ATLAS_LOG_WARN(kApp, "load requested with neither --load nor --save path");
            } else {
                const std::filesystem::path path{options->load_path.empty() ? options->save_path
                                                                            : options->load_path};
                if (auto s = load_guarded(path, sim, *accumulator,
                                          field.has_value() ? &*field : nullptr);
                    !s) {
                    ATLAS_LOG_ERROR(kApp, "load failed: {}", s.error());
                } else {
                    play_click(0.0F);
                    last_hash = sim.lab.world.hash();
                    if (device.has_value()) {
                        publish();
                    }
                }
            }
        }
    };
    atlas::lab::CellField::DrawStats last_draw;
    Phases phases;

    // A pick is a small state machine: a click becomes a request; the next frame draws the
    // identifier pass and, once submitted, asks for the pixel; a later frame collects it.
    struct Pick {
        atlas::math::Vec2 screen;
        std::optional<atlas::rhi::ReadbackHandle> ticket;
        bool drawn = false;
        std::uint64_t requested_frame = 0;  ///< For the latency figure: frames until ready.
    };

    std::optional<Pick> pick;
    std::vector<std::uint32_t> pick_chunks;
    std::uint64_t pick_mismatches = 0;

    while (!quit) {
        ATLAS_ZONE_NAMED("frame");
        const std::uint64_t frame_ns = atlas::to_unsigned_ns(clock.tick());
        ++frame_index;

        // ---- events and controls
        const auto events_start = std::chrono::steady_clock::now();
        const auto events = platform->pump();
        for (const auto& event : events) {
            if (overlay.has_value() && overlay->handle_event(event)) {
                continue;
            }
            if (std::holds_alternative<atlas::platform::QuitRequested>(event) ||
                std::holds_alternative<atlas::platform::WindowCloseRequested>(event)) {
                quit = true;
            } else if (const auto* pad =
                           std::get_if<atlas::platform::GamepadButtonPressed>(&event)) {
                // The same table as the keyboard, so a button and a key cannot come to mean
                // different things.
                if (const auto action = atlas::tools::action_for(pad->button)) {
                    apply_controls(atlas::tools::request_for(*action, controls_context()));
                }
            } else if (const auto* resized = std::get_if<atlas::platform::WindowResized>(&event)) {
                if (field.has_value()) {
                    field->resize(resized->pixel_size.width, resized->pixel_size.height);
                }
                if (id_pass.has_value()) {
                    if (auto s =
                            id_pass->resize(resized->pixel_size.width, resized->pixel_size.height);
                        !s) {
                        ATLAS_LOG_WARN(kApp, "resizing the id target failed: {}", s.error());
                    }
                }
            } else if (const auto* click =
                           std::get_if<atlas::platform::MouseButtonPressed>(&event)) {
                if (click->button == atlas::platform::MouseButton::Left && !pick.has_value()) {
                    // Stored in pixels: the camera's viewport and the id target both are.
                    const float scale = window.display_scale();
                    pick = Pick{.screen = {click->position.x * scale, click->position.y * scale}};
                }
            } else if (const auto* key = std::get_if<atlas::platform::KeyPressed>(&event)) {
                if (key->repeat || (overlay.has_value() && overlay->wants_keyboard())) {
                    continue;
                }
                // Quit is not a simulation control, so it stays here rather than joining the
                // binding table.
                if (key->key == atlas::platform::Key::Escape) {
                    quit = true;
                } else if (const auto action = atlas::tools::action_for(key->key)) {
                    apply_controls(atlas::tools::request_for(*action, controls_context()));
                }
            }
        }
        if (field.has_value()) {
            // The pointer is ignored while the overlay owns it; the gamepad is not, because
            // it has no pointer to be over a panel with.
            const bool mouse_allowed = !(overlay.has_value() && overlay->wants_mouse());
            field->update(platform->input(), events, window.display_scale(),
                          static_cast<float>(frame_ns) / 1'000'000'000.0F, mouse_allowed);
        }
        if (scripted_pick.has_value() && frame_index == 3 && field.has_value() &&
            !pick.has_value()) {
            const float scale = window.display_scale();
            pick = Pick{.screen = {scripted_pick->x * scale, scripted_pick->y * scale}};
            scripted_pick.reset();
        }
        // A click becomes a command, the only path into state. The cell comes from the
        // identifier pass; the analytic inverse of the projection is computed alongside and a
        // disagreement is logged, so the GPU path is checked on every click, not only in tests.
        if (pick.has_value() && pick->ticket.has_value() && device->readback_ready(*pick->ticket)) {
            auto pixel = device->take_readback(*pick->ticket);
            if (!pixel) {
                ATLAS_LOG_WARN(kApp, "pick readback failed: {}", pixel.error());
            } else {
                const auto picked = atlas::lab::CellIdPass::decode(*pixel);
                const auto analytic = field->cell_at_screen(pick->screen);
                // The readback is deferred: this is how many frames it took to arrive, and
                // therefore also proof that nothing waited on it.
                ATLAS_LOG_INFO(kApp,
                               "pick at pixel ({:.0f}, {:.0f}): identifier pass={} analytic={} "
                               "readback latency={} frame(s)",
                               pick->screen.x, pick->screen.y,
                               picked ? std::to_string(*picked) : "none",
                               analytic ? std::to_string(*analytic) : "none",
                               frame_index - pick->requested_frame);
                if (picked != analytic) {
                    ++pick_mismatches;
                    ATLAS_LOG_ERROR(kApp,
                                    "pick disagreement at ({:.0f}, {:.0f}): identifier pass says "
                                    "{}, the projection's inverse says {}",
                                    pick->screen.x, pick->screen.y,
                                    picked ? std::to_string(*picked) : "nothing",
                                    analytic ? std::to_string(*analytic) : "nothing");
                }
                if (picked.has_value()) {
                    if (const auto snapshot = channel.latest()) {
                        const auto current =
                            atlas::lab::band(*snapshot, atlas::lab::MapMode::ColorIndex);
                        const auto next_colour = static_cast<std::uint8_t>((current[*picked] + 1U) %
                                                                           atlas::lab::kColorCount);
                        const auto payload =
                            atlas::lab::encode_set_color_index(*picked, next_colour);
                        if (auto s = sim.commands.submit(sim.kernel->current_tick(),
                                                         atlas::sim::SourceId::Local,
                                                         atlas::lab::kSetColorIndex, payload);
                            !s) {
                            ATLAS_LOG_WARN(kApp, "pick command refused: {}", s.error());
                        } else {
                            // Panned by where the click landed, so the sound comes from the
                            // side of the field that changed. A tiny thing, and the reason
                            // `pan` exists at all rather than being deferred until something
                            // needed it.
                            // The pick is stored in pixels, so the width it is measured
                            // against has to be the pixel width too. Using the logical width
                            // on a scaled display would pan everything to the left half.
                            const float width =
                                std::max(1.0F, static_cast<float>(window.pixel_size().width));
                            play_click(((pick->screen.x / width) * 2.0F) - 1.0F);
                            ATLAS_LOG_INFO(kApp, "picked cell {} -> colour {}", *picked,
                                           next_colour);
                        }
                    }
                }
            }
            pick.reset();
        }
        phases.events = micros_since(events_start);

        // Mixed and pushed once a frame, on this thread. ADR-0011: no Atlas code runs on the
        // window system's audio thread, and the price is that a frame longer than the queued
        // audio is heard as a gap, which AudioStats::underruns counts.
        const auto audio_start = std::chrono::steady_clock::now();
        if (audio.has_value()) {
            audio->update();
        }
        phases.audio = micros_since(audio_start);

        // ---- simulation
        const auto tick_start = std::chrono::steady_clock::now();
        const auto plan = accumulator->advance(frame_ns);
        const std::uint32_t ticks_to_run = atlas::app::clamp_ticks(
            plan.ticks_to_run, sim.kernel->current_tick(), options->max_ticks);
        std::uint32_t ticks_run = 0;
        for (std::uint32_t i = 0; i < ticks_to_run; ++i) {
            const atlas::Tick tick = sim.kernel->current_tick();
            // Before the tick, never once per frame: peers run different numbers of frames per
            // tick, so a per-frame call would make a mod's output depend on frame rate.
            run_mod_for_tick(*mods, sim, tick, mod_gate);
            if (options->commands_per_tick > 0) {
                // Stamped with this peer's own identifier. Solo that is `Local`, which is
                // what it has always been; under lockstep it becomes the identifier the
                // handshake assigned, because `Local` means peer zero rather than "me".
                if (auto s = atlas::lab::submit_synthetic_commands(
                        sim.commands, tick, options->commands_per_tick, options->seed,
                        sim.lab.layout.cell_count(), atlas::sim::SourceId::Local);
                    !s) {
                    return std::unexpected(std::move(s).error().context("synthetic commands"));
                }
            }
            auto report = sim.kernel->step();
            if (!report) {
                return std::unexpected(std::move(report).error().context("stepping the kernel"));
            }
            ++ticks_run;
            last_hash = report->state_hash;
            last_applied = report->commands_applied;
            last_rejected = report->commands_rejected();
            if (recorder.has_value()) {
                // A recorder that has hit its limit stops the run rather than carrying on
                // producing a recording it can no longer write. Carrying on would mean
                // finishing the run, writing a file, and finding out at load time that the
                // tail is missing — which is the failure this guard exists to remove.
                if (auto status = recorder->record_commands(report->applied_commands); !status) {
                    return std::unexpected(std::move(status).error().context("recording a tick"));
                }
                if (auto status = recorder->record_tick(*report); !status) {
                    return std::unexpected(std::move(status).error().context("recording a tick"));
                }
            }
        }
        accumulator->commit(ticks_run);
        ATLAS_ASSERT_MSG(accumulator->current_tick() == sim.kernel->current_tick(),
                         "the accumulator and the kernel disagree about the tick");
        const auto tick_ns =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - tick_start)
                                           .count());
        phases.simulation = tick_ns / 1000;
        counters.record(frame_ns, tick_ns, ticks_run, plan.dropped_ticks);

        // ---- snapshot: once per frame, after the last tick, never headless
        if (device.has_value() && ticks_run > 0) {
            publish();
        }
        phases.snapshot = device.has_value() && ticks_run > 0 ? snapshot_us : 0;

        // ---- present
        if (device.has_value() && window.valid() && !window.is_minimized()) {
            ATLAS_ZONE_NAMED("present");
            const auto acquire_start = std::chrono::steady_clock::now();
            auto frame = device->begin_frame();
            std::uint64_t present_us = micros_since(acquire_start);
            if (!frame) {
                // A failed frame is where device loss surfaces: every later call fails too,
                // so the first failure is the only one that names the cause. Atlas does not
                // recover, by charter; it reports the reason and stops with its own code.
                if (device->is_lost()) {
                    return atlas::fail(
                        atlas::ErrorCode::DeviceLost,
                        std::format("the graphics device was lost: {}", device->loss_reason()));
                }
                ATLAS_LOG_ERROR(kApp, "begin_frame failed: {}", frame.error());
                quit = true;
            } else {
                if (frame->has_swapchain_target()) {
                    const bool capture_now =
                        !options->screenshot.empty() && !captured &&
                        ((options->max_frames != 0 && frame_index >= options->max_frames) || quit);
                    if (capture_now) {
                        device->request_capture();
                        captured = true;
                    }

                    atlas::tools::DebugUi::PreparedFrame prepared{};
                    std::array<std::string, 17> values;
                    if (overlay.has_value()) {
                        overlay->begin_frame(static_cast<float>(frame_ns) / 1'000'000'000.0F,
                                             frame->swapchain_extent().width,
                                             frame->swapchain_extent().height);
                        const std::uint64_t largest =
                            std::max({phases.events, phases.simulation, phases.snapshot,
                                      phases.draw, phases.present, phases.audio});
                        const auto phase = [&](std::uint64_t us) {
                            return std::format("{}{} us", us, us == largest && us > 0 ? " *" : "");
                        };
                        values[0] = std::format("{}", frame_index);
                        values[1] = std::format("{}", sim.kernel->current_tick());
                        values[2] = std::format("{:#018x}", last_hash);
                        values[3] = std::string{speed_name(accumulator->speed())};
                        values[4] = std::string{atlas::lab::to_string(mode)};
                        values[5] = std::format("{} of {}", last_draw.visible_chunks,
                                                sim.lab.layout.chunk_count());
                        values[6] = std::format("{} cells, {} draws, {} KiB", last_draw.cells.cells,
                                                last_draw.cells.draw_calls,
                                                last_draw.cells.bytes_uploaded / 1024);
                        values[7] = phase(phases.events);
                        values[8] = phase(phases.simulation);
                        values[9] = phase(phases.snapshot);
                        values[10] = phase(phases.draw);
                        values[11] = phase(phases.present);
                        values[12] = std::format("{:.2f}", field->camera().zoom());
                        values[13] = std::format("{:#018x}", sim.kernel->seed());
                        values[14] = std::format(
                            "{} applied, {} rejected, {} late, {} pending", last_applied,
                            last_rejected, sim.kernel->late_commands(), sim.commands.pending());
                        values[15] = std::format("{}", atlas::kHashAlgorithmVersion);
                        if (audio.has_value()) {
                            const auto audio_stats = audio->stats();
                            values[16] = std::format("{} voices, {} ms queued, {} underruns{}",
                                                     audio_stats.voices, audio_stats.queued_ms,
                                                     audio_stats.underruns,
                                                     audio_stats.null_device ? ", no device" : "");
                        } else {
                            values[16] = "off";
                        }
                        const std::array<atlas::tools::Stat, 17> stats{{
                            {.label = "frame", .value = values[0]},
                            {.label = "tick", .value = values[1]},
                            {.label = "state hash", .value = values[2]},
                            {.label = "speed", .value = values[3]},
                            {.label = "map mode", .value = values[4]},
                            {.label = "visible chunks", .value = values[5]},
                            {.label = "batch", .value = values[6]},
                            {.label = "events (cpu)", .value = values[7]},
                            {.label = "simulation (cpu)", .value = values[8]},
                            {.label = "snapshot (cpu)", .value = values[9]},
                            {.label = "draw (cpu)", .value = values[10]},
                            {.label = "acquire+present", .value = values[11]},
                            {.label = "zoom", .value = values[12]},
                            {.label = "seed", .value = values[13]},
                            {.label = "commands", .value = values[14]},
                            {.label = "hash version", .value = values[15]},
                            {.label = "audio", .value = values[16]},
                        }};
                        overlay->stats_panel("Strategy Lab", stats);

                        const atlas::tools::SimulationControlsView view{
                            .speed = accumulator->speed(),
                            .tick = sim.kernel->current_tick(),
                            .modes = kMapModeNames,
                            .mode_index = static_cast<std::size_t>(mode),
                            .can_save = !options->save_path.empty(),
                            .can_load = !options->load_path.empty() || !options->save_path.empty(),
                            .recording = recorder.has_value(),
                            .recorded_commands = recorder.has_value()
                                                     ? recorder->replay().commands.size()
                                                     : std::uint64_t{0},
                        };
                        apply_controls(overlay->simulation_controls_panel("Controls", view));

                        if (overlay->log_console_panel("Log", *log_buffer).clear_requested) {
                            log_buffer->clear();
                        }
                        // Text input is switched on only while a field has focus, and off
                        // again when it loses focus. Leaving it on changes how the platform
                        // treats ordinary keys: with an input method engaged a shortcut key
                        // becomes a composition keystroke instead.
                        const bool want_text = overlay->wants_text_input();
                        if (want_text != window.text_input_active()) {
                            if (auto s = window.set_text_input_active(want_text); !s) {
                                ATLAS_LOG_WARN(kApp, "text input: {}", s.error());
                            }
                        }

                        // Where the input method should put its candidate list. The overlay
                        // reports in its own pixels; the window wants logical units, so the
                        // display scale converts. Without this the candidate list sits wherever
                        // it last was, usually over the text being typed.
                        if (want_text) {
                            const auto ime = overlay->ime_request();
                            const float scale = window.display_scale();
                            if (auto s = window.set_text_input_area(
                                    atlas::platform::Rect2D{.x = ime.x / scale,
                                                            .y = ime.y / scale,
                                                            .width = 1.0F,
                                                            .height = ime.line_height / scale},
                                    0.0F);
                                !s) {
                                ATLAS_LOG_WARN(kApp, "text input area: {}", s.error());
                            }
                        }

                        prepared = overlay->end_frame(*frame);
                    }

                    if (pick.has_value() && !pick->drawn && id_pass.has_value()) {
                        field->cull(pick_chunks);
                        if (auto s = id_pass->render(*frame, *field, pick_chunks); !s) {
                            ATLAS_LOG_WARN(kApp, "the id pass failed: {}", s.error());
                            pick.reset();
                        } else {
                            pick->drawn = true;
                        }
                    }

                    const auto record_start = std::chrono::steady_clock::now();
                    auto pass = frame->begin_render_pass({
                        .colour = {.load = atlas::rhi::LoadOp::Clear,
                                   .clear_colour = {.r = 0.05F, .g = 0.05F, .b = 0.07F}},
                        .debug_name = "lab main pass",
                    });
                    if (!pass) {
                        ATLAS_LOG_ERROR(kApp, "begin_render_pass failed: {}", pass.error());
                    } else {
                        if (const auto snapshot = channel.latest()) {
                            last_draw = field->draw(*pass, *snapshot, mode);
                        }
                        if (overlay.has_value()) {
                            overlay->draw(*pass, prepared);
                        }
                    }
                    phases.draw = micros_since(record_start);
                }
                const auto present_start = std::chrono::steady_clock::now();
                if (const auto status = device->end_frame(std::move(*frame)); !status) {
                    ATLAS_LOG_ERROR(kApp, "end_frame failed: {}", status.error());
                    quit = true;
                } else if (pick.has_value() && pick->drawn && !pick->ticket.has_value()) {
                    auto ticket = id_pass->request_pixel(
                        static_cast<std::uint32_t>(std::max(0.0F, pick->screen.x)),
                        static_cast<std::uint32_t>(std::max(0.0F, pick->screen.y)));
                    if (!ticket) {
                        ATLAS_LOG_WARN(kApp, "pick readback refused: {}", ticket.error());
                        pick.reset();
                    } else {
                        pick->ticket = *ticket;
                        pick->requested_frame = frame_index;
                    }
                }
                present_us += micros_since(present_start);
            }
            phases.present = present_us;
        }
        ATLAS_FRAME_MARK();

        if (options->headless && !options->unbounded && ticks_run == 0) {
            std::this_thread::sleep_for(std::chrono::nanoseconds{
                atlas::app::headless_wait_ns(accumulator->tick_length_ns(), plan.alpha)});
        }
        if (options->max_frames != 0 && frame_index >= options->max_frames) {
            quit = true;
        }
        if (options->max_ticks != 0 && sim.kernel->current_tick() >= options->max_ticks) {
            quit = true;
        }
    }

    // ---- exit: files first, then the summary, then teardown in reverse construction order.
    if (!options->screenshot.empty() && device.has_value()) {
        if (auto capture = device->take_capture()) {
            const std::filesystem::path path{options->screenshot};
            if (const auto status = atlas::app::write_ppm(path, *capture); !status) {
                ATLAS_LOG_ERROR(kApp, "writing the screenshot failed: {}", status.error());
            } else {
                ATLAS_LOG_INFO(kApp, "screenshot written to '{}' ({}x{})", path.string(),
                               capture->extent.width, capture->extent.height);
            }
        } else {
            ATLAS_LOG_WARN(kApp, "no frame was captured for the screenshot");
        }
    }
    if (!options->save_path.empty()) {
        if (auto s = atlas::lab::save_world_to(std::filesystem::path{options->save_path},
                                               sim.lab.world, *sim.kernel, sim.commands);
            !s) {
            return std::unexpected(std::move(s).error().context("saving at exit"));
        }
        ATLAS_LOG_INFO(kApp, "saved '{}' at tick {}", options->save_path,
                       sim.kernel->current_tick());
    }
    if (recorder.has_value()) {
        const auto replay = recorder->take();
        auto bytes = replay.to_bytes();
        if (!bytes) {
            return std::unexpected(std::move(bytes).error().context("writing the replay"));
        }
        if (auto s = atlas::lab::write_file_bytes_atomically(
                std::filesystem::path{options->record_path}, *bytes);
            !s) {
            return std::unexpected(std::move(s).error().context("writing the replay"));
        }
        ATLAS_LOG_INFO(kApp, "replay written to '{}': {} ticks, {} commands, {} checkpoints",
                       options->record_path, replay.tick_count, replay.commands.size(),
                       replay.checkpoints.size());
    }

    if (audio.has_value()) {
        const auto stats = audio->stats();
        ATLAS_LOG_INFO(kApp, "audio: {} voices peak, {} underruns, {} refused{}", stats.voices_peak,
                       stats.underruns, stats.plays_refused,
                       stats.null_device ? ", no device" : "");
    }
    counters.report();
    const std::uint64_t ticks_total = sim.kernel->current_tick() - first_tick;
    const auto elapsed_us = std::max<std::uint64_t>(1, micros_since(run_start));
    report_mod(*mods, "");
    ATLAS_LOG_INFO(kApp, "final tick={} state hash={:#018x} late commands={} pick disagreements={}",
                   sim.kernel->current_tick(), last_hash, sim.kernel->late_commands(),
                   pick_mismatches);
    ATLAS_LOG_INFO(kApp,
                   "observed {:.0f} ticks/s over {} ticks and {} cells (an observation of this "
                   "run, not a benchmark; see atlas_bench --filter simulation)",
                   static_cast<double>(ticks_total) * 1'000'000.0 / static_cast<double>(elapsed_us),
                   ticks_total, sim.lab.layout.cell_count());

    overlay.reset();
    id_pass.reset();
    field.reset();
    if (device.has_value()) {
        if (const auto status = device->wait_idle(); !status) {
            ATLAS_LOG_WARN(kApp, "wait_idle at shutdown: {}", status.error());
        }
    }
    device.reset();
    ATLAS_LOG_INFO(kApp, "shutdown");
    return atlas::ok();
}

}  // namespace

int main(int argc, char** argv) {
    return atlas::app::guarded_main("atlas_lab", [&] { return run(argc, argv); });
}
