#pragma once
#include <SDL3/SDL_video.h>
#include <string>
#include <vector>

struct DesktopDisplay {
    SDL_DisplayID id;
    std::string name;
    std::string uuid;
    SDL_Rect bounds;
    SDL_Rect canvas;
    bool primary;
};

inline bool same_rect(const SDL_Rect& a, const SDL_Rect& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

inline bool operator==(const DesktopDisplay& a, const DesktopDisplay& b) {
    return a.id == b.id && a.name == b.name && a.uuid == b.uuid && a.primary == b.primary &&
           same_rect(a.bounds, b.bounds) && same_rect(a.canvas, b.canvas);
}

const DesktopDisplay* choose_hose_display(const std::vector<DesktopDisplay>& displays,
                                        const std::string& preferred_uuid, SDL_DisplayID current = 0);
bool run_display_checks();
