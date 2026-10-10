#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CAMetalDisplayLink.h>
#import <ImageIO/ImageIO.h>
#include "desktop.h"
#include "fluid_sim.h"
#include "screen_capture.h"
#include "pond.h"
#include <algorithm>
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <chrono>
#include <exception>
#include <cstring>
#include <memory>
#include <string>
#include <unistd.h>

static thread_local std::string last_desktop_error;

const char* desktop_error() { return last_desktop_error.c_str(); }

bool set_desktop_error(const char* format, ...) {
    char message[2048];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    last_desktop_error = message;
    return false;
}

bool initialize_hose_desktop() {
    @autoreleasepool {
        NSApplication* application = [NSApplication sharedApplication];
        if (!application) return set_desktop_error("Cannot create the macOS application");
        // LSUIElement already sets this policy for the packaged app. A request
        // to set the existing policy can return NO, so inspect the actual policy.
        if (application.activationPolicy != NSApplicationActivationPolicyAccessory)
            [application setActivationPolicy:NSApplicationActivationPolicyAccessory];
        if (application.activationPolicy != NSApplicationActivationPolicyAccessory)
            return set_desktop_error("Cannot initialize a macOS accessory application");
        return true;
    }
}

@interface HoseOverlayWindow : NSWindow
@end
@implementation HoseOverlayWindow
- (BOOL)canBecomeKeyWindow { return NO; }
- (BOOL)canBecomeMainWindow { return NO; }
@end

@interface HoseMetalView : NSView
- (void)updateDrawableSize;
@end
@implementation HoseMetalView
- (BOOL)isOpaque { return NO; }
- (BOOL)wantsUpdateLayer { return YES; }
- (CALayer*)makeBackingLayer { return [CAMetalLayer layer]; }
- (void)updateLayer { [self updateDrawableSize]; }
- (void)setFrameSize:(NSSize)size {
    [super setFrameSize:size];
    [self updateDrawableSize];
}
- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    [self updateDrawableSize];
}
- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    [self updateDrawableSize];
}
- (void)updateDrawableSize {
    CAMetalLayer* layer = (CAMetalLayer*)self.layer;
    if (!layer) return;
    const CGFloat scale = self.window ? self.window.backingScaleFactor : 1;
    const NSRect pixels = [self convertRectToBacking:self.bounds];
    layer.contentsScale = scale;
    layer.drawableSize = CGSizeMake(std::max<CGFloat>(1, std::round(pixels.size.width)),
                                    std::max<CGFloat>(1, std::round(pixels.size.height)));
}
@end

static NSRect appkit_canvas_frame(const DesktopRect& canvas) {
    NSScreen* primary = NSScreen.screens.firstObject;
    return NSMakeRect(canvas.x, NSMaxY(primary.frame) - canvas.y - canvas.h, canvas.w, canvas.h);
}

@interface FluidMenuController : NSObject
@property(nonatomic) BOOL pouring;
@property(nonatomic) BOOL paused;
@property(nonatomic) BOOL clearRequested;
@property(nonatomic) BOOL quitRequested;
@property(nonatomic) BOOL tiltEnabled;
@property(nonatomic) BOOL reverseTilt;
@property(nonatomic) DesktopDisplayID requestedDisplay;
@property(nonatomic) NSInteger particleSize;
@property(nonatomic) NSInteger liquidFeel;
@property(nonatomic) BOOL refraction;
@property(nonatomic) double flowRate;
@property(nonatomic) NSInteger duckRequests;
@property(nonatomic) BOOL removeDucksRequested;
@property(nonatomic) NSInteger fishRequests;
@property(nonatomic) NSInteger feedRequests;
@property(nonatomic) BOOL removeFishRequested;
@property(nonatomic) BOOL showBrain;
@property(nonatomic, strong) NSMenuItem* brainItem;
@property(nonatomic, strong) NSMenuItem* flowItem;
@property(nonatomic, strong) NSMenu* flowMenu;
@property(nonatomic) CFMachPortRef scrollTap;
@property(nonatomic, strong) NSMenu* sizeMenu;
@property(nonatomic, strong) NSMenu* liquidMenu;
- (void)togglePour:(NSMenuItem*)sender;
- (void)togglePause:(NSMenuItem*)sender;
- (void)clearWater:(NSMenuItem*)sender;
- (void)quit:(NSMenuItem*)sender;
- (void)toggleTilt:(NSMenuItem*)sender;
- (void)toggleReverseTilt:(NSMenuItem*)sender;
- (void)selectDisplay:(NSMenuItem*)sender;
- (void)selectSize:(NSMenuItem*)sender;
- (void)selectLiquid:(NSMenuItem*)sender;
- (void)toggleRefraction:(NSMenuItem*)sender;
- (void)selectFlow:(NSMenuItem*)sender;
- (void)addDuck:(NSMenuItem*)sender;
- (void)removeDucks:(NSMenuItem*)sender;
- (void)addFish:(NSMenuItem*)sender;
- (void)feedFish:(NSMenuItem*)sender;
- (void)removeFish:(NSMenuItem*)sender;
- (void)toggleBrain:(NSMenuItem*)sender;
- (void)adjustFlowByScroll:(double)points;
- (void)updateFlowMenu;
@end

@implementation FluidMenuController
- (void)togglePour:(NSMenuItem*)sender {
    self.pouring = !self.pouring;
    sender.state = self.pouring ? NSControlStateValueOn : NSControlStateValueOff;
}
- (void)togglePause:(NSMenuItem*)sender {
    self.paused = !self.paused;
    sender.state = self.paused ? NSControlStateValueOn : NSControlStateValueOff;
}
- (void)clearWater:(NSMenuItem*)sender { (void)sender; self.clearRequested = YES; }
- (void)quit:(NSMenuItem*)sender { (void)sender; self.quitRequested = YES; }
- (void)toggleTilt:(NSMenuItem*)sender {
    self.tiltEnabled = !self.tiltEnabled;
    sender.state = self.tiltEnabled ? NSControlStateValueOn : NSControlStateValueOff;
}
- (void)toggleReverseTilt:(NSMenuItem*)sender {
    self.reverseTilt = !self.reverseTilt;
    sender.state = self.reverseTilt ? NSControlStateValueOn : NSControlStateValueOff;
}
- (void)selectDisplay:(NSMenuItem*)sender {
    self.requestedDisplay = [sender.representedObject unsignedIntValue];
}
static void check_only(NSMenu* menu, NSInteger tag) {
    for (NSMenuItem* item in menu.itemArray)
        if (item.action) item.state = item.tag == tag ? NSControlStateValueOn : NSControlStateValueOff;
}
- (void)selectSize:(NSMenuItem*)sender {
    self.particleSize = sender.tag;
    check_only(self.sizeMenu, sender.tag);
}
- (void)selectLiquid:(NSMenuItem*)sender {
    self.liquidFeel = sender.tag;
    check_only(self.liquidMenu, sender.tag);
}
static const double flow_presets[] = {0.25, 0.5, 1.0, 1.5, 2.0, 3.0};
- (void)addDuck:(NSMenuItem*)sender { (void)sender; self.duckRequests += 1; }
- (void)removeDucks:(NSMenuItem*)sender { (void)sender; self.removeDucksRequested = YES; }
- (void)addFish:(NSMenuItem*)sender { (void)sender; self.fishRequests += 1; }
- (void)feedFish:(NSMenuItem*)sender { (void)sender; self.feedRequests += 1; }
- (void)removeFish:(NSMenuItem*)sender { (void)sender; self.removeFishRequested = YES; }
- (void)toggleBrain:(NSMenuItem*)sender {
    self.showBrain = !self.showBrain;
    sender.state = self.showBrain ? NSControlStateValueOn : NSControlStateValueOff;
}
- (void)selectFlow:(NSMenuItem*)sender {
    self.flowRate = flow_presets[sender.tag];
    [self updateFlowMenu];
}
- (void)adjustFlowByScroll:(double)points {
    // Exponential, so each scroll step changes the flow by the same proportion.
    self.flowRate = std::clamp(self.flowRate * std::exp(points * 0.008), static_cast<double>(FluidSimulation::min_flow),
                               static_cast<double>(FluidSimulation::max_flow));
    [self updateFlowMenu];
}
- (void)updateFlowMenu {
    self.flowItem.title = [NSString stringWithFormat:@"Flow: %.2g×", self.flowRate];
    for (NSMenuItem* item in self.flowMenu.itemArray)
        if (item.action) item.state = std::abs(flow_presets[item.tag] - self.flowRate) < 0.01 ? NSControlStateValueOn : NSControlStateValueOff;
}
- (void)toggleRefraction:(NSMenuItem*)sender {
    self.refraction = !self.refraction;
    // The first request shows the macOS Screen Recording prompt.
    if (self.refraction) ScreenCapture::request_permission();
    sender.state = self.refraction ? NSControlStateValueOn : NSControlStateValueOff;
}
@end

