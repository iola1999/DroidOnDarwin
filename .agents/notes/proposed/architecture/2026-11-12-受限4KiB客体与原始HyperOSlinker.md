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

### VMALLOC 分组实现

首次失败来自 `pcpu_alloc_chunk()` 的 `obj_exts` 元数据分配，实际请求为 65536 字节。当前生成配置已有 `CONFIG_NEED_PER_CPU_KM=y`，`mm/percpu.c` 包含 `percpu-km.c`，数据块由连续 `alloc_pages()` 分配。本轮只需继续处理元数据使用的通用 `vmalloc`；`percpu-vm.c` 的逐页映射留在未来 SMP 范围。

该阶段仍限定 4 KiB、单 CPU、Unicorn/PTE 和低 4 GiB，已在独立生成的 LKL 源码内实现以下行为。默认 16 KiB overlay 保持原行为。

- `__vmalloc_node_range_noprof()` 在申请 VM area 前，将该次实际分配长度和地址对齐提高到 16 KiB，并检查加法溢出及 RAM 上界。请求长度继续保留给原有对象大小统计。物理 backing 通过 `vm_area_alloc_pages()` 以 order 2 为最小单位分配，再沿用 `split_page()` 和逐 4 KiB `struct page` 数组及释放循环。页表仍使用普通 4 KiB PTE；不为该实验声明 PMD huge-vmap 支持，也不增加另一份持久页分组状态。order 2 分配失败必须释放已分配的页面；不能退回不满足宿主页条件的 order 0。`__GFP_NOFAIL` 的高阶失败处理必须单独验证，受限实现若无法保留其重试契约，就在入口明确停止并保存参数。
- `init_mm` 的 map/revoke 从单个 PTE 写入前通知，移到完整范围的 PTE 写入或清除后通知。优先复用 `ARCH_PAGE_TABLE_SYNC_MASK` 与 `arch_sync_kernel_mappings()`：`mm/vmalloc.c` 已在范围结束后调用；`mm/memory.c` 的 `apply_to_page_range()` 也会调用，需要限定实际 `init_mm`，用户 mm 继续采用现有失效通知。映射失败的范围只清理已写入 PTE，完整映射成功后才建立宿主 alias；前面已经成功的组在后续失败清理中全部撤销。回调返回前完成宿主操作，调用者随后才能使用或释放对应页面。
- 每个完整 16 KiB 目标组只有两种可执行状态：四个 PTE 都无映射，或者四页物理地址依次连续、首地址按 16 KiB 对齐且有效权限一致。前者撤销该完整组，后者使用真实共享 alias。混合状态、物理范围越界、非连续页及不同权限均明确失败。现有宿主 alias 检查继续执行，不改变其允许的范围。
- `vmap()`、`vm_map_ram()` 和范围映射入口在写 PTE 前检查目标长度、地址、物理页排列及权限。通用 page 数组可以由多个各自连续的 16 KiB 组组成；组内非连续的数组返回明确失败，不复制、重排或替换调用方的页面。当前配置以外的 `init_mm` 直接页表修改路径先记录为未覆盖，不能仅靠单页通知返回就宣称支持。
- 保留现有 VM area guard page 的含义。`free_unmap_vmap_area()` 会连同尾部 4 KiB guard 一起清 PTE，范围通知只能撤销请求内部的完整 16 KiB 组。边界不足一组且整组 PTE 都无映射时不调用宿主操作；边界组仍有映射则明确失败。不得扩大撤销范围覆盖相邻区域。partial map 失败、尚未建立宿主 alias 的清理和尾部 guard 分别检查。

实施前先保存原内核、运行器与诊断输入的摘要，候选使用新的内核输出目录。先在真实 LKL 内验证两组 16 KiB 的共享别名：逐个 4 KiB 页写入，经另一地址读回，再反向写入；撤销第一组后第二组的地址、权限和字节保持不变。非连续、错位、部分组、不同权限及分配/映射中途失败分别检查返回、PTE 与宿主映射释放。随后重跑 core CLI/GCD，通过后继续原始 HyperOS linker 和 SDK 对照。原始四文件的摘要与此前记录一致。

复杂度审查把此改动限定为一种既定宿主页分组条件。真实使用方是当前 `pcpu` 元数据和后续 `vmalloc` 调用者；PTE、页面数组与宿主 alias 各保留原有职责。没有增加运行期开关、通用内存模型、复制层或与 PTE 平行的已安装映射表。若现有范围通知无法覆盖实际失败清理，先保存失败，再按具体调用增加必要处理。

## 曾考虑的替代方案

