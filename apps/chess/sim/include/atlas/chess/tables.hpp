// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// The five tables that are a game of chess (ADR-0018). Every field is authoritative, hashed,
/// saved and integer; nothing here is presentation and nothing here knows how a piece moves.
/// A position is these tables and nothing else, which is what makes a famous game's final
/// position a hash that can be written down.
///
/// `sim::Table` asks for four things — hash, write, read, clear — and leaves the layout to the
/// application. That is the whole of what the rules library takes from the engine here: the
/// kernel never looks inside.
///
/// Thread affinity: written only by the command that moves, on the main thread, inside a tick.

#include <atlas/chess/piece.hpp>
#include <atlas/core/result.hpp>
#include <atlas/simulation/command.hpp>
#include <atlas/simulation/table.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace atlas::chess {

/// Sixty-four squares, a piece code each. Row count is fixed, and the file carries no count.
class BoardTable final : public sim::Table {
  public:
    std::array<std::uint8_t, kSquareCount> squares{};

    [[nodiscard]] Piece at(Square sq) const noexcept { return static_cast<Piece>(squares[sq]); }

    void put(Square sq, Piece piece) noexcept { squares[sq] = static_cast<std::uint8_t>(piece); }

    [[nodiscard]] std::size_t row_count() const noexcept override { return kSquareCount; }

    void hash_into(Hasher& hasher) const override;
    void write_to(sim::SaveWriter& writer) const override;
    [[nodiscard]] Status read_from(sim::SaveReader& reader) override;
    void clear() override;
};

/// Whose move it is, and everything about a position that is not on the board.
class StateTable final : public sim::Table {
  public:
    /// The halfmove clock at which the fifty-move rule draws the game automatically. A saved
    /// clock above this is refused: in play it cannot happen, and from a file it is a lie.
    static constexpr std::uint16_t kMaxHalfmoveClock = 100;
    /// A game that reached this many moves has been going for longer than any recorded one.
    static constexpr std::uint16_t kMaxFullmoveNumber = 10'000;

    Colour side_to_move = Colour::White;
    std::uint8_t castling = kAllCastling;
    std::uint8_t en_passant_file = kNoEnPassant;
    std::uint16_t halfmove_clock = 0;
    std::uint16_t fullmove_number = 1;

    [[nodiscard]] std::size_t row_count() const noexcept override { return 1; }

    void hash_into(Hasher& hasher) const override;
    void write_to(sim::SaveWriter& writer) const override;
    [[nodiscard]] Status read_from(sim::SaveReader& reader) override;
    void clear() override;
};

/// The position keys since the last irreversible move, oldest first, for repetition.
///
/// Bounded by the fifty-move rule rather than by hope: a reversible run is at most a hundred
/// plies before the game draws itself, so a file claiming more than `kMaxKeys` is not a game.
class HistoryTable final : public sim::Table {
  public:
    static constexpr std::size_t kMaxKeys = 256;

    std::vector<std::uint64_t> keys;

    [[nodiscard]] std::size_t row_count() const noexcept override { return keys.size(); }

    void hash_into(Hasher& hasher) const override;
    void write_to(sim::SaveWriter& writer) const override;
    [[nodiscard]] Status read_from(sim::SaveReader& reader) override;
    void clear() override;
};

enum class Outcome : std::uint8_t { Ongoing = 0, WhiteWins = 1, BlackWins = 2, Draw = 3 };
enum class Reason : std::uint8_t {
    None = 0,
    Checkmate = 1,
    Stalemate = 2,
    FiftyMoves = 3,
    Threefold = 4,
    InsufficientMaterial = 5,
};

/// How the game ended, if it has. A move after this is set is declined.
class ResultTable final : public sim::Table {
  public:
    Outcome outcome = Outcome::Ongoing;
    Reason reason = Reason::None;

    [[nodiscard]] std::size_t row_count() const noexcept override { return 1; }

    void hash_into(Hasher& hasher) const override;
    void write_to(sim::SaveWriter& writer) const override;
    [[nodiscard]] Status read_from(sim::SaveReader& reader) override;
    void clear() override;
};

/// Which command source holds which colour.
///
/// Hashed, so "not your turn" is refused inside the simulation where every peer agrees on it,
/// rather than in an application a modified client could skip. Both colours from one source is
/// hot-seat, and is the default.
class PlayersTable final : public sim::Table {
  public:
    sim::SourceId white = sim::SourceId::Local;
    sim::SourceId black = sim::SourceId::Local;

    [[nodiscard]] sim::SourceId holder(Colour colour) const noexcept {
        return colour == Colour::White ? white : black;
    }

    [[nodiscard]] std::size_t row_count() const noexcept override { return 1; }

    void hash_into(Hasher& hasher) const override;
    void write_to(sim::SaveWriter& writer) const override;
    [[nodiscard]] Status read_from(sim::SaveReader& reader) override;
    void clear() override;
};

/// The identifiers of the five tables, by the names they are registered under.
struct TableIds {
    sim::TableId board = sim::TableId::Invalid;
    sim::TableId state = sim::TableId::Invalid;
    sim::TableId history = sim::TableId::Invalid;
    sim::TableId result = sim::TableId::Invalid;
    sim::TableId players = sim::TableId::Invalid;
};

}  // namespace atlas::chess