struct HoseRenderer {
    HoseOverlayWindow* window = nil;
    HoseMetalView* view = nil;
    id<CAMetalDrawable> current_drawable = nil;
    bool displays_changed = false;
    bool has_pending_canvas = false;
    DesktopRect pending_canvas{};
    CAMetalLayer* layer = nil;
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLRenderPipelineState> splat_pipeline = nil;
    id<MTLRenderPipelineState> surface_pipeline = nil;
    id<MTLRenderPipelineState> blur_pipeline = nil;
    id<MTLRenderPipelineState> body_pipeline = nil;
    id<MTLTexture> body_layer = nil;
    // Bodies to draw, in a ring so frames still on the GPU keep theirs.
    id<MTLBuffer> body_buffers[3] = {nil, nil, nil};
    unsigned body_slot = 0;
    id<MTLTexture> blurred = nil;   // horizontal pass
    id<MTLTexture> smoothed = nil;  // both passes, with a mip chain
    id<MTLTexture> field = nil;
    id<MTLTexture> no_background = nil;
    id<MTLCommandBuffer> last_submission = nil;
    ScreenCapture capture;
    bool refraction = false;
    DesktopDisplay refraction_display{};
    CaptureState previous_capture_state = CaptureState::starting;
    NSMenuItem* refraction_status = nil;
    CFMachPortRef scroll_tap = nullptr;
    CFRunLoopSourceRef scroll_source = nullptr;
    id scroll_monitor = nil;
    dispatch_semaphore_t available = dispatch_semaphore_create(3);
    NSStatusItem* status_item = nil;
    FluidMenuController* controls = nil;
    NSMenuItem* tilt_status = nil;
    NSMenuItem* fish_status = nil;
    std::string previous_fish_status;
    NSMenu* display_menu = nil;
    std::vector<DesktopDisplay> display_options;
    DesktopDisplayID selected_display = 0;
    TiltStatus previous_tilt_status = TiltStatus::off;
    std::string device_name;

    ~HoseRenderer() {
        @autoreleasepool {
            if (status_item) [NSStatusBar.systemStatusBar removeStatusItem:status_item];
            if (scroll_source) {
                CFRunLoopRemoveSource(CFRunLoopGetMain(), scroll_source, kCFRunLoopCommonModes);
                CFRelease(scroll_source);
            }
            if (scroll_tap) {
                CGEventTapEnable(scroll_tap, false);
                CFMachPortInvalidate(scroll_tap);
                CFRelease(scroll_tap);
            }
            if (scroll_monitor) [NSEvent removeMonitor:scroll_monitor];
            if (queue) {
                id<MTLCommandBuffer> barrier = [queue commandBuffer];
                [barrier commit];
                [barrier waitUntilCompleted];
            }
            if (window) {
                [window orderOut:nil];
                [window close];
            }
        }
    }

    bool initialize(const char* shader_file) {
        device = hose_metal_device();
        if (!device) return set_desktop_error("No Metal device is accessible to this process");
        device_name = device.name.UTF8String;
        queue = hose_metal_queue();
        if (!queue) return set_desktop_error("Cannot create the fluid command queue");
        NSError* error = nil;
        id<MTLLibrary> library = hose_shader_library(shader_file);
        if (!library) return false;

        MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
        descriptor.vertexFunction = [library newFunctionWithName:@"splat_vertex"];
        descriptor.fragmentFunction = [library newFunctionWithName:@"splat_fragment"];
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
        descriptor.colorAttachments[0].blendingEnabled = YES;
        descriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
        descriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
        descriptor.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
        descriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        descriptor.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
        descriptor.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
        splat_pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!splat_pipeline) return set_desktop_error("Create fluid splat pipeline: %s", error.localizedDescription.UTF8String);

        descriptor = [[MTLRenderPipelineDescriptor alloc] init];
        descriptor.vertexFunction = [library newFunctionWithName:@"surface_vertex"];
        descriptor.fragmentFunction = [library newFunctionWithName:@"surface_fragment"];
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        surface_pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!surface_pipeline) return set_desktop_error("Create fluid surface pipeline: %s", error.localizedDescription.UTF8String);
        descriptor = [[MTLRenderPipelineDescriptor alloc] init];
        descriptor.vertexFunction = [library newFunctionWithName:@"surface_vertex"];
        descriptor.fragmentFunction = [library newFunctionWithName:@"blur_fragment"];
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
        blur_pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!blur_pipeline) return set_desktop_error("Create fluid blur pipeline: %s", error.localizedDescription.UTF8String);
        descriptor = [[MTLRenderPipelineDescriptor alloc] init];
        descriptor.vertexFunction = [library newFunctionWithName:@"body_vertex"];
        descriptor.fragmentFunction = [library newFunctionWithName:@"body_fragment"];
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        descriptor.colorAttachments[0].blendingEnabled = YES;
        descriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
        descriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        descriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        descriptor.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        body_pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!body_pipeline) return set_desktop_error("Create body pipeline: %s", error.localizedDescription.UTF8String);
        for (auto& buffer : body_buffers) {
            buffer = [device newBufferWithLength:FluidSimulation::max_bodies * sizeof(PondBody) options:MTLResourceStorageModeShared];
            if (!buffer) return set_desktop_error("Cannot allocate body storage");
        }
        MTLTextureDescriptor* placeholder = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                              width:1 height:1 mipmapped:NO];
        no_background = [device newTextureWithDescriptor:placeholder];
        if (!no_background) return set_desktop_error("Cannot allocate the fluid background placeholder");
        return true;
    }

    // Splats particles into a fluid field at point resolution, builds its mip
    // chain for smooth thickness lookups, then shades the water surface.
    bool encode(id<MTLCommandBuffer> commands, id<MTLTexture> target, id<MTLBuffer> particles, SceneUniforms scene,
                id<MTLTexture> background = nil, const std::vector<PondBody>& bodies = {}) {
        const NSUInteger reduction = scene.scale >= 1.5f ? 2 : 1;
        const NSUInteger width = std::max<NSUInteger>(1, target.width / reduction);
        const NSUInteger height = std::max<NSUInteger>(1, target.height / reduction);
        if (!field || field.width != width || field.height != height) {
            auto texture = [&](bool mipmapped) {
                MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                                                                   width:width height:height
                                                                                               mipmapped:mipmapped];
                descriptor.mipmapLevelCount = std::min<NSUInteger>(descriptor.mipmapLevelCount, 6);
                descriptor.storageMode = MTLStorageModePrivate;
                descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                return [device newTextureWithDescriptor:descriptor];
            };
            field = texture(false);
            blurred = texture(false);
            smoothed = texture(true);
            if (!field || !blurred || !smoothed) return set_desktop_error("Cannot allocate the fluid field");
        }

        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = field;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
        if (!encoder) return set_desktop_error("Cannot begin the fluid splat pass");
        if (scene.count && particles) {
            [encoder setRenderPipelineState:splat_pipeline];
            [encoder setVertexBytes:&scene length:sizeof(scene) atIndex:0];
            [encoder setVertexBuffer:particles offset:0 atIndex:1];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6 instanceCount:scene.count];
        }
        [encoder endEncoding];

        // Blur by about 60% of a splat radius, in field texels: wide enough to
        // flatten bumps a few particles across.
        const float texels_per_point = scene.scale / reduction;
        const float sigma = std::max(0.5f, scene.particle_radius * 0.6f * texels_per_point);
        const std::pair<id<MTLTexture>, id<MTLTexture>> passes[2] = {{field, blurred}, {blurred, smoothed}};
        for (int axis = 0; axis < 2; ++axis) {
            pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = passes[axis].second;
            pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            encoder = [commands renderCommandEncoderWithDescriptor:pass];
            if (!encoder) return set_desktop_error("Cannot begin the fluid blur pass");
            const float parameters[4] = {axis == 0 ? 1.0f / width : 0.0f, axis == 1 ? 1.0f / height : 0.0f, sigma, 0};
            [encoder setRenderPipelineState:blur_pipeline];
            [encoder setFragmentBytes:parameters length:sizeof(parameters) atIndex:0];
            [encoder setFragmentTexture:passes[axis].first atIndex:0];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [encoder endEncoding];
        }
        // Ducks, fish, and food are drawn at full resolution into their own layer.
        if (!body_layer || body_layer.width != target.width || body_layer.height != target.height) {
            MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                               width:target.width height:target.height
                                                                                           mipmapped:NO];
            descriptor.storageMode = MTLStorageModePrivate;
            descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            body_layer = [device newTextureWithDescriptor:descriptor];
            if (!body_layer) return set_desktop_error("Cannot allocate the body layer");
        }
        pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = body_layer;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        encoder = [commands renderCommandEncoderWithDescriptor:pass];
        if (!encoder) return set_desktop_error("Cannot begin the body pass");
        if (!bodies.empty()) {
            const std::size_t count = std::min(bodies.size(), FluidSimulation::max_bodies);
            id<MTLBuffer> buffer = body_buffers[body_slot];
            body_slot = (body_slot + 1) % 3;
            std::memcpy(buffer.contents, bodies.data(), count * sizeof(PondBody));
            [encoder setRenderPipelineState:body_pipeline];
            [encoder setVertexBytes:&scene length:sizeof(scene) atIndex:0];
            [encoder setVertexBuffer:buffer offset:0 atIndex:1];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6 instanceCount:count];
        }
        [encoder endEncoding];

        id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
        if (!blit) return set_desktop_error("Cannot build the fluid field mip chain");
        [blit generateMipmapsForTexture:smoothed];
        [blit endEncoding];

        pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = target;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        encoder = [commands renderCommandEncoderWithDescriptor:pass];
        if (!encoder) return set_desktop_error("Cannot begin the fluid surface pass");
        [encoder setRenderPipelineState:surface_pipeline];
        [encoder setFragmentBytes:&scene length:sizeof(scene) atIndex:0];
        [encoder setFragmentTexture:smoothed atIndex:0];
        [encoder setFragmentTexture:background ? background : no_background atIndex:1];
        [encoder setFragmentTexture:body_layer atIndex:2];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        return true;
    }
};

