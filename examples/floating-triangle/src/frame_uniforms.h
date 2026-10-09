#pragma once

// Shared by C++, Objective-C++, and the Metal shader.
struct FrameUniforms {
    float angle;
    float aspect;
    float offset_x;
    float offset_y;
};
static_assert(sizeof(FrameUniforms) == 16, "Frame uniforms must match the GPU layout");
