#pragma once
#include <SDL3/SDL_video.h>
#include "displays.h"
#include "fluid_types.h"
#include <vector>

struct HoseRenderer;
struct DesktopInput {
    FluidPoint pointer;
    bool pouring;
    bool paused;
    bool clear;
    bool quit;
    bool tilt;
    bool reverse_tilt;
    SDL_DisplayID display;
};
enum class TiltStatus { off, waiting, active, unavailable };

HoseRenderer* create_hose_renderer(SDL_Window* window, const char* shader_file, bool continuous, bool tilt = false);
void destroy_hose_renderer(HoseRenderer* renderer);
const char* hose_device_name(const HoseRenderer* renderer);
DesktopInput poll_desktop_input(HoseRenderer* renderer, const SDL_Rect& canvas);
void set_hose_tilt_status(HoseRenderer* renderer, TiltStatus status);
bool read_desktop_displays(std::vector<DesktopDisplay>& output);
std::string saved_hose_display();
void save_hose_display(const DesktopDisplay& display);
void set_hose_displays(HoseRenderer* renderer, const std::vector<DesktopDisplay>& displays, SDL_DisplayID selected);
bool read_window_obstacles(const SDL_Rect& canvas, std::vector<WindowRect>& output);
bool draw_hose_frame(HoseRenderer* renderer, const std::vector<FluidVertex>& particles,
                     const SceneUniforms& scene, bool& presented);
bool smoke_test_hose_renderer(const char* shader_file);
