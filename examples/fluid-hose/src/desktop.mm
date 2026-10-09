#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <SDL3/SDL_metal.h>
#include "desktop.h"
#include "fluid_sim.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unistd.h>

@interface FluidMenuController : NSObject
@property(nonatomic) BOOL pouring;
@property(nonatomic) BOOL paused;
@property(nonatomic) BOOL clearRequested;
@property(nonatomic) BOOL quitRequested;
@property(nonatomic) BOOL tiltEnabled;
@property(nonatomic) BOOL reverseTilt;
- (void)togglePour:(NSMenuItem*)sender;
- (void)togglePause:(NSMenuItem*)sender;
- (void)clearWater:(NSMenuItem*)sender;
- (void)quit:(NSMenuItem*)sender;
- (void)toggleTilt:(NSMenuItem*)sender;
- (void)toggleReverseTilt:(NSMenuItem*)sender;
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
@end

struct HoseRenderer {
    SDL_MetalView view = nullptr;
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
            if (view) SDL_Metal_DestroyView(view);
        }
    }

    bool initialize(const char* shader_file) {
        device = MTLCreateSystemDefaultDevice();
        if (!device) return SDL_SetError("No Metal device is accessible to this process");
        device_name = device.name.UTF8String;
        queue = [device newCommandQueue];
        if (!queue) return SDL_SetError("Cannot create the fluid command queue");
        NSError* error = nil;
        NSString* filename = [NSString stringWithUTF8String:shader_file];
        if (!filename) return SDL_SetError("Invalid shader path encoding");
        id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:filename] error:&error];
        if (!library) return SDL_SetError("Load fluid shaders: %s", error.localizedDescription.UTF8String);

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
        if (!density_pipeline) return SDL_SetError("Create fluid density pipeline: %s", error.localizedDescription.UTF8String);

        descriptor = [[MTLRenderPipelineDescriptor alloc] init];
        descriptor.vertexFunction = [library newFunctionWithName:@"surface_vertex"];
        descriptor.fragmentFunction = [library newFunctionWithName:@"surface_fragment"];
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        surface_pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!surface_pipeline) return SDL_SetError("Create fluid surface pipeline: %s", error.localizedDescription.UTF8String);
        for (auto& buffer : buffers) {
            buffer = [device newBufferWithLength:FluidSimulation::capacity * sizeof(FluidVertex) options:MTLResourceStorageModeShared];
            if (!buffer) return SDL_SetError("Cannot allocate fluid particle buffers");
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
            if (!field) { SDL_SetError("Cannot allocate the fluid density field"); return nil; }
        }
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        if (!commands) { SDL_SetError("Cannot allocate fluid commands"); return nil; }
        const std::size_t count = std::min(particles.size(), FluidSimulation::capacity);
        if (count) std::memcpy(buffers[slot].contents, particles.data(), count * sizeof(FluidVertex));

        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = field;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
        if (!encoder) { SDL_SetError("Cannot begin the fluid density pass"); return nil; }
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
        if (!encoder) { SDL_SetError("Cannot begin the fluid surface pass"); return nil; }
        [encoder setRenderPipelineState:surface_pipeline];
        [encoder setFragmentBytes:&scene length:sizeof(scene) atIndex:0];
        [encoder setFragmentTexture:field atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        return commands;
    }
};

HoseRenderer* create_hose_renderer(SDL_Window* window, const char* shader_file, bool continuous, bool tilt) {
    @autoreleasepool {
        auto renderer = std::make_unique<HoseRenderer>();
        if (!renderer->initialize(shader_file)) return nullptr;
        renderer->view = SDL_Metal_CreateView(window);
        if (!renderer->view) return nullptr;
        renderer->layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(renderer->view);
        if (!renderer->layer) { SDL_SetError("SDL did not provide a Metal layer"); return nullptr; }
        renderer->layer.device = renderer->device;
        renderer->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        renderer->layer.opaque = NO;
        renderer->layer.framebufferOnly = YES;
        renderer->layer.displaySyncEnabled = YES;
        renderer->layer.maximumDrawableCount = 3;

        NSWindow* native_window = (__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                                            SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
        if (!native_window) { SDL_SetError("Fluid Hose requires a Cocoa window"); return nullptr; }

        // SDL's Cocoa mouse handler recomputes ignoresMouseEvents from the shape
        // on pointer movement. An empty input shape keeps the overlay click-through;
        // the native Metal layer draws the visible water independently on macOS.
        std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> input_shape(
            SDL_CreateSurface(2, 2, SDL_PIXELFORMAT_RGBA32), SDL_DestroySurface);
        if (!input_shape || !SDL_ClearSurface(input_shape.get(), 0, 0, 0, 0) ||
            !SDL_SetWindowShape(window, input_shape.get())) return nullptr;
        native_window.ignoresMouseEvents = YES;
        native_window.hasShadow = NO;
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

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
const char* hose_device_name(const HoseRenderer* renderer) { return renderer->device_name.c_str(); }

DesktopInput poll_desktop_input(HoseRenderer* renderer, const SDL_Rect& canvas) {
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
                           static_cast<bool>(renderer->controls.tiltEnabled), static_cast<bool>(renderer->controls.reverseTilt)};
        renderer->controls.clearRequested = NO;
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

bool read_window_obstacles(const SDL_Rect& canvas, std::vector<WindowRect>& output) {
    @autoreleasepool {
        NSArray* windows = CFBridgingRelease(CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly |
                                                                      kCGWindowListExcludeDesktopElements, kCGNullWindowID));
        if (!windows) return SDL_SetError("Window geometry is unavailable: no accessible Quartz desktop session");
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
            return SDL_SetError("Fluid GPU command failed: %s", renderer->last_submission.error.localizedDescription.UTF8String);
        id<CAMetalDrawable> drawable = [renderer->layer nextDrawable];
        if (!drawable) return true;
        dispatch_semaphore_wait(renderer->available, DISPATCH_TIME_FOREVER);
        id<MTLCommandBuffer> commands = renderer->encode(drawable.texture, particles, scene);
        if (!commands) { dispatch_semaphore_signal(renderer->available); return false; }
        dispatch_semaphore_t completion = renderer->available;
        [commands addCompletedHandler:^(id<MTLCommandBuffer> finished) { (void)finished; dispatch_semaphore_signal(completion); }];
        [commands presentDrawable:drawable];
        [commands commit];
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
        if (!texture) return SDL_SetError("Cannot allocate fluid test texture");
        const std::vector<FluidVertex> particles{{24, 32, 12, 0}, {40, 32, 12, 0}};
        const SceneUniforms scene{64, 64, 0, 0, 0, 0, 0, 0};
        id<MTLCommandBuffer> commands = renderer.encode(texture, particles, scene);
        if (!commands) return false;
        [commands commit];
        [commands waitUntilCompleted];
        if (commands.status == MTLCommandBufferStatusError)
            return SDL_SetError("Fluid smoke-test command failed: %s", commands.error.localizedDescription.UTF8String);
        std::vector<unsigned char> pixels(64 * 64 * 4);
        [texture getBytes:pixels.data() bytesPerRow:256 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
        const auto* center = pixels.data() + (32 * 64 + 32) * 4;
        if (center[3] < 170 || center[3] > 205 || center[0] + center[1] + center[2] < 80 ||
            pixels[0] != 0 || pixels[1] != 0 || pixels[2] != 0 || pixels[3] != 0)
            return SDL_SetError("Fluid density did not merge into a translucent surface on a transparent background");
        std::puts("PASS: Metal density accumulation, translucent merged water surface, and transparent background.");
        return true;
    }
}
