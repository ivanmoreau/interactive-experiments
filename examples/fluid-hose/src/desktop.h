#pragma once
#include "displays.h"
#include "fluid_types.h"
#include <functional>
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
    DesktopDisplayID display;
};
enum class TiltStatus { off, waiting, active, unavailable };

const char* desktop_error();
bool initialize_hose_desktop();
HoseRenderer* create_hose_renderer(const DesktopRect& canvas, const char* shader_file, bool continuous, bool tilt = false);
void destroy_hose_renderer(HoseRenderer* renderer);
void show_hose_window(HoseRenderer* renderer);
void move_hose_window(HoseRenderer* renderer, const DesktopRect& canvas);
bool take_hose_display_changes(HoseRenderer* renderer);
void run_hose_event_loop(HoseRenderer* renderer, const std::function<bool()>& frame,
                         const std::function<bool()>& should_stop);
const char* hose_device_name(const HoseRenderer* renderer);
DesktopInput poll_desktop_input(HoseRenderer* renderer, const DesktopRect& canvas);
void set_hose_tilt_status(HoseRenderer* renderer, TiltStatus status);
bool read_desktop_displays(std::vector<DesktopDisplay>& output);
std::string saved_hose_display();
void save_hose_display(const DesktopDisplay& display);
void set_hose_displays(HoseRenderer* renderer, const std::vector<DesktopDisplay>& displays, DesktopDisplayID selected);
bool read_window_obstacles(const DesktopRect& canvas, std::vector<WindowRect>& output);
bool draw_hose_frame(HoseRenderer* renderer, const std::vector<FluidVertex>& particles,
                     const SceneUniforms& scene, bool& presented);
bool smoke_test_hose_renderer(const char* shader_file);
bool run_hose_window_checks(const char* shader_file);
