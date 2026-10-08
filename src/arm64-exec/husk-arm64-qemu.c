/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "cpu.h"
#include "internals.h"
#include "exec/exec-all.h"
#include "exec/cputlb.h"
#include "exec/helper-proto.h"
#include "exec/translation-block.h"
#include "accel/tcg/internal-common.h"
#include "hw/boards.h"
#include "system/cpu-timers.h"
#include "qemu/error-report.h"
#include "qemu/cutils.h"
#include "husk-arm64-exec.h"

typedef struct {
    HuskArm64Exec *engine;
    CPUARMState *env;
    void *code_host;
    void *code_hosts[256];
    unsigned code_count;
    bool reuse_mappings;
    uint64_t mapping_epoch, mapping_generation;
    int mapping_mmu;
    int64_t resume_after_ns[2];
    bool poll_dispatch, assist, assisted_pauth, chain_assist, landing, pauth_landing, kernel, kernel_started, mops_started;
    unsigned last_el;
    unsigned dispatch_polls;
    bool profile;
    unsigned profile_phase;
    int64_t profile_last, profile_report, profile_ns[4];
    struct { uint64_t page; unsigned entries; } warm_pages[1024];
} HuskArm64Qemu;

/* Attribute vCPU thread time to EL0/kernel TCG and Direct. CPU time excludes
 * waits and host scheduling; Direct includes compilation and its callbacks. */
static void husk_profile_phase(HuskArm64Qemu *ctx, unsigned phase)
{
    if (!ctx->profile) return;
    struct timespec time;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time)) return;
    int64_t now = (int64_t)time.tv_sec * 1000000000 + time.tv_nsec;
    if (ctx->profile_last) ctx->profile_ns[ctx->profile_phase] += now - ctx->profile_last;
    ctx->profile_last = now;
    ctx->profile_phase = phase;
    if (now - ctx->profile_report > 5000000000LL) {
        fprintf(stderr, "[husk-cpu-time] user_tcg_ms=%.3f kernel_tcg_ms=%.3f direct_ms=%.3f user_direct_ms=%.3f kernel_direct_ms=%.3f\n",
                ctx->profile_ns[0] / 1e6, ctx->profile_ns[1] / 1e6,
                (ctx->profile_ns[2] + ctx->profile_ns[3]) / 1e6,
                ctx->profile_ns[2] / 1e6, ctx->profile_ns[3] / 1e6);
        ctx->profile_report = now;
    }
}

static void husk_profile_el(ARMCPU *cpu, void *opaque)
{
    husk_profile_phase(opaque, arm_current_el(&cpu->env) != 0);
}

bool arm_husk_dispatch(CPUState *cs)
{
    ARMCPU *cpu = ARM_CPU(cs);
    HuskArm64Qemu *ctx = cpu->husk_exec;
    if (!ctx || !ctx->poll_dispatch) return false;
    unsigned el = arm_current_el(&cpu->env);
    bool changed = ctx->last_el != el;
    ctx->last_el = el;
    if (!cpu->env.aarch64 || (el != 0 && !(ctx->kernel && el == 1))) {
        return false;
    }
    /* TCG's indirect chaining can bypass cpu_exec_loop for an entire user
     * workload. Poll there in batches, and on a return from kernel execution. */
    if (!changed && (++ctx->dispatch_polls & 63)) return false;
    return qemu_clock_get_ns(QEMU_CLOCK_REALTIME) >= ctx->resume_after_ns[el != 0];
}

