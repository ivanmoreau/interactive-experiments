#pragma once
#include "fluid_types.h"
#include <cstddef>
#include <memory>
#include <vector>

// Rubber ducks simulated with Jolt Physics, locked to the screen plane. Windows
// are kinematic bodies that follow the window tracker, the screen edges are
// walls, and the water measured on the GPU supplies buoyancy and drag.
class DuckWorld {
public:
    static constexpr std::size_t max_ducks = 32;

    DuckWorld();
    ~DuckWorld();
    DuckWorld(const DuckWorld&) = delete;
    DuckWorld& operator=(const DuckWorld&) = delete;

    // Rebuilds the screen walls for a new canvas and removes ducks and windows.
    void reset(float width, float height);
    // Drops a duck with its centre at position. Returns false when full.
    bool add_duck(FluidPoint position);
    void clear_ducks();
    std::size_t size() const;

    // Sets the windows' positions for this frame; steps move them there
    // gradually from where the previous frame left them.
    void set_windows(const std::vector<WindowObstacle>& windows);
    // Advances one fixed step. progress is how far through the frame's window
    // motion this step ends (0 to 1). water holds the latest GPU measurement
    // for each duck, and particle_area the water area of one particle.
    void step(float dt, float progress, FluidPoint gravity, const std::vector<DuckSample>& water, float particle_area);

    const std::vector<DuckBody>& bodies() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

bool run_duck_checks(const char* shader_file);
