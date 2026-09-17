// SPDX-License-Identifier: GPL-3.0-or-later
//
// Instanced grid cells with no per-cell geometry.
//
// The sprite shader takes forty-eight bytes per instance: a rectangle, a texture rectangle and
// a colour. For a grid every one of those except the colour is derivable, because the cells are
// stored chunk-major and a run of consecutive chunks is a contiguous range of cells. So this
// takes four bytes — the colour — and works the rest out from the instance index, exactly as
// cell_id.vert does for picking. See docs/PERFORMANCE.md.
//
// Register spaces are not decoration: SDL_GPU requires vertex uniform buffers at space1, and
// glslang turns an HLSL register space into a SPIR-V descriptor set. See ADR-0006.

cbuffer Cells : register(b0, space1) {
    float4x4 view_projection;
    float4 cell_size;  // x: world units per cell; the rest unused
    uint4 layout;      // x: cells per chunk side, y: chunks per row, z: this run's first chunk
};

struct Input {
    // Per-instance, four bytes, normalised to zero through one.
    float4 colour : TEXCOORD0;
};

struct Output {
    float4 colour : TEXCOORD0;
    float4 position : SV_Position;
};

Output main(Input input, uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
    // A triangle strip over a unit square: (0,0) (1,0) (0,1) (1,1).
    const float2 corner = float2(float(vertex_id & 1u), float(vertex_id >> 1u));

    const uint side = layout.x;
    const uint per_chunk = side * side;
    const uint chunk = layout.z + (instance_id / per_chunk);
    const uint within = instance_id % per_chunk;

    const uint cell_x = ((chunk % layout.y) * side) + (within % side);
    const uint cell_y = ((chunk / layout.y) * side) + (within / side);

    const float size = cell_size.x;
    const float2 world = (float2(float(cell_x), float(cell_y)) + corner) * size;

    Output output;
    output.position = mul(view_projection, float4(world, 0.0, 1.0));
    output.colour = input.colour;
    return output;
}
