// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file
/// A world with the five chess tables in it, and the accessors the rules use to reach them.
///
/// There are no systems. A position changes only when a move is applied, which is a command,
/// so the schedule is empty and the kernel ticks it — M14 proved an empty schedule legal, and
/// chess is the first consumer to want one. Nothing here knows how a piece moves either; that
/// arrives with the command.

#include <atlas/chess/tables.hpp>
#include <atlas/core/result.hpp>
#include <atlas/simulation/world.hpp>

namespace atlas::chess {

/// A world and the identifiers of its tables.
struct ChessWorld {
    sim::World world;
    TableIds ids;
};

/// A world holding the five tables at the standard starting position, with both colours held
/// by `SourceId::Local`. Fails only if a table cannot be registered, which is a name collision
/// and therefore a programming error in this file.
[[nodiscard]] Result<ChessWorld> make_world();

/// Put the standard starting position into a world's tables, replacing what was there.
void set_start_position(sim::World& world, const TableIds& ids);

/// Each table, by identifier. The world owns them; these return references into it.
[[nodiscard]] BoardTable& board_table(sim::World& world, const TableIds& ids) noexcept;
[[nodiscard]] const BoardTable& board_table(const sim::World& world, const TableIds& ids) noexcept;
[[nodiscard]] StateTable& state_table(sim::World& world, const TableIds& ids) noexcept;
[[nodiscard]] const StateTable& state_table(const sim::World& world, const TableIds& ids) noexcept;
[[nodiscard]] HistoryTable& history_table(sim::World& world, const TableIds& ids) noexcept;
[[nodiscard]] const HistoryTable& history_table(const sim::World& world,
                                                const TableIds& ids) noexcept;
[[nodiscard]] ResultTable& result_table(sim::World& world, const TableIds& ids) noexcept;
[[nodiscard]] const ResultTable& result_table(const sim::World& world,
                                              const TableIds& ids) noexcept;
[[nodiscard]] PlayersTable& players_table(sim::World& world, const TableIds& ids) noexcept;
[[nodiscard]] const PlayersTable& players_table(const sim::World& world,
                                                const TableIds& ids) noexcept;

/// Whether the tables describe a position chess can be played from.
///
/// Each table validates its own bytes on load; this checks what no single table can see:
/// exactly one king a side, no pawn on a back rank, castling rights only where the king and
/// rook still stand, an en passant file only with a pawn that just moved two squares, and a
/// history whose length matches the halfmove clock. A save that passes every table and fails
/// here is a position the rules would misjudge, which is why a load calls it.
[[nodiscard]] Status validate_world(const sim::World& world, const TableIds& ids);

}  // namespace atlas::chess
