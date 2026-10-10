#include <metal_stdlib>
#include "fluid_types.h"
using namespace metal;

// Pass 1: each particle adds a smooth kernel to the fluid field.
// r = coverage, g = foam, b = speed, all weighted by coverage.
struct SplatOutput {
    float4 position [[position]];
    float2 local;
    float foam;
    float speed;
};

vertex SplatOutput splat_vertex(uint vertex_id [[vertex_id]], uint particle_id [[instance_id]],
                                constant SceneUniforms& scene [[buffer(0)]],
                                device const FluidParticle* particles [[buffer(1)]]) {
    const float2 corners[6] = {
        float2(-1, -1), float2(1, -1), float2(-1, 1),
        float2(-1, 1), float2(1, -1), float2(1, 1)
    };
    const FluidParticle particle = particles[particle_id];
    const float2 local = corners[vertex_id];
    const float2 point = float2(particle.x, particle.y) + local * scene.particle_radius;
    const float2 clip = float2(point.x / scene.width * 2 - 1, 1 - point.y / scene.height * 2);
    return {float4(clip, 0, 1), local, particle.foam, length(float2(particle.vx, particle.vy))};
}

fragment float4 splat_fragment(SplatOutput in [[stage_in]]) {
    const float r2 = dot(in.local, in.local);
    if (r2 >= 1) discard_fragment();
    const float x = 1 - r2;
    const float weight = x * x * x;
    return float4(weight, weight * in.foam, weight * saturate(in.speed / 1400.0), 0);
}

struct SurfaceOutput { float4 position [[position]]; float2 uv; };

