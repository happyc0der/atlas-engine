// SPDX-License-Identifier: GPL-3.0-or-later
//
// Chess: the first game on the engine, and a probe of it (ADR-0018).
//
// A composition root. The rules are `atlas::chess_sim`, fenced to the simulation contract; the
// board is `atlas::chess_view`, which turns a position into quads; this file puts a window, a
// device, and the people playing around them. It is written from the lab's and the sandbox's
// shapes rather than shared with them, by ADR-0018 D5: their loops agree in their ingredients
// and not in their shape.
//
// Every move a person makes becomes a `chess.move` command and reaches the board only through
// the kernel. The legal targets shown under a selected piece are a prediction made here so a
// person is not kept waiting; the rules inside the tick are the authority, and a move they
// decline changes nothing (ADR-0019).

#include <atlas/app/log_options.hpp>
#include <atlas/app/main_guard.hpp>
#include <atlas/app/mods.hpp>
#include <atlas/app/ppm.hpp>
#include <atlas/app/socket_session.hpp>
#include <atlas/assets/importer.hpp>
#include <atlas/assets/registry.hpp>
#include <atlas/chess/board_layout.hpp>
#include <atlas/chess/board_quads.hpp>
#include <atlas/chess/fen.hpp>
#include <atlas/chess/mod_views.hpp>
#include <atlas/chess/position.hpp>
#include <atlas/chess/rules.hpp>
#include <atlas/chess/world.hpp>
#include <atlas/core/args.hpp>
#include <atlas/core/assert.hpp>
#include <atlas/core/build_info.hpp>
#include <atlas/core/log.hpp>
#include <atlas/core/profile.hpp>
#include <atlas/core/result.hpp>
#include <atlas/core/time.hpp>
#include <atlas/math/camera.hpp>
#include <atlas/net/enet_hub.hpp>
#include <atlas/net/session.hpp>
#include <atlas/platform/platform.hpp>
#include <atlas/renderer/quad_batch.hpp>
#include <atlas/renderer/texture_cache.hpp>
#include <atlas/rhi/device.hpp>
#include <atlas/script/mod_host.hpp>
#include <atlas/simulation/kernel.hpp>
#include <atlas/simulation/turn_gate.hpp>
#include <atlas/text/catalog.hpp>
#include <atlas/text/substitute.hpp>
#include <atlas/tools/debug_ui.hpp>

#include "chess_text_keys.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

constexpr atlas::log::Category kApp = atlas::log::category::kApp;

#ifdef NDEBUG
constexpr bool kDebugBuild = false;
#else
constexpr bool kDebugBuild = true;
#endif

/// The kernel's seed. Chess draws no random numbers, so its value changes nothing, and it is
/// fixed rather than taken from the command line so that two runs of one game hash alike.
constexpr std::uint64_t kSeed = 0x0C4E'5500'0000'0000ULL;

struct Options {
    bool headless = false;
    bool no_render = false;
    bool flip = false;
    std::uint64_t max_frames = 0;
    std::string_view video_driver;
    std::string_view screenshot;
    std::string_view assets_dir = "assets/source";
    /// The engine's own data, from engine_data(): its sprite shaders and its string table.
    std::string shader_dir;
    std::string engine_strings_dir;
    std::string_view fen;
    std::string_view moves;
    std::string_view log_level = "info";
    std::string_view log_file;
    /// Over a socket: host a game as white, or join one as black.
    bool listen = false;
    std::uint16_t listen_port = 0;
    std::string_view connect;
    std::uint32_t input_delay = 2;
    /// A mod to play one side, or both, in a local game (ADR-0023).
    std::string_view mod;
    std::string_view mods_dir = "assets/mods";
    atlas::chess::Seat mod_seat = atlas::chess::Seat::Black;

    [[nodiscard]] bool networked() const noexcept { return listen || !connect.empty(); }
};

void print_usage() {
    std::printf(R"(atlas_chess — chess on the Atlas engine

Usage: atlas_chess [options]

  --fen FEN              Start from this position rather than the standard one.
  --moves LIST           Play these moves first, comma-separated in coordinates:
                         e2e4,e7e5,g1f3. A promotion names its piece: e7e8q.
                         A move the rules decline ends the run with an error.
  --headless             No window: play --moves, print the result and exit.
  --flip                 Black at the bottom of the board.
  --frames N             Stop after N frames. 0, the default, runs until closed.
  --video-driver NAME    SDL video driver, e.g. dummy.
  --no-render            Open a window but create no graphics device.
  --screenshot PATH      Write the last frame to PATH as a PPM image.
  --assets-dir PATH      Directory to mount as the asset root. Default: assets/source.
  --engine-data-dir PATH The engine's own data, laid out as an installed Atlas's share/atlas:
                         PATH/shaders and PATH/strings. Default: set when this was built,
                         and printed below.
  --shader-dir PATH      Where the cooked shaders are. Default: the engine's data.
  --log-level LEVEL      trace, debug, info, warning, error. Default: info.
  --log-file PATH        Also write the log to PATH.
  --listen [PORT]        Host a game over a socket, playing white. 0, or omitted, lets the
                         operating system choose the port, which is printed.
  --connect HOST:PORT    Join a hosted game, playing black.
  --input-delay N        Ticks between a move and its arrival, 1..16. Default 2.
  --mod NAME             Play against a sandboxed mod from the mods directory, such as
                         chess_opponent.wasm. Local games only. With --moves, the list is
                         the person's moves, played in turn, and the mod answers each.
  --mod-plays SIDE       white, black or both. Default black. Both plays the mod against
                         itself until the game ends.
  --mods-dir PATH        Where --mod looks. Default: assets/mods.
  --text-check           Resolve every chess string against the loaded tables, then exit.
  --version              Print build identity and exit.
  --help                 Print this message and exit.

In a window: click a piece, then a square it may move to. A pawn reaching the last rank
becomes a queen; --moves can name any promotion. Escape or the close button quits.

Over a socket each side plays its own colour, and --moves lists only that side's moves,
played in turn. The game ends where the rules say it does, and both sides finish there.
)");
    std::printf("\nThe engine's data, by default: shaders in %s, strings in %s\n",
                ATLAS_CHESS_SHADER_DIR, ATLAS_CHESS_ENGINE_STRINGS_DIR);
}

/// Where the engine keeps its own data: the sprite shaders and the engine's string table.
///
/// Chess's own strings, textures and mods are under --assets-dir and --mods-dir, and are chess's.
/// These are the engine's, and where they are depends on where the engine came from: this
/// repository's asset folders, or an installed Atlas's share/atlas (ADR-0024 D8). So the defaults
/// are set by the build, ATLAS_CHESS_SHADER_DIR and ATLAS_CHESS_ENGINE_STRINGS_DIR, and
/// --engine-data-dir names a directory laid out as the installed one. --shader-dir still wins for
/// the shaders alone.
struct EngineData {
    std::string shader_dir;
    std::string strings_dir;
};

