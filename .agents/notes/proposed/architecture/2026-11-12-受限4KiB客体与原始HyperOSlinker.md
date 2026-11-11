# 决策记录: 受限 4 KiB 客体与原始 HyperOS linker

状态: proposed

## 问题

[原始 linker 与 Bionic 检查](../../implemented/testing/2026-11-12-原始linker与Bionic组件验证.md)的 HyperOS CLI/GCD 均在原文件地址 `0x190610` 取指失败。原 linker 的 RX LOAD 为 `[0x4c000, 0x190660)`，RW LOAD 从 `0x191000` 开始，全部 LOAD 的 `p_align` 为 `0x1000`。两段按 16 KiB 取整共用 `[0x190000, 0x194000)`，按 4 KiB 取整则分属不同页。故障位置符合权限共页假设，仍需保持原始 ELF 字节的实际对照。

现有 translated kernel、页表布局、失效位图和运行报告共同使用 16 KiB。切换 Unicorn 仍会执行 LKL 的原生内核；内核 `init_mm` 的 VMALLOC 映射继续调用 Darwin `vm_remap`、`mprotect` 和 `mmap`，需要另外检查宿主 16 KiB 页的限制。

## 提案

### 受限运行条件

从主树 `a9f745ddc3cb53120cf1fef3cb1bdd345789dfdf` 加入标准 JIT 提交 `b9102031794d3acdff4759d0a4d932206004cec8`，建立独立 `codex/offline-4k-guest` 工作树。主树只增加本记录，独立树只复用 LKL、musl、sysroot 和 Unicorn 构建所需依赖；原始 SDK/HyperOS 文件只读引用，完整 Android 镜像不复制。

增加明确的 4 KiB 构建变体，首阶段限定 Mac、单 CPU、Unicorn、PTE、guest `[0x10000, 4 GiB)`。产品默认及原生 NCE 继续使用 16 KiB。4 KiB 三级页表使用 12/21/30 位移和每级 9 位索引，内核 RAM/VMALLOC 地址及完整长度必须落在 `2^39` 以下。客体范围、宿主 RAM 别名和 RAM backing 保持数值分离，所有范围在索引或取模前检查。

内核页大小、共享页表常量、代码失效位图和运行器使用同一个构建参数；1 GiB RAM 的位图从 1024 个 64 位字改为 4096 个。输出目录、构建清单、执行配置及校验必须包含实际客体页大小。宿主 kernel ELF、Unicorn JIT 内存和宿主 mmap 对齐继续使用 16 KiB，内核线程栈也保留现有 16 KiB。初期不启用 Direct、code-watch、read-view 或 SMP。

### 内核别名的拒绝条件

首轮保留现有宿主别名实现，记录实际 `init_mm` 映射/撤销及空闲 RAM 回收请求。执行 Darwin 调用前，target、source offset 和 size 都必须按 16 KiB 对齐；范围溢出、超出 RAM/VMALLOC reservation、地址重叠或不足一整个宿主页均使本轮明确失败，保存请求字段和失败日志。禁止向外取整后覆盖相邻客体页。完整启动映射 1 GiB RAM 满足该条件；默认 `reporter.order=4` 在 4 KiB 下回收至少 64 KiB 的隔离 buddy 块，仍需按实际请求检查对齐。

若 core 或 linker 实际请求不兼容的 VMALLOC 别名，本轮保留失败，先报告调用范围，再决定后续修改。可考虑为 kernel VMALLOC 按 16 KiB 组分配连续物理页、成组映射和撤销；通用 `vmap` 允许非连续物理页，不能由这个分配策略自动覆盖。此类后续改造另按实际失败补充记录与验收。

### 验证顺序与平台边界

先验证构建身份、参数拒绝和 4 KiB 边界，再执行 Linux core CLI/GCD。通过后使用原始 HyperOS 四文件运行 linker 帮助入口及既有 Bionic harness，保留每个阶段的实际退出/信号/字节摘要；随后以原始 SDK36 r07 四文件对照。低 4 GiB 仍无法容纳已观测的 SDK Scudo 8.25 GiB 预留，其实际失败继续记录。该变量与稀疏地址候选分别验证。

本阶段不改 iOS App、打包流程或默认 runtime，也不安排手机运行。宿主头文件和可编译的公共对象可做 iOS 编译检查，但本机的地址摆放、Unicorn JIT 条件与 VMALLOC 路径不能据此确认 iOS 可运行。首阶段无性能结论。

## 曾考虑的替代方案

- 同时扩到 64 GiB。会同时改变页大小与地址容量，本轮保留低 4 GiB，后续根据两个独立结果决定组合。
- 先改 Direct flat。需要同步汇编 PTE 掩码、代码页索引与每 mm 表容量；已有 Unicorn 通过软件 TLB 获取 LKL 页权限，适合作为首个页粒度对照。
- 把所有 16 KiB 常量改为 4 KiB。宿主映射与 JIT 仍受 16 KiB 约束，会混淆两种页大小；本轮按实际使用方区分。
- 将 4 KiB kernel alias 请求向外取整。可能覆盖同宿主页中的其他映射或拼接不连续物理页，本轮保留严格拒绝。

