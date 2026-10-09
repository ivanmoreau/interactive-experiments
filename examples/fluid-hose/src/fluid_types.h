#pragma once

struct FluidPoint { float x; float y; };
struct WindowRect { float x; float y; float width; float height; };

// Scalar-only layouts are shared by the C++ app and Metal shader.
struct FluidVertex { float x; float y; float radius; float padding; };
struct SceneUniforms {
    float width;
    float height;
    float nozzle_x;
    float nozzle_y;
    float show_nozzle;
    float time;
    float padding0;
    float padding1;
};
static_assert(sizeof(FluidVertex) == 16, "Particle layout must match Metal");
static_assert(sizeof(SceneUniforms) == 32, "Scene layout must match Metal");
