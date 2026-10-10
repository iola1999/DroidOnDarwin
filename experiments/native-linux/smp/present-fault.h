/* SPDX-License-Identifier: GPL-2.0 */
/* Included by nce_mmu.c so the signal path uses its protected view arrays. */
static int restore_present_data(unsigned long address, bool write)
{
    if (mapping_mode < NCE_MM_LAZY || address < USER_BASE ||
        address >= TASK_SIZE || !nce_window_try_lock_guest())
        return 0;
    int restored = 0;
    struct mm_struct *mm = current->mm;
    if (!mm || mm != active_mm)
        goto out;
    pte_t pte = lookup_window_locked(mm, address);
    unsigned long value = view(pte);
    unsigned long index = (address - USER_BASE) / PAGE_SIZE;
    if (!value || (value & _PAGE_EXEC) || !pte_young(pte) ||
        (write && (!(value & _PAGE_RW) || !pte_dirty(pte))))
        goto out;
    /* An installed view requires normal fault classification for alignment,
     * permissions, and other fault causes. */
    if (installed[index])
        goto out;
    unsigned long first = address & PAGE_MASK, count = 1;
    if (fault_prefetch) {
        while (count < 8 && first + count * PAGE_SIZE < TASK_SIZE) {
            pte_t next = lookup_window_locked(mm, first + count * PAGE_SIZE);
            unsigned long next_value = view(next);
            if (next_value != value + count * PAGE_SIZE || !pte_young(next) ||
                ((next_value & _PAGE_RW) && !pte_dirty(next)) || installed[index + count])
                break;
            ++count;
        }
    }
    if (!new_data_view(first, count * PAGE_SIZE, value))
        goto out;
    mmap_pages_for_ptes_window_locked(mm, first, count, pte);
    if (mapping_mode == NCE_MM_LAZY)
        for (unsigned long i = 0; i < count; ++i)
            __set_bit(index + i, mm->context.accessed);
    PROFILE_ADD(fast_faults, 1);
    PROFILE_ADD(fast_restored_pages, count);
    PROFILE_ADD(restored_pages, count);
    restored = 1;
out:
    nce_window_unlock_guest();
    return restored;
}
