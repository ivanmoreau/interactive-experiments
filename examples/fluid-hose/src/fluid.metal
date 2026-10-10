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

// Rubber ducks are drawn into their own layer, which the surface pass places
// under the water so submerged parts are tinted and refracted.
struct DuckOutput {
    float4 position [[position]];
    float2 local;  // duck points, facing right, y down
    float variant;
};

vertex DuckOutput duck_vertex(uint vertex_id [[vertex_id]], uint duck_id [[instance_id]],
                              constant SceneUniforms& scene [[buffer(0)]],
                              constant DuckBody* ducks [[buffer(1)]]) {
    const float2 corners[6] = {
        float2(-1, -1), float2(1, -1), float2(-1, 1),
        float2(-1, 1), float2(1, -1), float2(1, 1)
    };
    const DuckBody duck = ducks[duck_id];
    const float2 local = corners[vertex_id] * (DUCK_REACH + 2.0);
    const float2 scaled = local * float2(duck.facing, abs(duck.facing));
    const float c = cos(duck.angle), s = sin(duck.angle);
    const float2 point = float2(duck.x, duck.y) + float2(c * scaled.x - s * scaled.y, s * scaled.x + c * scaled.y);
    const float2 clip = float2(point.x / scene.width * 2 - 1, 1 - point.y / scene.height * 2);
    return {float4(clip, 0, 1), local, duck.variant};
}

namespace {
float ellipse_distance(float2 p, float2 centre, float2 radii) {
    const float2 q = (p - centre) / radii;
    return (length(q) - 1.0) * min(radii.x, radii.y);
}

float smooth_union(float a, float b, float k) {
    const float h = saturate(0.5 + 0.5 * (b - a) / k);
    return mix(b, a, h) - k * h * (1.0 - h);
}

// Layers a shape with antialiased coverage over a premultiplied colour.
float4 over(float4 below, float3 color, float distance, float pixel) {
    const float coverage = saturate(0.5 - distance / pixel);
    return float4(color * coverage, coverage) + below * (1.0 - coverage);
}
}

