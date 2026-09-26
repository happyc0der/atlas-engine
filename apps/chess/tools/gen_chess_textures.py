#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the chess piece sheet, or check that the committed one is current.

`chess_pieces.png` is the chess application's sheet (M22): eight sixteen-pixel cells a row, the six
white pieces on the top row and the six black ones below, in the order the rules library numbers
them — pawn, knight, bishop, rook, queen, king — so a piece's cell is its kind minus one and its
colour. The seventh cell of each row is solid white, which the board tints into its squares and
highlights, and the eighth is a white ring, which marks a legal move to an empty square. Each glyph
is written as a fill mask and its outline is derived from it, so a white piece is outlined dark and
a black one light, and either reads on either colour of square.

**Chess's own, and self-contained** (ADR-0024). Until M27 this sheet was drawn by the engine's
`tools/gen_textures.py`, beside the sandbox's sheets. Chess is leaving for its own repository, and a
generator it keeps must not import one it leaves behind, so the PNG writer below is a copy of that
script's, taken at M27. The two may now diverge; each is checked against its own output.

**The PNG carries no compressed data, deliberately**, for the reason the engine's generator gives:
zlib's compressor may produce different bytes for the same input on another machine, so the deflate
stream is written as stored blocks, spelled out here and identical everywhere. The comment inside
the file names this script by its file name only, so moving the script does not change the bytes.

    python3 apps/chess/tools/gen_chess_textures.py            write the sheet
    python3 apps/chess/tools/gen_chess_textures.py --check    regenerate and byte-compare
"""

from __future__ import annotations

import argparse
import pathlib
import struct
import sys
import zlib

# The repository root: three levels up from apps/chess/tools.
ROOT = pathlib.Path(__file__).resolve().parents[3]
OUTPUT = ROOT / "assets" / "source" / "textures" / "chess_pieces.png"

# Bumped by hand when the drawing changes on purpose, so a deliberate change and an accidental
# one look different in a diff.
GENERATOR_VERSION = 1

# One cell, in pixels.
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


def generate() -> bytes:
    comment = (f"Generated by gen_chess_textures.py v{GENERATOR_VERSION}. "
               "GPL-3.0-or-later.")
    return png_bytes(chess_pieces(), comment)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true",
                        help="regenerate in memory and compare, changing nothing")
    args = parser.parse_args()

    fresh = generate()
    if not args.check:
        OUTPUT.write_bytes(fresh)
        print(f"wrote {OUTPUT.relative_to(ROOT)} ({len(fresh)} bytes)")
        return 0
    if not OUTPUT.exists():
        print(f"textures are stale: {OUTPUT.name} has never been generated", file=sys.stderr)
        return 1
    if OUTPUT.read_bytes() != fresh:
        print(f"textures are stale: {OUTPUT.name} differs from what the generator produces; "
              "run apps/chess/tools/gen_chess_textures.py", file=sys.stderr)
        return 1
    print(f"chess textures are current: {OUTPUT.name} matches the generator")
    return 0


if __name__ == "__main__":
    sys.exit(main())
