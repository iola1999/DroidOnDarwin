# 决策记录: 原始 linker 与 Bionic 组件验证

状态: implemented

## 问题

当前 Android 基线重建了 linker、libc 和 libdl，并修改 ELF 地址预留、CFI、Scudo、SCS 与地址标签行为。现有运行结果无法确认 SDK 或 HyperOS 原始 runtime 是否能够直接加载。本轮使用相同测试分别检查 SDK36 r07 的原始 16 KiB runtime 与已恢复 HyperOS runtime，保留 Linux 16 KiB 页、低客体布局和原有 VM 限制。源码在基于 `1d1042bb555fa45a65447c4bbbda591215d1e445` 的 `work/offline-stock-bionic` 独立工作树实现。

## 决策

### 输入与运行入口

输入限制为 `linker64`、`libc.so`、`libdl.so`、`libm.so` 四个文件，保留原始字节。[输入与结果检查](../../../../scripts/stock_bionic.py)复核固定清单摘要及四文件摘要，把文件和原始清单复制到独立输出目录，并在运行结束后再次核对原输入。报告记录原目录、清单路径、发布身份及各文件摘要。

| 输入 | 原始目录（相对于主工作区） | 来源清单 SHA-256 |
| --- | --- | --- |
| SDK36 r07 | `work/native-linux-nce/android/android36/com.android.runtime` | `4b1a45b0ee51725c68f1c29765958d0b60b45a5f2325bedfc61764cd49232249` |
| HyperOS | `work/project-audit-2026-11-12/references/hyperos3-input/core-scan-v4/files/system/apex/com.android.runtime.apex.contents/files` | `c9ec384e1eee4c754152fddaae8d351ff5f1adaafb86e5cdc88fb02f5f712d0a` |

SDK 清单为 `android36-runtime.json`，记录 Google SDK `arm64-v8a-ps16k-36_r07.zip`，归档 SHA-1 为 `dda632745023567113d9954101709c6526a3c250`。HyperOS 清单为[输入扫描结果](../../../../experiments/native-linux/results-hyperos-input-20261112.json)，来源是 `NEORUAA/HyperOS-AVD` 提交 `9d7ddd3f046e89d33cb205b5fbbf6dc5b728155c` 的 `v0.1.0-a16-hyperos3-fuxi-r1` 第三方 MysticGSI 移植发布，原始发布清单 SHA-256 为 `7785f61d0e1846fa14d9f3af9db49c905a02604128747a0d617eeff381504ceb`。这次组件检查沿用扫描所得文件，未执行 APEX 激活、签名或系统盘验证。

新增 `bionic` 工作负载，复用 LKL/Direct 内核、CLI/GCD 和进程运行入口。宿主向临时根文件系统写入原始四文件、测试程序和最小库搜索配置，创建 `/dev/null`、`/dev/random`、`/dev/urandom`，并调用与现有 Android 入口相同的随机数初始化。原始 linker 的 SONAME 为 `ld-android.so`，自身没有 `DT_NEEDED`；libc/libdl 依赖其加载接口。组件测试使用 `/apex/com.android.runtime/lib64/bionic` 搜索路径，不装载 ART、系统镜像或数据盘。

[静态 musl 父程序](../../../../experiments/native-linux/translation/stock-bionic-supervisor.c)依次 fork/exec 原始 linker 无参数帮助入口及 [NDK 动态 ELF](../../../../experiments/native-linux/translation/stock-bionic-guest.c)，分别记录 exec errno、退出码、信号与阶段。linker 帮助入口失败后，动态阶段记录 `started:false`。动态程序进入 `main` 后报告实际页大小与加载对象，再检查 BSS、64 KiB 分配及内容、单线程 TLS、libm 动态调用。BSS 和分配内容通过 volatile 读访问，保留实际访存检查。

运行器仅在 `bionic` 工作负载记录客体 `mmap`、`mprotect`、`mremap` 的参数、PC、PID 和返回值；原有执行及内存权限规则继续生效。报告中的 `validated` 检查执行过程、配置、阶段顺序和资源清理，`component_compatible` 另行要求动态程序进入 `main` 并完成全部检查。非零退出和信号作为组件结果保存；超时、父程序异常、记录缺失/重复、错误顺序或输入变化使执行验证失败。

### 本机结果

