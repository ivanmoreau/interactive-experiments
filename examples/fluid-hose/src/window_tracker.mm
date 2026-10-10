#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include "window_tracker.h"
#include "desktop.h"
#include "fluid_sim.h"
#include <dlfcn.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <unordered_set>

namespace {
// WindowServer notification types, as used by JankyBorders and yabai.
enum : std::uint32_t {
    event_window_close = 804,
    event_window_move = 806,
    event_window_resize = 807,
    event_window_reorder = 808,
    event_window_level = 811,
    event_window_unhide = 815,
    event_window_hide = 816,
    event_window_create = 1325,
    event_window_destroy = 1326,
    event_space_change = 1401,
    event_front_change = 1508,
};

// Standard macOS document window corner radius before macOS 26 exposes it.
constexpr float default_corner_radius = 10.0f;

using Clock = std::chrono::steady_clock;
using NotifyHandler = void (*)(std::uint32_t, void*, std::size_t, void*);

// SkyLight is private, so every entry point is resolved at run time and the
// tracker falls back to polling if any required symbol is missing.
struct SkyLight {
    int (*main_connection)() = nullptr;
    CGError (*register_notify)(NotifyHandler, std::uint32_t, void*) = nullptr;
    CGError (*request_notifications)(int, std::uint32_t*, int) = nullptr;
    CGError (*window_bounds)(int, std::uint32_t, CGRect*) = nullptr;
    CFTypeRef (*query_windows)(int, CFArrayRef, std::uint32_t) = nullptr;
    CFTypeRef (*query_result_windows)(CFTypeRef) = nullptr;
    bool (*iterator_advance)(CFTypeRef) = nullptr;
    CFArrayRef (*iterator_corner_radii)(CFTypeRef) = nullptr;
    int connection = 0;

    bool load() {
        void* library = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight", RTLD_LAZY | RTLD_LOCAL);
        if (!library) return false;
        auto bind = [&](auto& target, const char* name) { target = reinterpret_cast<std::decay_t<decltype(target)>>(dlsym(library, name)); };
        bind(main_connection, "SLSMainConnectionID");
        bind(register_notify, "SLSRegisterNotifyProc");
        bind(request_notifications, "SLSRequestNotificationsForWindows");
        bind(window_bounds, "SLSGetWindowBounds");
        bind(query_windows, "SLSWindowQueryWindows");
        bind(query_result_windows, "SLSWindowQueryResultCopyWindows");
        bind(iterator_advance, "SLSWindowIteratorAdvance");
        bind(iterator_corner_radii, "SLSWindowIteratorGetCornerRadii");  // macOS 26 and later
        if (!main_connection || !register_notify || !request_notifications || !window_bounds) return false;
        connection = main_connection();
        return connection != 0;
    }

    float corner_radius(std::uint32_t window) const {
        if (!query_windows || !query_result_windows || !iterator_advance || !iterator_corner_radii) return default_corner_radius;
        float radius = default_corner_radius;
        CFNumberRef number = CFNumberCreate(nullptr, kCFNumberSInt32Type, &window);
        CFArrayRef list = CFArrayCreate(nullptr, reinterpret_cast<const void**>(&number), 1, &kCFTypeArrayCallBacks);
        CFRelease(number);
        if (CFTypeRef query = query_windows(connection, list, 0)) {
            if (CFTypeRef iterator = query_result_windows(query)) {
                if (iterator_advance(iterator)) {
                    if (CFArrayRef radii = iterator_corner_radii(iterator)) {
                        int value = 0;
                        if (CFArrayGetCount(radii) > 0 &&
                            CFNumberGetValue(static_cast<CFNumberRef>(CFArrayGetValueAtIndex(radii, 0)), kCFNumberSInt32Type, &value) &&
                            value > 0)
                            radius = static_cast<float>(value);
                        CFRelease(radii);
                    }
                }
                CFRelease(iterator);
            }
            CFRelease(query);
        }
        CFRelease(list);
        return radius;
    }
};

struct Tracked {
    CGRect bounds;
    float radius;
};

// Notifications are delivered on the main thread while AppKit services the
// WindowServer connection; the tracker is also used only on the main thread.
struct Events {
    bool structure_changed = true;
    std::unordered_set<std::uint32_t> moved;
};
Events* active_events = nullptr;

void window_changed(std::uint32_t event, void* data, std::size_t length, void*) {
    if (!active_events) return;
    if ((event == event_window_move || event == event_window_resize) && data && length >= sizeof(std::uint32_t)) {
        active_events->moved.insert(*static_cast<std::uint32_t*>(data));
        return;
    }
    active_events->structure_changed = true;
}
}

struct WindowTracker::Impl {
    SkyLight skylight;
    bool live = false;
    Events events;
    std::unordered_map<std::uint32_t, Tracked> windows;
    std::vector<std::uint32_t> order;  // front to back
    std::vector<std::uint32_t> subscribed;
    Clock::time_point next_full_refresh{};

