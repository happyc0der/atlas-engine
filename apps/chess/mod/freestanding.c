/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The two functions a compiler may call on a mod's behalf without being asked (ADR-0023 D1).
 *
 * A freestanding guest has no libc, but clang is still free to turn a structure copy or a
 * zeroing loop into a call to `memcpy` or `memset`, and wasm-ld refuses a module that calls a
 * function nobody defined. Written as plain loops, and compiled with `-fno-builtin` so the
 * compiler cannot recognise these very loops and turn them back into calls to themselves.
 */
#include <stddef.h>

void* memcpy(void* restrict dest, const void* restrict src, size_t count) {
    unsigned char* to = (unsigned char*)dest;
    const unsigned char* from = (const unsigned char*)src;
    for (size_t i = 0; i < count; ++i) {
        to[i] = from[i];
    }
    return dest;
}

void* memset(void* dest, int value, size_t count) {
    unsigned char* to = (unsigned char*)dest;
    for (size_t i = 0; i < count; ++i) {
        to[i] = (unsigned char)value;
    }
    return dest;
}
