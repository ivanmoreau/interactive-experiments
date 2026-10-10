#pragma once
#include "displays.h"
#include "fluid_sim.h"
#include "fluid_types.h"
#include "pond.h"
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
    FluidSettings fluid;
    bool refraction;
    float flow;
    unsigned add_ducks;  // ducks requested from the menu since the last poll
    bool remove_ducks;
    unsigned add_fish;   // fish requested from the menu since the last poll
    bool remove_fish;
    unsigned feed;       // pinches of food requested from the menu
    bool sprinkling;     // Control is held: food falls from the pointer
    bool show_brain;     // the fish brain window is wanted
};
struct FluidPreferences {
    FluidSettings fluid;
    bool refraction = false;
    float flow = 1.0f;
};
enum class TiltStatus { off, waiting, active, unavailable };

const char* desktop_error();
bool set_desktop_error(const char* format, ...);
bool initialize_hose_desktop();
HoseRenderer* create_hose_renderer(const DesktopRect& canvas, const char* shader_file, bool continuous, bool tilt,
                                   FluidSettings settings, bool refraction, float flow);
void destroy_hose_renderer(HoseRenderer* renderer);
void show_hose_window(HoseRenderer* renderer);
void move_hose_window(HoseRenderer* renderer, const DesktopRect& canvas);
bool take_hose_display_changes(HoseRenderer* renderer);
void run_hose_event_loop(HoseRenderer* renderer, const std::function<bool()>& frame,
                         const std::function<bool()>& should_stop);
const char* hose_device_name(const HoseRenderer* renderer);
DesktopInput poll_desktop_input(HoseRenderer* renderer, const DesktopRect& canvas);
void set_hose_tilt_status(HoseRenderer* renderer, TiltStatus status);
void set_hose_fish_status(HoseRenderer* renderer, const FishStatus& status);
// Ticks or unticks Show fish brain, for when its window is closed directly.
void set_hose_brain_shown(HoseRenderer* renderer, bool shown);
bool read_desktop_displays(std::vector<DesktopDisplay>& output);
std::string saved_hose_display();
void save_hose_display(const DesktopDisplay& display);
FluidPreferences saved_hose_preferences();
void save_hose_preferences(const FluidPreferences& preferences);
// Starts, moves, or stops desktop capture for refraction and updates its menu status.
void set_hose_refraction(HoseRenderer* renderer, bool enabled, const DesktopDisplay& display);
void set_hose_displays(HoseRenderer* renderer, const std::vector<DesktopDisplay>& displays, DesktopDisplayID selected);
// Encodes the simulation's pending steps, then draws the water, in one command buffer.
bool draw_hose_frame(HoseRenderer* renderer, FluidSimulation& simulation, const std::vector<PondBody>& bodies,
                     SceneUniforms scene, bool& presented);
// Renders a scripted scene offscreen over a synthetic desktop and writes a PNG.
bool render_hose_snapshot(const char* shader_file, const char* output_file, float seconds, const FluidSettings& settings,
                          bool refraction, float flow = 1.0f, unsigned ducks = 0, unsigned fish = 0);
bool smoke_test_hose_renderer(const char* shader_file);
bool run_hose_window_checks(const char* shader_file);
// Briefly captures the main display to confirm refraction can receive frames. Saves nothing.
bool run_capture_check();
