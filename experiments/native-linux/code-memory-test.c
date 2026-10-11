/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "code-memory.h"
#include <dispatch/dispatch.h>
#include <errno.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define PAGE 16384
#define CHECK(c) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "code-memory:%d: %s errno=%d\n", __LINE__, #c, errno); \
            abort(); \
        } \
    } while (0)

static vm_prot_t permissions(void *pointer)
{
    mach_vm_address_t address = (uintptr_t)pointer;
    mach_vm_size_t size;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object;
    CHECK(mach_vm_region(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64,
                         (vm_region_info_t)&info, &count, &object) == KERN_SUCCESS);
    if (object != MACH_PORT_NULL)
        CHECK(mach_port_deallocate(mach_task_self(), object) == KERN_SUCCESS);
    CHECK(address <= (uintptr_t)pointer && (uintptr_t)pointer - address < size);
    return info.protection;
}

static void update(NceCode code, unsigned value)
{
    uint32_t words[] = {0x52800000 | (value << 5), 0xd65f03c0};
    memcpy(code.rw, words, sizeof(words));
    sys_icache_invalidate(code.rx, sizeof(words));
    CHECK(!memcmp(code.rx, words, sizeof(words)));
}

static void *execute_updates(void *opaque)
{
    NceCode code = *(NceCode *)opaque;
    for (unsigned i = 0; i < 1024; ++i) {
        update(code, i);
        CHECK(((int (*)(void))code.rx)() == (int)i);
    }
    return NULL;
}

static void run(void *unused)
{
    (void)unused;
    CHECK(!nce_code_prepare_range(NULL, PAGE).rw && errno == EINVAL);
    unsigned char *space = mmap(NULL, 3 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(space != MAP_FAILED);
    CHECK(!nce_code_prepare_range(space + 1, PAGE).rw && errno == EINVAL);
    CHECK(!nce_code_prepare_range(space, PAGE - 1).rw && errno == EINVAL);
    CHECK(!nce_code_prepare_range(space, 0).rw && errno == EINVAL);
    NceCode first = nce_code_prepare_range(space + PAGE, PAGE);
    CHECK(first.rw && first.rx == space + PAGE && first.size == PAGE && first.rw != first.rx);
    CHECK(permissions(first.rx) == (VM_PROT_READ | VM_PROT_EXECUTE));
    CHECK(permissions(first.rw) == (VM_PROT_READ | VM_PROT_WRITE));
    CHECK(permissions(space) == (VM_PROT_READ | VM_PROT_WRITE));
    CHECK(permissions(space + 2 * PAGE) == (VM_PROT_READ | VM_PROT_WRITE));
    space[0] = 17;
    space[2 * PAGE] = 29;
    puts("{\"test\":\"code_memory_permissions_and_bounds\",\"passed\":true}");

    void *other = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(other != MAP_FAILED);
    NceCode second = nce_code_prepare_range(other, PAGE);
    CHECK(second.rw);
    pthread_t threads[2];
    CHECK(!pthread_create(&threads[0], NULL, execute_updates, &first));
    CHECK(!pthread_create(&threads[1], NULL, execute_updates, &second));
    CHECK(!pthread_join(threads[0], NULL) && !pthread_join(threads[1], NULL));
    update(second, 219);
    uint32_t branch[] = {0x58000050, 0xd61f0200}; /* ldr x16, +8; br x16 */
    memcpy(first.rw, branch, sizeof(branch));
    memcpy(first.rw + 8, &second.rx, sizeof(second.rx));
    sys_icache_invalidate(first.rx, 16);
    CHECK(((int (*)(void))first.rx)() == 219);
    CHECK(space[0] == 17 && space[2 * PAGE] == 29);
    CHECK(!munmap(first.rw, first.size) && !munmap(second.rw, second.size));
    CHECK(!munmap(space, 3 * PAGE) && !munmap(other, PAGE));
    puts("{\"test\":\"code_memory_parallel_updates_and_cross_range_branch\",\"passed\":true}");

    void *limited = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(limited != MAP_FAILED);
    CHECK(vm_protect(mach_task_self(), (uintptr_t)limited, PAGE, TRUE,
                     VM_PROT_READ | VM_PROT_EXECUTE) == KERN_SUCCESS);
    CHECK(!nce_code_prepare_range(limited, PAGE).rw);
    CHECK(!munmap(limited, PAGE));
    puts("{\"test\":\"code_memory_alias_permission_failure\",\"passed\":true}");
    puts("{\"result\":\"passed\"}");
    if (unused)
        dispatch_semaphore_signal((dispatch_semaphore_t)unused);
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