// Option+scroll changes the flow. An event tap can consume those scroll events
// so the window under the pointer does not also scroll; it needs the
// Accessibility permission, so without it a passive monitor is used instead.
static CGEventRef scroll_tap_callback(CGEventTapProxy, CGEventType type, CGEventRef event, void* context) {
    FluidMenuController* controls = (__bridge FluidMenuController*)context;
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        if (controls.scrollTap) CGEventTapEnable(controls.scrollTap, true);
        return event;
    }
    if (type != kCGEventScrollWheel || !(CGEventGetFlags(event) & kCGEventFlagMaskAlternate)) return event;
    const bool continuous = CGEventGetIntegerValueField(event, kCGScrollWheelEventIsContinuous) != 0;
    const double points = continuous ? CGEventGetIntegerValueField(event, kCGScrollWheelEventPointDeltaAxis1)
                                     : CGEventGetIntegerValueField(event, kCGScrollWheelEventDeltaAxis1) * 12.0;
    [controls adjustFlowByScroll:points];
    return nullptr;
}

static bool watch_flow_scrolling(HoseRenderer* renderer) {
    FluidMenuController* controls = renderer->controls;
    renderer->scroll_tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault,
                                            CGEventMaskBit(kCGEventScrollWheel), scroll_tap_callback,
                                            (__bridge void*)controls);
    if (renderer->scroll_tap) {
        controls.scrollTap = renderer->scroll_tap;
        renderer->scroll_source = CFMachPortCreateRunLoopSource(nullptr, renderer->scroll_tap, 0);
        CFRunLoopAddSource(CFRunLoopGetMain(), renderer->scroll_source, kCFRunLoopCommonModes);
        return true;
    }
    renderer->scroll_monitor = [NSEvent addGlobalMonitorForEventsMatchingMask:NSEventMaskScrollWheel handler:^(NSEvent* event) {
        if (!(event.modifierFlags & NSEventModifierFlagOption)) return;
        [controls adjustFlowByScroll:event.hasPreciseScrollingDeltas ? event.scrollingDeltaY : event.scrollingDeltaY * 12.0];
    }];
    return false;
}

