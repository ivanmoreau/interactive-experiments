#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_events.h>
#include "desktop.h"
#include "fluid_sim.h"
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

static std::filesystem::path shader_path() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> path(size);
    if (_NSGetExecutablePath(path.data(), &size) != 0) throw std::runtime_error("Cannot locate executable");
    auto directory = std::filesystem::canonical(path.data()).parent_path();
    if (directory.filename() == "MacOS") directory = directory.parent_path() / "Resources";
    return directory / "fluid.metallib";
}

struct App {
    SDL_Window* window = nullptr;
    HoseRenderer* renderer = nullptr;
    ~App() {
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
        unsigned duration = 0;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--physics-check") physics_check = true;
            else if (argument == "--smoke-test") smoke = true;
            else if (argument == "--probe") probe = true;
            else if (argument == "--pour") continuous = true;
            else if (argument == "--duration" && i + 1 < argc) {
                const char* value = argv[++i];
                const auto result = std::from_chars(value, value + std::strlen(value), duration);
                if (result.ec != std::errc{} || *result.ptr != '\0' || duration == 0)
                    throw std::runtime_error("--duration requires a positive integer number of seconds");
            } else if (argument == "--help") {
                std::puts("Fluid Hose\nUsage: fluid-hose [--pour] [--duration SECONDS] "
                          "[--probe | --physics-check | --smoke-test]\n"
                          "Hold Option to pour at the mouse pointer. Release Option to stop.\n"
                          "The overlay lets clicks pass through: arrange windows normally.\n"
                          "Use the droplet menu in the menu bar to pour continuously, pause, clear, or quit.\n"
                          "This prototype uses the primary display and ordinary window rectangles.");
                return 0;
            } else throw std::runtime_error("Unknown or incomplete option: " + argument);
        }
        if (physics_check) return run_physics_checks() ? 0 : 1;
        SDL_SetMainReady();
        App app;
        require(SDL_Init(smoke ? 0 : SDL_INIT_VIDEO), "Initialize SDL3");
        const auto shaders = shader_path().string();
        if (smoke) {
            require(smoke_test_hose_renderer(shaders.c_str()), "Check fluid renderer");
            return 0;
        }

        SDL_Rect canvas{};
        const auto display = SDL_GetPrimaryDisplay();
        if (!display) throw std::runtime_error("No primary display is available");
        require(SDL_GetDisplayUsableBounds(display, &canvas), "Read desktop bounds");
        if (canvas.w < 64 || canvas.h < 64) throw std::runtime_error("The desktop is too small for this example");
        std::vector<WindowRect> obstacles;
        require(read_window_obstacles(canvas, obstacles), "Read window geometry");
        std::printf("Fluid Hose\nDesktop: %d × %d points at (%d, %d)\nWindow obstacles: %zu\n",
                    canvas.w, canvas.h, canvas.x, canvas.y, obstacles.size());
        if (probe) {
            for (const auto& rect : obstacles)
                std::printf("  Rectangle: x=%.0f y=%.0f width=%.0f height=%.0f\n", rect.x, rect.y, rect.width, rect.height);
            return 0;
        }

        const SDL_WindowFlags flags = SDL_WINDOW_BORDERLESS | SDL_WINDOW_TRANSPARENT | SDL_WINDOW_ALWAYS_ON_TOP |
                                      SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_METAL | SDL_WINDOW_NOT_FOCUSABLE;
        app.window = require(SDL_CreateWindow("Fluid Hose", canvas.w, canvas.h, flags), "Create fluid overlay");
        require(SDL_SetWindowPosition(app.window, canvas.x, canvas.y), "Position fluid overlay");
        // SDL GPU rejects transparent windows. This example uses SDL's supported
        // Metal-view API and native Metal rendering, as in the working triangle.
        app.renderer = require(create_hose_renderer(app.window, shaders.c_str(), continuous), "Create fluid renderer");
        std::printf("Device: %s\nHold Option to pour; use the droplet menu to clear or quit.\n", hose_device_name(app.renderer));
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);

        FluidSimulation simulation(static_cast<float>(canvas.w), static_cast<float>(canvas.h));
        using Clock = std::chrono::steady_clock;
        const auto start = Clock::now();
        auto previous = start;
        auto next_window_poll = start;
        float accumulator = 0;
        float time = 0;
        constexpr float step = 1.0f / 120.0f;
        bool running = true;
        while (running && !stop_requested) {
            SDL_Event event{};
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT) running = false;
            }
            if (!running) break;
            const auto now = Clock::now();
            const float delta = std::min(std::chrono::duration<float>(now - previous).count(), 0.05f);
            previous = now;
            time += delta;
            if (duration && std::chrono::duration<float>(now - start).count() >= duration) break;
            if (now >= next_window_poll) {
                require(read_window_obstacles(canvas, obstacles), "Refresh window geometry");
                next_window_poll = now + std::chrono::milliseconds(125);
            }
            const auto input = poll_desktop_input(app.renderer, canvas);
            if (input.quit) break;
            if (input.clear) simulation.clear();
            const auto nozzle = simulation.place_hose(input.pointer, obstacles);
            const bool in_canvas = nozzle.x > 0 && nozzle.y > 0 && nozzle.x < canvas.w && nozzle.y < canvas.h;
            const bool pouring = input.pouring && in_canvas && !input.paused;
            if (!input.paused) {
                accumulator += delta;
                while (accumulator >= step) {
                    simulation.step(step, obstacles, pouring, nozzle);
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
