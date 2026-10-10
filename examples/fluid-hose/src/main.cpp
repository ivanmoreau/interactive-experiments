#include "desktop.h"
#include "fluid_sim.h"
#include "motion.h"
#include "window_tracker.h"
#include "pond.h"
#include "brain_view.h"
#include "fish_brain.h"
#include "pond_store.h"
#include <random>
#include <mach-o/dyld.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <cstdlib>
#include <ctime>
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

// The measured hindbrain wiring: bundled in the app, or next to a plain build.
static std::shared_ptr<const HindbrainConnectome> load_hindbrain() {
    const auto directory = shader_path().parent_path();
    for (const auto& path : {directory / "hindbrain.txt", directory / "connectome" / "hindbrain.txt"})
        if (auto hindbrain = HindbrainConnectome::load(path.string())) return hindbrain;
    return nullptr;
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
        std::optional<float> flow_option;
        unsigned initial_ducks = 0;
        bool duck_check = false;
        unsigned initial_fish = 0;
        bool fish_check = false;
        std::string brain_snapshot;
        bool show_brain = false;
        bool new_pond = false;
        std::filesystem::path pond_directory = default_pond_directory();
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
            else if (argument == "--duck-check") duck_check = true;
            else if (argument == "--fish-check") fish_check = true;
            else if (argument == "--brain-snapshot" && i + 1 < argc) brain_snapshot = argv[++i];
            else if (argument == "--brain") show_brain = true;
            else if (argument == "--new-pond") new_pond = true;
            else if (argument == "--pond" && i + 1 < argc) pond_directory = argv[++i];
            else if (argument == "--fish" && i + 1 < argc) {
                const char* value = argv[++i];
                const auto result = std::from_chars(value, value + std::strlen(value), initial_fish);
                if (result.ec != std::errc{} || *result.ptr != '\0' || initial_fish > Pond::max_fish)
                    throw std::runtime_error("--fish requires a number of fish from 0 to 8");
            }
            else if (argument == "--ducks" && i + 1 < argc) {
                const char* value = argv[++i];
                const auto result = std::from_chars(value, value + std::strlen(value), initial_ducks);
                if (result.ec != std::errc{} || *result.ptr != '\0' || initial_ducks > Pond::max_ducks)
                    throw std::runtime_error("--ducks requires a number of ducks from 0 to 32");
            }
            else if (argument == "--flow" && i + 1 < argc) {
                char* end = nullptr;
                const float value = std::strtof(argv[++i], &end);
                if (!end || *end || !(value >= FluidSimulation::min_flow && value <= FluidSimulation::max_flow))
                    throw std::runtime_error("--flow must be a number from 0.25 to 3");
                flow_option = value;
            }
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
                          "                  [--size fine|medium|coarse] [--liquid water|thick|goo] [--refraction] [--flow RATE] [--ducks N] [--fish N] [--brain] [--new-pond] [--pond DIR]\n"
                          "                  "
                          "[--list-displays | --probe | --physics-check | --motion-check | --display-check | --smoke-test | --window-check |\n"
                          " --snapshot FILE.png | --brain-snapshot FILE.png | --capture-check | --duck-check | --fish-check]\n"
                          "Hold Option to pour at the mouse pointer. Release Option to stop.\n"
                          "The overlay lets clicks pass through: arrange windows normally.\n"
                          "Use the droplet menu in the menu bar to pour continuously, pause, clear, or quit.\n"
                          "Choose a monitor in the Display submenu; the pond moves with it.\n"
                          "The app remembers your selected display and handles display disconnection.\n"
                          "--tilt lets Mac motion change gravity; toggle Use Mac tilt in the droplet menu.\n"
                          "The sensor helper stops and restores its settings when tilt is off or the app exits.\n"
                          "Windows are tracked live as rounded rectangles; moving one pushes the water.\n"
                          "--size and --liquid choose particle size and how runny the liquid is; the droplet menu changes them too.\n"
                          "--ducks drops rubber ducks at launch; the droplet menu adds and removes them too.\n"
                          "--fish drops fish at launch. They need water to breathe and food to live, grow up when fed,\n"
                          "and spawn in the morning (7:00 to 7:45, or Lights on in the droplet menu); eggs hatch in minutes.\n"
                          "hold Control to sprinkle food at the pointer, or use Feed the fish in the droplet menu.\n"
                          "Show fish brain in the droplet menu (or --brain) opens a window of a fish's neurons firing.\n"
                          "--brain-snapshot draws that window for a hungry fish that sees food and is startled.\n"
                          "The pond (fish, ducks, and water) is saved in ~/Library/Application Support/Fluid Hose and comes\n"
                          "back next launch; --new-pond starts an empty one, and --pond keeps it in another folder.\n"
                          "--flow sets how fast the nozzle pours, from 0.25 to 3; Option+scroll changes it while running.\n"
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
        if (duck_check) {
            require(run_duck_checks(shaders.c_str()), "Check rubber ducks");
            return 0;
        }
        if (!brain_snapshot.empty()) {
            require(initialize_hose_desktop(), "Initialize AppKit");
            // A hungry fish watching food above it, startled at the end.
            FishBrain brain(7, load_hindbrain());
            FishSenses senses;
            senses.hunger = 0.7f;
            senses.prey[11] = 0.8f;
            senses.prey[12] = 0.4f;
            FishBout bout;
            for (int i = 0; i < 240; ++i) brain.advance(senses, 1.0f / 120, bout);
            senses.looming[1] = senses.looming[2] = 1.0f;
            for (int i = 0; i < 4; ++i) brain.advance(senses, 1.0f / 120, bout);
            require(BrainWindow::snapshot(brain, "Fish 1 of 1", "hungry · hunting food above · startled from below", brain_snapshot.c_str()),
                    "Draw the fish brain");
            std::printf("Brain snapshot: %zu neurons, %zu synapses -> %s\n", brain.neuron_count(), brain.synapse_count(),
                        brain_snapshot.c_str());
            return 0;
        }
        if (fish_check) {
            require(run_fish_checks(shaders.c_str(), load_hindbrain()), "Check fish");
            return 0;
        }
        // Command-line choices override the saved menu choices for this launch.
        FluidPreferences preferences = smoke || physics_check ? FluidPreferences{} : saved_hose_preferences();
        if (size_option) preferences.fluid.size = *size_option;
        if (liquid_option) preferences.fluid.feel = *liquid_option;
        if (refraction_option) preferences.refraction = true;
        if (flow_option) preferences.flow = *flow_option;
        if (!snapshot.empty()) {
            require(render_hose_snapshot(shaders.c_str(), snapshot.c_str(), duration ? static_cast<float>(duration) : 8.0f,
                                         preferences.fluid, refraction_option, preferences.flow, initial_ducks, initial_fish),
                    "Render fluid snapshot");
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
                                                    preferences.fluid, preferences.refraction, preferences.flow), "Create fluid overlay");
        set_hose_displays(app.renderer, displays, active_display.id);
        if (show_brain) set_hose_brain_shown(app.renderer, true);
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
        // Ducks and fish drop from random points along the top of the canvas.
        Pond pond;
        pond.reset(static_cast<float>(canvas.w), static_cast<float>(canvas.h));
        const auto hindbrain = load_hindbrain();
        pond.set_hindbrain(hindbrain);
        std::printf("Fish hindbrain: %s\n", hindbrain ? "measured wiring (Vishwanathan et al. 2024)" : "hand-wired only");
        std::mt19937 random(std::random_device{}());
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        // A drop point clear of windows, so a new creature is not squeezed out
        // of one against the top of the screen.
        auto drop_point = [&]() {
            FluidPoint point{(0.15f + 0.7f * unit(random)) * canvas.w, 40};
            for (int attempt = 0; attempt < 40; ++attempt) {
                const bool covered = std::any_of(obstacles.begin(), obstacles.end(), [&](const WindowObstacle& w) {
                    return point.x > w.x - 30 && point.x < w.x + w.width + 30 && point.y > w.y - 30 && point.y < w.y + w.height + 30;
                });
                if (!covered) break;
                point = {(0.05f + 0.9f * unit(random)) * canvas.w, 40 + unit(random) * (canvas.h * 0.5f)};
            }
            return point;
        };
        // A pinch of food lands above a fish, or mid-screen when there is none.
        auto feed = [&]() {
            FluidPoint above{canvas.w * 0.5f, 60};
            std::vector<FluidPoint> targets;
            for (const auto& fish : pond.fish_vitals())
                if (fish.alive) targets.push_back(fish.position);
            if (!targets.empty()) {
                const auto target = targets[static_cast<std::size_t>(unit(random) * targets.size()) % targets.size()];
                above = {target.x, std::max(20.0f, target.y - 260)};
            }
            for (int i = 0; i < 6; ++i) pond.add_food({above.x + (unit(random) - 0.5f) * 90, above.y + (unit(random) - 0.5f) * 20});
        };
        for (unsigned i = 0; i < initial_ducks; ++i) pond.add_duck(drop_point());
        // Bring back the saved pond, scaled to this display.
        auto bring_back = [&](const PondState& state, std::vector<FluidParticle> water) {
            const float sx = state.width > 0 ? canvas.w / state.width : 1, sy = state.height > 0 ? canvas.h / state.height : 1;
            for (auto& p : water) {
                p.x *= sx;
                p.y *= sy;
            }
            simulation.restore_particles(water);
            pond.restore(state);
        };
        if (!new_pond) {
            PondState saved;
            std::vector<FluidParticle> water;
            if (load_pond(pond_directory, saved, water)) {
                bring_back(saved, water);
                std::printf("Pond: %zu fish, %zu ducks, %zu water particles from %s\n", saved.fish.size(), saved.ducks.size(),
                            water.size(), pond_directory.c_str());
            }
        }
        auto save = [&]() {
            std::vector<FluidParticle> water;
            if (!simulation.read_particles(water) || !save_pond(pond_directory, pond.state(), water))
                std::fprintf(stderr, "Could not save the pond in %s\n", pond_directory.c_str());
        };
        auto next_save = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        for (unsigned i = 0; i < initial_fish; ++i) pond.add_fish(drop_point());
        float sprinkle_wait = 0;
        auto lights_until = std::chrono::steady_clock::time_point{};
        BrainWindow brain_window;
        bool brain_shown = false;
        std::size_t watched_fish = 0;
        float brain_wait = 0;
        std::vector<BodySample> body_water;
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
                        // The pond moves with the display, scaled to fit.
                        const PondState moving = pond.state();
                        std::vector<FluidParticle> water;
                        simulation.read_particles(water);
                        canvas = selected.canvas;
                        pond.reset(static_cast<float>(canvas.w), static_cast<float>(canvas.h));
                        require(simulation.reset(static_cast<float>(canvas.w), static_cast<float>(canvas.h)),
                                "Resize fluid simulation");
                        bring_back(moving, water);
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
            if (now >= next_save) {
                save();
                next_save = now + std::chrono::seconds(30);
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
            if (input.fluid.size != preferences.fluid.size || input.fluid.feel != preferences.fluid.feel ||
                input.refraction != preferences.refraction || input.flow != preferences.flow) {
                preferences.fluid = input.fluid;
                preferences.refraction = input.refraction;
                preferences.flow = input.flow;
                simulation.configure(preferences.fluid);
                save_hose_preferences(preferences);
            }
            simulation.set_flow_rate(preferences.flow);
            set_hose_refraction(app.renderer, preferences.refraction, active_display);
            if (input.clear) simulation.clear();
            if (input.remove_ducks) pond.clear_ducks();
            for (unsigned i = 0; i < input.add_ducks; ++i) pond.add_duck(drop_point());
            if (input.remove_fish) pond.clear_fish();
            for (unsigned i = 0; i < input.add_fish; ++i) pond.add_fish(drop_point());
            for (unsigned i = 0; i < input.feed; ++i) feed();
            const bool pointer_in_canvas = input.pointer.x > 0 && input.pointer.y > 0 && input.pointer.x < canvas.w &&
                                           input.pointer.y < canvas.h;
            sprinkle_wait -= delta;
            if (input.sprinkling && pointer_in_canvas && !input.paused && sprinkle_wait <= 0) {
                pond.add_food({input.pointer.x + (unit(random) - 0.5f) * 14, input.pointer.y + 6});
                sprinkle_wait = 0.12f;
            }
            pond.set_pointer(input.pointer);
            // Zebrafish spawn at first light: from 7:00 to 7:45 local time, or
            // for five minutes after Lights on.
            if (input.lights_on) lights_until = now + std::chrono::minutes(5);
            {
                const std::time_t clock = std::time(nullptr);
                std::tm local{};
                localtime_r(&clock, &local);
                const bool morning = local.tm_hour == 7 && local.tm_min < 45;
                pond.set_spawning(morning || now < lights_until ? 1.0f : 0.0f);
            }
            set_hose_fish_status(app.renderer, pond.fish_status());
            // The brain window follows the menu, and unticks it when closed.
            if (input.show_brain != brain_shown) {
                brain_shown = input.show_brain;
                if (brain_shown) brain_window.show();
                else brain_window.hide();
            } else if (brain_shown && !brain_window.visible()) {
                brain_shown = false;
                set_hose_brain_shown(app.renderer, false);
            }
            brain_wait -= delta;
            if (brain_shown) {
                watched_fish += brain_window.take_clicks();
                if (brain_wait <= 0) {
                    brain_wait = 1.0f / 30;
                    const auto vitals = pond.fish_vitals();
                    if (vitals.empty()) brain_window.update_empty("No fish yet. Add one from the droplet menu.");
                    else {
                        watched_fish %= vitals.size();
                        const auto& fish = vitals[watched_fish];
                        char status[160];
                        std::snprintf(status, sizeof(status), "%s · energy %.0f%% · air %.0f%% · %u flakes eaten%s",
                                      !fish.alive ? "dead" : !fish.in_water ? "out of water!" :
                                      fish.courting > 0.3f ? (fish.birth.female ? "courted" : "courting") :
                                      fish.hunting > 0.3f ? "hunting" : "exploring",
                                      fish.energy * 100, fish.oxygen * 100, fish.meals,
                                      fish.escapes ? (" · " + std::to_string(fish.escapes) + " escapes").c_str() : "");
                        const char* stages[] = {"larva", "juvenile", "adult"};
                        std::string title = fish.name + (fish.birth.female ? " ♀" : " ♂") + " · " +
                                            stages[static_cast<int>(fish.stage)] + " · generation " +
                                            std::to_string(fish.birth.generation);
                        // Names come from fish numbers, so parents keep theirs after they are gone.
                        if (fish.birth.mother)
                            title += " · child of " + fish_name(fish.birth.mother) + " and " + fish_name(fish.birth.father);
                        title += "   (" + std::to_string(watched_fish + 1) + " of " + std::to_string(vitals.size()) + ")";
                        brain_window.update(*pond.fish_brain(watched_fish), title, status);
                    }
                }
            }
            const auto nozzle = place_hose(input.pointer, obstacles, static_cast<float>(canvas.w), static_cast<float>(canvas.h));
            const bool in_canvas = nozzle.x > 0 && nozzle.y > 0 && nozzle.x < canvas.w && nozzle.y < canvas.h;
            const bool pouring = input.pouring && in_canvas && !input.paused;
            if (!input.paused) {
                accumulator += delta;
                unsigned steps = 0;
                for (; accumulator >= step; accumulator -= step) ++steps;
                // Jolt steps the ducks and fish with the latest water
                // measurement, then the fluid steps with their new positions.
                // Windows keep following the tracker even with an empty pond,
                // so a newly dropped duck is not swept by stale window positions.
                if (steps) {
                    pond.set_windows(obstacles);
                    if (!simulation.latest_body_samples(body_water)) body_water.clear();
                    for (unsigned k = 0; k < steps; ++k)
                        pond.step(step, static_cast<float>(k + 1) / steps, gravity, body_water, simulation.particle_area());
                }
                simulation.set_bodies(pond.bodies());
                simulation.advance(steps, obstacles, pouring, nozzle, gravity);
            } else accumulator = 0;
            SceneUniforms scene{};
            scene.width = static_cast<float>(canvas.w);
            scene.height = static_cast<float>(canvas.h);
            scene.nozzle_x = nozzle.x;
            scene.nozzle_y = nozzle.y;
            scene.show_nozzle = pouring ? 1.0f : 0.0f;
            scene.flow = preferences.flow;
            scene.time = time;
            bool presented = false;
            require(draw_hose_frame(app.renderer, simulation, pond.bodies(), scene, presented), "Draw fluid overlay");
            return true;
        }, [&]() {
            return stop_requested || (duration && std::chrono::duration<float>(Clock::now() - start).count() >= duration);
        });
        save();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
