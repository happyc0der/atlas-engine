// SPDX-License-Identifier: GPL-3.0-or-later
// The command: a move through the kernel, every way it is declined, and every way a game ends.
#include <atlas/chess/fen.hpp>
#include <atlas/chess/rules.hpp>
#include <atlas/chess/world.hpp>

#include "chess_harness.hpp"
#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <vector>

using atlas::chess::board_table;
using atlas::chess::Colour;
using atlas::chess::decode_move;
using atlas::chess::encode_move;
using atlas::chess::history_table;
using atlas::chess::legal_moves;
using atlas::chess::Move;
using atlas::chess::MoveList;
using atlas::chess::Outcome;
using atlas::chess::parse_fen;
using atlas::chess::parse_move;
using atlas::chess::Piece;
using atlas::chess::PieceKind;
using atlas::chess::players_table;
using atlas::chess::Reason;
using atlas::chess::result_table;
using atlas::chess::set_position;
using atlas::chess::square;
using atlas::chess::state_table;
using atlas::chess::validate_world;
using atlas::chess::testing::ChessHarness;
using atlas::sim::SourceId;

namespace {

[[nodiscard]] Move mv(std::string_view text) {
    auto parsed = parse_move(text);
    REQUIRE(parsed.has_value());
    return *parsed;
}

/// Play a sequence of moves through the kernel, one per tick, all from `Local`, and return
/// the last tick's report.
[[nodiscard]] atlas::sim::TickReport play(ChessHarness& h, atlas::sim::Kernel& kernel,
                                          const std::vector<std::string_view>& moves) {
    atlas::sim::TickReport last;
    for (const auto text : moves) {
        REQUIRE(h.submit(kernel.current_tick(), mv(text)).has_value());
        auto report = kernel.step();
        REQUIRE(report.has_value());
        REQUIRE(validate_world(h.world(), h.ids()).has_value());
        last = *report;
    }
    return last;
}

}  // namespace

TEST_CASE("a move payload round-trips and a bad one is refused", "[chess][rules]") {
    const Move move{.from = square(4, 6), .to = square(4, 7), .promotion = PieceKind::Queen};
    const auto bytes = encode_move(move);
    auto decoded = decode_move(bytes);
    REQUIRE(decoded.has_value());
    CHECK(*decoded == move);

    const auto refuse = [](std::uint8_t from, std::uint8_t to, std::uint8_t promotion) {
        const std::array<std::byte, 3> payload{static_cast<std::byte>(from),
                                               static_cast<std::byte>(to),
                                               static_cast<std::byte>(promotion)};
        return !decode_move(payload).has_value();
    };
    CHECK(refuse(64, 0, 0));
    CHECK(refuse(0, 64, 0));
    CHECK(refuse(12, 12, 0));
    CHECK(refuse(12, 28, 1));  // a pawn is not a promotion piece
    CHECK(refuse(12, 28, 6));  // nor a king
    CHECK(refuse(12, 28, 7));
    CHECK_FALSE(refuse(12, 28, 0));
    CHECK_FALSE(refuse(12, 28, 5));
    const std::array<std::byte, 2> short_payload{};
    CHECK_FALSE(decode_move(short_payload).has_value());
}

TEST_CASE("a legal move reaches the board on its tick", "[chess][rules]") {
    ChessHarness h;
    auto kernel = h.kernel(1);
    const auto before = h.world().hash();
    const auto report = play(h, kernel, {"e2e4"});
    CHECK(report.commands_applied == 1);
    CHECK(report.commands_declined == 0);
    CHECK(report.state_hash != before);
    CHECK(board_table(h.world(), h.ids()).at(square(4, 3)) == Piece::WhitePawn);
    CHECK(state_table(h.world(), h.ids()).side_to_move == Colour::Black);
    CHECK(history_table(h.world(), h.ids()).keys.size() == 1);  // a pawn move resets it
    CHECK(result_table(h.world(), h.ids()).outcome == Outcome::Ongoing);
    MoveList legal;
    legal_moves(h.world(), h.ids(), legal);
    CHECK(legal.size() == 20);
}

