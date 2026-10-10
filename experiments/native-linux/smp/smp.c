/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/cpu.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/smp.h>
#include <linux/tick.h>
#include <asm/cpu.h>
#include <asm/host_ops.h>

static struct lkl_tls_key *cpu_key;
static struct task_struct *secondary_idle;
static struct lkl_sem *idle_event;
static int ipi_irq[NR_CPUS];

enum { IPI_RESCHEDULE = 1,
       IPI_CALL = 2,
       IPI_STOP = 4 };
static struct {
    struct lkl_sem *event;
    lkl_thread_t thread;
    unsigned int pending;
} notifications[NR_CPUS];

unsigned int lkl_current_cpu(void)
{
    return cpu_key ? (unsigned long)lkl_ops->tls_get(cpu_key) : 0;
}

void lkl_set_current_cpu(unsigned int cpu)
{
    if (lkl_ops->tls_set(cpu_key, (void *)(unsigned long)cpu))
        lkl_bug("cannot set LKL CPU identity\n");
}

void lkl_smp_init(void)
{
    cpu_key = lkl_ops->tls_alloc(NULL);
    idle_event = lkl_ops->sem_alloc(0);
    if (!cpu_key || !idle_event)
        lkl_bug("cannot allocate LKL SMP host state\n");
}

void lkl_smp_wake(unsigned int cpu)
{
    if (cpu)
        lkl_ops->sem_up(idle_event);
}

void lkl_smp_idle(void)
{
    /* CPU 1 has no host syscall task. Release ownership while its idle
     * pthread sleeps so timer/notification threads can deliver interrupts. */
    lkl_cpu_put();
    lkl_ops->sem_down(idle_event);
    lkl_cpu_get();
}

void lkl_trigger_cpu_irq(unsigned int cpu, int irq)
{
    unsigned int previous = lkl_current_cpu();
    lkl_set_current_cpu(cpu);
    lkl_trigger_irq(irq);
    lkl_smp_wake(cpu);
    lkl_set_current_cpu(previous);
}

static irqreturn_t receive_ipi(int irq, void *unused)
{
    unsigned int cpu = raw_smp_processor_id();
    unsigned int pending = __atomic_fetch_and(&notifications[cpu].pending, IPI_STOP, __ATOMIC_ACQUIRE);
    if (pending & IPI_RESCHEDULE)
        scheduler_ipi();
    if (pending & IPI_CALL)
        generic_smp_call_function_interrupt();
    return IRQ_HANDLED;
}

static void notification_thread(void *arg)
{
    unsigned int cpu = (unsigned long)arg;
    lkl_set_current_cpu(cpu);
    for (;;) {
        lkl_ops->sem_down(notifications[cpu].event);
        if (__atomic_load_n(&notifications[cpu].pending, __ATOMIC_ACQUIRE) & IPI_STOP)
            break;
        lkl_trigger_cpu_irq(cpu, ipi_irq[cpu]);
    }
}

static void send_ipi(unsigned int cpu, unsigned int kind)
{
    __atomic_fetch_or(&notifications[cpu].pending, kind, __ATOMIC_RELEASE);
    lkl_ops->sem_up(notifications[cpu].event);
}

void arch_smp_send_reschedule(int cpu)
{
    send_ipi(cpu, IPI_RESCHEDULE);
}

void arch_send_call_function_single_ipi(int cpu)
{
    send_ipi(cpu, IPI_CALL);
}

void arch_send_call_function_ipi_mask(const struct cpumask *mask)
{
    unsigned int cpu;
    for_each_cpu(cpu, mask)
        arch_send_call_function_single_ipi(cpu);
}

void __init lkl_smp_prepare_possible(void)
{
    unsigned int cpu;
    for (cpu = 0; cpu < NR_CPUS; ++cpu) {
        set_cpu_possible(cpu, true);
        set_cpu_present(cpu, true);
    }
}

void __init smp_prepare_cpus(unsigned int max_cpus)
{
    unsigned int cpu;
    for (cpu = 0; cpu < NR_CPUS; ++cpu) {
        ipi_irq[cpu] = lkl_get_free_irq("smp-notification");
        if (ipi_irq[cpu] < 0 || request_irq(ipi_irq[cpu], receive_ipi, IRQF_NO_THREAD, "smp-notification", NULL))
            panic("cannot initialize LKL IPI");
        notifications[cpu].event = lkl_ops->sem_alloc(0);
        if (notifications[cpu].event)
            notifications[cpu].thread = lkl_ops->thread_create(notification_thread, (void *)(unsigned long)cpu);
        if (!notifications[cpu].event || !notifications[cpu].thread)
            panic("cannot start LKL notification thread");
    }
}

int __cpu_up(unsigned int cpu, struct task_struct *idle)
{
    if (cpu != 1)
        return -EINVAL;
    secondary_idle = idle;
    lkl_smp_set_current(cpu, idle);
    lkl_ops->sem_up(task_thread_info(idle)->sched_sem);
    while (!cpu_online(cpu))
        cpu_relax();
    return 0;
}

int lkl_secondary_start(void *unused)
{
    unsigned int cpu = raw_smp_processor_id();
    lkl_cpu_get();
    mmgrab(&init_mm);
    current->active_mm = &init_mm;
    notify_cpu_starting(cpu);
    lkl_smp_clock_init(cpu);
    set_cpu_online(cpu, true);
    local_irq_enable();
    cpu_startup_entry(CPUHP_AP_ONLINE_IDLE);
    return 0;
}

void smp_cpus_done(unsigned int max_cpus)
{
}

void smp_send_stop(void)
{
    /* machine_halt performs the ownership handoff and joins both CPUs. */
}

void lkl_smp_join(void)
{
    unsigned int cpu;
    for (cpu = 0; cpu < NR_CPUS; ++cpu)
        send_ipi(cpu, IPI_STOP);
    for (cpu = 0; cpu < NR_CPUS; ++cpu) {
        lkl_ops->thread_join(notifications[cpu].thread);
        lkl_ops->sem_free(notifications[cpu].event);
    }
    if (secondary_idle)
        lkl_ops->thread_join(task_thread_info(secondary_idle)->tid);
}

void lkl_smp_cleanup(void)
{
    lkl_ops->sem_free(idle_event);
    lkl_ops->tls_free(cpu_key);
    cpu_key = NULL;
}
