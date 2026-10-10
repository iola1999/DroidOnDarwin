/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <unicorn/unicorn.h>
#include <unicorn/arm64.h>
#include <mach/mach.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { PAGE_BYTES = 16384,
       DATA_BYTES = 65536,
       ROUNDS = 3,
       SWITCHES = 256 };
static const uint64_t CODE_VA = 0x10000, DATA_VA = 0x100000;
static const uint64_t CODE_PA = 0x10000000, DATA_PA = 0x20000000;
typedef uint64_t (*Kernel)(uint64_t *, uint64_t);
extern uint64_t translation_integer(uint64_t *, uint64_t), translation_integer_end[];
extern uint64_t translation_memory(uint64_t *, uint64_t), translation_memory_end[];
extern uint64_t translation_atomic(uint64_t *, uint64_t), translation_atomic_end[];
extern uint64_t translation_state(uint64_t *, uint64_t), translation_state_end[];

typedef struct {
    uc_engine *uc;
    uint8_t *code;
    uint64_t *data[2];
    size_t code_size;
    unsigned active;
    uc_prot data_permissions;
    uint64_t fills;
} Engine;

static void check(bool passed, const char *test)
{
    printf("{\"test\":\"%s\",\"passed\":%s}\n", test, passed ? "true" : "false");
    if (!passed)
        exit(1);
}

static void checked(uc_err error)
{
    if (error != UC_ERR_OK) {
        fprintf(stderr, "Unicorn: %s\n", uc_strerror(error));
        exit(1);
    }
}

static uint64_t now(clockid_t clock)
{
    struct timespec value;
    if (clock_gettime(clock, &value))
        exit(1);
    return (uint64_t)value.tv_sec * 1000000000 + value.tv_nsec;
}

static void memory_sample(const char *phase)
{
    task_vm_info_data_t info = {0};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) != KERN_SUCCESS)
        exit(1);
    printf("{\"phase\":\"%s\",\"footprint_bytes\":%" PRIu64 ",\"resident_bytes\":%" PRIu64 ",\"resident_peak_bytes\":%" PRIu64 "}\n",
           phase, (uint64_t)info.phys_footprint, (uint64_t)info.resident_size, (uint64_t)info.resident_size_peak);
}

static void *allocate(size_t size)
{
    void *result = NULL;
    if (posix_memalign(&result, PAGE_BYTES, size))
        exit(1);
    memset(result, 0, size);
    return result;
}

static bool fill(uc_engine *uc, uint64_t address, uc_mem_type access, uc_tlb_entry *entry, void *opaque)
{
    (void)uc;
    (void)access;
    Engine *e = opaque;
    ++e->fills;
    uint64_t page = address & ~(uint64_t)(PAGE_BYTES - 1);
    if (page == CODE_VA) {
        entry->paddr = CODE_PA;
        entry->perms = UC_PROT_READ | UC_PROT_EXEC;
        return true;
    }
    if (page >= DATA_VA && page < DATA_VA + DATA_BYTES) {
        entry->paddr = DATA_PA + e->active * DATA_BYTES + page - DATA_VA;
        entry->perms = e->data_permissions;
        return true;
    }
    return false;
}

static void engine_open(Engine *e)
{
    memset(e, 0, sizeof(*e));
    checked(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &e->uc));
    checked(uc_ctl_set_cpu_model(e->uc, UC_CPU_ARM64_MAX));
    checked(uc_ctl_set_page_size(e->uc, PAGE_BYTES));
    checked(uc_ctl_set_tcg_buffer_size(e->uc, (uint32_t)(16 * 1024 * 1024)));
    checked(uc_ctl_tlb_mode(e->uc, UC_TLB_VIRTUAL));
    e->code = allocate(PAGE_BYTES);
    checked(uc_mem_map_ptr(e->uc, CODE_PA, PAGE_BYTES, UC_PROT_ALL, e->code));
    for (unsigned i = 0; i < 2; ++i) {
        e->data[i] = allocate(DATA_BYTES);
        checked(uc_mem_map_ptr(e->uc, DATA_PA + i * DATA_BYTES, DATA_BYTES, UC_PROT_ALL, e->data[i]));
    }
    e->data_permissions = UC_PROT_READ | UC_PROT_WRITE;
    uc_hook hook;
    checked(uc_hook_add(e->uc, &hook, UC_HOOK_TLB_FILL, fill, e, 1, 0));
}

static void load(Engine *e, Kernel kernel, const void *end)
{
    e->code_size = (uintptr_t)end - (uintptr_t)kernel;
    if (!e->code_size || e->code_size > PAGE_BYTES)
        exit(1);
    memcpy(e->code, (const void *)kernel, e->code_size);
    checked(uc_ctl_flush_tb(e->uc));
    checked(uc_ctl_flush_tlb(e->uc));
}

