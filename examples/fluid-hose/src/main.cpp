#include "desktop.h"
#include "fluid_sim.h"
#include "motion.h"
#include "window_tracker.h"
#include <mach-o/dyld.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

static volatile std::sig_atomic_t stop_requested = 0;
static void stop(int) { stop_requested = 1; }

static void require(bool success, const char* operation) {
    if (!success) throw std::runtime_error(std::string(operation) + ": " + desktop_error());
}
template<typename T> static T* require(T* value, const char* operation) {
    require(value != nullptr, operation);
    return value;
}

static std::filesystem::path executable_directory() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> path(size);
    if (_NSGetExecutablePath(path.data(), &size) != 0) throw std::runtime_error("Cannot locate executable");
    return std::filesystem::canonical(path.data()).parent_path();
}
static std::filesystem::path shader_path() {
    auto directory = executable_directory();
    if (directory.filename() == "MacOS") directory = directory.parent_path() / "Resources";
    // Builds without Xcode's offline Metal compiler bundle shader source instead.
    const auto compiled = directory / "fluid.metallib";
    return std::filesystem::exists(compiled) ? compiled : directory / "fluid.metal";
}

struct App {
    HoseRenderer* renderer = nullptr;
    MotionProcess motion;
    ~App() {
        motion.stop();
        if (renderer) destroy_hose_renderer(renderer);
    }
};

