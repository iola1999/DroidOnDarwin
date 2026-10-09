/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_LKL_ELF_H
#define _ASM_LKL_ELF_H
#ifdef CONFIG_MMU
#include <asm/host_ops.h>
#include <asm/nce-layout.h>
#define NCE_ELF_BRK (nce_user_base + NCE_BRK_OFFSET)
#define ARCH_HAS_SETUP_ADDITIONAL_PAGES 1
struct linux_binprm;
int arch_setup_additional_pages(struct linux_binprm *, int);
#define NCE_ELF_LAYOUT(file, header, phdrs) nce_elf_file_layout(file, 0, header, phdrs)
struct file;
struct elf64_hdr;
struct elf64_phdr;
unsigned long nce_elf_file_layout(struct file *, unsigned long, const struct elf64_hdr *, const struct elf64_phdr *);
#define NCE_ELF_INTERP_LAYOUT(file, header, phdrs) nce_elf_file_layout(file, 0, header, phdrs)
#endif
#define elf_check_arch(x) ((x)->e_machine == EM_AARCH64)
#define ELF_CLASS ELFCLASS64
#define ELF_DATA ELFDATA2LSB
#define ELF_ARCH EM_AARCH64
#define ELF_EXEC_PAGESIZE PAGE_SIZE
#define ELF_PLATFORM "aarch64"
#ifdef CONFIG_MMU
extern unsigned long nce_elf_hwcap;
#define ELF_HWCAP nce_elf_hwcap
#else
#define ELF_HWCAP 0L
#endif
#define ELF_ET_DYN_BASE nce_user_base
#define elf_gregset_t long
#define elf_fpregset_t double
#endif
