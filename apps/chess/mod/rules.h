/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The rules of chess, for a mod (ADR-0023 D8).
 *
 * A mod cannot ask the host whether a move is legal: an import that knew chess would put chess in
 * the engine, which ADR-0018 forbids. So the opponent carries its own rules, and this is them.
 * Freestanding C with no library beyond <stdint.h>, compiled twice: to wasm32 into the committed
 * module by `tools/build_mods.py`, and natively into a test that checks it against the published
 * perft counts and against `atlas::chess_sim`, move for move and byte for byte.
 *
 * **The encoding is the board table's own** (`apps/chess/sim/include/atlas/chess/piece.hpp`), so
 * the position the chess application publishes as a view needs no translation: a square is
 * `rank * 8 + file` from a1, a piece is a kind in the low three bits with 8 set for black,
 * castling rights are four bits, and an en passant file of 8 means none. A move is the three bytes
 * the `chess.move` command carries.
 */
#ifndef ATLAS_CHESS_MOD_RULES_H
#define ATLAS_CHESS_MOD_RULES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CG_WHITE 0
#define CG_BLACK 1

#define CG_PAWN 1
#define CG_KNIGHT 2
#define CG_BISHOP 3
#define CG_ROOK 4
#define CG_QUEEN 5
#define CG_KING 6
#define CG_BLACK_BIT 8

#define CG_WHITE_KING_SIDE 1
#define CG_WHITE_QUEEN_SIDE 2
#define CG_BLACK_KING_SIDE 4
#define CG_BLACK_QUEEN_SIDE 8

#define CG_NO_EN_PASSANT 8

/* Past the most legal moves any chess position has, which is 218. */
#define CG_MAX_MOVES 256

typedef struct cg_position {
    uint8_t squares[64];
    uint8_t side;
    uint8_t castling;
    uint8_t en_passant_file;
    uint16_t halfmove_clock;
    uint16_t fullmove_number;
} cg_position;

typedef struct cg_move {
    uint8_t from;
    uint8_t to;
    /* 0 for none, or the kind promoted to: knight, bishop, rook or queen. */
    uint8_t promotion;
} cg_move;

/* Whether `square` is attacked by a piece of colour `by`. */
int cg_attacked(const cg_position* position, int square, int by);

/* Whether `side`'s king is attacked. A position without that king is never in check. */
int cg_in_check(const cg_position* position, int side);

/* Every legal move for the side to move, written to `out`; returns how many. */
int cg_generate_legal(const cg_position* position, cg_move out[CG_MAX_MOVES]);

/* The position after `move`, which must be legal. Clocks, rights and en passant follow. */
void cg_make(const cg_position* position, cg_move move, cg_position* after);

/* Leaf positions reached in `depth` plies. For the tests; a mod has no use for it. */
uint64_t cg_perft(const cg_position* position, int depth);

#ifdef __cplusplus
}
#endif

#endif