[[nodiscard]] EngineData engine_data(const atlas::Args& args) {
    EngineData data{.shader_dir = ATLAS_CHESS_SHADER_DIR,
                    .strings_dir = ATLAS_CHESS_ENGINE_STRINGS_DIR};
    if (const auto dir = args.value_or("engine-data-dir", std::string_view{}); !dir.empty()) {
        data.shader_dir = (std::filesystem::path{dir} / "shaders").string();
        data.strings_dir = (std::filesystem::path{dir} / "strings").string();
    }
    data.shader_dir = std::string{args.value_or("shader-dir", std::string_view{data.shader_dir})};
    return data;
}

[[nodiscard]] atlas::Result<Options> read_options(const atlas::Args& args) {
    Options options;
    options.headless = args.has("headless");
    options.no_render = args.has("no-render");
    options.flip = args.has("flip");
    const auto frames = args.value_or("frames", std::uint64_t{0});
    if (!frames) {
        return std::unexpected(frames.error());
    }
    options.max_frames = *frames;
    options.video_driver = args.value_or("video-driver", std::string_view{});
    options.screenshot = args.value_or("screenshot", std::string_view{});
    options.assets_dir = args.value_or("assets-dir", options.assets_dir);
    EngineData engine = engine_data(args);
    options.shader_dir = std::move(engine.shader_dir);
    options.engine_strings_dir = std::move(engine.strings_dir);
    options.fen = args.value_or("fen", std::string_view{});
    options.moves = args.value_or("moves", std::string_view{});
    options.log_level = args.value_or("log-level", options.log_level);
    options.log_file = args.value_or("log-file", std::string_view{});
    options.listen = args.has("listen");
    if (options.listen) {
        // Read as text rather than as a number, because the port is optional: `--listen` on its
        // own is a flag with an empty value, and means "let the operating system choose".
        const std::string_view text = args.value_or("listen", std::string_view{});
        std::uint32_t port = 0;
        if (!text.empty()) {
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), port);
            if (error != std::errc{} || end != text.data() + text.size() || port > 65535) {
                return std::unexpected(
                    atlas::Error(atlas::ErrorCode::InvalidArgument,
                                 std::format("--listen wants a port in 0..65535, got '{}'", text)));
            }
        }
        options.listen_port = static_cast<std::uint16_t>(port);
    }
    options.connect = args.value_or("connect", std::string_view{});
    if (options.listen && !options.connect.empty()) {
        return std::unexpected(atlas::Error(atlas::ErrorCode::InvalidArgument,
                                            "--listen and --connect are two ends of one game"));
    }
    const auto delay = args.value_or("input-delay", std::uint64_t{2});
    if (!delay || *delay == 0 || *delay > 16) {
        return std::unexpected(
            atlas::Error(atlas::ErrorCode::InvalidArgument, "--input-delay wants 1..16"));
    }
    options.input_delay = static_cast<std::uint32_t>(*delay);

    options.mod = args.value_or("mod", std::string_view{});
    options.mods_dir = args.value_or("mods-dir", options.mods_dir);
    const std::string_view seat = args.value_or("mod-plays", std::string_view{});
    if (!seat.empty() && options.mod.empty()) {
        return std::unexpected(
            atlas::Error(atlas::ErrorCode::InvalidArgument, "--mod-plays needs --mod"));
    }
    if (seat == "white") {
        options.mod_seat = atlas::chess::Seat::White;
    } else if (seat == "both") {
        options.mod_seat = atlas::chess::Seat::Both;
    } else if (!seat.empty() && seat != "black") {
        return std::unexpected(
            atlas::Error(atlas::ErrorCode::InvalidArgument,
                         std::format("--mod-plays wants white, black or both, got '{}'", seat)));
    }
    if (!options.mod.empty() && options.networked()) {
        // ADR-0023 D9. Every peer runs the same mods, so a mod playing over a socket raises a
        // question about seats this milestone does not answer; it is deferred with its trigger.
        return std::unexpected(atlas::Error(atlas::ErrorCode::InvalidArgument,
                                            "--mod plays a local game; over a socket it is not "
                                            "supported"));
    }
    if (options.mod_seat == atlas::chess::Seat::Both && !options.moves.empty()) {
        return std::unexpected(
            atlas::Error(atlas::ErrorCode::InvalidArgument,
                         "the mod plays both sides, so --moves has nobody to play them"));
    }
    if (auto status = args.reject_unknown(); !status) {
        return std::unexpected(status.error());
    }
    return options;
}

[[nodiscard]] std::string_view seat_name(atlas::chess::Seat seat) noexcept {
    switch (seat) {
    case atlas::chess::Seat::White: return "white";
    case atlas::chess::Seat::Black: return "black";
    case atlas::chess::Seat::Both: return "both sides";
    }
    return "unknown";
}

/// A game: the world, an empty schedule, the queue and the kernel, owned together because the
/// kernel borrows the other three.
struct Game {
    atlas::chess::ChessWorld chess;
    atlas::sim::Schedule schedule;
    atlas::sim::CommandQueue commands;
    /// Present only in a game played over a socket. Owned here because the kernel borrows it.
    std::unique_ptr<atlas::sim::TurnGate> gate;
    std::unique_ptr<atlas::sim::Kernel> kernel;
    std::size_t plies = 0;
    std::optional<atlas::chess::Move> last_move;

    [[nodiscard]] atlas::chess::Position position() const {
        return atlas::chess::Position::from_tables(
            atlas::chess::board_table(chess.world, chess.ids),
            atlas::chess::state_table(chess.world, chess.ids));
    }

    [[nodiscard]] const atlas::chess::ResultTable& result() const {
        return atlas::chess::result_table(chess.world, chess.ids);
    }
};

