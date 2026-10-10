/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/mm.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <asm/sigcontext.h>
#include <asm/ucontext.h>
#include <asm/cpu.h>
#include <asm/mmu_context.h>
void wakeup_idle_host_task(void);

struct nce_sigframe {
    siginfo_t info;
    struct ucontext uc;
};

static int setup_frame(struct ksignal *signal, struct pt_regs *regs)
{
    unsigned long top = sigsp(regs->sp, signal);
    unsigned long address = (top - sizeof(struct nce_sigframe)) & ~15UL;
    struct nce_sigframe __user *user = (void *)address;
    struct nce_sigframe *frame;
    struct fpsimd_context *fp;
    int error;
    if (top < sizeof(*frame) || !access_ok(user, sizeof(*frame)))
        return -EFAULT;
    if (on_sig_stack(top) && !on_sig_stack(address))
        return -EFAULT;
    unsigned long restorer = signal->ka.sa.sa_flags & SA_RESTORER ? (unsigned long)signal->ka.sa.sa_restorer : current->mm->context.sigreturn;
    if (!restorer)
        return -EINVAL;
    /* Keep the 4 KiB extension area off the 16 KiB Linux kernel stack. */
    frame = kzalloc(sizeof(*frame), GFP_KERNEL);
    if (!frame)
        return -ENOMEM;
    frame->uc.uc_sigmask = *sigmask_to_save();
    memcpy(frame->uc.uc_mcontext.regs, regs->x, sizeof(regs->x));
    frame->uc.uc_mcontext.sp = regs->sp;
    frame->uc.uc_mcontext.pc = regs->pc;
    frame->uc.uc_mcontext.pstate = regs->nzcv;
    if (signal->sig == SIGSEGV || signal->sig == SIGBUS || signal->sig == SIGILL)
        frame->uc.uc_mcontext.fault_address = (unsigned long)signal->info.si_addr;
    fp = (void *)frame->uc.uc_mcontext.__reserved;
    fp->head.magic = FPSIMD_MAGIC;
    fp->head.size = sizeof(*fp);
    fp->fpsr = regs->fpsr;
    fp->fpcr = regs->fpcr;
    memcpy(fp->vregs, regs->vector, sizeof(regs->vector));
    error = copy_to_user(user, frame, sizeof(*frame)) ? -EFAULT : 0;
    kfree(frame);
    if (!error)
        error = __save_altstack(&user->uc.uc_stack, regs->sp);
    if (!error)
        error = copy_siginfo_to_user(&user->info, &signal->info);
    if (error)
        return error;
    regs->x[0] = signal->sig;
    regs->x[1] = (unsigned long)&user->info;
    regs->x[2] = (unsigned long)&user->uc;
    regs->x[30] = restorer;
    regs->sp = address;
    regs->pc = (unsigned long)signal->ka.sa.sa_handler;
    return 0;
}

SYSCALL_DEFINE0(rt_sigreturn)
{
    struct pt_regs *regs = task_pt_regs(current);
    struct nce_sigframe __user *user;
    struct nce_sigframe *frame;
    struct fpsimd_context *fp;
    struct _aarch64_ctx *end;
    if (!regs)
        return -ENOSYS;
    current->restart_block.fn = do_no_restart_syscall;
    user = (void *)regs->sp;
    if (regs->sp & 15)
        goto bad;
    frame = kmalloc(sizeof(*frame), GFP_KERNEL);
    if (!frame)
        goto bad;
    if (copy_from_user(frame, user, sizeof(*frame)))
        goto free_bad;
    fp = (void *)frame->uc.uc_mcontext.__reserved;
    end = (void *)(fp + 1);
    /* The signal frame carries FPSIMD only. Reject unsupported records
     * before changing registers, so host-only execution fields stay private. */
    if (fp->head.magic != FPSIMD_MAGIC || fp->head.size != sizeof(*fp) ||
        end->magic || end->size || (frame->uc.uc_mcontext.sp & 15) ||
        (frame->uc.uc_mcontext.pc & 3) || !access_ok((void *)frame->uc.uc_mcontext.pc, 4) ||
        (frame->uc.uc_mcontext.pstate & ~0xf0000000UL))
        goto free_bad;
    if (restore_altstack(&user->uc.uc_stack))
        goto free_bad;
    set_current_blocked(&frame->uc.uc_sigmask);
    memcpy(regs->x, frame->uc.uc_mcontext.regs, sizeof(regs->x));
    regs->sp = frame->uc.uc_mcontext.sp;
    regs->pc = frame->uc.uc_mcontext.pc;
    regs->nzcv = frame->uc.uc_mcontext.pstate;
    regs->fpcr = fp->fpcr;
    regs->fpsr = fp->fpsr;
    memcpy(regs->vector, fp->vregs, sizeof(regs->vector));
    kfree(frame);
    return regs->x[0];
free_bad:
    kfree(frame);
bad:
    force_sig(SIGSEGV);
    return 0;
}

