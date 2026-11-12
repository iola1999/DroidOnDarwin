/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/* user_exec has installed the new registers before this successful syscall returns. */
static void stock_init_trace_exec(long number, long result, const unsigned char *path, const Process *process)
{
    if (number != 221 || result || strcmp((const char *)path, "/system/bin/init"))
        return;
    unsigned long cursor = process->regs.sp, argc = 0, word = 0;
    unsigned long phdr = 0, entry = 0, at_base = 0, page_bytes = 0, phnum = 0, phentsize = 0;
    unsigned long phdr_mapping = 0, entry_mapping = 0, pc_mapping = 0;
    long args[6] = {0};
    long pid;
    unsigned seen = 0;
    long read_result = -EFAULT;
    if (cursor < guest_base || cursor > guest_end - sizeof(word) ||
        copy_user(&argc, cursor, sizeof(argc), 0) || argc > 64 ||
        (argc + 2) * sizeof(word) > guest_end - cursor)
        goto report;
    cursor += (argc + 2) * sizeof(word);
    unsigned i;
    for (i = 0; i < 128; ++i) {
        if (cursor > guest_end - sizeof(word) || copy_user(&word, cursor, sizeof(word), 0))
            goto report;
        cursor += sizeof(word);
        if (!word)
            break;
    }
    if (i == 128)
        goto report;
    for (i = 0; i < 64; ++i) {
        unsigned long auxv[2];
        if (cursor > guest_end - sizeof(auxv) || copy_user(auxv, cursor, sizeof(auxv), 0))
            goto report;
        cursor += sizeof(auxv);
        if (!auxv[0])
            break;
        unsigned bit = 0;
        switch (auxv[0]) {
            case 3:
                phdr = auxv[1];
                bit = 1;
                break;
            case 4:
                phentsize = auxv[1];
                bit = 2;
                break;
            case 5:
                phnum = auxv[1];
                bit = 4;
                break;
            case 6:
                page_bytes = auxv[1];
                bit = 8;
                break;
            case 7:
                at_base = auxv[1];
                bit = 16;
                break;
            case 9:
                entry = auxv[1];
                bit = 32;
                break;
        }
        if (seen & bit)
            goto report;
        seen |= bit;
    }
    if (i == 64 || seen != 63 || phdr < guest_base || phdr > guest_end - sizeof(word) ||
        entry < guest_base || entry > guest_end - sizeof(word) ||
        process->regs.pc < guest_base || process->regs.pc > guest_end - sizeof(word) ||
        copy_user(&word, phdr, sizeof(word), 0) || copy_user(&word, entry, sizeof(word), 0) ||
        copy_user(&word, process->regs.pc, sizeof(word), 0))
        goto report;
    phdr_mapping = translate_user(phdr, LKL_PROT_READ);
    entry_mapping = translate_user(entry, LKL_PROT_EXEC);
    pc_mapping = translate_user(process->regs.pc, LKL_PROT_EXEC);
    read_result = 0;
report:
    pid = run_syscall(172, args);
    printf("{\"stock_init_exec_layout\":true,\"read_result\":%ld,\"pid\":%ld,\"sp\":%lu,\"pc\":%lu,\"phdr\":%lu,\"entry\":%lu,\"at_base\":%lu,\"page_bytes\":%lu,\"phnum\":%lu,\"phentsize\":%lu,\"phdr_mapping\":%lu,\"entry_mapping\":%lu,\"pc_mapping\":%lu}\n",
           read_result, pid, process->regs.sp, process->regs.pc, phdr, entry, at_base, page_bytes, phnum, phentsize,
           phdr_mapping, entry_mapping, pc_mapping);
}
