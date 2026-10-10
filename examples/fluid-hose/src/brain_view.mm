#import <AppKit/AppKit.h>
#include "brain_view.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace {
struct Dot { CGFloat x, y, radius; std::size_t neuron; FishBrain::Circuit circuit; bool measured; };

NSColor* circuit_color(FishBrain::Circuit circuit, CGFloat alpha) {
    switch (circuit) {
        case FishBrain::Circuit::sense: return [NSColor colorWithSRGBRed:0.35 green:0.82 blue:1.0 alpha:alpha];
        case FishBrain::Circuit::decide: return [NSColor colorWithSRGBRed:0.78 green:0.55 blue:1.0 alpha:alpha];
        case FishBrain::Circuit::move: return [NSColor colorWithSRGBRed:1.0 green:0.62 blue:0.28 alpha:alpha];
        case FishBrain::Circuit::drive: return [NSColor colorWithSRGBRed:1.0 green:0.42 blue:0.62 alpha:alpha];
    }
    return NSColor.whiteColor;
}
}

@interface BrainCanvas : NSView
@property(nonatomic) unsigned clicks;
@property(nonatomic, copy) NSString* title;
@property(nonatomic, copy) NSString* status;
- (void)setAreas:(const std::vector<FishBrain::Area>&)areas activity:(const std::vector<float>&)activity;
- (void)clear;
@end

@implementation BrainCanvas {
    std::vector<FishBrain::Area> _areas;
    std::vector<float> _activity;
}

- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { (void)event; return YES; }
- (void)mouseDown:(NSEvent*)event { (void)event; self.clicks += 1; }

- (void)setAreas:(const std::vector<FishBrain::Area>&)areas activity:(const std::vector<float>&)activity {
    _areas = areas;
    _activity = activity;
    self.needsDisplay = YES;
}

- (void)clear {
    _areas.clear();
    _activity.clear();
    self.needsDisplay = YES;
}

- (bool)hasMeasured {
    for (const auto& area : _areas)
        if (area.measured && area.size) return true;
    return false;
}

// The diagram fills a 2:1 box below the header, snout to the left, leaving
// strips at the bottom for the spinal cord and the measured hindbrain.
- (CGFloat)spinalStrip { return self.bounds.size.height * 0.1; }
- (NSRect)diagram {
    const NSRect bounds = NSInsetRect(self.bounds, 16, 12);
    const CGFloat header = 40, legend = 22;
    const CGFloat strip = ([self hasMeasured] ? bounds.size.height * 0.27 : 0) + [self spinalStrip];
    CGFloat width = bounds.size.width, height = bounds.size.height - header - legend - strip;
    if (width > height * 2) width = height * 2;
    else height = width / 2;
    return NSMakeRect(NSMidX(bounds) - width / 2, bounds.origin.y + header, width, height);
}