- 同时扩到 64 GiB。会同时改变页大小与地址容量，本轮保留低 4 GiB，后续根据两个独立结果决定组合。
- 先改 Direct flat。需要同步汇编 PTE 掩码、代码页索引与每 mm 表容量；已有 Unicorn 通过软件 TLB 获取 LKL 页权限，适合作为首个页粒度对照。
- 把所有 16 KiB 常量改为 4 KiB。宿主映射与 JIT 仍受 16 KiB 约束，会混淆两种页大小；本轮按实际使用方区分。
- 将 4 KiB kernel alias 请求向外取整。可能覆盖同宿主页中的其他映射或拼接不连续物理页，本轮保留严格拒绝。
- 关闭 `CONFIG_MEMCG` 或仅将 `pcpu` 元数据改用大块连续分配。可以改变首次分配路径，通用 `vmalloc` 的页粒度约束仍然存在；下一阶段保留当前配置并处理实际映射条件。
- 启用 `CONFIG_HAVE_ARCH_HUGE_VMALLOC` 取得分组字段。该配置依赖 `HAVE_ARCH_HUGE_VMAP`，上游说明要求 PMD 级支持；本轮仍使用 4 KiB PTE，只提高 backing 分配 order，避免声明额外架构条件。

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

### 首次 VMALLOC 调用者诊断（2026-11-12）

在 `bdadae8a7e82874142bc4b254136ea703acef534` 上，临时宿主 `abort` 替代函数打印了调用帧与栈内 kernel 地址候选；当前内核关闭 frame pointer，候选地址不能还原完整调用栈。原镜像的两个返回位置已与反汇编对应：`0x24281c` 的前一条指令调用 `__vmalloc_noprof`，`0x266d28` 的前一条指令调用 `mmap_pages_for_ptes`。

随后只在 `work/native-linux-nce/guest-4k-vmalloc-trace/caller/` 中重编 `percpu.c` 和 `vmalloc.c` 两个诊断对象，保留原归档的其他对象及顺序，并链接独立镜像。`lkl_printf` 实际输出 `NCE_VMALLOC_CALLER obj_exts bytes=65536 nr_pages=8`，紧接 `NCE_VMALLOC_REQUEST bytes=65536 align=1`。首个 alias 仍为 `target=0x7040800000`、`source=0x7044811000`、`bytes=4096`，原检查继续拒绝。诊断退出码 86 由替代 `abort` 主动返回；客体尚未启动。环境与受限阶段相同，运行期间有其他本机功能验证，无性能结论。

[调用者诊断结果](../../../../experiments/native-linux/results-guest-4k-vmalloc-caller-20261112.json)保存实际配置、两个打印补丁、编译/链接命令、原始输出及摘要。原受限内核的 24 项清单输入再次校验一致。生成源码中的关键位置为 `mm/percpu.c:504`、`:1466`，`mm/percpu-km.c:53`，`mm/vmalloc.c:3001`、`:3600`、`:3743`；内核固定提交仍为 `d0f76a77e8c2ca68bd7d96190b8da09040eab072`，补丁输入由既有内核清单记录。

该诊断提交只完成调用者确认与后续方案。分组分配、范围通知及对应实际映射和失败回归在下一节记录。

### VMALLOC 与 core 阶段结果（2026-11-12）

[完整结果](../../../../experiments/native-linux/results-guest-4k-vmalloc-20261112.json)保存 4 KiB core、默认 16 KiB core、宿主 alias 与实际 LKL VMALLOC 测试四份报告，合计 1048448 字节，SHA256 为 `7bc151287383ce9ab590d800601324ea0f131aea18dc061937892a1e8f4b0ea5`。基础提交为 `84ecca1dde444699a4646287b91ab11c196d64ad`，实际源码和生成物由各报告的摘要记录。宿主仍为 Apple M5 Ultra、macOS 27.0 / `26A428`、16 KiB 页。运行与其他本机功能构建有重叠，本节只验收功能。

4 KiB core 使用单 CPU、Unicorn/PTE、low 4 GiB、1 GiB RAM、dual JIT 映射；CLI/GCD 均 `returncode=0`、`validated=true`，各 62 条 `passed=true`，各 68 个上下文分配并回收。每轮实际收到 6 次 host map、1 次 revoke、0 次 reclaim，全部通过严格范围检查；6 次 map 包含完整 RAM 映射和 5 个 VMALLOC 组。默认 16 KiB core 使用同一执行后端及 host 地址布局，两入口也各 62 条成功、68/68 上下文回收。两份报告分别 61、60 项输入摘要与运行结束后的文件一致。

