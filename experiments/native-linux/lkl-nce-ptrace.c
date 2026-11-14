/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/build_bug.h>
#include <linux/elf.h>
#include <linux/ptrace.h>
#include <linux/regset.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/uio.h>
#include <asm/host_ops.h>

struct user_pt_regs {
    __u64 regs[31], sp, pc, pstate;
};

static_assert(sizeof(struct user_pt_regs) == 272);
static_assert(sizeof(struct pt_regs) == 800);

static int gpr_get(struct task_struct *target, const struct user_regset *regset,
                   struct membuf to)
{
    const struct pt_regs *regs = task_pt_regs(target);
    struct user_pt_regs user;
    if (!regs)
        return -EIO;
    for (unsigned int index = 0; index < ARRAY_SIZE(user.regs); ++index)
        user.regs[index] = regs->x[index];
    user.sp = regs->sp;
    user.pc = regs->pc;
    user.pstate = regs->nzcv;
    return membuf_write(&to, &user, sizeof(user));
}

static int tls_get(struct task_struct *target, const struct user_regset *regset,
                   struct membuf to)
{
    void *state = target->thread.nce_state;
    __u64 tls[2];
    if (!state || !lkl_ops->user_get_tls)
        return -EIO;
    tls[0] = lkl_ops->user_get_tls(state);
    tls[1] = 0;
    return membuf_write(&to, tls, sizeof(tls));
}

static const struct user_regset regsets[] = {
    {.core_note_type = NT_PRSTATUS,
     .n = sizeof(struct user_pt_regs) / sizeof(__u64),
     .size = sizeof(__u64),
     .align = sizeof(__u64),
     .regset_get = gpr_get},
    {.core_note_type = NT_ARM_TLS,
     .n = 2,
     .size = sizeof(__u64),
     .align = sizeof(__u64),
     .regset_get = tls_get},
};

static const struct user_regset_view view = {
    .name = "aarch64",
    .e_machine = EM_AARCH64,
    .regsets = regsets,
    .n = ARRAY_SIZE(regsets),
};

long nce_ptrace(struct task_struct *child, long request, unsigned long addr,
                unsigned long data)
{
    struct iovec __user *user = (void __user *)data;
    struct iovec vector;
    const struct user_regset *regset;
    unsigned int index;
    int result;

    if (request != PTRACE_GETREGSET || (addr != NT_PRSTATUS && addr != NT_ARM_TLS))
        return ptrace_request(child, request, addr, data);
    index = addr == NT_PRSTATUS ? 0 : 1;
    regset = &regsets[index];
    if (copy_from_user(&vector, user, sizeof(vector)))
        return -EFAULT;
    if (vector.iov_len % regset->size)
        return -EINVAL;
    vector.iov_len = min_t(__kernel_size_t, vector.iov_len, regset->n * regset->size);
    result = copy_regset_to_user(child, &view, index, 0, vector.iov_len, vector.iov_base);
    if (result)
        return result;
    return put_user(vector.iov_len, &user->iov_len);
}
