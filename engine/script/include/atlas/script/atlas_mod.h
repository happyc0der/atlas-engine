/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef ATLAS_MOD_H
#define ATLAS_MOD_H

/**
 * The whole of a mod's world.
 *
 * This header is the **one definition of the guest interface**, and it is deliberately C rather
 * than C++: it is compiled for wasm32 by whatever toolchain a mod author has, not by this
 * project's build. Atlas compiles it as well, which keeps the error codes and log levels shared
 * rather than transcribed. It does not check that the host's implementations match these
 * signatures — the host's take an execution environment the guest never sees — so what enforces
 * that is the tests, which call every import through a real guest.
 *
 * **Everything a mod can reach is declared below.** WebAssembly has no ambient authority: a
 * module can touch only its own linear memory and the functions it imports, and the loader
 * refuses any import that is not in this file. So this list is not a summary of the interface,
 * it is the interface, and the things missing from it are missing on purpose:
 *
 *   - **No clock.** Not `time`, not a frame counter, not a monotonic tick of its own. Under
 *     lockstep every peer must reach the same decision, and a mod that can read a clock is a
 *     mod that can decide differently on a slower machine. `atlas_tick` gives the simulation's
 *     tick, which is the same number everywhere by construction.
 *   - **No filesystem, no network, no environment.** A mod is untrusted input from a mounted
 *     directory; it gets data through views and it gets to submit commands, and that is all.
 *   - **No allocator, no libc.** WASI and WAMR's built-in libc are both compiled out. A mod
 *     that wants a heap brings one inside its own linear memory.
 *   - **No random source of its own.** `atlas_random` is keyed by the simulation's seed, the
 *     tick, and the mod's identity, so two peers draw the same numbers and a recording replays
 *     them. A mod using its own generator would diverge on the first draw.
 *
 * **A mod never says who it is.** `atlas_submit` takes no source: the host stamps the mod's own
 * identifier and the target tick. Claiming to be another peer is not forbidden, it is
 * unrepresentable — which is where ADR-0015 decides the restriction belongs, after M14 tried
 * putting it in the interface every command source shares and found that wrong.
 *
 * Errors are returned, never thrown and never trapped: a negative result means the call did
 * nothing. A mod is free to ignore them, and a mod that ignores them produces fewer commands
 * rather than an inconsistent simulation.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The module name every import below lives under. */
#define ATLAS_IMPORT_MODULE "atlas"

/**
 * Severity for atlas_log, matching the engine's own levels.
 *
 * Prefixed `ATLAS_MOD_` rather than `ATLAS_` because the engine's logging macros are called
 * `ATLAS_LOG_DEBUG` and the rest, and this header is compiled on both sides of the boundary.
 * An object-like macro with the same name as a function-like one is a compile error in the one
 * translation unit that needs both.
 */
#define ATLAS_MOD_LOG_DEBUG 0
#define ATLAS_MOD_LOG_INFO 1
#define ATLAS_MOD_LOG_WARN 2
#define ATLAS_MOD_LOG_ERROR 3

/** Returned by a call that did nothing because its arguments were out of range. */
#define ATLAS_ERR_RANGE (-1)
/** Returned by a call refused because a per-tick limit is spent. */
#define ATLAS_ERR_EXHAUSTED (-2)
/** Returned by a call the host refused for any other reason, such as an unknown command type. */
#define ATLAS_ERR_REFUSED (-3)

/*
 * The import attributes exist only when compiling for WebAssembly. Atlas compiles this header
 * too — it shares the constants and the error codes with the host — and there the attributes
 * are meaningless and would be an unknown-attribute error under this project's warning
 * settings, so they compile away.
 */
#ifndef ATLAS_MOD_IMPORT
#if defined(__wasm__)
#define ATLAS_MOD_IMPORT(name)                                                                     \
    __attribute__((import_module(ATLAS_IMPORT_MODULE), import_name(name)))
#else
#define ATLAS_MOD_IMPORT(name)
#endif
#endif

/**
 * The tick this call is deciding for.
 *
 * Monotonic, identical on every peer, and the only notion of time a mod has.
 */
ATLAS_MOD_IMPORT("atlas_tick") int64_t atlas_tick(void);

/**
 * Resolve a command type by name.
 *
 * Returns the type's value, or 0 if the application has registered no handler for that name —
 * so a mod written against a game that is not running submits nothing rather than something
 * wrong. The value is stable for a given name and may be cached across ticks.
 */
ATLAS_MOD_IMPORT("atlas_command_type")
int32_t atlas_command_type(const char* name, int32_t name_len);

/**
 * Submit one command.
 *
 * Stamped by the host with this mod's identifier and with a target tick far enough ahead for
 * every peer to have it in time. Returns 0 on success, or a negative code: the payload is too
 * large, the type has no handler, the payload failed that handler's own validation, or this
 * mod's command budget for the tick is spent.
 *
 * A refused command is not an error the simulation notices. It is counted and the tick carries
 * on, because one mod producing rubbish must not stop the others being polled.
 */
ATLAS_MOD_IMPORT("atlas_submit")
int32_t atlas_submit(int32_t type, const void* payload, int32_t payload_len);

/**
 * A number from the simulation's own generator.
 *
 * Keyed by the seed, the tick, this mod's identity and `stream`, so two mods asking for the
 * same stream do not collide and the same mod re-run at the same tick draws the same sequence.
 * Returns a value in [0, bound), or a negative code if `bound` is not positive.
 */
ATLAS_MOD_IMPORT("atlas_random") int64_t atlas_random(int32_t stream, int64_t bound);

/** Write a line to the engine's log. Bounded per tick; beyond that, lines are dropped. */
ATLAS_MOD_IMPORT("atlas_log") void atlas_log(int32_t level, const char* text, int32_t text_len);

/**
 * How many read-only views the application has published this tick.
 *
 * A view is a flat run of bytes whose meaning the *application* defines, not the engine —
 * ADR-0015 D5, and the reason is that the engine has no game state to describe. Views are
 * recomputed each tick and their contents are identical on every peer.
 */
ATLAS_MOD_IMPORT("atlas_view_count") int32_t atlas_view_count(void);

/** Bytes in a view, or a negative code if there is no such view. */
ATLAS_MOD_IMPORT("atlas_view_size") int32_t atlas_view_size(int32_t view);

/**
 * Copy from a view into the mod's own memory.
 *
 * Returns how many bytes were copied, which is less than `len` at the end of the view, or a
 * negative code if the view or the offset is out of range.
 */
ATLAS_MOD_IMPORT("atlas_view_read")
int32_t atlas_view_read(int32_t view, int32_t offset, void* dest, int32_t len);

/*
 * What a mod must export. The loader refuses a module missing any of them. Exporting more than
 * this is a mod's own business and is not refused: an export offers the host something, where
 * an import asks for something, and only the asking is authority.
 *
 *   memory        linear memory, with a declared maximum the host can check
 *   mod_init      called once; return non-zero to decline to run
 *   mod_tick      called once per simulation tick, before that tick
 *   mod_shutdown  called once, before the mod is unloaded
 */
#ifndef ATLAS_MOD_EXPORT
#if defined(__wasm__)
#define ATLAS_MOD_EXPORT(name) __attribute__((export_name(name)))
#else
#define ATLAS_MOD_EXPORT(name)
#endif
#endif

#ifdef __cplusplus
}
#endif

#endif /* ATLAS_MOD_H */
