#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the demonstration mod, as WebAssembly bytes this repository writes itself.

**Not `wat2wasm`, and that is the whole point.** CLAUDE.md: "A `--check` that compares bytes
must compare bytes only this repository decides." A `.wasm` produced by a WebAssembly assembler
is third-party compiler output, exactly like the SPIR-V that made the shader currency check pass
by coincidence from M2 to M13 — two machines with different `wabt` versions would produce
different bytes for the same source and the check would fail for no reason, or worse, pass for
the wrong one. ADR-0015 left the choice open between cooking with a toolchain behind a
manifest-and-skip comparison and writing the bytes here; this is the second, and it is the same
answer `gen_textures.py` gives for PNG and `gen_audio_assets.py` gives for WAV.

It also means the mod needs no toolchain to rebuild, on any machine, for ever. A modder writing
a real mod will use AssemblyScript or Rust or clang; Atlas's own demonstration is forty-odd
instructions and does not need them.

What the mod does, once per tick:

    cell_count = view 0, first four bytes          (the grid's size, so nothing is baked in)
    cell       = atlas_random(stream 0, cell_count)
    colour     = atlas_random(stream 1, 8)
    submit(atlas_command_type("set_color_index"), [cell as 4 LE bytes, colour])

Every number it produces comes from the host's own generator, keyed by seed, tick and the mod's
name — so two peers running it reach the same answer, which is what makes it a lockstep proof
rather than a demonstration that embedding works.
"""

from __future__ import annotations

import argparse
import pathlib
import shutil
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
MODS_DIR = REPO_ROOT / "assets" / "mods"

# Bumped by hand when the mod's behaviour changes, so that a deliberate change and an accidental
# one look different in a diff.
GENERATOR_VERSION = 1

# --- WebAssembly encoding ------------------------------------------------------------------

SECTION_TYPE = 1
SECTION_IMPORT = 2
SECTION_FUNCTION = 3
SECTION_MEMORY = 5
SECTION_EXPORT = 7
SECTION_CODE = 10
SECTION_DATA = 11

VAL_I32 = 0x7F
VAL_I64 = 0x7E

OP_END = 0x0B
OP_CALL = 0x10
OP_DROP = 0x1A
OP_I32_CONST = 0x41
OP_I64_CONST = 0x42
OP_I32_LOAD = 0x28
OP_I32_STORE = 0x36
OP_I32_STORE8 = 0x3A
OP_I32_WRAP_I64 = 0xA7
OP_I64_EXTEND_I32_U = 0xAD
OP_I32_REM_U = 0x70


def uleb(value: int) -> bytes:
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def sleb(value: int) -> bytes:
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        sign_set = bool(byte & 0x40)
        if (value == 0 and not sign_set) or (value == -1 and sign_set):
            out.append(byte)
            return bytes(out)
        out.append(byte | 0x80)


def name(text: str) -> bytes:
    encoded = text.encode("utf-8")
    return uleb(len(encoded)) + encoded


def section(ident: int, payload: bytes) -> bytes:
    return bytes([ident]) + uleb(len(payload)) + payload


def func_type(params: list[int], results: list[int]) -> bytes:
    return bytes([0x60]) + uleb(len(params)) + bytes(params) + uleb(len(results)) + bytes(results)


# --- the module ----------------------------------------------------------------------------

# Every host function either mod imports, by the shape `atlas_mod.h` declares. A pointer is an
# i32 in WebAssembly, which is why each one takes i32 where the C header says `const void*`.
HOST = {
    "atlas_command_type": ([VAL_I32, VAL_I32], [VAL_I32]),
    "atlas_submit": ([VAL_I32, VAL_I32, VAL_I32], [VAL_I32]),
    "atlas_random": ([VAL_I32, VAL_I64], [VAL_I64]),
    "atlas_view_read": ([VAL_I32, VAL_I32, VAL_I32, VAL_I32], [VAL_I32]),
    # Not in atlas_mod.h, and deliberately not. See clock_tick_body.
    "atlas_debug_clock_ns": ([], [VAL_I64]),
}

# Linear memory layout, chosen so every address is a small constant and nothing overlaps.
ADDR_CELL_COUNT = 0  # four bytes, read from view 0
ADDR_PAYLOAD = 16  # five bytes: cell as four little-endian bytes, then the colour
ADDR_COMMAND_NAME = 32  # "set_color_index"

COMMAND_NAME = "set_color_index"
COLOR_COUNT = 8
PAYLOAD_BYTES = 5


def synthetic_tick_body(imports: list[str]) -> bytes:
    """The body of `mod_tick` for the well-behaved mod, without its trailing `end`."""
    index = {name: position for position, name in enumerate(imports)}
    code = bytearray()

    # atlas_view_read(0, 0, ADDR_CELL_COUNT, 4) — the grid's size, so the mod has no idea how
    # big the world is until the application tells it.
    code += bytes([OP_I32_CONST]) + sleb(0)  # view 0
    code += bytes([OP_I32_CONST]) + sleb(0)  # offset 0
    code += bytes([OP_I32_CONST]) + sleb(ADDR_CELL_COUNT)
    code += bytes([OP_I32_CONST]) + sleb(4)
    code += bytes([OP_CALL]) + uleb(index["atlas_view_read"])
    code += bytes([OP_DROP])

    # memory[ADDR_PAYLOAD .. +4] = (i32)atlas_random(0, (i64)cell_count)
    code += bytes([OP_I32_CONST]) + sleb(ADDR_PAYLOAD)
    code += bytes([OP_I32_CONST]) + sleb(0)  # stream 0
    code += bytes([OP_I32_CONST]) + sleb(ADDR_CELL_COUNT)
    code += bytes([OP_I32_LOAD]) + bytes([0x02, 0x00])  # align 4, offset 0
    code += bytes([OP_I64_EXTEND_I32_U])
    code += bytes([OP_CALL]) + uleb(index["atlas_random"])
    code += bytes([OP_I32_WRAP_I64])
    code += bytes([OP_I32_STORE]) + bytes([0x02, 0x00])

    # memory[ADDR_PAYLOAD + 4] = (i8)atlas_random(1, COLOR_COUNT)
    code += bytes([OP_I32_CONST]) + sleb(ADDR_PAYLOAD + 4)
    code += bytes([OP_I32_CONST]) + sleb(1)  # stream 1, so the two draws do not share a sequence
    code += bytes([OP_I64_CONST]) + sleb(COLOR_COUNT)
    code += bytes([OP_CALL]) + uleb(index["atlas_random"])
    code += bytes([OP_I32_WRAP_I64])
    code += bytes([OP_I32_STORE8]) + bytes([0x00, 0x00])  # align 1, offset 0

    # atlas_submit(atlas_command_type("set_color_index"), ADDR_PAYLOAD, 5)
    #
    # The type is looked up every tick rather than cached in a global. It costs one call and it
    # means the mod does nothing at all against an application that has no such command, instead
    # of submitting a type that happens to hash the same.
    code += bytes([OP_I32_CONST]) + sleb(ADDR_COMMAND_NAME)
    code += bytes([OP_I32_CONST]) + sleb(len(COMMAND_NAME))
    code += bytes([OP_CALL]) + uleb(index["atlas_command_type"])
    code += bytes([OP_I32_CONST]) + sleb(ADDR_PAYLOAD)
    code += bytes([OP_I32_CONST]) + sleb(PAYLOAD_BYTES)
    code += bytes([OP_CALL]) + uleb(index["atlas_submit"])
    code += bytes([OP_DROP])

    return bytes(code)


SYNTHETIC_IMPORTS = ["atlas_command_type", "atlas_submit", "atlas_random", "atlas_view_read"]
CLOCK_IMPORTS = ["atlas_command_type", "atlas_submit", "atlas_view_read", "atlas_debug_clock_ns"]


def clock_tick_body(imports: list[str]) -> bytes:
    """The body of `mod_tick` for the mod that diverges on purpose.

    Identical to the well-behaved mod except for where the cell comes from: a host clock rather
    than the simulation's generator. Two peers read different nanoseconds, pick different cells,
    and disagree at the first hash check — which is the whole point. This mod is the
    demonstration of why `atlas_mod.h` has no clock in it, and the test that fails if anybody
    ever adds one: the import it needs is registered only when a host explicitly opts in, under
    a flag with "unsafe" in its name.
    """
    index = {name: position for position, name in enumerate(imports)}
    code = bytearray()

    code += bytes([OP_I32_CONST]) + sleb(0)
    code += bytes([OP_I32_CONST]) + sleb(0)
    code += bytes([OP_I32_CONST]) + sleb(ADDR_CELL_COUNT)
    code += bytes([OP_I32_CONST]) + sleb(4)
    code += bytes([OP_CALL]) + uleb(index["atlas_view_read"])
    code += bytes([OP_DROP])

    # cell = (i32)atlas_debug_clock_ns() % cell_count
    code += bytes([OP_I32_CONST]) + sleb(ADDR_PAYLOAD)
    code += bytes([OP_CALL]) + uleb(index["atlas_debug_clock_ns"])
    code += bytes([OP_I32_WRAP_I64])
    code += bytes([OP_I32_CONST]) + sleb(ADDR_CELL_COUNT)
    code += bytes([OP_I32_LOAD]) + bytes([0x02, 0x00])
    code += bytes([OP_I32_REM_U])
    code += bytes([OP_I32_STORE]) + bytes([0x02, 0x00])

    # A fixed colour, so the clock is the only thing that differs between peers.
    code += bytes([OP_I32_CONST]) + sleb(ADDR_PAYLOAD + 4)
    code += bytes([OP_I32_CONST]) + sleb(1)
    code += bytes([OP_I32_STORE8]) + bytes([0x00, 0x00])

    code += bytes([OP_I32_CONST]) + sleb(ADDR_COMMAND_NAME)
    code += bytes([OP_I32_CONST]) + sleb(len(COMMAND_NAME))
    code += bytes([OP_CALL]) + uleb(index["atlas_command_type"])
    code += bytes([OP_I32_CONST]) + sleb(ADDR_PAYLOAD)
    code += bytes([OP_I32_CONST]) + sleb(PAYLOAD_BYTES)
    code += bytes([OP_CALL]) + uleb(index["atlas_submit"])
    code += bytes([OP_DROP])

    return bytes(code)


def function_body(code: bytes) -> bytes:
    inner = uleb(0) + code + bytes([OP_END])  # no local declarations
    return uleb(len(inner)) + inner


def build_module(imports: list[str], tick_body: bytes) -> bytes:
    out = bytearray(b"\x00asm\x01\x00\x00\x00")

    # Types: the three required functions, then one per import.
    types = [
        func_type([], [VAL_I32]),  # mod_init
        func_type([VAL_I64], []),  # mod_tick
        func_type([], []),  # mod_shutdown
    ] + [func_type(*HOST[field]) for field in imports]
    out += section(SECTION_TYPE, uleb(len(types)) + b"".join(types))

    section_bytes = bytearray(uleb(len(imports)))
    for position, field in enumerate(imports):
        section_bytes += name("atlas") + name(field) + bytes([0x00]) + uleb(3 + position)
    out += section(SECTION_IMPORT, bytes(section_bytes))

    out += section(SECTION_FUNCTION, uleb(3) + uleb(0) + uleb(1) + uleb(2))

    # One page, and a declared maximum of one: the loader refuses a module that does not say how
    # far it may grow, and this one never grows at all.
    out += section(SECTION_MEMORY, uleb(1) + bytes([0x01]) + uleb(1) + uleb(1))

    exports = bytearray(uleb(4))
    exports += name("memory") + bytes([0x02]) + uleb(0)
    first_local = len(imports)
    exports += name("mod_init") + bytes([0x00]) + uleb(first_local + 0)
    exports += name("mod_tick") + bytes([0x00]) + uleb(first_local + 1)
    exports += name("mod_shutdown") + bytes([0x00]) + uleb(first_local + 2)
    out += section(SECTION_EXPORT, bytes(exports))

    init = bytes([OP_I32_CONST]) + sleb(0)  # nothing to set up, so nothing can go wrong
    bodies = function_body(init) + function_body(tick_body) + function_body(b"")
    out += section(SECTION_CODE, uleb(3) + bodies)

    data = bytearray(uleb(1))
    data += uleb(0)  # memory 0, active
    data += bytes([OP_I32_CONST]) + sleb(ADDR_COMMAND_NAME) + bytes([OP_END])
    encoded_name = COMMAND_NAME.encode("ascii")
    data += uleb(len(encoded_name)) + encoded_name
    out += section(SECTION_DATA, bytes(data))

    return bytes(out)


MODULES = [
    ("synthetic.wasm", SYNTHETIC_IMPORTS, synthetic_tick_body),
    ("clock.wasm", CLOCK_IMPORTS, clock_tick_body),
]


def generate(directory: pathlib.Path) -> list[pathlib.Path]:
    directory.mkdir(parents=True, exist_ok=True)
    written = []
    for filename, imports, body in MODULES:
        path = directory / filename
        path.write_bytes(build_module(imports, body(imports)))
        written.append(path)
    return written


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="regenerate into a scratch tree and compare, changing nothing",
    )
    args = parser.parse_args()

    if not args.check:
        for path in generate(MODS_DIR):
            print(f"wrote {path.relative_to(REPO_ROOT)} ({path.stat().st_size} bytes)")
        return 0

    scratch = REPO_ROOT / "build" / "mods-check"
    if scratch.exists():
        shutil.rmtree(scratch)
    fresh = generate(scratch)

    stale = []
    for path in fresh:
        committed = MODS_DIR / path.name
        if not committed.exists():
            stale.append(f"{path.name} has never been generated")
        elif committed.read_bytes() != path.read_bytes():
            stale.append(f"{path.name} differs from what the generator produces")

    if stale:
        for line in stale:
            print(f"mods are stale: {line}", file=sys.stderr)
        print("run tools/gen_mods.py", file=sys.stderr)
        return 1

    print(f"mods are current: {len(fresh)} file(s) match the generator")
    return 0


if __name__ == "__main__":
    sys.exit(main())
