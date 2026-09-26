/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * A chess opponent, written as a sandboxed mod (ADR-0023).
 *
 * Freestanding C compiled to wasm32 by `tools/build_mods.py`, which also records the toolchain
 * that built the committed module. It reaches the game only through `atlas_mod.h`: it reads the
 * position the chess application publishes as bytes (`mod_view.h`), and submits a move as a
 * `chess.move` command, exactly as a person's click does. It knows chess because it carries its
 * own rules (`rules.c`); the engine does not, and no import it is given does.
 *
 * **How it thinks.** Two plies on material: each of its moves is scored by the worst the
 * opponent's best reply leaves it, with a move that mates scored above everything and one that
 * stalemates as a draw. A reply that leaves it checkmated is seen too, because a leaf where it is
 * in check is asked whether it has a move. It does not see further, and it does not know the
 * fifty-move rule or repetition; it is an opponent, not an engine.
 *
 * **How much it thinks per tick.** A fixed number of its own moves, `ROOT_MOVES_PER_TICK`, each
 * searched completely. A quota counted in moves rather than time is what keeps the decision the
 * same on every machine, and what bounds the most instructions any one tick can take, which is
 * measured against the runtime's budget rather than assumed. The search's state lives in linear
 * memory between ticks, which the runtime keeps.
 *
 * **What it never does** is submit twice for one position. A move it submitted lands a tick or
 * more later; until the position changes it waits.
 */
#include <atlas/chess/mod_view.h>
#include <atlas/script/atlas_mod.h>

#include "rules.h"

#define EXPORT(name) __attribute__((export_name(name)))

/*
 * The quota. See the file comment; the measured worst tick is in docs/PERFORMANCE.md. It was four
 * until the measurement: four held a 4.6-fold margin on every position measured, but one root
 * move facing the most replies any position has would cost about a million instructions, and
 * four of those would leave a margin of two and a half. Two keeps the four-fold margin even there.
 */
#define ROOT_MOVES_PER_TICK 2

#define MATE 1000000
#define WORST (-2 * MATE)

static const int32_t kValue[8] = {0, 100, 300, 300, 500, 900, 0, 0};

typedef struct search_state {
    int active;
    int submitted;
    uint8_t view[ATLAS_CHESS_VIEW_POSITION_BYTES];
    cg_position root;
    cg_move moves[CG_MAX_MOVES];
    int32_t scores[CG_MAX_MOVES];
    int count;
    int next;
} search_state;

static search_state state;
static int announced = 0;
static int warned_no_command = 0;

static void log_text(int32_t level, const char* text, int32_t length) {
    atlas_log(level, text, length);
}

static int32_t material(const cg_position* p, int side) {
    int32_t total = 0;
    for (int sq = 0; sq < 64; ++sq) {
        const uint8_t code = p->squares[sq];
        if (code == 0) {
            continue;
        }
        const int32_t value = kValue[code & 7];
        total += ((code & CG_BLACK_BIT) != 0) == (side == CG_BLACK) ? value : -value;
    }
    return total;
}

/* A position two plies on, with `us` to move again. */
static int32_t leaf(const cg_position* p, int us) {
    if (cg_in_check(p, us)) {
        cg_move replies[CG_MAX_MOVES];
        if (cg_generate_legal(p, replies) == 0) {
            return -MATE;
        }
    }
    return material(p, us);
}

/* The worst the opponent can do to us after `move`. */
static int32_t score_root_move(const cg_position* root, cg_move move) {
    const int us = root->side;
    cg_position child;
    cg_make(root, move, &child);

    cg_move replies[CG_MAX_MOVES];
    const int count = cg_generate_legal(&child, replies);
    if (count == 0) {
        return cg_in_check(&child, child.side) ? MATE : 0;
    }
    int32_t worst = MATE + 1;
    for (int i = 0; i < count; ++i) {
        cg_position grandchild;
        cg_make(&child, replies[i], &grandchild);
        const int32_t score = leaf(&grandchild, us);
        if (score < worst) {
            worst = score;
        }
    }
    return worst;
}

static void read_position(const uint8_t* view, cg_position* out) {
    for (int sq = 0; sq < 64; ++sq) {
        out->squares[sq] = view[ATLAS_CHESS_VIEW_SQUARES + sq];
    }
    out->side = view[ATLAS_CHESS_VIEW_SIDE];
    out->castling = view[ATLAS_CHESS_VIEW_CASTLING];
    out->en_passant_file = view[ATLAS_CHESS_VIEW_EN_PASSANT];
    out->halfmove_clock =
        (uint16_t)(view[ATLAS_CHESS_VIEW_HALFMOVE] | (view[ATLAS_CHESS_VIEW_HALFMOVE + 1] << 8));
    out->fullmove_number =
        (uint16_t)(view[ATLAS_CHESS_VIEW_FULLMOVE] | (view[ATLAS_CHESS_VIEW_FULLMOVE + 1] << 8));
}

