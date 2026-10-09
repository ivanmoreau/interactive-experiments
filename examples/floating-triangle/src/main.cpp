#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gpu.h>
#include "floating_metal.h"
#include "frame_uniforms.h"

#include <mach-o/dyld.h>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static SDL_HitTestResult SDLCALL drag_floating_window(SDL_Window*, const SDL_Point*, void*) {
    return SDL_HITTEST_DRAGGABLE;
}

static void require(bool success, const char* operation) {
    if (!success) {
        throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
    }
}

template <typename T>
static T* require(T* value, const char* operation) {
    require(value != nullptr, operation);
    return value;
}

// Resources live beside the CLI executable or in an app bundle's Resources.
static std::filesystem::path shader_path() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> path(size);
    if (_NSGetExecutablePath(path.data(), &size) != 0) {
        throw std::runtime_error("Cannot locate the executable");
    }
    auto directory = std::filesystem::canonical(path.data()).parent_path();
    if (directory.filename() == "MacOS") {
        directory = directory.parent_path() / "Resources";
    }
    return directory / "hello.metallib";
}

struct App {
    SDL_GPUDevice* device = nullptr;
    SDL_Window* window = nullptr;
    bool window_claimed = false;
    FloatingMetalRenderer* floating_renderer = nullptr;
    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    SDL_GPUTexture* test_texture = nullptr;
    SDL_GPUTransferBuffer* test_download = nullptr;
    SDL_GPUFence* test_fence = nullptr;

    ~App() {
        if (floating_renderer) destroy_floating_metal_renderer(floating_renderer);
        if (device) {
            SDL_WaitForGPUIdle(device);
            if (test_fence) SDL_ReleaseGPUFence(device, test_fence);
            if (test_download) SDL_ReleaseGPUTransferBuffer(device, test_download);
            if (test_texture) SDL_ReleaseGPUTexture(device, test_texture);
            if (pipeline) SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
            if (window_claimed) SDL_ReleaseWindowFromGPUDevice(device, window);
            SDL_DestroyGPUDevice(device);
        }
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
    }

    void create_pipeline(SDL_GPUTextureFormat format) {
        const auto path = shader_path();
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("Cannot open shader: " + path.string());
        std::vector<Uint8> code((std::istreambuf_iterator<char>(input)), {});
        if (code.empty()) throw std::runtime_error("Shader library is empty");

        SDL_GPUShaderCreateInfo shader{};
        shader.code_size = code.size();
        shader.code = code.data();
        shader.format = SDL_GPU_SHADERFORMAT_METALLIB;
        shader.entrypoint = "hello_vertex";
        shader.stage = SDL_GPU_SHADERSTAGE_VERTEX;
        shader.num_uniform_buffers = 1;
        auto* vertex = require(SDL_CreateGPUShader(device, &shader), "Create vertex shader");

        shader.entrypoint = "hello_fragment";
        shader.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
        shader.num_uniform_buffers = 0;
        auto* fragment = SDL_CreateGPUShader(device, &shader);
        if (!fragment) {
            const std::string error = SDL_GetError();
            SDL_ReleaseGPUShader(device, vertex);
            throw std::runtime_error("Create fragment shader: " + error);
        }

        SDL_GPUColorTargetDescription color{};
        color.format = format;
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = vertex;
        info.fragment_shader = fragment;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.target_info.color_target_descriptions = &color;
        info.target_info.num_color_targets = 1;
        pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
        const std::string error = pipeline ? "" : SDL_GetError();
        SDL_ReleaseGPUShader(device, fragment);
        SDL_ReleaseGPUShader(device, vertex);
        if (!pipeline) throw std::runtime_error("Create graphics pipeline: " + error);
    }