/// `record_moves` keeps the kernel's record of applied commands, so a move nobody on this side
/// made — a partner's over a socket, or a mod's — can still be counted and shown.
[[nodiscard]] atlas::Result<std::unique_ptr<Game>> make_game(std::string_view fen, bool networked,
                                                             bool record_moves) {
    auto made = atlas::chess::make_world();
    if (!made) {
        return std::unexpected(std::move(made).error());
    }
    auto game = std::make_unique<Game>();
    game->chess = std::move(*made);

    if (!fen.empty()) {
        auto position = atlas::chess::parse_fen(fen);
        if (!position) {
            return std::unexpected(std::move(position).error().context("reading --fen"));
        }
        atlas::chess::set_position(game->chess.world, game->chess.ids, *position);
        // A FEN is a claim about a position; this is where it is held to the rules' idea of
        // one — kings, pawns, rights, the en passant file — before any move is made from it.
        if (auto status = atlas::chess::validate_world(game->chess.world, game->chess.ids);
            !status) {
            return std::unexpected(std::move(status).error().context("the --fen position"));
        }
    }

    if (auto status = game->schedule.finalise(game->chess.world); !status) {
        return std::unexpected(std::move(status).error());
    }
    if (auto status = atlas::chess::register_chess_commands(game->commands, game->chess.ids);
        !status) {
        return std::unexpected(std::move(status).error());
    }
    if (networked) {
        // Who holds which colour is simulation state, hashed and identical on both sides, so
        // "not your turn" is a decline the rules make and no client can skip. Written before
        // the handshake, because the initial hash both peers compare includes it.
        auto& players = atlas::chess::players_table(game->chess.world, game->chess.ids);
        players.white = atlas::sim::SourceId{0};
        players.black = atlas::sim::SourceId{1};
        game->gate = std::make_unique<atlas::sim::TurnGate>();
    }
    game->kernel = std::make_unique<atlas::sim::Kernel>(
        game->chess.world, game->schedule, game->commands,
        atlas::sim::KernelConfig{.seed = kSeed,
                                 .record_applied_commands = networked || record_moves,
                                 .gate = game->gate.get()});
    return game;
}

/// Submit a move for the tick about to run, run it, and say whether the rules applied it.
[[nodiscard]] atlas::Result<bool> play(Game& game, atlas::chess::Move move) {
    const auto payload = atlas::chess::encode_move(move);
    if (auto status = game.commands.submit(game.kernel->current_tick(), atlas::sim::SourceId::Local,
                                           atlas::chess::kMoveCommand, payload);
        !status) {
        return std::unexpected(std::move(status).error());
    }
    auto report = game.kernel->step();
    if (!report) {
        return std::unexpected(std::move(report).error());
    }
    if (report->commands_applied == 1) {
        ++game.plies;
        game.last_move = move;
        return true;
    }
    return false;
}

/// Play a comma-separated list of moves, stopping at the first the rules decline.
[[nodiscard]] atlas::Status play_script(Game& game, std::string_view list) {
    std::size_t start = 0;
    while (start < list.size()) {
        const std::size_t comma = std::min(list.find(',', start), list.size());
        const std::string_view text = list.substr(start, comma - start);
        start = comma + 1;
        if (text.empty()) {
            continue;
        }
        auto move = atlas::chess::parse_move(text);
        if (!move) {
            return std::unexpected(std::move(move).error().context("reading --moves"));
        }
        auto applied = play(game, *move);
        if (!applied) {
            return std::unexpected(std::move(applied).error());
        }
        if (!*applied) {
            return atlas::fail(atlas::ErrorCode::InvalidArgument,
                               std::format("move {} ({}) was declined by the rules in {}",
                                           game.plies + 1, text,
                                           atlas::chess::to_fen(game.position())));
        }
    }
    return atlas::ok();
}

[[nodiscard]] std::string_view describe(const atlas::chess::ResultTable& result) {
    using atlas::chess::Outcome;
    using atlas::chess::Reason;
    switch (result.reason) {
    case Reason::None: return "ongoing";
    case Reason::Checkmate:
        return result.outcome == Outcome::WhiteWins ? "white wins by checkmate"
                                                    : "black wins by checkmate";
    case Reason::Stalemate: return "draw by stalemate";
    case Reason::FiftyMoves: return "draw by the fifty-move rule";
    case Reason::Threefold: return "draw by threefold repetition";
    case Reason::InsufficientMaterial: return "draw by insufficient material";
    }
    return "unknown";
}

/// What a run ends by printing. A diagnostic for whoever runs it and for the integration cases,
/// which is why it is not routed through a string table.
void print_summary(const Game& game) {
    std::printf("chess: plies=%zu tick=%llu\n", game.plies,
                static_cast<unsigned long long>(game.kernel->current_tick()));
    std::printf("position: %s\n", atlas::chess::to_fen(game.position()).c_str());
    std::printf("result: %s\n", std::string{describe(game.result())}.c_str());
    std::printf("state hash: %#018llx\n", static_cast<unsigned long long>(game.chess.world.hash()));
    std::fflush(stdout);
}

// ----------------------------------------------------------------------------------- opponent

[[nodiscard]] atlas::Result<std::deque<atlas::chess::Move>> parse_script(std::string_view list);

/// A mod playing one side, or both, in a local game (ADR-0023).
///
/// Declared so that the host goes before the runtime it was created in.
struct Opponent {
    atlas::app::LoadedMod loaded;
    std::unique_ptr<atlas::script::ModHost> host;
    atlas::chess::Seat seat = atlas::chess::Seat::Black;
    /// A mod never marks a gate; a host is polled with one because every command source is.
    atlas::sim::TurnGate gate;
    /// The person's --moves, played in turn.
    std::deque<atlas::chess::Move> script;

    [[nodiscard]] bool plays(atlas::chess::Colour colour) const noexcept {
        return seat == atlas::chess::Seat::Both ||
               (seat == atlas::chess::Seat::White) == (colour == atlas::chess::Colour::White);
    }
};

/// Load the mod and give it its seat or seats in the players table.
///
/// **The seat is simulation state.** `chess.players` names the mod's identifier for each colour it
/// plays, so "not your turn" stays a decline the rules make for the mod exactly as for a person,
/// and a person's click on the mod's side is declined by the same rule.
[[nodiscard]] atlas::Result<std::unique_ptr<Opponent>> open_opponent(const Options& options,
                                                                     Game& game) {
    auto opponent = std::make_unique<Opponent>();
    auto loaded = atlas::app::open_mod(options.mod, options.mods_dir, false);
    if (!loaded) {
        return std::unexpected(std::move(loaded).error());
    }
    opponent->loaded = *std::move(loaded);
    // A named mod always comes with a runtime; checked rather than assumed, since `--mod` with an
    // empty name would have reached here with none.
    if (!opponent->loaded.runtime.has_value()) {
        return std::unexpected(
            atlas::Error(atlas::ErrorCode::InvalidArgument, "--mod names no mod"));
    }
    auto host =
        atlas::script::ModHost::create(*opponent->loaded.runtime, 0, opponent->loaded.bytes,
                                       opponent->loaded.name, {.seed = kSeed, .input_delay = 1});
    if (!host) {
        return std::unexpected(std::move(host).error());
    }
    opponent->host = *std::move(host);
    if (auto status = opponent->host->start(); !status) {
        return std::unexpected(std::move(status).error().context("starting the mod"));
    }
    opponent->seat = options.mod_seat;

    auto& players = atlas::chess::players_table(game.chess.world, game.chess.ids);
    if (opponent->plays(atlas::chess::Colour::White)) {
        players.white = opponent->host->id();
    }
    if (opponent->plays(atlas::chess::Colour::Black)) {
        players.black = opponent->host->id();
    }
    auto script = parse_script(options.moves);
    if (!script) {
        return std::unexpected(std::move(script).error());
    }
    opponent->script = *std::move(script);
    ATLAS_LOG_INFO(kApp, "mod '{}' plays {} as source {}", opponent->loaded.name,
                   seat_name(opponent->seat), static_cast<std::uint32_t>(opponent->host->id()));
    return opponent;
}

