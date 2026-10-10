#include "displays.h"
#include <cstdio>

const DesktopDisplay* choose_hose_display(const std::vector<DesktopDisplay>& displays,
                                        const std::string& preferred_uuid, SDL_DisplayID current) {
    if (!preferred_uuid.empty())
        for (const auto& display : displays)
            if (display.uuid == preferred_uuid) return &display;
    for (const auto& display : displays)
        if (display.id == current) return &display;
    for (const auto& display : displays)
        if (display.primary) return &display;
    return displays.empty() ? nullptr : &displays.front();
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
          "reconnecting the saved display works with a new SDL display ID");
    check(choose_hose_display(both, "missing", external.id)->id == external.id,
          "an unavailable preference preserves an available current display");
    check(!choose_hose_display({}, "external"), "no displays is reported without a stale selection");
    return passed;
}
