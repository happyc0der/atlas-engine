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
#include <atlas/app/ppm.hpp>
#include <atlas/assets/registry.hpp>
#include <atlas/chess/board_layout.hpp>
#include <atlas/chess/board_quads.hpp>
#include <atlas/chess/fen.hpp>
#include <atlas/chess/position.hpp>
#include <atlas/chess/rules.hpp>
#include <atlas/chess/world.hpp>
#include <atlas/core/args.hpp>
#include <atlas/core/assert.hpp>
#include <atlas/core/build_info.hpp>
#include <atlas/core/log.hpp>
#include <atlas/core/profile.hpp>
#include <atlas/core/result.hpp>
#include <atlas/math/camera.hpp>
#include <atlas/platform/platform.hpp>
#include <atlas/renderer/quad_batch.hpp>
#include <atlas/renderer/texture_cache.hpp>
#include <atlas/rhi/device.hpp>
#include <atlas/simulation/kernel.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
    std::string_view shader_dir = "assets/cooked/shaders";
    std::string_view fen;
    std::string_view moves;
    std::string_view log_level = "info";
    std::string_view log_file;
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
  --shader-dir PATH      Where the cooked shaders are. Default: assets/cooked/shaders.
  --log-level LEVEL      trace, debug, info, warning, error. Default: info.
  --log-file PATH        Also write the log to PATH.
  --version              Print build identity and exit.
  --help                 Print this message and exit.

In a window: click a piece, then a square it may move to. A pawn reaching the last rank
becomes a queen; --moves can name any promotion. Escape or the close button quits.
)");
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
    options.shader_dir = args.value_or("shader-dir", options.shader_dir);
    options.fen = args.value_or("fen", std::string_view{});
    options.moves = args.value_or("moves", std::string_view{});
    options.log_level = args.value_or("log-level", options.log_level);
    options.log_file = args.value_or("log-file", std::string_view{});
    if (auto status = args.reject_unknown(); !status) {
        return std::unexpected(status.error());
    }
    return options;
}

/// A game: the world, an empty schedule, the queue and the kernel, owned together because the
/// kernel borrows the other three.
struct Game {
    atlas::chess::ChessWorld chess;
    atlas::sim::Schedule schedule;
    atlas::sim::CommandQueue commands;
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

[[nodiscard]] atlas::Result<std::unique_ptr<Game>> make_game(std::string_view fen) {
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
    game->kernel = std::make_unique<atlas::sim::Kernel>(
        game->chess.world, game->schedule, game->commands, atlas::sim::KernelConfig{.seed = kSeed});
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

/// Fit the whole board in the window with a margin.
void frame_board(atlas::math::OrthoCamera& camera, float width, float height) {
    camera.set_viewport(width, height);
    camera.set_centre(atlas::chess::BoardLayout::centre());
    constexpr float kMargin = 1.08F;
    camera.set_zoom(std::min(width, height) / (atlas::chess::BoardLayout::kExtent * kMargin));
}

[[nodiscard]] atlas::Status run_windowed(const Options& options, Game& game) {
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

    const atlas::chess::BoardLayout layout{.flipped = options.flip};
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
        const auto size = window->pixel_size();
        frame_board(camera, static_cast<float>(size.width), static_cast<float>(size.height));

        std::optional<atlas::chess::Move> chosen;
        for (const auto& event : platform->pump()) {
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
            }
            if (auto status = device->end_frame(std::move(*frame)); !status) {
                return std::unexpected(std::move(status).error().context("ending a frame"));
            }
        }

        if (options.max_frames != 0 && frame_index >= options.max_frames) {
            quit = true;
        }
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

    auto game = make_game(options->fen);
    if (!game) {
        return std::unexpected(std::move(game).error());
    }
    if (auto status = play_script(**game, options->moves); !status) {
        print_summary(**game);
        return status;
    }

    if (!options->headless) {
        if (auto status = run_windowed(*options, **game); !status) {
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
