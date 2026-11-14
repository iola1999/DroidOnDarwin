# 决策记录: LKL 通用 ptrace 请求

状态: implemented

## 问题

原 SDK v10/v11 的 `crash_dump64` 在 GETEVENTMSG 和 INTERRUPT 上返回 `EINVAL`。实际 LKL 的 `arch_ptrace()` 无条件返回 `-EINVAL`，通用 `ptrace_request()` 已有对应实现。四份原日志分别保存 374、454、444、359 次失败调用和 66 个样本；Android 的 GETEVENTMSG 失败位置已完成 clone 事件的 wait 检查。原服务的 SIGABRT、keystore2 的附加权限错误、原始崩溃 PC 缺失与转储失败分别记录，详见[只读调查](../../../../../../../sdk-plain-validation-v1/crash-dump-audit-v1/report.md)。

修复从 `b1fdf1b` 的独立工作树开始，完成原内核与候选的 CLI/GCD 比较、初始停止回归、消融和来源审阅后，按逐文件摘要集成到 plain-repo 候选。原 v10/v11 输入和完整 boot 失败保持。阶段提交与推送沿用主会话的离线开发授权。

## 决策

在现有 `lkl_nce_patch.py` 中为生成的 asm/ptrace.h 增加 `ptrace_request()` 前置声明，将 `arch_ptrace()` 转交通用实现。继续沿用 syscall 入口的 SEIZE/ATTACH、凭据、SELinux、停止状态与生命周期处理。新内核使用 APFS 克隆的实际 LKL 源码和原配置，编译和输出放在独立目录。

独立 `ptrace-guest.c` 通过现有 translation CLI/GCD loader 执行。同一 guest 文件分别运行原内核和候选内核，原失败继续保存。回归检查 SEIZE/INTERRUPT/CONT、TRACECLONE/GETEVENTMSG/DETACH、纯用户循环的停止与恢复、阻塞 read 的恢复和进程回收。原配置未启用 HAVE_ARCH_TRACEHOOK，本轮显式检查 GETREGSET/SETREGSET 的当前失败返回；ARM64 regset 留作后续独立范围。

通用转发候选的 CLI/GCD 回归已发现 clone 初始停止错误。`ptrace_init_task()` 给自动附加的子任务设置 `JOBCTL_TRAP_STOP`，LKL 的 `user_bootstrap()` 直接调用 `user_run()`。子任务执行首个共享变量写入后，在 `getpid` 的返回处理中才停止。主会话已授权在 `user_bootstrap()` 中先调用现有 `nce_user_return(-1, 0, 0)`，然后进入 `user_run()`。负 syscall 参数沿用抢占与异常处理入口的用法，继续保留 clone 的 x0、PC、SP、TLS 和信号状态契约。

实际生成源码的 `copy_thread()` 只在 `user_clone()` 成功且 `p->thread.nce_state` 非空后选择 `user_bootstrap()`，该入口位于 CONFIG_MMU 分支，`task_pt_regs()` 直接返回 `nce_state`。这里沿用非空契约。launcher 的 `nce_launch()` 使用独立的 `user_start()`，该入口设置状态后直接调用 `user_run()`，此次变更保留该路径。

## 曾考虑的替代方案

- 仅在生成 execution 中更改 ptrace 返回值：无法建立内核停止、事件消息与回收语义，未采用。
- 重写 GETEVENTMSG、INTERRUPT 和 clone 事件：通用内核已经提供这些实现，新增重复状态没有必要。
- 同时增加 ARM64 regset：涉及公开寄存器布局、FPSIMD、guest TLS 与写入校验，本轮先验证已有通用请求。

## 验收标准

1. 原 SDK 调查和同一 guest 的原内核失败均保留，来源摘要前后保持。
2. CLI/GCD 都验证纯用户循环和阻塞 read 的 INTERRUPT、wait、CONT 与 DETACH；停止期间目标任务不得继续运行。
3. TRACECLONE 的 GETEVENTMSG 返回实际新子 PID，子任务停止、恢复、退出和原父任务回收均成功。
4. GETREGSET/SETREGSET 保留当前未实现的失败返回，权限与无效目标错误继续由原 syscall 入口产生。
5. 新源码与实际编译依赖对应；工作树 diff、结构化结果和清理清单可供主会话审阅。完整 boot、桌面、APK、iOS 执行及持续性能继续分别验收。

### 复杂度审查

实际使用方是原 Android crash_dump64 和独立 guest 回归。通用请求继续使用 task 的 ptrace、jobctl 与 ptrace_message 状态，没有新增运行期状态或执行线程。移除转发后，上述两个实际请求恢复到 EINVAL；新回归检查真实内核停止和 clone 消息，覆盖该行为差异。新增测试 driver 只负责生成同一 guest、调用现有 loader、核对结果与记录来源。