vertex SurfaceOutput surface_vertex(uint id [[vertex_id]]) {
    const float2 positions[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    const float2 p = positions[id];
    return {float4(p, 0, 1), float2((p.x + 1) * 0.5, (1 - p.y) * 0.5)};
}

// Pass 2: a separable Gaussian smooths the field, so the surface follows
// the water's shape rather than individual rows of particles.
struct BlurParameters { float2 step; float sigma; float padding; };

fragment float4 blur_fragment(SurfaceOutput in [[stage_in]],
                              constant BlurParameters& blur [[buffer(0)]],
                              texture2d<float> source [[texture(0)]]) {
    constexpr sampler linear(coord::normalized, address::clamp_to_edge, filter::linear);
    const int reach = min(int(ceil(blur.sigma * 2.5)), 8);
    float4 sum = 0;
    float total = 0;
    for (int i = -reach; i <= reach; ++i) {
        const float weight = exp(-float(i * i) / (2.0 * blur.sigma * blur.sigma));
        sum += source.sample(linear, in.uv + blur.step * float(i)) * weight;
        total += weight;
    }
    return sum / total;
}

namespace {
float hash(float2 p) {
    p = fract(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float value_noise(float2 p) {
    const float2 i = floor(p), f = fract(p);
    const float2 u = f * f * (3 - 2 * f);
    return mix(mix(hash(i), hash(i + float2(1, 0)), u.x), mix(hash(i + float2(0, 1)), hash(i + float2(1, 1)), u.x), u.y);
}

// Bright filaments where a domain-warped wave pattern folds over itself,
// like light focused by a rippling surface.
float caustics(float2 p, float t) {
    float2 q = p;
    float light = 0;
    for (int i = 0; i < 3; ++i) {
        const float k = 1.0 + i * 0.6;
        q += float2(sin(q.y * 1.3 * k + t * 0.9), cos(q.x * 1.1 * k - t * 0.7)) * (0.55 / k);
        const float band = abs(sin(q.x * 1.7 + q.y * 1.3 + t * 0.4)) * abs(sin(q.x * -1.2 + q.y * 1.9 - t * 0.3));
        light += pow(1.0 - band, 9.0) / k;
    }
    return light;
}

// Water touching a screen edge continues past it: lookups are clamped inside
// the edge, so the edge itself is not drawn as a water surface.
float4 field_at(texture2d<float> field, float2 uv, float lod, float2 inset) {
    constexpr sampler linear(coord::normalized, address::clamp_to_edge, filter::linear, mip_filter::linear);
    return field.sample(linear, clamp(uv, inset, 1.0 - inset), level(lod));
}

// Coverage of a lone particle peaks at 1; the packed fluid sums to about 3.5.
constant float surface_low = 0.5;
constant float surface_high = 0.85;
constant float packed_coverage = 3.5;
constant float surface_rounding = 1.8;  // coverage over which the surface curves down to the edge

float surface_height(texture2d<float> field, float2 uv, float lod, float2 inset) {
    return smoothstep(surface_low, surface_rounding, field_at(field, uv, lod, inset).r) * surface_rounding;
}
}

fragment float4 surface_fragment(SurfaceOutput in [[stage_in]],
                                 constant SceneUniforms& scene [[buffer(0)]],
                                 texture2d<float> field [[texture(0)]],
                                 texture2d<float> desktop [[texture(1)]]) {
    constexpr sampler linear(coord::normalized, address::clamp_to_edge, filter::linear, mip_filter::linear);
    const float2 inset = scene.particle_radius * 1.2 / float2(scene.width, scene.height);
    const float4 sample = field_at(field, in.uv, 0, inset);
    const float coverage = sample.r;
    float alpha = smoothstep(surface_low, surface_high, coverage);
    float3 color = 0;

    if (alpha > 0.001) {
        const float2 point = in.uv * float2(scene.width, scene.height);
        // Normals from a slightly blurred level keep the surface glassy
        // instead of showing individual particles.
        const float2 texel = 1.0 / float2(field.get_width(), field.get_height());
        // Deeper inside the water, normals come from coarser levels, so the
        // interior refracts like calm water while the surface keeps its detail.
        const float depth = saturate(field_at(field, in.uv, 3, inset).r / packed_coverage - 0.4);
        const float normal_level = mix(1.0, 3.5, depth);
        const float2 step = texel * 1.5 * exp2(normal_level - 1.0);
        // Normals come from a height that rises only across the surface band,
        // so highlights sit on the visible edge instead of inside the water.
        const float l = surface_height(field, in.uv - float2(step.x, 0), normal_level, inset);
        const float r = surface_height(field, in.uv + float2(step.x, 0), normal_level, inset);
        const float u = surface_height(field, in.uv - float2(0, step.y), normal_level, inset);
        const float d = surface_height(field, in.uv + float2(0, step.y), normal_level, inset);
        const float2 gradient = float2(r - l, d - u);
        // Small travelling ripples keep highlights moving on still water.
        const float2 ripple = float2(value_noise(point * 0.11 + float2(scene.time * 0.7, 0)),
                                     value_noise(point * 0.11 + float2(0, scene.time * 0.6) + 17.0)) - 0.5;
        const float3 normal = normalize(float3(-gradient * 1.4 + ripple * 0.05, 0.6));

        // Thickness from coarse mip levels: deep pools absorb more light.
        const float body = field_at(field, in.uv, 3, inset).r / packed_coverage;
        const float deep = saturate(field_at(field, in.uv, 5, inset).r / packed_coverage);
        const float thickness = saturate(body * 0.3 + deep * 0.95);

        // Light from the upper left, in screen space with y pointing down.
        const float3 view = float3(0, 0, 1);
        const float3 key = normalize(float3(-0.45, -0.75, 0.55));
        const float3 half_vector = normalize(key + view);
        const float specular = pow(saturate(dot(normal, half_vector)), 90.0) * 1.5 +
                               pow(saturate(dot(normal, half_vector)), 14.0) * 0.12;
        const float fresnel = pow(1.0 - saturate(normal.z), 3.0);
        const float shimmer = caustics(point * 0.03, scene.time * 1.1) * saturate(thickness * 1.5);

        // A thin meniscus traces every edge; the top of each pool is brightest.
        const float edge = alpha * (1 - smoothstep(surface_high, surface_high + 0.5, coverage));
        const float2 direction = gradient / max(length(gradient), 1e-4);
        const float top = saturate(direction.y);
        const float below_surface = smoothstep(surface_high + 2.5, surface_high, coverage) * top;

        // Fast, aerated water and lingering splash foam turn white.
        const float foam_amount = sample.g / max(coverage, 1e-3);
        const float speed = sample.b / max(coverage, 1e-3);
        const float grain = value_noise(point * 0.35 + float2(0, scene.time * 6.0));
        const float foam = saturate(smoothstep(0.08, 0.45, foam_amount) * (0.55 + 0.6 * grain) + smoothstep(0.35, 0.8, speed) * 0.25);
        const float3 foam_color = float3(0.93, 0.97, 1.0);

        if (scene.has_background > 0.5) {
            // Refraction: the surface acts as a lens over the captured desktop.
            // Tilted surfaces and deeper water bend the view further, and each
            // colour bends slightly differently.
            const float2 to_uv = 1.0 / float2(scene.background_width, scene.background_height);
            const float2 base = (float2(scene.background_x, scene.background_y) + point) * to_uv;
            // Thin water bends little, so edges do not drag in what lies just beyond them.
            const float2 bend = -normal.xy / max(normal.z, 0.45) * (0.8 + 8.0 * thickness) * to_uv;
            const float3 seen = float3(desktop.sample(linear, base + bend * 0.97, level(0)).r,
                                       desktop.sample(linear, base + bend, level(0)).g,
                                       desktop.sample(linear, base + bend * 1.03, level(0)).b);
            // Water absorbs red first, so depth tints the view toward cyan and blue.
            const float3 transmit = exp(-thickness * float3(0.5, 0.18, 0.08));
            float3 water = seen * transmit + float3(0.05, 0.25, 0.45) * (1.0 - transmit.g) * 0.3;
            water *= 1.0 + shimmer * 0.12;
            // At grazing angles and along the edges the surface reflects a bright sky.
            const float3 sky = mix(float3(0.85, 0.94, 1.0), float3(0.45, 0.62, 0.85), saturate(0.5 + 0.5 * normal.y));
            water = mix(water, sky, saturate(fresnel * 0.3 + edge * (0.08 + 0.22 * top)));
            water += float3(0.3, 0.6, 0.8) * below_surface * 0.15;
            water = mix(water + specular, foam_color, foam);
            color = saturate(water) * alpha;
        } else {
            // Without a desktop image, tint and light the water, and let the
            // real desktop show through thin water.
            const float3 shallow_color = float3(0.42, 0.82, 1.0);
            const float3 deep_color = float3(0.01, 0.13, 0.42);
            const float3 absorption = exp(-thickness * float3(3.2, 1.5, 0.7));
            float3 water = mix(deep_color, shallow_color, absorption);
            water += float3(0.55, 0.85, 1.0) * shimmer * 0.2;
            water += float3(0.75, 0.92, 1.0) * edge * (0.35 + 0.65 * top);
            water += float3(0.25, 0.6, 0.85) * below_surface * 0.35;
            water += float3(0.6, 0.85, 1.0) * fresnel * 0.5;
            water = mix(water, foam_color, foam);
            alpha *= saturate(mix(0.28, 0.85, thickness) + edge * 0.35 + foam * 0.5 + specular * 0.6);
            color = saturate(water + specular) * alpha;
        }
    }

    if (scene.show_nozzle > 0.5) {
        const float2 point = in.uv * float2(scene.width, scene.height);
        const float2 local = point - float2(scene.nozzle_x, scene.nozzle_y - 8);
        const float2 box = abs(local) - float2(7, 11);
        const float distance = length(max(box, 0.0)) + min(max(box.x, box.y), 0.0) - 2.5;
        const float nozzle_alpha = (1.0 - smoothstep(-0.5, 0.5, distance)) * 0.97;
        // Brushed metal shading across the barrel, with a glowing opening.
        const float across = local.x / 7.0;
        float3 metal = mix(float3(0.16, 0.20, 0.25), float3(0.70, 0.76, 0.82), pow(1 - abs(across + 0.35), 3.0));
        const float opening = 1.0 - smoothstep(3.0, 4.5, length(point - float2(scene.nozzle_x, scene.nozzle_y + 1)));
        metal = mix(metal, float3(0.10, 0.75, 0.98), opening);
        color = metal * nozzle_alpha + color * (1.0 - nozzle_alpha);
        alpha = nozzle_alpha + alpha * (1.0 - nozzle_alpha);
    }
    // The desktop compositor expects premultiplied alpha.
    return float4(color, alpha);
}
