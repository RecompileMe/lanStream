#ifdef PLATFORM_MACOS

#import "capture_cg.hpp"
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreVideo/CoreVideo.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <CoreFoundation/CoreFoundation.h>
#import <objc/message.h>

#include <chrono>
#include <vector>
#include <memory>
#include <sstream>

// Helper: implement SCStreamOutput protocol to receive sample buffers via a block.
@interface StreamOutputHandler : NSObject <SCStreamOutput>
@property (nonatomic, copy) void (^captureHandler)(CMSampleBufferRef sampleBuffer, SCStreamOutputType type);
@end

@implementation StreamOutputHandler
- (void)stream:(SCStream *)stream
didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
        ofType:(SCStreamOutputType)type
{
    if (self.captureHandler) {
        self.captureHandler(sampleBuffer, type);
    }
}
@end

// 内部实现结构体，封装Objective-C对象
struct CaptureCG::Impl {
    SCStream* stream = nil;
    SCStreamConfiguration* config = nil;
    SCDisplay* selectedDisplay = nil;
};

// 匿名命名空间，避免符号冲突
namespace {
    // 查找指定索引的显示器
    SCDisplay* findDisplayByIndex(int index) {
        __block SCDisplay* targetDisplay = nil;
        __block int counter = 0;
        __block bool done = false;

        [SCShareableContent getShareableContentWithCompletionHandler:
         ^(SCShareableContent *content, NSError *error) {
            if (error) {
                NSLog(@"获取共享内容失败: %@", error);
                done = true;
                return;
            }

            for (SCDisplay* display in content.displays) {
                if (counter == index) {
                    CFRetain((__bridge CFTypeRef)display);
                    targetDisplay = display;
                    break;
                }
                counter++;
            }
            done = true;
        }];

        NSDate* timeout = [NSDate dateWithTimeIntervalSinceNow:1.0];
        while (!done && [[NSDate date] compare:timeout] == NSOrderedAscending) {
            [[NSRunLoop currentRunLoop]
             runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
        }

        if (!targetDisplay && counter > 0) {
            done = false;
            [SCShareableContent getShareableContentWithCompletionHandler:
             ^(SCShareableContent *content, NSError *error) {
                if (!error && content.displays.count > 0) {
                    SCDisplay* first = content.displays.firstObject;
                    CFRetain((__bridge CFTypeRef)first);
                    targetDisplay = first;
                }
                done = true;
            }];

            timeout = [NSDate dateWithTimeIntervalSinceNow:1.0];
            while (!done && [[NSDate date] compare:timeout] == NSOrderedAscending) {
                [[NSRunLoop currentRunLoop]
                 runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
            }
        }

        return targetDisplay;
    }
}

CaptureCG::~CaptureCG() {
    stop();
}

bool CaptureCG::init(int display_index) {
    if (display_index < 0) {
        NSLog(@"无效的显示器索引: %d", display_index);
        return false;
    }

    display_index_ = display_index;

    // 这里只用 display.width/height 做一个“逻辑尺寸”占位，真正像素在 start 里用 contentRect 计算
    SCDisplay* display = findDisplayByIndex(display_index_);
    if (!display) {
        NSLog(@"未找到显示器 index=%d", display_index_);
        return false;
    }

    NSUInteger dwidth = 0;
    NSUInteger dheight = 0;
    @try {
        dwidth = (NSUInteger)display.width;
        dheight = (NSUInteger)display.height;
    } @catch (NSException *ex) {
        NSLog(@"读取显示器尺寸时异常: %@", ex);
    }
    CFRelease((__bridge CFTypeRef)display);

    width_  = (int)dwidth;
    height_ = (int)dheight;

    if (width_ <= 0 || height_ <= 0) {
        NSLog(@"无效的显示器尺寸: %dx%d", width_, height_);
        return false;
    }

    NSLog(@"初始化显示器: 索引=%d, 尺寸(point)=%dx%d", display_index_, width_, height_);
    return true;
}

bool CaptureCG::start(FrameCallback cb) {
    if (running_.load()) {
        NSLog(@"捕获已在运行");
        return false;
    }

    if (width_ <= 0 || height_ <= 0) {
        if (!init(0)) {
            NSLog(@"自动初始化显示器失败");
            return false;
        }
    }

    cb_ = cb;
    running_.store(true);

    thread_ = std::thread([this] {
        @autoreleasepool {
            std::unique_ptr<Impl> impl(new Impl());

            // 1. 获取显示器
            impl->selectedDisplay = findDisplayByIndex(display_index_);
            if (!impl->selectedDisplay) {
                NSLog(@"线程中获取显示器失败");
                running_.store(false);
                return;
            }

            // 2. 创建流配置
            impl->config = [[SCStreamConfiguration alloc] init];
            impl->config.width  = (size_t)width_;
            impl->config.height = (size_t)height_;
            impl->config.scalesToFit = NO;
            impl->config.preservesAspectRatio = YES;
            impl->config.queueDepth = 2;          // 低延迟[web:27]
            impl->config.showsCursor = YES;
            impl->config.pixelFormat = kCVPixelFormatType_32BGRA;
            impl->config.colorSpaceName = kCGColorSpaceSRGB;
            impl->config.minimumFrameInterval = CMTimeMake(1, 60);

            NSError* error = nil;
            Class streamClass = NSClassFromString(@"SCStream");
            Class filterClass = NSClassFromString(@"SCContentFilter");
            if (!streamClass || !filterClass) {
                NSLog(@"当前系统不支持 ScreenCaptureKit");
                running_.store(false);
                return;
            }

            // 3. 构造 SCContentFilter（捕获整个 display）
            __block NSArray<SCRunningApplication*>* allApps = nil;
            __block bool appsReady = false;
            [SCShareableContent getShareableContentWithCompletionHandler:
             ^(SCShareableContent* content, NSError* err) {
                if (!err) {
                    allApps = content.applications;
                } else {
                    NSLog(@"获取应用列表失败: %@", err);
                }
                appsReady = true;
            }];
            NSDate* appTimeout = [NSDate dateWithTimeIntervalSinceNow:2.0];
            while (!appsReady && [[NSDate date] compare:appTimeout] == NSOrderedAscending) {
                [[NSRunLoop currentRunLoop]
                 runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
            }

            SEL selWithApps =
                NSSelectorFromString(@"initWithDisplay:includingApplications:exceptingWindows:");
            SEL selExcludeApps =
                NSSelectorFromString(@"initWithDisplay:excludingApplications:exceptingWindows:");
            SEL selExcludeWindows =
                NSSelectorFromString(@"initWithDisplay:excludingWindows:");

            id filterAlloc = ((id (*)(Class, SEL))objc_msgSend)(
                filterClass, sel_registerName("alloc"));
            id contentFilter = nil;

            if (allApps && [filterClass instancesRespondToSelector:selWithApps]) {
                contentFilter = ((id (*)(id, SEL, id, id, id))objc_msgSend)(
                    filterAlloc, selWithApps, impl->selectedDisplay, allApps, @[]);
            } else if ([filterClass instancesRespondToSelector:selExcludeApps]) {
                NSArray<SCRunningApplication*>* emptyApps = @[];
                NSArray<SCWindow*>* emptyWindows = @[];
                contentFilter = ((id (*)(id, SEL, id, id, id))objc_msgSend)(
                    filterAlloc, selExcludeApps, impl->selectedDisplay,
                    emptyApps, emptyWindows);
            } else if ([filterClass instancesRespondToSelector:selExcludeWindows]) {
                contentFilter = ((id (*)(id, SEL, id, id))objc_msgSend)(
                    filterAlloc, selExcludeWindows, impl->selectedDisplay, @[]);
            }

            if (!contentFilter) {
                NSLog(@"无法创建 SCContentFilter");
                running_.store(false);
                return;
            }

            // 4. 用 contentRect × pointPixelScale 设置真实像素分辨率[web:20][web:23]
            SCContentFilter *filter = (SCContentFilter *)contentFilter;
            CGFloat scale = filter.pointPixelScale;
            CGRect rect = filter.contentRect;
            size_t pixelWidth  = (size_t)(CGRectGetWidth(rect)  * scale);
            size_t pixelHeight = (size_t)(CGRectGetHeight(rect) * scale);

            impl->config.width  = pixelWidth;
            impl->config.height = pixelHeight;
            impl->config.scalesToFit = NO;
            impl->config.preservesAspectRatio = YES;

            NSLog(@"SCStream capture size: %zux%zu (scale=%.2f, rect=%.0fx%.0f)",
                  pixelWidth, pixelHeight,
                  scale,
                  CGRectGetWidth(rect), CGRectGetHeight(rect));

            if ([impl->config respondsToSelector:@selector(setCaptureResolution:)]) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunguarded-availability"
                impl->config.captureResolution = SCCaptureResolutionBest;
#pragma clang diagnostic pop
            }

            // 5. 创建 SCStream
            SEL selInitFilterNoError =
                NSSelectorFromString(@"initWithFilter:configuration:delegate:");
            id alloced = ((id (*)(Class, SEL))objc_msgSend)(
                streamClass, sel_registerName("alloc"));
            id streamObj = nil;

            if ([streamClass instancesRespondToSelector:selInitFilterNoError]) {
                streamObj = ((id (*)(id, SEL, id, id, id))objc_msgSend)(
                    alloced, selInitFilterNoError, contentFilter, impl->config, nil);
            } else {
                NSLog(@"SCStream 初始化方法不可用");
            }

            impl->stream = (SCStream*)streamObj;
            if (!impl->stream) {
                NSLog(@"创建SCStream失败");
                running_.store(false);
                return;
            }

            // 6. 添加视频输出
            StreamOutputHandler* output = [[StreamOutputHandler alloc] init];
            __block Impl* implPtr = impl.get();
            output.captureHandler = ^(CMSampleBufferRef sampleBuffer,
                                      SCStreamOutputType type) {
                if (!running_.load()) return;
                if (type != SCStreamOutputTypeScreen) return;

                CVImageBufferRef imageBuffer =
                    CMSampleBufferGetImageBuffer(sampleBuffer);
                if (!imageBuffer) return;

                OSType pixelFormat =
                    CVPixelBufferGetPixelFormatType(imageBuffer);
                if (pixelFormat != kCVPixelFormatType_32BGRA) {
                    NSLog(@"不支持的像素格式: %u", (unsigned int)pixelFormat);
                    return;
                }

                CVPixelBufferLockBaseAddress(imageBuffer,
                                             kCVPixelBufferLock_ReadOnly);

                size_t width       = CVPixelBufferGetWidth(imageBuffer);
                size_t height      = CVPixelBufferGetHeight(imageBuffer);
                size_t bytesPerRow = CVPixelBufferGetBytesPerRow(imageBuffer);
                void* baseAddress  = CVPixelBufferGetBaseAddress(imageBuffer);

                if (baseAddress && width > 0 && height > 0 && bytesPerRow > 0) {
                    auto frame = std::make_shared<RawFrame>();
                    frame->timestamp_us = now_us();
                    frame->width        = (int)width;
                    frame->height       = (int)height;
                    frame->linesize     = (int)bytesPerRow;
                    frame->format       = PixelFormat::BGRA;

                    size_t dataSize = bytesPerRow * height;
                    frame->data.resize(dataSize);
                    memcpy(frame->data.data(), baseAddress, dataSize);

                    if (cb_ && !frame->data.empty()) {
                        cb_(std::move(frame));   // 编码+发送放在外面线程
                    }
                }

                CVPixelBufferUnlockBaseAddress(imageBuffer,
                                               kCVPixelBufferLock_ReadOnly);
            };

            error = nil;
            dispatch_queue_t queue =
                dispatch_queue_create("capture.queue", DISPATCH_QUEUE_SERIAL);
            BOOL success = [impl->stream addStreamOutput:output
                                                    type:SCStreamOutputTypeScreen
                                    sampleHandlerQueue:queue
                                                 error:&error];
            if (!success) {
                NSLog(@"添加流输出失败: %@", error);
                running_.store(false);
                return;
            }

            // 7. 开始捕获
            [impl->stream startCaptureWithCompletionHandler:
            ^(NSError* _Nullable err) {
                if (err) {
                    NSLog(@"开始捕获失败: %@", err);
                    running_.store(false);
                } else {
                    NSLog(@"屏幕捕获已开始, stream=%p, 像素尺寸: %zux%zu",
                            (__bridge void*)implPtr->stream,
                            implPtr->config.width, implPtr->config.height);

                }
            }];

            // 8. 运行消息循环
            NSRunLoop* runLoop = [NSRunLoop currentRunLoop];
            while (running_.load()) {
                @autoreleasepool {
                    [runLoop runUntilDate:
                     [NSDate dateWithTimeIntervalSinceNow:0.016]];
                }
            }

            // 9. 清理
            NSLog(@"停止捕获...");
            if (impl->stream) {
                [impl->stream stopCaptureWithCompletionHandler:
                 ^(NSError* _Nullable err) {
                    if (err) {
                        NSLog(@"停止捕获时出错: %@", err);
                    } else {
                        NSLog(@"捕获已停止");
                    }
                }];
                impl->stream = nil;
            }
            if (impl->selectedDisplay) {
                CFRelease((__bridge CFTypeRef)impl->selectedDisplay);
                impl->selectedDisplay = nil;
            }
            NSLog(@"捕获线程退出");
        }
    });

    for (int i = 0; i < 50 && !running_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!running_.load()) {
        NSLog(@"捕获线程启动失败");
        if (thread_.joinable()) {
            thread_.join();
        }
        return false;
    }

    return true;
}

void CaptureCG::stop() {
    if (!running_.load()) return;

    NSLog(@"正在停止捕获...");
    running_.store(false);

    if (thread_.joinable()) {
        thread_.join();
    }

    cb_ = nullptr;
    NSLog(@"捕获已完全停止");
}

#endif // PLATFORM_MACOS
