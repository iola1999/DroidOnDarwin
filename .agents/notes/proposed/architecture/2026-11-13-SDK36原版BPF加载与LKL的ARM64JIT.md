# 决策记录: SDK36 原版 BPF 加载与 LKL 的 ARM64 JIT

状态: proposed

## 问题

无加密、16 KiB 的原 PID 1 已完成 first-stage 与 ART 编译，原 NetBpfLoad v0.47 打开 `/proc/sys/net/core/bpf_jit_enable` 返回 ENOENT，并按原 `reboot_on_failure` 退出。当前 LKL 配置具备 CONFIG_BPF/CONFIG_BPF_SYSCALL，arch/lkl 缺少 eBPF JIT。Android U+ 写入 JIT enable/kallsyms，v0.47 还检查每个已加载程序的 `jited_prog_len > 0`。

固定 Linux/LKL 来源为 `d0f76a77e8c2ca68bd7d96190b8da09040eab072`。原 NetBpfLoad 来自 `android-16.0.0_r1`，来源 manifest 为 `experiments/native-linux/services/network-sources.json`，源码 SHA256 为 `03d6adf9bf499cc23ef170dafa23a86c8a5ecb1e2c182dea940c0cdfba650254`。调查报告和原对象清单位于主工作区 `work/sdk-plain-validation-v1/bpf-jit-plan-v1/`。

旧提交 `6fcb003` 已验证预备 RX/RW 执行池。旧检查点 `7c3cb08` 的 ARM64 接入仅构建成功，CLI/GCD 原 callback 探针均收到 SIGSEGV；原 NetBpfLoad 尚未运行。该检查点保留子程序失败后的 RW buffer 与 offset 泄漏，故障因果关系尚未建立。

## 提案

2026-11-13 主会话已选择实际 ARM64 后端，并授权在大小写敏感卷的独立 `codex/sdk-plain-bpf-jit` 工作树实施。起点为 `3a4d470`，plain-repo 保持只读。原 NetBpfLoad、BPF verifier、SELinux、原重启规则继续作为验收输入。

选择性导入 `6fcb003` 的 `bpf-code-memory.c/h`、宿主 fixture 与 probe。现有 code-memory、execute-check 和 ios-jit 声明在旧提交和当前主线中内容相同。复用固定源码的 ARM64 BPF 编译器、指令编码实现和必要头文件；宿主用户地址用固定四条 MOVZ/MOVK，保留 Darwin x18；LKL helper 通过真实通用 helper 调用，原子操作使用 LL/SC。内核与宿主使用同一份生成 host_ops。

宿主在 debugger detach 和 kernel startup 前准备执行池。Linux 保留 pack、程序、map、RCU 和 deferred work 生命周期，编译缓冲使用普通 RW 内存。最终代码经 RW alias 复制并刷新两个 alias 的缓存，再从 RX 地址调用。宿主锁不提供正在执行代码的同步；Linux 必须在覆盖或回收前停止相关执行。内核停止且分配全部归还后解除映射。

修复 `bpf_jit_free()` 的未完成子程序路径，释放保留的 offset，并向 pack free 传递保留的 RW header。重复触发后续子程序 JIT 失败，核对 kernel 计数与宿主分配归还。旧 SIGSEGV 先保存新的函数、寄存器、回溯与 RX 指令，再按实际原因修复。

构建器保留当前源码隔离、同卷 APFS 克隆、ASLR、块设备配置与来源哈希检查。单 CPU、16 KiB core 使用 `--reuse-engine-build`，共享 macOS/iOS Unicorn 来源保持只读；内核/source/build 均为独立目录。增加已确认的 CONFIG_BINFMT_SCRIPT，继续采用 plain 核心关闭数据加密的配置，其余内核选项由真实消费者决定。

### 复杂度审查

执行池分配记录由宿主管理，BPF pack 与程序引用由 Linux 管理，两者保护不同的生命周期边界，继续保留。五个 host_ops 对应分配、回收、alias、发布与容量，后端分别有直接调用方。复用上游编译器和通用 BPF core，不新增指令执行器或程序注册表；普通原网络对象阶段保留必要 JIT 指令与 helper，不先扩展 ARM64 内核 tracing/arena/kfunc。

删去原 JIT 会恢复当前 loader 的 ENOENT 或零长度失败；删去 alias/publish 会失去宿主 W^X 与缓存发布保证；删去 Linux pack/RCU 会破坏并发释放契约。每项保留均对应实际消费者与失败行为。旧 4 KiB callback 接入仅作为参考，当前构建器、运行器与无关测试继续按现有职责组织。

## 曾考虑的替代方案

仅注册 JIT sysctl：原 NetBpfLoad v0.47 仍会拒绝零 JIT 长度。修改原 loader 或启用 prototype：改变当前原系统验收输入。直接整体导入 `7c3cb08`：其运行失败和释放缺陷尚未解决，且会覆盖当前构建器与运行器的后续修复。先处理 4 KiB ringbuf：原 loader 已按实际页大小扩大 map，可先验证 16 KiB 的真实 JIT。

