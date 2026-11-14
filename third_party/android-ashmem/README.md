# Android ashmem

原始文件来自 Android common 的 `android16-6.12-2025-06_r1`，修订为 `2d954fcf3d1b73a41d0fa498324da357ec96cbdf`，该修订的内核版本为 6.12.23。文件位置、下载地址和 SHA-256 由 [sources.json](sources.json) 保存，构建时校验这些原始文件。

`ashmem.c` 使用 GPL-2.0；`ashmem.h` 和 `uapi/ashmem.h` 使用 GPL-2.0 OR Apache-2.0。版权声明和 SPDX 行保持原文。Linux 的 [COPYING](COPYING)、[GPL-2.0 正文](LICENSES/preferred/GPL-2.0)与 [Apache-2.0 正文](LICENSES/dual/Apache-2.0)一并保存。

[lkl_ashmem_patch.py](../../scripts/lkl_ashmem_patch.py)将三个源码文件复制到独立内核树的 `drivers/android/`。生成的 C 文件移除 `linux/page_size_compat.h`，将唯一的 `__PAGE_ALIGN` 改为 `PAGE_ALIGN`；固定 4 KiB 和 16 KiB 的 LKL 构建继续使用各自的实际页大小。原始文件保持不变。

Kconfig 沿用原 `ASHMEM` 的 bool 类型、`SHMEM` 依赖与帮助正文。构建使用固定 C 实现，Makefile 通过 `CONFIG_ASHMEM` 选择 `ashmem.o`。原始 Kconfig、Makefile 和页仿真头保存在 [upstream](upstream/)，用于核对配置与两处适配的来源。生成文件、内核配置、实际 `ashmem.o` 编译命令和依赖文件进入构建清单。
