// SPDX-License-Identifier: GPL-3.0-or-later
#include <atlas/chess/fen.hpp>

#include <charconv>
#include <format>
#include <vector>

namespace atlas::chess {
namespace {

/// The board's width as a signed number, because file arithmetic here is signed.
constexpr int kFiles = kFileCount;

[[nodiscard]] Piece piece_from_letter(char letter) noexcept {
    switch (letter) {
    case 'P': return Piece::WhitePawn;
    case 'N': return Piece::WhiteKnight;
    case 'B': return Piece::WhiteBishop;
    case 'R': return Piece::WhiteRook;
    case 'Q': return Piece::WhiteQueen;
    case 'K': return Piece::WhiteKing;
    case 'p': return Piece::BlackPawn;
    case 'n': return Piece::BlackKnight;
    case 'b': return Piece::BlackBishop;
    case 'r': return Piece::BlackRook;
    case 'q': return Piece::BlackQueen;
    case 'k': return Piece::BlackKing;
    default: return Piece::None;
    }
}

[[nodiscard]] char letter_of(Piece piece) noexcept {
    constexpr std::string_view kWhite = " PNBRQK";
    constexpr std::string_view kBlack = " pnbrqk";
    const auto kind = static_cast<std::size_t>(kind_of(piece));
    return colour_of(piece) == Colour::White ? kWhite[kind] : kBlack[kind];
}

[[nodiscard]] Result<Square> parse_square(std::string_view text) {
    if (text.size() != 2 || text[0] < 'a' || text[0] > 'h' || text[1] < '1' || text[1] > '8') {
        return std::unexpected(
            Error(ErrorCode::MalformedData, std::format("'{}' is not a square", text)));
    }
    return square(static_cast<std::uint8_t>(text[0] - 'a'),
                  static_cast<std::uint8_t>(text[1] - '1'));
}

[[nodiscard]] std::vector<std::string_view> split_fields(std::string_view text) {
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (start < text.size()) {
        while (start < text.size() && text[start] == ' ') {
            ++start;
        }
        if (start >= text.size()) {
            break;
        }
        std::size_t end = start;
        while (end < text.size() && text[end] != ' ') {
            ++end;
        }
        fields.push_back(text.substr(start, end - start));
        start = end;
    }
    return fields;
}

template <typename T>
[[nodiscard]] Result<T> parse_number(std::string_view text, std::string_view what, T min, T max) {
    T value{};
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || value < min || value > max) {
        return std::unexpected(Error(
            ErrorCode::MalformedData,
            std::format("FEN {} '{}' is not a number between {} and {}", what, text, min, max)));
    }
    return value;
}

}  // namespace

Result<Position> parse_fen(std::string_view fen) {
    if (fen.size() > kMaxFenLength) {
        return std::unexpected(
            Error(ErrorCode::MalformedData,
                  std::format("FEN of {} characters is longer than any position", fen.size())));
    }
    const auto fields = split_fields(fen);
    if (fields.size() < 4 || fields.size() > 6) {
        return std::unexpected(
            Error(ErrorCode::MalformedData,
                  std::format("FEN has {} fields; four to six are expected", fields.size())));
    }

    Position position;
    position.castling = 0;

    // Placement: eight ranks from the eighth down, each summing to eight squares.
    int rank = 7;
    int file = 0;
    for (const char c : fields[0]) {
        if (c == '/') {
            if (file != kFiles) {
                return std::unexpected(
                    Error(ErrorCode::MalformedData,
                          std::format("FEN rank {} has {} squares", rank + 1, file)));
            }
            --rank;
            file = 0;
            if (rank < 0) {
                return std::unexpected(
                    Error(ErrorCode::MalformedData, "FEN has more than eight ranks"));
            }
            continue;
        }
        if (c >= '1' && c <= '8') {
            file += c - '0';
        } else {
            const Piece piece = piece_from_letter(c);
            if (piece == Piece::None) {
                return std::unexpected(
                    Error(ErrorCode::MalformedData, std::format("'{}' is not a piece letter", c)));
            }
            if (file >= kFiles) {
                return std::unexpected(
                    Error(ErrorCode::MalformedData,
                          std::format("FEN rank {} has more than eight squares", rank + 1)));
            }
            position.put(square(static_cast<std::uint8_t>(file), static_cast<std::uint8_t>(rank)),
                         piece);
            ++file;
        }
        if (file > kFiles) {
            return std::unexpected(
                Error(ErrorCode::MalformedData,
                      std::format("FEN rank {} has more than eight squares", rank + 1)));
        }
    }
    if (rank != 0 || file != kFiles) {
        return std::unexpected(
            Error(ErrorCode::MalformedData, "FEN placement does not describe eight full ranks"));
    }

    if (fields[1] == "w") {
        position.side_to_move = Colour::White;
    } else if (fields[1] == "b") {
        position.side_to_move = Colour::Black;
    } else {
        return std::unexpected(
            Error(ErrorCode::MalformedData, std::format("FEN side to move '{}'", fields[1])));
    }

    if (fields[2] != "-") {
        for (const char c : fields[2]) {
            std::uint8_t right = 0;
            switch (c) {
            case 'K': right = kWhiteKingSide; break;
            case 'Q': right = kWhiteQueenSide; break;
            case 'k': right = kBlackKingSide; break;
            case 'q': right = kBlackQueenSide; break;
            default:
                return std::unexpected(
                    Error(ErrorCode::MalformedData, std::format("FEN castling '{}'", fields[2])));
            }
            if ((position.castling & right) != 0) {
                return std::unexpected(
                    Error(ErrorCode::MalformedData,
                          std::format("FEN castling '{}' names a right twice", fields[2])));
            }
            position.castling |= right;
        }
    }

    if (fields[3] != "-") {
        auto ep = parse_square(fields[3]);
        if (!ep) {
            return std::unexpected(std::move(ep).error().context("FEN en passant"));
        }
        const std::uint8_t expected_rank = position.side_to_move == Colour::White ? 5 : 2;
        if (rank_of(*ep) != expected_rank) {
            return std::unexpected(
                Error(ErrorCode::MalformedData,
                      std::format("FEN en passant square {} is on the wrong rank for the side "
                                  "to move",
                                  fields[3])));
        }
        // Recorded only when a pawn can use it, as `Position::after` does, so a FEN that
        // names a square nobody can capture on parses to the same position as one that does
        // not — which is what keeps the repetition key honest.
        const Piece capturer = make_piece(position.side_to_move, PieceKind::Pawn);
        const std::uint8_t capturer_rank = position.side_to_move == Colour::White ? 4 : 3;
        const int ep_file = file_of(*ep);
        for (const int df : {-1, 1}) {
            const int f = ep_file + df;
            if (f >= 0 && f < kFiles &&
                position.at(square(static_cast<std::uint8_t>(f), capturer_rank)) == capturer) {
                position.en_passant_file = static_cast<std::uint8_t>(ep_file);
            }
        }
    }

    if (fields.size() >= 5) {
        auto halfmove = parse_number<std::uint16_t>(fields[4], "halfmove clock", 0,
                                                    StateTable::kMaxHalfmoveClock);
        if (!halfmove) {
            return std::unexpected(std::move(halfmove).error());
        }
        position.halfmove_clock = *halfmove;
    }
    if (fields.size() >= 6) {
        auto fullmove = parse_number<std::uint16_t>(fields[5], "fullmove number", 1,
                                                    StateTable::kMaxFullmoveNumber);
        if (!fullmove) {
            return std::unexpected(std::move(fullmove).error());
        }
        position.fullmove_number = *fullmove;
    }
    return position;
}