- (void)drawOutline:(NSRect)box {
    auto at = [&](CGFloat x, CGFloat y) { return NSMakePoint(box.origin.x + x * box.size.width, box.origin.y + y * box.size.height); };
    NSBezierPath* brain = [NSBezierPath bezierPath];
    [brain moveToPoint:at(0.17, 0.62)];
    [brain curveToPoint:at(0.30, 0.18) controlPoint1:at(0.15, 0.40) controlPoint2:at(0.20, 0.20)];
    [brain curveToPoint:at(0.56, 0.06) controlPoint1:at(0.38, 0.06) controlPoint2:at(0.48, 0.02)];
    [brain curveToPoint:at(0.70, 0.20) controlPoint1:at(0.64, 0.08) controlPoint2:at(0.68, 0.14)];
    [brain curveToPoint:at(1.00, 0.36) controlPoint1:at(0.80, 0.22) controlPoint2:at(0.92, 0.30)];
    [brain lineToPoint:at(1.00, 0.70)];
    [brain curveToPoint:at(0.60, 0.80) controlPoint1:at(0.88, 0.76) controlPoint2:at(0.72, 0.80)];
    [brain curveToPoint:at(0.22, 0.94) controlPoint1:at(0.46, 0.80) controlPoint2:at(0.32, 0.98)];
    [brain curveToPoint:at(0.17, 0.62) controlPoint1:at(0.16, 0.90) controlPoint2:at(0.16, 0.74)];
    [brain closePath];
    [[NSColor colorWithWhite:1 alpha:0.05] setFill];
    [brain fill];
    [[NSColor colorWithWhite:1 alpha:0.16] setStroke];
    brain.lineWidth = 1;
    [brain stroke];
    const NSRect eye = NSMakeRect(box.origin.x + 0.02 * box.size.width, box.origin.y + 0.40 * box.size.height,
                                  0.16 * box.size.width, 0.32 * box.size.height);
    NSBezierPath* eye_path = [NSBezierPath bezierPathWithOvalInRect:eye];
    [[NSColor colorWithWhite:1 alpha:0.04] setFill];
    [eye_path fill];
    [eye_path stroke];
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    [[NSColor colorWithSRGBRed:0.05 green:0.06 blue:0.09 alpha:1] setFill];
    NSRectFill(self.bounds);
    NSDictionary* title_style = @{NSFontAttributeName: [NSFont boldSystemFontOfSize:13],
                                  NSForegroundColorAttributeName: [NSColor colorWithWhite:0.95 alpha:1]};
    NSDictionary* status_style = @{NSFontAttributeName: [NSFont systemFontOfSize:11],
                                   NSForegroundColorAttributeName: [NSColor colorWithWhite:0.7 alpha:1]};
    [(self.title ? self.title : @"") drawAtPoint:NSMakePoint(16, 10) withAttributes:title_style];
    [(self.status ? self.status : @"") drawAtPoint:NSMakePoint(16, 28) withAttributes:status_style];
    if (_areas.empty()) return;

    const NSRect box = [self diagram];
    [self drawOutline:box];
    const CGFloat unit = box.size.width / 100;  // a dot's spacing
    NSDictionary* label_style = @{NSFontAttributeName: [NSFont systemFontOfSize:std::max<CGFloat>(8, unit * 1.15)],
                                  NSForegroundColorAttributeName: [NSColor colorWithWhite:0.6 alpha:1]};

    std::vector<Dot> dots;
    // A strip of compact grids, one per population, sized by neuron count.
    auto grids = [&](const std::vector<const FishBrain::Area*>& row, CGFloat left, CGFloat right, CGFloat y, CGFloat height) {
        if (row.empty()) return;
        const CGFloat gap = unit * 1.5;
        std::vector<CGFloat> widths;
        CGFloat sum = 0;
        std::size_t total = 0;
        for (const auto* area : row) total += area->size;
        const CGFloat usable = right - left - gap * (row.size() - 1);
        for (const auto* area : row) {
            NSString* label = [NSString stringWithUTF8String:area->label];
            widths.push_back(std::max(usable * area->size / total, [label sizeWithAttributes:label_style].width + unit));
            sum += widths.back();
        }
        CGFloat x = left;
        for (std::size_t k = 0; k < row.size(); ++k) {
            const auto* area = row[k];
            const CGFloat width = widths[k] * usable / sum;
            CGFloat cell = std::sqrt(width * height / area->size);
            std::size_t columns = 1;
            for (; cell > 1.5; cell *= 0.95) {
                columns = std::max<std::size_t>(1, static_cast<std::size_t>(width / cell));
                if ((area->size + columns - 1) / columns * cell <= height) break;
            }
            for (std::size_t i = 0; i < area->size; ++i)
                dots.push_back({x + (i % columns + 0.5) * cell, y + (i / columns + 0.5) * cell, cell * 0.32, area->first + i,
                                area->circuit, true});
            [[NSString stringWithUTF8String:area->label] drawAtPoint:NSMakePoint(x, y + height + unit * 0.1) withAttributes:label_style];
            x += width + gap;
        }
    };
    NSDictionary* heading = @{NSFontAttributeName: [NSFont systemFontOfSize:std::max<CGFloat>(9, unit * 1.2)],
                              NSForegroundColorAttributeName: [NSColor colorWithWhite:0.75 alpha:1]};
    {
        std::vector<const FishBrain::Area*> spinal;
        for (const auto& area : _areas)
            if (area.spinal && area.size) spinal.push_back(&area);
        const CGFloat top = NSMaxY(box) + unit * 2;
        [@"Spinal cord: the swim rhythm" drawAtPoint:NSMakePoint(16, top - unit * 1.6) withAttributes:heading];
        grids(spinal, 16, NSMaxX(self.bounds) - 16, top + unit * 0.8, [self spinalStrip] - unit * 4.2);
    }
    // The measured hindbrain: one row per side, a grid per cell type.
    if ([self hasMeasured]) {
        const CGFloat top = NSMaxY(box) + [self spinalStrip] + unit * 2, bottom = NSMaxY(self.bounds) - 34;
        [@"Measured hindbrain wiring, one copy per side" drawAtPoint:NSMakePoint(16, top - unit * 1.6) withAttributes:heading];
        std::vector<const FishBrain::Area*> left, right;
        for (const auto& area : _areas) {
            if (!area.measured || !area.size) continue;
            (std::string(area.name).find("(left)") != std::string::npos ? left : right).push_back(&area);
        }
        const CGFloat row_height = (bottom - top) / 2;
        const CGFloat gap = unit * 1.5, side_label = unit * 5;
        // Widths in proportion to the number of cells, at least a label's
        // worth, then scaled to fit.
        std::vector<CGFloat> widths;
        CGFloat sum = 0;
        std::size_t total = 0;
        for (const auto* area : left) total += area->size;
        const CGFloat usable = self.bounds.size.width - 32 - side_label - gap * (left.size() - 1);
        for (const auto* area : left) {
            NSString* label = [NSString stringWithUTF8String:area->label];
            widths.push_back(std::max(usable * area->size / total, [label sizeWithAttributes:label_style].width + unit));
            sum += widths.back();
        }
        for (auto& width : widths) width *= usable / sum;
        for (int side = 0; side < 2; ++side) {
            const auto& row = side ? right : left;
            const CGFloat y = top + side * row_height + unit * 0.8;
            [(side ? @"right" : @"left") drawAtPoint:NSMakePoint(16, y) withAttributes:label_style];
            CGFloat x = 16 + side_label;
            for (std::size_t k = 0; k < row.size() && k < widths.size(); ++k) {
                const auto* area = row[k];
                const CGFloat width = widths[k];
                const CGFloat height = row_height - unit * 2.6;
                // The largest square cells whose grid fits the box.
                CGFloat cell = std::sqrt(width * height / area->size);
                std::size_t columns = 1;
                for (; cell > 1.5; cell *= 0.95) {
                    columns = std::max<std::size_t>(1, static_cast<std::size_t>(width / cell));
                    if ((area->size + columns - 1) / columns * cell <= height) break;
                }
                for (std::size_t i = 0; i < area->size; ++i)
                    dots.push_back({x + (i % columns + 0.5) * cell, y + (i / columns + 0.5) * cell, cell * 0.32, area->first + i,
                                    area->circuit, true});
                NSString* label = [NSString stringWithUTF8String:area->label];
                [label drawAtPoint:NSMakePoint(x, y + height + unit * 0.1) withAttributes:label_style];
                x += width + gap;
            }
        }
    }
    for (const auto& area : _areas) {
        if (area.measured || area.spinal) continue;
        const CGFloat cx = box.origin.x + area.x * box.size.width, cy = box.origin.y + area.y * box.size.height;
        const CGFloat radius = unit * (area.size == 1 ? 1.3 : 0.55);
        if (area.topographic) {
            // Ordered by direction around a dome: from the belly side behind,
            // over the top, to the back side, with neurons stacked in layers.
            const std::size_t layers = area.size >= 32 ? 3 : 2;
            const std::size_t columns = (area.size + layers - 1) / layers;
            const CGFloat reach = unit * (2.0 + columns * 0.45);
            for (std::size_t i = 0; i < area.size; ++i) {
                const CGFloat t = ((i / layers) + 0.5) / columns;
                const CGFloat angle = M_PI * (1.1 + 0.8 * t);
                const CGFloat r = reach - (i % layers) * unit * 1.3;
                dots.push_back({cx + std::cos(angle) * r * 1.3, cy + std::sin(angle) * r * 0.8 + reach * 0.5, radius,
                                area.first + i, area.circuit, area.measured});
            }
        } else {
            // A sunflower cluster; the big measured populations pack tighter.
            const CGFloat spacing = unit * (area.size > 60 ? 0.42 : area.size > 20 ? 0.8 : 1.25);
            const CGFloat dot = area.size > 60 ? radius * 0.55 : area.size > 20 ? radius * 0.8 : radius;
            for (std::size_t i = 0; i < area.size; ++i) {
                const CGFloat r = spacing * std::sqrt(i + (area.size == 1 ? 0.0 : 0.5));
                const CGFloat angle = i * 2.39996;
                dots.push_back({cx + r * std::cos(angle), cy + r * std::sin(angle), dot, area.first + i, area.circuit, area.measured});
            }
        }
        NSString* label = [NSString stringWithUTF8String:area.label];
        const NSSize size = [label sizeWithAttributes:label_style];
        const CGFloat spread = area.size > 60 ? 0.42 : area.size > 20 ? 0.8 : 1.25;
        const CGFloat below = area.topographic ? unit * 1.2 : unit * spread * std::sqrt(static_cast<CGFloat>(area.size)) + unit * 0.6;
        [label drawAtPoint:NSMakePoint(cx - size.width / 2, cy + below) withAttributes:label_style];
    }
    for (const auto& dot : dots) {
        const float spikes = dot.neuron < _activity.size() ? _activity[dot.neuron] : 0.0f;
        const CGFloat glow = std::min(1.0f, spikes / 2.0f);
        if (glow > 0.05 && !dot.measured) {
            const CGFloat halo = dot.radius * (1.6 + 1.6 * glow);
            [circuit_color(dot.circuit, 0.2 * glow) setFill];
            [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(dot.x - halo, dot.y - halo, halo * 2, halo * 2)] fill];
        }
        [circuit_color(dot.circuit, 0.22 + 0.78 * glow) setFill];
        [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(dot.x - dot.radius, dot.y - dot.radius, dot.radius * 2, dot.radius * 2)] fill];
    }


    // Legend.
    const std::pair<FishBrain::Circuit, NSString*> legend[] = {
        {FishBrain::Circuit::sense, @"senses"}, {FishBrain::Circuit::decide, @"decisions"},
        {FishBrain::Circuit::move, @"movement"}, {FishBrain::Circuit::drive, @"hunger & breath"}};
    CGFloat x = 16;
    const CGFloat y = NSMaxY(self.bounds) - 22;
    for (const auto& entry : legend) {
        [circuit_color(entry.first, 1) setFill];
        [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(x, y + 4, 7, 7)] fill];
        [entry.second drawAtPoint:NSMakePoint(x + 11, y) withAttributes:status_style];
        x += 22 + [entry.second sizeWithAttributes:status_style].width;
    }
    if ([self hasMeasured])
        [@"measured wiring: Vishwanathan et al. 2024" drawAtPoint:NSMakePoint(x, y) withAttributes:status_style];
    NSString* hint = @"click for the next fish";
    const NSSize hint_size = [hint sizeWithAttributes:status_style];
    [hint drawAtPoint:NSMakePoint(NSMaxX(self.bounds) - hint_size.width - 16, y) withAttributes:status_style];
}
@end

