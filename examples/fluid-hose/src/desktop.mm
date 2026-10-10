#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CAMetalDisplayLink.h>
#include "desktop.h"
#include "fluid_sim.h"
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

static bool set_desktop_error(const char* format, ...) {
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
- (void)togglePour:(NSMenuItem*)sender;
- (void)togglePause:(NSMenuItem*)sender;
- (void)clearWater:(NSMenuItem*)sender;
- (void)quit:(NSMenuItem*)sender;
- (void)toggleTilt:(NSMenuItem*)sender;
- (void)toggleReverseTilt:(NSMenuItem*)sender;
- (void)selectDisplay:(NSMenuItem*)sender;
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
    id<MTLRenderPipelineState> density_pipeline = nil;
    id<MTLRenderPipelineState> surface_pipeline = nil;
    id<MTLTexture> field = nil;
    id<MTLBuffer> buffers[3] = {nil, nil, nil};
    id<MTLCommandBuffer> last_submission = nil;
    dispatch_semaphore_t available = dispatch_semaphore_create(3);
    unsigned slot = 0;
    NSStatusItem* status_item = nil;
    FluidMenuController* controls = nil;
    NSMenuItem* tilt_status = nil;
    NSMenu* display_menu = nil;
    std::vector<DesktopDisplay> display_options;
    DesktopDisplayID selected_display = 0;
    TiltStatus previous_tilt_status = TiltStatus::off;
    std::string device_name;

    ~HoseRenderer() {
        @autoreleasepool {
            if (status_item) [NSStatusBar.systemStatusBar removeStatusItem:status_item];
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
        device = MTLCreateSystemDefaultDevice();
        if (!device) return set_desktop_error("No Metal device is accessible to this process");
        device_name = device.name.UTF8String;
        queue = [device newCommandQueue];
        if (!queue) return set_desktop_error("Cannot create the fluid command queue");
        NSError* error = nil;
        NSString* filename = [NSString stringWithUTF8String:shader_file];
        if (!filename) return set_desktop_error("Invalid shader path encoding");
        id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:filename] error:&error];
        if (!library) return set_desktop_error("Load fluid shaders: %s", error.localizedDescription.UTF8String);

        MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
        descriptor.vertexFunction = [library newFunctionWithName:@"density_vertex"];
        descriptor.fragmentFunction = [library newFunctionWithName:@"density_fragment"];
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatR16Float;
        descriptor.colorAttachments[0].blendingEnabled = YES;
        descriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
        descriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
        descriptor.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
        descriptor.colorAttachments[0].writeMask = MTLColorWriteMaskRed;
        density_pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!density_pipeline) return set_desktop_error("Create fluid density pipeline: %s", error.localizedDescription.UTF8String);

        descriptor = [[MTLRenderPipelineDescriptor alloc] init];
        descriptor.vertexFunction = [library newFunctionWithName:@"surface_vertex"];
        descriptor.fragmentFunction = [library newFunctionWithName:@"surface_fragment"];
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        surface_pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!surface_pipeline) return set_desktop_error("Create fluid surface pipeline: %s", error.localizedDescription.UTF8String);
        for (auto& buffer : buffers) {
            buffer = [device newBufferWithLength:FluidSimulation::capacity * sizeof(FluidVertex) options:MTLResourceStorageModeShared];
            if (!buffer) return set_desktop_error("Cannot allocate fluid particle buffers");
        }
        return true;
    }

    id<MTLCommandBuffer> encode(id<MTLTexture> target, const std::vector<FluidVertex>& particles,
                                const SceneUniforms& scene) {
        if (!field || field.width != target.width || field.height != target.height) {
            MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR16Float
                                                                                               width:target.width height:target.height mipmapped:NO];
            descriptor.storageMode = MTLStorageModePrivate;
            descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            field = [device newTextureWithDescriptor:descriptor];
            if (!field) { set_desktop_error("Cannot allocate the fluid density field"); return nil; }
        }
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        if (!commands) { set_desktop_error("Cannot allocate fluid commands"); return nil; }
        const std::size_t count = std::min(particles.size(), FluidSimulation::capacity);
        if (count) std::memcpy(buffers[slot].contents, particles.data(), count * sizeof(FluidVertex));

        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = field;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
        if (!encoder) { set_desktop_error("Cannot begin the fluid density pass"); return nil; }
        if (count) {
            [encoder setRenderPipelineState:density_pipeline];
            [encoder setVertexBytes:&scene length:sizeof(scene) atIndex:0];
            [encoder setVertexBuffer:buffers[slot] offset:0 atIndex:1];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6 instanceCount:count];
        }
        [encoder endEncoding];

        pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = target;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        encoder = [commands renderCommandEncoderWithDescriptor:pass];
        if (!encoder) { set_desktop_error("Cannot begin the fluid surface pass"); return nil; }
        [encoder setRenderPipelineState:surface_pipeline];
        [encoder setFragmentBytes:&scene length:sizeof(scene) atIndex:0];
        [encoder setFragmentTexture:field atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        return commands;
    }
};

