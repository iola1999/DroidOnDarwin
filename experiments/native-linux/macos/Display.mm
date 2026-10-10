/* SPDX-License-Identifier: GPL-2.0-or-later */
#import "Display.h"
#import <Metal/Metal.h>
#import <QuartzCore/CATransaction.h>
#include "../input.h"
#include "../graphics/host-graphics.h"

@implementation NceDisplay {
    dispatch_queue_t _inputQueue;
    BOOL _dragging;
}
- (instancetype)initWithFrame:(NSRect)frame
{
    if ((self = [super initWithFrame:frame])) {
        _inputQueue = dispatch_queue_create("app.droidondarwin.mac.input", DISPATCH_QUEUE_SERIAL);
        self.wantsLayer = YES;
        _metalLayer = [CAMetalLayer layer];
        _metalLayer.device = MTLCreateSystemDefaultDevice();
        _metalLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        _metalLayer.framebufferOnly = NO;
        _metalLayer.contentsScale = 1;
        _metalLayer.drawableSize = CGSizeMake(NCE_GPU_WIDTH, NCE_GPU_HEIGHT);
        self.layer.masksToBounds = YES;
        [self.layer addSublayer:_metalLayer];
        [self updateMetalGeometry];
        self.accessibilityLabel = @"Android 桌面";
    }
    return self;
}
- (void)layout
{
    [super layout];
    [self updateMetalGeometry];
}
- (void)updateMetalGeometry
{
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    // ANGLE 根据 layer.bounds 确定渲染尺寸；通过 transform 缩放显示，保持客体画面尺寸。
    _metalLayer.bounds = CGRectMake(0, 0, NCE_GPU_WIDTH, NCE_GPU_HEIGHT);
    _metalLayer.position = CGPointMake(NSMidX(self.bounds), NSMidY(self.bounds));
    _metalLayer.transform = CATransform3DMakeScale(self.bounds.size.width / NCE_GPU_WIDTH,
                                                   self.bounds.size.height / NCE_GPU_HEIGHT, 1);
    [CATransaction commit];
}
- (BOOL)acceptsFirstResponder
{
    return YES;
}
- (void)sendPointer:(NSEvent *)event action:(int)action
{
    NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    int x = MAX(0, MIN(NCE_GPU_WIDTH - 1, (int)(point.x * NCE_GPU_WIDTH / self.bounds.size.width)));
    int y = MAX(0, MIN(NCE_GPU_HEIGHT - 1, (int)((self.bounds.size.height - point.y) * NCE_GPU_HEIGHT / self.bounds.size.height)));
    dispatch_async(_inputQueue, ^{
      nce_host_input_send(action, x, y);
    });
}
- (void)mouseDown:(NSEvent *)event
{
    if (self.inputEnabled) {
        _dragging = YES;
        [self.window makeFirstResponder:self];
        [self sendPointer:event action:NCE_INPUT_DOWN];
    }
}
- (void)mouseDragged:(NSEvent *)event
{
    if (_dragging)
        [self sendPointer:event action:NCE_INPUT_MOVE];
}
- (void)mouseUp:(NSEvent *)event
{
    if (_dragging) {
        [self sendPointer:event action:NCE_INPUT_UP];
        _dragging = NO;
    }
}
- (void)navigate:(id)sender
{
    if (!self.inputEnabled)
        return;
    int key = (int)[sender tag];
    dispatch_async(_inputQueue, ^{
      nce_host_input_send(NCE_INPUT_KEY, key, 0);
    });
}
@end
