// SPDX-License-Identifier: GPL-3.0-or-later
// The five tables on their own: what they hash, what they save, and what they refuse.
#include <atlas/chess/tables.hpp>
#include <atlas/simulation/save_stream.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <vector>

using atlas::chess::BoardTable;
using atlas::chess::Colour;
using atlas::chess::HistoryTable;
using atlas::chess::Outcome;
using atlas::chess::Piece;
using atlas::chess::PlayersTable;
using atlas::chess::Reason;
using atlas::chess::ResultTable;
using atlas::chess::square;
using atlas::chess::StateTable;
using atlas::sim::SaveReader;
using atlas::sim::SaveWriter;
using atlas::sim::Table;

namespace {

[[nodiscard]] std::uint64_t hash_of(const Table& table) {
    atlas::Hasher hasher;
    table.hash_into(hasher);
    return hasher.value();
}

[[nodiscard]] std::vector<std::byte> bytes_of(const Table& table) {
    SaveWriter writer;
    table.write_to(writer);
    return {writer.bytes().begin(), writer.bytes().end()};
}

/// Write a table, read it into a fresh one, and return whether the fresh one hashes alike.
template <typename T> [[nodiscard]] bool round_trips(const T& table) {
    const auto bytes = bytes_of(table);
    T loaded;
    SaveReader reader(bytes);
    if (!loaded.read_from(reader)) {
        return false;
    }
    if (!reader.expect_end()) {
        return false;
    }
    return hash_of(loaded) == hash_of(table);
}

}  // namespace

TEST_CASE("the piece encoding round-trips and rejects the gaps", "[chess][tables]") {
    using atlas::chess::colour_of;
    using atlas::chess::is_piece_code;
    using atlas::chess::kind_of;
    using atlas::chess::make_piece;
    using atlas::chess::PieceKind;

    CHECK(make_piece(Colour::Black, PieceKind::Queen) == Piece::BlackQueen);
    CHECK(kind_of(Piece::BlackQueen) == PieceKind::Queen);
    CHECK(colour_of(Piece::BlackQueen) == Colour::Black);
    CHECK(colour_of(Piece::WhitePawn) == Colour::White);
    CHECK(make_piece(Colour::Black, PieceKind::None) == Piece::None);

    // Every byte: the fourteen codes are pieces or empty, and nothing else is.
    int accepted = 0;
    for (int code = 0; code < 256; ++code) {
        if (is_piece_code(static_cast<std::uint8_t>(code))) {
            ++accepted;
        }
    }
    CHECK(accepted == 13);
    CHECK_FALSE(is_piece_code(7));
    CHECK_FALSE(is_piece_code(8));
    CHECK_FALSE(is_piece_code(15));
    CHECK_FALSE(is_piece_code(16));
}

TEST_CASE("a board hashes its squares and a changed square changes the hash", "[chess][tables]") {
    BoardTable board;
    const auto empty = hash_of(board);
    board.put(square(4, 0), Piece::WhiteKing);
    const auto with_king = hash_of(board);
    CHECK(with_king != empty);
    board.put(square(4, 0), Piece::BlackKing);
    CHECK(hash_of(board) != with_king);
    CHECK(round_trips(board));
    CHECK(bytes_of(board).size() == 64);
}

TEST_CASE("a board refuses a byte that is not a piece", "[chess][tables]") {
    BoardTable board;
    auto bytes = bytes_of(board);
    bytes[10] = std::byte{7};
    BoardTable loaded;
    loaded.put(square(0, 0), Piece::WhiteRook);
    SaveReader reader(bytes);
    CHECK_FALSE(loaded.read_from(reader).has_value());
    // A failed load leaves the table as it was.
    CHECK(loaded.at(square(0, 0)) == Piece::WhiteRook);
}

