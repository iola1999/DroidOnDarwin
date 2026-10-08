/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/syscalls.h>
#include <asm/host_ops.h>

static int user_start(void *state)
{
    current->thread.nce_state = state;
    lkl_ops->user_switch(state);
    lkl_ops->user_run(state);
    return 0;
}

long nce_launch(long *params)
{
    struct kernel_clone_args args = {
        .exit_signal = SIGCHLD,
        .fn = user_start,
        .fn_arg = (void *)params[0],
    };
    return kernel_clone(&args);
}

void start_thread(struct pt_regs *regs, unsigned long entry, unsigned long stack)
{
    struct page *pages[65536 / PAGE_SIZE];
    long count = get_user_pages(entry, ARRAY_SIZE(pages), FOLL_WRITE | FOLL_FORCE, pages);
    BUG_ON(count != ARRAY_SIZE(pages));
    for (long i = 0; i < count; ++i) put_page(pages[i]);
    /* Rewriting is confined to private executable pages of the fixture. */
    lkl_ops->user_exec(current->thread.nce_state, entry, stack);
}