    bool refresh_all() {
        @autoreleasepool {
            NSArray* list = CFBridgingRelease(CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly |
                                                                       kCGWindowListExcludeDesktopElements, kCGNullWindowID));
            if (!list) return set_desktop_error("Window geometry is unavailable: no accessible Quartz desktop session");
            std::unordered_map<std::uint32_t, Tracked> refreshed;
            order.clear();
            for (NSDictionary* info in list) {
                NSNumber* layer = info[(__bridge NSString*)kCGWindowLayer];
                NSNumber* owner = info[(__bridge NSString*)kCGWindowOwnerPID];
                NSNumber* alpha = info[(__bridge NSString*)kCGWindowAlpha];
                NSNumber* number = info[(__bridge NSString*)kCGWindowNumber];
                if (!layer || !number || layer.intValue != 0 || owner.intValue == getpid() || alpha.doubleValue <= 0.01) continue;
                NSDictionary* bounds = info[(__bridge NSString*)kCGWindowBounds];
                CGRect rect;
                if (!bounds || !CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)bounds, &rect)) continue;
                const std::uint32_t id = number.unsignedIntValue;
                const auto known = windows.find(id);
                const float radius = known != windows.end() ? known->second.radius :
                                     live ? skylight.corner_radius(id) : default_corner_radius;
                refreshed[id] = {rect, radius};
                order.push_back(id);
            }
            windows = std::move(refreshed);
            if (live) {
                std::vector<std::uint32_t> ids = order;
                std::sort(ids.begin(), ids.end());
                if (ids != subscribed) {
                    subscribed = ids;
                    skylight.request_notifications(skylight.connection, subscribed.data(), static_cast<int>(subscribed.size()));
                }
            }
            return true;
        }
    }
};

WindowTracker::WindowTracker() : impl_(std::make_unique<Impl>()) {
    auto& s = *impl_;
    s.live = s.skylight.load();
    if (!s.live) return;
    // Notify procs cannot be unregistered, so register them once per process
    // and route them to whichever tracker is current.
    static bool registered = false;
    if (!registered) {
        registered = true;
        for (const std::uint32_t event : {event_window_close, event_window_move, event_window_resize, event_window_reorder,
                                          event_window_level, event_window_unhide, event_window_hide, event_window_create,
                                          event_window_destroy, event_space_change, event_front_change})
            s.skylight.register_notify(window_changed, event, nullptr);
    }
    active_events = &s.events;
}

WindowTracker::~WindowTracker() {
    if (active_events == &impl_->events) active_events = nullptr;
}

bool WindowTracker::live() const { return impl_->live; }

bool WindowTracker::update(const DesktopRect& canvas, std::vector<WindowObstacle>& output) {
    auto& s = *impl_;
    const auto now = Clock::now();
    // Live tracking still re-reads the list occasionally in case a
    // notification is missed; polling refreshes eight times per second.
    if (s.events.structure_changed || now >= s.next_full_refresh) {
        s.events.structure_changed = false;
        s.events.moved.clear();
        if (!s.refresh_all()) return false;
        s.next_full_refresh = now + (s.live ? std::chrono::milliseconds(1000) : std::chrono::milliseconds(125));
    } else if (!s.events.moved.empty()) {
        for (const std::uint32_t id : s.events.moved) {
            const auto window = s.windows.find(id);
            CGRect bounds;
            if (window != s.windows.end() && s.skylight.window_bounds(s.skylight.connection, id, &bounds) == kCGErrorSuccess)
                window->second.bounds = bounds;
        }
        s.events.moved.clear();
    }

    output.clear();
    for (const std::uint32_t id : s.order) {
        const auto& window = s.windows[id];
        const CGRect rect = window.bounds;
        WindowObstacle obstacle{};
        obstacle.x = static_cast<float>(rect.origin.x) - canvas.x;
        obstacle.y = static_cast<float>(rect.origin.y) - canvas.y;
        obstacle.width = static_cast<float>(rect.size.width);
        obstacle.height = static_cast<float>(rect.size.height);
        obstacle.radius = window.radius;
        obstacle.id = id;
        if (obstacle.width < 10 || obstacle.height < 10 || obstacle.x >= canvas.w || obstacle.y >= canvas.h ||
            obstacle.x + obstacle.width <= 0 || obstacle.y + obstacle.height <= 0) continue;
        // A solid rectangle covering the entire canvas leaves no space for
        // a fluid simulation, so fullscreen/maximized covering windows are omitted.
        if (obstacle.x <= 0 && obstacle.y <= 0 && obstacle.x + obstacle.width >= canvas.w &&
            obstacle.y + obstacle.height >= canvas.h) continue;
        output.push_back(obstacle);
        if (output.size() == FluidSimulation::max_windows) break;
    }
    return true;
}
