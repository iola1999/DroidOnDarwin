/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "cpu.h"
#include "internals.h"
#include "exec/exec-all.h"
#include "exec/translation-block.h"
#include "hw/boards.h"
#include "system/cpu-timers.h"
#include "qemu/error-report.h"
#include "qemu/cutils.h"
#include "husk-arm64-exec.h"

typedef struct {
    HuskArm64Exec *engine;
    CPUARMState *env;
    void *code_host;
    int64_t resume_after_ns;
    struct { uint64_t page; unsigned entries; } warm_pages[1024];
} HuskArm64Qemu;

static bool husk_memory(HuskArm64Qemu *ctx, uint64_t addr, void *buffer,
                        size_t size, bool write)
{
    void *host;
    /* Tagged and unaligned accesses retain QEMU's architecture-specific checks. */
    if ((addr >> 56) || (addr & (size - 1)) ||
        (addr & ~TARGET_PAGE_MASK) + size > TARGET_PAGE_SIZE) {
        return false;
    }
    int flags = probe_access_flags(ctx->env, addr, size,
                                  write ? MMU_DATA_STORE : MMU_DATA_LOAD,
                                  arm_env_mmu_index(ctx->env), true, &host, 0);
    if (flags || !host) {
        return false;
    }
    if (write) {
        uintptr_t first = (uintptr_t)host;
        uintptr_t code = (uintptr_t)ctx->code_host;
        /* Catch writable aliases too, before the instruction changes guest RAM. */
        if (first < code + 4096 && first + size > code) {
            return false;
        }
        memcpy(host, buffer, size);
    } else {
        memcpy(buffer, host, size);
    }
    return true;
}

static bool husk_read(void *opaque, uint64_t a, void *buffer, size_t size)
{
    return husk_memory(opaque, a, buffer, size, false);
}

static bool husk_write(void *opaque, uint64_t a, const void *buffer, size_t size)
{
    return husk_memory(opaque, a, (void *)buffer, size, true);
}

static void *husk_map_page(void *opaque, uint64_t page, bool write)
{
    HuskArm64Qemu *ctx = opaque;
    void *host;
    int flags = probe_access_flags(ctx->env, page, 4096,
                                  write ? MMU_DATA_STORE : MMU_DATA_LOAD,
                                  arm_env_mmu_index(ctx->env), true, &host, 0);
    if (flags || !host) {
        return NULL;
    }
    if (write) {
        uintptr_t first = (uintptr_t)host;
        uintptr_t code = (uintptr_t)ctx->code_host;
        if (first < code + 4096 && first + 4096 > code) {
            return NULL;
        }
    }
    return host;
}

