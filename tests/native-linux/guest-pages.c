/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../../experiments/native-linux/translation/linux-execution.c"
#include <signal.h>
#include <mach/mach_vm.h>

static void rejected(int signal_number)
{
    (void)signal_number;
    _Exit(86);
}

static vm_prot_t protection_at(void *pointer)
{
    mach_vm_address_t address = (uintptr_t)pointer;
    mach_vm_size_t size;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    REQUIRE(mach_vm_region(mach_task_self(), &address, &size, VM_REGION_BASIC_INFO_64,
                           (vm_region_info_t)&info, &count, &object) == KERN_SUCCESS);
    if (object != MACH_PORT_NULL)
        mach_port_deallocate(mach_task_self(), object);
    REQUIRE(address <= (uintptr_t)pointer && (uintptr_t)pointer - address < size);
    return info.protection;
}

static int mapped_aliases(void)
{
    const unsigned long group = NCE_HOST_PAGE_SIZE;
    ram = mmap(NULL, NCE_RAM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(ram != MAP_FAILED);
    reservation_size = NCE_RAM_SIZE + NCE_VMALLOC_SIZE;
    void *reservation = mmap(NULL, reservation_size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(reservation != MAP_FAILED);
    physical_base = reservation_base = (uintptr_t)reservation;
    unsigned char *alias = (void *)(physical_base + NCE_RAM_SIZE + (8UL << 20));
    shmem_map(alias, 0, group, LKL_PROT_READ | LKL_PROT_WRITE);
    shmem_map(alias + group, group, group, LKL_PROT_READ | LKL_PROT_WRITE);
    for (unsigned long i = 0; i < 2 * group; i += PAGE) {
        alias[i] = (unsigned char)(i / PAGE + 7);
        REQUIRE(ram[i] == alias[i]);
        ram[i + PAGE - 1] = (unsigned char)(i / PAGE + 17);
        REQUIRE(alias[i + PAGE - 1] == ram[i + PAGE - 1]);
    }
    shmem_map(alias, 0, group, LKL_PROT_READ);
    REQUIRE(protection_at(alias) == VM_PROT_READ);
    REQUIRE(protection_at(alias + group) == (VM_PROT_READ | VM_PROT_WRITE));
    revoke_pages(alias, group);
    REQUIRE(protection_at(alias) == VM_PROT_NONE);
    REQUIRE(protection_at(alias + group) == (VM_PROT_READ | VM_PROT_WRITE));
    for (unsigned long i = group; i < 2 * group; i += PAGE) {
        REQUIRE(alias[i] == i / PAGE + 7);
        REQUIRE(alias[i + PAGE - 1] == i / PAGE + 17);
    }
    REQUIRE(munmap(reservation, reservation_size) == 0 && munmap(ram, NCE_RAM_SIZE) == 0);
    puts("{\"test\":\"host_alias_shared_permissions_revoke\",\"passed\":true}");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "mapping"))
        return mapped_aliases();
    if (argc != 5)
        return 2;
    signal(SIGABRT, rejected);
    physical_base = reservation_base = 0x1000000000UL;
    reservation_size = NCE_RAM_SIZE + NCE_VMALLOC_SIZE;
    ram = (void *)0x2000000000UL;
    host_alias_request(argv[1], strtoul(argv[2], NULL, 0), strtoul(argv[3], NULL, 0), strtoul(argv[4], NULL, 0));
    return 0;
}