static bool husk_tcg_bridge(uint32_t insn, unsigned el)
{
    /* Kernel system operations keep QEMU's register, MMU, interrupt and trap
     * behavior, then recheck Direct eligibility after that single instruction.
     * ERET also returns through QEMU before the next privilege-level check. */
    if (el == 1 && ((insn & 0xff000000) == 0xd5000000 || insn == 0xd69f03e0))
        return true;
    /* A refused MOPS sequence still consists of three adjacent instructions.
     * QEMU owns partial progress, faults and large copies for these encodings. */
    if ((insn & 0xff20fc00) == 0x1d000400 ||
        (insn & 0xffe03c00) == 0x19c00400)
        return true;
    /* Keep QEMU's PAUTH, BTI and system-register semantics. These short
     * sequences otherwise stop Direct at nearly every function boundary. */
    switch (insn) {
    case 0xd503211f: case 0xd503215f: case 0xd503219f: case 0xd50321df:
    case 0xd503231f: case 0xd503233f: case 0xd503235f: case 0xd503237f:
    case 0xd503239f: case 0xd50323bf: case 0xd50323df: case 0xd50323ff:
    case 0xd503241f: case 0xd503245f: case 0xd503249f: case 0xd50324df:
    case 0xd65f0bff: case 0xd65f0fff: return true;
    }
    switch (insn & 0xffffffe0) {
    case 0xd53be000: case 0xd53be020: case 0xd53be040:
    case 0xd53be0a0: case 0xd53be0c0:
        // QEMU retains counter frequency, offsets, virtual time and trap checks.
        return true;
    }
    uint32_t branch = insn & 0xfffffc1f;
    return branch == 0xd61f0000 || branch == 0xd63f0000 ||
           (insn & 0xfffff09f) == 0xd503309f;
}

static void husk_one_tcg(CPUState *cs)
{
    cs->cflags_next_tb = (curr_cflags(cs) & ~CF_COUNT_MASK) |
                        1 | CF_NO_GOTO_TB | CF_NO_GOTO_PTR;
}

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
        for (unsigned i = 0; i < ctx->code_count; ++i) {
            uintptr_t code = (uintptr_t)ctx->code_hosts[i];
            if (first < code + 4096 && first + 4096 > code) return NULL;
        }
    }
    return host;
}

static bool husk_bulk_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("HUSK_ARM64_BULK");
        enabled = value && !strcmp(value, "1") && current_machine->smp.cpus == 1;
    }
    return enabled;
}

static void *husk_bulk_tcg_page(void *opaque, uint64_t page, bool write)
{
    CPUARMState *env = opaque;
    void *host = NULL;
    int flags = probe_access_flags(env, page, 4096,
                                  write ? MMU_DATA_STORE : MMU_DATA_LOAD,
                                  arm_env_mmu_index(env), true, &host, 0);
    return flags ? NULL : host;
}

void HELPER(husk_bulk)(CPUARMState *env)
{
    HuskArm64Memory memory = { .opaque = env, .map_page = husk_bulk_tcg_page };
    env->xregs[8] = 1;
    if (husk_bulk_enabled() && arm_current_el(env) == 0 &&
        env->xregs[3] <= HUSK_BULK_SET &&
        !EX_TBFLAG_A64(env->hflags, MTE_ACTIVE) &&
        husk_arm64_bulk(env->xregs[0], env->xregs[1], env->xregs[2], env->xregs[3], memory))
        env->xregs[8] = 0;
}

static const void *husk_map_code(void *opaque, uint64_t page)
{
    HuskArm64Qemu *ctx = opaque;
    void *host;
    int flags = probe_access_flags(ctx->env, page, 4096, MMU_INST_FETCH,
                                  arm_env_mmu_index(ctx->env), true, &host, 0);
    if (flags || !host) return NULL;
    for (unsigned i = 0; i < ctx->code_count; ++i) {
        if (ctx->code_hosts[i] == host) return host;
    }
    if (ctx->code_count == ARRAY_SIZE(ctx->code_hosts)) return NULL;
    ctx->code_hosts[ctx->code_count++] = host;
    return host;
}

static void husk_export_state(CPUARMState *env, const HuskArm64State *state)
{
    env->btype = (state->access_flags & HUSK_BTYPE_MASK) >> HUSK_BTYPE_SHIFT;
    memcpy(env->xregs, state->x, sizeof(state->x));
    env->xregs[31] = state->sp;
    env->pc = state->pc;
    env->exclusive_addr = state->exclusive_address;
    env->exclusive_val = state->exclusive_value;
    pstate_write(env, (pstate_read(env) & ~PSTATE_NZCV) | state->nzcv);
    for (unsigned i = 0; i < 32; ++i) {
        memcpy(aa64_vfp_qreg(env, i), state->v[i], sizeof(state->v[i]));
    }
    vfp_set_fpsr(env, state->fpsr);
}

