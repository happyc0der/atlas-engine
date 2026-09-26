// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/chess/tables.hpp>

#include <format>
#include <span>
#include <utility>

namespace atlas::chess {

// ------------------------------------------------------------------------------ BoardTable

void BoardTable::hash_into(Hasher& hasher) const {
    hasher.add(std::as_bytes(std::span<const std::uint8_t>{squares}));
}

void BoardTable::write_to(sim::SaveWriter& writer) const {
    for (const std::uint8_t code : squares) {
        writer.write_u8(code);
    }
}

Status BoardTable::read_from(sim::SaveReader& reader) {
    std::array<std::uint8_t, kSquareCount> loaded{};
    for (Square sq = 0; sq < kSquareCount; ++sq) {
        auto code = reader.read_u8();
        if (!code) {
            return std::unexpected(std::move(code).error().context("chess.board"));
        }
        if (!is_piece_code(*code)) {
            return std::unexpected(
                Error(ErrorCode::MalformedData,
                      std::format("chess.board square {} holds code {}, which is not a piece", sq,
                                  *code)));
        }
        loaded[sq] = *code;
    }
    squares = loaded;
    return ok();
}

void BoardTable::clear() {
    squares.fill(0);
}

// ------------------------------------------------------------------------------ StateTable

void StateTable::hash_into(Hasher& hasher) const {
    hasher.add(side_to_move);
    hasher.add(castling);
    hasher.add(en_passant_file);
    hasher.add(halfmove_clock);
    hasher.add(fullmove_number);
}

void StateTable::write_to(sim::SaveWriter& writer) const {
    writer.write_u8(static_cast<std::uint8_t>(side_to_move));
    writer.write_u8(castling);
    writer.write_u8(en_passant_file);
    writer.write_u16(halfmove_clock);
    writer.write_u16(fullmove_number);
}

Status StateTable::read_from(sim::SaveReader& reader) {
    auto side = reader.read_u8();
    if (!side) {
        return std::unexpected(std::move(side).error().context("chess.state.side_to_move"));
    }
    auto rights = reader.read_u8();
    if (!rights) {
        return std::unexpected(std::move(rights).error().context("chess.state.castling"));
    }
    auto ep = reader.read_u8();
    if (!ep) {
        return std::unexpected(std::move(ep).error().context("chess.state.en_passant_file"));
    }
    auto halfmove = reader.read_u16();
    if (!halfmove) {
        return std::unexpected(std::move(halfmove).error().context("chess.state.halfmove_clock"));
    }
    auto fullmove = reader.read_u16();
    if (!fullmove) {
        return std::unexpected(std::move(fullmove).error().context("chess.state.fullmove_number"));
    }

    if (*side > 1) {
        return std::unexpected(
            Error(ErrorCode::MalformedData, std::format("chess.state side to move is {}", *side)));
    }
    if ((*rights & static_cast<std::uint8_t>(~kAllCastling)) != 0) {
        return std::unexpected(Error(ErrorCode::MalformedData,
                                     std::format("chess.state castling bits {:#x}", *rights)));
    }
    if (*ep > kNoEnPassant) {
        return std::unexpected(
            Error(ErrorCode::MalformedData, std::format("chess.state en passant file {}", *ep)));
    }
    if (*halfmove > kMaxHalfmoveClock) {
        return std::unexpected(Error(
            ErrorCode::MalformedData,
            std::format("chess.state halfmove clock {} is past the fifty-move rule", *halfmove)));
    }
    if (*fullmove == 0 || *fullmove > kMaxFullmoveNumber) {
        return std::unexpected(Error(ErrorCode::MalformedData,
                                     std::format("chess.state fullmove number {}", *fullmove)));
    }

    side_to_move = static_cast<Colour>(*side);
    castling = *rights;
    en_passant_file = *ep;
    halfmove_clock = *halfmove;
    fullmove_number = *fullmove;
    return ok();
}

void StateTable::clear() {
    side_to_move = Colour::White;
    castling = kAllCastling;
    en_passant_file = kNoEnPassant;
    halfmove_clock = 0;
    fullmove_number = 1;
}

// ---------------------------------------------------------------------------- HistoryTable

void HistoryTable::hash_into(Hasher& hasher) const {
    hasher.add(static_cast<std::uint64_t>(keys.size()));
    for (const std::uint64_t key : keys) {
        hasher.add(key);
    }
}

void HistoryTable::write_to(sim::SaveWriter& writer) const {
    writer.write_u64(static_cast<std::uint64_t>(keys.size()));
    for (const std::uint64_t key : keys) {
        writer.write_u64(key);
    }
}

Status HistoryTable::read_from(sim::SaveReader& reader) {
    auto count = reader.read_count(kMaxKeys, sizeof(std::uint64_t));
    if (!count) {
        return std::unexpected(std::move(count).error().context("chess.history"));
    }
    std::vector<std::uint64_t> loaded;
    loaded.reserve(*count);
    for (std::size_t i = 0; i < *count; ++i) {
        auto key = reader.read_u64();
        if (!key) {
            return std::unexpected(std::move(key).error().context("chess.history"));
        }
        loaded.push_back(*key);
    }
    keys = std::move(loaded);
    return ok();
}

void HistoryTable::clear() {
    keys.clear();
}

// ----------------------------------------------------------------------------- ResultTable

void ResultTable::hash_into(Hasher& hasher) const {
    hasher.add(outcome);
    hasher.add(reason);
}

void ResultTable::write_to(sim::SaveWriter& writer) const {
    writer.write_u8(static_cast<std::uint8_t>(outcome));
    writer.write_u8(static_cast<std::uint8_t>(reason));
}

Status ResultTable::read_from(sim::SaveReader& reader) {
    auto outcome_code = reader.read_u8();
    if (!outcome_code) {
        return std::unexpected(std::move(outcome_code).error().context("chess.result"));
    }
    auto reason_code = reader.read_u8();
    if (!reason_code) {
        return std::unexpected(std::move(reason_code).error().context("chess.result"));
    }
    if (*outcome_code > static_cast<std::uint8_t>(Outcome::Draw)) {
        return std::unexpected(
            Error(ErrorCode::MalformedData, std::format("chess.result outcome {}", *outcome_code)));
    }
    if (*reason_code > static_cast<std::uint8_t>(Reason::InsufficientMaterial)) {
        return std::unexpected(
            Error(ErrorCode::MalformedData, std::format("chess.result reason {}", *reason_code)));
    }
    // A reason without an outcome, or an outcome without one, is a file that disagrees with
    // itself about whether the game is over.
    const bool over = *outcome_code != static_cast<std::uint8_t>(Outcome::Ongoing);
    const bool explained = *reason_code != static_cast<std::uint8_t>(Reason::None);
    if (over != explained) {
        return std::unexpected(
            Error(ErrorCode::MalformedData, std::format("chess.result outcome {} with reason {}",
                                                        *outcome_code, *reason_code)));
    }
    outcome = static_cast<Outcome>(*outcome_code);
    reason = static_cast<Reason>(*reason_code);
    return ok();
}

void ResultTable::clear() {
    outcome = Outcome::Ongoing;
    reason = Reason::None;
}

// ---------------------------------------------------------------------------- PlayersTable

void PlayersTable::hash_into(Hasher& hasher) const {
    hasher.add(white);
    hasher.add(black);
}

void PlayersTable::write_to(sim::SaveWriter& writer) const {
    writer.write_u32(static_cast<std::uint32_t>(white));
    writer.write_u32(static_cast<std::uint32_t>(black));
}

Status PlayersTable::read_from(sim::SaveReader& reader) {
    auto white_source = reader.read_u32();
    if (!white_source) {
        return std::unexpected(std::move(white_source).error().context("chess.players.white"));
    }
    auto black_source = reader.read_u32();
    if (!black_source) {
        return std::unexpected(std::move(black_source).error().context("chess.players.black"));
    }
    // **A mod may hold a colour** (ADR-0023 D6). Until M26 a mod's identifier was refused here,
    // because it is never sent on the wire (ADR-0015). It is nonetheless the same on every
    // machine — a pure function of the mod's index — and a saved file is not the wire. Any
    // identifier is accepted: one that nobody is running means only that nobody can move for
    // that side, which is a stalled game rather than a corrupt one, and the application checks
    // that a mod named here is the mod it attached before it plays on.
    white = static_cast<sim::SourceId>(*white_source);
    black = static_cast<sim::SourceId>(*black_source);
    return ok();
}

void PlayersTable::clear() {
    white = sim::SourceId::Local;
    black = sim::SourceId::Local;
}

}  // namespace atlas::chess
