/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NCE_READ_VIEW_H
#define NCE_READ_VIEW_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *base;
    size_t size;
    uint64_t maps, revocations;
} NceReadView;

/* Create a zero-initialized view. One owner updates the view only while its
 * guest is outside the executor.
 * Source mappings must remain alive until the corresponding aliases are
 * replaced or removed. All sizes and offsets use Darwin's 16 KiB pages.
 * If a mapping operation fails, the owner must stop using and destroy the
 * view: a Mach remap may precede a failed permission change. Argument
 * validation failures leave the view unchanged. Returns 0 or -1 with errno. */
int nce_read_view_create(NceReadView *, size_t size);
int nce_read_view_map(NceReadView *, size_t offset, const void *source, size_t size);
int nce_read_view_revoke(NceReadView *, size_t offset, size_t size);
int nce_read_view_destroy(NceReadView *);
#ifdef __cplusplus
}
#endif
#endif