HoseRenderer* create_hose_renderer(const DesktopRect& canvas, const char* shader_file, bool continuous, bool tilt,
                                   FluidSettings settings, bool refraction, float flow) {
    @autoreleasepool {
        auto renderer = std::make_unique<HoseRenderer>();
        if (!renderer->initialize(shader_file)) return nullptr;
        renderer->window = [[HoseOverlayWindow alloc] initWithContentRect:appkit_canvas_frame(canvas)
            styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
        if (!renderer->window) { set_desktop_error("Cannot create the native overlay window"); return nullptr; }
        renderer->window.title = @"Fluid Hose";
        renderer->window.releasedWhenClosed = NO;
        renderer->window.opaque = NO;
        renderer->window.backgroundColor = NSColor.clearColor;
        renderer->window.hasShadow = NO;
        renderer->window.ignoresMouseEvents = YES;
        renderer->window.hidesOnDeactivate = NO;
        renderer->window.level = NSFloatingWindowLevel;
        renderer->window.collectionBehavior = NSWindowCollectionBehaviorFullScreenAuxiliary |
                                               NSWindowCollectionBehaviorIgnoresCycle;
        renderer->window.animationBehavior = NSWindowAnimationBehaviorNone;

        renderer->view = [[HoseMetalView alloc] initWithFrame:NSMakeRect(0, 0, canvas.w, canvas.h)];
        renderer->view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        renderer->view.wantsLayer = YES;
        renderer->layer = (CAMetalLayer*)renderer->view.layer;
        if (!renderer->layer) { set_desktop_error("Cannot create the native Metal layer"); return nullptr; }
        renderer->layer.device = renderer->device;
        renderer->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        // Captured desktop frames are sRGB too, so refracted pixels match the desktop around them.
        CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        renderer->layer.colorspace = srgb;
        CGColorSpaceRelease(srgb);
        renderer->layer.opaque = NO;
        renderer->layer.framebufferOnly = YES;
        renderer->layer.displaySyncEnabled = YES;
        renderer->layer.maximumDrawableCount = 3;
        renderer->window.contentView = renderer->view;
        [renderer->view updateDrawableSize];

        renderer->controls = [[FluidMenuController alloc] init];
        renderer->controls.pouring = continuous;
        renderer->controls.tiltEnabled = tilt;
        renderer->controls.particleSize = static_cast<NSInteger>(settings.size);
        renderer->controls.liquidFeel = static_cast<NSInteger>(settings.feel);
        renderer->controls.refraction = refraction;
        renderer->controls.flowRate = flow;
        renderer->status_item = [NSStatusBar.systemStatusBar statusItemWithLength:NSVariableStatusItemLength];
        renderer->status_item.button.title = @"💧";
        renderer->status_item.button.toolTip = @"Fluid Hose — hold Option to pour";
        NSMenu* menu = [[NSMenu alloc] initWithTitle:@"Fluid Hose"];
        for (NSString* text in @[@"Hold Option to pour at the pointer", @"Hold Control to sprinkle fish food"]) {
            NSMenuItem* hint = [[NSMenuItem alloc] initWithTitle:text action:nil keyEquivalent:@""];
            hint.enabled = NO;
            [menu addItem:hint];
        }
        [menu addItem:NSMenuItem.separatorItem];
        NSMenuItem* display = [[NSMenuItem alloc] initWithTitle:@"Display" action:nil keyEquivalent:@""];
        renderer->display_menu = [[NSMenu alloc] initWithTitle:@"Display"];
        display.submenu = renderer->display_menu;
        [menu addItem:display];
        [menu addItem:NSMenuItem.separatorItem];
        NSMenuItem* pour = [[NSMenuItem alloc] initWithTitle:@"Pour continuously" action:@selector(togglePour:) keyEquivalent:@""];
        pour.target = renderer->controls;
        pour.state = continuous ? NSControlStateValueOn : NSControlStateValueOff;
        [menu addItem:pour];
        NSMenuItem* pause = [[NSMenuItem alloc] initWithTitle:@"Pause fluid" action:@selector(togglePause:) keyEquivalent:@""];
        pause.target = renderer->controls;
        [menu addItem:pause];
        NSMenuItem* clear = [[NSMenuItem alloc] initWithTitle:@"Clear water" action:@selector(clearWater:) keyEquivalent:@""];
        clear.target = renderer->controls;
        [menu addItem:clear];
        NSMenuItem* duck = [[NSMenuItem alloc] initWithTitle:@"Add rubber duck 🦆" action:@selector(addDuck:) keyEquivalent:@""];
        duck.target = renderer->controls;
        [menu addItem:duck];
        NSMenuItem* no_ducks = [[NSMenuItem alloc] initWithTitle:@"Remove ducks" action:@selector(removeDucks:) keyEquivalent:@""];
        no_ducks.target = renderer->controls;
        [menu addItem:no_ducks];
        [menu addItem:NSMenuItem.separatorItem];
        NSMenuItem* fish = [[NSMenuItem alloc] initWithTitle:@"Add fish 🐟" action:@selector(addFish:) keyEquivalent:@""];
        fish.target = renderer->controls;
        [menu addItem:fish];
        NSMenuItem* feed = [[NSMenuItem alloc] initWithTitle:@"Feed the fish" action:@selector(feedFish:) keyEquivalent:@""];
        feed.target = renderer->controls;
        [menu addItem:feed];
        NSMenuItem* no_fish = [[NSMenuItem alloc] initWithTitle:@"Remove fish" action:@selector(removeFish:) keyEquivalent:@""];
        no_fish.target = renderer->controls;
        [menu addItem:no_fish];
        NSMenuItem* brain = [[NSMenuItem alloc] initWithTitle:@"Show fish brain" action:@selector(toggleBrain:) keyEquivalent:@""];
        brain.target = renderer->controls;
        renderer->controls.brainItem = brain;
        [menu addItem:brain];
        renderer->fish_status = [[NSMenuItem alloc] initWithTitle:@"No fish" action:nil keyEquivalent:@""];
        renderer->fish_status.enabled = NO;
        [menu addItem:renderer->fish_status];
        [menu addItem:NSMenuItem.separatorItem];
        auto submenu = [&](NSString* title, NSArray<NSString*>* options, SEL action, NSInteger selected, NSString* hint) {
            NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
            NSMenu* choices = [[NSMenu alloc] initWithTitle:title];
            for (NSUInteger i = 0; i < options.count; ++i) {
                NSMenuItem* option = [[NSMenuItem alloc] initWithTitle:options[i] action:action keyEquivalent:@""];
                option.target = renderer->controls;
                option.tag = static_cast<NSInteger>(i);
                option.state = option.tag == selected ? NSControlStateValueOn : NSControlStateValueOff;
                [choices addItem:option];
            }
            if (hint) {
                [choices addItem:NSMenuItem.separatorItem];
                NSMenuItem* note = [[NSMenuItem alloc] initWithTitle:hint action:nil keyEquivalent:@""];
                note.enabled = NO;
                [choices addItem:note];
            }
            item.submenu = choices;
            [menu addItem:item];
            return choices;
        };
        const bool scroll_consumed = watch_flow_scrolling(renderer.get());
        renderer->controls.flowMenu = submenu(@"Flow", @[@"0.25×", @"0.5×", @"1×", @"1.5×", @"2×", @"3×"],
                                              @selector(selectFlow:), -1,
                                              scroll_consumed ? @"Option+scroll adjusts the flow"
                                                              : @"Option+scroll adjusts the flow (allow Accessibility to stop it also scrolling windows)");
        renderer->controls.flowItem = menu.itemArray.lastObject;
        [renderer->controls updateFlowMenu];
        renderer->controls.liquidMenu = submenu(@"Liquid", @[@"Water", @"Thick", @"Goo"], @selector(selectLiquid:),
                                                renderer->controls.liquidFeel, nil);
        renderer->controls.sizeMenu = submenu(@"Particle size", @[@"Fine — most detail", @"Medium", @"Coarse — most water"],
                                              @selector(selectSize:), renderer->controls.particleSize,
                                              @"Changing the size clears the water");
        NSMenuItem* refract = [[NSMenuItem alloc] initWithTitle:@"Refract the desktop" action:@selector(toggleRefraction:) keyEquivalent:@""];
        refract.target = renderer->controls;
        refract.state = refraction ? NSControlStateValueOn : NSControlStateValueOff;
        [menu addItem:refract];
        renderer->refraction_status = [[NSMenuItem alloc] initWithTitle:@"Refraction: off" action:nil keyEquivalent:@""];
        renderer->refraction_status.enabled = NO;
        [menu addItem:renderer->refraction_status];
        [menu addItem:NSMenuItem.separatorItem];
        NSMenuItem* tilt_item = [[NSMenuItem alloc] initWithTitle:@"Use Mac tilt" action:@selector(toggleTilt:) keyEquivalent:@""];
        tilt_item.target = renderer->controls;
        tilt_item.state = tilt ? NSControlStateValueOn : NSControlStateValueOff;
        [menu addItem:tilt_item];
        NSMenuItem* reverse = [[NSMenuItem alloc] initWithTitle:@"Reverse tilt left/right" action:@selector(toggleReverseTilt:) keyEquivalent:@""];
        reverse.target = renderer->controls;
        [menu addItem:reverse];
        renderer->tilt_status = [[NSMenuItem alloc] initWithTitle:@"Tilt: off" action:nil keyEquivalent:@""];
        renderer->tilt_status.enabled = NO;
        [menu addItem:renderer->tilt_status];
        [menu addItem:NSMenuItem.separatorItem];
        NSMenuItem* quit = [[NSMenuItem alloc] initWithTitle:@"Quit Fluid Hose" action:@selector(quit:) keyEquivalent:@""];
        quit.target = renderer->controls;
        [menu addItem:quit];
        renderer->status_item.menu = menu;
        return renderer.release();
    }
}

void destroy_hose_renderer(HoseRenderer* renderer) { delete renderer; }

void show_hose_window(HoseRenderer* renderer) {
    [renderer->window orderFrontRegardless];
    [renderer->view updateDrawableSize];
}

void move_hose_window(HoseRenderer* renderer, const DesktopRect& canvas) {
    // Apply geometry after the current drawable has been submitted.
    [renderer->window orderOut:nil];
    if (renderer->current_drawable) {
        renderer->pending_canvas = canvas;
        renderer->has_pending_canvas = true;
        return;
    }
    [renderer->window setFrame:appkit_canvas_frame(canvas) display:YES];
    [renderer->view updateDrawableSize];
    show_hose_window(renderer);
}

bool take_hose_display_changes(HoseRenderer* renderer) {
    const bool changed = renderer->displays_changed;
    renderer->displays_changed = false;
    return changed;
}

@interface HoseAppDelegate : NSObject <NSApplicationDelegate, CAMetalDisplayLinkDelegate> {
@public
    HoseRenderer* renderer;
    std::function<bool()> render_frame;
    std::function<bool()> should_stop;
    std::exception_ptr failure;
}
@property(nonatomic, strong) CAMetalDisplayLink* displayLink;
@property(nonatomic, strong) NSTimer* stopTimer;
@property(nonatomic) BOOL stopping;
- (void)start;
- (void)stop;
- (void)checkStop:(NSTimer*)timer;
- (void)updateFrameRate;
@end

@implementation HoseAppDelegate
- (void)start {
    self.displayLink = [[CAMetalDisplayLink alloc] initWithMetalLayer:renderer->layer];
    self.displayLink.delegate = self;
    [self updateFrameRate];
    [self.displayLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    // Signals, menu quit, and --duration must work even if no drawable is available.
    self.stopTimer = [NSTimer timerWithTimeInterval:0.1 target:self selector:@selector(checkStop:)
                                          userInfo:nil repeats:YES];
    [NSRunLoop.mainRunLoop addTimer:self.stopTimer forMode:NSRunLoopCommonModes];
}
- (void)stop {
    if (self.stopping) return;
    self.stopping = YES;
    [self.displayLink invalidate];
    [self.stopTimer invalidate];
    [NSApp stop:nil];
    NSEvent* wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint
        modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0];
    [NSApp postEvent:wake atStart:NO];
}
- (void)checkStop:(NSTimer*)timer {
    (void)timer;
    try {
        if (renderer->controls.quitRequested || should_stop()) [self stop];
    } catch (...) {
        failure = std::current_exception();
        [self stop];
    }
}
- (void)updateFrameRate {
    const NSInteger maximum = renderer->window.screen.maximumFramesPerSecond;
    const float rate = std::min(120.0f, static_cast<float>(maximum > 0 ? maximum : 60));
    self.displayLink.preferredFrameRateRange = CAFrameRateRangeMake(rate, rate, rate);
}
- (void)metalDisplayLink:(CAMetalDisplayLink*)link needsUpdate:(CAMetalDisplayLinkUpdate*)update {
    (void)link;
    @autoreleasepool {
        if (self.stopping) return;
        try {
            if (renderer->controls.quitRequested || should_stop()) { [self stop]; return; }
            renderer->current_drawable = update.drawable;
            if (!render_frame()) [self stop];
        } catch (...) {
            failure = std::current_exception();
            [self stop];
        }
        renderer->current_drawable = nil;
        if (renderer->has_pending_canvas) {
            renderer->has_pending_canvas = false;
            move_hose_window(renderer, renderer->pending_canvas);
            [self updateFrameRate];
        }
    }
}
- (void)applicationDidChangeScreenParameters:(NSNotification*)notification {
    (void)notification;
    renderer->displays_changed = true;
    [renderer->view updateDrawableSize];
    [self updateFrameRate];
}
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)application {
    (void)application;
    [self stop];
    // Return through main so C++ destructors close the motion helper's control pipe.
    return NSTerminateCancel;
}
@end

