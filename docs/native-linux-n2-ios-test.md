# N2-E1 原生 Linux + NCE 手机测试

日期：2026-10-09。适用设备：当前的 iPhone 17 / iOS 27.2。
本包合并 Linux 信号与动态链接用户态，实现和检查记录见
[N2d](native-linux-n2d-progress.md) 与 [N2e](native-linux-n2e-progress.md)。

## 安装包

```text
/Users/fan/project/app/husk_hyos/work/native-linux-nce/ios/Husk-NativeLinux-N2-E1.ipa
```

构建编号 `N2-E1-iOS-20261009-093002`，版本 `0.9.1 (61)`，包大小 15.18 MiB。
SHA-256：

```text
365b7a0f6ded960b899b8b8711649801d89233fed63955ea93caec1996045074
```

应用使用 `com.husk.app`，通过 iLoader 沿用当前账号覆盖安装后显示为 Husk NCE。
手机中的配对文件沿用原有路径。运行内容为 Linux 6.12、musl 动态链接器、
dash shell 和 ARM64 测试程序；HyperOS 用户界面尚未接入。
运行过程全部在 iPhone 内完成。

## 操作

1. 在 iLoader 中导入这个 IPA，沿用原来安装 Husk 的账号，保留 JIT Helper 扩展。
2. 打开手机上的 LocalDevVPN 并连接，然后打开 Husk NCE。
3. 确认构建编号为 `N2-E1-iOS-20261009-093002`，点“启用内置 JIT 并测试”。
   应用复用已有配对文件，使用默认映射策略。
4. 保持 Husk 在前台，等待“全部测试通过”，导出日志。一次运行包括原有检查、
   Linux 信号、动态 dash 和动态库测试。运行时禁用自动锁屏，完成后恢复。

若应用退回桌面，重新打开后直接导出上次日志。再次运行会覆盖本轮日志。
若进入 Linux 测试后超过 60 秒没有完成，先导出日志再关闭应用。
60 秒是本轮停止等待条件，设备正常耗时尚待确认。

## 本轮需要确认什么

C1 的优化版与基准版都已在手机通过。N2-E1 增加 Linux 信号帧、异常返回、
系统调用重启、共享库加载及动态 TLS；预先准备的代码范围从五个增加到十个。
此次集中检查 iOS 上固定执行地址更新、缺页后恢复和信号上下文返回。
Mac 上这些流程已经通过，设备的代码签名与 JIT 条件需要实际确认。

| 日志项目 | 验证内容 |
| --- | --- |
| `prepare_jit_pool` | 保留 208 MiB 虚拟地址，准备 16 MiB 主代码池和合计 800 KiB 的九个 ELF 执行区 |
| `debugger_detached` | JIT 助手已断开 |
| `prepared_fixed_address_code_updates` | 十个代码范围各执行八次更新 |
| `data_fixed_remap_after_detach` / `linux_ram_remap` | 代码区两侧的数据切换，64 MiB Linux RAM 映射与共享写入 |
| `linux_mapping_policy` | 默认策略为 `lazy` |
| `native_linux_boot` / `linux_uname` | 原生启动 Linux 6.12 |
| `linux_signal_*` | 信号屏蔽、备用栈、寄存器、EINTR/重启、异常恢复、帧校验和定时抢占 |
| `linux_pthread_cancel_signal` | 阻塞 pthread 取消 |
| `linux_nce_fork_cow_shared_futex_pipe_exec_wait` | 原有 32 轮进程检查 |
| `linux_userspace_stress` | 16 轮、128 次命令、896 个 pthread 线程完成 |
| `linux_user_memory_boundaries` | 指针、权限、跨页访问和文件偏移，以及 64 轮共享、COW 与地址复用 |
| `linux_mm_exchange_integrity` | 28 个通信场景及数据一致性检查通过 |
| `linux_dash_shell_suite` | 动态 dash 管道、重定向、exec、trap 和后台 wait |
| `linux_dynamic_link_dlopen_constructors` | 依赖库、插件、构造函数、符号查找与缺失符号 |
| `linux_dynamic_tls_threads` | 动态主程序和共享库在 8 个 pthread 中的 TLS 独立性 |
| `linux_dynamic_fork_signal` | 动态程序 fork、wait 和信号返回 |
| `linux_dynamic_elf_files_unchanged` | ELF 文件内容保持原样 |
| `linux_nce_context_lifetime` | 分配与释放各 1,334 个 NCE 上下文 |
| `linux_guest_events` / `guest_preemption_signal_mask` | 抢占、缺页与继承屏蔽的处理次数 |
| `after_userspace` | 每轮结束时的应用物理占用与常驻内存峰值 |
| `result: passed` | 检查及内核关闭完成 |

本包已通过主线程与 GCD 入口的默认策略回归、`full` / `diff` 功能回归、
10 项 ELF 单测、宿主上下文信号检查、5 项调试器断开检查和执行异常恢复
检查。iOS Release 构建、应用及扩展签名、IPA 完整性检查通过；使用临时
签名，由 iLoader 重签。设备执行待确认。shell 测试自动执行内置脚本，
当前界面尚未提供交互终端。

## C1 手机结果

日志 `(4)` 为优化版、`(5)` 为基准版，两次均完成全部检查，1,294 个 NCE
上下文全部释放。16 MiB 稀疏访问通信快约 77–103 倍，全私有页面密集访问
快约 2.04 倍，全部共享页面密集访问快约 56 倍。原有 C 用户态 16 轮总耗时
分别为 714.700 ms 和 736.275 ms，差约 3%。两种策略各运行一次，温度和
调度条件未控制，完整 Android 的交互性能仍未测量。详细记录见
[N2c 进展](native-linux-n2c-progress.md)。