/// One tick of a local game with a mod: publish the position, let the mod decide, submit the
/// person's move if there is one, and run the tick.
///
/// **Once per tick, before the tick** (ADR-0015 D4), exactly as the lab polls its mod. Returns
/// whether the person's move, if one was given, was applied.
[[nodiscard]] atlas::Result<bool> advance_local(Game& game, Opponent& opponent,
                                                std::optional<atlas::chess::Move> person) {
    const auto view = atlas::chess::position_view(game.chess.world, game.chess.ids);
    const std::array<std::uint8_t, 1> seat{static_cast<std::uint8_t>(opponent.seat)};
    const std::array<atlas::script::ModView, 2> views{
        atlas::script::ModView{.name = "chess.position", .bytes = std::as_bytes(std::span(view))},
        atlas::script::ModView{.name = "chess.seat", .bytes = std::as_bytes(std::span(seat))},
    };
    opponent.host->set_views(views);
    if (const auto report =
            opponent.host->poll(game.kernel->current_tick(), game.commands, opponent.gate);
        !report) {
        // Cannot happen by design — a mod that misbehaves is reported closed rather than as an
        // error — but a driver that ignored it would be assuming that on the reader's behalf.
        ATLAS_LOG_ERROR(kApp, "polling the mod failed: {}", report.error());
    }
    atlas::app::say_messages(*opponent.host, nullptr, "");

    if (person.has_value()) {
        const auto payload = atlas::chess::encode_move(*person);
        if (auto status =
                game.commands.submit(game.kernel->current_tick(), atlas::sim::SourceId::Local,
                                     atlas::chess::kMoveCommand, payload);
            !status) {
            return std::unexpected(std::move(status).error());
        }
    }
    auto stepped = game.kernel->step();
    if (!stepped) {
        return std::unexpected(std::move(stepped).error());
    }
    bool person_applied = false;
    for (const auto& applied : stepped->applied_commands) {
        if (auto decoded = atlas::chess::decode_move(applied.payload)) {
            ++game.plies;
            game.last_move = *decoded;
            const bool from_mod = applied.source == opponent.host->id();
            person_applied = person_applied || !from_mod;
            ATLAS_LOG_INFO(kApp, "ply {}: {}{}", game.plies, atlas::chess::to_string(*decoded),
                           from_mod ? " (the mod)" : "");
        }
    }
    return person_applied;
}

/// A local game with a mod and no window: the person's --moves in turn, the mod's answers,
/// until the game ends or the person has no move left to play.
[[nodiscard]] atlas::Status run_local_headless(Game& game, Opponent& opponent) {
    // A game against itself ends by the rules — mate, stalemate, fifty moves, repetition or
    // material — long before this. The bound is for a mod that stopped answering.
    constexpr std::uint64_t kMaxTicks = 200'000;
    for (std::uint64_t ticks = 0; ticks < kMaxTicks; ++ticks) {
        if (game.result().outcome != atlas::chess::Outcome::Ongoing) {
            return atlas::ok();
        }
        std::optional<atlas::chess::Move> person;
        if (!opponent.plays(game.position().side_to_move)) {
            if (opponent.script.empty()) {
                return atlas::ok();
            }
            person = opponent.script.front();
            opponent.script.pop_front();
        }
        if (opponent.host->disabled()) {
            return atlas::fail(
                atlas::ErrorCode::Unavailable,
                std::format("the mod stopped: {}", opponent.host->disabled_because()));
        }
        auto applied = advance_local(game, opponent, person);
        if (!applied) {
            return std::unexpected(std::move(applied).error());
        }
        if (person.has_value() && !*applied) {
            return atlas::fail(atlas::ErrorCode::InvalidArgument,
                               std::format("move {} ({}) was declined by the rules in {}",
                                           game.plies + 1, atlas::chess::to_string(*person),
                                           atlas::chess::to_fen(game.position())));
        }
    }
    return atlas::fail(atlas::ErrorCode::Unavailable,
                       std::format("the game did not end within {} ticks", kMaxTicks));
}

/// What the mod did, for whoever runs it and for the integration cases.
void print_opponent(const Opponent& opponent) {
    atlas::app::report_mod(*opponent.host, "");
    const auto stats = opponent.host->stats();
    std::printf("opponent: mod '%s' plays %s, %llu move(s) submitted, %s\n",
                opponent.loaded.name.c_str(), std::string{seat_name(opponent.seat)}.c_str(),
                static_cast<unsigned long long>(stats.commands_submitted),
                opponent.host->disabled() ? "disabled" : "still running");
    std::fflush(stdout);
}

// ------------------------------------------------------------------------------------ network

/// A game played over a socket: the link, the session, and this side's bookkeeping.
///
/// Declared so that the session goes before the hub and the hub before the runtime, which is
/// the order each borrows from the next.
struct Network {
    std::optional<atlas::net::EnetRuntime> runtime;
    std::unique_ptr<atlas::net::EnetHub> hub;
    std::unique_ptr<atlas::net::Session> session;

    /// The listener plays white as source 0 and the connector black as source 1, which is what
    /// the players table says on both sides.
    atlas::chess::Colour colour = atlas::chess::Colour::White;
    /// The next tick this side has still to announce.
    atlas::Tick next_turn = 0;
    /// The tick this side's last move is stamped for. Another move of its own waits until that
    /// tick has run, so one side never has two moves in flight.
    std::optional<atlas::Tick> in_flight;
    /// The tick the result was set on, which is where both sides finish.
    std::optional<atlas::Tick> result_tick;
    /// --moves, over a socket: this side's moves only, played in turn.
    std::deque<atlas::chess::Move> script;
};

/// Generous, because a continuous-integration runner under load is slow rather than broken,
/// and bounded, because a game that hangs is a job that times out with nothing to read.
constexpr auto kConnectTimeout = std::chrono::seconds{30};

[[nodiscard]] atlas::Result<std::deque<atlas::chess::Move>> parse_script(std::string_view list) {
    std::deque<atlas::chess::Move> moves;
    std::size_t start = 0;
    while (start < list.size()) {
        const std::size_t comma = std::min(list.find(',', start), list.size());
        const std::string_view text = list.substr(start, comma - start);
        start = comma + 1;
        if (text.empty()) {
            continue;
        }
        auto move = atlas::chess::parse_move(text);
        if (!move) {
            return std::unexpected(std::move(move).error().context("reading --moves"));
        }
        moves.push_back(*move);
    }
    return moves;
}

