// GOW-Port: iOS: the host window is drawn once per display refresh. EAGL's present does not wait for the display, so
// raylib's sleep-based 60 fps limiter put frames on screen at uneven times against the panel's own refresh (some shown
// for one refresh, some for two or three: jerky motion at a full frame rate). A CADisplayLink at 60 Hz on its own
// thread counts refreshes; the host waits for the next one before latching and drawing a frame. Compiled with ARC.
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#include <pthread/qos.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace
{
    std::mutex g_mutex;
    std::condition_variable g_refresh;
    uint64_t g_refreshes = 0;
}

@interface PS2XDisplayTicker : NSObject
- (void)refresh:(CADisplayLink *)link;
@end

@implementation PS2XDisplayTicker
- (void)refresh:(CADisplayLink *)link
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ++g_refreshes;
    }
    g_refresh.notify_all();
}
@end

// Returns at the next display refresh after the previous call (at once if one already passed); 40 ms at most.
void ps2xIosWaitDisplayRefresh()
{
    static std::once_flag started;
    std::call_once(started, [] {
        std::thread([] {
            pthread_setname_np("DisplayLink");
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
            @autoreleasepool
            {
                PS2XDisplayTicker *ticker = [PS2XDisplayTicker new];
                CADisplayLink *link = [CADisplayLink displayLinkWithTarget:ticker selector:@selector(refresh:)];
                if (@available(iOS 15.0, *))
                    link.preferredFrameRateRange = CAFrameRateRangeMake(60.0f, 60.0f, 60.0f);
                else
                    link.preferredFramesPerSecond = 60;
                [link addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSRunLoopCommonModes];
                [[NSRunLoop currentRunLoop] run];
            }
        }).detach();
    });
    static uint64_t seen = 0;
    std::unique_lock<std::mutex> lock(g_mutex);
    g_refresh.wait_for(lock, std::chrono::milliseconds(40), [] { return g_refreshes != seen; });
    seen = g_refreshes;
}