    void draw(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target,
              Uint32 width, Uint32 height, float angle, bool floating = false) {
        const FrameUniforms frame{angle, static_cast<float>(width) / height, 0.0f, 0.0f};
        SDL_PushGPUVertexUniformData(commands, 0, &frame, sizeof(frame));
        SDL_GPUColorTargetInfo color{};
        color.texture = target;
        color.clear_color = floating ? SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f}
                                    : SDL_FColor{0.025f, 0.035f, 0.06f, 1.0f};
        color.load_op = SDL_GPU_LOADOP_CLEAR;
        color.store_op = SDL_GPU_STOREOP_STORE;
        auto* pass = SDL_BeginGPURenderPass(commands, &color, 1, nullptr);
        if (!pass) {
            const std::string error = SDL_GetError();
            SDL_CancelGPUCommandBuffer(commands);
            throw std::runtime_error("Begin render pass: " + error);
        }
        SDL_BindGPUGraphicsPipeline(pass, pipeline);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    }

    void smoke_test(bool floating) {
        // A 64-pixel RGBA row is 256 bytes, also satisfying Metal's row alignment.
        constexpr Uint32 size = 64;
        constexpr Uint32 bytes = size * size * 4;
        SDL_GPUTextureCreateInfo texture{};
        texture.type = SDL_GPU_TEXTURETYPE_2D;
        texture.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        texture.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        texture.width = size;
        texture.height = size;
        texture.layer_count_or_depth = 1;
        texture.num_levels = 1;
        texture.sample_count = SDL_GPU_SAMPLECOUNT_1;
        test_texture = require(SDL_CreateGPUTexture(device, &texture), "Create test texture");
        create_pipeline(texture.format);

        SDL_GPUTransferBufferCreateInfo transfer{};
        transfer.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        transfer.size = bytes;
        test_download = require(SDL_CreateGPUTransferBuffer(device, &transfer), "Create readback buffer");

        auto* commands = require(SDL_AcquireGPUCommandBuffer(device), "Acquire test command buffer");
        draw(commands, test_texture, size, size, 0.0f, floating);
        auto* copy = SDL_BeginGPUCopyPass(commands);
        if (!copy) {
            SDL_CancelGPUCommandBuffer(commands);
            throw std::runtime_error("Cannot begin readback copy pass");
        }
        SDL_GPUTextureRegion source{};
        source.texture = test_texture;
        source.w = size;
        source.h = size;
        source.d = 1;
        SDL_GPUTextureTransferInfo destination{};
        destination.transfer_buffer = test_download;
        destination.pixels_per_row = size;
        destination.rows_per_layer = size;
        SDL_DownloadFromGPUTexture(copy, &source, &destination);
        SDL_EndGPUCopyPass(copy);
        test_fence = require(SDL_SubmitGPUCommandBufferAndAcquireFence(commands), "Submit test frame");
        require(SDL_WaitForGPUFences(device, true, &test_fence, 1), "Wait for test frame");
        auto* mapped = require(static_cast<Uint8*>(SDL_MapGPUTransferBuffer(device, test_download, false)),
                               "Map readback buffer");
        std::vector<Uint8> pixels(mapped, mapped + bytes);
        SDL_UnmapGPUTransferBuffer(device, test_download);

        const auto* center = pixels.data() + (32 * size + 32) * 4;
        const auto* corner = pixels.data();
        const int expected_corner_alpha = floating ? 0 : 255;
        if (center[0] + center[1] + center[2] < 180 || center[3] != 255 ||
            corner[0] + corner[1] + corner[2] > 50 || corner[3] != expected_corner_alpha) {
            throw std::runtime_error("GPU readback did not contain the expected triangle and background");
        }
        if (floating && (corner[0] != 0 || corner[1] != 0 || corner[2] != 0)) {
            throw std::runtime_error("Transparent background must have zero RGB as well as zero alpha");
        }
        std::puts(floating ? "PASS: GPU triangle with opaque center and transparent background."
                           : "PASS: GPU shaders, pipeline, drawing, and pixel readback.");
    }

    void run(unsigned frame_limit, bool floating) {
        const SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY |
            (floating ? SDL_WINDOW_BORDERLESS | SDL_WINDOW_TRANSPARENT | SDL_WINDOW_ALWAYS_ON_TOP | SDL_WINDOW_METAL
                      : SDL_WINDOW_RESIZABLE);
        window = require(SDL_CreateWindow("Hello, SDL GPU | Space: pause | R: reset | Esc: quit",
                                          floating ? 360 : 960, floating ? 360 : 640, flags),
                         "Create window");
        if (floating) {
            require(SDL_SetWindowHitTest(window, drag_floating_window, nullptr), "Enable floating triangle dragging");
            const auto shaders = shader_path().string();
            floating_renderer = require(create_floating_metal_renderer(window, shaders.c_str()), "Create floating Metal renderer");
            std::printf("Backend: native Metal (floating window)\nDevice: %s\n", floating_metal_device_name(floating_renderer));
            std::puts("Floating triangle: drag to move; Space pauses; R resets; Escape quits.");
        } else {
            // SDL GPU rejects SDL_WINDOW_TRANSPARENT, so only opaque windows
            // are claimed here. Floating mode uses SDL's native Metal view API.
            require(SDL_ClaimWindowForGPUDevice(device, window), "Claim window for SDL GPU");
            window_claimed = true;
            require(SDL_SetGPUSwapchainParameters(device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                                  SDL_GPU_PRESENTMODE_VSYNC), "Set vsync");
            create_pipeline(SDL_GetGPUSwapchainTextureFormat(device, window));
        }

        using Clock = std::chrono::steady_clock;
        auto previous = Clock::now();
        float angle = 0.0f;
        bool paused = false;
        bool running = true;
        unsigned frames = 0;
        while (running && (frame_limit == 0 || frames < frame_limit)) {
            SDL_Event event{};
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) running = false;
                if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                    if (event.key.key == SDLK_ESCAPE) running = false;
                    if (event.key.key == SDLK_SPACE) {
                        paused = !paused;
                        SDL_SetWindowTitle(window, paused ? "Hello, SDL GPU | Paused | Space: resume | Esc: quit"
                                                         : "Hello, SDL GPU | Space: pause | R: reset | Esc: quit");
                    }
                    if (event.key.key == SDLK_R) angle = 0.0f;
                }
            }
            if (!running) break;
            const auto now = Clock::now();
            const float delta = std::chrono::duration<float>(now - previous).count();
            previous = now;
            if (!paused) {
                angle += std::min(delta, 0.1f) * 0.5f;
                if (angle >= 6.2831853f) angle -= 6.2831853f;
            }

            if (floating) {
                bool presented = false;
                require(draw_floating_metal_frame(floating_renderer, angle, presented), "Draw floating frame");
                if (presented) ++frames;
                else std::this_thread::sleep_for(std::chrono::milliseconds(16));
                continue;
            }

            auto* commands = require(SDL_AcquireGPUCommandBuffer(device), "Acquire command buffer");
            SDL_GPUTexture* swapchain = nullptr;
            Uint32 width = 0;
            Uint32 height = 0;
            if (!SDL_WaitAndAcquireGPUSwapchainTexture(commands, window, &swapchain, &width, &height)) {
                const std::string error = SDL_GetError();
                SDL_CancelGPUCommandBuffer(commands);
                throw std::runtime_error("Acquire swapchain: " + error);
            }
            if (!swapchain || width == 0 || height == 0) {
                require(SDL_SubmitGPUCommandBuffer(commands), "Submit empty command buffer");
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                continue;
            }
            draw(commands, swapchain, width, height, angle, floating);
            require(SDL_SubmitGPUCommandBuffer(commands), "Submit frame");
            ++frames;
        }
        std::printf("Rendered %u frames.\n", frames);
    }
};

