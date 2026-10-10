/* SPDX-License-Identifier: GPL-2.0-or-later */
#import "Display.h"
#include "../input.h"
#include <assert.h>
#include <mutex>
#include <vector>
#include <unistd.h>

static std::mutex commandsMutex;
static std::vector<NceInputCommand> commands;
extern "C" int nce_host_input_send(int action, int x, int y)
{
    std::lock_guard<std::mutex> lock(commandsMutex);
    commands.push_back({action, x, y});
    return 0;
}

static NSEvent *pointer(NSWindow *window, NSEventType type, NSPoint point)
{
    return [NSEvent mouseEventWithType:type
                              location:point
                         modifierFlags:0
                             timestamp:0
                          windowNumber:window.windowNumber
                               context:nil
                           eventNumber:0
                            clickCount:1
                              pressure:1];
}

int main(void)
{
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 720, 1324)
                                                       styleMask:NSWindowStyleMaskTitled
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        window.releasedWhenClosed = NO;
        NceDisplay *display = [[NceDisplay alloc] initWithFrame:NSMakeRect(0, 44, 720, 1280)];
        assert(NSEqualRects(display.metalLayer.frame, display.bounds));
        assert(CGSizeEqualToSize(display.metalLayer.drawableSize, CGSizeMake(360, 640)));
        [window.contentView addSubview:display];
        NSEvent *down = pointer(window, NSEventTypeLeftMouseDown, NSMakePoint(360, 684));
        [display mouseDown:down];
        display.inputEnabled = YES;
        [display mouseDown:down];
        [display mouseDragged:pointer(window, NSEventTypeLeftMouseDragged, NSMakePoint(900, -10))];
        [display mouseUp:pointer(window, NSEventTypeLeftMouseUp, NSMakePoint(-20, 1500))];
        NSButton *home = [NSButton new];
        home.tag = 102;
        [display navigate:home];
        for (int attempt = 0; attempt < 200; ++attempt) {
            {
                std::lock_guard<std::mutex> lock(commandsMutex);
                if (commands.size() == 4)
                    break;
            }
            usleep(5000);
        }
        std::lock_guard<std::mutex> lock(commandsMutex);
        assert(commands.size() == 4);
        assert(commands[0].action == NCE_INPUT_DOWN && commands[0].x == 180 && commands[0].y == 320);
        assert(commands[1].action == NCE_INPUT_MOVE && commands[1].x == 359 && commands[1].y == 639);
        assert(commands[2].action == NCE_INPUT_UP && commands[2].x == 0 && commands[2].y == 0);
        assert(commands[3].action == NCE_INPUT_KEY && commands[3].x == 102);
        puts("display checks passed: initial layer geometry, fixed drawable, scaled coordinates, edge clamping, input readiness and ordered navigation");
    }
}
