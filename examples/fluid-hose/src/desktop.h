#pragma once
#include "displays.h"
#include "fluid_types.h"
#include <string>
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
bool set_desktop_error(const char* format, ...);
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
class FluidSimulation;
// Encodes the simulation's pending steps, then draws the water, in one command buffer.
bool draw_hose_frame(HoseRenderer* renderer, FluidSimulation& simulation, SceneUniforms scene, bool& presented);
// Renders a scripted scene offscreen over a synthetic desktop and writes a PNG.
bool render_hose_snapshot(const char* shader_file, const char* output_file, float seconds);
bool smoke_test_hose_renderer(const char* shader_file);
bool run_hose_window_checks(const char* shader_file);
