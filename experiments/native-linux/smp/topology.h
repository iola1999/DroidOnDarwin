/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_LKL_TOPOLOGY_H
#define _ASM_LKL_TOPOLOGY_H

#include <linux/cpumask.h>

/* Both virtual CPUs share one Darwin guest-address window. Core scheduling
 * coordinates that resource; this does not describe the host CPU topology. */
#define topology_sibling_cpumask(cpu) ((void)(cpu), cpu_possible_mask)
#define topology_core_cpumask(cpu) ((void)(cpu), cpu_possible_mask)

#include <asm-generic/topology.h>
#endif