void run_hose_event_loop(HoseRenderer* renderer, const std::function<bool()>& frame,
                         const std::function<bool()>& should_stop) {
    @autoreleasepool {
        HoseAppDelegate* delegate = [[HoseAppDelegate alloc] init];
        delegate->renderer = renderer;
        delegate->render_frame = frame;
        delegate->should_stop = should_stop;
        NSApp.delegate = delegate;
        [delegate start];
        [NSApp run];
        [delegate stop];
        NSApp.delegate = nil;
        if (delegate->failure) std::rethrow_exception(delegate->failure);
    }
}
const char* hose_device_name(const HoseRenderer* renderer) { return renderer->device_name.c_str(); }

DesktopInput poll_desktop_input(HoseRenderer* renderer, const DesktopRect& canvas) {
    @autoreleasepool {
        FluidPoint pointer{-100, -100};
        CGEventRef event = CGEventCreate(nullptr);
        if (event) {
            const CGPoint position = CGEventGetLocation(event);
            pointer = {static_cast<float>(position.x) - canvas.x, static_cast<float>(position.y) - canvas.y};
            CFRelease(event);
        }
        const CGEventFlags flags = CGEventSourceFlagsState(kCGEventSourceStateCombinedSessionState);
        const bool option = (flags & kCGEventFlagMaskAlternate) != 0;
        const bool control = (flags & kCGEventFlagMaskControl) != 0;
        DesktopInput input{pointer, option || static_cast<bool>(renderer->controls.pouring),
                           static_cast<bool>(renderer->controls.paused), static_cast<bool>(renderer->controls.clearRequested),
                           static_cast<bool>(renderer->controls.quitRequested),
                           static_cast<bool>(renderer->controls.tiltEnabled), static_cast<bool>(renderer->controls.reverseTilt),
                           renderer->controls.requestedDisplay,
                           {static_cast<ParticleSize>(renderer->controls.particleSize),
                            static_cast<LiquidFeel>(renderer->controls.liquidFeel)},
                           static_cast<bool>(renderer->controls.refraction),
                           static_cast<float>(renderer->controls.flowRate),
                           static_cast<unsigned>(renderer->controls.duckRequests),
                           static_cast<bool>(renderer->controls.removeDucksRequested),
                           static_cast<unsigned>(renderer->controls.fishRequests),
                           static_cast<bool>(renderer->controls.removeFishRequested),
                           static_cast<unsigned>(renderer->controls.feedRequests), control,
                           static_cast<bool>(renderer->controls.showBrain)};
        renderer->controls.duckRequests = 0;
        renderer->controls.fishRequests = 0;
        renderer->controls.feedRequests = 0;
        renderer->controls.removeFishRequested = NO;
        renderer->controls.removeDucksRequested = NO;
        renderer->controls.clearRequested = NO;
        renderer->controls.requestedDisplay = 0;
        return input;
    }
}

void set_hose_brain_shown(HoseRenderer* renderer, bool shown) {
    renderer->controls.showBrain = shown;
    renderer->controls.brainItem.state = shown ? NSControlStateValueOn : NSControlStateValueOff;
}

void set_hose_fish_status(HoseRenderer* renderer, const FishStatus& status) {
    std::string text;
    if (!status.alive && !status.dead) text = "No fish";
    else if (!status.alive) text = status.dead == 1 ? "Your fish died 💀" : "All your fish died 💀";
    else {
        const unsigned alive = status.alive;
        text = alive == 1 ? "Fish: " : std::to_string(alive) + " fish";
        if (alive > 1 && status.young) text += " (" + std::to_string(status.young) + " young)";
        if (status.generations > 1) text += ", " + std::to_string(status.generations) + " generations";
        if (alive > 1) text += ": ";
        std::vector<std::string> notes;
        if (status.gasping) notes.push_back(alive == 1 ? "out of water!" : std::to_string(status.gasping) + " out of water!");
        if (status.starving) notes.push_back(alive == 1 ? "starving" : std::to_string(status.starving) + " starving");
        if (status.hungry) notes.push_back(alive == 1 ? "hungry" : std::to_string(status.hungry) + " hungry");
        if (notes.empty()) notes.push_back(alive == 1 ? "happy" : "all happy");
        for (std::size_t i = 0; i < notes.size(); ++i) text += (i ? ", " : "") + notes[i];
        if (status.dead) text += ", " + std::to_string(status.dead) + " dead";
    }
    if (text == renderer->previous_fish_status) return;
    renderer->previous_fish_status = text;
    @autoreleasepool { renderer->fish_status.title = [NSString stringWithUTF8String:text.c_str()]; }
}

void set_hose_tilt_status(HoseRenderer* renderer, TiltStatus status) {
    if (status == renderer->previous_tilt_status) return;
    renderer->previous_tilt_status = status;
    @autoreleasepool {
        switch (status) {
            case TiltStatus::off: renderer->tilt_status.title = @"Tilt: off"; break;
            case TiltStatus::waiting: renderer->tilt_status.title = @"Tilt: waiting for motion"; break;
            case TiltStatus::active: renderer->tilt_status.title = @"Tilt: active"; break;
            case TiltStatus::unavailable: renderer->tilt_status.title = @"Tilt: unavailable — gravity down"; break;
        }
    }
}

bool run_capture_check() {
    @autoreleasepool {
        if (!CGPreflightScreenCaptureAccess())
            return set_desktop_error("Screen Recording permission is not granted to this process or its terminal");
        ScreenCapture capture;
        capture.start(CGMainDisplayID(), 1.0);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!capture.latest_frame() && capture.state() != CaptureState::failed && capture.state() != CaptureState::denied &&
               std::chrono::steady_clock::now() < deadline)
            [NSRunLoop.mainRunLoop runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
        HoseCaptureFrame* frame = capture.latest_frame();
        capture.stop();
        if (!frame) return set_desktop_error("No desktop frame arrived from ScreenCaptureKit");
        std::printf("PASS: ScreenCaptureKit delivered a %lu × %lu desktop frame as a Metal texture.\n",
                    static_cast<unsigned long>(frame.texture.width), static_cast<unsigned long>(frame.texture.height));
        return true;
    }
}

