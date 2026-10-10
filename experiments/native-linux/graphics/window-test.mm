/* SPDX-License-Identifier: GPL-2.0-or-later */
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#include "host-graphics.h"
#include "../input.h"
#include "../session.h"
extern "C" int nce_lkl_run(const char *path);

@interface NceWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic) BOOL stopping;
@end

@implementation NceWindowDelegate
- (BOOL)windowShouldClose:(NSWindow *)window
{
    if (!getenv("NCE_DESKTOP"))
        return YES;
    if (self.stopping)
        return NO;
    self.stopping = YES;
    window.title = @"DroidOnDarwin · 正在停止 Android";
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      int result = nce_session_request_stop();
      if (result)
          dispatch_async(dispatch_get_main_queue(), ^{
            self.stopping = NO;
            window.title = @"DroidOnDarwin · 启动准备中，请稍后关闭";
          });
    });
    return NO;
}
@end

@interface NceInputView : NSView
@property(nonatomic) dispatch_queue_t inputQueue;
@end

@implementation NceInputView
- (BOOL)acceptsFirstResponder
{
    return YES;
}
- (void)sendPointer:(NSEvent *)event action:(int)action
{
    NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    int x = MAX(0, MIN(359, (int)(point.x * 360 / self.bounds.size.width)));
    int y = MAX(0, MIN(639, (int)((self.bounds.size.height - point.y) * 640 / self.bounds.size.height)));
    dispatch_async(self.inputQueue, ^{
      nce_host_input_send(action, x, y);
    });
}
- (void)mouseDown:(NSEvent *)event
{
    [self sendPointer:event action:NCE_INPUT_DOWN];
}
- (void)mouseDragged:(NSEvent *)event
{
    [self sendPointer:event action:NCE_INPUT_MOVE];
}
- (void)mouseUp:(NSEvent *)event
{
    [self sendPointer:event action:NCE_INPUT_UP];
}
- (void)navigate:(NSButton *)sender
{
    int code = (int)sender.tag;
    dispatch_async(self.inputQueue, ^{
      nce_host_input_send(NCE_INPUT_KEY, code, 0);
    });
}
@end

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    @autoreleasepool {
        NSApplication *app = NSApplication.sharedApplication;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(80, 80, 360, 680)
                                                       styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        window.title = @"DroidOnDarwin · Android GPU";
        // NSWindow does not retain its delegate; keep it through the event loop.
        __attribute__((objc_precise_lifetime)) NceWindowDelegate *delegate = [NceWindowDelegate new];
        window.delegate = delegate;
        NceInputView *view = [[NceInputView alloc] initWithFrame:NSMakeRect(0, 40, 360, 640)];
        view.inputQueue = dispatch_queue_create("app.droidondarwin.android-input", DISPATCH_QUEUE_SERIAL);
        [window.contentView addSubview:view];
        NSArray<NSString *> *titles = @[ @"返回", @"主页", @"最近应用" ];
        const int keys[] = {158, 102, 580};
        for (unsigned i = 0; i < 3; ++i) {
            NSButton *button = [NSButton buttonWithTitle:titles[i] target:view action:@selector(navigate:)];
            button.frame = NSMakeRect(i * 120, 0, 120, 40);
            button.tag = keys[i];
            [window.contentView addSubview:button];
        }
        view.wantsLayer = YES;
        CAMetalLayer *layer = [CAMetalLayer layer];
        layer.device = MTLCreateSystemDefaultDevice();
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = NO;
        layer.frame = view.bounds;
        layer.drawableSize = CGSizeMake(NCE_GPU_WIDTH, NCE_GPU_HEIGHT);
        view.layer = layer;
        [window makeKeyAndOrderFront:nil];
        [app activateIgnoringOtherApps:YES];
        nce_graphics_set_window((__bridge void *)layer);
        const char *path = argv[1];
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
          int result = nce_lkl_run(path);
          fflush(stdout);
          fflush(stderr);
          int delay = getenv("NCE_DESKTOP") ? 0 : 30;
          dispatch_after(dispatch_time(DISPATCH_TIME_NOW, delay * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            exit(result);
          });
        });
        [app run];
    }
}