int main(int argc, char** argv) {
    try {
        bool probe = false;
        bool smoke = false;
        bool debug = false;
        bool floating = false;
        unsigned frame_limit = 0;
        for (int i = 1; i < argc; ++i) {
            const std::string argument = argv[i];
            if (argument == "--probe") probe = true;
            else if (argument == "--smoke-test") smoke = true;
            else if (argument == "--debug") debug = true;
            else if (argument == "--floating") floating = true;
            else if (argument == "--windowed") floating = false;
            else if (argument == "--help") {
                std::puts("Hello, SDL GPU\nUsage: hello-sdl-gpu [--floating | --windowed] "
                          "[--probe | --smoke-test] [--debug] [--frames N]\n"
                          "--floating: transparent, borderless, always-on-top triangle; drag to move.\n"
                          "--windowed: standard resizable window (default).\n"
                          "Space pauses rotation; R resets; Escape closes the app.");
                return 0;
            } else if (argument == "--frames" && i + 1 < argc) {
                const char* value = argv[++i];
                const auto result = std::from_chars(value, value + std::strlen(value), frame_limit);
                if (result.ec != std::errc{} || *result.ptr != '\0' || frame_limit == 0) {
                    throw std::runtime_error("--frames requires a positive integer");
                }
            } else throw std::runtime_error("Unknown or incomplete option: " + argument);
        }
        SDL_SetMainReady();
        App app;
        const bool native_floating = floating && !probe;
        // The native offscreen check needs a Metal device, but no display.
        require(SDL_Init(native_floating && smoke ? 0 : SDL_INIT_VIDEO), "Initialize SDL3");
        std::puts("Hello, SDL GPU!");
        if (!native_floating) {
            app.device = require(SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_METALLIB, debug || smoke, "metal"),
                                 "Create SDL GPU Metal device");
            const auto properties = SDL_GetGPUDeviceProperties(app.device);
            std::printf("Backend: %s\nDevice: %s\n", SDL_GetGPUDeviceDriver(app.device),
                        SDL_GetStringProperty(properties, SDL_PROP_GPU_DEVICE_NAME_STRING, "unknown"));
        }
        if (probe) {
            std::puts("Metal device creation succeeded; precompiled Metal shaders are supported.");
        } else if (smoke) {
            if (native_floating) {
                const auto shaders = shader_path().string();
                require(smoke_test_floating_metal(shaders.c_str()), "Check floating Metal rendering");
            } else {
                app.smoke_test(false);
            }
        } else {
            app.run(frame_limit, floating);
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