static bool husk_mops_allowed(CPUARMState *env)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *setting = getenv("HUSK_ARM64_MOPS");
        enabled = !setting || strcmp(setting, "0");
    }
    return enabled && arm_husk_mops_allowed(env) &&
        !EX_TBFLAG_A64(env->hflags, MTE_ACTIVE);
}

static HuskArm64Assist husk_assist(void *opaque, HuskArm64State *state, uint32_t insn)
{
    HuskArm64Qemu *ctx = opaque;
    CPUARMState *env = ctx->env;
    unsigned reg = 30, operation;
    uint64_t modifier = 0;
    uint32_t branch = insn & 0xfffffc1f;
    if (husk_arm64_mops_prologue(insn)) {
        if (!husk_mops_allowed(env) || (state->pc & 4095) > 4084)
            return HUSK_ASSIST_DECLINED;
        const uint8_t *page = husk_map_code(ctx, state->pc & ~UINT64_C(4095));
        if (!page) return HUSK_ASSIST_DECLINED;
        const uint8_t *code = page + (state->pc & 4095);
        uint32_t words[3] = {ldl_le_p(code), ldl_le_p(code + 4), ldl_le_p(code + 8)};
        HuskArm64Memory memory = { .opaque = ctx, .map_page = husk_map_page };
        if (!husk_arm64_mops(state, words, memory)) return HUSK_ASSIST_DECLINED;
        if (!ctx->mops_started) {
            fprintf(stderr, "[husk-arm64] MOPS sequence assist active\n");
            ctx->mops_started = true;
        }
        return HUSK_ASSIST_CONTINUE;
    }
    if (insn == HUSK_BULK_INSN) {
        HuskArm64Memory memory = { .opaque = ctx, .map_page = husk_map_page };
        state->x[8] = 1;
        if (husk_bulk_enabled() && arm_current_el(env) == 0 && state->x[3] <= HUSK_BULK_SET &&
            husk_arm64_bulk(state->x[0], state->x[1], state->x[2], state->x[3], memory))
            state->x[8] = 0;
        state->pc += 4;
        return HUSK_ASSIST_CONTINUE;
    }
    if (branch == 0xd61f0000 || branch == 0xd63f0000) {
        husk_export_state(env, state);
        arm_husk_branch(env, (insn >> 5) & 31, branch == 0xd63f0000);
        state->access_flags = (state->access_flags & ~HUSK_BTYPE_MASK) | (env->btype << HUSK_BTYPE_SHIFT);
        state->pc = env->pc;
        state->x[30] = env->xregs[30];
        if (!ctx->chain_assist) return HUSK_ASSIST_BRANCH;
        bool cache_call = branch == 0xd63f0000;
        bool cache_jump = branch == 0xd61f0000 && ((insn >> 5) & 31) != 31;
        if (!env->btype) return cache_call ? HUSK_ASSIST_CHAIN_CALL :
            cache_jump ? HUSK_ASSIST_CHAIN_JUMP : HUSK_ASSIST_CHAIN;
        /* Validate BTI before Direct executes the first instruction. BTYPE
         * stays pending until that instruction completes successfully. */
        void *host = NULL;
        int flags = probe_access_flags(env, state->pc, 4, MMU_INST_FETCH,
                                      arm_env_mmu_index(env), true, &host, 0);
        if (flags || !host || (state->pc & 3)) return HUSK_ASSIST_BRANCH;
        uint32_t landing = ldl_le_p(host);
        arm_husk_bti_check(env, landing);
        switch (landing) {
        case 0xd503241f: case 0xd503245f: case 0xd503249f: case 0xd50324df:
            break;
        case 0xd503233f: case 0xd503237f:
            if (ctx->landing && ctx->pauth_landing)
                return cache_call ? HUSK_ASSIST_CHAIN_LANDING_CALL :
                    cache_jump ? HUSK_ASSIST_CHAIN_LANDING_JUMP : HUSK_ASSIST_CHAIN;
            cache_call = false;
            cache_jump = false;
            if (EX_TBFLAG_A64(env->hflags, PAUTH_ACTIVE)) {
                state->x[30] = arm_husk_pauth(env, state->x[30], state->sp,
                                             landing == 0xd503237f);
            }
            break;
        default:
            if (ctx->landing && husk_arm64_landing_supported(landing))
                return cache_call ? HUSK_ASSIST_CHAIN_LANDING_CALL :
                    cache_jump ? HUSK_ASSIST_CHAIN_LANDING_JUMP : HUSK_ASSIST_CHAIN;
            return HUSK_ASSIST_BRANCH;
        }
        state->pc += 4;
        env->btype = 0;
        state->access_flags &= ~HUSK_BTYPE_MASK;
        return cache_call ? HUSK_ASSIST_CHAIN_CALL : cache_jump ? HUSK_ASSIST_CHAIN_JUMP : HUSK_ASSIST_CHAIN;
    }
    if ((insn & 0xfffff0ff) == 0xd50330bf || (insn & 0xfffff0ff) == 0xd503309f) {
        smp_mb();
        state->pc += 4;
        return HUSK_ASSIST_CONTINUE;
    }
    /* Entry and indirect-branch landing checks run before this callback.
     * A BTI hint within an executing block has no further side effects. */
    switch (insn) {
    case 0xd503241f: case 0xd503245f: case 0xd503249f: case 0xd50324df:
        state->pc += 4;
        return HUSK_ASSIST_CONTINUE;
    case 0xd50320ff: operation = 4; break; // XPACLRI
    case 0xd503211f: case 0xd503215f: case 0xd503219f: case 0xd50321df:
        reg = 17;
        modifier = state->x[16];
        operation = (insn >> 6) & 3;
        break;
    case 0xd503231f: case 0xd503233f: case 0xd503235f: case 0xd503237f:
    case 0xd503239f: case 0xd50323bf: case 0xd50323df: case 0xd50323ff:
        modifier = (insn & 32) ? state->sp : 0;
        operation = (insn >> 6) & 3;
        break;
    default:
        return HUSK_ASSIST_DECLINED;
    }
    if (EX_TBFLAG_A64(env->hflags, PAUTH_ACTIVE)) {
        if (!ctx->assisted_pauth) {
            fprintf(stderr, "[husk-arm64] PAUTH assist active\n");
            ctx->assisted_pauth = true;
        }
        /* Authentication can raise a guest exception. Preserve all architectural
         * state at the faulting PC before QEMU leaves through its CPU loop. */
        husk_export_state(env, state);
        state->x[reg] = arm_husk_pauth(env, state->x[reg], modifier, operation);
    }
    state->pc += 4;
    return HUSK_ASSIST_PAUTH;
}

