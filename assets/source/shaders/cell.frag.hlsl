// SPDX-License-Identifier: GPL-3.0-or-later
//
// The cell's colour, with no texture: a grid cell is a flat rectangle, and sampling a white
// texel to multiply by one was costing a sampler binding for nothing.

struct Input {
    float4 colour : TEXCOORD0;
};

float4 main(Input input) : SV_Target0 {
    return input.colour;
}
