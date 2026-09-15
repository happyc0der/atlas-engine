// SPDX-License-Identifier: GPL-3.0-or-later
//
// Samples the atlas and tints by the instance colour. See sprite.vert.hlsl.
//
// Fragment textures and samplers live in space2, which is what SDL_GPU expects.

Texture2D<float4> atlas_texture : register(t0, space2);
SamplerState atlas_sampler : register(s0, space2);

struct Input {
    float2 uv : TEXCOORD0;
    float4 colour : TEXCOORD1;
};

float4 main(Input input) : SV_Target0 {
    return atlas_texture.Sample(atlas_sampler, input.uv) * input.colour;
}