static uc_err execute(Engine *e, uint64_t iterations, uint64_t *value)
{
    uint64_t pointer = DATA_VA, stop = CODE_VA + e->code_size;
    checked(uc_reg_write(e->uc, UC_ARM64_REG_X0, &pointer));
    checked(uc_reg_write(e->uc, UC_ARM64_REG_X1, &iterations));
    checked(uc_reg_write(e->uc, UC_ARM64_REG_LR, &stop));
    /* The runner enforces a process deadline; no per-instruction hook is used. */
    uc_err error = uc_emu_start(e->uc, CODE_VA, stop, 0, 0);
    if (error == UC_ERR_OK)
        checked(uc_reg_read(e->uc, UC_ARM64_REG_X0, value));
    else {
        uint64_t pc = 0;
        checked(uc_reg_read(e->uc, UC_ARM64_REG_PC, &pc));
        fprintf(stderr, "execute failed: pc=0x%" PRIx64 " stop=0x%" PRIx64 " fills=%" PRIu64 " error=%s\n", pc, stop, e->fills, uc_strerror(error));
    }
    return error;
}

static void reset_data(uint64_t *data)
{
    for (unsigned i = 0; i < DATA_BYTES / sizeof(*data); ++i)
        data[i] = i + 17;
}

static void measure(Engine *e, const char *name, Kernel kernel, const void *end, uint64_t iterations)
{
    uint64_t *native = allocate(DATA_BYTES);
    load(e, kernel, end);
    for (unsigned round = 0; round < ROUNDS; ++round) {
        uint64_t values[2];
        for (unsigned order = 0; order < 2; ++order) {
            unsigned translated = (round + order) % 2;
            uint64_t *data = translated ? e->data[0] : native;
            reset_data(data);
            if (translated) {
                checked(execute(e, 64, &values[1]));
                reset_data(data);
            }
            uint64_t fills = e->fills;
            uint64_t wall = now(CLOCK_MONOTONIC), cpu = now(CLOCK_THREAD_CPUTIME_ID);
            if (translated)
                checked(execute(e, iterations, &values[1]));
            else
                values[0] = kernel(data, iterations);
            cpu = now(CLOCK_THREAD_CPUTIME_ID) - cpu;
            wall = now(CLOCK_MONOTONIC) - wall;
            printf("{\"bench\":\"user_translation\",\"kernel\":\"%s\",\"engine\":\"%s\",\"round\":%u,\"iterations\":%" PRIu64 ",\"wall_ns\":%" PRIu64 ",\"thread_cpu_ns\":%" PRIu64 ",\"tlb_fills\":%" PRIu64 "}\n",
                   name, translated ? "unicorn" : "native", round, iterations, wall, cpu, e->fills - fills);
        }
        check(values[0] == values[1] && !memcmp(native, e->data[0], DATA_BYTES), name);
    }
    free(native);
    memory_sample(name);
}

static void boundaries(Engine *e)
{
    load(e, translation_atomic, translation_atomic_end);
    e->data[0][0] = 17;
    e->data[1][0] = 83;
    uint64_t value, wall = now(CLOCK_MONOTONIC), cpu = now(CLOCK_THREAD_CPUTIME_ID);
    for (unsigned i = 0; i < SWITCHES; ++i) {
        e->active = i % 2;
        /* Both address spaces use the same physical code page. */
        checked(uc_ctl_flush_tlb(e->uc));
        checked(execute(e, 64, &value));
        if (value != (e->active ? 83 : 17) + (i / 2 + 1) * 64)
            exit(1);
    }
    cpu = now(CLOCK_THREAD_CPUTIME_ID) - cpu;
    wall = now(CLOCK_MONOTONIC) - wall;
    check(e->data[0][0] == 17 + SWITCHES / 2 * 64 && e->data[1][0] == 83 + SWITCHES / 2 * 64,
          "same_virtual_address_private_data");
    printf("{\"bench\":\"translation_context_switch\",\"switches\":%d,\"iterations_per_switch\":64,\"wall_ns\":%" PRIu64 ",\"thread_cpu_ns\":%" PRIu64 "}\n", SWITCHES, wall, cpu);
    e->data_permissions = UC_PROT_READ;
    checked(uc_ctl_flush_tlb(e->uc));
    uint64_t before = e->data[e->active][0];
    uc_err error = execute(e, 1, &value);
    uint64_t pc = 0;
    checked(uc_reg_read(e->uc, UC_ARM64_REG_PC, &pc));
    /* The virtual TLB reports a CPU exception at the denied STLXR. */
    check(error == UC_ERR_EXCEPTION && pc == CODE_VA + 12 && e->data[e->active][0] == before,
          "software_tlb_write_protection");
    e->data_permissions = UC_PROT_READ | UC_PROT_WRITE;
    checked(uc_ctl_flush_tlb(e->uc));
    checked(execute(e, 1, &value));
    check(value == before + 1, "software_tlb_permission_restore");

    const uint32_t returns[] = {0xd2800220, 0xd65f03c0}; /* mov x0, #17; ret */
    e->code_size = sizeof(returns);
    memcpy(e->code, returns, sizeof(returns));
    checked(uc_ctl_flush_tb(e->uc));
    checked(uc_ctl_flush_tlb(e->uc));
    checked(execute(e, 1, &value));
    check(value == 17, "dynamic_code_first_execution");
    const uint32_t changed = 0xd2800a60; /* mov x0, #83 */
    memcpy(e->code, &changed, sizeof(changed));
    checked(uc_ctl_flush_tb(e->uc));
    checked(uc_ctl_flush_tlb(e->uc));
    checked(execute(e, 1, &value));
    check(value == 83, "dynamic_code_invalidation");
    memory_sample("boundaries");
}