static int same_view(const uint8_t* a, const uint8_t* b) {
    for (int i = 0; i < ATLAS_CHESS_VIEW_POSITION_BYTES; ++i) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static void begin(const uint8_t* view) {
    for (int i = 0; i < ATLAS_CHESS_VIEW_POSITION_BYTES; ++i) {
        state.view[i] = view[i];
    }
    read_position(view, &state.root);
    state.count = cg_generate_legal(&state.root, state.moves);
    state.next = 0;
    state.submitted = 0;
    state.active = 1;
}

/* "plays e2e4", or "plays a7a8q", into `out`; returns its length. */
static int32_t describe(cg_move move, char* out) {
    static const char kPrefix[] = "chess opponent plays ";
    static const char kPromotion[8] = {0, 0, 'n', 'b', 'r', 'q', 0, 0};
    int32_t length = 0;
    for (int i = 0; kPrefix[i] != 0; ++i) {
        out[length++] = kPrefix[i];
    }
    out[length++] = (char)('a' + move.from % 8);
    out[length++] = (char)('1' + move.from / 8);
    out[length++] = (char)('a' + move.to % 8);
    out[length++] = (char)('1' + move.to / 8);
    if (move.promotion != 0) {
        out[length++] = kPromotion[move.promotion & 7];
    }
    return length;
}

static void choose_and_submit(void) {
    int32_t best = WORST;
    for (int i = 0; i < state.count; ++i) {
        if (state.scores[i] > best) {
            best = state.scores[i];
        }
    }
    int ties = 0;
    for (int i = 0; i < state.count; ++i) {
        if (state.scores[i] == best) {
            ++ties;
        }
    }
    /* The host's generator, keyed by seed, tick and this mod's name: every peer draws the same. */
    int64_t pick = ties > 1 ? atlas_random(0, ties) : 0;
    int chosen = 0;
    for (int i = 0; i < state.count; ++i) {
        if (state.scores[i] == best) {
            if (pick == 0) {
                chosen = i;
                break;
            }
            --pick;
        }
    }

    static const char kCommand[] = "chess.move";
    const int32_t type = atlas_command_type(kCommand, (int32_t)(sizeof kCommand - 1));
    /* Zero, and only zero, means nothing handles it: a type is a hash, and may be negative. */
    if (type == 0) {
        if (!warned_no_command) {
            static const char kNoCommand[] = "chess opponent: nothing handles chess.move here";
            log_text(ATLAS_MOD_LOG_WARN, kNoCommand, (int32_t)(sizeof kNoCommand - 1));
            warned_no_command = 1;
        }
        return;
    }
    const cg_move move = state.moves[chosen];
    const uint8_t payload[3] = {move.from, move.to, move.promotion};
    if (atlas_submit(type, payload, 3) == 0) {
        state.submitted = 1;
        char line[40];
        log_text(ATLAS_MOD_LOG_INFO, line, describe(move, line));
    }
}

EXPORT("mod_init") int32_t mod_init(void) {
    /* No import may be called here: the host sets up a call's context only around a tick. */
    return 0;
}

EXPORT("mod_tick") void mod_tick(int64_t tick) {
    (void)tick;
    if (!announced) {
        static const char kReady[] = "chess opponent ready";
        log_text(ATLAS_MOD_LOG_INFO, kReady, (int32_t)(sizeof kReady - 1));
        announced = 1;
    }

    uint8_t view[ATLAS_CHESS_VIEW_POSITION_BYTES];
    uint8_t seat = 0;
    if (atlas_view_count() <= ATLAS_CHESS_VIEW_SEAT ||
        atlas_view_read(ATLAS_CHESS_VIEW_POSITION, 0, view, ATLAS_CHESS_VIEW_POSITION_BYTES) !=
            ATLAS_CHESS_VIEW_POSITION_BYTES ||
        atlas_view_read(ATLAS_CHESS_VIEW_SEAT, 0, &seat, ATLAS_CHESS_VIEW_SEAT_BYTES) !=
            ATLAS_CHESS_VIEW_SEAT_BYTES) {
        return;
    }
    const int side = view[ATLAS_CHESS_VIEW_SIDE];
    const int ours = seat == ATLAS_CHESS_SEAT_BOTH || seat == side;
    if (view[ATLAS_CHESS_VIEW_OUTCOME] != 0 || !ours) {
        state.active = 0;
        return;
    }

    if (!state.active || !same_view(view, state.view)) {
        begin(view);
    }
    if (state.submitted || state.count == 0) {
        return;
    }

    for (int done = 0; done < ROOT_MOVES_PER_TICK && state.next < state.count; ++done) {
        state.scores[state.next] = score_root_move(&state.root, state.moves[state.next]);
        ++state.next;
    }
    if (state.next == state.count) {
        choose_and_submit();
    }
}

EXPORT("mod_shutdown") void mod_shutdown(void) {}
