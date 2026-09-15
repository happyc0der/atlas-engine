// SPDX-License-Identifier: GPL-3.0-or-later
//
// Interpolated vertex colour, straight out. See triangle.vert.hlsl.

struct Input {
    float4 colour : TEXCOORD0;
};

float4 main(Input input) : SV_Target0 {
    return input.colour;
}