HoseRenderer* create_hose_renderer(const DesktopRect& canvas, const char* shader_file, bool continuous, bool tilt) {
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
        renderer->layer.opaque = NO;
        renderer->layer.framebufferOnly = YES;
        renderer->layer.displaySyncEnabled = YES;
        renderer->layer.maximumDrawableCount = 3;
        renderer->window.contentView = renderer->view;
        [renderer->view updateDrawableSize];

        renderer->controls = [[FluidMenuController alloc] init];
        renderer->controls.pouring = continuous;
        renderer->controls.tiltEnabled = tilt;
        renderer->status_item = [NSStatusBar.systemStatusBar statusItemWithLength:NSVariableStatusItemLength];
        renderer->status_item.button.title = @"💧";
        renderer->status_item.button.toolTip = @"Fluid Hose — hold Option to pour";
        NSMenu* menu = [[NSMenu alloc] initWithTitle:@"Fluid Hose"];
        NSMenuItem* hint = [[NSMenuItem alloc] initWithTitle:@"Hold Option to pour at the pointer" action:nil keyEquivalent:@""];
        hint.enabled = NO;
        [menu addItem:hint];
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
        const bool option = (CGEventSourceFlagsState(kCGEventSourceStateCombinedSessionState) & kCGEventFlagMaskAlternate) != 0;
        DesktopInput input{pointer, option || static_cast<bool>(renderer->controls.pouring),
                           static_cast<bool>(renderer->controls.paused), static_cast<bool>(renderer->controls.clearRequested),
                           static_cast<bool>(renderer->controls.quitRequested),
                           static_cast<bool>(renderer->controls.tiltEnabled), static_cast<bool>(renderer->controls.reverseTilt),
                           renderer->controls.requestedDisplay};
        renderer->controls.clearRequested = NO;
        renderer->controls.requestedDisplay = 0;
        return input;
    }
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

bool read_window_obstacles(const DesktopRect& canvas, std::vector<WindowRect>& output) {
    @autoreleasepool {
        NSArray* windows = CFBridgingRelease(CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly |
                                                                      kCGWindowListExcludeDesktopElements, kCGNullWindowID));
        if (!windows) return set_desktop_error("Window geometry is unavailable: no accessible Quartz desktop session");
        output.clear();
        for (NSDictionary* info in windows) {
            NSNumber* layer = info[(__bridge NSString*)kCGWindowLayer];
            NSNumber* owner = info[(__bridge NSString*)kCGWindowOwnerPID];
            NSNumber* alpha = info[(__bridge NSString*)kCGWindowAlpha];
            if (!layer || layer.intValue != 0 || owner.intValue == getpid() || alpha.doubleValue <= 0.01) continue;
            NSDictionary* bounds = info[(__bridge NSString*)kCGWindowBounds];
            CGRect rect;
            if (!bounds || !CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)bounds, &rect)) continue;
            WindowRect obstacle{static_cast<float>(rect.origin.x) - canvas.x, static_cast<float>(rect.origin.y) - canvas.y,
                                static_cast<float>(rect.size.width), static_cast<float>(rect.size.height)};
            if (obstacle.width < 10 || obstacle.height < 10 || obstacle.x >= canvas.w || obstacle.y >= canvas.h ||
                obstacle.x + obstacle.width <= 0 || obstacle.y + obstacle.height <= 0) continue;
            // A solid rectangle covering the entire canvas leaves no space for
            // a fluid simulation, so fullscreen/maximized covering windows are omitted.
            if (obstacle.x <= 0 && obstacle.y <= 0 && obstacle.x + obstacle.width >= canvas.w &&
                obstacle.y + obstacle.height >= canvas.h) continue;
            output.push_back(obstacle);
            if (output.size() == 128) break;
        }
        return true;
    }
}