## 验收标准

- CLI/GCD 执行池的五参数调用、权限故障、独立范围更新、耗尽、归还与完整解除映射通过。
- 单 CPU、16 KiB core 的原有检查全部通过，配置具有真实 HAVE_EBPF_JIT/BPF_JIT；原 sysctl 可写 1。
- 算术、普通 helper、map、32/64 位原子指令、BPF-to-BPF 实际执行返回预期结果，BPF_OBJ_GET_INFO_BY_FD 返回非零 jited_prog_len。
- 后续子程序 JIT 失败多次复现并回收先前 RW buffer、offset 与 pack，kernel shutdown 后宿主池无活动分配。
- 原网络 `.o` 按原 metadata 与 map relocation 装载，保存原 helper/opcode、实际 skip/load、verifier 结果及 JIT 长度；原 type27 ringbuf reserve/submit 行为通过。
- callback 合法参数、继续/停止、非法返回及非法访问按上游 verifier 契约检查。
- 使用通过的源码与生成 host_ops 完成 iOS 对象/runtime 编译。结果记录 SDK、源码和输入哈希。
- 主会话在 plain-repo 完成原 PID 1 与原 NetBpfLoad 整合，原失败和重启规则保持原值。

实施前状态为尚未验证。阶段完成后追加实际结果、偏离和未覆盖条件，并由主会话 review 后提交。

### 两种页大小的本机结果

原 `bpf-jit-core16k-v2` 已完成 CLI/GCD，原有 core 检查及执行池关闭成功。该客体未发起 BPF 系统调用，结果仅覆盖内核接入和原 core 行为。随后使用同一宿主和独立 `NCE_TRANSLATION_FILES` 输入运行真实 BPF 程序；`bpf-jit-syscall16k-v1` 的算术、普通 helper、array map、32/64 位原子操作和 BPF-to-BPF 均返回预期值，每个程序的 `BPF_OBJ_GET_INFO_BY_FD` 都报告非零 `jited_prog_len`。两项原 JIT sysctl 的写入及读回成功，宿主退出、进程组、客体上下文和执行池回收成功；到该轮结束，原 core 的 144 项输入摘要保持。

审查发现第二轮子程序的多个错误出口会保留已释放的 `bpf_func`、`jited` 和 `jited_len`，通用 finalize 发布失败还缺少 charge 归还。当前生成后端在统一错误出口清除这三个字段，offset 和 jit_data 继续由原出口释放。通用 core 在 finalize 前保存 RW header 的分配大小，发布失败据此释放 pack 和归还 charge；尚未 finalize 的 free 直接使用 RW header 的大小，省去错误清理中的额外代码发布。固定 17 项 BPF 基础来源在复用检查之前校验，基础输入和实际生成文件进入既有构建记录；生成文件与记录不符后重新准备独立源码。

上述修改后的 `bpf-jit-core16k-v3` 和 `bpf-jit-core4k-v1` 均完成原 core 的 CLI/GCD，实际来源摘要保持。`bpf-jit-syscall16k-v2` 和 `bpf-jit-syscall4k-v1` 使用这两份宿主，完成九个程序的实际加载、JIT 长度和返回值检查。新增 type27 ringbuf 在 16 KiB / 4 KiB 客体上分别使用 65,536 / 16,384 bytes，原 reserve/submit helper 生成 16-byte 记录，consumer/producer 位置和 payload 符合预期；producer 映射的写权限请求返回 EACCES。两入口全部关闭成功，执行池无活动分配。该轮覆盖单次 reserve/submit，原网络对象、完整 loader、callback、第二轮发布失败及 pin/attach 生命周期继续验收。

真实客体来源为 [bpf-jit-test.c](../../../../experiments/native-linux/userspace/bpf-jit-test.c)，运行和结果检查由 [probe_lkl_bpf_jit.py](../../../../scripts/probe_lkl_bpf_jit.py)完成。该探针复用已通过且输入保持的 core 宿主，运行前后核对来源；运行期程序文件使用只读引用。Mac 为 Apple M5 Ultra、macOS 27.0（26A428），Direct/PTE/flat/sparse/TBI/dual、单 CPU、1 GiB 客体 RAM 和 128 MiB 翻译缓存。功能检查的 wrapper 用时包含准备、编译和关闭，不作为 Android 启动或应用性能。16 KiB 的 iOS 运行库已编译成功，128 项输入摘要保持；4 KiB 编译正在进行。手机运行和热状态仍待真机验证。

### 错误回收的后续验证

错误回收使用独立的诊断内核和宿主目录，BPF verifier、原权限及已有盘保持。普通两函数的 JIT 失败可以由原 verifier 回退到解释器，加载仍可能成功；检查零 JIT 长度、实际解释执行返回 42、charge 和 RW/offset/pack 归还。合法 callback 的函数地址不能由解释器执行，需要按原 verifier 的加载拒绝契约检查。正常路径成功尚未覆盖这些条件。

