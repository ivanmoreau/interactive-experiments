#include "displays.h"
#include <cmath>
#include <cstdio>

const DesktopDisplay* choose_hose_display(const std::vector<DesktopDisplay>& displays,
                                        const std::string& preferred_uuid, DesktopDisplayID current) {
    if (!preferred_uuid.empty())
        for (const auto& display : displays)
            if (display.uuid == preferred_uuid) return &display;
    for (const auto& display : displays)
        if (display.id == current) return &display;
    for (const auto& display : displays)
        if (display.primary) return &display;
    return displays.empty() ? nullptr : &displays.front();
}

DesktopRect desktop_rect_from_appkit(double x, double y, double width, double height, double desktop_top) {
    return {static_cast<int>(std::lround(x)), static_cast<int>(std::lround(desktop_top - y - height)),
            static_cast<int>(std::lround(width)), static_cast<int>(std::lround(height))};
}

bool run_display_checks() {
    const DesktopDisplay laptop{1, "Laptop", "laptop", {0, 0, 1512, 982}, {0, 38, 1512, 900}, true};
    const DesktopDisplay external{2, "Monitor", "external", {-1920, -280, 1920, 1080}, {-1920, -256, 1920, 1056}, false};
    const std::vector<DesktopDisplay> both{laptop, external};
    bool passed = true;
    auto check = [&](bool value, const char* name) {
        std::printf("%s: %s\n", value ? "PASS" : "FAIL", name);
        passed = passed && value;
    };
    const auto* selected = choose_hose_display(both, "external");
    check(selected && selected->id == external.id && selected->canvas.x < 0 && selected->canvas.y < 0,
          "saved display selection supports monitors left of and above the main display");
    check(choose_hose_display(both, "")->id == laptop.id, "first launch defaults to the main display");
    const std::vector<DesktopDisplay> unplugged{laptop};
    check(choose_hose_display(unplugged, "external", external.id)->id == laptop.id,
          "disconnecting the selected monitor falls back to an available display");
    auto reconnected = both;
    reconnected[1].id = 42;
    check(choose_hose_display(reconnected, "external", laptop.id)->id == 42,
          "reconnecting the saved display works with a new native display ID");
    check(choose_hose_display(both, "missing", external.id)->id == external.id,
          "an unavailable preference preserves an available current display");
    check(!choose_hose_display({}, "external"), "no displays is reported without a stale selection");
    check(same_rect(desktop_rect_from_appkit(0, 0, 1512, 982, 982), laptop.bounds),
          "AppKit coordinates map the main display to the Quartz origin");
    check(same_rect(desktop_rect_from_appkit(0, 44, 1512, 900, 982), laptop.canvas),
          "usable bounds exclude the menu bar and Dock in top-left coordinates");
    check(same_rect(desktop_rect_from_appkit(-1920, 182, 1920, 1080, 982), external.bounds),
          "AppKit conversion preserves monitors left of and above the main display");
    check(same_rect(desktop_rect_from_appkit(227, -982, 1512, 982, 1440), {227, 1440, 1512, 982}),
          "a Retina screen below an external main display uses desktop points");
    return passed;
}