void nce_user_return(long syscall, long result, unsigned long original_x0)
{
    struct pt_regs *regs = task_pt_regs(current);
    struct ksignal signal;
    unsigned long continuation;
    bool restart;
    if (!regs)
        return;
    if ((syscall == __NR_tgkill || syscall == __NR_rt_tgsigqueueinfo) && regs->x[2] == SIGABRT) {
        unsigned long fp = regs->x[29];
        pr_err("NCE guest abort pid=%d comm=%s pc=%lx lr=%lx sp=%lx\n",
               current->pid, current->comm, regs->pc, regs->x[30], regs->sp);
        for (unsigned int depth = 0; depth < 24 && fp; ++depth) {
            unsigned long frame[2];
            if ((fp & 15) || copy_from_user(frame, (void __user *)fp, sizeof(frame)))
                break;
            pr_err("NCE abort frame[%u] fp=%lx lr=%lx\n", depth, fp, frame[1]);
            if (frame[0] <= fp || frame[0] - fp > 1024 * 1024)
                break;
            fp = frame[0];
        }
    }
    continuation = regs->pc;
    restart = syscall >= 0 && syscall != __NR_rt_sigreturn && (result == -ERESTARTSYS || result == -ERESTARTNOHAND || result == -ERESTARTNOINTR || result == -ERESTART_RESTARTBLOCK);
    if (syscall >= 0 && syscall != __NR_rt_sigreturn)
        regs->x[0] = result;
    if (restart) {
        regs->x[0] = original_x0;
        regs->pc -= 4;
    }
    while (get_signal(&signal)) {
        int error;
        if (restart && (result == -ERESTARTNOHAND || result == -ERESTART_RESTARTBLOCK ||
                        (result == -ERESTARTSYS && !(signal.ka.sa.sa_flags & SA_RESTART)))) {
            regs->pc = continuation;
            regs->x[0] = -EINTR;
        }
        error = setup_frame(&signal, regs);
        signal_setup_done(error, &signal, 0);
        if (!error) {
            nce_prepare_user();
            return;
        }
        restart = false;
    }
    if (restart && result == -ERESTART_RESTARTBLOCK)
        regs->x[8] = __NR_restart_syscall;
    restore_saved_sigmask();
    nce_prepare_user();
}

void nce_user_fault(unsigned long address, int kind)
{
    if (kind == 2)
        force_sig_fault(SIGTRAP, TRAP_BRKPT, (void __user *)address);
    else if (kind == 1)
        force_sig_fault(SIGILL, ILL_ILLOPC, (void __user *)address);
    else {
        mmap_read_lock(current->mm);
        struct vm_area_struct *vma = find_vma(current->mm, address);
        int code = vma && address >= vma->vm_start ? SEGV_ACCERR : SEGV_MAPERR;
        mmap_read_unlock(current->mm);
        force_sig_fault(SIGSEGV, code, (void __user *)address);
    }
    nce_user_return(-1, 0, 0);
}

/* Host timer delivery yields the current Linux task without issuing a guest
 * syscall. The saved x8 still belongs to the interrupted guest instruction. */
void nce_user_preempt(void)
{
    if (lkl_cpu_has_waiters()) {
        wakeup_idle_host_task();
        sys_sched_yield();
    } else {
        /* Drain pending LKL timer IRQs and honor the scheduler's time slice.
         * The host polling interval alone does not require a task switch. */
        cond_resched();
    }
    nce_user_return(-1, 0, 0);
}
