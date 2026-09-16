// SPDX-License-Identifier: GPL-3.0-or-later
//
// Cell identifiers for picking. No vertex buffers at all: each cell's rectangle is derived
// from the instance index and a per-chunk uniform, and its identifier from the instance index
// plus the chunk's first cell. Chunk-major storage is what makes that derivation possible.
//
// Identifiers start at one, because an integer target clears to zero and zero has to mean
// "nothing here".
//
// Register spaces as in sprite.vert: vertex uniform buffers at space1 (ADR-0006).

cbuffer Chunk : register(b0, space1) {
    float4x4 view_projection;
    float4 origin_and_cell;  // chunk origin x, y in world units; cell size; unused
    uint4 layout;            // cells per chunk side; first cell of this chunk; unused; unused
};

struct Output {
    nointerpolation uint id : TEXCOORD0;
    float4 position : SV_Position;
};

Output main(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
    // A triangle strip over a unit square: (0,0) (1,0) (0,1) (1,1).
    const float2 corner = float2(float(vertex_id & 1u), float(vertex_id >> 1u));

    const uint side = layout.x;
    const uint cell_x = instance_id % side;
    const uint cell_y = instance_id / side;
    const float cell_size = origin_and_cell.z;
    const float2 world = origin_and_cell.xy +
                         (float2(float(cell_x), float(cell_y)) + corner) * cell_size;

    Output output;
    output.position = mul(view_projection, float4(world, 0.0, 1.0));
    output.id = layout.y + instance_id + 1u;
    return output;
}