struct BrainWindow::Impl {
    NSPanel* panel = nil;
    BrainCanvas* canvas = nil;
};

BrainWindow::BrainWindow() : impl_(std::make_unique<Impl>()) {}

BrainWindow::~BrainWindow() {
    @autoreleasepool {
        if (impl_->panel) [impl_->panel close];
    }
}

void BrainWindow::show() {
    @autoreleasepool {
        auto& s = *impl_;
        if (!s.panel) {
            s.panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(80, 120, 760, 560)
                                                 styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                           NSWindowStyleMaskResizable | NSWindowStyleMaskUtilityWindow
                                                   backing:NSBackingStoreBuffered
                                                     defer:NO];
            s.panel.title = @"Fish brain";
            s.panel.releasedWhenClosed = NO;
            s.panel.floatingPanel = YES;
            s.panel.hidesOnDeactivate = NO;  // the app never becomes active
            s.panel.becomesKeyOnlyIfNeeded = YES;
            s.panel.minSize = NSMakeSize(420, 260);
            s.panel.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces;
            s.canvas = [[BrainCanvas alloc] initWithFrame:NSMakeRect(0, 0, 760, 560)];
            s.panel.contentView = s.canvas;
            [s.panel center];
        }
        if (!s.panel.visible) [s.panel orderFrontRegardless];
    }
}

