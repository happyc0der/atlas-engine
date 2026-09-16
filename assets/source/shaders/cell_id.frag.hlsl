// SPDX-License-Identifier: GPL-3.0-or-later
//
// Writes the cell identifier into an R32Uint target. The pipeline must use BlendMode::Replace:
// blending is undefined for integer formats, and the RHI refuses any other mode for one.

struct Input {
    nointerpolation uint id : TEXCOORD0;
};

uint main(Input input) : SV_Target0 {
    return input.id;
}