/// Open the socket, create the session, and wait until both sides agree.
[[nodiscard]] atlas::Result<std::unique_ptr<Network>> open_network(const Options& options,
                                                                   Game& game) {
    auto network = std::make_unique<Network>();
    auto runtime = atlas::net::EnetRuntime::create();
    if (!runtime) {
        return std::unexpected(std::move(runtime).error().context("the socket library"));
    }
    network->runtime = std::move(*runtime);

    atlas::app::SocketEndpoint endpoint{.listen = true, .port = options.listen_port};
    if (!options.listen) {
        auto parsed = atlas::app::parse_connect(options.connect);
        if (!parsed) {
            return std::unexpected(std::move(parsed).error());
        }
        endpoint = *std::move(parsed);
    }
    auto hub = atlas::app::open_socket_hub(*network->runtime, endpoint, kConnectTimeout);
    if (!hub) {
        return std::unexpected(std::move(hub).error());
    }
    network->hub = *std::move(hub);
    network->colour = network->hub->local_index() == 0 ? atlas::chess::Colour::White
                                                       : atlas::chess::Colour::Black;

    auto session =
        atlas::net::Session::create(network->hub->end(network->hub->local_index()),
                                    {.input_delay = options.input_delay,
                                     .hash_check_interval = 8,
                                     .seed = kSeed,
                                     .start_tick = 0,
                                     .initial_state_hash = game.chess.world.hash(),
                                     .tick_rate = 60,
                                     .build_id = std::string{atlas::build_info::summary()}});
    if (!session) {
        return std::unexpected(std::move(session).error());
    }
    network->session = *std::move(session);
    network->session->set_schedule(&game.schedule);
    if (auto status = atlas::app::await_handshake(*network->session, *network->hub, game.commands,
                                                  *game.gate, kConnectTimeout);
        !status) {
        return std::unexpected(std::move(status).error());
    }
    ATLAS_LOG_INFO(kApp, "playing {} over a socket, input delay {}",
                   network->colour == atlas::chess::Colour::White ? "white" : "black",
                   network->session->agreed_delay());
    return network;
}

/// Whether this side may make a move now: its turn, the game not over, and its last move run.
[[nodiscard]] bool may_move(const Game& game, const Network& network) {
    if (game.result().outcome != atlas::chess::Outcome::Ongoing) {
        return false;
    }
    if (network.in_flight.has_value() && game.kernel->current_tick() <= *network.in_flight) {
        return false;
    }
    return game.position().side_to_move == network.colour;
}

/// One step of a networked game: bring in what arrived, announce this side's turns with its
/// move if it has one, run what the gate allows, and finish where the result is set.
[[nodiscard]] atlas::Status advance(Game& game, Network& network,
                                    std::optional<atlas::chess::Move> move) {
    atlas::net::Session& session = *network.session;
    atlas::sim::TurnGate& gate = *game.gate;
    const atlas::Tick now = game.kernel->current_tick();

    auto report = session.poll(now, game.commands, gate);
    if (!report) {
        return std::unexpected(std::move(report).error().context("the session"));
    }
    if (session.finished()) {
        return atlas::ok();
    }

    // No check here of a partner's declared finish against this side's game, unlike the lab's
    // against its bound. The two games differ only if the states diverged, and then the session
    // already refuses to run past the partner's finish (ADR-0020): a second check would be a
    // branch no test could reach.
    if (session.state() != atlas::net::SessionState::Running) {
        return atlas::ok();
    }

    const atlas::Tick horizon = now + session.agreed_delay();
    while (network.next_turn <= horizon) {
        std::vector<atlas::sim::Command> turn;
        if (move.has_value()) {
            const auto self = session.self();
            const auto payload = atlas::chess::encode_move(*move);
            atlas::sim::Command command{
                .target = network.next_turn,
                .source = self,
                .sequence = game.commands.next_sequence(self),
                .type = atlas::chess::kMoveCommand,
                .payload = {payload.begin(), payload.end()},
            };
            if (auto status = game.commands.submit_stamped(command); !status) {
                return status;
            }
            network.in_flight = network.next_turn;
            turn.push_back(std::move(command));
            move.reset();
        }
        if (auto status = session.send_turn(network.next_turn, turn, gate); !status) {
            return status;
        }
        ++network.next_turn;
    }

    auto second = session.poll(game.kernel->current_tick(), game.commands, gate);
    if (!second) {
        return std::unexpected(std::move(second).error().context("the session"));
    }

    // Nothing runs past the tick the game ended on: that is where both sides finish, and a
    // side that ran further would have run a tick its partner never will.
    while (!network.result_tick.has_value() && game.kernel->ready()) {
        auto stepped = game.kernel->step();
        if (!stepped) {
            return std::unexpected(std::move(stepped).error());
        }
        gate.retire_before(stepped->tick);
        if (auto status =
                session.send_hash_check(stepped->tick, stepped->state_hash, stepped->system_hashes);
            !status) {
            return status;
        }
        for (const auto& applied : stepped->applied_commands) {
            if (auto decoded = atlas::chess::decode_move(applied.payload)) {
                ++game.plies;
                game.last_move = *decoded;
                ATLAS_LOG_INFO(kApp, "ply {}: {}", game.plies, atlas::chess::to_string(*decoded));
            }
        }
        if (game.result().outcome != atlas::chess::Outcome::Ongoing) {
            network.result_tick = stepped->tick;
        }
    }

    if (network.result_tick.has_value()) {
        if (auto status = session.finish(*network.result_tick); !status) {
            return std::unexpected(std::move(status).error().context("finishing the game"));
        }
    }
    return atlas::ok();
}