## B2 手机结果

构建 `N2-B2-iOS-20261009-025346` 已通过全部检查，累计 139 次抢占、
896 个线程，1,264 个上下文全部释放。旧版从 GCD 线程继承 SIGUSR2 屏蔽
造成的停顿已经修复。完整记录见
`experiments/native-linux/results-ios-n2-b2.json` 和
[N2b 实现记录](native-linux-n2b-progress.md)。

## R1 闪退原因与 R2 调整

本次 `native-linux-n2-ios 2.log` 来自 R1。80 MiB JIT 分配和原始 RX 地址上的
函数执行成功，断开等待耗时 13 ms。日志结束于 `check_executable_remapping`。

取回的两份 R1 系统崩溃报告中，`Husk-2026-10-09-003108.ips` 记录新映射
地址 `0x1099f4000` 取指失败，异常为 `EXC_BAD_ACCESS / SIGBUS`，返回码
`0x32`。iPhoneOS SDK 的 `mach/kern_return.h` 将 50 定义为
`KERN_CODESIGN_ERROR`。该页显示 `r-x`，执行仍被 iOS 拒绝。
另一份 `003112` 报告属于本次日志的 PID 30048，记录原有信号处理器重新
抛出的 SIGBUS，其主线程位置无法确定最初故障点。摘要与文件哈希保存在
`experiments/native-linux/results-ios-n2-r1.json`。

R2 保留调试器最初准备的 RX 地址。进程切换时，将 Linux 私有代码页复制到
这个地址对应的 RW 别名；数据页继续按 Linux 页表重映射。客体地址窗口缩为
64 MiB，与 64 MiB 物理内存、64 MiB 内核虚拟地址区一起纳入准备的内存池。
受控 ELF 固定使用一个 64 KiB 代码区，系统调用和 TLS 改写写回私有页。
Linux 数据和栈的默认权限改为读写。

这个实现需要测量代码复制的调度成本。当前支持固定布局的受控 ELF，任意
ELF、动态代码、独立 init、完整信号语义、Android 和 GPU 仍需继续实现。

## R2 手机结果与 R3 调整

`native-linux-n2-ios 3.log` 确认 R2 已通过断开、固定地址代码更新、数据页
重映射和五轮 NCE 计算对照。随后进入 `linux_init`，`shmem_map` 中的
`vm_remap` 返回失败；断言调用 `abort()`。系统报告 `010126` 的 PID 与日志
一致，异常为 SIGABRT。该版没有记录 Mach 返回码，具体拒绝分支尚未确认。

Apple 的 [XNU 映射实现](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/vm/vm_map.c)
在启用可执行映射不可变条件时，会把成功关联的代码区域标记为
`vme_permanent`。据此推断，R2 把全部 208 MiB 区域交给调试器准备，可能
使其中需要被替换的数据映射也受到限制。

R3 先保留普通虚拟地址，仅准备实际代码区域：16 MiB 主代码池和 64 KiB
客体代码区。Linux 的 64 MiB RAM 使用普通匿名内存，数据映射目标也保持
普通映射。Husk 的现有 JIT 分配器增加指定地址准备入口，复用同一份调试器
脚本与 RW 别名实现。

启动内核前，应用在实际目标地址检查完整的 64 MiB 映射、首尾页面及共享
写入。预检失败会记录 Mach 返回码、源地址、目标地址和长度，并返回界面。
Mac 上已验证失败调用能返回错误，随后正常代码仍可执行。内核运行中的
异常恢复仍有限制。iOS 若保留已准备的代码映射，它们随应用进程退出回收。

## R3 已完成的检查与边界

共用运行核心的 Mac 检查通过两个代码区的更新、代码区两侧的数据重映射、
64 MiB RAM 映射、五轮计算对照、
Linux 启动和 32 轮进程检查；96 个上下文全部释放。iOS Release 构建、应用及
扩展的签名完整性、IPA ZIP 完整性检查通过。包中包含内核映像、JIT 脚本、
HuskJITHelper 和 StikJIT。IPA 使用临时签名，需要 iLoader 重签。

R3 已在当前 iPhone / iOS 27.2 完成全部检查。日志来自构建
`N2-R3-iOS-20261009-011553`，最终结果为 `passed`：64 MiB 数据映射成功，
Linux 6.12.0 启动耗时 15.678 ms；32 轮进程检查共 14.227 ms，处理 352 次
缺页、复制代码 512 次，分配与释放各 96 个 NCE 上下文。完整记录保存在
`experiments/native-linux/results-ios-n2-r3.json`。

R3 验收后进入普通 ELF、init、线程、信号和较大工作集的实现。后续产物与
验证安排见 [N2b 记录](native-linux-n2b-progress.md)。
这次运行时间很短，代码仍为受控程序；持续性能、运行峰值内存与 HyperOS
整机加速倍数尚未测量。

## 初版断开问题

初版 `N2-iOS-20261009-001629` 在断开请求返回后立即检查调试状态，
`!nce_debugger_attached()` 断言失败并调用 `abort()`。R1 改为每 10 ms
查询一次，最多等待五秒；查询失败或超时返回界面。立即断开、延迟断开、
持续连接、查询失败和返回数据长度异常五种场景均通过回归检查。

[debugserver 的公开实现](https://github.com/llvm/llvm-project/blob/main/lldb/tools/debugserver/source/MacOSX/MachProcess.mm#L1676-L1777)
在处理断开时可能先恢复进程，再安排 SIGSTOP 与 `PT_DETACH`。
R1 手机日志已确认等待后调试标志清除。
