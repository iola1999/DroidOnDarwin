/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NCE_LKL_SECCOMP_H
#define NCE_LKL_SECCOMP_H
#include <linux/audit.h>
#include <asm-generic/seccomp.h>
#define SECCOMP_ARCH_NATIVE AUDIT_ARCH_AARCH64
#define SECCOMP_ARCH_NATIVE_NR __NR_syscalls
#define SECCOMP_ARCH_NATIVE_NAME "aarch64"
#endif
