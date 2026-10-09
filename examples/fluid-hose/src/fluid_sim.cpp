#include "fluid_sim.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
constexpr float support = 12.0f;
constexpr float rest_density = 1.65f;
constexpr float stiffness = 1700.0f;
constexpr float near_stiffness = 2800.0f;

bool inside(FluidPoint point, const WindowRect& rect) {
    return point.x > rect.x - FluidSimulation::radius && point.x < rect.x + rect.width + FluidSimulation::radius &&
           point.y > rect.y - FluidSimulation::radius && point.y < rect.y + rect.height + FluidSimulation::radius;
}
}

FluidSimulation::FluidSimulation(float width, float height)
    : width_(width), height_(height), grid_width_(static_cast<int>(width / support) + 1),
      grid_height_(static_cast<int>(height / support) + 1) {
    particles_.reserve(capacity);
    render_vertices_.reserve(capacity);
    grid_next_.reserve(capacity);
    correction_.reserve(capacity);
    grid_heads_.resize(grid_width_ * grid_height_, -1);
}

float FluidSimulation::random() {
    random_state_ ^= random_state_ << 13;
    random_state_ ^= random_state_ >> 17;
    random_state_ ^= random_state_ << 5;
    return static_cast<float>(random_state_ & 0xffffffu) / 16777216.0f;
}

void FluidSimulation::clear() {
    particles_.clear();
    render_vertices_.clear();
    emission_ = 0;
}

FluidPoint FluidSimulation::place_hose(FluidPoint pointer, const std::vector<WindowRect>& windows) const {
    auto free = [&](FluidPoint point) {
        if (point.x < 8 || point.y < 8 || point.x > width_ - 8 || point.y > height_ - 8) return false;
        return std::none_of(windows.begin(), windows.end(), [&](const auto& rect) { return inside(point, rect); });
    };
    if (free(pointer)) return pointer;
    if (pointer.x < 0 || pointer.y < 0 || pointer.x > width_ || pointer.y > height_) return {-100, -100};
    FluidPoint best{-100, -100};
    float best_distance = 1e20f;
    for (const auto& rect : windows) {
        const FluidPoint candidates[8] = {
            {pointer.x, rect.y - 12}, {pointer.x, rect.y + rect.height + 12},
            {rect.x - 12, pointer.y}, {rect.x + rect.width + 12, pointer.y},
            {rect.x - 12, rect.y - 12}, {rect.x + rect.width + 12, rect.y - 12},
            {rect.x - 12, rect.y + rect.height + 12}, {rect.x + rect.width + 12, rect.y + rect.height + 12}
        };
        for (const auto candidate : candidates) {
            const float dx = candidate.x - pointer.x;
            const float dy = candidate.y - pointer.y;
            const float distance = dx * dx + dy * dy;
            if (distance < best_distance && free(candidate)) { best = candidate; best_distance = distance; }
        }
    }
    return best;
}

