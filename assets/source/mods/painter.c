/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The engine's own compiled mod: each tick, recolour one cell of the lab's grid to a colour it is
 * not already (ADR-0024 D8).
 *
 * It exists so that the mod toolchain ADR-0023 chose is exercised by this repository, not only by
 * the chess application that first needed it and that has moved to its own. It is the C counterpart
 * of `assets/mods/synthetic.wasm`, which `tools/gen_mods.py` writes byte by byte, and does one
 * thing more: it reads a cell's current colour before choosing a new one, so it reads both of the
 * views the lab publishes rather than only the first. It means nothing; the lab has no game in it.
 *
 * What the lab publishes (`apps/lab/sim/include/atlas/lab/mod_views.hpp`):
 *   view 0  the grid's shape: cell count, width, height, each four bytes little-endian
 *   view 1  every cell's colour index, one byte each, in cell order
 * and the command it accepts: `set_color_index`, a four-byte little-endian cell and a colour.
 *
 * Freestanding C: `atlas_mod.h` is the only header from the engine, and there is no libc.
 */
#include <atlas/script/atlas_mod.h>

#include <stdint.h>

#define EXPORT(name) __attribute__((export_name(name)))

#define VIEW_LAYOUT 0
#define VIEW_COLORS 1
#define COLOR_COUNT 8

/* Two named streams, so the cell and the colour do not share a sequence. */
#define STREAM_CELL 0
#define STREAM_COLOR 1

static const char kCommand[] = "set_color_index";

EXPORT("mod_init") int32_t mod_init(void) {
    /* No import may be called here: the host sets up a call's context only around a tick. */
    return 0;
}

EXPORT("mod_tick") void mod_tick(int64_t tick) {
    (void)tick;

    uint8_t layout[4];
    if (atlas_view_read(VIEW_LAYOUT, 0, layout, 4) != 4) {
        return; /* Not the lab, or not a grid: nothing to paint. */
    }
    const uint32_t cells = (uint32_t)layout[0] | ((uint32_t)layout[1] << 8) |
                           ((uint32_t)layout[2] << 16) | ((uint32_t)layout[3] << 24);
    if (cells == 0) {
        return;
    }
    const uint32_t cell = (uint32_t)atlas_random(STREAM_CELL, (int64_t)cells);

    uint8_t current = 0;
    if (atlas_view_read(VIEW_COLORS, (int32_t)cell, &current, 1) != 1) {
        return;
    }
    /* One of the other seven colours, never the one the cell already has. */
    const uint8_t next =
        (uint8_t)((current + 1U + (uint32_t)atlas_random(STREAM_COLOR, COLOR_COUNT - 1)) %
                  COLOR_COUNT);

    /* Looked up every tick rather than kept: against an application without this command the
     * answer is zero and the mod submits nothing, instead of a type that happens to collide. */
    const int32_t type = atlas_command_type(kCommand, (int32_t)(sizeof kCommand - 1));
    if (type == 0) {
        return;
    }
    const uint8_t payload[5] = {(uint8_t)cell, (uint8_t)(cell >> 8), (uint8_t)(cell >> 16),
                                (uint8_t)(cell >> 24), next};
    (void)atlas_submit(type, payload, 5);
}

EXPORT("mod_shutdown") void mod_shutdown(void) {}
