/* SPDX-License-Identifier: GPL-2.0-or-later */
#import "Display.h"
#include <assert.h>
#include <atomic>
#include <unistd.h>
#include <signal.h>

static std::atomic<bool> stopped;
static std::atomic<unsigned> stopRequests;

extern "C" int nce_lkl_run(const char *)
{
    puts("{\"phase\":\"android_desktop_running\"}");
    fflush(stdout);
    while (!stopped.load())
        usleep(10000);
    assert(stopRequests.load() == 2);
    puts("app lifecycle checks passed: proportional layout, programmatic size normalization, restoration disabled and deferred stop retry");
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
extern "C" int nce_session_profile(int, const char *)
{
    return -19;
}

static NceDisplay *findDisplay(NSView *root)
{
    if ([root isKindOfClass:NceDisplay.class])
        return (NceDisplay *)root;
    for (NSView *child in root.subviews) {
        NceDisplay *found = findDisplay(child);
        if (found)
            return found;
    }
    return nil;
}
static void checkLayout(NSWindow *window)
{
    [window.contentView layoutSubtreeIfNeeded];
    NceDisplay *display = findDisplay(window.contentView);
    assert(display);
    assert(!window.restorable);
    assert(fabs(display.bounds.size.width - window.contentView.bounds.size.width) < 1);
    assert(fabs(display.bounds.size.height + 90 - window.contentView.bounds.size.height) < 1);
    assert(fabs(display.bounds.size.width / display.bounds.size.height - 360.0 / 640) < 0.001);
    assert(CGSizeEqualToSize(display.metalLayer.bounds.size, CGSizeMake(360, 640)));
    assert(CGSizeEqualToSize(display.metalLayer.drawableSize, CGSizeMake(360, 640)));
}

extern "C" void nce_graphics_set_window(void *)
{
    NSWindow *window = NSApp.windows.firstObject;
    assert(window);
    checkLayout(window);
    NSSize requested = [window.delegate windowWillResize:window toSize:NSMakeSize(800, 500)];
    [window setFrame:NSMakeRect(0, 0, requested.width, requested.height) display:NO];
    checkLayout(window);
    [window setContentSize:NSMakeSize(800, 500)];
    checkLayout(window);
    // 管理进程通过 SIGTERM 请求停止；覆盖信号转入 AppKit 退出事件循环的路径。
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
      kill(getpid(), SIGTERM);
    });
}
