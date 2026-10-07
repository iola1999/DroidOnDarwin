/* SPDX-License-Identifier: Apache-2.0 */
#include "gfxstream/host/native_sub_window.h"

/* QEMU uses RUTABAGA_WSI_SURFACELESS and copies scanout pixels into Pixman.
 * The iOS backend therefore implements the required native-window API without
 * creating UIKit views or owning an additional display layer. */
EGLNativeWindowType createSubWindow(FBNativeWindowType, int, int, int, int, float,
                                   SubWindowRepaintCallback, void *, int)
{
    return nullptr;
}
void destroySubWindow(EGLNativeWindowType) {}
int moveSubWindow(FBNativeWindowType, EGLNativeWindowType, int, int, int, int, float)
{
    return 0;
}
void *getNativeDisplay() { return nullptr; }
void *getMetalLayerFromView(void *) { return nullptr; }
