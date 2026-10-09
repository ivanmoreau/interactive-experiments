#pragma once
#include "fluid_types.h"
#include <cstdint>
#include <vector>

class FluidSimulation {
public:
    static constexpr std::size_t capacity = 8000;
    static constexpr float radius = 2.8f;
    FluidSimulation(float width, float height);
    void step(float dt, const std::vector<WindowRect>& windows, bool pouring, FluidPoint nozzle,
              FluidPoint gravity = {0, 950});
    void clear();
    FluidPoint place_hose(FluidPoint pointer, const std::vector<WindowRect>& windows) const;
    const std::vector<FluidVertex>& vertices() const { return render_vertices_; }
    std::size_t size() const { return particles_.size(); }

private:
    struct Particle { FluidPoint position; FluidPoint previous; FluidPoint velocity; };
    struct Neighbor { int index; FluidPoint delta; float distance; float q; };
    float width_;
    float height_;
    float emission_ = 0;
    std::uint32_t random_state_ = 0x4127a35u;
    std::vector<Particle> particles_;
    std::vector<FluidVertex> render_vertices_;
    std::vector<int> grid_heads_;
    std::vector<int> grid_next_;
    std::vector<FluidPoint> correction_;
    std::vector<Neighbor> neighbor_cache_;
    int grid_width_;
    int grid_height_;
    float random();
    void rebuild_grid();
    void collide(Particle& particle, const std::vector<WindowRect>& windows);
    void relax_density(float dt);
};

bool run_physics_checks();
