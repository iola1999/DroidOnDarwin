/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "translation/bpf-code-memory.h"
#include <dispatch/dispatch.h>
#include <errno.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "BPF code memory:%d: %s errno=%d\n", __LINE__, #c, errno); \
            abort(); \
        } \
    } while (0)
#define PAGE TRANSLATION_BPF_CODE_PAGE

typedef uint64_t (*Callback)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static _Thread_local sigjmp_buf resume;
static _Thread_local uintptr_t expected_address;
static _Thread_local volatile sig_atomic_t fault_signal;

static vm_prot_t permissions(const void *pointer)
{
    mach_vm_address_t address = (uintptr_t)pointer;
    mach_vm_size_t size;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object;
    kern_return_t result = mach_vm_region(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64,
                                          (vm_region_info_t)&info, &count, &object);
    if (result == KERN_INVALID_ADDRESS)
        return VM_PROT_NONE;
    CHECK(result == KERN_SUCCESS);
    if (object != MACH_PORT_NULL)
        CHECK(mach_port_deallocate(mach_task_self(), object) == KERN_SUCCESS);
    return address <= (uintptr_t)pointer && (uintptr_t)pointer - address < size ? info.protection : VM_PROT_NONE;
}

static void caught(int signal, siginfo_t *info, void *unused)
{
    (void)unused;
    if ((uintptr_t)info->si_addr != expected_address)
        _Exit(128 + signal);
    fault_signal = signal;
    siglongjmp(resume, 1);
}

static void check_fault(void *address, bool execute)
{
    const int signals[] = {SIGBUS, SIGSEGV};
    struct sigaction original[2], action = {.sa_sigaction = caught, .sa_flags = SA_SIGINFO};
    sigemptyset(&action.sa_mask);
    sigset_t mask;
    CHECK(!pthread_sigmask(SIG_BLOCK, NULL, &mask));
    for (unsigned i = 0; i < 2; ++i)
        CHECK(!sigaction(signals[i], &action, &original[i]));
    expected_address = (uintptr_t)address;
    fault_signal = 0;
    if (!sigsetjmp(resume, 0)) {
        if (execute)
            ((Callback)address)(1, 2, 3, 4, 5);
        else
            *(volatile uint32_t *)address = 0xd65f03c0;
    }
    for (unsigned i = 0; i < 2; ++i)
        CHECK(!sigaction(signals[i], &original[i], NULL));
    CHECK(!pthread_sigmask(SIG_SETMASK, &mask, NULL));
    CHECK(fault_signal == SIGBUS || fault_signal == SIGSEGV);
}

static void *update(void *address)
{
    for (unsigned i = 0; i < 1024; ++i) {
        uint32_t words[] = {0x52800000 | (i << 5), 0xd65f03c0};
        void *alias = translation_bpf_code_write_alias(address, sizeof(words));
        CHECK(alias);
        memcpy(alias, words, sizeof(words));
        CHECK(!translation_bpf_code_publish(address, sizeof(words)));
        CHECK(((Callback)address)(0, 0, 0, 0, 0) == i);
    }
    return NULL;
}

static void write_code(void *address, const void *source, size_t bytes)
{
    void *alias = translation_bpf_code_write_alias(address, bytes);
    CHECK(alias);
    memcpy(alias, source, bytes);
    CHECK(!translation_bpf_code_publish(address, bytes));
}

