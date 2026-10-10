/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/mm.h>
#include <linux/futex.h>
#include <linux/pgtable.h>
#include <linux/sched/signal.h>
#include <linux/uaccess.h>
#include <asm/nce-smp-uaccess.h>

extern int nce_page_fault(unsigned long address, int access);

struct user_access {
    pte_t *pte;
    spinlock_t *lock;
    unsigned long irq_flags;
    void *alias;
};

/* IRQ exclusion pairs with MMU_GATHER_RCU_TABLE_FREE, including its allocation
 * failure IPI path. The PTE lock keeps COW, unmap and writeback from replacing
 * or write-protecting the page while its RAM alias is accessed. */
static bool begin_access(unsigned long address, bool write, struct user_access *access)
{
    struct mm_struct *mm = current->mm;
    if (!mm)
        return false;
    local_irq_save(access->irq_flags);
    pgd_t *pgd = pgd_offset(mm, address);
    p4d_t *p4d = p4d_offset(pgd, address);
    pud_t *pud = pud_offset(p4d, address);
    if (!pud_present(READ_ONCE(*pud)))
        goto failed;
    pmd_t *pmd = pmd_offset(pud, address);
    access->pte = pte_offset_map_lock(mm, pmd, address, &access->lock);
    if (!access->pte)
        goto failed;
    pte_t value = ptep_get(access->pte);
    if (!pte_present(value) || !pte_read(value) || !pte_young(value) ||
        (write && (!pte_write(value) || !pte_dirty(value))) ||
        (pte_special(value) && !is_zero_pfn(pte_pfn(value))) || !pfn_valid(pte_pfn(value))) {
        pte_unmap_unlock(access->pte, access->lock);
        goto failed;
    }
    access->alias = page_address(pte_page(value)) + offset_in_page(address);
    return true;
failed:
    local_irq_restore(access->irq_flags);
    return false;
}

static void end_access(struct user_access *access)
{
    pte_unmap_unlock(access->pte, access->lock);
    local_irq_restore(access->irq_flags);
}

unsigned long nce_copy_user_smp(void *kernel, unsigned long user,
                                unsigned long size, int operation)
{
    unsigned long remaining = size;
    while (remaining) {
        struct user_access access;
        if (!begin_access(user, operation != 0, &access)) {
            if (pagefault_disabled() || in_atomic() || fatal_signal_pending(current) ||
                nce_page_fault(user, operation != 0))
                break;
            cond_resched();
            continue;
        }
        unsigned long part = min(remaining, PAGE_SIZE - offset_in_page(user));
        if (operation == 2)
            memset(access.alias, 0, part);
        else if (operation)
            memcpy(access.alias, kernel, part);
        else
            memcpy(kernel, access.alias, part);
        end_access(&access);
        user += part;
        if (kernel)
            kernel += part;
        remaining -= part;
    }
    return remaining;
}

int nce_futex_cmpxchg(u32 *value, u32 __user *address, u32 old, u32 next)
{
    struct user_access access;
    if (!nce_access_ok(address, sizeof(*address)) ||
        !begin_access((unsigned long)address, true, &access))
        return -EFAULT;
    __atomic_compare_exchange_n((u32 *)access.alias, &old, next, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    *value = old;
    end_access(&access);
    return 0;
}

int nce_futex_operation(int operation, u32 argument, int *old, u32 __user *address)
{
    struct user_access access;
    int error = 0;
    if (!nce_access_ok(address, sizeof(*address)) ||
        !begin_access((unsigned long)address, true, &access))
        return -EFAULT;
    u32 *word = access.alias;
    switch (operation) {
        case FUTEX_OP_SET:
            *old = __atomic_exchange_n(word, argument, __ATOMIC_SEQ_CST);
            break;
        case FUTEX_OP_ADD:
            *old = __atomic_fetch_add(word, argument, __ATOMIC_SEQ_CST);
            break;
        case FUTEX_OP_OR:
            *old = __atomic_fetch_or(word, argument, __ATOMIC_SEQ_CST);
            break;
        case FUTEX_OP_ANDN:
            *old = __atomic_fetch_and(word, ~argument, __ATOMIC_SEQ_CST);
            break;
        case FUTEX_OP_XOR:
            *old = __atomic_fetch_xor(word, argument, __ATOMIC_SEQ_CST);
            break;
        default:
            error = -ENOSYS;
    }
    end_access(&access);
    return error;
}