TEST_CASE("an illegal move is declined and the world is what it would have been",
          "[chess][rules]") {
    ChessHarness with;
    ChessHarness twin;
    auto kernel = with.kernel(1);
    auto twin_kernel = twin.kernel(1);
    REQUIRE(with.submit(0, mv("e2e5")).has_value());  // three squares
    const auto report = kernel.step();
    const auto twin_report = twin_kernel.step();
    REQUIRE(report.has_value());
    REQUIRE(twin_report.has_value());
    CHECK(report->commands_declined == 1);
    CHECK(report->commands_applied == 0);
    CHECK(report->commands_rejected() == 0);
    CHECK(report->state_hash == twin_report->state_hash);
    CHECK(state_table(with.world(), with.ids()).side_to_move == Colour::White);
}

TEST_CASE("moving the other side's piece, or from an empty square, is declined", "[chess][rules]") {
    ChessHarness h;
    auto kernel = h.kernel(1);
    REQUIRE(h.submit(0, mv("e7e5")).has_value());  // black's pawn, white to move
    REQUIRE(h.submit(0, mv("e4e5")).has_value());  // nothing on e4
    const auto report = kernel.step();
    REQUIRE(report.has_value());
    CHECK(report->commands_declined == 2);
    CHECK(report->commands_applied == 0);
}

TEST_CASE("each colour moves only from the source that holds it", "[chess][rules]") {
    ChessHarness h;
    players_table(h.world(), h.ids()).white = SourceId{0};
    players_table(h.world(), h.ids()).black = SourceId{1};
    auto kernel = h.kernel(1);

    REQUIRE(h.submit(0, mv("e2e4"), SourceId{1}).has_value());  // black's source, white's move
    auto report = kernel.step();
    REQUIRE(report.has_value());
    CHECK(report->commands_declined == 1);
    CHECK(report->commands_applied == 0);

    REQUIRE(h.submit(1, mv("e2e4"), SourceId{0}).has_value());
    report = kernel.step();
    REQUIRE(report.has_value());
    CHECK(report->commands_applied == 1);

    REQUIRE(h.submit(2, mv("e7e5"), SourceId{0}).has_value());  // white's source, black's move
    REQUIRE(h.submit(2, mv("e7e5"), SourceId{1}).has_value());
    report = kernel.step();
    REQUIRE(report.has_value());
    CHECK(report->commands_declined == 1);
    CHECK(report->commands_applied == 1);
    CHECK(state_table(h.world(), h.ids()).side_to_move == Colour::White);
}

TEST_CASE("Scholar's mate ends the game, and nothing moves afterwards", "[chess][rules]") {
    ChessHarness h;
    auto kernel = h.kernel(1);
    const auto report = play(h, kernel, {"e2e4", "e7e5", "f1c4", "b8c6", "d1h5", "g8f6", "h5f7"});
    CHECK(report.commands_applied == 1);
    const auto& result = result_table(h.world(), h.ids());
    CHECK(result.outcome == Outcome::WhiteWins);
    CHECK(result.reason == Reason::Checkmate);
    MoveList legal;
    legal_moves(h.world(), h.ids(), legal);
    CHECK(legal.empty());

    // The king cannot take the queen — it is defended — and nothing else is allowed either:
    // the game is over, so the decline comes before legality is even asked.
    REQUIRE(h.submit(kernel.current_tick(), mv("e8f7")).has_value());
    const auto after = kernel.step();
    REQUIRE(after.has_value());
    CHECK(after->commands_declined == 1);
    CHECK(result.outcome == Outcome::WhiteWins);
}

TEST_CASE("a move with no reply and no check is stalemate", "[chess][rules]") {
    ChessHarness h;
    set_position(h.world(), h.ids(), parse_fen("k7/8/2K5/1Q6/8/8/8/8 w - - 0 1").value());
    auto kernel = h.kernel(1);
    (void)play(h, kernel, {"b5b6"});
    CHECK(result_table(h.world(), h.ids()).outcome == Outcome::Draw);
    CHECK(result_table(h.world(), h.ids()).reason == Reason::Stalemate);
}