初始停止修复复用现有信号处理函数，不新增状态或重复 ptrace 事件实现。移除该调用后，通用转发候选的两个入口都会在停止前写入共享变量；回归保留这一实际失败。`user_bootstrap()` 只执行新用户任务，宿主 syscall 任务与普通内核线程继续使用原入口。

## 本机验证

通用转发候选使用原 LKL 源码的 APFS 克隆，五条内核构建命令完成。首次配置严格比较发现唯一差异 `CONFIG_RUSTC_LLVM_VERSION=220100`，默认 PATH 选择了 rustup nightly；改用 `/opt/homebrew/bin/rustc` 的 LLVM 22.1.6 后，`.config` 与原文件完全相同，SHA256 为 `a813b734f7d7f6ae8c6ef82cb11381712976432e23a262c76b9a8702254a05a9`。原构建输入前后摘要保持。首次测试 driver 使用不存在的 LLVM 目录内 `ld.lld`，在编译阶段失败，未启动 guest；后续使用实际 `ld.lld` 路径。

同一静态 musl guest 经现有 loader 的 `bionic` 分支执行，该分支提供实时日志并省略 core 专用的 host pointer 检查。准备所需文件使用原 core 运行库与 SDK runtime 的 APFS 克隆，运行程序为 `/probe-guest`。两个内核的 CLI/GCD 均从本工作树的同一组 C 源码重新编译，Direct 对象和 Unicorn 库只读复用原 core；实际 `.d` 核对各自 `lkl-image.h`。配置为 Mac ARM64、单 CPU、16 KiB guest/host page、Direct、flat、sparse、data TBI、dual code memory、2 ms timer preemption、BPF JIT。

原内核的 CLI/GCD 均复现两个 INTERRUPT 和一个 GETEVENTMSG 的 EINVAL。通用转发候选的两个入口均通过纯用户循环和阻塞 read 的两次停止、CONT、DETACH 及回收，用户 read 最终返回 `1`、errno 为 `0`。GETEVENTMSG 返回实际 clone PID `5`，无效输出为 EFAULT，已停止任务的 GETREGSET/SETREGSET 返回 EIO，寄存器输出保持。候选两个入口的 `clone_stopped_before_user_write` 均失败，值为 `1`；其余已经执行的检查成功。四轮宿主进程组、guest 子任务和 BPF code pool 清理均成功，输入摘要保持。结果保存于本轮 `generic-only-attempt02/report.json`。

无条件初始返回候选在 `bootstrap-attempt02` 的 CLI/GCD 启动阶段失败，宿主退出 `-6`，guest 尚未输出检查，stderr 保存 `translation:837: copy_user(storage, address, sizeof(storage), 1) == 0`。随后加 SP 条件的候选在 `clone-entry-attempt01` 的两个入口退出 `-11`。回归 driver 克隆了原 core 的 `translation-symbols.h`，其中 COPY_USER 和 LOOKUP 偏移分别为 `0x1175b8`、`0x1152a0`；新增代码后，无条件候选的实际偏移为 `0x1175c8`、`0x1152b0`，SP 条件候选为 `0x1175dc`、`0x1152c4`。LLDB 在后一个候选的 `user_run()` 启动参数复制处确认错误跳转进入 `nce_access_ok()` 中间位置，`user_bootstrap()` 尚未执行。原始失败与调试输出保留。此前对初始 SP 和信号准备的原因推断撤回，SP 条件没有进入最终变更。

driver 现为每个内核从实际 `lkl.so` 生成 COPY_USER 与 LOOKUP 偏移，并核对 `lkl-image.h` 中全部宿主函数偏移。合计 16 个入口均与 ELF 的可执行 segment 及镜像首 64 字节对应，实际 `.d` 同时核对内核 ABI 头和当前候选的 `translation-symbols.h`。原基线生成头须与历史 core 头完全相同。增强 guest 首次在 ptrace 停止前输出 TLS 检查，输出只写入半行后停止，原始失败已保存；后续把输出放在首次 `getpid` 返回之后，初始共享变量写入检查继续保持。

原 core 的固定 `scripts/probe_lkl.py` 在其他阶段更新到 `c1b9616` 后，`bootstrap-attempt03` 的准备检查按原摘要停止，尚未启动 guest。主会话确认该更新仅修改准备与构建阶段的输入时间检查；其余原输入摘要保持。本轮通过显式 `--baseline-build-revision b1fdf1becbbba5f62e6fb8184d105a176e26814b` 核对该历史构建脚本的 Git blob，SHA256 仍为原 `0e0cf5c8b31281c28903ca01156879bf4fa68c6f26212ef10e350311e20471cc`。报告同时记录当前文件摘要与提交来源，原 core summary 保持原内容，全部实际编译及运行输入继续严格核对。