int arm_husk_exec_user(CPUState *cs)
{
    ARMCPU *cpu = ARM_CPU(cs);
    CPUARMState *env = &cpu->env;
    HuskArm64Qemu *ctx = cpu->husk_exec;
    static int enabled = -1;
    static bool direct;
    static unsigned tcg_quantum_us = 2000;
    if (enabled == -1) {
        const char *setting = getenv("HUSK_ARM64_EXEC");
        enabled = setting && !strcmp(setting, "1");
        const char *backend = getenv("HUSK_ARM64_BACKEND");
        direct = backend && !strcmp(backend, "direct");
        const char *quantum = getenv("HUSK_ARM64_TCG_QUANTUM_US");
        if (quantum && (qemu_strtoui(quantum, NULL, 10, &tcg_quantum_us) || tcg_quantum_us > 1000000)) {
            error_report("HUSK_ARM64_TCG_QUANTUM_US must be between 0 and 1000000");
            exit(EXIT_FAILURE);
        }
        if (enabled && (current_machine->smp.cpus != 1 || icount_enabled())) {
            error_report("HUSK_ARM64_EXEC requires one vCPU and wall-clock timing");
            exit(EXIT_FAILURE);
        }
    }
    if (!enabled || !env->aarch64 || arm_current_el(env) != 0 ||
        arm_is_secure(env) || cs->singlestep_enabled ||
        !QTAILQ_EMPTY(&cs->breakpoints) || !QTAILQ_EMPTY(&cs->watchpoints) ||
        cs->cflags_next_tb != -1 || (env->pc & 3) || (env->pc >> 56) ||
        TARGET_PAGE_SIZE < 4096 ||
        EX_TBFLAG_ANY(env->hflags, BE_DATA) ||
        EX_TBFLAG_ANY(env->hflags, SS_ACTIVE) ||
        /* Indirect branch landing checks run in TCG before direct execution.
         * BT enables those checks; a zero BTYPE has no pending landing check. */
        (EX_TBFLAG_A64(env->hflags, BT) && !direct) || env->btype ||
        EX_TBFLAG_A64(env->hflags, MTE_ACTIVE) ||
        EX_TBFLAG_A64(env->hflags, PSTATE_SM)) {
        return 0;
    }
    if (ctx && ctx->resume_after_ns &&
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) < ctx->resume_after_ns) {
        return 0;
    }

    uint64_t page = env->pc & ~UINT64_C(4095);
    void *code_host;
    int flags = probe_access_flags(env, page, 4096, MMU_INST_FETCH,
                                  arm_env_mmu_index(env), true, &code_host, 0);
    if (flags || !code_host) {
        return 0;
    }
    uint32_t insn = ldl_le_p((uint8_t *)code_host + env->pc - page);
    if (!husk_arm64_supported(insn)) {
        return 0;
    }

    if (!ctx) {
        ctx = g_new0(HuskArm64Qemu, 1);
        cpu->husk_exec = ctx;
    }
    if (direct) {
        unsigned index = ((page >> 12) ^ (page >> 24)) & 1023;
        if (ctx->warm_pages[index].page != page) {
            ctx->warm_pages[index].page = page;
            ctx->warm_pages[index].entries = 0;
        }
        /* First let TCG connect cold code. A repeated page may repay compilation;
         * collisions only delay admission and never select cached machine code. */
        if (ctx->warm_pages[index].entries < 2) {
            ++ctx->warm_pages[index].entries;
            return 0;
        }
    }
    if (!ctx->engine) {
        ctx->engine = husk_arm64_create();
        if (!ctx->engine) {
            error_report("Husk ARM64 engine allocation failed");
            exit(EXIT_FAILURE);
        }
        fprintf(stderr, "[husk-arm64] integer EL0 engine active, vCPU=%d\n", cs->cpu_index);
    }
    ctx->env = env;
    ctx->code_host = code_host;
    HuskArm64State state = { .sp = env->xregs[31], .pc = env->pc,
                             .nzcv = pstate_read(env) & PSTATE_NZCV };
    memcpy(state.x, env->xregs, sizeof(state.x));
    HuskArm64Exit result = husk_arm64_run(ctx->engine, &state, page, code_host,
                                        (HuskArm64Memory){ctx, husk_read, husk_write, husk_map_page},
                                        4096);
    if (result == HUSK_ARM64_ERROR) {
        error_report("Husk ARM64 execution failed at guest PC 0x%" PRIx64, env->pc);
        exit(EXIT_FAILURE);
    }
    memcpy(env->xregs, state.x, sizeof(state.x));
    env->xregs[31] = state.sp;
    env->pc = state.pc;
    pstate_write(env, (pstate_read(env) & ~PSTATE_NZCV) | state.nzcv);
    if (result == HUSK_ARM64_FALLBACK) {
        /* Let TCG build and reuse connected blocks before another Direct entry.
         * Immediate re-entry can prevent those connections from ever forming. */
        ctx->resume_after_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + (int64_t)tcg_quantum_us * 1000;
        /* Normal TCG chaining keeps unsupported regions out of the dispatcher.
         * The CPU loop still checks interrupts and eligibility when TCG exits.
         * Returning -1 clears last_tb after Direct changed the guest state. */
        return -1;
    }
    return 1;
}

void arm_husk_exec_destroy(ARMCPU *cpu)
{
    HuskArm64Qemu *ctx = cpu->husk_exec;
    if (ctx) {
        husk_arm64_destroy(ctx->engine);
        g_free(ctx);
        cpu->husk_exec = NULL;
    }
}
