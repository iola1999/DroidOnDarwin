/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/auxv.h>
#include <asm/hwcap.h>

__attribute__((visibility("hidden"))) void *(*husk_original_copy)(void *, const void *, size_t);
__attribute__((visibility("hidden"))) void *(*husk_original_move)(void *, const void *, size_t);
__attribute__((visibility("hidden"))) void *(*husk_original_set)(void *, int, size_t);
__attribute__((visibility("hidden"))) int husk_bulk_enabled;
static int count_requests;
static _Atomic uint64_t completed, refused, transferred;

__attribute__((visibility("hidden"))) void *husk_fallback_move(void *destination, const void *source, size_t size)
{
    volatile unsigned char *d = destination;
    const volatile unsigned char *s = source;
    if ((uintptr_t)d > (uintptr_t)s && (uintptr_t)d - (uintptr_t)s < size)
        while (size) {
            --size;
            d[size] = s[size];
        }
    else
        for (size_t i = 0; i < size; ++i)
            d[i] = s[i];
    return destination;
}

__attribute__((visibility("hidden"))) void *husk_fallback_set(void *destination, int value, size_t size)
{
    volatile unsigned char *d = destination;
    for (size_t i = 0; i < size; ++i)
        d[i] = value;
    return destination;
}

__attribute__((constructor)) static void initialize(void)
{
    husk_original_copy = dlsym(RTLD_NEXT, "memcpy");
    husk_original_move = dlsym(RTLD_NEXT, "memmove");
    husk_original_set = dlsym(RTLD_NEXT, "memset");
    const char *disable = getenv("HUSK_BULK_DISABLE");
    if (disable && !strcmp(disable, "1"))
        return;
    /* QEMU already handles Bionic MOPS copies with host memory operations. */
    if (getauxval(AT_HWCAP2) & HWCAP2_MOPS)
        return;
    int saved_errno = errno;
    int fd = open("/sys/firmware/devicetree/base/chosen/husk,bulk-memory", O_RDONLY | O_CLOEXEC);
    char version[2] = {0};
    if (fd >= 0) {
        husk_bulk_enabled = read(fd, version, sizeof(version)) == 2 && version[0] == '1' && !version[1];
        close(fd);
    }
    if (!husk_bulk_enabled) {
        fd = open("/sys/firmware/acpi/tables/HUSK", O_RDONLY | O_CLOEXEC);
        unsigned char header[36];
        if (fd >= 0) {
            if (read(fd, header, sizeof(header)) == sizeof(header)) {
                unsigned char checksum = 0;
                for (unsigned i = 0; i < sizeof(header); ++i)
                    checksum += header[i];
                husk_bulk_enabled = !memcmp(header, "HUSK\x24\0\0\0\x01", 9) &&
                                    !memcmp(header + 16, "BULKv001", 8) && !checksum;
            }
            close(fd);
        }
    }
    errno = saved_errno;
    const char *stats = getenv("HUSK_BULK_STATS");
    count_requests = stats && !strcmp(stats, "1");
}

static int request(void *destination, uintptr_t source, size_t bytes, unsigned operation)
{
    register uintptr_t x0 __asm__("x0") = (uintptr_t)destination;
    register uintptr_t x1 __asm__("x1") = source;
    register uintptr_t x2 __asm__("x2") = bytes;
    register uintptr_t x3 __asm__("x3") = operation;
    register uintptr_t x8 __asm__("x8");
    __asm__ volatile("hlt #0x484d" : "=r"(x8) : "r"(x0), "r"(x1), "r"(x2), "r"(x3) : "memory");
    if (count_requests) {
        atomic_fetch_add_explicit(x8 ? &refused : &completed, 1, memory_order_relaxed);
        if (!x8)
            atomic_fetch_add_explicit(&transferred, bytes, memory_order_relaxed);
    }
    return x8 == 0;
}

__attribute__((visibility("hidden"))) void *husk_bulk_copy(void *restrict destination, const void *restrict source, size_t size)
{
    unsigned char *d = destination;
    const unsigned char *s = source;
    while (husk_bulk_enabled && size >= 1024) {
        size_t chunk = size < 65536 ? size : 65536;
        if (!request(d, (uintptr_t)s, chunk, 0))
            break;
        d += chunk;
        s += chunk;
        size -= chunk;
    }
    if (size)
        (husk_original_copy ? husk_original_copy : husk_fallback_move)(d, s, size);
    return destination;
}

__attribute__((visibility("hidden"))) void *husk_bulk_move(void *destination, const void *source, size_t size)
{
    // One request gathers the complete source before writing any destination.
    if (husk_bulk_enabled && size >= 1024 && size <= 65536 && request(destination, (uintptr_t)source, size, 1))
        return destination;
    return (husk_original_move ? husk_original_move : husk_fallback_move)(destination, source, size);
}

__attribute__((visibility("hidden"))) void *husk_bulk_set(void *destination, int value, size_t size)
{
    unsigned char *d = destination;
    while (husk_bulk_enabled && size >= 1024) {
        size_t chunk = size < 65536 ? size : 65536;
        if (!request(d, (unsigned char)value, chunk, 2))
            break;
        d += chunk;
        size -= chunk;
    }
    if (size)
        (husk_original_set ? husk_original_set : husk_fallback_set)(d, value, size);
    return destination;
}

/* Optional benchmark counters; ordinary processes incur no atomic updates. */
int husk_bulk_counters(uint64_t values[3])
{
    values[0] = atomic_load_explicit(&completed, memory_order_relaxed);
    values[1] = atomic_load_explicit(&refused, memory_order_relaxed);
    values[2] = atomic_load_explicit(&transferred, memory_order_relaxed);
    return husk_bulk_enabled;
}
