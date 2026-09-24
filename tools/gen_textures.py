#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the committed textures, or check that the committed ones are current.

The same arrangement `gen_audio_assets.py` and `cook_shaders.py` use, and for the same reason:
a binary asset of unknown provenance is one nobody can regenerate, relicense, or explain.
Everything here comes from integer arithmetic in the standard library, so this script is the
provenance and `--check` proves the committed bytes still match it.

Two files, and they exist for different reasons.

`sheet.png` is the four-cell sprite sheet the sandbox's grandchild cycles through. Its cells
are **two columns by two rows**, not four in a line, so that the real application exercises the
row arithmetic in `animation::cell_uv` rather than only the tests. Each cell is a different
colour with a bright corner marker, so a wrong cell or a transposed grid is visible at a glance
rather than subtly off.

`chess_pieces.png` is the chess application's sheet (M22): eight sixteen-pixel cells a row,
the six white pieces on the top row and the six black ones below, in the order the rules library
numbers them — pawn, knight, bishop, rook, queen, king — so a piece's cell is its kind minus one
and its colour. The seventh cell of each row is solid white, which the board tints into its
squares and highlights, and the eighth is a white ring, which marks a legal move to an empty
square. Each glyph is written as a fill mask and its outline is derived from it, so a white
piece is outlined dark and a black one light, and either reads on either colour of square.

`tile.png` replaces the file of the same name that had been committed since M4 with no recorded
origin at all. `assets/source/PROVENANCE.md` said it should be replaced by a generated pattern
when something next touched the sandbox's assets, and this is that.

