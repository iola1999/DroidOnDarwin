/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NCE_ART_REFERENCE_HEAP_H
#define NCE_ART_REFERENCE_HEAP_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

extern uintptr_t nce_art_reference_base;
/* Reserved capacity; compressed references retain their 32-bit format. */
#define NCE_ART_HEAP_SIZE (UINT64_C(1) << 30)
int nce_art_heap_init(void);
int nce_art_heap_contains(const void *address, size_t length);
void *nce_art_heap_map(void *address, size_t length, int prot, int flags, int fd, off_t offset);
int nce_art_heap_unmap(void *address, size_t length);
__attribute__((noreturn)) void nce_art_invalid_reference(const void *pointer);

/* 零保留为空引用；越界指针必须在截断前终止。 */
static inline uint32_t nce_art_encode_reference(const void *pointer)
{
    if (!pointer) return 0;
    uintptr_t offset = (uintptr_t)pointer - nce_art_reference_base;
    if (!nce_art_reference_base || !offset || offset > UINT32_MAX)
        nce_art_invalid_reference(pointer);
    return (uint32_t)offset;
}

static inline void *nce_art_decode_reference(uint32_t reference)
{
    return reference ? (void *)(nce_art_reference_base + reference) : NULL;
}

#ifdef __cplusplus
}

#include <type_traits>
template <typename Dest, typename Source>
inline Dest nce_art_reference_cast(Source source)
{
    if constexpr (std::is_pointer_v<Dest>) {
        static_assert(std::is_same_v<Source, int32_t> || std::is_same_v<Source, uint32_t>);
        return static_cast<Dest>(nce_art_decode_reference(static_cast<uint32_t>(source)));
    } else {
        static_assert(std::is_same_v<Dest, int32_t> || std::is_same_v<Dest, uint32_t>);
        static_assert(std::is_pointer_v<Source>);
        return static_cast<Dest>(nce_art_encode_reference(source));
    }
}
#endif
#endif