static void run(void *done)
{
    CHECK(translation_bpf_code_capacity() == 0);
    CHECK(translation_bpf_code_check() == -1 && errno == EINVAL);
    CHECK(translation_bpf_code_alloc(PAGE) == NULL && errno == ENOMEM);
    CHECK(translation_bpf_code_prepare(0) == -1 && errno == EINVAL);
    CHECK(translation_bpf_code_prepare(PAGE - 1) == -1 && errno == EINVAL);
    CHECK(!translation_bpf_code_prepare(2 * PAGE));
    CHECK(translation_bpf_code_prepare(PAGE) == -1 && errno == EBUSY);
    CHECK(translation_bpf_code_capacity() == 2 * PAGE);
    CHECK(!translation_bpf_code_check());
    CHECK(translation_bpf_code_alloc(0) == NULL && errno == EINVAL);
    CHECK(translation_bpf_code_alloc(SIZE_MAX) == NULL && errno == EINVAL);
    unsigned char *first = translation_bpf_code_alloc(1);
    unsigned char *second = translation_bpf_code_alloc(PAGE);
    CHECK(first && second == first + PAGE);
    CHECK(translation_bpf_code_alloc(1) == NULL && errno == ENOMEM);
    CHECK(permissions(first) == (VM_PROT_READ | VM_PROT_EXECUTE));
    CHECK(permissions(second) == (VM_PROT_READ | VM_PROT_EXECUTE));
    void *first_alias = translation_bpf_code_write_alias(first, PAGE);
    void *second_alias = translation_bpf_code_write_alias(second, PAGE);
    CHECK(first_alias && second_alias && first_alias != first && second_alias != second);
    CHECK(permissions(first_alias) == (VM_PROT_READ | VM_PROT_WRITE));
    CHECK(permissions(second_alias) == (VM_PROT_READ | VM_PROT_WRITE));
    CHECK(translation_bpf_code_release(false) == -1 && errno == EBUSY);
    CHECK(translation_bpf_code_check() == -1 && errno == EBUSY);
    CHECK(translation_bpf_code_free(first + 1) == -1 && errno == EINVAL);
    puts("{\"test\":\"bpf_code_allocation_and_exhaustion\",\"passed\":true}");

    const uint32_t sum[] = {0x8b010000, 0x8b020000, 0xca030000, 0x8b040000, 0xd65f03c0};
    write_code(second, sum, sizeof(sum));
    CHECK(((Callback)second)(17, 23, 31, 47, 59) == ((17UL + 23 + 31) ^ 47) + 59);
    uint32_t branch[] = {0x58000050, 0xd61f0200};
    write_code(first, branch, sizeof(branch));
    write_code(first + sizeof(branch), &second, sizeof(second));
    CHECK(((Callback)first)(17, 23, 31, 47, 59) == ((17UL + 23 + 31) ^ 47) + 59);
    CHECK(!memcmp(second, sum, sizeof(sum)));
    CHECK(translation_bpf_code_write_alias(first + PAGE - 4, 8) == NULL && errno == EINVAL);
    CHECK(translation_bpf_code_write_alias(NULL, sizeof(sum)) == NULL && errno == EINVAL);
    CHECK(translation_bpf_code_write_alias(first, 0) == NULL && errno == EINVAL);
    CHECK(translation_bpf_code_publish(first + PAGE - 4, 8) == -1 && errno == EINVAL);
    CHECK(translation_bpf_code_publish(NULL, sizeof(sum)) == -1 && errno == EINVAL);
    CHECK(translation_bpf_code_publish(first, 0) == -1 && errno == EINVAL);
    check_fault(first, false);
    check_fault(first_alias, true);
    CHECK(((Callback)first)(17, 23, 31, 47, 59) == ((17UL + 23 + 31) ^ 47) + 59);
    puts("{\"test\":\"bpf_code_callback_abi_bounds_and_rx_permission\",\"passed\":true}");

    pthread_t threads[2];
    CHECK(!pthread_create(&threads[0], NULL, update, first));
    CHECK(!pthread_create(&threads[1], NULL, update, second));
    CHECK(!pthread_join(threads[0], NULL) && !pthread_join(threads[1], NULL));
    puts("{\"test\":\"bpf_code_parallel_publication\",\"passed\":true,\"updates_per_range\":1024}");

    CHECK(!translation_bpf_code_free(first));
    CHECK(*(const uint32_t *)first == 0xd4200000);
    CHECK(translation_bpf_code_free(first) == -1 && errno == EINVAL);
    CHECK(translation_bpf_code_write_alias(first, sizeof(sum)) == NULL && errno == EINVAL);
    CHECK(translation_bpf_code_alloc(PAGE) == first);
    write_code(first, sum, sizeof(sum));
    CHECK(((Callback)first)(17, 23, 31, 47, 59) == ((17UL + 23 + 31) ^ 47) + 59);
    CHECK(!translation_bpf_code_free(first) && !translation_bpf_code_free(second));
    void *both = translation_bpf_code_alloc(2 * PAGE);
    CHECK(both == first);
    write_code(second + PAGE - sizeof(sum), sum, sizeof(sum));
    CHECK(((Callback)(second + PAGE - sizeof(sum)))(17, 23, 31, 47, 59) == ((17UL + 23 + 31) ^ 47) + 59);
    CHECK(translation_bpf_code_free(second) == -1 && errno == EINVAL);
    CHECK(!translation_bpf_code_free(both) && !translation_bpf_code_free(NULL));
    CHECK(!translation_bpf_code_release(false));
    CHECK(translation_bpf_code_capacity() == 0);
    CHECK(permissions(first) == VM_PROT_NONE && permissions(second) == VM_PROT_NONE);
    CHECK(permissions(first_alias) == VM_PROT_NONE && permissions(second_alias) == VM_PROT_NONE);
    CHECK(!translation_bpf_code_release(false));
    puts("{\"test\":\"bpf_code_reuse_and_release\",\"passed\":true}");
    CHECK(!translation_bpf_code_prepare(2 * PAGE));
    void *retained = translation_bpf_code_alloc(2 * PAGE);
    void *retained_alias = translation_bpf_code_write_alias(retained, 2 * PAGE);
    CHECK(retained && retained_alias);
    CHECK(translation_bpf_code_release(false) == -1 && errno == EBUSY);
    CHECK(!translation_bpf_code_release(true));
    CHECK(translation_bpf_code_capacity() == 0);
    CHECK(permissions(retained) == VM_PROT_NONE && permissions(retained_alias) == VM_PROT_NONE);
    CHECK(translation_bpf_code_free(retained) == -1 && errno == EINVAL);
    CHECK(translation_bpf_code_write_alias(retained, 4) == NULL && errno == EINVAL);
    CHECK(!translation_bpf_code_prepare(PAGE) && !translation_bpf_code_check());
    CHECK(!translation_bpf_code_release(false));
    puts("{\"test\":\"bpf_code_stopped_kernel_release\",\"passed\":true}");
    puts("{\"result\":\"passed\"}");
    if (done)
        dispatch_semaphore_signal(done);
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "gcd")) {
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        dispatch_async_f(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), done, run);
        CHECK(!dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER));
        dispatch_release(done);
    } else
        run(NULL);
    return 0;
}