TEST_CASE("the state table round-trips and refuses what a game cannot reach", "[chess][tables]") {
    StateTable state;
    state.side_to_move = Colour::Black;
    state.castling = atlas::chess::kWhiteQueenSide | atlas::chess::kBlackKingSide;
    state.en_passant_file = 3;
    state.halfmove_clock = 17;
    state.fullmove_number = 40;
    CHECK(round_trips(state));

    const auto refuse_with = [&](auto mutate) {
        StateTable bad;
        mutate(bad);
        const auto bytes = bytes_of(bad);
        StateTable loaded;
        SaveReader reader(bytes);
        return !loaded.read_from(reader).has_value();
    };
    // The writer will happily write these; the reader must not accept them.
    CHECK(refuse_with([](StateTable& s) { s.side_to_move = static_cast<Colour>(2); }));
    CHECK(refuse_with([](StateTable& s) { s.castling = 0x10; }));
    CHECK(refuse_with([](StateTable& s) { s.en_passant_file = 9; }));
    CHECK(refuse_with([](StateTable& s) { s.halfmove_clock = 101; }));
    CHECK(refuse_with([](StateTable& s) { s.fullmove_number = 0; }));
    CHECK_FALSE(refuse_with([](StateTable& s) { s.halfmove_clock = 100; }));
    CHECK_FALSE(refuse_with([](StateTable& s) { s.en_passant_file = 8; }));
}

TEST_CASE("the history round-trips, hashes its length, and is bounded", "[chess][tables]") {
    HistoryTable history;
    history.keys = {1, 2, 3};
    CHECK(round_trips(history));
    HistoryTable shorter;
    shorter.keys = {1, 2};
    CHECK(hash_of(history) != hash_of(shorter));

    // A count past the bound is refused before anything is allocated: the reader checks the
    // count against the bytes that remain, so a claim of a million keys with three keys'
    // bytes behind it fails on the count.
    SaveWriter writer;
    writer.write_u64(1'000'000);
    writer.write_u64(1);
    HistoryTable loaded;
    SaveReader reader(writer.bytes());
    CHECK_FALSE(loaded.read_from(reader).has_value());
    CHECK(loaded.keys.empty());
}

TEST_CASE("the result round-trips and refuses an outcome that disagrees with its reason",
          "[chess][tables]") {
    ResultTable result;
    result.outcome = Outcome::Draw;
    result.reason = Reason::Threefold;
    CHECK(round_trips(result));

    const auto refuse = [](std::uint8_t outcome, std::uint8_t reason) {
        SaveWriter writer;
        writer.write_u8(outcome);
        writer.write_u8(reason);
        ResultTable loaded;
        SaveReader reader(writer.bytes());
        return !loaded.read_from(reader).has_value();
    };
    CHECK(refuse(4, 1));  // no such outcome
    CHECK(refuse(1, 6));  // no such reason
    CHECK(refuse(1, 0));  // a winner with no reason
    CHECK(refuse(0, 1));  // a reason with no result
    CHECK_FALSE(refuse(0, 0));
    CHECK_FALSE(refuse(2, 1));
}

TEST_CASE("the players table round-trips and refuses a mod as a player", "[chess][tables]") {
    PlayersTable players;
    players.white = atlas::sim::SourceId{0};
    players.black = atlas::sim::SourceId{1};
    CHECK(round_trips(players));
    CHECK(players.holder(Colour::Black) == atlas::sim::SourceId{1});

    SaveWriter writer;
    writer.write_u32(0);
    writer.write_u32(0x8000'0001U);  // a mod identifier, bit 31 set
    PlayersTable loaded;
    SaveReader reader(writer.bytes());
    CHECK_FALSE(loaded.read_from(reader).has_value());
}

TEST_CASE("clear returns every table to its constructed state", "[chess][tables]") {
    BoardTable board;
    board.put(3, Piece::WhiteQueen);
    board.clear();
    CHECK(hash_of(board) == hash_of(BoardTable{}));

    StateTable state;
    state.side_to_move = Colour::Black;
    state.halfmove_clock = 9;
    state.clear();
    CHECK(hash_of(state) == hash_of(StateTable{}));

    HistoryTable history;
    history.keys = {5};
    history.clear();
    CHECK(hash_of(history) == hash_of(HistoryTable{}));

    ResultTable result;
    result.outcome = Outcome::WhiteWins;
    result.reason = Reason::Checkmate;
    result.clear();
    CHECK(hash_of(result) == hash_of(ResultTable{}));

    PlayersTable players;
    players.black = atlas::sim::SourceId{3};
    players.clear();
    CHECK(hash_of(players) == hash_of(PlayersTable{}));
}
