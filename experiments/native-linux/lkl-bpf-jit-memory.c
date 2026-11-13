// SPDX-License-Identifier: GPL-2.0-only
#include <linux/bpf.h>
#include <linux/err.h>
#include <linux/filter.h>
#include <linux/string.h>
#include <asm/host_ops.h>
#include <asm/insn.h>

void *bpf_jit_alloc_exec(unsigned long size)
{
    return lkl_ops->bpf_code_alloc ? lkl_ops->bpf_code_alloc(size) : NULL;
}

void bpf_jit_free_exec(void *address)
{
    if (address)
        WARN_ON_ONCE(lkl_ops->bpf_code_free(address));
}

u64 bpf_jit_alloc_exec_limit(void)
{
    return lkl_ops->bpf_code_capacity ? lkl_ops->bpf_code_capacity() : 0;
}

void *bpf_arch_text_copy(void *dst, void *src, size_t len)
{
    void *alias = lkl_ops->bpf_code_write_alias(dst, len);
    if (!alias)
        return ERR_PTR(-EINVAL);
    memcpy(alias, src, len);
    if (lkl_ops->bpf_code_publish(dst, len))
        return ERR_PTR(-EIO);
    return dst;
}

int bpf_arch_text_invalidate(void *dst, size_t len)
{
    __le32 *alias = lkl_ops->bpf_code_write_alias(dst, len);
    if (!alias || len % sizeof(*alias))
        return -EINVAL;
    for (size_t i = 0; i < len / sizeof(*alias); ++i)
        alias[i] = cpu_to_le32(AARCH64_BREAK_FAULT);
    return lkl_ops->bpf_code_publish(dst, len) ? -EIO : 0;
}
