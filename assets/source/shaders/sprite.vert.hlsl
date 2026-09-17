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

    // Rotation in radians, then the pivot as a fraction of the quad. The fourth component is
    // spare: a four-component attribute is what the layout already aligns to, so the room is
    // there whether or not anything uses it.
    float4 instance_rotation_pivot : TEXCOORD5;
};

struct Output {
    float2 uv : TEXCOORD0;
    float4 colour : TEXCOORD1;
    float4 position : SV_Position;
};

Output main(Input input) {
    // Where this corner sits inside the quad, in world units, before any turn.
    const float2 size = input.instance_position_size.zw;
    const float2 offset = input.corner * size;

    // Turn about the pivot rather than about the corner. A rotation needs a point, and the
    // centre is what a sprite spinning on the spot wants; rotating about the origin would
    // swing the rectangle around instead of spinning it.
    //
    // The trigonometry is here rather than on the processor: this runs four times per quad on
    // hardware built for it, against two library calls per quad on the thread that is also
    // building every other instance. An angle of zero costs a cosine of one and a sine of
    // zero, which is why an unrotated quad is no slower than it was before this existed.
    const float2 pivot = input.instance_rotation_pivot.yz * size;
    const float angle = input.instance_rotation_pivot.x;
    const float cosine = cos(angle);
    const float sine = sin(angle);

    const float2 from_pivot = offset - pivot;
    const float2 turned = float2(from_pivot.x * cosine - from_pivot.y * sine,
                                 from_pivot.x * sine + from_pivot.y * cosine);

    const float2 world = input.instance_position_size.xy + pivot + turned;

    Output output;
    output.position = mul(view_projection, float4(world, 0.0, 1.0));
    output.uv = input.instance_uv_rect.xy + (input.uv * input.instance_uv_rect.zw);
    output.colour = input.instance_colour;
    return output;
}