修正偏移后的 `bootstrap-attempt04` 四轮验证成功。原 CLI/GCD 各保留两个 INTERRUPT 和一个 GETEVENTMSG 的 EINVAL；无条件候选的两个入口通过初始停止、TLS/SP/x29、继承信号 mask、停止期间加入的阻塞 SIGUSR1、CONT/DETACH、GETEVENTMSG、阻塞 read、regset 当前 EIO 及全部回收。子任务的首次用户共享写入值在停止处为 `0`，恢复后退出 `43`，原父任务 wait 成功后退出 `44`。宿主无超时、额外清理信号或进程组残留，guest 子任务和 BPF code pool 清理成功，输入摘要前后保持。

`generic-only-ablation-v1` 用同一最终 guest 和保留的通用转发宿主再执行 CLI/GCD。两个入口仅在 `clone_stopped_before_user_write` 失败，值为 `1`；已执行的其他检查及清理均成功，输入摘要保持。移除初始返回调用的实际行为与原复现一致。`final-source-replay.json` 记录从固定上游 `d0f76a77e8c2ca68bd7d96190b8da09040eab072` 的 APFS 克隆重放最终补丁；连续两次 NCE patch 后，ptrace 与 threads 文件摘要相同，bootstrap 定义与初始返回调用各一处，原输入保持。

2026-11-15 的 `final-kernel-build.json` 记录最终重放源码的五条构建命令、实际 threads/ptrace 对象及依赖文件摘要，配置与原输入保持。最终 `lkl-image.bin` SHA256 为 `7a0d8977ac60369782cb5a4ea92643070777090b4fe528c5a698cee3e31be77d`，与最小无条件候选相同。`final-cli-gcd-v1/report.json` 的四轮严格回归成功，使用同一最终 guest，原 CLI/GCD 退出 `1` 并仅保留预期的三个 EINVAL，候选 CLI/GCD 退出 `0` 且全部检查成功。16 个函数入口、实际源码依赖、进程组、guest 子任务、BPF code pool 和输入前后摘要均核对成功。完整 boot、桌面、通用 APK、iOS 运行及持续性能尚未由本轮验证。

### 标准 processes 与 iOS 集成

从 `c1b9616` 和已集成的修复建立独立工作树，执行 `python3 -B scripts/probe_lkl.py --processes --storage-image work/native-linux-nce/ptrace-processes-check-v1/data.img`，退出 `0`。CLI/GCD 各 204 项检查成功，GCD 包含 67 个继承信号屏蔽线程；ext4 的持久代次从 1 推进到 2，关闭后状态为 clean。`arm64-apple-ios17.0` 的三份宿主对象编译成功，两份宿主对象的独立依赖复编译摘要相同。该检查使用标准 processes 的独立构建，内核镜像 SHA256 为 `f4ce2896538befdd036ed098193b57c370dd1a2904d89b918f320602def05387`，配置 SHA256 为 `56ad2ebe2e5d3ec1d199b5669c792dc216951d71f33ce9d70a7a36bde59015f4`；与前述原 SDK 配置的专项回归分别记录。

822 项实际编译输入、199 项仓库编译输入、14 个内核导出偏移、实际宿主 `.d` 和内核 `.cmd` 均核对成功。两个宿主程序从实际链接输入重新链接，指令区摘要相同；内核从原链接对象重新链接，ELF 摘要相同。外层 driver 的 `protected_inputs_unchanged=false` 原值保留，唯一变化是主会话更新的 v12 结果 JSON，运行前后摘要与该文件不参与实际编译的检查分别保存。原结果与两份独立复查报告保持，详细来源见[阶段结果](../../../../experiments/native-linux/results-lkl-ptrace-integration-20261115.json)。

## 影响

原 Android crash_dump64 使用的通用 ptrace 请求可以进入内核现有实现，自动附加的 clone 子任务在执行首个用户指令前处理 ptrace 停止和待处理信号。该阶段没有新增 ptrace 状态或宿主执行线程。GETREGSET/SETREGSET 的 EIO 边界继续保留，ARM64 公开寄存器布局另行调查；完整 SDK 在集成此修复后的启动、桌面、APK、HyperOS、iOS 执行与持续性能继续验收。

## 风险

通用转发的成功返回需要实际停止与恢复检查支持。NCE 在 syscall 返回和用户代码抢占处处理信号，阻塞恢复与纯用户执行均需覆盖。当前 regset 失败会影响完整多线程转储，原 SDK 服务自身的崩溃仍需按各自原因继续处理。真机资源、iOS 执行、JIT 授权、性能及热状态继续待真机验证。
