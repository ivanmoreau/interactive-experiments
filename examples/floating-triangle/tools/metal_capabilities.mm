#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstdio>

int main() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            std::fprintf(stderr, "No Metal device is accessible to this process.\n");
            return 1;
        }

        MTLSize threads = device.maxThreadsPerThreadgroup;
        NSDictionary *report = @{
            @"gpu": device.name,
            @"os": NSProcessInfo.processInfo.operatingSystemVersionString,
            @"unified_memory": @(device.hasUnifiedMemory),
            @"system_memory_gib": @(NSProcessInfo.processInfo.physicalMemory / (1024ULL * 1024ULL * 1024ULL)),
            @"apple7_family": @([device supportsFamily:MTLGPUFamilyApple7]),
            @"mac2_family": @([device supportsFamily:MTLGPUFamilyMac2]),
            @"metal3_family": @([device supportsFamily:MTLGPUFamilyMetal3]),
            @"max_buffer_mib": @(device.maxBufferLength / (1024ULL * 1024ULL)),
            @"recommended_working_set_mib": @(device.recommendedMaxWorkingSetSize / (1024ULL * 1024ULL)),
            @"max_threads_per_threadgroup": @[@(threads.width), @(threads.height), @(threads.depth)],
            @"bc_texture_compression": @(device.supportsBCTextureCompression)
        };
        NSError *error = nil;
        NSData *json = [NSJSONSerialization dataWithJSONObject:report
                                                      options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys
                                                        error:&error];
        if (json == nil) {
            std::fprintf(stderr, "Cannot serialize capabilities: %s\n", error.localizedDescription.UTF8String);
            return 1;
        }
        std::fwrite(json.bytes, 1, json.length, stdout);
        std::putchar('\n');
    }
    return 0;
}
