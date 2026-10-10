#pragma once
#include <cstdint>
#include <string>
#include <vector>

using DesktopDisplayID = std::uint32_t;
// Global desktop points, with the origin at the main display's top-left.
struct DesktopRect { int x, y, w, h; };

struct DesktopDisplay {
    DesktopDisplayID id;
    std::string name;
    std::string uuid;
    DesktopRect bounds;
    DesktopRect canvas;
    bool primary;
};

inline bool same_rect(const DesktopRect& a, const DesktopRect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

inline bool operator==(const DesktopDisplay& a, const DesktopDisplay& b) {
    return a.id == b.id && a.name == b.name && a.uuid == b.uuid && a.primary == b.primary &&
           same_rect(a.bounds, b.bounds) && same_rect(a.canvas, b.canvas);
}

const DesktopDisplay* choose_hose_display(const std::vector<DesktopDisplay>& displays,
                                        const std::string& preferred_uuid, DesktopDisplayID current = 0);
DesktopRect desktop_rect_from_appkit(double x, double y, double width, double height, double desktop_top);
bool run_display_checks();