int arm_husk_exec_user(CPUState *cs)
{
    ARMCPU *cpu = ARM_CPU(cs);
    CPUARMState *env = &cpu->env;
    HuskArm64Qemu *ctx = cpu->husk_exec;
    static int enabled = -1;
    static bool direct;
    static bool bridge;
    static bool kernel;
    static bool landing = true, pauth_landing = true;
    static unsigned tcg_quantum_us = 2000;
    static unsigned slice_budget = 4096;
    if (enabled == -1) {
        const char *setting = getenv("HUSK_ARM64_EXEC");
        enabled = setting && !strcmp(setting, "1");
        const char *backend = getenv("HUSK_ARM64_BACKEND");
        direct = backend && !strcmp(backend, "direct");
        const char *kernel_setting = getenv("HUSK_ARM64_KERNEL");
        kernel = direct && kernel_setting && !strcmp(kernel_setting, "1");
        const char *bridge_setting = getenv("HUSK_ARM64_TCG_BRIDGE");
        bridge = bridge_setting && !strcmp(bridge_setting, "1");
        const char *landing_setting = getenv("HUSK_ARM64_LANDING");
        landing = !landing_setting || strcmp(landing_setting, "0");
        const char *pauth_setting = getenv("HUSK_ARM64_PAC_LANDING");
        pauth_landing = !pauth_setting || strcmp(pauth_setting, "0");
        const char *quantum = getenv("HUSK_ARM64_TCG_QUANTUM_US");
        if (quantum && (qemu_strtoui(quantum, NULL, 10, &tcg_quantum_us) || tcg_quantum_us > 1000000)) {
            error_report("HUSK_ARM64_TCG_QUANTUM_US must be between 0 and 1000000");
            exit(EXIT_FAILURE);
        }
        const char *budget = getenv("HUSK_ARM64_SLICE_BUDGET");
        if (budget && (qemu_strtoui(budget, NULL, 10, &slice_budget) ||
                       slice_budget < 256 || slice_budget > 1048576)) {
            error_report("HUSK_ARM64_SLICE_BUDGET must be between 256 and 1048576");
            exit(EXIT_FAILURE);
        }
        if (enabled && (current_machine->smp.cpus != 1 || icount_enabled())) {
            error_report("HUSK_ARM64_EXEC requires one vCPU and wall-clock timing");
            exit(EXIT_FAILURE);
        }
    }
    unsigned el = arm_current_el(env);
    if (!enabled || !env->aarch64 || (el != 0 && !(kernel && el == 1)) ||
        arm_is_secure(env) || cs->singlestep_enabled ||
        !QTAILQ_EMPTY(&cs->breakpoints) || !QTAILQ_EMPTY(&cs->watchpoints) ||
        cs->cflags_next_tb != -1 || (env->pc & 3) ||
        TARGET_PAGE_SIZE < 4096 ||
        EX_TBFLAG_ANY(env->hflags, BE_DATA) ||
        EX_TBFLAG_ANY(env->hflags, SS_ACTIVE) ||
        EX_TBFLAG_ANY(env->hflags, PSTATE__IL) ||
        EX_TBFLAG_A64(env->hflags, VL) ||
        EX_TBFLAG_A64(env->hflags, SME_TRAP_NONSTREAMING) ||
        /* The Direct backend uses QEMU helpers for BTI validation. Other
         * backends leave guarded-page execution to TCG. */
        (EX_TBFLAG_A64(env->hflags, BT) && !direct) ||
        EX_TBFLAG_A64(env->hflags, MTE_ACTIVE) ||
        EX_TBFLAG_A64(env->hflags, PSTATE_SM)) {
        return 0;
    }
    if (env->btype && !direct) {
        /* QEMU checks the landing instruction, including invalid BTI targets. */
        return 0;
    }
    if (ctx && ctx->resume_after_ns[el != 0] &&
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) < ctx->resume_after_ns[el != 0]) {
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
        if (direct && bridge && ctx && husk_tcg_bridge(insn, el)) husk_one_tcg(cs);
        return 0;
    }
    if (direct && env->btype) {
        arm_husk_bti_check(env, insn);
        switch (insn) {
        case 0xd503241f: case 0xd503245f: case 0xd503249f: case 0xd50324df:
            break;
        case 0xd503233f: case 0xd503237f:
            if (landing && pauth_landing) goto landing_ready;
            if (EX_TBFLAG_A64(env->hflags, PAUTH_ACTIVE)) {
                env->xregs[30] = arm_husk_pauth(env, env->xregs[30], env->xregs[31],
                                               insn == 0xd503237f);
            }
            break;
        default:
            /* Unsupported landing instructions retain BTYPE and run in TCG.
             * Supported instructions export it if execution faults. */
            if (!landing || !husk_arm64_landing_supported(insn)) {
                husk_one_tcg(cs);
                return 0;
            }
            goto landing_ready;
        }
        env->pc += 4;
        env->btype = 0;
        return 1;
    }