环境为 Apple M5 Ultra，macOS 27.0，系统构建 `26A428`。两组均使用 Linux 16 KiB 页、客体 `[0x10000, 0x100000000)`、1 GiB 客体 RAM、单 CPU、PTE、Direct flat、128 MiB 代码缓存、双映射代码内存、2 ms timer 抢占、按页执行缓存及 TLB 失效。`code_watch`、`cross_page`、`native_exclusive` 开启；`memory_groups`、`route_epochs`、`retain_routes`、`vector_table`、`simd_math` 关闭。各入口执行一次，允许并行离线构建，本轮不作性能比较。报告中的 ART 开关沿用通用脚本配置，本工作负载未启动 ART。

| 原始组件 | 入口 | linker 帮助入口 | 动态程序 | 资源清理 |
| --- | --- | --- | --- | --- |
| SDK36 r07 | CLI | exit 0，输出 Usage | exec errno 0，main 前 SIGABRT（6） | allocated/freed 5/5 |
| SDK36 r07 | GCD | exit 0，输出 Usage | exec errno 0，main 前 SIGABRT（6） | allocated/freed 5/5 |
| HyperOS | CLI | exec errno 0，SIGSEGV（11），无 Usage | 未执行 | allocated/freed 2/2 |
| HyperOS | GCD | exec errno 0，SIGSEGV（11），无 Usage | 未执行 | allocated/freed 2/2 |

四次运行的 `validated` 均为 true，`component_compatible` 均为 false。SDK 两个入口均捕获原 libc 的 `mmap(0, 8858370048, 0, 16418, -1, 0)` 返回 `-12`（`ENOMEM`），随后输出 `Scudo ERROR: internal map failure (error desc=Out of memory) requesting 8650752KB` 并终止。请求大小为 8.25 GiB，超过当前不足 4 GiB 的客体用户范围。这次运行确认了所测 SDK 原始二进制的实际预留要求；该请求使用 `PROT_NONE`，本结果未测量实际提交 8.25 GiB RAM 的行为。

HyperOS 两个入口均记录 `guest fault pc=250610 address=250610 access=2 result=-14`。`access=2` 为取指访问；linker 装载基址是 `0xc0000`，故障对应原文件虚拟地址 `0x190610`，位于 `.iplt`。原 linker 的 RX LOAD 为 `[0x4c000, 0x190660)`，后续 RW LOAD 从 `0x191000` 开始，两者按 16 KiB 页取整共用 `[0x190000, 0x194000)`。故障位置与权限共页一致，尚未通过 4 KiB 客体或加载器权限对照确认原因。此次失败发生在 libc 动态程序检查之前，HyperOS libc 的 Scudo、TLS 和 libm 行为仍待验证。

最初 SDK 检查遗漏随机数设备，两个入口在 linker 阶段因 `getentropy failed: No such file or directory` 收到 SIGABRT。补齐已有 Android 入口使用的设备与随机数初始化后，帮助入口成功，动态阶段出现上述 Scudo 失败。首轮保存在 `work/native-linux-nce/stock-bionic-sdk36/summary.json`（63,433 字节，SHA-256 `e7fd67d1060154b5ea47722cb9ca501291d7eb95ec0b16671ae5934fc96eed5e`）；该结果用于记录测试环境修正，不用于判断原 runtime 的地址兼容性。

### 归档与复现

归档保留输入、完整命令、源码/生成物摘要、各入口全部 `component_*`、配置、`runtime_elf`、测试/清理记录和 stderr 原文。内存样本、内存统计与执行计数保留在工作目录的完整 summary，归档通过 `archive` 字段记录原报告路径、大小、SHA-256 及筛选范围。

| 报告 | 字节数 | SHA-256 |
| --- | ---: | --- |
| [SDK 组件报告](../../../../experiments/native-linux/results-stock-bionic-sdk36-20261112.json) | 116,972 | `58e141fce8a5fb35a048eb5543d5a3f92a63b38ed65d1036f737ee91397eeb23` |
| [HyperOS 组件报告](../../../../experiments/native-linux/results-stock-bionic-hyperos-20261112.json) | 46,729 | `61cc8fc1ac068d8bb79bca756ae63d7c42a50837df5707ac613818b611d38513` |

