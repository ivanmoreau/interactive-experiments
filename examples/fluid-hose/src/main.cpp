#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_events.h>
#include "desktop.h"
#include "fluid_sim.h"
#include "motion.h"
#include <mach-o/dyld.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static volatile std::sig_atomic_t stop_requested = 0;
static void stop(int) { stop_requested = 1; }

static void require(bool success, const char* operation) {
    if (!success) throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
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
    return directory / "fluid.metallib";
}

struct App {
    SDL_Window* window = nullptr;
    HoseRenderer* renderer = nullptr;
    MotionProcess motion;
    ~App() {
        motion.stop();
        if (renderer) destroy_hose_renderer(renderer);
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
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
        unsigned display_number = 0;
        unsigned duration = 0;
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
                std::puts("Fluid Hose\nUsage: fluid-hose [--pour] [--tilt] [--display NUMBER] [--duration SECONDS] "
                          "[--list-displays | --probe | --physics-check | --motion-check | --display-check | --smoke-test]\n"
                          "Hold Option to pour at the mouse pointer. Release Option to stop.\n"
                          "The overlay lets clicks pass through: arrange windows normally.\n"
                          "Use the droplet menu in the menu bar to pour continuously, pause, clear, or quit.\n"
                          "Choose a monitor in the Display submenu; switching clears the water.\n"
                          "The app remembers your selected display and handles display disconnection.\n"
                          "--tilt lets Mac motion change gravity; toggle Use Mac tilt in the droplet menu.\n"
                          "The sensor helper stops and restores its settings when tilt is off or the app exits.\n"
                          "This prototype uses one selected display and ordinary window rectangles.");
                return 0;
            } else throw std::runtime_error("Unknown or incomplete option: " + argument);
        }
        if (physics_check) return run_physics_checks() ? 0 : 1;
        if (display_check) return run_display_checks() ? 0 : 1;
        const auto motion_helper = (executable_directory() / "probe-motion").string();
        if (motion_check) return run_motion_checks(motion_helper) ? 0 : 1;
        SDL_SetMainReady();
        App app;
        require(SDL_Init(smoke ? 0 : SDL_INIT_VIDEO), "Initialize SDL3");
        const auto shaders = shader_path().string();
        if (smoke) {
            require(smoke_test_hose_renderer(shaders.c_str()), "Check fluid renderer");
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
        SDL_Rect canvas = active_display.canvas;
        std::vector<WindowRect> obstacles;
        require(read_window_obstacles(canvas, obstacles), "Read window geometry");
        std::printf("Fluid Hose\nDisplay: %s\nDesktop: %d × %d points at (%d, %d)\nWindow obstacles: %zu\n",
                    active_display.name.c_str(), canvas.w, canvas.h, canvas.x, canvas.y, obstacles.size());
        if (probe) {
            for (const auto& rect : obstacles)
                std::printf("  Rectangle: x=%.0f y=%.0f width=%.0f height=%.0f\n", rect.x, rect.y, rect.width, rect.height);
            return 0;
        }

        const SDL_WindowFlags flags = SDL_WINDOW_BORDERLESS | SDL_WINDOW_TRANSPARENT | SDL_WINDOW_ALWAYS_ON_TOP |
                                      SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_METAL | SDL_WINDOW_NOT_FOCUSABLE | SDL_WINDOW_HIDDEN;
        app.window = require(SDL_CreateWindow("Fluid Hose", canvas.w, canvas.h, flags), "Create fluid overlay");
        require(SDL_SetWindowPosition(app.window, canvas.x, canvas.y), "Position fluid overlay");
        require(SDL_SyncWindow(app.window), "Wait for fluid overlay position");
        // SDL GPU rejects transparent windows. This example uses SDL's supported
        // Metal-view API and native Metal rendering, as in the working triangle.
        app.renderer = require(create_hose_renderer(app.window, shaders.c_str(), continuous, tilt_enabled), "Create fluid renderer");
        set_hose_displays(app.renderer, displays, active_display.id);
        if (display_number) save_hose_display(active_display);
        require(SDL_ShowWindow(app.window), "Show fluid overlay");
        std::printf("Device: %s\nHold Option to pour; use the droplet menu to clear or quit.\n", hose_device_name(app.renderer));
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);

        FluidSimulation simulation(static_cast<float>(canvas.w), static_cast<float>(canvas.h));
        TiltGravity tilt;
        bool requested_tilt = false;
        SDL_DisplayID pending_display = 0;
        using Clock = std::chrono::steady_clock;
        const auto start = Clock::now();
        auto previous = start;
        auto next_window_poll = start;
        auto next_display_poll = start + std::chrono::seconds(1);
        float accumulator = 0;
        float time = 0;
        constexpr float step = 1.0f / 120.0f;
        bool running = true;
        while (running && !stop_requested) {
            bool displays_changed = false;
            SDL_Event event{};
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT) running = false;
                if (event.type >= SDL_EVENT_DISPLAY_FIRST && event.type <= SDL_EVENT_DISPLAY_LAST) displays_changed = true;
            }
            if (!running) break;
            const auto now = Clock::now();
            const float delta = std::min(std::chrono::duration<float>(now - previous).count(), 0.05f);
            previous = now;
            time += delta;
            if (duration && std::chrono::duration<float>(now - start).count() >= duration) break;
            auto input = poll_desktop_input(app.renderer, canvas);
            if (input.quit) break;
            if (input.display) pending_display = input.display;
            if (displays_changed || input.display || now >= next_display_poll) {
                std::vector<DesktopDisplay> refreshed;
                if (read_desktop_displays(refreshed)) {
                    displays = std::move(refreshed);
                    SDL_DisplayID requested_display = active_display.id;
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
                        require(SDL_HideWindow(app.window), "Hide fluid overlay while switching displays");
                        require(SDL_SetWindowSize(app.window, selected.canvas.w, selected.canvas.h), "Resize fluid overlay");
                        require(SDL_SetWindowPosition(app.window, selected.canvas.x, selected.canvas.y), "Move fluid overlay to display");
                        require(SDL_SyncWindow(app.window), "Wait for display switch");
                        input.pointer.x += canvas.x - selected.canvas.x;
                        input.pointer.y += canvas.y - selected.canvas.y;
                        canvas = selected.canvas;
                        simulation = FluidSimulation(static_cast<float>(canvas.w), static_cast<float>(canvas.h));
                        accumulator = 0;
                        next_window_poll = now;
                        require(SDL_ShowWindow(app.window), "Show fluid overlay on selected display");
                        std::printf("Display: %s — %d × %d points at (%d, %d)\n", selected.name.c_str(),
                                    canvas.w, canvas.h, canvas.x, canvas.y);
                    }
                    active_display = selected;
                    set_hose_displays(app.renderer, displays, active_display.id);
                    next_display_poll = now + std::chrono::seconds(1);
                } else {
                    // NSScreen and SDL can briefly disagree during a hot-plug.
                    // Keep the current simulation until a complete list is available.
                    next_display_poll = now + std::chrono::milliseconds(250);
                }
            }
            if (now >= next_window_poll) {
                require(read_window_obstacles(canvas, obstacles), "Refresh window geometry");
                next_window_poll = now + std::chrono::milliseconds(125);
            }
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
            if (input.clear) simulation.clear();
            const auto nozzle = simulation.place_hose(input.pointer, obstacles);
            const bool in_canvas = nozzle.x > 0 && nozzle.y > 0 && nozzle.x < canvas.w && nozzle.y < canvas.h;
            const bool pouring = input.pouring && in_canvas && !input.paused;
            if (!input.paused) {
                accumulator += delta;
                while (accumulator >= step) {
                    simulation.step(step, obstacles, pouring, nozzle, gravity);
                    accumulator -= step;
                }
            } else accumulator = 0;
            const SceneUniforms scene{static_cast<float>(canvas.w), static_cast<float>(canvas.h),
                                      nozzle.x, nozzle.y, pouring ? 1.0f : 0.0f, time, 0, 0};
            bool presented = false;
            require(draw_hose_frame(app.renderer, simulation.vertices(), scene, presented), "Draw fluid overlay");
            if (!presented) std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