void FluidSimulation::collide(Particle& particle, const std::vector<WindowRect>& windows) {
    for (int pass = 0; pass < 4; ++pass) {
        bool moved = false;
        for (const auto& rect : windows) {
            if (!inside(particle.position, rect)) continue;
            const float left = rect.x - radius;
            const float right = rect.x + rect.width + radius;
            const float top = rect.y - radius;
            const float bottom = rect.y + rect.height + radius;
            const float distances[4] = {particle.position.x - left, right - particle.position.x,
                                        particle.position.y - top, bottom - particle.position.y};
            int side = static_cast<int>(std::min_element(distances, distances + 4) - distances);
            // Prefer the face crossed this step; nearest-face projection also
            // pushes water out when a window is moved into an existing pool.
            if (particle.previous.y <= top) side = 2;
            else if (particle.previous.y >= bottom) side = 3;
            else if (particle.previous.x <= left) side = 0;
            else if (particle.previous.x >= right) side = 1;
            switch (side) {
                case 0: particle.position.x = left; break;
                case 1: particle.position.x = right; break;
                case 2: particle.position.y = top; break;
                case 3: particle.position.y = bottom; break;
            }
            moved = true;
        }
        particle.position.x = std::clamp(particle.position.x, radius, width_ - radius);
        particle.position.y = std::clamp(particle.position.y, radius, height_ - radius);
        if (!moved) break;
    }
    if (std::none_of(windows.begin(), windows.end(), [&](const auto& rect) { return inside(particle.position, rect); })) return;

    // Overlapping rectangles can otherwise project a particle back and forth.
    // Find exits from their connected horizontal/vertical cross sections.
    const FluidPoint point = particle.position;
    float left = point.x, right = point.x, top = point.y, bottom = point.y;
    for (std::size_t pass = 0; pass < windows.size(); ++pass) {
        bool changed = false;
        for (const auto& rect : windows) {
            const float l = rect.x - radius, r = rect.x + rect.width + radius;
            const float t = rect.y - radius, b = rect.y + rect.height + radius;
            if (point.y > t && point.y < b && l <= right && r >= left) {
                const float next_l = std::min(left, l), next_r = std::max(right, r);
                changed |= next_l != left || next_r != right;
                left = next_l; right = next_r;
            }
            if (point.x > l && point.x < r && t <= bottom && b >= top) {
                const float next_t = std::min(top, t), next_b = std::max(bottom, b);
                changed |= next_t != top || next_b != bottom;
                top = next_t; bottom = next_b;
            }
        }
        if (!changed) break;
    }
    FluidPoint best = point;
    float best_distance = 1e20f;
    auto candidate = [&](FluidPoint p) {
        if (p.x < radius || p.x > width_ - radius || p.y < radius || p.y > height_ - radius) return;
        if (std::any_of(windows.begin(), windows.end(), [&](const auto& rect) { return inside(p, rect); })) return;
        const float dx = p.x - point.x, dy = p.y - point.y;
        const float distance = dx * dx + dy * dy;
        if (distance < best_distance) { best = p; best_distance = distance; }
    };
    candidate({left - 0.001f, point.y}); candidate({right + 0.001f, point.y});
    candidate({point.x, top - 0.001f}); candidate({point.x, bottom + 0.001f});
    // Clipped corners also handle a cross-shaped union spanning the screen.
    for (const auto& rect : windows) {
        for (const float x : {rect.x - radius - 0.001f, rect.x + rect.width + radius + 0.001f}) {
            for (const float y : {rect.y - radius - 0.001f, rect.y + rect.height + radius + 0.001f})
                candidate({std::clamp(x, radius, width_ - radius), std::clamp(y, radius, height_ - radius)});
        }
    }
    particle.position = best;
}

void FluidSimulation::rebuild_grid() {
    std::fill(grid_heads_.begin(), grid_heads_.end(), -1);
    grid_next_.resize(particles_.size());
    for (std::size_t i = 0; i < particles_.size(); ++i) {
        const auto p = particles_[i].position;
        const int x = std::clamp(static_cast<int>(p.x / support), 0, grid_width_ - 1);
        const int y = std::clamp(static_cast<int>(p.y / support), 0, grid_height_ - 1);
        const int cell = y * grid_width_ + x;
        grid_next_[i] = grid_heads_[cell];
        grid_heads_[cell] = static_cast<int>(i);
    }
}

void FluidSimulation::relax_density(float dt) {
    rebuild_grid();
    correction_.assign(particles_.size(), {0, 0});
    for (std::size_t i = 0; i < particles_.size(); ++i) {
        const FluidPoint p = particles_[i].position;
        const int cell_x = static_cast<int>(p.x / support);
        const int cell_y = static_cast<int>(p.y / support);
        float density = 0;
        float near_density = 0;
        auto neighbors = [&](auto action) {
            for (int y = std::max(0, cell_y - 1); y <= std::min(grid_height_ - 1, cell_y + 1); ++y) {
                for (int x = std::max(0, cell_x - 1); x <= std::min(grid_width_ - 1, cell_x + 1); ++x) {
                    for (int j = grid_heads_[y * grid_width_ + x]; j != -1; j = grid_next_[j]) {
                        if (j == static_cast<int>(i)) continue;
                        FluidPoint delta{particles_[j].position.x - p.x, particles_[j].position.y - p.y};
                        float squared = delta.x * delta.x + delta.y * delta.y;
                        if (squared < 0.0001f) {
                            const auto low = std::min(i, static_cast<std::size_t>(j));
                            const auto high = std::max(i, static_cast<std::size_t>(j));
                            const auto hash = (low + 1) * 73856093u ^ (high + 1) * 19349663u;
                            const float sign = j > static_cast<int>(i) ? 1.0f : -1.0f;
                            delta = (hash & 1) ? FluidPoint{sign * 0.01f, 0} : FluidPoint{0, sign * 0.01f};
                            squared = 0.0001f;
                        }
                        if (squared < support * support) {
                            const float distance = std::sqrt(squared);
                            action(j, delta, distance, 1.0f - distance / support);
                        }
                    }
                }
            }
        };
        neighbors([&](int, FluidPoint, float, float q) {
            density += q * q;
            near_density += q * q * q;
        });
        const float pressure = std::clamp(stiffness * (density - rest_density), -350.0f, 7000.0f);
        const float near_pressure = std::min(near_stiffness * near_density, 9000.0f);
        neighbors([&](int j, FluidPoint delta, float distance, float q) {
            const float amount = std::clamp(dt * dt * (pressure * q + near_pressure * q * q), -0.12f, 0.55f);
            const FluidPoint shift{delta.x / distance * amount * 0.5f, delta.y / distance * amount * 0.5f};
            correction_[j].x += shift.x;
            correction_[j].y += shift.y;
            correction_[i].x -= shift.x;
            correction_[i].y -= shift.y;
        });
    }
    for (std::size_t i = 0; i < particles_.size(); ++i) {
        particles_[i].position.x += std::clamp(correction_[i].x, -2.0f, 2.0f);
        particles_[i].position.y += std::clamp(correction_[i].y, -2.0f, 2.0f);
    }
}

