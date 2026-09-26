/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * A chess opponent, written as a sandboxed mod (ADR-0023).
 *
 * Freestanding C compiled to wasm32 by `tools/build_mods.py`, which also records the toolchain
 * that built the committed module. It reaches the game only through `atlas_mod.h`: it reads the
 * position the chess application publishes as bytes, and submits a move as a command, exactly
 * as a person's click does. It knows chess because it carries its own rules; the engine does
 * not, and no import it is given does.
 *
 * This is the skeleton M26's first slice proves the toolchain with: it loads, runs under the
 * limits, and says so once.
 */
#include <atlas/script/atlas_mod.h>

#define EXPORT(name) __attribute__((export_name(name)))

static int announced = 0;

EXPORT("mod_init") int32_t mod_init(void) {
    /* No import may be called here: the host sets up a call's context only around a tick. */
    return 0;
}

EXPORT("mod_tick") void mod_tick(int64_t tick) {
    (void)tick;
    if (!announced) {
        static const char kReady[] = "chess opponent ready";
        atlas_log(ATLAS_MOD_LOG_INFO, kReady, (int32_t)(sizeof kReady - 1));
        announced = 1;
    }
}

EXPORT("mod_shutdown") void mod_shutdown(void) {}