int main(int argc, char** argv) {
    try {
        bool physics_check = false;
        bool smoke = false;
        bool probe = false;
        bool continuous = false;
        bool tilt_enabled = false;
        bool motion_check = false;
        bool display_check = false;
        bool list_displays = false;
        bool window_check = false;
        bool refraction_option = false;
        bool capture_check = false;
        unsigned display_number = 0;
        unsigned duration = 0;
        std::string snapshot;
        std::optional<ParticleSize> size_option;
        std::optional<LiquidFeel> liquid_option;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--physics-check") physics_check = true;
            else if (argument == "--smoke-test") smoke = true;
            else if (argument == "--probe") probe = true;
            else if (argument == "--pour") continuous = true;
            else if (argument == "--tilt") tilt_enabled = true;
            else if (argument == "--motion-check") motion_check = true;
            else if (argument == "--display-check") display_check = true;
            else if (argument == "--list-displays") list_displays = true;
            else if (argument == "--window-check") window_check = true;
            else if (argument == "--refraction") refraction_option = true;
            else if (argument == "--capture-check") capture_check = true;
            else if (argument == "--snapshot" && i + 1 < argc) snapshot = argv[++i];
            else if (argument == "--size" && i + 1 < argc) {
                const std::string value = argv[++i];
                if (value == "fine") size_option = ParticleSize::fine;
                else if (value == "medium") size_option = ParticleSize::medium;
                else if (value == "coarse") size_option = ParticleSize::coarse;
                else throw std::runtime_error("--size must be fine, medium, or coarse");
            } else if (argument == "--liquid" && i + 1 < argc) {
                const std::string value = argv[++i];
                if (value == "water") liquid_option = LiquidFeel::water;
                else if (value == "thick") liquid_option = LiquidFeel::thick;
                else if (value == "goo") liquid_option = LiquidFeel::goo;
                else throw std::runtime_error("--liquid must be water, thick, or goo");
            }
            else if (argument == "--display" && i + 1 < argc) {
                const char* value = argv[++i];
                const auto result = std::from_chars(value, value + std::strlen(value), display_number);
                if (result.ec != std::errc{} || *result.ptr != '\0' || display_number == 0)
                    throw std::runtime_error("--display requires a positive display number from --list-displays");
            } else if (argument == "--duration" && i + 1 < argc) {
                const char* value = argv[++i];
                const auto result = std::from_chars(value, value + std::strlen(value), duration);
                if (result.ec != std::errc{} || *result.ptr != '\0' || duration == 0)
                    throw std::runtime_error("--duration requires a positive integer number of seconds");
            } else if (argument == "--help") {
                std::puts("Fluid Hose\nUsage: fluid-hose [--pour] [--tilt] [--display NUMBER] [--duration SECONDS]\n"
                          "                  [--size fine|medium|coarse] [--liquid water|thick|goo] [--refraction]\n"
                          "                  "
                          "[--list-displays | --probe | --physics-check | --motion-check | --display-check | --smoke-test | --window-check |\n"
                          " --snapshot FILE.png | --capture-check]\n"
                          "Hold Option to pour at the mouse pointer. Release Option to stop.\n"
                          "The overlay lets clicks pass through: arrange windows normally.\n"
                          "Use the droplet menu in the menu bar to pour continuously, pause, clear, or quit.\n"
                          "Choose a monitor in the Display submenu; switching clears the water.\n"
                          "The app remembers your selected display and handles display disconnection.\n"
                          "--tilt lets Mac motion change gravity; toggle Use Mac tilt in the droplet menu.\n"
                          "The sensor helper stops and restores its settings when tilt is off or the app exits.\n"
                          "Windows are tracked live as rounded rectangles; moving one pushes the water.\n"
                          "--size and --liquid choose particle size and how runny the liquid is; the droplet menu changes them too.\n"
                          "--refraction bends the desktop seen through the water; it needs the Screen Recording permission.\n"
                          "--snapshot renders a scripted scene offscreen to a PNG (--duration sets its length).");
                return 0;
            } else throw std::runtime_error("Unknown or incomplete option: " + argument);
        }
        if (display_check) return run_display_checks() ? 0 : 1;
        const auto motion_helper = (executable_directory() / "probe-motion").string();
        if (motion_check) return run_motion_checks(motion_helper) ? 0 : 1;
        App app;
        const auto shaders = shader_path().string();
        if (physics_check) {
            require(run_physics_checks(shaders.c_str()), "Check fluid physics");
            return 0;
        }
        // Command-line choices override the saved menu choices for this launch.
        FluidPreferences preferences = smoke || physics_check ? FluidPreferences{} : saved_hose_preferences();
        if (size_option) preferences.fluid.size = *size_option;
        if (liquid_option) preferences.fluid.feel = *liquid_option;
        if (refraction_option) preferences.refraction = true;
        if (!snapshot.empty()) {
            require(render_hose_snapshot(shaders.c_str(), snapshot.c_str(), duration ? static_cast<float>(duration) : 8.0f,
                                         preferences.fluid, refraction_option), "Render fluid snapshot");
            return 0;
        }
        if (smoke) {
            require(smoke_test_hose_renderer(shaders.c_str()), "Check fluid renderer");
            return 0;
        }

        require(initialize_hose_desktop(), "Initialize AppKit");
        if (capture_check) {
            require(run_capture_check(), "Check desktop capture");
            return 0;
        }
        if (window_check) {
            require(run_hose_window_checks(shaders.c_str()), "Check native desktop windows");
            return 0;
        }
        std::vector<DesktopDisplay> displays;
        require(read_desktop_displays(displays), "Read desktop displays");
        std::string preferred_display = saved_hose_display();
        if (display_number > displays.size()) throw std::runtime_error("Display number is out of range; use --list-displays");
        DesktopDisplay active_display = display_number ? displays[display_number - 1] :
                                        *choose_hose_display(displays, preferred_display);
        if (list_displays) {
            for (std::size_t i = 0; i < displays.size(); ++i) {
                const auto& display = displays[i];
                std::printf("%zu. %s — %d × %d points at (%d, %d)%s%s\n", i + 1, display.name.c_str(),
                            display.bounds.w, display.bounds.h, display.bounds.x, display.bounds.y,
                            display.primary ? " (main)" : "", display.id == active_display.id ? " (selected)" : "");
            }
            return 0;
        }
        if (display_number) preferred_display = active_display.uuid;
        DesktopRect canvas = active_display.canvas;
        WindowTracker tracker;
        std::vector<WindowObstacle> obstacles;
        require(tracker.update(canvas, obstacles), "Read window geometry");
        std::printf("Fluid Hose\nDisplay: %s\nDesktop: %d × %d points at (%d, %d)\nWindow tracking: %s\nWindow obstacles: %zu\n",
                    active_display.name.c_str(), canvas.w, canvas.h, canvas.x, canvas.y,
                    tracker.live() ? "live WindowServer events" : "polling", obstacles.size());
        if (probe) {
            for (const auto& rect : obstacles)
                std::printf("  Window %u: x=%.0f y=%.0f width=%.0f height=%.0f corner radius=%.0f\n",
                            rect.id, rect.x, rect.y, rect.width, rect.height, rect.radius);
            return 0;
        }

        app.renderer = require(create_hose_renderer(canvas, shaders.c_str(), continuous, tilt_enabled,
                                                    preferences.fluid, preferences.refraction), "Create fluid overlay");
        set_hose_displays(app.renderer, displays, active_display.id);
        if (display_number) save_hose_display(active_display);
        show_hose_window(app.renderer);
        std::printf("Backend: AppKit + native Metal\nDevice: %s\nHold Option to pour; use the droplet menu to clear or quit.\n",
                    hose_device_name(app.renderer));
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);

        FluidSimulation simulation;
        require(simulation.initialize(shaders.c_str(), static_cast<float>(canvas.w), static_cast<float>(canvas.h),
                                      preferences.fluid),
                "Create fluid simulation");
        TiltGravity tilt;
        bool requested_tilt = false;
        DesktopDisplayID pending_display = 0;
        using Clock = std::chrono::steady_clock;
        const auto start = Clock::now();
        auto previous = start;
        auto next_display_poll = start + std::chrono::seconds(1);
        float accumulator = 0;
        float time = 0;
        constexpr float step = FluidSimulation::step_time;
        run_hose_event_loop(app.renderer, [&]() {
            const bool displays_changed = take_hose_display_changes(app.renderer);
            const auto now = Clock::now();
            const float delta = std::min(std::chrono::duration<float>(now - previous).count(), 0.05f);
            previous = now;
            time += delta;
            auto input = poll_desktop_input(app.renderer, canvas);
            if (input.quit) return false;
            if (input.display) pending_display = input.display;
            if (displays_changed || input.display || now >= next_display_poll) {
                std::vector<DesktopDisplay> refreshed;
                if (read_desktop_displays(refreshed)) {
                    displays = std::move(refreshed);
                    DesktopDisplayID requested_display = active_display.id;
                    if (pending_display) {
                        const auto choice = std::find_if(displays.begin(), displays.end(),
                            [&](const auto& display) { return display.id == pending_display; });
                        if (choice != displays.end()) {
                            preferred_display = choice->uuid;
                            requested_display = choice->id;
                            save_hose_display(*choice);
                        }
                        pending_display = 0;
                    }
                    const auto selected = *choose_hose_display(displays, preferred_display, requested_display);
                    if (selected.id != active_display.id || !same_rect(selected.canvas, canvas)) {
                        move_hose_window(app.renderer, selected.canvas);
                        input.pointer.x += canvas.x - selected.canvas.x;
                        input.pointer.y += canvas.y - selected.canvas.y;
                        canvas = selected.canvas;
                        require(simulation.reset(static_cast<float>(canvas.w), static_cast<float>(canvas.h)),
                                "Resize fluid simulation");
                        accumulator = 0;
                        std::printf("Display: %s — %d × %d points at (%d, %d)\n", selected.name.c_str(),
                                    canvas.w, canvas.h, canvas.x, canvas.y);
                    }
                    active_display = selected;
                    set_hose_displays(app.renderer, displays, active_display.id);
                    next_display_poll = now + std::chrono::seconds(1);
                } else {
                    // Screen geometry can be temporarily unavailable during a hot-plug.
                    // Keep the current simulation until a complete list is available.
                    next_display_poll = now + std::chrono::milliseconds(250);
                }
            }
            require(tracker.update(canvas, obstacles), "Refresh window geometry");
            if (input.tilt != requested_tilt) {
                requested_tilt = input.tilt;
                tilt.reset();
                if (requested_tilt) app.motion.start(motion_helper);
                else app.motion.stop();
            }
            MotionPacket packet;
            if (requested_tilt && app.motion.poll(packet)) tilt.update(packet);
            const auto motion_time = motion_now();
            const bool fresh_tilt = requested_tilt && tilt.fresh(motion_time);
            set_hose_tilt_status(app.renderer, !requested_tilt ? TiltStatus::off :
                fresh_tilt ? TiltStatus::active : app.motion.running() ? TiltStatus::waiting : TiltStatus::unavailable);
            const FluidPoint gravity = requested_tilt ? tilt.gravity(motion_time, input.reverse_tilt) : FluidPoint{0, 950};
            if (input.fluid.size != preferences.fluid.size || input.fluid.feel != preferences.fluid.feel ||
                input.refraction != preferences.refraction) {
                preferences.fluid = input.fluid;
                preferences.refraction = input.refraction;
                simulation.configure(preferences.fluid);
                save_hose_preferences(preferences);
            }
            set_hose_refraction(app.renderer, preferences.refraction, active_display);
            if (input.clear) simulation.clear();
            const auto nozzle = place_hose(input.pointer, obstacles, static_cast<float>(canvas.w), static_cast<float>(canvas.h));
            const bool in_canvas = nozzle.x > 0 && nozzle.y > 0 && nozzle.x < canvas.w && nozzle.y < canvas.h;
            const bool pouring = input.pouring && in_canvas && !input.paused;
            if (!input.paused) {
                accumulator += delta;
                unsigned steps = 0;
                for (; accumulator >= step; accumulator -= step) ++steps;
                simulation.advance(steps, obstacles, pouring, nozzle, gravity);
            } else accumulator = 0;
            SceneUniforms scene{};
            scene.width = static_cast<float>(canvas.w);
            scene.height = static_cast<float>(canvas.h);
            scene.nozzle_x = nozzle.x;
            scene.nozzle_y = nozzle.y;
            scene.show_nozzle = pouring ? 1.0f : 0.0f;
            scene.time = time;
            bool presented = false;
            require(draw_hose_frame(app.renderer, simulation, scene, presented), "Draw fluid overlay");
            return true;
        }, [&]() {
            return stop_requested || (duration && std::chrono::duration<float>(Clock::now() - start).count() >= duration);
        });
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