TEST_CASE("the hundredth quiet ply draws by the fifty-move rule", "[chess][rules]") {
    ChessHarness h;
    set_position(h.world(), h.ids(), parse_fen("k7/8/8/8/8/8/8/K6R w - - 99 80").value());
    auto kernel = h.kernel(1);
    (void)play(h, kernel, {"h1h2"});
    CHECK(state_table(h.world(), h.ids()).halfmove_clock == 100);
    CHECK(result_table(h.world(), h.ids()).outcome == Outcome::Draw);
    CHECK(result_table(h.world(), h.ids()).reason == Reason::FiftyMoves);

    // The king has moves, and none of them is allowed: the game is over. This is where the
    // game-over check is actually tested — after a checkmate there is no legal move to try,
    // so a mutation removing the check survived the mate case and not this one.
    REQUIRE(h.submit(kernel.current_tick(), mv("a8b8")).has_value());
    const auto after = kernel.step();
    REQUIRE(after.has_value());
    CHECK(after->commands_declined == 1);
    CHECK(after->commands_applied == 0);
    CHECK(state_table(h.world(), h.ids()).side_to_move == Colour::Black);
}

TEST_CASE("a mate on the hundredth ply is a mate, not a draw", "[chess][rules]") {
    // Rook to h8 mates the king in the corner, and it is also the hundredth quiet ply.
    ChessHarness h;
    set_position(h.world(), h.ids(), parse_fen("k7/8/1K6/8/8/8/8/7R w - - 99 80").value());
    auto kernel = h.kernel(1);
    (void)play(h, kernel, {"h1h8"});
    CHECK(result_table(h.world(), h.ids()).outcome == Outcome::WhiteWins);
    CHECK(result_table(h.world(), h.ids()).reason == Reason::Checkmate);
}

TEST_CASE("the third occurrence of a position draws by repetition", "[chess][rules]") {
    // Knights out and back, twice: the starting position is seen at ply 0, 4 and 8.
    ChessHarness h;
    auto kernel = h.kernel(1);
    (void)play(h, kernel, {"g1f3", "g8f6", "f3g1", "f6g8", "g1f3", "g8f6", "f3g1"});
    CHECK(result_table(h.world(), h.ids()).outcome == Outcome::Ongoing);
    (void)play(h, kernel, {"f6g8"});
    CHECK(result_table(h.world(), h.ids()).outcome == Outcome::Draw);
    CHECK(result_table(h.world(), h.ids()).reason == Reason::Threefold);
    CHECK(history_table(h.world(), h.ids()).keys.size() == 9);
}

TEST_CASE("a repetition with different castling rights is not a repetition", "[chess][rules]") {
    // The same squares after a rook shuffle, but the rights are gone: the key differs, and
    // the game goes on.
    ChessHarness h;
    set_position(h.world(), h.ids(), parse_fen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1").value());
    auto kernel = h.kernel(1);
    (void)play(h, kernel, {"a1a2", "a8a7", "a2a1", "a7a8", "a1a2", "a8a7", "a2a1", "a7a8"});
    CHECK(result_table(h.world(), h.ids()).outcome == Outcome::Ongoing);
}

TEST_CASE("a capture that leaves nobody able to mate draws the game", "[chess][rules]") {
    ChessHarness h;
    set_position(h.world(), h.ids(), parse_fen("k7/8/8/8/8/8/6B1/K6r w - - 0 1").value());
    auto kernel = h.kernel(1);
    (void)play(h, kernel, {"g2h1"});
    CHECK(result_table(h.world(), h.ids()).outcome == Outcome::Draw);
    CHECK(result_table(h.world(), h.ids()).reason == Reason::InsufficientMaterial);
}

TEST_CASE("two worlds fed the same moves reach the same hash at every tick",
          "[chess][rules][determinism]") {
    ChessHarness a;
    ChessHarness b;
    auto ka = a.kernel(7);
    auto kb = b.kernel(7);
    const std::vector<std::string_view> moves{"e2e4", "c7c5", "g1f3", "d7d6", "d2d4",
                                              "c5d4", "f3d4", "g8f6", "b1c3", "a7a6"};
    for (const auto text : moves) {
        REQUIRE(a.submit(ka.current_tick(), mv(text)).has_value());
        REQUIRE(b.submit(kb.current_tick(), mv(text)).has_value());
        const auto ra = ka.step();
        const auto rb = kb.step();
        REQUIRE(ra.has_value());
        REQUIRE(rb.has_value());
        INFO(text);
        CHECK(ra->state_hash == rb->state_hash);
        CHECK(ra->commands_applied == 1);
    }
}