```sh
python3 -B scripts/probe_lkl_translation.py --guest-page-bytes 4096 --guest-layout low --mapping pte --execution unicorn --code-memory dual --reuse-engine-build --workload core --entry both --output work/native-linux-nce/guest-4k-vmalloc-core-v4
python3 -B scripts/probe_lkl_translation.py --guest-page-bytes 16384 --guest-layout host --mapping pte --execution unicorn --code-memory dual --reuse-engine-build --workload core --entry both --output work/native-linux-nce/guest-16k-vmalloc-regression-v2
python3 -B scripts/test_guest_pages.py
python3 -B scripts/test_guest_vmalloc.py work/native-linux-nce/guest-4k-vmalloc-core-v4/summary.json
```

宿主测试的 26 项检查成功，覆盖原有范围与参数拒绝，以及新增的真实 `shmem_map`、只读权限变更和撤销。两个 16 KiB 组逐 4 KiB 检查双向共享；第一组撤销后，其宿主权限为 `PROT_NONE`，第二组仍为读写且字节不变。测试直接包含产品 `linux-execution.c`，权限来自 `mach_vm_region` 查询。

LKL 测试从成功构建的真实 `mm/vmalloc.c` 生成单个测试对象，故障计数只作用于当前测试 task；其余内核对象保持原归档顺序。normal CLI/GCD 各 9 条内核检查成功，并继续执行 core 至正常退出。检查覆盖 1 字节请求实际取得 16 KiB backing 和 4 KiB guard、真实 alias 双向共享与相邻组保留、非连续/错位/部分组/NULL 页拒绝。order 2 第 0/1 组分配失败分别分配并释放 0/0、4/4 页；写入第 1/3/5 个 PTE 后失败均分配并释放 8/8 页，PTE 清空、VMALLOC 页面计数复原，先前 alias 的字节保留。报告的 27 项测试输入摘要一致，原内核清单在测试前后均一致。

当前 `CONFIG_HAVE_ARCH_HUGE_VMALLOC` 与 `CONFIG_HAVE_ARCH_HUGE_VMAP` 均关闭。受限补丁额外使用编译期检查拒绝开启任意一项，两个真实编译用例均失败并输出 `4 KiB LKL VMALLOC requires ordinary PTE mappings`。这是必要条件：原 `__vmalloc_node_range_noprof()` 的 huge 分支会重新使用 `real_size`/`real_align`，可能覆盖本实验的 16 KiB 分组条件。

`__GFP_NOFAIL` 在本实验入口打印参数并调用 `BUG()`，改变了上游持续重试契约。普通 core 和 normal 测试均未触发该条件；单独 CLI/GCD 用例实际请求 1 字节、`gfp=8cc0`，均在客体启动前退出 86。混合权限用例先创建真实共享 alias，再修改组内一个 PTE，两入口也均在范围通知处退出 86。退出 86 来自测试宿主对 `abort` 的替代，产品路径仍主动停止。这四个用例的 `validated=true` 只表示识别到预定拒绝，不能作为相应调用成功的结论。本阶段只提供受限 4 KiB 实验，完整 Android、其他 VMALLOC 调用者、Direct、SMP 与 iOS 仍未验证。

初次测试对象构建已通过 9 条内核检查，进入 guest 后发生 `SIGSEGV`。该测试镜像改变了内核函数地址，宿主却复用原 core 的 `translation-symbols.h`。测试脚本现从测试 ELF 重新提取 `nce_copy_user`、`nce_translate_user`，并核对 ELF 可执行段与 image 中的字节；随后六次 CLI/GCD 运行按预期结束。原失败日志保留在 `work/native-linux-nce/guest-4k-vmalloc-tests-initial/`，这次失败属于测试构建身份错误。

复杂度审查保留一个仅由 4 KiB 构建调用的生成补丁，使用方为现有 `vmalloc`、`vmap`、范围同步与宿主 alias。普通 PTE 和 `struct page` 数组继续保存映射与释放信息，未增加另一份 alias 表。去掉 order 2 分配会重新产生宿主页内非连续 backing；去掉范围通知会恢复首次 4 KiB alias 失败；去掉 guard 检查可能扩大宿主撤销范围。故障注入保留在独立测试对象，产品没有新增测试开关。测试脚本沿用已有构建清单、链接命令和 ELF 打包函数，未新增内核构建框架。

Python 编译、格式检查、`git diff --check` 与 Bionic 解析 7 项回归成功。原始 HyperOS/SDK linker 的 4 KiB 对照见下一节；本节没有 iOS 验收结论。

### 原始组件的 4 KiB 对照（2026-11-12）