fragment float4 duck_fragment(DuckOutput in [[stage_in]]) {
    const float2 p = in.local;
    const float pixel = max(fwidth(p.x) + fwidth(p.y), 1e-3) * 0.7;
    const float body = ellipse_distance(p, float2(DUCK_BODY_X, DUCK_BODY_Y), float2(DUCK_BODY_RX, DUCK_BODY_RY));
    const float tail = ellipse_distance(p, float2(DUCK_BODY_X - 17, DUCK_BODY_Y - 9), float2(7, 5));
    const float head = length(p - float2(DUCK_HEAD_X, DUCK_HEAD_Y)) - DUCK_HEAD_R;
    const float silhouette = smooth_union(smooth_union(body, tail, 4.0), head, 5.0);
    const float beak = ellipse_distance(p, float2(DUCK_BEAK_X, DUCK_BEAK_Y), float2(DUCK_BEAK_RX, DUCK_BEAK_RY));
    if (min(silhouette, beak) > pixel * 2) discard_fragment();

    float3 base = float3(1.0, 0.84, 0.16);           // classic yellow
    if (in.variant > 2.5) base = float3(1.0, 0.72, 0.18);       // gold
    else if (in.variant > 1.5) base = float3(0.52, 0.9, 0.74);  // mint
    else if (in.variant > 0.5) base = float3(1.0, 0.58, 0.72);  // pink
    const bool gold = in.variant > 2.5;

    // Soft plastic shading: darker toward the rim and the underside, with a
    // glossy highlight on the back and the head.
    const float depth = saturate(-silhouette / 8.0);
    float3 color = base * mix(0.72, 1.04, depth) * mix(1.06, 0.82, saturate((p.y + 8.0) / 26.0));
    const float gloss = exp(-dot(p - float2(-7, -4), p - float2(-7, -4)) / 30.0) +
                        exp(-dot(p - float2(8, -17), p - float2(8, -17)) / 8.0);
    color += float3(1.0, 1.0, 0.95) * gloss * (gold ? 0.9 : 0.55);
    color = mix(base * 0.62, color, smoothstep(0.0, 1.2, -silhouette));  // thin darker outline
    float4 duck = float4(color, 1.0) * saturate(0.5 - silhouette / pixel);

    // Wing: a darker crescent on the body.
    const float wing = max(ellipse_distance(p, float2(-5, 3), float2(10, 6)), -ellipse_distance(p, float2(-3, -1), float2(10, 5)));
    duck = over(duck, base * 0.78, wing, pixel);
    // Beak and eye.
    duck = over(duck, mix(float3(0.95, 0.42, 0.08), float3(1.0, 0.62, 0.25), saturate((DUCK_BEAK_Y - p.y) / 3.0 + 0.5)), beak, pixel);
    duck = over(duck, float3(0.05, 0.05, 0.08), length(p - float2(14.5, -14.5)) - 1.9, pixel);
    duck = over(duck, float3(1.0), length(p - float2(15.1, -15.2)) - 0.6, pixel);
    return duck;
}

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
                                 texture2d<float> desktop [[texture(1)]],
                                 texture2d<float> ducks [[texture(2)]]) {
    constexpr sampler linear(coord::normalized, address::clamp_to_edge, filter::linear, mip_filter::linear);
    // Ducks lie under the water: seen directly outside it, and tinted and
    // bent by the surface where water covers them.
    const float4 duck_here = ducks.sample(linear, in.uv, level(0));
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
        const float foam = saturate(smoothstep(0.08, 0.45, foam_amount) * (0.55 + 0.6 * grain) + smoothstep(0.65, 1.0, speed) * 0.15);
        const float3 foam_color = float3(0.93, 0.97, 1.0);

        // The surface acts as a lens: tilted surfaces and deeper water bend the
        // view further. Thin water bends little, so edges do not drag in what
        // lies just beyond them.
        const float2 bend_points = -normal.xy / max(normal.z, 0.45) * (0.8 + 8.0 * thickness);
        const float4 duck_under = ducks.sample(linear, in.uv + bend_points / float2(scene.width, scene.height), level(0));

        if (scene.has_background > 0.5) {
            // Refraction of the captured desktop; each colour bends slightly differently.
            const float2 to_uv = 1.0 / float2(scene.background_width, scene.background_height);
            const float2 base = (float2(scene.background_x, scene.background_y) + point) * to_uv;
            const float2 bend = bend_points * to_uv;
            const float3 desktop_seen = float3(desktop.sample(linear, base + bend * 0.97, level(0)).r,
                                               desktop.sample(linear, base + bend, level(0)).g,
                                               desktop.sample(linear, base + bend * 1.03, level(0)).b);
            const float3 seen = duck_under.rgb + desktop_seen * (1.0 - duck_under.a);
            // Water absorbs red first, so depth tints the view toward cyan and blue.
            const float3 transmit = exp(-thickness * float3(0.5, 0.18, 0.08));
            float3 water = seen * transmit + float3(0.05, 0.25, 0.45) * (1.0 - transmit.g) * 0.3;
            water *= 1.0 + shimmer * 0.12;
            // At grazing angles and along the edges the surface reflects a bright sky.
            const float3 sky = mix(float3(0.85, 0.94, 1.0), float3(0.45, 0.62, 0.85), saturate(0.5 + 0.5 * normal.y));
            water = mix(water, sky, saturate(fresnel * 0.3 + edge * (0.08 + 0.22 * top)));
            water += float3(0.3, 0.6, 0.8) * below_surface * 0.15;
            water = mix(water + specular, foam_color, foam);
            color = saturate(water) * alpha + duck_here.rgb * (1.0 - alpha);
            alpha += duck_here.a * (1.0 - alpha);
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
            const float opacity = saturate(mix(0.28, 0.85, thickness) + edge * 0.35 + foam * 0.5 + specular * 0.6);
            // Inside the water: tinted water over the submerged duck.
            const float3 tint = exp(-thickness * float3(0.9, 0.35, 0.15));
            const float3 inside = saturate(water + specular) * opacity + duck_under.rgb * tint * (1.0 - opacity);
            const float inside_alpha = opacity + duck_under.a * (1.0 - opacity);
            color = inside * alpha + duck_here.rgb * (1.0 - alpha);
            alpha = inside_alpha * alpha + duck_here.a * (1.0 - alpha);
        }
    } else {
        color = duck_here.rgb;
        alpha = duck_here.a;
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
        // The opening widens and glows brighter with the flow rate.
        const float flow = max(scene.flow, 0.25);
        const float mouth = 1.8 + 1.6 * sqrt(flow);
        const float opening = 1.0 - smoothstep(mouth, mouth + 1.5, length(point - float2(scene.nozzle_x, scene.nozzle_y + 1)));
        metal = mix(metal, mix(float3(0.10, 0.55, 0.80), float3(0.55, 0.95, 1.0), saturate((flow - 0.25) / 2.75)), opening);
        color = metal * nozzle_alpha + color * (1.0 - nozzle_alpha);
        alpha = nozzle_alpha + alpha * (1.0 - nozzle_alpha);
    }
    // The desktop compositor expects premultiplied alpha.
    return float4(color, alpha);
}
