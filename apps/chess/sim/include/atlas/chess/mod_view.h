/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The bytes the chess application publishes to a mod, and nothing else (ADR-0023 D7).
 *
 * A view is bytes whose meaning is the application's (ADR-0015 D5), so this layout belongs to
 * chess rather than to the engine. It is written as C macros, not C++, because both sides of the
 * sandbox read it: `mod_views.cpp` writes these bytes from the world, and the opponent in
 * `apps/chess/mod`, which is C compiled to wasm32, reads them. One definition, two languages, so
 * the two cannot drift. `tools/build_mods.py` hashes this file into the mod's manifest, so a
 * change here without a rebuilt mod fails the check.
 *
 * Views are found by index, in the order the application publishes them.
 */
#ifndef ATLAS_CHESS_MOD_VIEW_H
#define ATLAS_CHESS_MOD_VIEW_H

/* View 0: the position, from the board, state and result tables. */
#define ATLAS_CHESS_VIEW_POSITION 0
/* Sixty-four squares in the board table's own encoding, a1 first. */
#define ATLAS_CHESS_VIEW_SQUARES 0
/* 0 for white to move, 1 for black. */
#define ATLAS_CHESS_VIEW_SIDE 64
/* The four castling bits. */
#define ATLAS_CHESS_VIEW_CASTLING 65
/* The en passant file, or 8 for none. */
#define ATLAS_CHESS_VIEW_EN_PASSANT 66
/* Two bytes each, little-endian. */
#define ATLAS_CHESS_VIEW_HALFMOVE 67
#define ATLAS_CHESS_VIEW_FULLMOVE 69
/* 0 while the game goes on; otherwise the result table's outcome. */
#define ATLAS_CHESS_VIEW_OUTCOME 71
#define ATLAS_CHESS_VIEW_POSITION_BYTES 72

/* View 1: one byte saying which side or sides the mod plays. */
#define ATLAS_CHESS_VIEW_SEAT 1
#define ATLAS_CHESS_SEAT_WHITE 0
#define ATLAS_CHESS_SEAT_BLACK 1
#define ATLAS_CHESS_SEAT_BOTH 2
#define ATLAS_CHESS_VIEW_SEAT_BYTES 1

#endif
