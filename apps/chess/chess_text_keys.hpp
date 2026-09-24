// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// Every string the chess application shows a person, by key (ADR-0016, ADR-0018).
///
/// **The application's own keys, in the application.** The engine's keys live in
/// `engine/tools/include/atlas/tools/text_keys.hpp` and its table in `strings/en.json`; a game
/// that had to edit either to add "White to move" would be patching the engine, which is the
/// one thing the probe exists to prove a game need not do. So these live here, their text in
/// `strings/chess.en.json`, and the catalog holds both tables side by side
/// (`text::Catalog::add_table`). `atlas_chess --text-check` resolves every key below against
/// what is actually loaded.

#include <array>
#include <string_view>

namespace atlas::chess::keys {

inline constexpr std::string_view kTitleGame = "chess.title.game";

inline constexpr std::string_view kStatToMove = "chess.stat.to_move";
inline constexpr std::string_view kStatMove = "chess.stat.move";
inline constexpr std::string_view kStatLastMove = "chess.stat.last_move";
inline constexpr std::string_view kStatResult = "chess.stat.result";

inline constexpr std::string_view kSideWhite = "chess.side.white";
inline constexpr std::string_view kSideBlack = "chess.side.black";
/// "{0}, in check": the side is substituted, so a translator chooses where it goes.
inline constexpr std::string_view kInCheck = "chess.value.in_check";
inline constexpr std::string_view kNoMoveYet = "chess.value.no_move_yet";
inline constexpr std::string_view kGameOver = "chess.value.game_over";

inline constexpr std::string_view kResultOngoing = "chess.result.ongoing";
inline constexpr std::string_view kResultWhiteMates = "chess.result.white_mates";
inline constexpr std::string_view kResultBlackMates = "chess.result.black_mates";
inline constexpr std::string_view kResultStalemate = "chess.result.stalemate";
inline constexpr std::string_view kResultFiftyMoves = "chess.result.fifty_moves";
inline constexpr std::string_view kResultThreefold = "chess.result.threefold";
inline constexpr std::string_view kResultInsufficient = "chess.result.insufficient";

/// Every key above, which is what `--text-check` walks.
inline constexpr std::array kAllKeys{
    kTitleGame,       kStatToMove,         kStatMove,         kStatLastMove,    kStatResult,
    kSideWhite,       kSideBlack,          kInCheck,          kNoMoveYet,       kGameOver,
    kResultOngoing,   kResultWhiteMates,   kResultBlackMates, kResultStalemate, kResultFiftyMoves,
    kResultThreefold, kResultInsufficient,
};

}  // namespace atlas::chess::keys