两份报告各记录 118 项输入/生成物摘要；共享路径对应的摘要全部一致。内核 `lkl-image.bin` SHA-256 为 `2e7be6160538430541a8434b89956bfbe6e84db562056953c47cb28b290475e7`。父程序 ELF 为 `a44487175962f85b97be7a59e4a9372cad651dae1af99fe673505850211cda3e`，NDK 动态 ELF 为 `45bb5e3ce5cae39c3a8940e10e929de9517d7fa9b4a53732cc9a201cd4e8ca44`，两组完全相同。NDK 版本固定为 `29.0.14206865`，动态 ELF 使用 Android API 35 和 16 KiB 链接页配置。

依赖通过最小 APFS clone 准备，只复制 LKL 源码与配置、musl、小型既有 ELF、宿主头文件、compiler-rt、Oaknut 和校验过的 Unicorn 输入。新工作树独立构建 translated 内核；Unicorn 清单记录复用来源及新工作树 prefix，保持原库与源码摘要。未复制完整 Android 依赖。准备清单位于 `work/native-linux-nce/dependency-reuse.json`。

在已有这些依赖的工作树执行：

```sh
python3 scripts/build_lkl_translation_kernel.py
python3 scripts/probe_lkl_translation.py \
  --guest-layout low --mapping pte --execution direct --direct-memory flat \
  --direct-code-watch 1 --direct-cross-page 1 --direct-native-exclusive 1 \
  --cache-mib 128 --code-memory dual --preemption timer --exec-cache pages \
  --tlb-invalidation pages --cpus 1 --reuse-engine-build --workload bionic --entry both \
  --runtime-source sdk36 --runtime-directory <SDK原始com.android.runtime目录> \
  --runtime-manifest <android36-runtime.json> \
  --output work/native-linux-nce/stock-bionic-sdk36-final
```

HyperOS 使用相同参数，替换 `--runtime-source hyperos`、原始目录、扫描清单路径和输出目录。输出目录应另建，保留历史结果。原 JSONL/log 分别位于 `stock-bionic-sdk36-final` 与 `stock-bionic-hyperos-final`。这些文件在工作树清理后可由固定输入、报告所列依赖与命令重新生成。

`python3 scripts/test_stock_bionic.py` 的 7 项检查成功，覆盖固定清单与文件变化、缺失/重复记录、全部两两交换顺序、exec errno、main 前后信号、退出状态矛盾和已加载对象格式。`python3 scripts/test_translation_guest_layout.py` 的有效 host/low 与 131 项负例成功。定向格式检查、Python 编译和 `git diff --check` 成功。主会话只读复核了输入校验、两个 C 程序与 runner/probe 分支，未发现阻止本轮归档的问题。

### 复杂度审查

按 reclaim-code-entropy 检查实际使用方和状态。新增组件分支由 `--workload bionic` 调用；输入校验与结果校验放在同一小模块，父程序负责进程退出状态，动态程序负责 libc 行为。单独维护四文件清单是固定输入校验所需。运行器继续使用已有文件复制、进程创建、执行与清理过程；从 Android 初始化中提取随机数初始化供两个实际入口调用。没有新增通用工作负载框架或另一套内核运行器。

## 曾考虑的替代方案

- 先修改原始 ELF、重建 Bionic 或切换 4 KiB Linux。会同时改变输入和执行环境，本轮保留现状，后续按实际失败选择对照。
- 使用完整 Android 镜像运行现有 `bionic-test`。该测试包含 128 线程、48 MiB Scudo 压力及多项系统调用检查；本轮用较小程序区分 linker 入口、libc 初始化和基本调用。
- 单独复制一套 LKL/Direct 运行器。现有工作负载可以增加明确的组件分支，保留同一初始化和清理过程，减少重复代码。

## 影响

低 4 GiB 的完整用户范围无法满足所测 SDK 原始 Scudo 的首次 8.25 GiB 预留。下一项地址布局实验应允许更大的稀疏客体范围，并单独约束 ART 压缩引用使用的低地址对象范围；宿主 RAM 地址判定、用户复制、页表索引和 Direct flat 越界处理需同时检查。先以同一 SDK 四文件能否进入 `main` 并完成五项检查验证，后续再恢复上游 ART。

HyperOS 当前先受 linker 早期执行故障阻塞。更大地址范围只改变 SDK 已观测的地址容量条件；HyperOS 的 16 KiB 权限共页仍需独立加载器或 4 KiB 客体对照。原始四文件、APK/JNI 与完整 Android/HyperOS 的兼容性范围分别验收。Mac CLI/GCD 结果未覆盖 iPhone JIT、地址容量、APEX 激活、签名、Framework、图形或持续性能。
