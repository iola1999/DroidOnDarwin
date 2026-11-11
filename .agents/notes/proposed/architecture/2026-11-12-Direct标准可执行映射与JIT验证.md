# 决策记录: Direct 标准可执行映射与 JIT 验证

状态: proposed

## 问题

完整 Android 与原始签名 APK 需要通过标准 Linux 接口加载、生成和更新 ARM64 代码。实施前 Direct 的 `--mapping pte` 内核沿用原生 NCE 的普通内存系统调用限制：匿名可执行映射、普通 memfd 的 RW→RX、专用窗口之外新增执行权限，以及移动可执行 VMA 均可能提前返回 `EACCES`。这些限制会阻止依赖标准 JIT 接口的 JNI 库。真实标准 JIT 程序已复现匿名 RW→RX 的 `EACCES`，本轮独立工作树已恢复 PTE 内核的普通映射语义；主工作区完整桌面集成结果见后文，原始 APK 在本次变更上的完整验收仍待完成。

初次调查期间产品源码处于基线冻结阶段。静态调查以主工作区 `b5cace0df3db3caad6ec15fda7228f6a885639f7` 的相关源码为依据，并核对 `work/offline-low-guest` 中基于 `934deca289da8086a36adf17fd4d4c93385363da` 的未提交低地址候选。初次调查只读取源码；后续实施与本机功能结果记录如下。低地址方案见[Android 地址空间与执行架构重选](2026-11-12-Android地址空间与执行架构重选.md)。

### 实施状态

2026-11-12 主会话继续授权实现。在低地址提交 `c0406bafb650f84b65cb4af0046781398534db2c` 上建立独立工作树 `work/offline-standard-jit` 与分支 `codex/offline-standard-jit`，实现修改仅发生在该工作树。主工作区只更新本记录。依赖按低地址候选清单通过 APFS clone 复用，Unicorn 输入与静态库逐项核对摘要；构建输出位于独立树的 `work/`。先加入真实匿名 RW→RX 程序复现现有拒绝，再修改 translation 普通 VM 语义。功能检查允许与其他任务并行，耗时不用于性能比较。

### 实施前的调用与限制

