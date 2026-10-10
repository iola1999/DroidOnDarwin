/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "nce.h"
#include "nce-thread.h"
#include <libkern/OSCacheControl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(offsetof(NceState, regs.sp) == 248, "assembly layout");
_Static_assert(offsetof(NceState, regs.vector) == 288, "assembly layout");
_Static_assert(offsetof(NceState, host_sp) == 800, "assembly layout");
_Static_assert(offsetof(NceState, resume) == 816, "assembly layout");
_Static_assert(offsetof(NceState, instruction) == 824, "assembly layout");
_Static_assert(offsetof(NceState, done) == 848, "assembly layout");

extern const unsigned char nce_stub[], nce_stub_end[];
extern void nce_enter_bound(NceState *);

void nce_enter(NceState *state)
{
    int result = nce_thread_bind(state);
    if (result) {
        fprintf(stderr, "NCE thread context binding failed: %d\n", result);
        abort();
    }
    nce_enter_bound(state);
    result = nce_thread_bind(NULL);
    if (result) {
        fprintf(stderr, "NCE thread context clearing failed: %d\n", result);
        abort();
    }
}

static int x18_instruction(uint32_t word)
{
    unsigned rd = word & 31, rn = (word >> 5) & 31, rm = (word >> 16) & 31;
    if ((word & 0x7fe0fc00) == 0x2a000000 && (rd == 18 || rn == 18 || rm == 18))
        return 1;
    if (word == 0x92403643 || word == 0x9272c652)
        return 1;
    return (word & 0xff800000) == 0xa9000000 && (rd == 18 || ((word >> 10) & 31) == 18);
}

int nce_write_branch(uint32_t *out, uintptr_t from, uintptr_t to)
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
    if (stub.size < 16 || (entry & 3))
        return -1;
    uint32_t *words = (void *)stub.rw;
    words[0] = 0xa8c147f0;
    return nce_write_branch(&words[1], (uintptr_t)stub.rx + 4, entry);
}

int nce_write_patch(uint32_t *write_address, uintptr_t execute_address,
                    NceCode stub, uint32_t instruction)
{
    unsigned key;
    if (stub.size < 80 || nce_stub_end - nce_stub != 80 || nce_thread_key(&key))
        return -1;
    memcpy(stub.rw, nce_stub, 80);
    uint32_t *words = (void *)stub.rw;
    words[2] = 0xf9400210u | (key << 10);  // ldr x16, [x16, #key * 8]
    uint64_t resume = (uintptr_t)stub.rx + 40;
    uint64_t gate = (uintptr_t)nce_gate;
    memcpy(stub.rw + 56, &resume, 8);
    memcpy(stub.rw + 64, &instruction, 4);
    memcpy(stub.rw + 72, &gate, 8);
    unsigned reg = instruction & 31;
    if ((instruction & 0xffffffe0) == 0xd53bd040 && reg != 18 && reg != 31) {
        // Only the destination changes. The TSD slot belongs to this pthread,
        // so shared code needs neither guest stack nor host dispatch for TLS.
        _Static_assert(offsetof(NceState, tls) % 8 == 0 && offsetof(NceState, tls) < 32768, "TLS load offset");
        words[0] = 0xd53bd060u | reg;  // mrs Xt, tpidrro_el0
        words[1] = 0xf9400000u | (key << 10) | (reg << 5) | reg;
        words[2] = 0xf9400000u | ((offsetof(NceState, tls) / 8) << 10) | (reg << 5) | reg;
        if (nce_write_branch(&words[3], (uintptr_t)stub.rx + 12, execute_address + 4))
            return -1;
    }
    if (nce_write_branch((void *)(stub.rw + 44), (uintptr_t)stub.rx + 44, execute_address + 4))
        return -1;
    return nce_write_branch(write_address, execute_address, (uintptr_t)stub.rx);
}

