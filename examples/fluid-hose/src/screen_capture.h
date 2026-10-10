#pragma once
#import <Metal/Metal.h>
#include <CoreGraphics/CoreGraphics.h>
#include <memory>

enum class CaptureState { off, starting, active, denied, failed };

// One captured desktop frame. Holding the object keeps its pixels alive, so
// keep it until the GPU work that samples it has completed.
@interface HoseCaptureFrame : NSObject
@property(nonatomic, readonly) id<MTLTexture> texture;
@end

// Streams one display through ScreenCaptureKit, excluding this app's own
// windows, so the water can refract what lies beneath it. Requires the
// Screen Recording permission.
class ScreenCapture {
public:
    ScreenCapture();
    ~ScreenCapture();
    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;

    // Asks macOS for permission if it has not been granted. Returns whether
    // capture is currently allowed; a first grant can require a relaunch.
    static bool request_permission();
    void start(CGDirectDisplayID display, double scale);
    void stop();
    CaptureState state() const;
    CGDirectDisplayID display() const;
    HoseCaptureFrame* latest_frame() const;

    // Shared with the stream callbacks, which can outlive a stopped stream.
    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};
