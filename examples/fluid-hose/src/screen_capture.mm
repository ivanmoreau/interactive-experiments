#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#include "screen_capture.h"
#include "fluid_sim.h"
#include <unistd.h>
#include <atomic>
#include <mutex>

@interface HoseCaptureFrame ()
- (instancetype)initWithTexture:(CVMetalTextureRef)texture;
@end

@implementation HoseCaptureFrame {
    CVMetalTextureRef _reference;
}
- (instancetype)initWithTexture:(CVMetalTextureRef)texture {
    if ((self = [super init])) {
        _reference = (CVMetalTextureRef)CFRetain(texture);
        _texture = CVMetalTextureGetTexture(texture);
    }
    return self;
}
- (void)dealloc {
    if (_reference) CFRelease(_reference);
}
@end

struct ScreenCapture::Impl {
    mutable std::mutex mutex;
    HoseCaptureFrame* frame = nil;
    std::atomic<CaptureState> state{CaptureState::off};
    std::atomic<unsigned> generation{0};  // invalidates callbacks from a stopped stream
    CGDirectDisplayID display = 0;
    CVMetalTextureCacheRef cache = nullptr;
    SCStream* stream = nil;
    id output = nil;
};

@interface HoseCaptureOutput : NSObject <SCStreamOutput, SCStreamDelegate>
- (instancetype)initWithCapture:(std::shared_ptr<ScreenCapture::Impl>)capture generation:(unsigned)generation;
@end

@implementation HoseCaptureOutput {
    std::weak_ptr<ScreenCapture::Impl> _capture;
    unsigned _generation;
}
- (instancetype)initWithCapture:(std::shared_ptr<ScreenCapture::Impl>)capture generation:(unsigned)generation {
    if ((self = [super init])) {
        _capture = capture;
        _generation = generation;
    }
    return self;
}
- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)buffer ofType:(SCStreamOutputType)type {
    (void)stream;
    const auto capture = _capture.lock();
    if (!capture || type != SCStreamOutputTypeScreen || capture->generation != _generation) return;
    // Idle and blank frames carry no new pixels; keep the previous frame.
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(buffer, false);
    if (!attachments || CFArrayGetCount(attachments) == 0) return;
    NSDictionary* info = (__bridge NSDictionary*)CFArrayGetValueAtIndex(attachments, 0);
    NSNumber* status = info[SCStreamFrameInfoStatus];
    if (!status || status.integerValue != SCFrameStatusComplete) return;
    CVPixelBufferRef pixels = CMSampleBufferGetImageBuffer(buffer);
    if (!pixels || !capture->cache) return;
    CVMetalTextureRef texture = nullptr;
    if (CVMetalTextureCacheCreateTextureFromImage(nullptr, capture->cache, pixels, nullptr, MTLPixelFormatBGRA8Unorm,
                                                  CVPixelBufferGetWidth(pixels), CVPixelBufferGetHeight(pixels), 0,
                                                  &texture) != kCVReturnSuccess || !texture) return;
    HoseCaptureFrame* frame = [[HoseCaptureFrame alloc] initWithTexture:texture];
    CFRelease(texture);
    std::lock_guard<std::mutex> lock(capture->mutex);
    capture->frame = frame;
    capture->state = CaptureState::active;
}
- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
    (void)stream;
    (void)error;
    const auto capture = _capture.lock();
    if (capture && capture->generation == _generation) capture->state = CaptureState::failed;
}
@end

ScreenCapture::ScreenCapture() : impl_(std::make_shared<Impl>()) {
    CVMetalTextureCacheCreate(nullptr, nullptr, hose_metal_device(), nullptr, &impl_->cache);
}

ScreenCapture::~ScreenCapture() {
    stop();
    if (impl_->cache) CFRelease(impl_->cache);
    impl_->cache = nullptr;
}

bool ScreenCapture::request_permission() {
    if (CGPreflightScreenCaptureAccess()) return true;
    return CGRequestScreenCaptureAccess();
}

void ScreenCapture::start(CGDirectDisplayID display, double scale) {
    stop();
    auto capture = impl_;
    capture->display = display;
    capture->state = CaptureState::starting;
    const unsigned generation = ++capture->generation;
    [SCShareableContent getShareableContentExcludingDesktopWindows:NO onScreenWindowsOnly:YES
                                                 completionHandler:^(SCShareableContent* content, NSError* error) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (capture->generation != generation) return;
            if (!content) {
                // Without permission, macOS reports a user-declined error.
                capture->state = error.code == SCStreamErrorUserDeclined || !CGPreflightScreenCaptureAccess()
                                 ? CaptureState::denied : CaptureState::failed;
                return;
            }
            SCDisplay* target = nil;
            for (SCDisplay* candidate in content.displays)
                if (candidate.displayID == display) target = candidate;
            if (!target) { capture->state = CaptureState::failed; return; }
            NSMutableArray<SCRunningApplication*>* own = [NSMutableArray array];
            for (SCRunningApplication* application in content.applications)
                if (application.processID == getpid()) [own addObject:application];
            SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:target excludingApplications:own exceptingWindows:@[]];
            SCStreamConfiguration* configuration = [[SCStreamConfiguration alloc] init];
            configuration.width = static_cast<size_t>(target.width * scale);
            configuration.height = static_cast<size_t>(target.height * scale);
            configuration.pixelFormat = kCVPixelFormatType_32BGRA;
            configuration.colorSpaceName = kCGColorSpaceSRGB;
            configuration.minimumFrameInterval = CMTimeMake(1, 60);
            configuration.showsCursor = NO;
            configuration.queueDepth = 4;
            HoseCaptureOutput* output = [[HoseCaptureOutput alloc] initWithCapture:capture generation:generation];
            SCStream* stream = [[SCStream alloc] initWithFilter:filter configuration:configuration delegate:output];
            NSError* add_error = nil;
            if (![stream addStreamOutput:output type:SCStreamOutputTypeScreen
                      sampleHandlerQueue:dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0) error:&add_error]) {
                capture->state = CaptureState::failed;
                return;
            }
            capture->stream = stream;
            capture->output = output;
            [stream startCaptureWithCompletionHandler:^(NSError* start_error) {
                if (!start_error || capture->generation != generation) return;
                capture->state = start_error.code == SCStreamErrorUserDeclined ? CaptureState::denied : CaptureState::failed;
            }];
        });
    }];
}

void ScreenCapture::stop() {
    auto& s = *impl_;
    ++s.generation;
    if (s.stream) [s.stream stopCaptureWithCompletionHandler:^(NSError*) {}];
    s.stream = nil;
    s.output = nil;
    std::lock_guard<std::mutex> lock(s.mutex);
    s.frame = nil;
    s.state = CaptureState::off;
}

CaptureState ScreenCapture::state() const { return impl_->state; }
CGDirectDisplayID ScreenCapture::display() const { return impl_->display; }

HoseCaptureFrame* ScreenCapture::latest_frame() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->frame;
}
