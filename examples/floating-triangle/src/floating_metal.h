#pragma once
#include <SDL3/SDL_video.h>

struct FloatingMetalRenderer;

FloatingMetalRenderer* create_floating_metal_renderer(SDL_Window* window, const char* shader_filename);
void destroy_floating_metal_renderer(FloatingMetalRenderer* renderer);
const char* floating_metal_device_name(const FloatingMetalRenderer* renderer);
bool draw_floating_metal_frame(FloatingMetalRenderer* renderer, float angle, bool& presented);
bool smoke_test_floating_metal(const char* shader_filename);