landing_ready:

    if (!ctx) {
        ctx = g_new0(HuskArm64Qemu, 1);
        const char *dispatch = getenv("HUSK_ARM64_DISPATCH");
        ctx->poll_dispatch = direct && (!dispatch || strcmp(dispatch, "0"));
        const char *assist = getenv("HUSK_ARM64_ASSIST");
        ctx->assist = direct && (!assist || strcmp(assist, "0"));
        const char *chain = getenv("HUSK_ARM64_BRANCH_CHAIN");
        ctx->chain_assist = !chain || strcmp(chain, "0");
        cpu->husk_exec = ctx;
        ctx->kernel = kernel;
        ctx->landing = landing;
        ctx->pauth_landing = pauth_landing;
        const char *reuse = getenv("HUSK_ARM64_FASTMEM_REUSE");
        ctx->reuse_mappings = direct && !kernel && (!reuse || strcmp(reuse, "0"));
        ctx->profile = getenv("HUSK_ARM64_PROFILE") != NULL;
        if (ctx->profile) {
            arm_register_el_change_hook(cpu, husk_profile_el, ctx);
            husk_profile_phase(ctx, el != 0);
        }
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
        fprintf(stderr, "[husk-arm64] EL0 engine active, vCPU=%d\n", cs->cpu_index);
    }
    ctx->env = env;
    if (el == 1 && !ctx->kernel_started) {
        fprintf(stderr, "[husk-arm64] kernel Direct active\n");
        ctx->kernel_started = true;
    }
    ctx->code_host = code_host;
    ctx->code_hosts[0] = code_host;
    ctx->code_count = 1;
    HuskArm64State state = { .sp = env->xregs[31], .pc = env->pc,
                             .nzcv = pstate_read(env) & PSTATE_NZCV,
                             .fpcr = vfp_get_fpcr(env), .fpsr = vfp_get_fpsr(env),
                             .access_flags = !EX_TBFLAG_ANY(env->hflags, ALIGN_MEM) |
                                 (EX_TBFLAG_ANY(env->hflags, FPEXC_EL) ? 2 : 0) |
                                 (cpu_isar_feature(aa64_atomics, cpu) ? 4 : 0) |
                                 (cpu_isar_feature(aa64_crc32, cpu) ? 64 : 0) |
                                 (husk_mops_allowed(env) ? HUSK_MOPS_ALLOWED : 0) |
                                 ((el == 0 && (EX_TBFLAG_A64(env->hflags, TBID) & 1)) ? 8 : 0) |
                                 (env->btype << HUSK_BTYPE_SHIFT),
                             .tpidr_el0 = env->cp15.tpidr_el[0],
                             .exclusive_address = env->exclusive_addr,
                             .exclusive_value = env->exclusive_val };
    memcpy(state.x, env->xregs, sizeof(state.x));
    for (unsigned i = 0; i < 32; ++i) {
        memcpy(state.v[i], aa64_vfp_qreg(env, i), sizeof(state.v[i]));
    }
    husk_profile_phase(ctx, 2 + (el != 0));
    uint64_t epoch = 0;
    if (ctx->reuse_mappings) {
        uint64_t generation = tlb_husk_mapping_generation();
        int mmu = arm_env_mmu_index(env);
        if (ctx->mapping_generation != generation || ctx->mapping_mmu != mmu) {
            ctx->mapping_generation = generation;
            ctx->mapping_mmu = mmu;
            ++ctx->mapping_epoch;
        }
        epoch = ctx->mapping_epoch;
    }
    HuskArm64Exit result = husk_arm64_run(ctx->engine, &state, page, code_host,
                                        (HuskArm64Memory){ctx, husk_read, husk_write, husk_map_page,
                                                         ctx->assist ? husk_assist : NULL,
                                                         direct ? husk_map_code : NULL, epoch},
                                        slice_budget);
    husk_profile_phase(ctx, el != 0);
    if (result == HUSK_ARM64_ERROR) {
        error_report("Husk ARM64 execution failed at guest PC 0x%" PRIx64, env->pc);
        exit(EXIT_FAILURE);
    }
    if (result == HUSK_ARM64_BRANCH) state.pc = arm_husk_branch_pc(env, state.pc);
    husk_export_state(env, &state);
    if (result == HUSK_ARM64_FALLBACK) {
        void *stopped_host = NULL;
        int stopped_flags = probe_access_flags(env, state.pc, 4, MMU_INST_FETCH,
                                               arm_env_mmu_index(env), true, &stopped_host, 0);
        uint32_t stopped_insn = !stopped_flags && stopped_host ? ldl_le_p(stopped_host) : 0;
        if ((stopped_insn & 0xffe0001f) == 0xd4000001) {
            /* SVC already ends the user region. Kernel execution supplies the
             * transition; delaying EL0 after its return discards useful slices. */
            ctx->resume_after_ns[el != 0] = 0;
            return -1;
        }
        if (direct && bridge && husk_tcg_bridge(stopped_insn, el)) {
            ctx->resume_after_ns[el != 0] = 0;
            husk_one_tcg(cs);
            return -1;
        }
        /* Let TCG build and reuse connected blocks before another Direct entry.
         * Immediate re-entry can prevent those connections from ever forming. */
        ctx->resume_after_ns[el != 0] = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + (int64_t)tcg_quantum_us * 1000;
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
