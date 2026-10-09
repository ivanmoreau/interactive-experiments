#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <SDL3/SDL_metal.h>
#include "floating_metal.h"
#include "frame_uniforms.h"
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

struct FloatingMetalRenderer {
    SDL_MetalView view = nullptr;
    CAMetalLayer* layer = nil;
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLRenderPipelineState> pipeline = nil;
    id<MTLCommandBuffer> last_submission = nil;
    std::string device_name;

    ~FloatingMetalRenderer() {
        @autoreleasepool {
            // Drain the queue before removing the view that owns the drawables.
            if (queue) {
                id<MTLCommandBuffer> barrier = [queue commandBuffer];
                [barrier commit];
                [barrier waitUntilCompleted];
            }
            if (view) SDL_Metal_DestroyView(view);
        }
    }

    bool initialize(const char* shader_filename) {
        device = MTLCreateSystemDefaultDevice();
        if (!device) return SDL_SetError("No Metal device is accessible to this process");
        device_name = device.name.UTF8String;
        queue = [device newCommandQueue];
        if (!queue) return SDL_SetError("Cannot create the floating Metal command queue");

        NSString* filename = [NSString stringWithUTF8String:shader_filename];
        if (!filename) return SDL_SetError("Invalid shader path encoding");
        NSError* error = nil;
        id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:filename] error:&error];
        if (!library) {
            return SDL_SetError("Load floating Metal shaders: %s",
                                error ? error.localizedDescription.UTF8String : "unknown error");
        }
        id<MTLFunction> vertex = [library newFunctionWithName:@"hello_vertex"];
        id<MTLFunction> fragment = [library newFunctionWithName:@"hello_fragment"];
        if (!vertex || !fragment) return SDL_SetError("The shader library is missing the triangle functions");

        MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
        descriptor.vertexFunction = vertex;
        descriptor.fragmentFunction = fragment;
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!pipeline) {
            return SDL_SetError("Create floating Metal pipeline: %s",
                                error ? error.localizedDescription.UTF8String : "unknown error");
        }
        return true;
    }

    id<MTLCommandBuffer> encode_frame(id<MTLTexture> texture, float angle) {
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        if (!commands) {
            SDL_SetError("Cannot allocate a floating Metal command buffer");
            return nil;
        }
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
        id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
        if (!encoder) {
            SDL_SetError("Cannot begin the floating Metal render pass");
            return nil;
        }
        const FrameUniforms frame{angle, static_cast<float>(texture.width) / texture.height, 0.0f, 0.0f};
        [encoder setRenderPipelineState:pipeline];
        [encoder setVertexBytes:&frame length:sizeof(frame) atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        return commands;
    }
};

FloatingMetalRenderer* create_floating_metal_renderer(SDL_Window* window, const char* shader_filename) {
    @autoreleasepool {
        auto renderer = std::make_unique<FloatingMetalRenderer>();
        if (!renderer->initialize(shader_filename)) return nullptr;
        renderer->view = SDL_Metal_CreateView(window);
        if (!renderer->view) return nullptr;
        renderer->layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(renderer->view);
        if (!renderer->layer) {
            SDL_SetError("SDL did not provide a Metal layer for the floating window");
            return nullptr;
        }
        renderer->layer.device = renderer->device;
        renderer->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        renderer->layer.framebufferOnly = YES;
        renderer->layer.opaque = NO;
        renderer->layer.displaySyncEnabled = YES;
        renderer->layer.maximumDrawableCount = 3;
        return renderer.release();
    }
}

void destroy_floating_metal_renderer(FloatingMetalRenderer* renderer) {
    delete renderer;
}

const char* floating_metal_device_name(const FloatingMetalRenderer* renderer) {
    return renderer->device_name.c_str();
}

bool draw_floating_metal_frame(FloatingMetalRenderer* renderer, float angle, bool& presented) {
    @autoreleasepool {
        presented = false;
        if (renderer->last_submission.status == MTLCommandBufferStatusError) {
            return SDL_SetError("Floating Metal command failed: %s",
                                renderer->last_submission.error.localizedDescription.UTF8String);
        }
        if (renderer->layer.drawableSize.width <= 0 || renderer->layer.drawableSize.height <= 0) return true;
        id<CAMetalDrawable> drawable = [renderer->layer nextDrawable];
        if (!drawable) return true;
        id<MTLCommandBuffer> commands = renderer->encode_frame(drawable.texture, angle);
        if (!commands) return false;
        [commands presentDrawable:drawable];
        [commands commit];
        renderer->last_submission = commands;
        presented = true;
        return true;
    }
}

bool smoke_test_floating_metal(const char* shader_filename) {
    @autoreleasepool {
        FloatingMetalRenderer renderer;
        if (!renderer.initialize(shader_filename)) return false;
        std::printf("Backend: native Metal (floating-mode offscreen check)\nDevice: %s\n",
                    renderer.device_name.c_str());

        constexpr NSUInteger size = 64;
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                           width:size
                                                                                          height:size
                                                                                       mipmapped:NO];
        descriptor.usage = MTLTextureUsageRenderTarget;
        descriptor.storageMode = MTLStorageModeShared;
        id<MTLTexture> texture = [renderer.device newTextureWithDescriptor:descriptor];
        if (!texture) return SDL_SetError("Cannot allocate the native Metal smoke-test texture");
        id<MTLCommandBuffer> commands = renderer.encode_frame(texture, 0.0f);
        if (!commands) return false;
        [commands commit];
        [commands waitUntilCompleted];
        if (commands.status == MTLCommandBufferStatusError) {
            return SDL_SetError("Native Metal smoke-test command failed: %s", commands.error.localizedDescription.UTF8String);
        }
        std::vector<unsigned char> pixels(size * size * 4);
        [texture getBytes:pixels.data() bytesPerRow:size * 4 fromRegion:MTLRegionMake2D(0, 0, size, size) mipmapLevel:0];
        const auto* center = pixels.data() + (32 * size + 32) * 4;
        const auto* corner = pixels.data();
        if (center[0] + center[1] + center[2] < 180 || center[3] != 255 ||
            corner[0] != 0 || corner[1] != 0 || corner[2] != 0 || corner[3] != 0) {
            return SDL_SetError("Native Metal readback did not contain an opaque triangle and transparent background");
        }
        std::puts("PASS: native Metal triangle with opaque center and transparent background.");
        return true;
    }
}
