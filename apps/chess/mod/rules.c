/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The rules of chess, for a mod. See rules.h for why a mod carries its own.
 *
 * Mailbox, copy-make, pseudo-legal generation filtered by whether the mover's king is left
 * attacked: slow, obvious and correct, which is the same target `chess_sim` set itself. A mod
 * that searched deeply would want something faster, and would measure first.
 */
#include "rules.h"

typedef struct delta {
    int file;
    int rank;
} delta;

static const delta kKnight[8] = {{1, 2},   {2, 1},   {2, -1}, {1, -2},
                                 {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}};
static const delta kKing[8] = {{1, 0},  {1, 1},   {0, 1},  {-1, 1},
                               {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
static const delta kDiagonal[4] = {{1, 1}, {-1, 1}, {-1, -1}, {1, -1}};
static const delta kStraight[4] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
static const uint8_t kPromotions[4] = {CG_QUEEN, CG_ROOK, CG_BISHOP, CG_KNIGHT};

static int on_board(int file, int rank) {
    return file >= 0 && file < 8 && rank >= 0 && rank < 8;
}

static int at(int file, int rank) {
    return rank * 8 + file;
}

static uint8_t piece(int side, int kind) {
    return (uint8_t)(kind | (side == CG_BLACK ? CG_BLACK_BIT : 0));
}

static int kind_of(uint8_t code) {
    return code & 7;
}

static int colour_of(uint8_t code) {
    return (code & CG_BLACK_BIT) != 0 ? CG_BLACK : CG_WHITE;
}

static int forward(int side) {
    return side == CG_WHITE ? 1 : -1;
}

static int home_rank(int side) {
    return side == CG_WHITE ? 0 : 7;
}

static uint8_t corner_right(int square) {
    switch (square) {
    case 7: return CG_WHITE_KING_SIDE;
    case 0: return CG_WHITE_QUEEN_SIDE;
    case 63: return CG_BLACK_KING_SIDE;
    case 56: return CG_BLACK_QUEEN_SIDE;
    default: return 0;
    }
}

static int ray_hits(const cg_position* p, int file, int rank, const delta* rays, int kind, int by) {
    const uint8_t slider = piece(by, kind);
    const uint8_t queen = piece(by, CG_QUEEN);
    for (int i = 0; i < 4; ++i) {
        int f = file + rays[i].file;
        int r = rank + rays[i].rank;
        while (on_board(f, r)) {
            const uint8_t found = p->squares[at(f, r)];
            if (found != 0) {
                if (found == slider || found == queen) {
                    return 1;
                }
                break;
            }
            f += rays[i].file;
            r += rays[i].rank;
        }
    }
    return 0;
}

int cg_attacked(const cg_position* p, int square, int by) {
    const int file = square % 8;
    const int rank = square / 8;

    const int pawn_rank = rank - forward(by);
    for (int df = -1; df <= 1; df += 2) {
        if (on_board(file + df, pawn_rank) &&
            p->squares[at(file + df, pawn_rank)] == piece(by, CG_PAWN)) {
            return 1;
        }
    }
    for (int i = 0; i < 8; ++i) {
        const int f = file + kKnight[i].file;
        const int r = rank + kKnight[i].rank;
        if (on_board(f, r) && p->squares[at(f, r)] == piece(by, CG_KNIGHT)) {
            return 1;
        }
    }
    for (int i = 0; i < 8; ++i) {
        const int f = file + kKing[i].file;
        const int r = rank + kKing[i].rank;
        if (on_board(f, r) && p->squares[at(f, r)] == piece(by, CG_KING)) {
            return 1;
        }
    }
    return ray_hits(p, file, rank, kDiagonal, CG_BISHOP, by) ||
           ray_hits(p, file, rank, kStraight, CG_ROOK, by);
}

int cg_in_check(const cg_position* p, int side) {
    const uint8_t king = piece(side, CG_KING);
    for (int square = 0; square < 64; ++square) {
        if (p->squares[square] == king) {
            return cg_attacked(p, square, side == CG_WHITE ? CG_BLACK : CG_WHITE);
        }
    }
    return 0;
}

typedef struct move_list {
    cg_move* moves;
    int count;
} move_list;

static void push(move_list* list, int from, int to, int promotion) {
    if (list->count < CG_MAX_MOVES) {
        cg_move* move = &list->moves[list->count++];
        move->from = (uint8_t)from;
        move->to = (uint8_t)to;
        move->promotion = (uint8_t)promotion;
    }
}

static void push_pawn(move_list* list, int side, int from, int to) {
    if (to / 8 == (side == CG_WHITE ? 7 : 0)) {
        for (int i = 0; i < 4; ++i) {
            push(list, from, to, kPromotions[i]);
        }
    } else {
        push(list, from, to, 0);
    }
}

/* Push a move to an empty or enemy square; returns whether a ray may continue past it. */
static int push_step(const cg_position* p, move_list* list, int from, int file, int rank,
                     int them) {
    if (!on_board(file, rank)) {
        return 0;
    }
    const uint8_t target = p->squares[at(file, rank)];
    if (target == 0) {
        push(list, from, at(file, rank), 0);
        return 1;
    }
    if (colour_of(target) == them) {
        push(list, from, at(file, rank), 0);
    }
    return 0;
}

static void slide(const cg_position* p, move_list* list, int from, const delta* rays, int them) {
    const int file = from % 8;
    const int rank = from / 8;
    for (int i = 0; i < 4; ++i) {
        int f = file + rays[i].file;
        int r = rank + rays[i].rank;
        while (push_step(p, list, from, f, r, them)) {
            f += rays[i].file;
            r += rays[i].rank;
        }
    }
}

static void generate_pseudo_legal(const cg_position* p, move_list* list) {
    const int us = p->side;
    const int them = us == CG_WHITE ? CG_BLACK : CG_WHITE;
    const int dir = forward(us);

    for (int from = 0; from < 64; ++from) {
        const uint8_t code = p->squares[from];
        if (code == 0 || colour_of(code) != us) {
            continue;
        }
        const int file = from % 8;
        const int rank = from / 8;
        switch (kind_of(code)) {
        case CG_PAWN: {
            const int ahead = rank + dir;
            if (on_board(file, ahead) && p->squares[at(file, ahead)] == 0) {
                push_pawn(list, us, from, at(file, ahead));
                const int two = rank + 2 * dir;
                if (rank == (us == CG_WHITE ? 1 : 6) && p->squares[at(file, two)] == 0) {
                    push(list, from, at(file, two), 0);
                }
            }
            for (int df = -1; df <= 1; df += 2) {
                if (!on_board(file + df, ahead)) {
                    continue;
                }
                const int to = at(file + df, ahead);
                const uint8_t target = p->squares[to];
                if (target != 0 && colour_of(target) == them) {
                    push_pawn(list, us, from, to);
                } else if (target == 0 && p->en_passant_file == file + df &&
                           rank == (us == CG_WHITE ? 4 : 3)) {
                    push(list, from, to, 0);
                }
            }
            break;
        }
        case CG_KNIGHT:
            for (int i = 0; i < 8; ++i) {
                (void)push_step(p, list, from, file + kKnight[i].file, rank + kKnight[i].rank,
                                them);
            }
            break;
        case CG_BISHOP: slide(p, list, from, kDiagonal, them); break;
        case CG_ROOK: slide(p, list, from, kStraight, them); break;
        case CG_QUEEN:
            slide(p, list, from, kDiagonal, them);
            slide(p, list, from, kStraight, them);
            break;
        case CG_KING: {
            for (int i = 0; i < 8; ++i) {
                (void)push_step(p, list, from, file + kKing[i].file, rank + kKing[i].rank, them);
            }
            const int home = home_rank(us);
            if (from != at(4, home) || cg_attacked(p, from, them)) {
                break;
            }
            const uint8_t rook = piece(us, CG_ROOK);
            const uint8_t king_side = us == CG_WHITE ? CG_WHITE_KING_SIDE : CG_BLACK_KING_SIDE;
            const uint8_t queen_side = us == CG_WHITE ? CG_WHITE_QUEEN_SIDE : CG_BLACK_QUEEN_SIDE;
            if ((p->castling & king_side) != 0 && p->squares[at(7, home)] == rook &&
                p->squares[at(5, home)] == 0 && p->squares[at(6, home)] == 0 &&
                !cg_attacked(p, at(5, home), them) && !cg_attacked(p, at(6, home), them)) {
                push(list, from, at(6, home), 0);
            }
            if ((p->castling & queen_side) != 0 && p->squares[at(0, home)] == rook &&
                p->squares[at(1, home)] == 0 && p->squares[at(2, home)] == 0 &&
                p->squares[at(3, home)] == 0 && !cg_attacked(p, at(3, home), them) &&
                !cg_attacked(p, at(2, home), them)) {
                push(list, from, at(2, home), 0);
            }
            break;
        }
        default: break;
        }
    }
}

void cg_make(const cg_position* p, cg_move move, cg_position* next) {
    *next = *p;
    const uint8_t mover = p->squares[move.from];
    const int us = colour_of(mover);
    const int them = us == CG_WHITE ? CG_BLACK : CG_WHITE;
    const int is_pawn = kind_of(mover) == CG_PAWN;
    int capture = p->squares[move.to] != 0;

    /* En passant: a pawn moving diagonally to an empty square takes the pawn beside it. */
    if (is_pawn && move.from % 8 != move.to % 8 && !capture) {
        next->squares[at(move.to % 8, move.from / 8)] = 0;
        capture = 1;
    }

    next->squares[move.to] = move.promotion != 0 ? piece(us, move.promotion) : mover;
    next->squares[move.from] = 0;

    if (kind_of(mover) == CG_KING) {
        const int home = home_rank(us);
        if (move.from == at(4, home) && move.to == at(6, home)) {
            next->squares[at(7, home)] = 0;
            next->squares[at(5, home)] = piece(us, CG_ROOK);
        } else if (move.from == at(4, home) && move.to == at(2, home)) {
            next->squares[at(0, home)] = 0;
            next->squares[at(3, home)] = piece(us, CG_ROOK);
        }
        const uint8_t both = us == CG_WHITE ? (CG_WHITE_KING_SIDE | CG_WHITE_QUEEN_SIDE)
                                            : (CG_BLACK_KING_SIDE | CG_BLACK_QUEEN_SIDE);
        next->castling = (uint8_t)(next->castling & ~both);
    }
    /* A rook leaving its corner, or captured on it, takes that side's right with it. */
    next->castling = (uint8_t)(next->castling & ~corner_right(move.from));
    next->castling = (uint8_t)(next->castling & ~corner_right(move.to));

    /* Recorded only when a pawn could take it, as the board table records it. */
    next->en_passant_file = CG_NO_EN_PASSANT;
    const int rank_step = move.to / 8 - move.from / 8;
    if (is_pawn && (rank_step == 2 || rank_step == -2)) {
        const int file = move.to % 8;
        const int rank = move.to / 8;
        for (int df = -1; df <= 1; df += 2) {
            if (on_board(file + df, rank) &&
                p->squares[at(file + df, rank)] == piece(them, CG_PAWN)) {
                next->en_passant_file = (uint8_t)file;
            }
        }
    }

    next->halfmove_clock = (is_pawn || capture) ? 0 : (uint16_t)(p->halfmove_clock + 1);
    if (us == CG_BLACK) {
        next->fullmove_number = (uint16_t)(p->fullmove_number + 1);
    }
    next->side = (uint8_t)them;
}

int cg_generate_legal(const cg_position* p, cg_move out[CG_MAX_MOVES]) {
    cg_move pseudo[CG_MAX_MOVES];
    move_list list = {pseudo, 0};
    generate_pseudo_legal(p, &list);

    int count = 0;
    for (int i = 0; i < list.count; ++i) {
        cg_position next;
        cg_make(p, pseudo[i], &next);
        if (!cg_in_check(&next, p->side)) {
            out[count++] = pseudo[i];
        }
    }
    return count;
}

uint64_t cg_perft(const cg_position* p, int depth) {
    if (depth <= 0) {
        return 1;
    }
    cg_move moves[CG_MAX_MOVES];
    const int count = cg_generate_legal(p, moves);
    if (depth == 1) {
        return (uint64_t)count;
    }
    uint64_t nodes = 0;
    for (int i = 0; i < count; ++i) {
        cg_position next;
        cg_make(p, moves[i], &next);
        nodes += cg_perft(&next, depth - 1);
    }
    return nodes;
}