void FluidSimulation::step(float dt, const std::vector<WindowRect>& windows, bool pouring, FluidPoint nozzle) {
    if (pouring && nozzle.x > radius && nozzle.x < width_ - radius && nozzle.y > radius && nozzle.y < height_ - radius) {
        emission_ += dt * 200.0f;
        while (emission_ >= 1.0f && particles_.size() < capacity) {
            emission_ -= 1.0f;
            const FluidPoint p{nozzle.x + (random() - 0.5f) * 7.0f, nozzle.y + 4.0f};
            particles_.push_back({p, p, {(random() - 0.5f) * 30.0f, 90.0f + random() * 40.0f}});
        }
        emission_ = std::min(emission_, 1.0f);
    } else emission_ = 0;

    for (auto& particle : particles_) {
        particle.previous = particle.position;
        particle.velocity.y += 950.0f * dt;
        particle.position.x += particle.velocity.x * dt;
        particle.position.y += particle.velocity.y * dt;
        collide(particle, windows);
    }
    for (int iteration = 0; iteration < 3; ++iteration) {
        relax_density(dt);
        for (auto& particle : particles_) collide(particle, windows);
    }
    render_vertices_.resize(particles_.size());
    for (std::size_t i = 0; i < particles_.size(); ++i) {
        auto& particle = particles_[i];
        particle.velocity.x = std::clamp((particle.position.x - particle.previous.x) / dt * 0.996f, -650.0f, 650.0f);
        particle.velocity.y = std::clamp((particle.position.y - particle.previous.y) / dt * 0.998f, -900.0f, 900.0f);
        render_vertices_[i] = {particle.position.x, particle.position.y, radius * 2.7f, 0};
    }
}

bool run_physics_checks() {
    constexpr float dt = 1.0f / 120.0f;
    FluidSimulation falling(640, 480);
    for (int i = 0; i < 360; ++i) falling.step(dt, {}, i < 24, {80, 40});
    if (falling.size() == 0) return false;
    for (const auto& p : falling.vertices()) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || p.y < 450 || p.y > 480) return false;
    }
    std::puts("PASS: gravity and pooling at the desktop floor.");

    FluidSimulation collision(640, 480);
    std::vector<WindowRect> windows{{200, 180, 240, 160}};
    for (int i = 0; i < 480; ++i) {
        collision.step(dt, windows, i < 240, {320, 50});
        for (const auto& p : collision.vertices()) {
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || inside({p.x, p.y}, windows[0])) return false;
        }
    }
    std::puts("PASS: water stays outside the window rectangle.");
    const auto nozzle = collision.place_hose({320, 200}, windows);
    if (inside(nozzle, windows[0]) || nozzle.y >= windows[0].y) return false;
    std::puts("PASS: the hose snaps outside solid window bodies.");
    windows[0] = {250, 250, 200, 170};
    collision.step(dt, windows, false, {0, 0});
    for (const auto& p : collision.vertices()) {
        if (inside({p.x, p.y}, windows[0])) return false;
    }
    std::puts("PASS: moving a window pushes overlapping water out.");
    FluidSimulation overlap(800, 600);
    for (int i = 0; i < 360; ++i) overlap.step(dt, {}, i < 60, {400, 50});
    const std::vector<WindowRect> moved_windows{{100, 100, 400, 600}, {350, 100, 300, 600}};
    overlap.step(dt, moved_windows, false, {0, 0});
    for (const auto& p : overlap.vertices()) {
        for (const auto& rect : moved_windows) if (inside({p.x, p.y}, rect)) return false;
    }
    std::puts("PASS: water is displaced outside overlapping windows.");
    collision.clear();
    if (collision.size() != 0 || !collision.vertices().empty()) return false;
    std::puts("PASS: clearing the water resets the simulation.");
    return true;
}
