/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unicorn/unicorn.h>

extern const unsigned char load_alias[], load_post[], load_pair[], store_post[];
__asm__(
    ".text\n.p2align 2\n"
    "_load_alias: ldr x0, [x0]\n add x0, x0, #1\n b 1f\n1: svc #0\n"
    "_load_post: ldr x0, [x1], #8\n add x0, x0, #1\n b 2f\n2: svc #0\n"
    "_load_pair: ldp x0, x2, [x1], #16\n add x0, x0, x2\n b 3f\n3: svc #0\n"
    "_store_post: str x0, [x1], #8\n add x0, x0, #1\n b 4f\n4: svc #0\n");

#ifndef TEST_STOCK_STOP
extern uc_err nce_uc_emu_stop_at_tb(uc_engine *engine);
extern uc_err nce_uc_set_linux_memory(uc_engine *engine, bool enabled);
#endif
#define PAGE 16384
#define CODE 0x10000000UL
#define DATA 0x20000000UL
static bool requested, finished;

static void checked(uc_err result)
{
    if (result != UC_ERR_OK) {
        fprintf(stderr, "%s\n", uc_strerror(result));
        exit(2);
    }
}

static bool fill(uc_engine *engine, uint64_t address, uc_mem_type access, uc_tlb_entry *entry, void *unused)
{
    (void)unused;
    if (address == CODE) {
        entry->paddr = 0;
        entry->perms = UC_PROT_READ | UC_PROT_EXEC;
    } else if (address == DATA) {
        entry->paddr = PAGE;
        entry->perms = UC_PROT_READ | UC_PROT_WRITE;
        if (!requested && (access == UC_MEM_READ || access == UC_MEM_WRITE)) {
            requested = true;
#ifdef TEST_MEMORY_FAULT
            (void)engine;
            return false;
#elif defined(TEST_STOCK_STOP)
            checked(uc_emu_stop(engine));
#else
            checked(nce_uc_emu_stop_at_tb(engine));
#endif
        }
    } else {
        return false;
    }
    return true;
}

static void interrupt(uc_engine *engine, uint32_t number, void *unused)
{
    (void)unused;
    finished = number == 2;
    checked(uc_emu_stop(engine));
}

int main(void)
{
    const unsigned char *code[] = {load_alias, load_post, load_pair, store_post};
    const char *names[] = {"load_alias", "load_post", "load_pair", "store_post"};
    bool all = true;
    for (unsigned test = 0; test < 4; ++test) {
        unsigned char *memory = calloc(2, PAGE);
        if (!memory)
            return 2;
        memcpy(memory, code[test], 16);
        ((uint64_t *)(memory + PAGE))[0] = 41;
        ((uint64_t *)(memory + PAGE))[1] = 43;
        uc_engine *engine;
        checked(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &engine));
        checked(uc_ctl_set_page_size(engine, PAGE));
        checked(uc_ctl_tlb_mode(engine, UC_TLB_VIRTUAL));
#ifdef TEST_LINUX_MEMORY
        checked(nce_uc_set_linux_memory(engine, true));
#endif
        checked(uc_mem_map_ptr(engine, 0, 2 * PAGE, UC_PROT_ALL, memory));
        uc_hook hook;
        checked(uc_hook_add(engine, &hook, UC_HOOK_TLB_FILL, fill, NULL, 1, 0));
        checked(uc_hook_add(engine, &hook, UC_HOOK_INTR, interrupt, NULL, 1, 0));
        uint64_t x0 = test ? 17 : DATA, x1 = DATA, pc;
        checked(uc_reg_write(engine, UC_ARM64_REG_X0, &x0));
        checked(uc_reg_write(engine, UC_ARM64_REG_X1, &x1));
        requested = finished = false;
        uc_err result = uc_emu_start(engine, CODE, 0, 0, 0);
        checked(uc_reg_read(engine, UC_ARM64_REG_PC, &pc));
        checked(uc_reg_read(engine, UC_ARM64_REG_X0, &x0));
        checked(uc_reg_read(engine, UC_ARM64_REG_X1, &x1));
        uint64_t expected = test == 2 ? 84 : test == 3 ? 18
                                                       : 42;
        uint64_t next = DATA + (test == 2 ? 16 : test ? 8
                                                      : 0);
#ifdef TEST_MEMORY_FAULT
        bool passed = result == UC_ERR_EXCEPTION && requested && !finished && pc == CODE &&
                      x0 == (test ? 17 : DATA) && x1 == DATA && *(uint64_t *)(memory + PAGE) == 41;
        if (passed) {
            result = uc_emu_start(engine, pc, 0, 0, 0);
            checked(uc_reg_read(engine, UC_ARM64_REG_X0, &x0));
            checked(uc_reg_read(engine, UC_ARM64_REG_X1, &x1));
            passed = result == UC_ERR_OK && finished && x0 == expected && x1 == next;
        }
#else
        bool passed = result == UC_ERR_OK && requested && !finished && pc == CODE + 12 && x0 == expected && x1 == next;
        if (passed) {
            result = uc_emu_start(engine, pc, 0, 0, 0);
            passed = result == UC_ERR_OK && finished;
        }
#endif
        if (test == 3)
            passed &= *(uint64_t *)(memory + PAGE) == 17;
        printf("{\"test\":\"%s\",\"passed\":%s,\"pc\":%llu,\"x0\":%llu,\"x1\":%llu}\n",
               names[test], passed ? "true" : "false", (unsigned long long)pc, (unsigned long long)x0, (unsigned long long)x1);
        all &= passed;
        checked(uc_close(engine));
        free(memory);
    }
    return !all;
}
