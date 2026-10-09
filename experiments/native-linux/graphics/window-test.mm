/* SPDX-License-Identifier: GPL-2.0-or-later */
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#include "host-graphics.h"
extern "C" int nce_lkl_run(const char *path);

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    @autoreleasepool {
        NSApplication *app = NSApplication.sharedApplication;
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(80, 80, 360, 640)
            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
            backing:NSBackingStoreBuffered defer:NO];
        window.title = @"Husk NCE · Android GPU";
        NSView *view = window.contentView;
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
            fflush(stdout); fflush(stderr);
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
                exit(result);
            });
        });
        [app run];
    }
}