## 验收标准

- 独立构建与结果保存基础提交、补丁摘要、内核配置、guest/host 页大小、地址上界及实际后端；旧 16 KiB 产物不被复用为 4 KiB 结果。
- core CLI/GCD 实际报告 4096，覆盖相邻 4 KiB 页的读写/执行权限、跨页复制、fork/COW、共享页、撤销和非法高位地址，完整保留失败日志。
- 每个宿主别名/撤销/回收请求经过 16 KiB 条件检查；首次失败保存完整参数。报告不能仅凭 runner 退出码宣称通过。
- 原始 SDK/HyperOS 四文件逐项摘要与已有记录相同；linker、`main` 与组件检查分别验收，SDK 的预期容量限制与其他失败分开记录。
- 必要的宿主边界回归使用真实参数检查，覆盖 4 KiB 对齐但非 16 KiB 对齐的 target/source/size、溢出和边界值。需要扩展 VMALLOC 后再补对应实际映射/撤销测试。

### 受限阶段结果（2026-11-12）

独立树从 `a9f745d` 加入 `b910203`，合并基础提交为 `c26c548`；冲突只涉及 `probe_lkl_translation.py` 的 Bionic 与标准 JIT 入口组合。4 KiB LKL、CLI/GCD 和客体测试 ELF 均构建成功。宿主为 Apple M5 Ultra，macOS 27.0 / `26A428`，宿主页为 16384 字节。结构化结果见[受限 4 KiB 记录](../../../../experiments/native-linux/results-guest-4k-restricted-20261112.json)。

首轮 CLI 在 kernel VMALLOC 的首次 4096 字节请求处被拒绝。最终源码版本的 CLI/GCD 均复现该失败，退出为 `SIGABRT`（`returncode=-6`），报告保留 `validated=false`。两轮的 `physical_base=0x7000000000`，先成功映射完整 1 GiB RAM；第二次请求为 `target=0x7040800000`、`source=0x7044811000`、`bytes=4096`。target 为 RAM 末尾加 8 MiB，即当前 VMALLOC 起点；source 在宿主页中的偏移为 `0x1000`。检查在 Darwin 调用之前停止，未修改相邻页。内核尚未运行到 core 客体，HyperOS/SDK 组件检查与 iOS 构建继续待验收。

复现命令在独立树执行：

```sh
python3 -B scripts/probe_lkl_translation.py --guest-page-bytes 4096 --guest-layout low --mapping pte --execution unicorn --code-memory dual --reuse-engine-build --workload core --entry both --output work/native-linux-nce/guest-4k-core-v2
python3 -B scripts/probe_lkl_translation.py --guest-page-bytes 4096 --guest-layout low --mapping pte --execution unicorn --code-memory dual --reuse-engine-build --workload core --entry gcd --output work/native-linux-nce/guest-4k-core-gcd
python3 -B scripts/test_guest_pages.py
```

第一条命令按既有行为在 CLI 失败后停止，第二条单独验证 GCD。两份结果各 60 项输入摘要与最终文件匹配，原始 jsonl/log 均保留在对应目录。`test_guest_pages.py` 编译实际 `linux-execution.c` 中的私有检查函数，19 项范围/对齐检查与 6 项不兼容参数拒绝检查成功。该测试调用检查函数，不建立宿主映射，也不运行 Linux。既有 Bionic 解析 7 项回归成功；标准 JIT 真实报告的两轮样本通过，682 个缺项/重复/错误变体被拒绝；默认 16 KiB Direct 汇编、Python 编译及格式检查成功。

依赖使用 APFS clone 复制 LKL 源码、基础构建配置、宿主/Linux 头文件、musl install、compiler-rt、小型运行 ELF 和 Unicorn 所需文件。17 项 Unicorn 摘要匹配。独立 `work/native-linux-nce` 的 `du -sh` 为 3.4 GiB，其中包含两份 Linux 源码树；该数值未扣除 APFS 共享区块，未复制完整 Android 镜像。

复杂度审查保留一个构建期页大小参数；运行器、内核与客体从该值取得页大小，宿主映射页单独保持 16 KiB。私有 alias 检查集中于三处实际宿主操作之前，测试直接调用该函数；没有引入替代内存模型或通用 VMALLOC 适配层。现阶段只增加严格拒绝和诊断，后续共享别名处理按已出现的实际请求继续调查。

## 风险

首次运行可能在原生内核 VMALLOC 路径停止，这属于本阶段需要定位的依赖。4 KiB 三级页表仅覆盖 39 位地址，现有 `2^40` 宿主上界必须收紧；地址申请无法满足上界则明确失败。4 KiB 会改变分配碎片、页缓存、COW 和回收统计，本轮结果仅用于功能与页权限分析。Direct flat、宿主 code-watch 的四客体页分组失效、read-view 任意物理页别名及 SMP 仍需后续独立实现。
