/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <android/log.h>
#include <dlfcn.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>

/* libandroid_runtime calls this exported function through its PLT. Keep the
 * SDK's ARM64 policy, with explicit entries for NCE's three code-map calls. */
bool nce_seccomp_policy(int type) __asm__("_Z19_set_seccomp_filter10FilterType");
bool nce_seccomp_policy(int type)
{
    const char *names[] = {"arm64_app_filter", "arm64_app_zygote_filter", "arm64_system_filter"};
    if (type < 0 || type > 2) abort();
    char size_name[64];
    snprintf(size_name, sizeof(size_name), "%s_size", names[type]);
    const struct sock_filter *policy = dlsym(RTLD_DEFAULT, names[type]);
    const size_t *count = dlsym(RTLD_DEFAULT, size_name);
    if (!policy || !count || *count > 1024) abort();
    struct sock_filter rules[1032] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 243, 0, 2),
        BPF_JUMP(BPF_JMP | BPF_JGT | BPF_K, 245, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    memcpy(rules + 7, policy, *count * sizeof(*policy));
    rules[7 + *count] = (struct sock_filter)BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP);
    struct sock_fprog program = {(unsigned short)(8 + *count), rules};
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) < 0) abort();
    return true;
}

/* app_process starts through exec, so configure its logger in that process. */
__attribute__((constructor)) static void framework_logger(void)
{
    __android_log_set_logger(__android_log_stderr_logger);
}
