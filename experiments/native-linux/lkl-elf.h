/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_LKL_ELF_H
#define _ASM_LKL_ELF_H
#define elf_check_arch(x) ((x)->e_machine == EM_AARCH64)
#define ELF_CLASS ELFCLASS64
#define ELF_DATA ELFDATA2LSB
#define ELF_ARCH EM_AARCH64
#define ELF_EXEC_PAGESIZE PAGE_SIZE
#define ELF_PLATFORM "aarch64"
#define ELF_HWCAP 0L
#define ELF_ET_DYN_BASE nce_user_base
#define elf_gregset_t long
#define elf_fpregset_t double
#endif
