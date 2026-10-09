#include <metal_stdlib>
#include "frame_uniforms.h"
using namespace metal;

struct VertexOutput {
    float4 position [[position]];
    float3 color;
};

vertex VertexOutput hello_vertex(uint id [[vertex_id]],
                                constant FrameUniforms& frame [[buffer(0)]]) {
    const float2 positions[3] = {
        float2(0.0, 0.62), float2(-0.58, -0.44), float2(0.58, -0.44)
    };
    const float3 colors[3] = {
        float3(1.0, 0.35, 0.25), float3(0.2, 0.9, 0.75), float3(0.35, 0.45, 1.0)
    };
    float2 p = positions[id];
    float c = cos(frame.angle);
    float s = sin(frame.angle);
    p = float2(c * p.x - s * p.y, s * p.x + c * p.y);
    // Preserve the triangle's proportions in both wide and tall windows.
    p.x /= max(frame.aspect, 1.0f);
    p.y *= min(frame.aspect, 1.0f);
    p += float2(frame.offset_x, frame.offset_y);
    return {float4(p, 0.0, 1.0), colors[id]};
}

fragment float4 hello_fragment(VertexOutput in [[stage_in]]) {
    return float4(in.color, 1.0);
}
