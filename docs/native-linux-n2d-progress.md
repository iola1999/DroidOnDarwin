# N2d：Linux 信号与实际用户态程序

更新日期：2026-10-09。状态：本地主线程与 GCD 入口通过，已合入 N2-E1。
后续动态链接实现和集中设备验证见 [N2e 记录](native-linux-n2e-progress.md)。

## 目标与交付条件

在 N2-C1 已通过的 iOS 执行条件上实现 Linux 信号、异常返回、系统调用
中断和重启，扩大可运行的实际 Linux 用户态程序。功能与失败场景先通过
本地主线程及 GCD 入口回归，形成完整阶段产物后再安排设备测试。

当前 goal 追踪完整 Linux / Android 在 iPhone 内的运行与可用性。暂停条件和当前验收要求见
[Android 阶段目标](native-linux-android-stage.md)。过程中的局部性能修改
使用本地对照，设备验证集中到该阶段交付。

## 实现中的决策

信号排队、屏蔽、等待、默认动作、停止与继续仍由 Linux 管理。宿主在系统
调用返回和定时抢占后进入 Linux 的返回用户态处理，依据 AArch64 信号 ABI
保存寄存器并递送处理器。宿主执行状态与 TLS 保留在信号帧之外。

复用固定版本 Linux 的 AArch64 signal、sigcontext 和 ucontext 声明；统一
宿主 NCE 与内核 pt_regs 的寄存器布局，保持现有汇编保存偏移。首先支持
FPSIMD、备用信号栈、嵌套递送和 musl 提供的返回入口。

## 本地验收范围

- 信号屏蔽、待处理信号、同步等待、备用栈与嵌套处理。
- 阻塞 read、nanosleep、sigsuspend 的 EINTR，以及 SA_RESTART。
- 非法访存转 SIGSEGV，修改权限后重试；非法指令转 SIGILL 后恢复。
- 异常信号帧、默认终止、进程停止和继续。
- pthread 取消及没有系统调用的循环接收定时信号。
- 保留既有内存、线程、抢占与 ELF 布局回归。

本阶段已接入 dash shell，测试结果见下文。性能结论仍需
按真实负载测量，N2-C1 设备结果见 [N2c 记录](native-linux-n2c-progress.md)。

## 已完成与本地结果

Linux 6.12 的信号队列、屏蔽、停止、继续、终止和系统调用等待逻辑已用于
NCE 线程。新实现负责 AArch64 用户信号帧、FPSIMD 保存与恢复、备用栈和
`rt_sigreturn`。目前信号返回入口由 musl 的 `SA_RESTORER` 提供。

`python3 scripts/probe_lkl.py --processes` 完成主线程与 GCD 两次运行；
两次均通过信号检查、32 轮原有进程检查、16 轮 C 用户态压力检查、64 轮
内存边界检查及 28 个工作集场景。两次均创建并释放 1,324 个 NCE 上下文。
另有 8 项 ELF 布局单测及独立的宿主上下文信号屏蔽检查通过。N0 寄存器
检查与 iOS C 源码编译通过。结果保存为
`experiments/native-linux/results-n2d-build.json`。

新增信号检查覆盖通用寄存器、NZCV、NEON、FPCR/FPSR 的恢复，并分别
破坏 PC 对齐、SP 对齐、FPSIMD 标识和扩展记录终止项，确认 Linux 以
SIGSEGV 结束对应子进程。没有系统调用的计算循环可以收到 ITIMER_REAL
信号；pthread 取消、EINTR、SA_RESTART、备用栈和嵌套信号也已通过。

实际用户态使用 dash 0.5.12，源码归档 SHA-256 为
`6a474ac46e8b0b32916c4c60df694c82058d3297d8b385b74508030ca4a8f28a`。
交叉编译保留 x18、使用 16 KiB ELF 对齐和静态 PIE，信号名称生成器采用
Linux 信号编号。shell 联合测试覆盖管道、重定向、here document、命令替换、
函数、参数、外部 exec、trap、后台 wait 和信号终止状态。

## 调度停顿的修复

定时信号测试发现：阻塞调用结束后，宿主抢占使用的 SIGUSR2 被屏蔽。
Darwin 的 `longjmp` 通过 `sigprocmask` 恢复保存的信号屏蔽状态，能够影响
其他线程；LKL 的 GCD 定时器线程因此把它的屏蔽状态带入客体工作线程。
LKL 调度上下文改用 `_setjmp` / `_longjmp`，保留各 pthread 的信号状态。

独立回归在一个屏蔽 SIGUSR2 的线程执行上下文恢复，并检查另一个线程
仍保持解除屏蔽。原实现返回失败，新实现通过。相关平台实现可查
[Apple libplatform setjmp](https://github.com/apple-oss-distributions/libplatform/blob/main/src/setjmp/arm64/setjmp.s)
与 [XNU 信号处理](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/kern_sig.c)。

宿主的信号回调现在只保存与恢复上下文，Linux 缺页和信号处理在普通宿主
栈上执行。启动 exec 参数所在页面先显式准备；手写程序与编译程序的 SVC
入口均处理栈缺页。原有在 Darwin 信号回调内调用 Linux 的路径已移除。

## 复杂度审查与偏离记录

按 reclaim-code-entropy 检查了新增入口和状态的用途。Linux 继续拥有信号
队列与调度，没有增加另一套信号队列。宿主和内核共享一份寄存器布局，汇编
偏移保持不变。临时诊断输出已移除；独立宿主回归保留，因为它能在不启动
Linux 的情况下重现跨线程信号屏蔽问题。

本阶段扩大为实际 dash shell 测试，新增 `/dev/null` 和 `/bin/sh`，ELF 槽位
由 8 MiB 调整为 4 MiB，启动参数栈放在文件区域末尾。原计划中的手机包继续
后移：新 Linux ABI 和 shell 已通过本地检查，先完成动态链接再交付设备验证。
