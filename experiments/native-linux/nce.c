/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "nce.h"
#include <libkern/OSCacheControl.h>
#include <string.h>

_Static_assert(offsetof(NceState, sp) == 248, "assembly layout");
_Static_assert(offsetof(NceState, vector) == 288, "assembly layout");
_Static_assert(offsetof(NceState, host_sp) == 800, "assembly layout");
_Static_assert(offsetof(NceState, resume) == 816, "assembly layout");
_Static_assert(offsetof(NceState, instruction) == 824, "assembly layout");
_Static_assert(offsetof(NceState, done) == 848, "assembly layout");

extern const unsigned char nce_stub[], nce_stub_end[];

static int branch(uint32_t *out, uintptr_t from, uintptr_t to)
{
    intptr_t distance = (intptr_t)to - (intptr_t)from;
    if ((distance & 3) || distance < -(1 << 27) || distance >= (1 << 27))
        return -1;
    *out = 0x14000000u | ((uint32_t)(distance / 4) & 0x03ffffffu);
    return 0;
}

void nce_sync_code(NceCode code)
{
    sys_dcache_flush(code.rw, code.size);
    sys_icache_invalidate(code.rx, code.size);
}

int nce_write_entry(NceCode stub, uintptr_t entry)
{
    if (stub.size < 16 || (entry & 3)) return -1;
    uint32_t *words = (void *)stub.rw;
    words[0] = 0xa8c147f0;
    return branch(&words[1], (uintptr_t)stub.rx + 4, entry);
}

int nce_write_patch(uint32_t *write_address, uintptr_t execute_address,
                    NceCode stub, NceState **active, uint32_t instruction)
{
    if (stub.size < 80 || nce_stub_end - nce_stub != 80) return -1;
    memcpy(stub.rw, nce_stub, 80);
    uint64_t context = (uintptr_t)active;
    uint64_t resume = (uintptr_t)stub.rx + 40;
    uint64_t gate = (uintptr_t)nce_gate;
    memcpy(stub.rw + 48, &context, 8);
    memcpy(stub.rw + 56, &resume, 8);
    memcpy(stub.rw + 64, &instruction, 4);
    memcpy(stub.rw + 72, &gate, 8);
    if (branch((void *)(stub.rw + 44), (uintptr_t)stub.rx + 44, execute_address + 4)) return -1;
    return branch(write_address, execute_address, (uintptr_t)stub.rx);
}

int nce_prepare_shared(NceCode code, NceState *s, NceState **active,
                       const void *bytes, size_t size, size_t entry)
{
    size_t cursor = (size + 15) & ~(size_t)15;
    if (!code.rw || !code.rx || (size & 3) || (entry & 3) || entry >= size ||
        cursor + 16 > code.size || nce_stub_end - nce_stub != 80)
        return -1;
    memcpy(code.rw, bytes, size);
    /* Entry and syscall returns restore x16/x17 after leaving the common gate. */
    if (nce_write_entry((NceCode){code.rw + cursor, code.rx + cursor, 16},
                        (uintptr_t)code.rx + entry)) return -1;
    s->resume = (uintptr_t)code.rx + cursor;
    s->pc = (uintptr_t)code.rx + entry;
    cursor += 16;
    for (size_t pos = 0; pos < size; pos += 4) {
        uint32_t word;
        memcpy(&word, code.rw + pos, 4);
        int svc = (word & 0xffe0001f) == 0xd4000001;
        int tls = (word & 0xffffffe0) == 0xd53bd040 ||
                  (word & 0xffffffe0) == 0xd51bd040;
        if (!svc && !tls) continue;
        if (cursor + 80 > code.size) return -1;
        if (nce_write_patch((void *)(code.rw + pos), (uintptr_t)code.rx + pos,
                            (NceCode){code.rw + cursor, code.rx + cursor, 80}, active, word)) return -1;
        cursor += 80;
    }
    nce_sync_code(code);
    return 0;
}

int nce_prepare(NceCode code, NceState *s, const void *bytes, size_t size, size_t entry)
{
    s->self = s;
    return nce_prepare_shared(code, s, &s->self, bytes, size, entry);
}

NceState *nce_dispatch(NceState *s)
{
    /* Every synchronous resume has a B to the next guest instruction. */
    uint32_t branch_word;
    memcpy(&branch_word, (void *)(s->resume + 4), 4);
    s->pc = s->resume + 4 + ((int64_t)(int32_t)(branch_word << 6) >> 4);
    uint32_t word = s->instruction;
    unsigned reg = word & 31;
    if ((word & 0xffffffe0) == 0xd53bd040) {
        if (reg != 31) s->x[reg] = s->tls;
        ++s->tls_accesses;
    } else if ((word & 0xffffffe0) == 0xd51bd040) {
        s->tls = reg == 31 ? 0 : s->x[reg];
        ++s->tls_accesses;
    } else {
        ++s->calls;
        s->syscall(s);
    }
    return s;
}

NceState *nce_event_dispatch(NceState *s) { return s->event(s); }
