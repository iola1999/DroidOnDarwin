/* SPDX-License-Identifier: GPL-2.0 */
/* Included only by the test object's copy of mm/vmalloc.c. */
#include <asm/host_ops.h>
#include <linux/init.h>

#define CHECK_VM(expression) \
    do { \
        if (!(expression)) { \
            lkl_printf("NCE_VMALLOC_TEST {\"test\":\"kernel_vmalloc\",\"passed\":false,\"line\":%d}\n", __LINE__); \
            return -EINVAL; \
        } \
    } while (0)

static bool nce_test_unmapped(unsigned long address, unsigned long bytes)
{
    for (unsigned long offset = 0; offset < bytes; offset += PAGE_SIZE)
        if (vmalloc_to_page((void *)(address + offset)))
            return false;
    return true;
}

#ifdef NCE_VMALLOC_PERMISSION_TEST
static int nce_test_readonly_pte(pte_t *pte, unsigned long address, void *unused)
{
    (void)unused;
    set_pte_at(&init_mm, address, pte, pte_wrprotect(*pte));
    return 0;
}
#endif

static int __init nce_vmalloc_tests(void)
{
    const unsigned long group = NCE_HOST_PAGE_SIZE;
    struct page *block, *pages[8], *invalid[8];
    unsigned char *first, *second;
    struct vm_struct *area;
    unsigned long address;
    long baseline;

#ifdef NCE_VMALLOC_NOFAIL_TEST
    lkl_printf("NCE_VMALLOC_TEST nofail_requested\n");
    __vmalloc(1, GFP_KERNEL | __GFP_NOFAIL);
    CHECK_VM(false);
#endif
    first = vmalloc(1);
    CHECK_VM(first && !((unsigned long)first & (group - 1)));
    area = find_vm_area(first);
    CHECK_VM(area && area->nr_pages == 4 && area->size == group + PAGE_SIZE);
    for (unsigned i = 0; i < 4; ++i) {
        CHECK_VM(page_to_phys(area->pages[i]) == page_to_phys(area->pages[0]) + i * PAGE_SIZE);
        first[i * PAGE_SIZE] = i + 11;
        CHECK_VM(*(unsigned char *)page_address(area->pages[i]) == i + 11);
    }
    address = (unsigned long)first;
    vfree(first);
    CHECK_VM(nce_test_unmapped(address, group + PAGE_SIZE));
    lkl_printf("NCE_VMALLOC_TEST {\"test\":\"vmalloc_rounding_guard\",\"passed\":true}\n");

    block = alloc_pages(GFP_KERNEL | __GFP_ZERO, 3);
    CHECK_VM(block);
    split_page(block, 3);
    for (unsigned i = 0; i < 8; ++i)
        pages[i] = block + i;
    first = vmap(pages, 8, VM_MAP, PAGE_KERNEL);
    second = vmap(pages, 8, VM_MAP, PAGE_KERNEL);
    CHECK_VM(first && second && first != second);
#ifdef NCE_VMALLOC_PERMISSION_TEST
    lkl_printf("NCE_VMALLOC_TEST mixed_permissions_requested\n");
    apply_to_page_range(&init_mm, (unsigned long)first + PAGE_SIZE, PAGE_SIZE, nce_test_readonly_pte, NULL);
    CHECK_VM(false);
#endif
    for (unsigned i = 0; i < 8; ++i) {
        first[i * PAGE_SIZE] = i + 21;
        CHECK_VM(second[i * PAGE_SIZE] == i + 21);
        second[(i + 1) * PAGE_SIZE - 1] = i + 41;
        CHECK_VM(first[(i + 1) * PAGE_SIZE - 1] == i + 41);
    }
    vunmap_range_noflush((unsigned long)first, (unsigned long)first + group);
    CHECK_VM(nce_test_unmapped((unsigned long)first, group));
    for (unsigned i = 4; i < 8; ++i) {
        CHECK_VM(first[i * PAGE_SIZE] == i + 21);
        CHECK_VM(first[(i + 1) * PAGE_SIZE - 1] == i + 41);
    }
    vunmap(first);
    CHECK_VM(nce_test_unmapped((unsigned long)first, 2 * group + PAGE_SIZE));
    lkl_printf("NCE_VMALLOC_TEST {\"test\":\"vmalloc_shared_alias_neighbor\",\"passed\":true}\n");

    memcpy(invalid, pages, sizeof(pages));
    invalid[1] = pages[2];
    invalid[2] = pages[1];
    CHECK_VM(!vmap(invalid, 8, VM_MAP, PAGE_KERNEL));
    CHECK_VM(!vmap(pages + 1, 4, VM_MAP, PAGE_KERNEL));
    CHECK_VM(!vmap(pages, 3, VM_MAP, PAGE_KERNEL));
    invalid[0] = NULL;
    CHECK_VM(!vmap(invalid, 8, VM_MAP, PAGE_KERNEL));
    area = get_vm_area(2 * group, VM_MAP);
    CHECK_VM(area);
    address = (unsigned long)area->addr;
    CHECK_VM(vmap_pages_range_noflush(address + PAGE_SIZE, address + PAGE_SIZE + group,
                                      PAGE_KERNEL, pages, PAGE_SHIFT) == -EINVAL);
    CHECK_VM(nce_test_unmapped(address, 2 * group + PAGE_SIZE));
    free_vm_area(area);
    lkl_printf("NCE_VMALLOC_TEST {\"test\":\"vmalloc_invalid_groups\",\"passed\":true}\n");

    nce_test_task = current;
    for (int stop = 0; stop <= 1; ++stop) {
        baseline = atomic_long_read(&nr_vmalloc_pages);
        nce_test_alloc_left = stop;
        nce_test_allocated = nce_test_freed = 0;
        CHECK_VM(!vmalloc(2 * group));
        nce_test_alloc_left = -1;
        CHECK_VM(nce_test_allocated == (unsigned)stop * 4 && nce_test_freed == nce_test_allocated);
        CHECK_VM(atomic_long_read(&nr_vmalloc_pages) == baseline);
        CHECK_VM(nce_test_unmapped(nce_test_area, 2 * group + PAGE_SIZE));
        lkl_printf("NCE_VMALLOC_TEST {\"test\":\"vmalloc_allocation_failure\",\"after_groups\":%d,\"allocated\":%u,\"freed\":%u,\"passed\":true}\n",
                   stop, nce_test_allocated, nce_test_freed);
    }
    for (int stop = 1; stop <= 5; stop += 2) {
        baseline = atomic_long_read(&nr_vmalloc_pages);
        nce_test_map_left = stop;
        nce_test_allocated = nce_test_freed = 0;
        CHECK_VM(!vmalloc(2 * group));
        nce_test_map_left = -1;
        CHECK_VM(nce_test_allocated == 8 && nce_test_freed == 8);
        CHECK_VM(atomic_long_read(&nr_vmalloc_pages) == baseline);
        CHECK_VM(nce_test_unmapped(nce_test_area, 2 * group + PAGE_SIZE));
        lkl_printf("NCE_VMALLOC_TEST {\"test\":\"vmalloc_partial_map_failure\",\"after_pages\":%d,\"allocated\":%u,\"freed\":%u,\"passed\":true}\n",
                   stop, nce_test_allocated, nce_test_freed);
    }
    nce_test_task = NULL;
    for (unsigned i = 0; i < 8; ++i) {
        CHECK_VM(second[i * PAGE_SIZE] == i + 21);
        CHECK_VM(second[(i + 1) * PAGE_SIZE - 1] == i + 41);
    }
    vunmap(second);
    for (unsigned i = 0; i < 8; ++i)
        __free_page(pages[i]);
    lkl_printf("NCE_VMALLOC_TEST {\"test\":\"kernel_vmalloc\",\"passed\":true}\n");
    return 0;
}
late_initcall(nce_vmalloc_tests);