在 `ee745ce0c23c6cda3972468a625e843b0114b00d` 上完成原始 HyperOS 与 SDK36 r07 四文件的 CLI/GCD 运行。[完整结果](../../../../experiments/native-linux/results-guest-4k-stock-bionic-20261112.json)包含两份报告、四轮原始 stdout/stderr、命令与摘要，372296 字节，SHA256 为 `c19159cd3647d944c6c83ab84407270b855a94f7e654f02ba8ab210b15d7ec38`。环境与上一阶段相同，配置仍为 4 KiB、单 CPU、Unicorn/PTE、low 4 GiB、1 GiB RAM、dual JIT 映射；没有性能比较。

| 原始 runtime | 入口 | linker 帮助入口 | 动态阶段 | `main` 与五项检查 |
| --- | --- | --- | --- | --- |
| HyperOS | CLI、GCD | `exit_code=0`、`usage=true` | Scudo 预留失败，`signal=6` | 未到达 |
| SDK36 r07 | CLI、GCD | `exit_code=0`、`usage=true` | Scudo 预留失败，`signal=6` | 未到达 |

HyperOS 先前在 `0x190610` 的取指失败没有在本轮出现，原始 linker 已执行到帮助入口正常退出。该结果支持继续沿 4 KiB 页粒度处理这份 ELF。动态阶段已经装入原始 libc，但四轮均请求 `mmap(0, 8858370048, PROT_NONE, 16418, -1, 0)`，返回 `-12`；stderr 输出 `Scudo ERROR: internal map failure (error desc=Out of memory) requesting 8650752KB`，随后 `SIGABRT`。请求长度为 8.25 GiB，超过当前低 4 GiB 客体范围。HyperOS 与 SDK 的动态阶段均继续受地址容量限制，原始 Bionic 功能检查仍未完成。

四轮宿主均 `returncode=0`、`validated=true`，该字段表示监督程序完整保存并通过失败结果的格式与顺序检查。组件是否成功以 `component_stage.success`、退出/信号及 `component_main`、五项 `component_check` 为准；本轮动态阶段的 `success` 均为 false，后两类记录均缺失。

HyperOS 输入来自既有第三方 `NEORUAA/HyperOS-AVD` 的 `v0.1.0-a16-hyperos3-fuxi-r1`，来源提交与文件身份由原清单记录；SDK 为固定 `arm64-v8a-ps16k-36_r07.zip`。两份清单摘要分别为 `c9ec384e1eee4c754152fddaae8d351ff5f1adaafb86e5cdc88fb02f5f712d0a`、`4b1a45b0ee51725c68f1c29765958d0b60b45a5f2325bedfc61764cd49232249`。每份报告的 64 项输入摘要、原四文件和清单在运行结束后再次逐项匹配；本轮没有修改原始 ELF 字节。完整 Android、Direct、SMP 和 iOS 继续未验证。

复现命令如下，输入路径使用原工作区已校验文件：

```sh
python3 -B scripts/probe_lkl_translation.py --guest-page-bytes 4096 --guest-layout low --mapping pte --execution unicorn --code-memory dual --reuse-engine-build --workload bionic --entry both --runtime-source hyperos --runtime-directory /Users/fan/project/github.com/iola1999/DroidOnDarwin/work/project-audit-2026-11-12/references/hyperos3-input/core-scan-v4/files/system/apex/com.android.runtime.apex.contents/files --runtime-manifest /Users/fan/project/github.com/iola1999/DroidOnDarwin/experiments/native-linux/results-hyperos-input-20261112.json --output work/native-linux-nce/guest-4k-hyperos
python3 -B scripts/probe_lkl_translation.py --guest-page-bytes 4096 --guest-layout low --mapping pte --execution unicorn --code-memory dual --reuse-engine-build --workload bionic --entry both --runtime-source sdk36 --runtime-directory /Users/fan/project/github.com/iola1999/DroidOnDarwin/work/native-linux-nce/android/android36/com.android.runtime --runtime-manifest /Users/fan/project/github.com/iola1999/DroidOnDarwin/work/native-linux-nce/android/android36-runtime.json --output work/native-linux-nce/guest-4k-sdk36
```

## 风险

当前 VMALLOC 只接受完整宿主页组，组内非连续或不同权限请求会被拒绝；`__GFP_NOFAIL` 会主动停止。4 KiB 三级页表仅覆盖 39 位地址，现有 `2^40` 宿主上界必须收紧；地址申请无法满足上界则明确失败。4 KiB 会改变分配碎片、页缓存、COW 和回收统计，本轮结果仅用于功能与页权限分析。Direct flat、宿主 code-watch 的四客体页分组失效、read-view 任意物理页别名及 SMP 仍需后续独立实现。