std::string to_fen(const Position& position) {
    std::string out;
    for (int rank = 7; rank >= 0; --rank) {
        int empty = 0;
        for (int file = 0; file < kFiles; ++file) {
            const Piece piece = position.at(
                square(static_cast<std::uint8_t>(file), static_cast<std::uint8_t>(rank)));
            if (piece == Piece::None) {
                ++empty;
                continue;
            }
            if (empty > 0) {
                out += static_cast<char>('0' + empty);
                empty = 0;
            }
            out += letter_of(piece);
        }
        if (empty > 0) {
            out += static_cast<char>('0' + empty);
        }
        if (rank > 0) {
            out += '/';
        }
    }
    out += position.side_to_move == Colour::White ? " w " : " b ";
    if (position.castling == 0) {
        out += '-';
    } else {
        if ((position.castling & kWhiteKingSide) != 0) {
            out += 'K';
        }
        if ((position.castling & kWhiteQueenSide) != 0) {
            out += 'Q';
        }
        if ((position.castling & kBlackKingSide) != 0) {
            out += 'k';
        }
        if ((position.castling & kBlackQueenSide) != 0) {
            out += 'q';
        }
    }
    out += ' ';
    if (position.en_passant_file == kNoEnPassant) {
        out += '-';
    } else {
        const std::uint8_t rank = position.side_to_move == Colour::White ? 5 : 2;
        out += square_name(square(position.en_passant_file, rank));
    }
    out += std::format(" {} {}", position.halfmove_clock, position.fullmove_number);
    return out;
}

Result<Move> parse_move(std::string_view text) {
    if (text.size() != 4 && text.size() != 5) {
        return std::unexpected(
            Error(ErrorCode::MalformedData, std::format("'{}' is not a move", text)));
    }
    auto from = parse_square(text.substr(0, 2));
    if (!from) {
        return std::unexpected(std::move(from).error());
    }
    auto to = parse_square(text.substr(2, 2));
    if (!to) {
        return std::unexpected(std::move(to).error());
    }
    Move move{.from = *from, .to = *to};
    if (text.size() == 5) {
        switch (text[4]) {
        case 'q': move.promotion = PieceKind::Queen; break;
        case 'r': move.promotion = PieceKind::Rook; break;
        case 'b': move.promotion = PieceKind::Bishop; break;
        case 'n': move.promotion = PieceKind::Knight; break;
        default:
            return std::unexpected(Error(ErrorCode::MalformedData,
                                         std::format("'{}' is not a promotion piece", text[4])));
        }
    }
    return move;
}

std::string square_name(Square sq) {
    return std::string{static_cast<char>('a' + file_of(sq)), static_cast<char>('1' + rank_of(sq))};
}

std::string to_string(Move move) {
    std::string out = square_name(move.from) + square_name(move.to);
    switch (move.promotion) {
    case PieceKind::Queen: out += 'q'; break;
    case PieceKind::Rook: out += 'r'; break;
    case PieceKind::Bishop: out += 'b'; break;
    case PieceKind::Knight: out += 'n'; break;
    default: break;
    }
    return out;
}

}  // namespace atlas::chess