| 位置 | 已核对行为 | 对标准 JIT 的影响 |
| --- | --- | --- |
| [lkl_nce_patch.py](../../../../scripts/lkl_nce_patch.py#L235) 的 syscall 注册与分发 | AArch64 `mmap` 进入 `nce_mmap`；`mprotect`、`mremap` 在 Linux 原有实现前调用 `nce_check_memory_syscall`。`secure_computing()` 位于这些分发之前 | Direct 使用的 translation 内核继承了 NCE 前置限制；恢复映射语义无需改变 seccomp 顺序 |
| [lkl-nce-elf.c](../../../../experiments/native-linux/lkl-nce-elf.c#L226) 的 `nce_mmap` | 文件先做 ELF 注册；`PROT_EXEC` 要求已注册 ELF 且没有 `PROT_WRITE`；fixed 映射不得覆盖专用动态窗口。整个 ELF 的映射还可能强制使用注册地址 | 普通匿名、非 ELF 文件及 memfd 可执行映射被拒绝，普通映射位置受宿主 ELF 注册影响 |
| 同文件 `nce_check_memory_syscall`，第 187–223 行 | `mprotect` 拒绝 W|X，拒绝动态窗口之外从非执行 VMA 增加执行权限；`mremap` 拒绝动态窗口和任意可执行源 VMA | 常见 RW→RX JIT 及可执行映射移动失败 |
| 同文件 `nce_populate_code`，第 94–105 行；`nce_mmap` 第 257–263 行 | 可执行 `mmap` 返回前逐页读 fault，失败后撤销映射 | 该 eager populate 服务原生代码准备，Direct 已有指令缺页路径 |
| [linux-execution.c](../../../../experiments/native-linux/translation/linux-execution.c#L523) 的 `direct_map_code` | 通过 Linux PTE 的执行权限获取 RAM 字节，未查询 ELF 注册表 | Direct 取指本身没有固定 ELF 或 64 MiB 动态窗口要求 |
| [code-tests.c](../../../../experiments/native-linux/userspace/code-tests.c#L83) 的 `dynamic_code_tests` | 明确断言普通 memfd RW→RX 返回 `EACCES`，随后通过 243/244 映射与发布代码；translation core 在 `linux-guest.c:87` 调用它 | 现有成功结果仅覆盖专用动态代码契约，需要新的标准接口回归 |

原生 NCE 的预备代码地址与宿主执行窗口仍有实际用途，来源见[Android 用户态与 Metal 阶段记录](../../implemented/architecture/2026-10-09-Android用户态与Metal阶段记录.md)。本提案仅调整 translation 内核的普通 VM 操作。`--mapping window` 继续使用原生内核，不包含本次语义变更。

### 实施前的缓存处理与调查项

Linux 保持 VMA、PTE、文件页与 COW 的所有权。[lkl-nce-mmu.c](../../../../experiments/native-linux/lkl-nce-mmu.c#L390) 的 `nce_page_fault` 先检查 VMA 的读、写、执行权限，再调用 `handle_mm_fault`。translation overlay 将 PTE 变更写入 mapping generation、TLB 范围和可执行物理页 bitmap，见 [lkl_translation_patch.py](../../../../scripts/lkl_translation_patch.py#L138)。新增执行权限、COW 更换物理页和撤销执行权限使用这条已有通知路径，本次 core 回归分别覆盖这些操作。

[Direct `resolve`](../../../../experiments/native-linux/direct/engine.cpp#L110) 按客体 4 KiB 代码页缓存，检查取指权限、物理来源和 code-watch version；没有可用 version 的页面通过内容比较确认是否变化。执行期间访问过的代码物理页会限制直接写入；写目标的 Linux PTE 可写时，`direct_execute` 将该次写入交给 Unicorn。code-watch 按宿主 16 KiB 物理页保护 `ram` 和 Linux direct-map 两份别名，写故障更新版本与 epoch；额外别名可以使页面退出版本保护，随后使用内容检查。对应实现位于 [code-watch.c](../../../../experiments/native-linux/translation/code-watch.c#L169) 与 [linux-execution.c](../../../../experiments/native-linux/translation/linux-execution.c#L579)。

单核 `prepare_unicorn` 消费 `code_generation`、当前 CPU 的 code generation 与物理页 bitmap，未直接消费 code-watch epoch。单核 `nce_copy_user` 通过 RAM 别名复制，没有显式调用 `changed_code`。SMP overlay 另有 executable PTE 计数、`nce_translation_written`、写入同步和停止执行逻辑，见 [lkl_translation_smp_patch.py](../../../../scripts/lkl_translation_smp_patch.py#L84)；这些处理目前不能直接用于描述单核行为。

这些差异决定了本轮验证场景：Unicorn 已缓存某代码页，随后 guest writable alias、内核 `pwrite` 或另一物理别名修改字节，VMA/PTE 未变化，执行又回到 Unicorn。尤其需要覆盖代码页在一次 fallback 后半段才被 Unicorn 访问、尚未经过 Direct `resolve` 的情况。后续实测复现了 `pwrite` 后继续执行旧代码，具体失败与修复见本机验证小节。

[decode.cpp](../../../../experiments/native-linux/direct/decode.cpp#L194) 当前识别 `DMB`/`DSB` 等指令，未识别 `CTR_EL0`、`DC_CVAU`、`IC_IVAU`、`ISB`；这些指令进入 Unicorn。已下载的 Unicorn 2.1.4 在 `qemu/target/arm/helper.c:4389` 附近将 cache ops 定义为 NOP，普通 guest store 则可通过 `qemu/accel/tcg/cputlb.c` 的 `notdirty_write` 使其 TB 失效。内核或宿主写入不经过这条 guest store 路径。Direct 编译器调用 `sys_icache_invalidate` 的对象是宿主生成代码，客体标准 cache maintenance 的通知缺口由本次新增的 IC hook 处理。

## 提案

### 先恢复普通 VM 操作，保留已有专用接口

在 `scripts/lkl_translation_patch.py` 内统一生成 translation 版本 `arch/lkl/kernel/nce_elf.c`，使用 tracked `lkl-nce-elf.c` 作为输入，替换普通 `nce_mmap` 和 `nce_check_memory_syscall` 的函数体。原生源文件保持现有实现。低地址候选已经在 overlay 末尾从该文件重新生成 `nce_elf.c`，新变更必须合并到同一生成步骤，防止后续写入覆盖此前修改。

普通 `nce_mmap` 只保留 AArch64 byte offset 对齐与页偏移转换、fixed 客体范围检查、低于合法下界的非 fixed hint 归零，然后进入 `ksys_mmap_pgoff`。移除该普通入口中的 ELF 注册、强制 ELF 地址、动态窗口冲突检查、私有 `PROT_EXEC`/W|X 拒绝与 eager populate。Linux 继续检查 flags、文件权限、LSM、noexec、memfd seal 和实际 VMA 分配。调用者显式传入的 `MAP_POPULATE` 等标准 flags 保留其 Linux 行为。

普通 `mprotect`/`mremap` 的前置处理只保留客体地址范围约束，包含 `MREMAP_FIXED` 目的范围、长度与地址运算溢出、向上页对齐后的实际范围，以及 `old_size == 0` 的共享映射复制入口。权限变更、可执行 VMA 移动和 Linux 错误返回由原有实现处理。新增执行权限后首次取指通过 `nce_page_fault` 安装页面，保持 Linux PTE 与现有失效通知的一致性。

低地址接口沿用 route 候选：`guest_end` 决定 `TASK_SIZE`，`physical_base` 决定 Linux RAM 宿主别名，`ram` 保持独立 backing；合法客体范围为 `[64 KiB, 4 GiB)`。继续保留 `FIRST_USER_ADDRESS`、`user_reserved` 和非 fixed 低 hint 处理。flat 容量仍覆盖 4 GiB，末尾 64 KiB 没有合法 PTE。本次不新增布局配置或第二份 VMA 表。

translation overlay 同时用于 Direct 与纯 Unicorn 的 PTE 模式，二者采用相同的 Linux VM 契约。执行器选择继续由现有宿主参数控制，不增加「是否允许 JIT」的内核状态。客体 RWX 页仍存放在 RAM，宿主 Direct/Unicorn 生成代码池的 W^X 或 dual-mapping 实现维持现有管理方式。

保留 243/244/245 及其调用方：ART 的 `MemMap::MapNceCode` 与 code publish 分别位于 [android16.patch](../../../../experiments/native-linux/art/android16.patch#L831) 第 831–839、1328 行，Android linker 在 [build_android_linker.py](../../../../scripts/build_android_linker.py#L70) 调用 245。`arch_setup_additional_pages`、ELF exec 装载和专用 reserve 仍使用 ELF 注册；`nce_populate_code` 保留在 sigreturn 与专用 publish 路径。243 的分配和 244 的发布继续使用旧 64 MiB 动态窗口，Linux VMA 冲突由 allocator 处理。普通映射可能占用未来旧接口需要的地址，必须通过现有 ART/linker 回归检查实际兼容情况。

本阶段没有恢复全部 ELF 装载和 ART 上游实现。`fs/binfmt_elf.c` 的注册布局、sigreturn 与专用 linker 预留仍有当前调用方。后续移除这些适配需要独立验证与记录。

### 通过标准接口测试决定是否补充失效通知

先用真实程序区分权限限制与缓存一致性故障。权限放开后，所有测试都使用标准 `mmap`、`mprotect`、`mremap`、`memfd_create`、`pwrite`、`fork`、signal 和 cache maintenance，生成代码路径不得调用 243/244/245。

若复现旧代码执行，先记录写入来源、物理页、VMA/PTE 是否改变、Direct 与 Unicorn 的实际进入顺序，再补充缺失通知。优先复用现有物理页 bitmap、code generation 和写入通知；SMP 已有的 executable PTE 生命周期可作为复用来源。单纯比较 code-watch epoch 只覆盖受到其保护或被显式 invalidation 的页面，不能据此宣称覆盖所有 Unicorn-only 代码页和外部写入。

可临时使用「每次进入 Unicorn 前清空 TB」作为诊断对照，以判断结果是否依赖旧 TB。最终实现需记录失效范围与触发来源；完整清空产生的成本需在后续性能阶段测量。当前阶段不新增 JIT 地址注册表，不要求第三方库调用私有发布接口，也不提前扩大 Direct 指令支持范围。`CTR_EL0` 与 cache maintenance 的原生支持可以随后独立评估，正确性验收先接受有明确记录的 Unicorn fallback。

### 最小文件范围与复杂度审查

| 文件 | 本次处理 | 必要性与移除影响 |
| --- | --- | --- |
| `scripts/lkl_translation_patch.py` | translation 版本普通 VM 函数替换，与低地址生成步骤合并 | Linux 已有 VMA、flags 和权限实现，减少 NCE 规则在翻译路径中的重复检查；原生 NCE 源码保留 |
| `scripts/build_lkl_translation_kernel.py` | 确认 tracked/生成 `nce_elf.c` 纳入输入摘要 | 低地址候选已加入这两项；合并后无需重复新增配置或状态 |
| 新 `experiments/native-linux/translation/jit-guest.c` | 标准接口测试函数，可用编译宏选择独立 `main` | 同一源码可进入 core 或编译为普通 ARM64 ELF；不引入 fixture 框架 |
| `translation/linux-guest.c`、`userspace/code-tests.c` | PTE core 调用标准 JIT；保留原生专用测试和已有 SIMD fallback 检查 | 将 `simd_fallback_tests` 作为测试侧可调用函数即可复用，避免复制整段测试；window core 仍使用原生动态代码契约 |
| `scripts/probe_lkl_translation.py` 与一个对应解析回归文件 | 按实际内核 mapping 选择测试入口、编译输入与预期结果；严格解析 JIT 各用例 | 新标准结果不能依赖旧 `linux_dynamic_code` 成功行；既有 window 模式不得误用标准预期 |
| `translation/linux-execution.c` | 复现旧代码后增加 `IC_IVAU` hook，按客体物理页失效 Unicorn TB，并更新其他 CPU 已有 code bitmap | 内核 `pwrite` 不经过 Unicorn guest store；复用 `nce_uc_invalidate_ram`，没有修改 code-watch 或新增代码映射表 |

复杂度审查使用 `reclaim-code-entropy`：普通 VM 的实际消费者是标准 Android/JNI 动态代码；专用接口的实际消费者是当前 ART、linker 与原生 NCE。删去 translation 普通 VM 的 NCE 条件后，由 Linux 接管原有规则检查；移除 guest 边界检查会产生无法翻译的客体地址，必须保留。删除 243/244/245 会破坏当前 Android 用户态，本次保留。移除标准 JIT 测试会使普通接口继续依赖专用接口结果，故新增测试有独立验收用途。实施后复查确认：运行状态只增加每 CPU 两个诊断计数，VMA、物理页归属和跨 CPU 待失效集合继续使用现有 Linux 与 translation 结构。删除 IC hook 可重现 `unicorn_pwrite_update` 返回旧值；删除新解析检查会丢失用例唯一性、失败行和 signal 字段约束。新增 guest 用例由 PTE core 测试入口调用；Android 工作负载的启动路径没有增加这些检查。

## 曾考虑的替代方案

- 修改共用 `lkl-nce-elf.c`，直接对所有执行路径开放普通可执行映射。原生 NCE 仍依赖预备执行地址；该选项扩大本轮影响范围，未采用。
- 继续要求 JNI/JIT 使用 243/244。原始签名第三方 APK 的 JNI 库通常调用标准系统接口；修改这些二进制或重新签名会改变验收目标，未采用。
- 扩大专用动态窗口。它仍保留地址与接口限制，普通 RW→RX 和标准 memfd/mremap 行为未恢复，未采用。
- 单独构建一个 Direct 专用 VM 内核，并让纯 Unicorn 保持限制。PTE overlay 与 Linux VM 契约可以共同使用，额外内核模式增加构建与配置状态，未采用。
- 立即删除全部 ELF 注册、ART 修改和专用系统调用。现有 exec、sigreturn、linker、ART 仍有消费者；与低地址 ART 恢复分别验收，本轮保留。
- 未运行程序前引入一套新 JIT 映射管理器。Linux 已管理映射关系，初次调查尚未复现缓存缺口，未采用。

## 验收标准

### 真实 ARM64 标准 JIT 程序

程序使用 `sysconf(_SC_PAGESIZE)`，生成 `mov w0, #value; ret` 等真实 ARM64 指令。独立 ELF 与 core 链接版本使用同一测试源码，无 NCE 地址常量；生成代码路径只用标准系统调用。先反汇编实际构建，确认 cache maintenance 和强制 fallback 指令存在。两个 VA 的共享页先清理写别名的数据缓存，再失效执行别名的指令缓存；本机精简 `libbuiltins.a` 缺少 `__clear_cache`，首次编译失败后改用同一测试源内的 ARM64 标准指令序列：读取 `CTR_EL0` 确定 cache line，执行 `DC_CVAU`、`DSB ISH`、`IC_IVAU`、`DSB ISH`、`ISB`。独立 ELF 已成功编译和反汇编，指令检查保存在 `work/native-linux-nce/jit-standalone/build.json`。独立入口的 noexec 用例需要挂载权限。

| 用例 | 操作与预期 |
| --- | --- |
| 匿名 RW→RX | RW 写入，cache maintenance，改 RX，调用返回指定值；改回 RW、更新值、再改 RX 后返回新值 |
| 普通 RX 与 RWX | 匿名 RX 映射应进入 Linux 标准路径；RWX 是否允许由本次 Linux/LSM 配置决定，记录配置、返回值与 errno。基础探针允许的情况下必须成功，策略拒绝单独归类 |
| 非 ELF 可执行文件 | 普通文件写入代码后按 RX 映射并执行；确认无需 ELF header。追加只读 fd 请求 `MAP_SHARED | PROT_WRITE`、noexec 文件请求执行权限和 memfd seal 拒绝写入的负例，要求保留 Linux 拒绝行为 |
| memfd 双映射 | 使用 `MFD_EXEC` 创建 memfd；同一非零文件页偏移建立 RW/RX `MAP_SHARED` 两个 VA。先普通 store，再 `pwrite` 更新；每次同步后执行得到新值 |
| private fork/COW | 父子先执行相同代码；子进程把私有 RX 页改 RW、修改并恢复 RX，仅子进程得到新值。父进程仍得到原值；私有文件映射另核对 backing 文件没有修改 |
| shared fork | 父子共享 memfd 代码页，pipe/wait 同步写入与执行；父进程必须看到子进程完成发布后的新值 |
| 可执行 mremap | 事先保留目的地址，使用 `MREMAP_MAYMOVE | MREMAP_FIXED` 移动可执行映射；新地址可执行，旧地址得到 `SIGSEGV/SEGV_MAPERR`。另测共享映射 `old_size == 0` 复制，新旧地址均继续有效 |
| 权限撤回 | RX 写入产生 `SIGSEGV/SEGV_ACCERR`；去除 X 后执行同样得到 `SEGV_ACCERR`；munmap 后执行得到 `SEGV_MAPERR`。子进程 signal handler 通过 pipe 报告 signal、`si_code` 与故障地址，父进程核对结果 |
| 地址复用 | 解除旧代码映射，在相同 VA 映射不同 backing；执行结果对应新字节，父子同 VA 私有页仍隔离 |
| Direct/Unicorn 缓存切换 | 显式未支持指令使 Unicorn 预热代码页；分别通过 guest alias 与内核 `pwrite` 修改，保持 VMA/PTE 不变，再执行。覆盖 Direct 已访问页与只在 fallback 后半段访问过的页 |

标准 clear-cache 实现自身可能触发 Unicorn fallback。要检查后续 Direct 执行，clear-cache 后增加一次 `syscall(SYS_getpid)`，使下一轮重新尝试 Direct；同时保留无该额外系统调用的版本，检查同次 Unicorn 执行中更新后再次调用。配置、fallback 指令和执行计数必须入报告。程序返回正确结果仍需结合实际路径解释，不能据此宣称生成函数全程由 Direct 执行。

### 构建、运行与解析

以下为本机复现命令；本次运行从独立树已核对摘要的引擎产物增加 `--reuse-engine-build` 复用，具体参数与输出路径以结构化结果为准。`--guest-layout` 来自基点低地址提交；所有命令均使用 PTE 内核。host/low 两组各含 CLI 与 GCD；纯 Unicorn 对照使用 host。当前 low 候选只接受单核 Direct/flat。

```sh
python3 scripts/probe_lkl_translation.py --mapping pte --execution direct --direct-memory flat --guest-layout host --cpus 1 --code-memory dual --workload core --entry both --direct-code-watch 1 --direct-retain-routes 1 --direct-route-epochs 1 --output work/native-linux-nce/jit-standard-host
python3 scripts/probe_lkl_translation.py --mapping pte --execution direct --direct-memory flat --guest-layout low --cpus 1 --code-memory dual --workload core --entry both --direct-code-watch 1 --direct-retain-routes 1 --direct-route-epochs 1 --output work/native-linux-nce/jit-standard-low
python3 scripts/probe_lkl_translation.py --mapping pte --execution unicorn --guest-layout host --cpus 1 --code-memory dual --workload core --entry both --output work/native-linux-nce/jit-standard-unicorn
```

保留 code-watch、关闭 retained routes 后复跑一组，再同时关闭两者复跑，判断结果是否依赖版本保护或旧 route；不机械组合所有开关。现有参数校验要求 retained routes 启用 code-watch，关闭前者后才能关闭后者。SMP host 模式追加两 CPU 的共享代码更新与 COW 检查，覆盖 executable PTE 计数和写入通知；沿用当前 SMP 的原生独占、timer、pages exec-cache 和关闭 retained routes 要求。低地址 SMP 尚未实现，该矩阵保留为后续工作。原生 NCE 和 window core 继续运行原有专用动态代码检查，核对本次 overlay 未改变它们的契约。

结果按用例唯一名称、布尔 `passed`、实际返回值、errno、signal、`si_code`、故障地址和迭代次数严格解析。缺项、重名、类型错误、超时、子进程异常、任意失败行均导致整体失败。保留 stdout/stderr 原始日志；记录 guest ELF、内核、执行器、overlay 输入摘要及实际配置。解析回归覆盖缺项、重复成功掩盖失败和信号信息错误，不以进程 exit 0 代替行为核对。

普通权限与缓存回归完成后，再执行现有 Android linker/ART JIT、动态库加载、原始 Java/JNI APK 安装更新与重启检查。完整 Android 与原生 NCE 基线分别记录，当前 APK 验收状态见[通用 APK 离线验证](../testing/2026-11-12-通用APK离线验证.md)。本提案不提供性能改善数字，也没有 iPhone 上的标准 JIT 验证结果。

### 本机验证与实际修复

结构化结果见 [results-standard-jit-20261112.json](../../../../experiments/native-linux/results-standard-jit-20261112.json)。结果记录基点、最终源码摘要、每组构建与运行参数、内核和 guest ELF 摘要、stdout/stderr 摘要、实际用例输出及解析检查。原始 summary 与日志保留在独立工作树 `work/offline-standard-jit/work/native-linux-nce/`。本机为 Apple M5 Ultra、macOS 27.0、构建 `26A428`，客体 ARM64/Linux 页大小 16 KiB、RAM 1 GiB、Unicorn 2.1.4、宿主生成代码使用 dual mapping。其他任务可同时运行；本轮只核对功能。

先后保留了两个失败：

- `jit-permission-before-v2/summary.json`：初始最小程序的 `anon_rw_rx` 在 `mprotect` 返回 `EACCES`，CLI exit 1、validated false。该版 fixture 源码另存于相同目录 `input-jit-guest.c`。
- `jit-stale-before/summary.json`：普通 VM 已恢复，IC hook 尚未加入。`unicorn_pwrite_update` 修改代码为 202 后实际返回 201，CLI exit 1、validated false。该中间阶段保留原始日志和输入摘要；没有保存完整源码快照。

缓存修复仅增加 translation/PTE 的 `IC_IVAU` SYS hook。它将操作数经现有 `translate_user(..., LKL_PROT_READ)` 转换为客体物理页，立即调用 `nce_uc_invalidate_ram` 失效当前 Unicorn 的该物理页全部 TB；其他 CPU 使用已有 `code_pages` bitmap 与 code generation，在下次 `prepare_unicorn` 消费。SMP fallback 已取得 writer lock 并停止其他执行器。hook 返回 0，继续 Unicorn 原有系统指令处理；window 不注册该 hook。每 CPU 新增两个计数，记录接收的 IC 操作和成功取得物理页的失效次数。

未建立 PTE 的有效执行映射由后续 Linux instruction fault 安装页表项，现有 `changed_code` 处理物理页失效。`unpopulated_exec_alias` 在 RX 别名没有 load/fetch 的条件下先执行 cache maintenance，再首次调用成功。`exec_only_vma` 验证 `mprotect(PROT_EXEC)` 后执行成功；当前 LKL `arch/lkl/mm/mmu_mem.c:14` 将 `VM_EXEC` 转为 `PAGE_READONLY | _PAGE_EXEC`，其 PTE 可读，现有 hook 可以查询。严格 execute-only 数据隔离尚未实现，无效或不可访问 cache operand 的硬件异常语义仍未验证。

27 个用例包含同一 fallback 执行区间内的 alias 更新、非零文件页偏移、内核 `pwrite`、此前只由 Unicorn 执行的代码页、private fork/COW、shared fork、mremap 移动与共享克隆、同 VA 更换 backing，以及 RX 写入、撤回执行权限、解除映射后的 signal/si_code/address。`unicorn_self_page_flush` 把 cache maintenance 与目标代码放在同一物理页；更新目标后执行 `IC_IVAU`、`DSB`、`ISB` 再跳转，返回新值 67，覆盖失效当前 TB 所在物理页的情况。noexec tmpfs 的可执行 `mmap` 实际返回 `EPERM`，已有 READ 映射新增 X 返回 `EACCES`；只读 fd 的共享可写映射及 memfd seal 同样保留 Linux 拒绝。

以下每组均运行 CLI 与 GCD，exit 0 且完整 core validator 成功。七组 PTE 各入口都通过 27 个标准 JIT 用例；window 保留旧 `linux_dynamic_code` 的 128 次专用接口更新检查。

| 产物目录 | 本次配置 |
| --- | --- |
| `jit-low-complete` | PTE、Direct/flat、low、单 CPU、code-watch/retained routes/route epochs 开启 |
| `jit-host-complete` | 同上，guest layout 为 host |
| `jit-unicorn-complete` | PTE、纯 Unicorn、host、单 CPU |
| `jit-no-retain-complete` | PTE、Direct/flat、low、单 CPU，code-watch 与 route epochs 开启，retained routes 关闭 |
| `jit-no-watch-complete` | PTE、Direct/flat、low、单 CPU，code-watch/retained routes/route epochs 均关闭 |
| `jit-fastpath-complete` | low 单 CPU，code-watch/retained routes/route epochs 开启，`linux-fastpath=1`、exec-cache/TLB 为 pages、timer 抢占 |
| `jit-smp-complete` | PTE、Direct/flat、host、两 CPU、code-watch/native-exclusive 开启，exec-cache 为 pages、timer 抢占、retained routes 关闭 |
| `jit-window-complete` | window、纯 Unicorn、host、单 CPU，复用现有原生内核产物 |

`test_standard_jit_report.py` 对 14 组真实 PTE 运行记录逐一删除/重复记录、删除或改变字段、将整数换成布尔值、追加失败行，合计拒绝 4774 份变异输入。`test_translation_flat.py`、三个修改脚本的 `py_compile`、四个 C 源文件格式检查与 `git diff --check` 成功。Direct 进入次数、fallback 次数和 IC 计数是整个 core 的汇总；用例代码控制调用前的 syscall/fallback，当前报告没有逐函数执行器占比。

独立 ARM64 ELF 与 core 使用同一 `jit-guest.c`。独立 ELF 完成编译与反汇编，实际运行来自 core 集成入口。iOS 只使用 iPhoneOS 27.1 SDK、`arm64-apple-ios26.0` 目标，分别编译 1/2 CPU 的 `linux-execution.c` 对象；未链接运行库，也没有设备执行。原生 NCE Android、完整 Direct Android、ART JIT 与原始 Java/JNI APK 尚未在本次普通 VM 变更上验收，由主会话合并后统一重建。

### 主工作区集成检查

2026-11-12 合并标准 VM/IC 修复与原始 Bionic 探针，解决 `probe_lkl_translation.py` 的工作负载分支冲突。core/PTE 运行标准 JIT fixture，bionic 使用独立 supervisor，Android 工作负载继续使用完整系统输入。主工作区 low、单 CPU、Direct/PTE/flat、dual、128 MiB、timer、pages、code-watch、跨页与原生独占配置的 CLI/GCD 均成功，各有 62 条成功检查，其中标准 JIT 27 条。报告为 `work/native-linux-nce/standard-jit-integrated-core/summary.json`，102210 字节，SHA-256 `ef7941d3b65622a6ba7c5164e7952f9ba1a07639c1f92a8018643d26330a378c`。本轮与原生构建并行，只验证功能；原始组件基线保持原构建含义。

同时合入 Settings 首次打开采集，需要重新生成完整 Android 镜像。首轮原生命令误设置 `NCE_DESKTOP=1`，进入持续交互模式，另启用 nterp 并使用新数据盘；Android 未在 240 秒轮询窗口完成启动，framework helper 以 109 退出，数据盘只读卸载成功。首轮失败的 JSONL、控制台及产物摘要保存在 `work/native-linux-nce/native-standard-jit-profile-failed/`，GCD 未执行，该目录外已有 GCD 日志来自上轮。该结果没有确认源码回归原因。后续改用 `NCE_SYSTEM_SERVER_PROBE=1`、清除 `NCE_DESKTOP`、沿用原生基线的 nterp=0，在新的数据盘执行有限桌面回归；Direct 的 nterp=1 完整 Android/APK 检查随后进行。

第二轮有限桌面检查也未在启动窗口内完成。实际输出为 `linux_mapping_policy.mode=lazy`，命令漏设 `NCE_MM_MODE=demand`，与此前成功原生基线不一致；首轮同样未指定该变量。第二轮 CLI 结果、控制台与产物摘要保存在 `work/native-linux-nce/native-standard-jit-profile-failed-v2/`，GCD 未执行。独立 2 秒宿主采样位于 `work/project-audit-2026-11-12/native-standard-jit-v2.sample.txt`，包含 `vm_remap`、`mmap`、`mprotect`；该采样不足以单独解释整个启动耗时。第三轮显式设置 `NCE_MM_MODE=demand`，保持有限回归、nterp=0 和新数据盘，重新核对实际映射配置后继续。没有延长原有启动时限。

第三轮原生 NCE 完整 Android 回归已成功。实际配置为 `demand`、nterp=0，CLI/GCD 各完成 225 次成功检查，包含 Launcher、Clock、Files 的实际画面、时钟数据与 Keystore 重启恢复，以及数据盘只读卸载。报告原位置为 `work/native-linux-nce/processes/lkl-summary.json`，本阶段结束后连同 CLI/GCD 原始日志移动至 `work/native-linux-nce/native-standard-jit-profile-passed/`，1,227,936 字节，SHA-256 `83aaef41304c37d7f5711ba8d2c776e074e5cc2e6cbff4ba9cf9a8a63212cd4e`；系统镜像 SHA-256 `8dc537809d7f76798fec1b690ca78857dbe206c68a11d18be8f0cec924aadff3`，190 个源码输入均在报告中。iOS 核心交叉编译与现有用户态检查成功。此次完整执行与其他组件实验并行，只验证功能。Direct host 布局、nterp=1 的完整 Android/APK 回归继续运行，使用新的独立数据盘。

Direct 完整 Android/APK 首轮在安装助手处失败，未运行 GCD。CLI 已通过 Launcher 画面，Java v1 的原始 APK 检查成功；日志确认 JNI v1 原库加载并返回 101，随后 Android 配置更新重建 Activity，同一打开请求输出 `launch_count=1` 与 `2`、相同 marker 的两条结果。`NceApkInstall.application()` 抛出 `Duplicate application JSON after launch`。本轮保留在 `work/native-linux-nce/standard-jit-profile-desktop-apk/`，该结果不能作为完整 APK 回归成功。安装助手的 Activity 生命周期假设由独立修复处理；本阶段继续用已正常卸载的原生回归数据盘检查 Direct 完整桌面，以生成 Settings 采集所需的 App。该桌面轮次未运行 APK 安装助手，APK 验收继续保留失败状态。

Direct host、单 CPU、PTE/flat、dual 128 MiB、nterp=1、timer、pages、code-watch、跨页与原生独占的完整桌面 CLI/GCD 随后成功，各完成 58 条检查，创建与释放的上下文分别为 1003/1003 和 1004/1004，数据盘均正常卸载。该轮未运行 APK 安装助手。报告为 `work/native-linux-nce/standard-jit-profile-desktop/summary.json`，1,218,579 字节，SHA-256 `f24798750c261625dbe997266a3459684de935681ad0fd2c792923664ec3c68d`。

根据这份完整桌面报告生成 macOS 构建 `0.25.0-Mac-20261112-042712-direct1-dual` 和 iOS 构建 `0.25.0-iOS-20261112-042848-direct1-dual`。IPA 为 `work/native-linux-nce/ios-direct-1/DroidOnDarwin-0.25.0.ipa`，476,096,176 字节，SHA-256 `7f51fef3d43997a7618b534359d10821e0e330c6f9342941e5ff1e21f2c7a53b`；该生成路径中的旧包已被本次构建替换，旧包需按原记录重新构建。iOS 未安装或执行。Mac Settings 首开采集在窗口可见性检查处失败，未取得有效帧或 trace，详见[交互记录](../testing/2026-10-11-交互卡顿测量与优化流程.md#本次集成与可见采集边界)。

[主工作区集成归档](../../../../experiments/native-linux/results-standard-jit-integration-20261112.json)保留 core、原生 Android、Direct 桌面、APK 失败、两端包清单与可见性失败的配置和摘要。原始 APK 的 Activity 重建检查由独立修复继续处理。本阶段交付标准 VM 和指令缓存修复；完整原始 ART、HyperOS、实际交互性能及 iPhone 执行继续验证。

## 风险

本机 core 已验证标准动态代码与本次缓存修复，完整 Android 桌面已完成两入口功能检查，标准 JIT 在真实 APK 应用域中的执行及复杂并发写入仍需后续验证。单核和 SMP 的通知生命周期不同；本次两 CPU core 包含共享代码更新与 COW，但没有覆盖所有线程调度和并发发布顺序。

普通文件映射不再强制注册地址，当前适配 linker 的 245 路径仍保持原地址来源。musl 动态装载、CFI、ART 及 sigreturn 必须随本阶段检查，故障按实际调用区分，不通过恢复普通 mmap 的全局 ELF 限制来跳过问题。

现有 16 KiB 客体页、ELF 段对齐、CPU 指令覆盖、Android SELinux 与应用域 seccomp 仍会影响 JNI/JIT 兼容。root core 探针只验证相应 Linux 配置下的接口与执行行为；真实应用进程必须另测。完整 HyperOS、未修改 ART 与 iOS JIT 条件继续按各自阶段验收。