int nce_prepare(NceCode code, NceState *s, const void *bytes, size_t size, size_t entry)
{
    size_t cursor = (size + 15) & ~(size_t)15;
    if (!code.rw || !code.rx || (size & 3) || (entry & 3) || entry >= size ||
        cursor + 16 > code.size || nce_stub_end - nce_stub != 80)
        return -1;
    memcpy(code.rw, bytes, size);
    /* Entry and syscall returns restore x16/x17 after leaving the common gate. */
    if (nce_write_entry((NceCode){code.rw + cursor, code.rx + cursor, 16},
                        (uintptr_t)code.rx + entry))
        return -1;
    s->resume = (uintptr_t)code.rx + cursor;
    s->regs.pc = (uintptr_t)code.rx + entry;
    cursor += 16;
    for (size_t pos = 0; pos < size; pos += 4) {
        uint32_t word;
        memcpy(&word, code.rw + pos, 4);
        int svc = (word & 0xffe0001f) == 0xd4000001;
        int tls = (word & 0xffffffe0) == 0xd53bd040 ||
                  (word & 0xffffffe0) == 0xd51bd040;
        int cache_type = (word & 0xffffffe0) == 0xd53b0020;
        if (!svc && !tls && !cache_type && !x18_instruction(word))
            continue;
        if (cursor + 80 > code.size)
            return -1;
        if (nce_write_patch((void *)(code.rw + pos), (uintptr_t)code.rx + pos,
                            (NceCode){code.rw + cursor, code.rx + cursor, 80}, word))
            return -1;
        cursor += 80;
    }
    nce_sync_code(code);
    return 0;
}

NceState *nce_dispatch(NceState *s)
{
    /* Every synchronous resume has a B to the next guest instruction. */
    uint32_t branch_word;
    memcpy(&branch_word, (void *)(s->resume + 4), 4);
    s->regs.pc = s->resume + 4 + ((int64_t)(int32_t)(branch_word << 6) >> 4);
    return nce_emulate(s);
}

NceState *nce_emulate(NceState *s)
{
    uint32_t word = s->instruction;
    unsigned reg = word & 31;
    if ((word & 0xffffffe0) == 0xd53bd040) {
        if (reg != 31)
            s->regs.x[reg] = s->tls;
    } else if ((word & 0xffffffe0) == 0xd51bd040) {
        s->tls = reg == 31 ? 0 : s->regs.x[reg];
    } else if ((word & 0xffffffe0) == 0xd53b0020) {
        /* Virtual I/D maintenance granules are 64 bytes; IDC/DIC remain clear.
         * Executable publication uses Darwin's cache maintenance functions. */
        if (reg != 31)
            s->regs.x[reg] = UINT64_C(0x80040004);
    } else if ((word & 0x7fe0fc00) == 0x2a000000) {
        unsigned source = (word >> 16) & 31;
        unsigned left = (word >> 5) & 31;
        uint64_t value = source == 31 ? 0 : s->regs.x[source];
        value |= left == 31 ? 0 : s->regs.x[left];
        if (reg != 31)
            s->regs.x[reg] = word & 0x80000000u ? value : (uint32_t)value;
    } else if (word == 0x92403643) {
        s->regs.x[3] = s->regs.x[18] & 0x3fff;
    } else if (word == 0x9272c652) {
        s->regs.x[18] &= ~UINT64_C(0x3fff);
    } else if ((word & 0xff800000) == 0xa9000000) {
        unsigned second = (word >> 10) & 31, base = (word >> 5) & 31;
        int offset = (int32_t)(word << 10) >> 25;
        uintptr_t address = (base == 31 ? s->regs.sp : s->regs.x[base]) + offset * 8;
        int write = !(word & (1u << 22));
        uint64_t values[2] = {reg == 31 ? 0 : s->regs.x[reg], second == 31 ? 0 : s->regs.x[second]};
        if (!s->memory(s, address, values, sizeof(values), write) && !write) {
            if (reg != 31)
                s->regs.x[reg] = values[0];
            if (second != 31)
                s->regs.x[second] = values[1];
        }
    } else {
        ++s->calls;
        s->syscall(s);
    }
    return s;
}

NceState *nce_event_dispatch(NceState *s)
{
    return s->event(s);
}
