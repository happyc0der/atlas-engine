// SPDX-License-Identifier: GPL-3.0-or-later
//
// The first triangle. Deliberately reads nothing: no vertex buffer, no uniforms, no
// textures. Positions and colours come from the vertex index, so this exercises the shader
// pipeline and the draw path without involving resource binding, which differs between
// graphics backends and is dealt with separately in M3.

struct Output {
    float4 colour : TEXCOORD0;
    float4 position : SV_Position;
};

Output main(uint vertex_index : SV_VertexID) {
    const float2 positions[3] = {
        float2(0.0, 0.6),
        float2(-0.6, -0.6),
        float2(0.6, -0.6),
    };
    const float3 colours[3] = {
        float3(1.0, 0.2, 0.2),
        float3(0.2, 1.0, 0.2),
        float3(0.2, 0.2, 1.0),
    };

    Output output;
    output.position = float4(positions[vertex_index], 0.0, 1.0);
    output.colour = float4(colours[vertex_index], 1.0);
    return output;
}
