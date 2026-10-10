#pragma once
#include "displays.h"
#include "fluid_types.h"
#include <memory>
#include <vector>

// Tracks on-screen windows as fluid colliders. With WindowServer's private
// SkyLight notifications, moves and resizes arrive as they happen and only the
// changed window is re-read; the full window list is re-read when windows
// appear, disappear, reorder, or change spaces. Without SkyLight it polls.
class WindowTracker {
public:
    WindowTracker();
    ~WindowTracker();
    WindowTracker(const WindowTracker&) = delete;
    WindowTracker& operator=(const WindowTracker&) = delete;

    bool live() const;
    // Fills colliders in canvas coordinates. Returns false when the desktop
    // session's window list is unavailable.
    bool update(const DesktopRect& canvas, std::vector<WindowObstacle>& output);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
