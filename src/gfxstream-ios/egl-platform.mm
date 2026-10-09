/* SPDX-License-Identifier: Apache-2.0 */
#include "egl_os_api.h"
#include "egl-platform.h"
#import <QuartzCore/CAMetalLayer.h>

EglOS::Engine *EglOS::Engine::createHostInstance()
{
    return EglOS::getEgl2EglHostInstance(false);
}

bool huskGfxstreamWindowSize(void *window, unsigned *width, unsigned *height)
{
    id object = (__bridge id)window;
    if (![object isKindOfClass:[CAMetalLayer class]]) return false;
    CGSize size = ((CAMetalLayer *)object).drawableSize;
    if (size.width <= 0 || size.height <= 0) return false;
    *width = (unsigned)size.width;
    *height = (unsigned)size.height;
    return true;
}
