/* SPDX-License-Identifier: GPL-2.0-or-later */
#import "Display.h"
#include <assert.h>
#include <atomic>
#include <unistd.h>

static std::atomic<bool> stopped;
static std::atomic<unsigned> stopRequests;

extern "C" int nce_lkl_run(const char *)
{
    puts("{\"phase\":\"android_desktop_running\"}");
    fflush(stdout);
    while (!stopped.load())
        usleep(10000);
    assert(stopRequests.load() == 2);
    puts("app lifecycle checks passed: initial layout, landscape letterboxing, fixed render bounds and deferred stop retry");
    return 0;
}

extern "C" int nce_session_request_stop(void)
{
    if (++stopRequests == 1)
        return -19;
    stopped.store(true);
    return 0;
}
extern "C" int nce_host_input_send(int, int, int)
{
    return 0;
}

static void checkLayout(NSWindow *window)
{
    [window.contentView layoutSubtreeIfNeeded];
    NceDisplay *display = nil;
    NSStackView *navigation = nil;
    for (NSView *view in window.contentView.subviews) {
        if ([view isKindOfClass:NceDisplay.class])
            display = (NceDisplay *)view;
        if ([view isKindOfClass:NSStackView.class])
            navigation = (NSStackView *)view;
    }
    assert(display && navigation);
    assert(NSMinY(display.frame) >= NSMaxY(navigation.frame));
    assert(NSMinX(display.frame) >= 0 && NSMaxX(display.frame) <= window.contentView.bounds.size.width);
    assert(fabs(display.bounds.size.width / display.bounds.size.height - 360.0 / 640) < 0.001);
    assert(CGSizeEqualToSize(display.metalLayer.bounds.size, CGSizeMake(360, 640)));
    assert(CGSizeEqualToSize(display.metalLayer.drawableSize, CGSizeMake(360, 640)));
}

extern "C" void nce_graphics_set_window(void *)
{
    NSWindow *window = NSApp.windows.firstObject;
    assert(window);
    checkLayout(window);
    [window setContentSize:NSMakeSize(800, 500)];
    checkLayout(window);
    // 退出会进入 AppKit 事件循环；从运行循环调用，允许主 GCD 队列继续处理停止回调。
    [NSApp performSelector:@selector(terminate:) withObject:nil afterDelay:1];
}