/// A networked game with no window: play this side's --moves until the game is over.
[[nodiscard]] atlas::Status run_network_headless(Game& game, Network& network) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{120};
    while (!network.session->finished()) {
        std::optional<atlas::chess::Move> move;
        if (may_move(game, network)) {
            if (network.script.empty()) {
                return atlas::fail(atlas::ErrorCode::InvalidArgument,
                                   "--moves ran out before the game ended, on this side's turn");
            }
            move = network.script.front();
            network.script.pop_front();
        }
        if (auto status = advance(game, network, move); !status) {
            return status;
        }
        // Finished first, link second: see `Session::finished`.
        if (network.session->finished()) {
            break;
        }
        if (network.hub->status().ended) {
            return atlas::fail(atlas::ErrorCode::Unavailable,
                               std::format("the partner left before the game was agreed over: {}",
                                           network.hub->status().reason));
        }
        if (std::chrono::steady_clock::now() > deadline) {
            return atlas::fail(atlas::ErrorCode::Unavailable,
                               "the game made no progress before the deadline");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    std::printf("session: finished at tick %llu\n",
                static_cast<unsigned long long>(network.result_tick.value_or(0)));
    return atlas::ok();
}

/// What a click does: pick a piece up, move it, or put it down.
class Selection {
  public:
    /// Handle a click on `square`, returning the move it makes, if any.
    [[nodiscard]] std::optional<atlas::chess::Move> click(const Game& game,
                                                          atlas::chess::Square square) {
        const atlas::chess::Position position = game.position();
        if (m_selected.has_value()) {
            for (const auto& move : m_targets) {
                // Promotion by click is always to a queen; --moves can name any piece.
                if (move.to == square && (move.promotion == atlas::chess::PieceKind::None ||
                                          move.promotion == atlas::chess::PieceKind::Queen)) {
                    clear();
                    return move;
                }
            }
        }
        clear();
        const atlas::chess::Piece piece = position.at(square);
        if (piece != atlas::chess::Piece::None &&
            atlas::chess::colour_of(piece) == position.side_to_move) {
            m_selected = square;
            atlas::chess::MoveList legal;
            atlas::chess::legal_moves(game.chess.world, game.chess.ids, legal);
            for (const auto& move : legal) {
                if (move.from == square) {
                    m_targets.push_back(move);
                }
            }
        }
        return std::nullopt;
    }

    void clear() {
        m_selected.reset();
        m_targets.clear();
    }

    [[nodiscard]] std::optional<atlas::chess::Square> selected() const noexcept {
        return m_selected;
    }

    [[nodiscard]] std::span<const atlas::chess::Move> targets() const noexcept { return m_targets; }

  private:
    std::optional<atlas::chess::Square> m_selected;
    std::vector<atlas::chess::Move> m_targets;
};

/// Read one string table through a validated path.
[[nodiscard]] atlas::Result<atlas::assets::ImportedStringTable>
read_table(const atlas::assets::FileSystem& files, std::string_view name) {
    auto path = atlas::assets::VirtualPath::parse(name);
    if (!path) {
        return std::unexpected(std::move(path).error());
    }
    auto bytes = files.read(*path);
    if (!bytes) {
        return std::unexpected(std::move(bytes).error().context(name));
    }
    return atlas::assets::import_string_table(*bytes, path->text());
}

/// The engine's table, from where the engine keeps its data, then the application's own beside it
/// (ADR-0018, ADR-0024 D8). Read directly rather than through a registry, as the lab reads its
/// table: a validated path is what is needed, and hot reload of the interface's text is not
/// something a game of chess wants. Two mounts, because the two tables need not share a folder.
[[nodiscard]] atlas::Status load_strings(atlas::text::Catalog& catalog,
                                         std::string_view engine_strings_dir,
                                         std::string_view assets_dir) {
    atlas::assets::FileSystem engine_files;
    if (auto status = engine_files.mount("engine", std::filesystem::path{engine_strings_dir});
        !status) {
        return status;
    }
    atlas::assets::FileSystem files;
    if (auto status = files.mount("strings", std::filesystem::path{assets_dir} / "strings");
        !status) {
        return status;
    }
    auto engine = read_table(engine_files, "en.json");
    if (!engine) {
        return std::unexpected(std::move(engine).error().context("the engine's string table"));
    }
    if (auto status = catalog.load(*engine); !status) {
        return status;
    }
    auto own = read_table(files, "chess.en.json");
    if (!own) {
        return std::unexpected(std::move(own).error().context("the chess string table"));
    }
    if (auto status = catalog.add_table(*own); !status) {
        return std::unexpected(std::move(status).error().context("the chess string table"));
    }
    return atlas::ok();
}

/// Resolve every chess key against what is loaded, and fail naming any that is missing.
[[nodiscard]] atlas::Status run_text_check(std::string_view engine_strings_dir,
                                           std::string_view assets_dir) {
    atlas::text::Catalog catalog;
    if (auto status = load_strings(catalog, engine_strings_dir, assets_dir); !status) {
        return status;
    }
    for (const std::string_view key : atlas::chess::keys::kAllKeys) {
        (void)catalog.lookup(key);
    }
    const std::size_t missing = catalog.distinct_misses();
    if (missing > 0) {
        (void)catalog.log_new_misses();
        return atlas::fail(atlas::ErrorCode::NotFound,
                           std::format("{} of {} chess keys have no string", missing,
                                       atlas::chess::keys::kAllKeys.size()));
    }
    std::printf("text check: %zu chess keys, all resolved in '%s', %zu entries in the tables\n",
                atlas::chess::keys::kAllKeys.size(), std::string{catalog.locale()}.c_str(),
                catalog.size());
    return atlas::ok();
}

/// The key naming how a game ended, or that it has not.
[[nodiscard]] std::string_view result_key(const atlas::chess::ResultTable& result) {
    namespace keys = atlas::chess::keys;
    using atlas::chess::Outcome;
    using atlas::chess::Reason;
    switch (result.reason) {
    case Reason::None: return keys::kResultOngoing;
    case Reason::Checkmate:
        return result.outcome == Outcome::WhiteWins ? keys::kResultWhiteMates
                                                    : keys::kResultBlackMates;
    case Reason::Stalemate: return keys::kResultStalemate;
    case Reason::FiftyMoves: return keys::kResultFiftyMoves;
    case Reason::Threefold: return keys::kResultThreefold;
    case Reason::InsufficientMaterial: return keys::kResultInsufficient;
    }
    return keys::kResultOngoing;
}

/// The status panel's four values, resolved here rather than in the panel: a value is the
/// application's to compose, and M18 found what happens when one is handed over as a key.
struct StatusValues {
    std::string to_move;
    std::string move;
    std::string last_move;
    std::string result;
};

[[nodiscard]] StatusValues status_values(const Game& game, const atlas::text::Catalog& catalog) {
    namespace keys = atlas::chess::keys;
    const atlas::chess::Position position = game.position();
    StatusValues values;
    if (game.result().outcome != atlas::chess::Outcome::Ongoing) {
        values.to_move = std::string{catalog.lookup(keys::kGameOver)};
    } else {
        const std::string_view side =
            catalog.lookup(position.side_to_move == atlas::chess::Colour::White ? keys::kSideWhite
                                                                                : keys::kSideBlack);
        if (position.in_check(position.side_to_move)) {
            const std::array<std::string_view, 1> args{side};
            values.to_move = atlas::text::substitute(catalog.lookup(keys::kInCheck), args);
        } else {
            values.to_move = std::string{side};
        }
    }
    values.move = std::format("{}", position.fullmove_number);
    // A move in coordinates is notation, like a FEN: an identifier rather than prose.
    values.last_move = game.last_move.has_value() ? atlas::chess::to_string(*game.last_move)
                                                  : std::string{catalog.lookup(keys::kNoMoveYet)};
    values.result = std::string{catalog.lookup(result_key(game.result()))};
    return values;
}

/// Fit the whole board in the window with a margin.
void frame_board(atlas::math::OrthoCamera& camera, float width, float height) {
    camera.set_viewport(width, height);
    camera.set_centre(atlas::chess::BoardLayout::centre());
    constexpr float kMargin = 1.08F;
    camera.set_zoom(std::min(width, height) / (atlas::chess::BoardLayout::kExtent * kMargin));
}

[[nodiscard]] atlas::Status run_windowed(const Options& options, Game& game, Network* network,
                                         Opponent* opponent) {
    atlas::assets::FileSystem filesystem;
    if (auto status = filesystem.mount("assets", std::filesystem::path{options.assets_dir});
        !status) {
        return std::unexpected(std::move(status).error().context("mounting the asset root"));
    }
    auto registry = atlas::assets::Registry::create(filesystem, {});
    if (!registry) {
        return std::unexpected(std::move(registry).error().context("starting the asset registry"));
    }
    auto sheet_path = atlas::assets::VirtualPath::parse("textures/chess_pieces.png");
    if (!sheet_path) {
        return std::unexpected(std::move(sheet_path).error());
    }
    auto sheet = registry->request(*sheet_path, atlas::assets::AssetType::Texture);
    if (!sheet) {
        return std::unexpected(std::move(sheet).error().context("requesting the piece sheet"));
    }

    auto platform = atlas::platform::Platform::create({
        .video = true,
        .video_driver = options.video_driver,
        .app_name = "Atlas chess",
    });
    if (!platform) {
        return std::unexpected(std::move(platform).error().context("starting the platform"));
    }
    auto window = platform->create_window({.title = "Atlas chess"});
    if (!window) {
        return std::unexpected(std::move(window).error().context("opening a window"));
    }

    std::optional<atlas::rhi::Device> device;
    std::optional<atlas::renderer::TextureCache> textures;
    std::optional<atlas::renderer::QuadBatch> batch;
    if (!options.no_render) {
        auto created = atlas::rhi::Device::create({.debug = kDebugBuild}, *window);
        if (!created) {
            return std::unexpected(
                std::move(created).error().context("creating the graphics device"));
        }
        device = std::move(*created);
        auto cache = atlas::renderer::TextureCache::create(*device);
        if (!cache) {
            return std::unexpected(std::move(cache).error().context("creating the texture cache"));
        }
        textures = std::move(*cache);
        auto quads = atlas::renderer::QuadBatch::create(
            *device, {.capacity = 256, .shader_directory = options.shader_dir});
        if (!quads) {
            return std::unexpected(std::move(quads).error().context("creating the quad batch"));
        }
        batch = std::move(*quads);
    }

    // The interface's text. A table that fails to load is not fatal: the panel then shows its
    // keys, which is what a missing key does everywhere else and is legible rather than blank.
    atlas::text::Catalog catalog;
    if (auto status = load_strings(catalog, options.engine_strings_dir, options.assets_dir);
        !status) {
        ATLAS_LOG_WARN(kApp, "no string tables: {}", status.error());
    }
    std::optional<atlas::tools::DebugUi> overlay;
    if (device.has_value()) {
        auto ui = atlas::tools::DebugUi::create(*device, *window);
        if (ui) {
            overlay = std::move(*ui);
            overlay->set_catalog(&catalog);
        } else {
            ATLAS_LOG_WARN(kApp, "the status panel is unavailable: {}", ui.error());
        }
    }
    atlas::SteadyClock clock;

    // Each player sees their own pieces at the bottom.
    const bool black_below =
        options.flip || (network != nullptr && network->colour == atlas::chess::Colour::Black);
    const atlas::chess::BoardLayout layout{.flipped = black_below};
    atlas::math::OrthoCamera camera;
    Selection selection;
    std::vector<atlas::renderer::Quad> quads;
    quads.reserve(160);
    atlas::chess::Reason reported = game.result().reason;
    std::uint64_t frame_index = 0;
    bool quit = false;
    bool captured = false;

    while (!quit) {
        ATLAS_ZONE_NAMED("frame");
        ++frame_index;
        const std::uint64_t frame_ns = atlas::to_unsigned_ns(clock.tick());
        const auto size = window->pixel_size();
        frame_board(camera, static_cast<float>(size.width), static_cast<float>(size.height));

        std::optional<atlas::chess::Move> chosen;
        for (const auto& event : platform->pump()) {
            // The panel sees every event first: a click on it must not also land on the board.
            if (overlay.has_value() && overlay->handle_event(event)) {
                continue;
            }
            if (std::holds_alternative<atlas::platform::QuitRequested>(event) ||
                std::holds_alternative<atlas::platform::WindowCloseRequested>(event)) {
                quit = true;
            } else if (const auto* key = std::get_if<atlas::platform::KeyPressed>(&event)) {
                if (key->key == atlas::platform::Key::Escape) {
                    quit = true;
                }
            } else if (const auto* click =
                           std::get_if<atlas::platform::MouseButtonPressed>(&event)) {
                if (click->button != atlas::platform::MouseButton::Left) {
                    continue;
                }
                // Over a socket only this side's own turn is clickable, and only once its last
                // move has run: the rules would decline anything else, and a person should not
                // have to wait for the decline to find out.
                if (network != nullptr && !may_move(game, *network)) {
                    selection.clear();
                    continue;
                }
                // Nor the mod's side: its pieces are its to move.
                if (opponent != nullptr && opponent->plays(game.position().side_to_move)) {
                    selection.clear();
                    continue;
                }
                // Events are in logical units and the camera's viewport is in pixels.
                const float scale = window->display_scale();
                const auto world = camera.screen_to_world(
                    {.x = click->position.x * scale, .y = click->position.y * scale});
                if (const auto square = layout.square_at(world)) {
                    chosen = selection.click(game, *square);
                } else {
                    selection.clear();
                }
            }
        }

        if (network != nullptr) {
            if (auto status = advance(game, *network, chosen); !status) {
                return status;
            }
            if (!network->session->finished() && network->hub->status().ended) {
                return atlas::fail(
                    atlas::ErrorCode::Unavailable,
                    std::format("the partner left before the game was agreed over: {}",
                                network->hub->status().reason));
            }
            chosen.reset();
        }
        if (opponent != nullptr) {
            // A game with a mod runs a tick a frame, whether or not anybody clicked: the mod
            // thinks a few moves a tick and answers when it has. --moves, if given, are the
            // person's, taken in turn before any click.
            if (!chosen.has_value() && !opponent->plays(game.position().side_to_move) &&
                !opponent->script.empty() &&
                game.result().outcome == atlas::chess::Outcome::Ongoing) {
                chosen = opponent->script.front();
                opponent->script.pop_front();
            }
            if (game.result().outcome == atlas::chess::Outcome::Ongoing) {
                auto applied = advance_local(game, *opponent, chosen);
                if (!applied) {
                    return std::unexpected(std::move(applied).error());
                }
                if (chosen.has_value() && !*applied) {
                    ATLAS_LOG_WARN(kApp, "{} was declined", atlas::chess::to_string(*chosen));
                }
            }
            chosen.reset();
        }
        if (chosen.has_value()) {
            auto applied = play(game, *chosen);
            if (!applied) {
                return std::unexpected(std::move(applied).error());
            }
            if (*applied) {
                ATLAS_LOG_INFO(kApp, "ply {}: {}", game.plies, atlas::chess::to_string(*chosen));
            } else {
                // Predicted legal and declined anyway would be a disagreement between the
                // prediction and the rules, which is worth hearing about.
                ATLAS_LOG_WARN(kApp, "{} was shown as legal and declined",
                               atlas::chess::to_string(*chosen));
            }
        }
        if (game.result().reason != reported) {
            reported = game.result().reason;
            ATLAS_LOG_INFO(kApp, "game over: {}", describe(game.result()));
        }

        registry->pump();
        if (textures.has_value()) {
            (void)textures->finalise_pending(*registry);
        }

        if (device.has_value() && !window->is_minimized()) {
            auto frame = device->begin_frame();
            if (!frame) {
                if (device->is_lost()) {
                    return atlas::fail(
                        atlas::ErrorCode::DeviceLost,
                        std::format("the graphics device was lost: {}", device->loss_reason()));
                }
                return std::unexpected(std::move(frame).error().context("beginning a frame"));
            }
            if (frame->has_swapchain_target()) {
                const bool last = options.max_frames != 0 && frame_index >= options.max_frames;
                if (!options.screenshot.empty() && !captured && (last || quit)) {
                    device->request_capture();
                    captured = true;
                }

                const atlas::chess::Position position = game.position();
                std::optional<atlas::chess::Square> in_check;
                if (position.in_check(position.side_to_move)) {
                    in_check = position.king_square(position.side_to_move);
                }
                quads.clear();
                atlas::chess::build_board_quads(position,
                                                {.selected = selection.selected(),
                                                 .targets = selection.targets(),
                                                 .last_move = game.last_move,
                                                 .in_check = in_check},
                                                layout, quads);

                atlas::tools::DebugUi::PreparedFrame prepared;
                if (overlay.has_value()) {
                    namespace keys = atlas::chess::keys;
                    const StatusValues values = status_values(game, catalog);
                    const std::array<atlas::tools::Stat, 4> stats{{
                        {.label = keys::kStatToMove, .value = values.to_move},
                        {.label = keys::kStatMove, .value = values.move},
                        {.label = keys::kStatLastMove, .value = values.last_move},
                        {.label = keys::kStatResult, .value = values.result},
                    }};
                    overlay->begin_frame(static_cast<float>(frame_ns) / 1'000'000'000.0F,
                                         size.width, size.height);
                    overlay->stats_panel(keys::kTitleGame, stats);
                    prepared = overlay->end_frame(*frame);
                }

                auto pass = frame->begin_render_pass({
                    .colour = {.load = atlas::rhi::LoadOp::Clear,
                               .clear_colour = {.r = 0.10F, .g = 0.11F, .b = 0.13F}},
                    .debug_name = "chess board",
                });
                if (!pass) {
                    return std::unexpected(std::move(pass).error().context("the board pass"));
                }
                batch->begin(*pass, camera.view_projection());
                batch->set_texture(textures->texture_for(*sheet), textures->sampler());
                batch->add(quads);
                (void)batch->end();
                if (overlay.has_value()) {
                    overlay->draw(*pass, prepared);
                }
            }
            if (auto status = device->end_frame(std::move(*frame)); !status) {
                return std::unexpected(std::move(status).error().context("ending a frame"));
            }
        }

        if (options.max_frames != 0 && frame_index >= options.max_frames) {
            quit = true;
        }
    }

    // Leaving a game that is not over says so, and the partner hears it as a peer that left,
    // which is what it is. Leaving one that is over says nothing: `quit` is quiet once the
    // session has finished, so a completed game is not turned into a failed one on the far side.
    if (network != nullptr && !network->session->finished()) {
        ATLAS_LOG_INFO(kApp, "leaving a game that is not over");
        network->session->quit();
    }

    if (!options.screenshot.empty() && device.has_value()) {
        if (auto capture = device->take_capture()) {
            if (auto status =
                    atlas::app::write_ppm(std::filesystem::path{options.screenshot}, *capture);
                !status) {
                return std::unexpected(std::move(status).error().context("the screenshot"));
            }
        }
    }
    return atlas::ok();
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
    if (args->has("text-check")) {
        return run_text_check(engine_data(*args).strings_dir,
                              args->value_or("assets-dir", std::string_view{"assets/source"}));
    }
    const auto options = read_options(*args);
    if (!options) {
        return std::unexpected(options.error());
    }

    const atlas::app::LogSession logging;
    if (auto status = atlas::app::configure_logging(options->log_level, options->log_file, {});
        !status) {
        return status;
    }
    ATLAS_THREAD_NAME("main");
    // Normally the platform records this, and a headless game creates no platform; the kernel
    // asserts it on every step either way.
    atlas::mark_main_thread();
    ATLAS_LOG_INFO(kApp, "startup: {}", atlas::build_info::summary());

    auto game = make_game(options->fen, options->networked(), !options->mod.empty());
    if (!game) {
        return std::unexpected(std::move(game).error());
    }

    if (options->networked()) {
        auto network = open_network(*options, **game);
        if (!network) {
            return std::unexpected(std::move(network).error());
        }
        auto script = parse_script(options->moves);
        if (!script) {
            return std::unexpected(std::move(script).error());
        }
        (*network)->script = *std::move(script);
        const auto status = options->headless
                                ? run_network_headless(**game, **network)
                                : run_windowed(*options, **game, network->get(), nullptr);
        print_summary(**game);
        return status;
    }

    if (!options->mod.empty()) {
        auto opponent = open_opponent(*options, **game);
        if (!opponent) {
            return std::unexpected(std::move(opponent).error());
        }
        const auto status = options->headless
                                ? run_local_headless(**game, **opponent)
                                : run_windowed(*options, **game, nullptr, opponent->get());
        print_summary(**game);
        print_opponent(**opponent);
        if (status) {
            ATLAS_LOG_INFO(kApp, "shutdown");
        }
        return status;
    }

    if (auto status = play_script(**game, options->moves); !status) {
        print_summary(**game);
        return status;
    }
    if (!options->headless) {
        if (auto status = run_windowed(*options, **game, nullptr, nullptr); !status) {
            return status;
        }
    }
    print_summary(**game);
    ATLAS_LOG_INFO(kApp, "shutdown");
    return atlas::ok();
}

}  // namespace

// NOLINTNEXTLINE(misc-const-correctness): the signature of main is fixed by the standard.
int main(int argc, char** argv) {
    return atlas::app::guarded_main("atlas_chess", [&] { return run(argc, argv); });
}