void set_hose_refraction(HoseRenderer* renderer, bool enabled, const DesktopDisplay& display) {
    const bool restart = enabled && (!renderer->refraction || renderer->refraction_display.id != display.id ||
                                     !same_rect(renderer->refraction_display.bounds, display.bounds));
    renderer->refraction_display = display;
    renderer->refraction = enabled;
    if (!enabled && renderer->capture.state() != CaptureState::off) renderer->capture.stop();
    if (restart) {
        NSScreen* screen = renderer->window.screen;
        renderer->capture.start(display.id, screen ? screen.backingScaleFactor : 2.0);
    }
    const CaptureState state = renderer->capture.state();
    if (state == renderer->previous_capture_state) return;
    renderer->previous_capture_state = state;
    @autoreleasepool {
        switch (state) {
            case CaptureState::off: renderer->refraction_status.title = @"Refraction: off"; break;
            case CaptureState::starting: renderer->refraction_status.title = @"Refraction: starting"; break;
            case CaptureState::active: renderer->refraction_status.title = @"Refraction: active"; break;
            case CaptureState::denied:
                renderer->refraction_status.title = @"Refraction: allow Screen Recording in System Settings, then relaunch"; break;
            case CaptureState::failed: renderer->refraction_status.title = @"Refraction: capture unavailable"; break;
        }
    }
}

bool read_desktop_displays(std::vector<DesktopDisplay>& output) {
    @autoreleasepool {
        NSArray<NSScreen*>* screens = NSScreen.screens;
        if (!screens.count) return set_desktop_error("No accessible macOS desktop displays");
        const double desktop_top = NSMaxY(screens.firstObject.frame);
        output.clear();
        for (NSScreen* screen in screens) {
            const CGDirectDisplayID id = [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
            if (!id) continue;
            const NSRect frame = screen.frame, visible = screen.visibleFrame;
            const auto bounds = desktop_rect_from_appkit(frame.origin.x, frame.origin.y,
                                                         frame.size.width, frame.size.height, desktop_top);
            const auto canvas = desktop_rect_from_appkit(visible.origin.x, visible.origin.y,
                                                         visible.size.width, visible.size.height, desktop_top);
            if (canvas.w < 64 || canvas.h < 64) continue;
            std::string uuid;
            CFUUIDRef identifier = CGDisplayCreateUUIDFromDisplayID(id);
            if (identifier) {
                NSString* value = CFBridgingRelease(CFUUIDCreateString(kCFAllocatorDefault, identifier));
                if (value.UTF8String) uuid = value.UTF8String;
                CFRelease(identifier);
            }
            const char* name = screen.localizedName.UTF8String;
            output.push_back({id, name ? name : "Display", uuid, bounds, canvas, id == CGMainDisplayID()});
        }
        if (output.empty()) return set_desktop_error("No usable desktop display is available");
        return true;
    }
}

std::string saved_hose_display() {
    @autoreleasepool {
        NSUserDefaults* preferences = [[NSUserDefaults alloc] initWithSuiteName:@"local.interactive-experiences.fluid-hose"];
        NSString* uuid = [preferences stringForKey:@"SelectedDisplayUUID"];
        return uuid.UTF8String ? uuid.UTF8String : "";
    }
}

FluidPreferences saved_hose_preferences() {
    @autoreleasepool {
        NSUserDefaults* preferences = [[NSUserDefaults alloc] initWithSuiteName:@"local.interactive-experiences.fluid-hose"];
        FluidPreferences saved;
        if ([preferences objectForKey:@"ParticleSize"])
            saved.fluid.size = static_cast<ParticleSize>(std::clamp<NSInteger>([preferences integerForKey:@"ParticleSize"], 0, 2));
        if ([preferences objectForKey:@"LiquidFeel"])
            saved.fluid.feel = static_cast<LiquidFeel>(std::clamp<NSInteger>([preferences integerForKey:@"LiquidFeel"], 0, 2));
        saved.refraction = [preferences boolForKey:@"Refraction"];
        if ([preferences objectForKey:@"FlowRate"])
            saved.flow = std::clamp(static_cast<float>([preferences doubleForKey:@"FlowRate"]),
                                    FluidSimulation::min_flow, FluidSimulation::max_flow);
        return saved;
    }
}

void save_hose_preferences(const FluidPreferences& saved) {
    @autoreleasepool {
        NSUserDefaults* preferences = [[NSUserDefaults alloc] initWithSuiteName:@"local.interactive-experiences.fluid-hose"];
        [preferences setInteger:static_cast<NSInteger>(saved.fluid.size) forKey:@"ParticleSize"];
        [preferences setInteger:static_cast<NSInteger>(saved.fluid.feel) forKey:@"LiquidFeel"];
        [preferences setBool:saved.refraction forKey:@"Refraction"];
        [preferences setDouble:saved.flow forKey:@"FlowRate"];
    }
}

void save_hose_display(const DesktopDisplay& display) {
    if (display.uuid.empty()) return;
    @autoreleasepool {
        NSUserDefaults* preferences = [[NSUserDefaults alloc] initWithSuiteName:@"local.interactive-experiences.fluid-hose"];
        [preferences setObject:[NSString stringWithUTF8String:display.uuid.c_str()] forKey:@"SelectedDisplayUUID"];
    }
}

void set_hose_displays(HoseRenderer* renderer, const std::vector<DesktopDisplay>& displays, DesktopDisplayID selected) {
    if (renderer->display_options == displays && renderer->selected_display == selected) return;
    renderer->display_options = displays;
    renderer->selected_display = selected;
    @autoreleasepool {
        [renderer->display_menu removeAllItems];
        unsigned index = 0;
        for (const auto& display : displays) {
            NSString* title = [NSString stringWithFormat:@"%u. %s — %d × %d%@", ++index, display.name.c_str(),
                              display.bounds.w, display.bounds.h, display.primary ? @" (main)" : @""];
            NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:title action:@selector(selectDisplay:) keyEquivalent:@""];
            item.target = renderer->controls;
            item.representedObject = @(display.id);
            item.state = display.id == selected ? NSControlStateValueOn : NSControlStateValueOff;
            [renderer->display_menu addItem:item];
        }
        [renderer->display_menu addItem:NSMenuItem.separatorItem];
        NSMenuItem* hint = [[NSMenuItem alloc] initWithTitle:@"Switching displays clears the water" action:nil keyEquivalent:@""];
        hint.enabled = NO;
        [renderer->display_menu addItem:hint];
    }
}

bool draw_hose_frame(HoseRenderer* renderer, FluidSimulation& simulation, const std::vector<PondBody>& bodies,
                     SceneUniforms scene, bool& presented) {
    @autoreleasepool {
        presented = false;
        if (renderer->last_submission.status == MTLCommandBufferStatusError)
            return set_desktop_error("Fluid GPU command failed: %s", renderer->last_submission.error.localizedDescription.UTF8String);
        id<CAMetalDrawable> drawable = renderer->current_drawable;
        if (!drawable) return true;
        dispatch_semaphore_wait(renderer->available, DISPATCH_TIME_FOREVER);
        id<MTLCommandBuffer> commands = [renderer->queue commandBuffer];
        if (!commands) {
            dispatch_semaphore_signal(renderer->available);
            return set_desktop_error("Cannot allocate fluid commands");
        }
        scene.count = simulation.size();
        scene.particle_radius = simulation.splat_radius();
        scene.scale = static_cast<float>(drawable.texture.width) / std::max(scene.width, 1.0f);
        HoseCaptureFrame* background = renderer->refraction ? renderer->capture.latest_frame() : nil;
        if (background) {
            const auto& display = renderer->refraction_display;
            scene.has_background = 1;
            scene.background_x = static_cast<float>(display.canvas.x - display.bounds.x);
            scene.background_y = static_cast<float>(display.canvas.y - display.bounds.y);
            scene.background_width = static_cast<float>(display.bounds.w);
            scene.background_height = static_cast<float>(display.bounds.h);
        }
        if (!simulation.encode(commands) ||
            !renderer->encode(commands, drawable.texture, simulation.particle_buffer(), scene, background.texture, bodies)) {
            dispatch_semaphore_signal(renderer->available);
            return false;
        }
        dispatch_semaphore_t completion = renderer->available;
        // The block keeps the captured frame alive until the GPU has sampled it.
        [commands addCompletedHandler:^(id<MTLCommandBuffer> finished) {
            (void)finished;
            (void)background;
            dispatch_semaphore_signal(completion);
        }];
        // CAMetalDisplayLink supplies this drawable. Submit before presenting it.
        [commands commit];
        [drawable present];
        renderer->last_submission = commands;
        presented = true;
        return true;
    }
}

