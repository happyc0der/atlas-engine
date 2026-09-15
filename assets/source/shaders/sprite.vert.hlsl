// SPDX-License-Identifier: GPL-3.0-or-later
//
// Instanced textured quads.
//
// One quad is four vertices from a shared vertex buffer; everything that differs between
// quads arrives as per-instance attributes. That is what lets thousands of them be drawn
// with one call.
//
// Register spaces are not decoration: SDL_GPU requires vertex uniform buffers at space1,
// and glslang turns an HLSL register space into a SPIR-V descriptor set, which is how that
// requirement reaches both the Vulkan and the Metal output. See ADR-0006.

cbuffer Camera : register(b0, space1) {
    float4x4 view_projection;
};

struct Input {
    // Per-vertex: the corner of a unit quad, and its texture coordinate.
    float2 corner : TEXCOORD0;
    float2 uv : TEXCOORD1;

    // Per-instance: where the quad goes, how big it is, which part of the texture it shows,
    // and what colour it is multiplied by.
    float4 instance_position_size : TEXCOORD2;
    float4 instance_uv_rect : TEXCOORD3;
    float4 instance_colour : TEXCOORD4;
};

struct Output {
    float2 uv : TEXCOORD0;
    float4 colour : TEXCOORD1;
    float4 position : SV_Position;
};

Output main(Input input) {
    const float2 world =
        input.instance_position_size.xy + (input.corner * input.instance_position_size.zw);

    Output output;
    output.position = mul(view_projection, float4(world, 0.0, 1.0));
    output.uv = input.instance_uv_rect.xy + (input.uv * input.instance_uv_rect.zw);
    output.colour = input.instance_colour;
    return output;
}