**The PNGs carry no compressed data, deliberately.** A PNG's pixel data is a zlib stream, and
zlib's *compressor* is free to produce different bytes for the same input in different versions
and implementations — which is exactly how the shader currency check came to pass by
coincidence for six milestones (see M13's report). A byte comparison against a file built by
another machine's zlib would be comparing libraries rather than content. So the deflate stream
here is written as **stored blocks**: uncompressed, spelled out by this file, and identical
wherever it runs. The checksums stay in `zlib` because CRC-32 and Adler-32 are exact algorithms
with one defined answer, not heuristics. The cost is about four kilobytes per file, which is
nothing, and the benefit is a check that means what it says.

    tools/gen_textures.py            write the textures
    tools/gen_textures.py --check    regenerate into a scratch tree and byte-compare
"""

from __future__ import annotations

import argparse
import pathlib
import shutil
import struct
import sys
import zlib

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
TEXTURE_DIR = REPO_ROOT / "assets" / "source" / "textures"

# Bumped by hand when the generated content changes in a way that invalidates what is
# committed. Nothing reads it at runtime; it exists so that a deliberate change to a pattern
# and an accidental one look different in a diff.
GENERATOR_VERSION = 1

# One cell of the sprite sheet, in pixels. Small on purpose: the sheet is a demonstration of
# frame animation, not artwork, and a reader opening it should see the four cells at once.
CELL = 16

Pixel = tuple[int, int, int, int]


def stored_deflate(payload: bytes) -> bytes:
    """A zlib stream whose deflate blocks are all stored, so the bytes depend on nothing else.

    See the module docstring for why this is not `zlib.compress`. The header is the usual
    0x78 0x01 — deflate, 32 KiB window, no preset dictionary — whose two bytes read as a
    multiple of 31, which is the check the format requires.
    """
    out = bytearray(b"\x78\x01")
    if not payload:
        payload = b""
    view = memoryview(payload)
    # A stored block carries a 16-bit length, so anything longer is split. The final block is
    # the one that sets the low bit of its header byte.
    chunks = [view[i:i + 0xFFFF] for i in range(0, len(view), 0xFFFF)] or [view]
    for index, chunk in enumerate(chunks):
        final = 1 if index == len(chunks) - 1 else 0
        out.append(final)  # BFINAL, then BTYPE 00 for a stored block
        out += struct.pack("<HH", len(chunk), len(chunk) ^ 0xFFFF)
        out += chunk
    out += struct.pack(">I", zlib.adler32(payload))
    return bytes(out)


def chunk(tag: bytes, body: bytes) -> bytes:
    return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body))


def png_bytes(rows: list[list[Pixel]], comment: str) -> bytes:
    """An 8-bit RGBA PNG with no filtering and no timestamp chunk.

    Filter type 0 on every row: a filter would make the stored stream smaller and the code
    longer, and neither is worth anything for a thirty-two pixel image.
    """
    height = len(rows)
    width = len(rows[0])
    raw = bytearray()
    for row in rows:
        assert len(row) == width, "every row of a PNG is the same width"
        raw.append(0)  # filter: none
        for red, green, blue, alpha in row:
            raw += bytes((red, green, blue, alpha))

    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    # The comment travels with the file, so a copy that escapes the repository still says
    # where it came from. Latin-1 because that is what the format's text chunks are.
    text = b"Comment\x00" + comment.encode("latin-1")
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"tEXt", text)
        + chunk(b"IDAT", stored_deflate(bytes(raw)))
        + chunk(b"IEND", b"")
    )


def sprite_sheet() -> list[list[Pixel]]:
    """Four cells in a two by two grid, each its own colour with a marker in one corner.

    The marker is in a *different* corner per cell, so the four are told apart by shape as well
    as by colour. That matters for the one failure this sheet exists to make visible: a grid
    read as rows-by-columns instead of columns-by-rows shows cells 1 and 2 swapped, which two
    similar colours would hide and two different shapes do not.
    """
    fill: list[Pixel] = [
        (222, 96, 74, 255),    # cell 0
        (244, 196, 82, 255),   # cell 1
        (108, 186, 128, 255),  # cell 2
        (104, 140, 226, 255),  # cell 3
    ]
    marker: Pixel = (250, 250, 250, 255)
    edge: Pixel = (24, 26, 34, 255)

    rows: list[list[Pixel]] = []
    for y in range(CELL * 2):
        row: list[Pixel] = []
        for x in range(CELL * 2):
            cell = (y // CELL) * 2 + (x // CELL)
            local_x = x % CELL
            local_y = y % CELL
            # A one-pixel border, so a cell drawn with the wrong rectangle bleeds visibly.
            if local_x == 0 or local_y == 0 or local_x == CELL - 1 or local_y == CELL - 1:
                row.append(edge)
                continue
            # The marker corner rotates with the cell index: 0 top-left, 1 top-right,
            # 2 bottom-left, 3 bottom-right.
            near_left = local_x < CELL // 2
            near_top = local_y < CELL // 2
            wants_left = cell in (0, 2)
            wants_top = cell in (0, 1)
            if near_left == wants_left and near_top == wants_top:
                inset_x = local_x if near_left else CELL - 1 - local_x
                inset_y = local_y if near_top else CELL - 1 - local_y
                if 3 <= inset_x <= 6 and 3 <= inset_y <= 6:
                    row.append(marker)
                    continue
            row.append(fill[cell])
        rows.append(row)
    return rows


# Fill masks, sixteen by sixteen, "X" filled and "." empty. The outline is not drawn here: every
# empty pixel beside a filled one becomes outline, which keeps the six shapes the only thing to
# read and guarantees every piece has a closed edge.
CHESS_GLYPHS: dict[str, list[str]] = {
    "pawn": [
        "................",
        "................",
        "................",
        "................",
        ".......XX.......",
        "......XXXX......",
        "......XXXX......",
        ".......XX.......",
        "......XXXX......",
        ".......XX.......",
        ".......XX.......",
        "......XXXX......",
        ".....XXXXXX.....",
        "....XXXXXXXX....",
        "....XXXXXXXX....",
        "................",
    ],
    "knight": [
        "................",
        "................",
        ".......X.X......",
        "......XXXXX.....",
        ".....XXXXXXX....",
        "....XXXX.XXX....",
        "....XXXXXXXXX...",
        "...XXXXXXXXXX...",
        "...XXX..XXXXX...",
        ".......XXXXX....",
        "......XXXXX.....",
        ".....XXXXXX.....",
        "....XXXXXXXX....",
        "....XXXXXXXX....",
        "................",
        "................",
    ],
    "bishop": [
        "................",
        ".......XX.......",
        "......XXXX......",
        ".....XXX.XX.....",
        ".....XX.XXX.....",
        ".....XXXXXX.....",
        "......XXXX......",
        ".......XX.......",
        "......XXXX......",
        "......XXXX......",
        ".....XXXXXX.....",
        "....XXXXXXXX....",
        "....XXXXXXXX....",
        "................",
        "................",
        "................",
    ],
    "rook": [
        "................",
        "................",
        "....XX.XX.XX....",
        "....XXXXXXXX....",
        ".....XXXXXX.....",
        ".....XXXXXX.....",
        ".....XXXXXX.....",
        ".....XXXXXX.....",
        ".....XXXXXX.....",
        ".....XXXXXX.....",
        "....XXXXXXXX....",
        "...XXXXXXXXXX...",
        "...XXXXXXXXXX...",
        "................",
        "................",
        "................",
    ],
    "queen": [
        "................",
        "..X....XX....X..",
        "..XX..XXXX..XX..",
        "..XXX.XXXX.XXX..",
        "...XXXXXXXXXX...",
        "...XXXXXXXXXX...",
        "....XXXXXXXX....",
        ".....XXXXXX.....",
        ".....XXXXXX.....",
        "....XXXXXXXX....",
        "...XXXXXXXXXX...",
        "...XXXXXXXXXX...",
        "................",
        "................",
        "................",
        "................",
    ],
    "king": [
        ".......XX.......",
        "......XXXX......",
        ".......XX.......",
        "....XX.XX.XX....",
        "...XXXXXXXXXX...",
        "...XXXXXXXXXX...",
        "....XXXXXXXX....",
        ".....XXXXXX.....",
        ".....XXXXXX.....",
        "....XXXXXXXX....",
        "...XXXXXXXXXX...",
        "...XXXXXXXXXX...",
        "................",
        "................",
        "................",
        "................",
    ],
}

# In the rules library's order: PieceKind 1 to 6.
CHESS_ORDER = ["pawn", "knight", "bishop", "rook", "queen", "king"]


def chess_cell(glyph: list[str], fill: Pixel, outline: Pixel) -> list[list[Pixel]]:
    assert len(glyph) == CELL and all(len(row) == CELL for row in glyph), "a glyph is one cell"
    clear: Pixel = (0, 0, 0, 0)
    cell: list[list[Pixel]] = []
    for y in range(CELL):
        row: list[Pixel] = []
        for x in range(CELL):
            if glyph[y][x] == "X":
                row.append(fill)
                continue
            beside = any(
                0 <= y + dy < CELL and 0 <= x + dx < CELL and glyph[y + dy][x + dx] == "X"
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
            row.append(outline if beside else clear)
        cell.append(row)
    return cell


def chess_pieces() -> list[list[Pixel]]:
    """Two rows of eight cells: the pieces, then a solid cell and a ring."""
    white_fill: Pixel = (242, 238, 226, 255)
    white_edge: Pixel = (28, 28, 34, 255)
    black_fill: Pixel = (44, 44, 52, 255)
    black_edge: Pixel = (226, 226, 232, 255)
    solid: Pixel = (255, 255, 255, 255)
    clear: Pixel = (0, 0, 0, 0)

    ring: list[list[Pixel]] = []
    centre = (CELL - 1) / 2
    for y in range(CELL):
        row: list[Pixel] = []
        for x in range(CELL):
            # Integer arithmetic on doubled coordinates, so the ring is exact and symmetric.
            distance_squared = (2 * x - 2 * centre) ** 2 + (2 * y - 2 * centre) ** 2
            row.append(solid if 8 * 8 <= distance_squared <= 12 * 12 else clear)
        ring.append(row)
    block = [[solid] * CELL for _ in range(CELL)]

    rows: list[list[Pixel]] = []
    for fill, edge in ((white_fill, white_edge), (black_fill, black_edge)):
        cells = [chess_cell(CHESS_GLYPHS[name], fill, edge) for name in CHESS_ORDER]
        cells += [block, ring]
        for y in range(CELL):
            rows.append([pixel for cell in cells for pixel in cell[y]])
    return rows


def tile() -> list[list[Pixel]]:
    """A neutral sixteen-pixel tile: a light face, a darker border, and a centre dot.

    White-ish on purpose. Every sandbox sprite multiplies this by its own tint, so a strongly
    coloured tile would make every tint a lie.
    """
    face: Pixel = (236, 238, 243, 255)
    edge: Pixel = (150, 156, 172, 255)
    dot: Pixel = (196, 200, 212, 255)

    rows: list[list[Pixel]] = []
    for y in range(CELL):
        row: list[Pixel] = []
        for x in range(CELL):
            if x == 0 or y == 0 or x == CELL - 1 or y == CELL - 1:
                row.append(edge)
            elif 6 <= x <= 9 and 6 <= y <= 9:
                row.append(dot)
            else:
                row.append(face)
        rows.append(row)
    return rows


def generate(into: pathlib.Path) -> list[pathlib.Path]:
    into.mkdir(parents=True, exist_ok=True)
    written = []

    comment = f"Generated by tools/gen_textures.py v{GENERATOR_VERSION}. GPL-3.0-or-later."

    sheet = into / "sheet.png"
    sheet.write_bytes(png_bytes(sprite_sheet(), comment))
    written.append(sheet)

    plain = into / "tile.png"
    plain.write_bytes(png_bytes(tile(), comment))
    written.append(plain)

    pieces = into / "chess_pieces.png"
    pieces.write_bytes(png_bytes(chess_pieces(), comment))
    written.append(pieces)

    return written


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true",
                       help="regenerate into a scratch tree and compare, changing nothing")
    args = parser.parse_args()

    if not args.check:
        written = generate(TEXTURE_DIR)
        for path in written:
            print(f"wrote {path.relative_to(REPO_ROOT)} ({path.stat().st_size} bytes)")
        return 0

    scratch = REPO_ROOT / "build" / "texture-check"
    if scratch.exists():
        shutil.rmtree(scratch)
    fresh = generate(scratch)

    stale = []
    for path in fresh:
        committed = TEXTURE_DIR / path.name
        if not committed.exists():
            stale.append(f"{path.name} has never been generated")
        elif committed.read_bytes() != path.read_bytes():
            stale.append(f"{path.name} differs from what the generator produces")

    if stale:
        for line in stale:
            print(f"textures are stale: {line}", file=sys.stderr)
        print("run tools/gen_textures.py", file=sys.stderr)
        return 1

    print(f"textures are current: {len(fresh)} file(s) match the generator")
    return 0


if __name__ == "__main__":
    sys.exit(main())