static id<MTLTexture> offscreen_target(id<MTLDevice> device, NSUInteger width, NSUInteger height) {
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                       width:width height:height mipmapped:NO];
    descriptor.usage = MTLTextureUsageRenderTarget;
    descriptor.storageMode = MTLStorageModeShared;
    return [device newTextureWithDescriptor:descriptor];
}

bool smoke_test_hose_renderer(const char* shader_file) {
    @autoreleasepool {
        HoseRenderer renderer;
        if (!renderer.initialize(shader_file)) return false;
        id<MTLTexture> texture = offscreen_target(renderer.device, 64, 64);
        if (!texture) return set_desktop_error("Cannot allocate fluid test texture");
        // A small packed blob of water in the middle of the target.
        std::vector<FluidParticle> particles;
        for (int y = -2; y <= 2; ++y)
            for (int x = -2; x <= 2; ++x) particles.push_back({32 + x * 4.5f, 32 + y * 4.5f, 0, 0, 0, 0, 0, 0});
        id<MTLBuffer> buffer = [renderer.device newBufferWithBytes:particles.data() length:particles.size() * sizeof(FluidParticle)
                                                           options:MTLResourceStorageModeShared];
        SceneUniforms scene{};
        scene.width = 64;
        scene.height = 64;
        scene.particle_radius = 9;  // matches the blob's 4.5-point spacing
        scene.scale = 1;
        scene.count = static_cast<fluid_uint>(particles.size());
        id<MTLCommandBuffer> commands = [renderer.queue commandBuffer];
        if (!buffer || !commands || !renderer.encode(commands, texture, buffer, scene)) return false;
        [commands commit];
        [commands waitUntilCompleted];
        if (commands.status == MTLCommandBufferStatusError)
            return set_desktop_error("Fluid smoke-test command failed: %s", commands.error.localizedDescription.UTF8String);
        std::vector<unsigned char> pixels(64 * 64 * 4);
        [texture getBytes:pixels.data() bytesPerRow:256 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
        const auto* center = pixels.data() + (32 * 64 + 32) * 4;
        const bool premultiplied = center[0] <= center[3] && center[1] <= center[3] && center[2] <= center[3];
        if (center[3] < 90 || center[3] > 240 || center[0] <= center[2] / 2 || !premultiplied ||
            pixels[0] != 0 || pixels[1] != 0 || pixels[2] != 0 || pixels[3] != 0)
            return set_desktop_error("Fluid did not render a translucent blue surface on a transparent background "
                                     "(centre BGRA %u %u %u %u)", center[0], center[1], center[2], center[3]);
        std::puts("PASS: Metal splats merge into a translucent, premultiplied water surface on a transparent background.");
        return true;
    }
}

bool run_hose_window_checks(const char* shader_file) {
    @autoreleasepool {
        std::vector<DesktopDisplay> displays;
        if (!read_desktop_displays(displays)) return false;
        std::unique_ptr<HoseRenderer, decltype(&destroy_hose_renderer)> renderer(
            create_hose_renderer(displays.front().canvas, shader_file, false, false, {}, false, 1.0f), destroy_hose_renderer);
        if (!renderer) return false;
        show_hose_window(renderer.get());
        for (std::size_t index = 0; index < displays.size(); ++index) {
            const auto& display = displays[index];
            const auto& canvas = display.canvas;
            renderer->controls.quitRequested = NO;
            set_hose_displays(renderer.get(), displays, display.id);
            [renderer->display_menu performActionForItemAtIndex:index];
            if (poll_desktop_input(renderer.get(), canvas).display != display.id)
                return set_desktop_error("Native Display menu did not dispatch the selected monitor");
            SceneUniforms scene{};
            scene.width = static_cast<float>(canvas.w);
            scene.height = static_cast<float>(canvas.h);
            FluidSimulation simulation;
            if (!simulation.initialize(shader_file, scene.width, scene.height)) return false;
            simulation.advance(30, {}, true, {scene.width * 0.5f, scene.height * 0.5f});
            unsigned frames = 0;
            bool moved = false;
            bool draw_success = true;
            bool dimensions_match = true;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            run_hose_event_loop(renderer.get(), [&]() {
                if (!moved) {
                    moved = true;
                    move_hose_window(renderer.get(), canvas);
                    return true;
                }
                const auto texture = renderer->current_drawable.texture;
                const CGFloat scale = renderer->window.backingScaleFactor;
                dimensions_match = dimensions_match &&
                    texture.width == static_cast<NSUInteger>(std::lround(canvas.w * scale)) &&
                    texture.height == static_cast<NSUInteger>(std::lround(canvas.h * scale));
                bool presented = false;
                draw_success = draw_hose_frame(renderer.get(), simulation, {}, scene, presented);
                if (!draw_success) return false;
                if (presented && ++frames == 3)
                    [renderer->status_item.menu performActionForItemAtIndex:renderer->status_item.menu.numberOfItems - 1];
                return true;
            }, [&]() { return std::chrono::steady_clock::now() >= deadline; });
            if (!draw_success) return false;
            if (frames < 3 || !renderer->controls.quitRequested)
                return set_desktop_error("Native display link or menu quit did not complete on %s", display.name.c_str());
            if (!dimensions_match)
                return set_desktop_error("Metal drawable size does not match native backing scale on %s", display.name.c_str());
            if (!renderer->window.ignoresMouseEvents || renderer->window.isKeyWindow ||
                renderer->window.canBecomeKeyWindow || renderer->window.isOpaque ||
                renderer->view.isOpaque || renderer->layer.isOpaque)
                return set_desktop_error("Native overlay lost its click-through, non-focusing, or transparent behavior");

            NSArray* windows = CFBridgingRelease(CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow,
                                                                           static_cast<CGWindowID>(renderer->window.windowNumber)));
            NSDictionary* info = windows.firstObject;
            NSDictionary* bounds = info[(__bridge NSString*)kCGWindowBounds];
            CGRect actual;
            if (!bounds || !CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)bounds, &actual) ||
                std::abs(actual.origin.x - canvas.x) > 1 || std::abs(actual.origin.y - canvas.y) > 1 ||
                std::abs(actual.size.width - canvas.w) > 1 || std::abs(actual.size.height - canvas.h) > 1)
                return set_desktop_error("WindowServer bounds do not match the selected canvas on %s", display.name.c_str());
            if (renderer->last_submission) [renderer->last_submission waitUntilCompleted];
            if (renderer->last_submission.status == MTLCommandBufferStatusError)
                return set_desktop_error("Live Metal rendering failed: %s", renderer->last_submission.error.localizedDescription.UTF8String);
            std::printf("PASS: %s — native window at (%d,%d), %d × %d points, %.0f× backing, %u frames, Display/Quit menu actions.\n",
                        display.name.c_str(), canvas.x, canvas.y, canvas.w, canvas.h,
                        renderer->window.backingScaleFactor, frames);
        }
        std::puts("PASS: native overlays remain transparent, click-through, and unable to take keyboard focus.");
        return true;
    }
}

