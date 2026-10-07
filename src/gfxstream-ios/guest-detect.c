/* SPDX-License-Identifier: Apache-2.0 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <drm/virtgpu_drm.h>

/* Runs before virtgpu_detect assigns the read-only EGL driver property. The
 * GLES capset is specific to Rutabaga; other GPU devices retain their driver. */
int main(void)
{
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("husk-gfxstream: render node"); return 1; }
    uint64_t ids = 0;
    struct drm_virtgpu_getparam query = {
        .param = VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs,
        .value = (uintptr_t)&ids,
    };
    int status = ioctl(fd, DRM_IOCTL_VIRTGPU_GETPARAM, &query);
    close(fd);
    if (status || !(ids & (1u << 8))) return 0;
    const char *settings[][2] = {
        {"ro.hardware.egl", "emulation"},
        {"ro.boot.hardware.gltransport", "virtio-gpu-pipe"},
        {"ro.hardware.gralloc", "minigbm"},
        {"debug.hwui.renderer", "skiagl"},
    };
    for (unsigned i = 0; i < sizeof(settings) / sizeof(settings[0]); ++i) {
        if (__system_property_set(settings[i][0], settings[i][1])) {
            fprintf(stderr, "husk-gfxstream: cannot set %s\n", settings[i][0]);
            return 1;
        }
    }
    puts("husk-gfxstream: GLES capset present; emulation driver selected");
    return 0;
}