审查发现，统一错误出口对首次 JIT 失败也清除 `bpf_func`。固定 `kernel/bpf/core.c` 的 `bpf_prog_select_runtime()` 先选择解释器，再调用 JIT；首次失败后不会重新选择函数。当前修正仅在 `extra_pass` 清空上一轮的 JIT 字段，首次尝试保留原解释器入口。该修正需要单函数与两函数的实际发布失败回归，原成功路径也按最终来源重新验证。

### 停止后的执行池释放

`work/sdk-plain-validation-v1/bpf-pin-lifecycle-v1/summary.json` 的四次真实运行确认了 pin 的引用行为：删除 pin、关闭重新取得的 FD 并卸载 bpffs 后，CLI/GCD 的执行池释放成功；保留 pin 直到 `lkl_sys_halt` 后，两个入口的池释放均返回 `-1`、`errno=16 (EBUSY)`。四次宿主进程组为空，客体上下文归还成功，198 项输入摘要保持。

固定 halt 清理内核线程和 RAM，没有逐项释放 bpffs、attachment 和待处理 BPF 引用。原 init 在独立的 private mount namespace 中运行，宿主卸载 `/sys/fs/bpf` 不能覆盖其全部引用。当前给既有池释放函数增加显式终止参数：普通释放继续拒绝活动分配；内核与调用方停止后释放整个映射和分配记录。调用位置保持原 `halt → cleanup → translation_linux_cleanup` 顺序，随后才释放 BPF 池和 kernel image。运行期间的 pin、map、attachment 与 RCU 行为保持。

固定内核的三个停止入口忽略 `thread_join` 返回值。宿主 `thread_join` 改为要求真实 `pthread_join` 成功；失败触发既有 REQUIRE，观察器保存实际失败和回收结果。halt 前的 logger 与 stop peer 已 join，timer 取消使用串行队列 barrier 排空回调。halt 后外部 syscall、IRQ 或已进入的 TLS 析构回调仍可能执行 kernel 入口指令，kernel image 的释放继续要求全部宿主调用线程结束。

上述接口复用现有分配记录，不增加 pin 注册表、另一份分配计数或停止状态表。宿主池检查覆盖普通 EBUSY、终止释放、两份 alias 解除映射和再次准备；真实 pin fixture 使用最终 core 复验，首次发布失败另用独立诊断内核。完整原 loader 与系统启动仍待验证。

## 风险

旧 SIGSEGV 根因未知。宿主 alias 的并发写入与执行依赖 Linux 同步，池容量与延迟回收需实际负载确认。原对象的 static section 清单包含 loader 可能跳过的版本变体，verifier 也会产生额外指令。ARM64 内核 exception/tracing/kfunc/arena 依赖需逐项确认，设备授权、持续性能和热状态保留真机待验证。

### 2026-11-14 接续验证

plain 的 `sdk-plain-bpf-core16k-v2` 重新完成 CLI/GCD 核心与来源检查；`bpf-jit-takeover16k-v1` 的九程序、ringbuf 与全部回收成功。`bpf-pin-lifecycle-takeover-v2` 中删除 pin 与保留 pin 的四次真实运行均退出 0，终止池释放均为 0、errno 0，程序长度为 56、实际返回 42；207 项输入前后保持。join fixture 的 GCD 返回值修复及六项结果见[线程 join 回归](../../implemented/bug-fix/2026-11-14-LKL线程join回归的GCD返回值.md)。

[发布失败回归](../bug-fix/2026-11-13-BPF首次JIT失败的解释器入口.md)已完成十入口的普通与发布失败、解释执行和 charge/宿主池检查。原 init 的 v12 已执行后续服务启动，但因通用诊断日志限额停止；完整 NetBpfLoad、boot、callback 和全部编译缓冲回收继续验收。当前探针的 logger、stop peer、内核和抢占线程的停止顺序已审阅；产品中的外部 UI/GCD 线程与已进入的 TLS 析构仍需独立并发停止回归，不能仅凭 pool mutex 或内核线程 join 推定这些调用已结束。

### 原加载器成功与完整启动验收失败

最终 v13 的 lifecycle、original、同盘 recovery CLI/GCD 共六个入口通过各自观察、停止与回收检查，原 first-stage 完成，两盘最后 clean。CLI 原 NetBpfLoad v0.47 报告 success，bpfloader 退出 0；完整 boot 验收为 failed。原 SurfaceFlinger 的 SIGABRT 及后续 Zygote/netd 重启继续调查。结构化结果见[接续阶段结果](../../../../experiments/native-linux/results-sdk-plain-bpf-takeover-20261114.json)，完整报告按清单压缩保存。

iOS runtime 的 129 项输入与库摘要保持；实际 v13 的 runner/execution 两份 arm64 Mach-O对象编译成功，编译前依赖预扫描和编译后 159 项输入核对保持。上述结果覆盖本机功能和交叉编译，当前没有整机提速或手机运行结论。
