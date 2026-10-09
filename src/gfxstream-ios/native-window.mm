/* SPDX-License-Identifier: Apache-2.0 */
#include "gfxstream/host/native_sub_window.h"
#import <QuartzCore/CAMetalLayer.h>

/* The app owns the visible view and updates its geometry on the main thread.
 * Keep its Metal layer alive until the renderer destroys the EGL surface. */
EGLNativeWindowType createSubWindow(FBNativeWindowType parent, int, int, int, int, float,
                                    SubWindowRepaintCallback, void *, int)
{
    id layer = (__bridge id)parent;
    if (![layer isKindOfClass:[CAMetalLayer class]])
        return nullptr;
    CFRetain((__bridge CFTypeRef)layer);
    return parent;
}
void destroySubWindow(EGLNativeWindowType window)
{
    if (window)
        CFRelease((CFTypeRef)window);
}
int moveSubWindow(FBNativeWindowType, EGLNativeWindowType, int, int, int, int, float)
{
    return 0;
}
void *getNativeDisplay()
{
    return nullptr;
}
void *getMetalLayerFromView(void *layer)
{
    return layer;
}
