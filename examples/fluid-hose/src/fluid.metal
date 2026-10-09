#include <metal_stdlib>
#include "fluid_types.h"
using namespace metal;

struct SplatOutput {
    float4 position [[position]];
    float2 local;
};

vertex SplatOutput density_vertex(uint vertex_id [[vertex_id]], uint particle_id [[instance_id]],
                                 constant SceneUniforms& scene [[buffer(0)]],
                                 device const FluidVertex* particles [[buffer(1)]]) {
    const float2 corners[6] = {
        float2(-1, -1), float2(1, -1), float2(-1, 1),
        float2(-1, 1), float2(1, -1), float2(1, 1)
    };
    FluidVertex particle = particles[particle_id];
    float2 local = corners[vertex_id];
    float2 point = float2(particle.x, particle.y) + local * particle.radius;
    float2 clip = float2(point.x / scene.width * 2 - 1, 1 - point.y / scene.height * 2);
    return {float4(clip, 0, 1), local};
}

fragment float4 density_fragment(SplatOutput in [[stage_in]]) {
    float density = exp(-dot(in.local, in.local) * 2.3);
    return float4(density, 0, 0, 0);
}

struct SurfaceOutput { float4 position [[position]]; float2 uv; };

vertex SurfaceOutput surface_vertex(uint id [[vertex_id]]) {
    const float2 positions[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    float2 p = positions[id];
    return {float4(p, 0, 1), float2((p.x + 1) * 0.5, (1 - p.y) * 0.5)};
}

fragment float4 surface_fragment(SurfaceOutput in [[stage_in]],
                                 constant SceneUniforms& scene [[buffer(0)]],
                                 texture2d<float> field [[texture(0)]]) {
    constexpr sampler sample_field(coord::normalized, address::clamp_to_edge, filter::linear);
    float density = field.sample(sample_field, in.uv).r;
    float alpha = smoothstep(0.38, 0.68, density);
    float2 texel = 1.0 / float2(field.get_width(), field.get_height());
    float2 gradient = float2(
        field.sample(sample_field, in.uv + float2(texel.x, 0)).r - field.sample(sample_field, in.uv - float2(texel.x, 0)).r,
        field.sample(sample_field, in.uv + float2(0, texel.y)).r - field.sample(sample_field, in.uv - float2(0, texel.y)).r
    );
    float3 normal = normalize(float3(-gradient * 1.7, 0.55));
    float light = pow(max(dot(normal, normalize(float3(-0.35, -0.55, 0.8))), 0.0), 12.0);
    float depth = clamp(density * 0.09, 0.0, 1.0);
    float3 water = mix(float3(0.035, 0.50, 0.86), float3(0.015, 0.28, 0.63), depth);
    water += float3(0.55, 0.78, 0.9) * light;
    float3 color = water * alpha;

    if (scene.show_nozzle > 0.5) {
        float2 point = in.uv * float2(scene.width, scene.height);
        float2 local = point - float2(scene.nozzle_x, scene.nozzle_y - 7);
        float2 box = abs(local) - float2(6, 10);
        float distance = length(max(box, 0.0)) + min(max(box.x, box.y), 0.0) - 2.0;
        float nozzle_alpha = (1.0 - smoothstep(-0.5, 0.5, distance)) * 0.97;
        float opening = 1.0 - smoothstep(3.0, 4.2, length(point - float2(scene.nozzle_x, scene.nozzle_y + 1)));
        float3 nozzle_color = mix(float3(0.12, 0.17, 0.22), float3(0.04, 0.73, 0.94), opening);
        color = nozzle_color * nozzle_alpha + color * (1.0 - nozzle_alpha);
        alpha = nozzle_alpha + alpha * (1.0 - nozzle_alpha);
    }
    // The desktop compositor expects premultiplied alpha.
    return float4(color, alpha);
}