static void context_switches(Engine *e)
{
    uc_context *contexts[2];
    load(e, translation_state, translation_state_end);
    for (unsigned i = 0; i < 2; ++i) {
        uint64_t x18 = 17 + 83 * i, tls = 0xabc000 + 4096 * i;
        uint64_t vector[2] = {17 + i, 31 + i}, increment[2] = {3, 5};
        uint64_t nzcv = i ? 0x60000000 : 0x90000000;
        checked(uc_reg_write(e->uc, UC_ARM64_REG_X18, &x18));
        checked(uc_reg_write(e->uc, UC_ARM64_REG_TPIDR_EL0, &tls));
        checked(uc_reg_write(e->uc, UC_ARM64_REG_Q0, vector));
        checked(uc_reg_write(e->uc, UC_ARM64_REG_Q1, increment));
        checked(uc_reg_write(e->uc, UC_ARM64_REG_NZCV, &nzcv));
        checked(uc_context_alloc(e->uc, &contexts[i]));
        checked(uc_context_save(e->uc, contexts[i]));
    }
    memory_sample("contexts_ready");
    uint64_t wall = now(CLOCK_MONOTONIC), cpu = now(CLOCK_THREAD_CPUTIME_ID);
    for (unsigned i = 0; i < SWITCHES; ++i) {
        unsigned index = i % 2;
        uint64_t value;
        checked(uc_context_restore(e->uc, contexts[index]));
        checked(execute(e, 1, &value));
        checked(uc_context_save(e->uc, contexts[index]));
        if (value != ((0xabc000 + 4096 * index) ^ (17 + 83 * index + i / 2 + 1)))
            exit(1);
    }
    cpu = now(CLOCK_THREAD_CPUTIME_ID) - cpu;
    wall = now(CLOCK_MONOTONIC) - wall;
    for (unsigned i = 0; i < 2; ++i) {
        uint64_t vector[2], nzcv;
        checked(uc_context_reg_read(contexts[i], UC_ARM64_REG_Q0, vector));
        checked(uc_context_reg_read(contexts[i], UC_ARM64_REG_NZCV, &nzcv));
        check(vector[0] == 17 + i + SWITCHES / 2 * 3 && vector[1] == 31 + i + SWITCHES / 2 * 5 &&
                  nzcv == (i ? 0x60000000 : 0x90000000),
              "guest_tls_x18_simd_context_restore");
        checked(uc_context_free(contexts[i]));
    }
    printf("{\"bench\":\"translation_register_switch\",\"switches\":%d,\"context_bytes\":%zu,\"wall_ns\":%" PRIu64 ",\"thread_cpu_ns\":%" PRIu64 "}\n", SWITCHES, uc_context_size(e->uc), wall, cpu);
    memory_sample("contexts_closed");
}

int main(void)
{
    setbuf(stdout, NULL);
    unsigned major, minor;
    uc_version(&major, &minor);
    printf("{\"engine_version\":\"%u.%u\",\"guest_page_bytes\":%d,\"tcg_buffer_bytes\":16777216}\n", major, minor, PAGE_BYTES);
    memory_sample("before_engine");
    Engine engine;
    engine_open(&engine);
    memory_sample("engine_ready");
    measure(&engine, "integer", translation_integer, translation_integer_end, 320000000);
    measure(&engine, "memory", translation_memory, translation_memory_end, 160000000);
    measure(&engine, "atomic", translation_atomic, translation_atomic_end, 40000000);
    boundaries(&engine);
    context_switches(&engine);
    checked(uc_close(engine.uc));
    free(engine.code);
    free(engine.data[0]);
    free(engine.data[1]);
    memory_sample("engine_closed");
    puts("{\"result\":\"passed\"}");
    return 0;
}