namespace {
struct SnapshotWindow { float x, y, width, height; };

// A synthetic desktop: a soft wallpaper gradient and plain light windows.
void paint_desktop(std::vector<unsigned char>& rgba, int width, int height, float scale,
                   const std::vector<SnapshotWindow>& windows) {
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float u = x / static_cast<float>(width), v = y / static_cast<float>(height);
            unsigned char* pixel = rgba.data() + (y * width + x) * 4;
            const float px = x / scale, py = y / scale;
            // Fine grid lines and soft circles give refraction something to bend.
            const bool grid = std::fmod(px, 40.0f) < 1.0f || std::fmod(py, 40.0f) < 1.0f;
            const float ring = std::abs(std::hypot(std::fmod(px, 160.0f) - 80, std::fmod(py, 160.0f) - 80) - 34);
            const float light = (grid ? 28.0f : 0.0f) + (ring < 2.0f ? 40.0f : 0.0f);
            pixel[0] = static_cast<unsigned char>(std::min(255.0f, 60 + 90 * u + light));
            pixel[1] = static_cast<unsigned char>(std::min(255.0f, 70 + 50 * v + light));
            pixel[2] = static_cast<unsigned char>(std::min(255.0f, 120 + 60 * (1 - v) + light));
            pixel[3] = 255;
            for (const auto& w : windows) {
                const float hx = w.width * 0.5f, hy = w.height * 0.5f, r = 10;
                const float qx = std::abs(px - w.x - hx) - (hx - r), qy = std::abs(py - w.y - hy) - (hy - r);
                const float d = std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - r;
                if (d > 0) continue;
                const bool title = py - w.y < 28;
                // Rows of dark bars stand in for text.
                const float line = std::fmod(py - w.y - 40, 18.0f);
                const bool text = !title && py - w.y > 40 && line < 7 && px - w.x > 16 && px < w.x + w.width - 40 &&
                                  std::fmod(px - w.x + std::floor((py - w.y) / 18) * 23, 70.0f) < 52;
                pixel[0] = pixel[1] = pixel[2] = title ? 214 : text ? 90 : 246;
            }
        }
    }
}
}

bool render_hose_snapshot(const char* shader_file, const char* output_file, float seconds, const FluidSettings& settings,
                          bool refraction, float flow, unsigned duck_count, unsigned fish_count) {
    @autoreleasepool {
        constexpr float width = 960, height = 600, scale = 2;
        HoseRenderer renderer;
        if (!renderer.initialize(shader_file)) return false;
        FluidSimulation simulation;
        if (!simulation.initialize(shader_file, width, height, settings)) return false;
        simulation.set_flow_rate(flow);
        Pond pond;
        pond.reset(width, height);
        std::vector<BodySample> water;
        // A shelf window catches the stream; a second window slides into the
        // pool after a few seconds and pushes the water back.
        const int steps = static_cast<int>(seconds / FluidSimulation::step_time);
        std::vector<SnapshotWindow> windows;
        const FluidPoint nozzle{300, 40};
        for (int i = 0; i < steps; ++i) {
            const float t = i * FluidSimulation::step_time;
            const float slide = t < 5 ? 0 : std::min(1.0f, (t - 5) / 1.2f);
            windows = {{120, 200, 380, 230}, {980 - 380 * slide, 380, 300, 260}};
            std::vector<WindowObstacle> obstacles;
            for (std::size_t w = 0; w < windows.size(); ++w) {
                const auto& r = windows[w];
                obstacles.push_back({r.x, r.y, r.width, r.height, r.x, r.y, r.width, r.height, 10,
                                     static_cast<fluid_uint>(w + 1), 0, 0});
            }
            // Ducks drop in one by one across the scene; fish go into the pool
            // once it has formed, and food is sprinkled over it near the end.
            if (pond.duck_count() < duck_count && t >= 1.0f + pond.duck_count() * 0.7f)
                pond.add_duck({150.0f + std::fmod(pond.duck_count() * 263.0f, width - 300), 40});
            if (pond.fish_count() < fish_count && t >= 3.0f + pond.fish_count() * 0.5f) {
                // A family portrait: a female, a male, then young of every age,
                // one of them spotted.
                const std::size_t k = pond.fish_count();
                FishGenome::Random genes{static_cast<std::uint32_t>(97 + k * 31)};
                FishBirth birth;
                birth.genome = FishGenome::founder(genes);
                birth.female = k % 2 == 0;
                birth.growth = k < 2 ? 1.0f : k == 2 ? 0.55f : k == 3 ? 0.15f : 0.75f;
                if (k == 4) birth.genome.stripes = 0.05f;
                if (k == 1) birth.genome.hue = 0.8f;
                pond.add_fish({120.0f + std::fmod(k * 157.0f, 400.0f), height - 40}, birth);
            }
            if (fish_count && t >= seconds - 4 && t < seconds - 3 && i % 12 == 0)
                pond.add_food({100.0f + std::fmod(i * 37.0f, 400.0f), height - 160});
            pond.set_windows(obstacles);
            if (!simulation.latest_body_samples(water)) water.clear();
            pond.step(FluidSimulation::step_time, 1, {0, 950}, water, simulation.particle_area());
            simulation.set_bodies(pond.bodies());
            simulation.advance(1, obstacles, true, nozzle);
            // With bodies, flush every step so buoyancy sees the current water.
            if ((duck_count || fish_count || i % 120 == 119) && !simulation.flush()) return false;
        }
        id<MTLTexture> texture = offscreen_target(renderer.device, width * scale, height * scale);
        const int w = static_cast<int>(texture.width), h = static_cast<int>(texture.height);
        std::vector<unsigned char> overlay(w * h * 4), image(w * h * 4);
        paint_desktop(image, w, h, scale, windows);
        // With refraction, the painted desktop stands in for a captured frame.
        id<MTLTexture> background = nil;
        if (refraction) {
            MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                               width:w height:h mipmapped:NO];
            background = [renderer.device newTextureWithDescriptor:descriptor];
            [background replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:image.data() bytesPerRow:w * 4];
        }
        id<MTLCommandBuffer> commands = [renderer.queue commandBuffer];
        SceneUniforms scene{};
        scene.width = width;
        scene.height = height;
        scene.nozzle_x = nozzle.x;
        scene.nozzle_y = nozzle.y;
        scene.show_nozzle = 1;
        scene.time = seconds;
        scene.particle_radius = simulation.splat_radius();
        scene.flow = simulation.flow_rate();
        scene.scale = scale;
        scene.count = simulation.size();
        scene.has_background = background ? 1 : 0;
        scene.background_width = width;
        scene.background_height = height;
        if (!texture || !commands || !simulation.encode(commands) ||
            !renderer.encode(commands, texture, simulation.particle_buffer(), scene, background, pond.bodies())) return false;
        [commands commit];
        [commands waitUntilCompleted];
        if (commands.status == MTLCommandBufferStatusError)
            return set_desktop_error("Snapshot rendering failed: %s", commands.error.localizedDescription.UTF8String);

        [texture getBytes:overlay.data() bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
        for (int i = 0; i < w * h; ++i) {
            // Premultiplied BGRA over the opaque RGBA desktop.
            const float alpha = overlay[i * 4 + 3] / 255.0f;
            for (int c = 0; c < 3; ++c)
                image[i * 4 + c] = static_cast<unsigned char>(std::min(255.0f, overlay[i * 4 + 2 - c] + image[i * 4 + c] * (1 - alpha)));
        }
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGContextRef context = CGBitmapContextCreate(image.data(), w, h, 8, w * 4, space,
                                                     static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast));
        CGImageRef result = context ? CGBitmapContextCreateImage(context) : nullptr;
        NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:output_file]];
        CGImageDestinationRef destination = result ? CGImageDestinationCreateWithURL((__bridge CFURLRef)url, CFSTR("public.png"), 1, nullptr) : nullptr;
        bool written = false;
        if (destination) {
            CGImageDestinationAddImage(destination, result, nullptr);
            written = CGImageDestinationFinalize(destination);
            CFRelease(destination);
        }
        if (result) CGImageRelease(result);
        if (context) CGContextRelease(context);
        CGColorSpaceRelease(space);
        if (!written) return set_desktop_error("Cannot write snapshot %s", output_file);
        std::printf("Snapshot: %u particles after %.1f s -> %s\n", simulation.size(), seconds, output_file);
        return true;
    }
}