bool draw_hose_frame(HoseRenderer* renderer, const std::vector<FluidVertex>& particles,
                     const SceneUniforms& scene, bool& presented) {
    @autoreleasepool {
        presented = false;
        if (renderer->last_submission.status == MTLCommandBufferStatusError)
            return set_desktop_error("Fluid GPU command failed: %s", renderer->last_submission.error.localizedDescription.UTF8String);
        id<CAMetalDrawable> drawable = renderer->current_drawable;
        if (!drawable) return true;
        dispatch_semaphore_wait(renderer->available, DISPATCH_TIME_FOREVER);
        id<MTLCommandBuffer> commands = renderer->encode(drawable.texture, particles, scene);
        if (!commands) { dispatch_semaphore_signal(renderer->available); return false; }
        dispatch_semaphore_t completion = renderer->available;
        [commands addCompletedHandler:^(id<MTLCommandBuffer> finished) { (void)finished; dispatch_semaphore_signal(completion); }];
        // CAMetalDisplayLink supplies this drawable. Submit before presenting it.
        [commands commit];
        [drawable present];
        renderer->last_submission = commands;
        renderer->slot = (renderer->slot + 1) % 3;
        presented = true;
        return true;
    }
}

bool smoke_test_hose_renderer(const char* shader_file) {
    @autoreleasepool {
        HoseRenderer renderer;
        if (!renderer.initialize(shader_file)) return false;
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                           width:64 height:64 mipmapped:NO];
        descriptor.usage = MTLTextureUsageRenderTarget;
        descriptor.storageMode = MTLStorageModeShared;
        id<MTLTexture> texture = [renderer.device newTextureWithDescriptor:descriptor];
        if (!texture) return set_desktop_error("Cannot allocate fluid test texture");
        const std::vector<FluidVertex> particles{{24, 32, 12, 0}, {40, 32, 12, 0}};
        const SceneUniforms scene{64, 64, 0, 0, 0, 0, 0, 0};
        id<MTLCommandBuffer> commands = renderer.encode(texture, particles, scene);
        if (!commands) return false;
        [commands commit];
        [commands waitUntilCompleted];
        if (commands.status == MTLCommandBufferStatusError)
            return set_desktop_error("Fluid smoke-test command failed: %s", commands.error.localizedDescription.UTF8String);
        std::vector<unsigned char> pixels(64 * 64 * 4);
        [texture getBytes:pixels.data() bytesPerRow:256 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
        const auto* center = pixels.data() + (32 * 64 + 32) * 4;
        if (center[3] < 170 || center[3] > 205 || center[0] + center[1] + center[2] < 80 ||
            pixels[0] != 0 || pixels[1] != 0 || pixels[2] != 0 || pixels[3] != 0)
            return set_desktop_error("Fluid density did not merge into a translucent surface on a transparent background");
        std::puts("PASS: Metal density accumulation, translucent merged water surface, and transparent background.");
        return true;
    }
}

bool run_hose_window_checks(const char* shader_file) {
    @autoreleasepool {
        std::vector<DesktopDisplay> displays;
        if (!read_desktop_displays(displays)) return false;
        std::unique_ptr<HoseRenderer, decltype(&destroy_hose_renderer)> renderer(
            create_hose_renderer(displays.front().canvas, shader_file, false), destroy_hose_renderer);
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
            const SceneUniforms scene{static_cast<float>(canvas.w), static_cast<float>(canvas.h), 0, 0, 0, 0, 0, 0};
            const std::vector<FluidVertex> particles{{canvas.w * 0.5f, canvas.h * 0.5f, 12, 0}};
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
                draw_success = draw_hose_frame(renderer.get(), particles, scene, presented);
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
