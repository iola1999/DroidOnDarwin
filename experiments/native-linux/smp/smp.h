/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_LKL_SMP_H
#define _ASM_LKL_SMP_H

#ifndef __ASSEMBLY__
struct task_struct;
struct cpumask;

unsigned int lkl_current_cpu(void);
void lkl_set_current_cpu(unsigned int cpu);
#define raw_smp_processor_id() lkl_current_cpu()

void lkl_smp_prepare_possible(void);
void smp_prepare_cpus(unsigned int max_cpus);
int __cpu_up(unsigned int cpu, struct task_struct *idle);
void smp_cpus_done(unsigned int max_cpus);
void smp_send_stop(void);
void arch_smp_send_reschedule(int cpu);
void arch_send_call_function_single_ipi(int cpu);
void arch_send_call_function_ipi_mask(const struct cpumask *mask);

void lkl_smp_init(void);
int lkl_secondary_start(void *unused);
void lkl_smp_set_current(unsigned int cpu, struct task_struct *task);
void lkl_smp_idle(void);
void lkl_smp_wake(unsigned int cpu);
void lkl_trigger_cpu_irq(unsigned int cpu, int irq);
void lkl_smp_clock_init(unsigned int cpu);
void lkl_smp_join(void);
void lkl_smp_cleanup(void);
long lkl_smp_test(void);
#endif
#endif