void BrainWindow::hide() {
    @autoreleasepool {
        if (impl_->panel) [impl_->panel orderOut:nil];
    }
}

bool BrainWindow::visible() const { return impl_->panel && impl_->panel.visible; }

unsigned BrainWindow::take_clicks() {
    if (!impl_->canvas) return 0;
    const unsigned clicks = impl_->canvas.clicks;
    impl_->canvas.clicks = 0;
    return clicks;
}

void BrainWindow::update(const FishBrain& brain, const std::string& title, const std::string& status) {
    @autoreleasepool {
        if (!visible()) return;
        impl_->canvas.title = [NSString stringWithUTF8String:title.c_str()];
        impl_->canvas.status = [NSString stringWithUTF8String:status.c_str()];
        [impl_->canvas setAreas:brain.layout() activity:brain.activity()];
    }
}

bool BrainWindow::snapshot(const FishBrain& brain, const std::string& title, const std::string& status, const char* path) {
    @autoreleasepool {
        BrainCanvas* canvas = [[BrainCanvas alloc] initWithFrame:NSMakeRect(0, 0, 760, 560)];
        canvas.title = [NSString stringWithUTF8String:title.c_str()];
        canvas.status = [NSString stringWithUTF8String:status.c_str()];
        [canvas setAreas:brain.layout() activity:brain.activity()];
        NSBitmapImageRep* image = [canvas bitmapImageRepForCachingDisplayInRect:canvas.bounds];
        if (!image) return false;
        [canvas cacheDisplayInRect:canvas.bounds toBitmapImageRep:image];
        NSData* png = [image representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        return png && [png writeToFile:[NSString stringWithUTF8String:path] atomically:YES];
    }
}

void BrainWindow::update_empty(const std::string& message) {
    @autoreleasepool {
        if (!visible()) return;
        impl_->canvas.title = [NSString stringWithUTF8String:message.c_str()];
        impl_->canvas.status = @"";
        [impl_->canvas clear];
    }
}
