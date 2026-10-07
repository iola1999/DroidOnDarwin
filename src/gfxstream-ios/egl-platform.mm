/* SPDX-License-Identifier: Apache-2.0 */
#include "egl_os_api.h"
#include "egl-platform.h"

EglOS::Engine *EglOS::Engine::createHostInstance()
{
    return EglOS::getEgl2EglHostInstance(false);
}

bool huskGfxstreamWindowSize(void *, unsigned *, unsigned *)
{
    // This integration supports EGL pbuffers and surfaceless contexts only.
    return false;
}
