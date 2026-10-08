# N2-B2 原生 Linux + NCE 手机测试

日期：2026-10-09。适用设备：当前的 iPhone 17 / iOS 27.2。
方案、当前范围和后续安排见 [N2b 实现记录](native-linux-n2b-progress.md)。

## 安装包

```text
/Users/fan/project/app/husk_hyos/work/native-linux-nce/ios/Husk-NativeLinux-N2-B2.ipa
```

构建编号 `N2-B2-iOS-20261009-025346`，版本 `0.9.1 (40)`，包大小 14.46 MiB。
SHA-256：

```text
456f9200d76cd4aaad3bc6e88e038d52e5f7f63aa2ae81834e351fa582feebe2
```

应用使用 `com.husk.app`，通过 iLoader 沿用当前账号覆盖安装后显示为 Husk NCE。
手机中的配对文件沿用原有路径。运行内容为 Linux 6.12 与 musl 编译的 ARM64
程序；HyperOS 用户界面尚未接入。运行过程全部在 iPhone 内完成。

## 操作

1. 在 iLoader 中导入这个 IPA，沿用原来安装 Husk 的账号，保留 JIT Helper 扩展。
2. 打开手机上的 LocalDevVPN 并连接，然后打开 Husk NCE。
3. 确认界面构建编号为 `N2-B2-iOS-20261009-025346`，点“启用内置 JIT 并测试”。
   应用复用已有配对文件；若提示缺少文件，导入这台手机已有的 remote pairing
   文件，或使用“通过 StikDebug 启用并测试”。
4. 保持 Husk 在前台。应用完成 JIT 准备与调试器断开后，检查内存映射、运行
   Linux 内核、进程与线程测试。完成时显示“全部测试通过”。运行时禁用自动
   锁屏，完成后恢复。
5. 点“导出测试日志”，将 `native-linux-n2-ios.log` 放到 Mac 的
   `/Users/fan/Downloads/husk-log/`，或直接提供日志文件。

若应用退回桌面，重新打开 Husk 后直接导出上次日志。若停在某一步，先保留
当前日志再关闭应用。再次测试需要从多任务界面关闭 Husk 后重新打开；开始
新一轮会覆盖本轮日志。

Mac 的 GCD 回归中，16 轮 C 用户态测试累计约 1.15 秒，JIT 准备另计。
手机耗时尚待测量。本轮建议进入 `linux_userspace_tests` 后若超过 30 秒仍
无后续记录，就导出日志并关闭应用；30 秒是此次排查的停止等待条件。

## 为什么现在需要手机验证

N2-B1 已在手机通过多个 ELF 执行区和原有进程检查，随后停在第一轮 C 程序
测试。Mac 用与 iOS 一致的 GCD 入口复现了停顿：客体线程继承被屏蔽的
`SIGUSR2`，计算循环无法被抢占。N2-B2 在客体线程入口启用该信号，并通过
新增的 GCD 完整回归。当前需要确认手机能完成全部 pthread、抢占和线程
退出检查。运行范围及后续安排见 [路线复核](native-linux-nce-feasibility-review.md)。

## 日志检查项

| 日志项目 | 验证内容 |
| --- | --- |
| `prepare_jit_pool` | 保留 208 MiB 虚拟地址，准备 16 MiB 主代码池及合计 224 KiB 的三个程序执行区 |
| `wait_debugger_detach` / `debugger_detached` | 等待 `P_TRACED` 清除，最长五秒 |
| `prepared_fixed_address_code_updates` | 四个代码范围各执行八次更新 |
| `data_fixed_remap_after_detach` / `linux_ram_remap` | 代码区两侧数据切换，64 MiB Linux RAM 映射与共享写入 |
| `prepared_nce_compute` | 五轮直接执行与 NCE 的相同计算结果 |
| `native_linux_boot` / `linux_uname` | 原生启动 Linux 6.12 |
| `linux_nce_fork_cow_shared_futex_pipe_exec_wait` | 原有 32 轮受控进程回归 |
| `unblocked_inherited_preemption_signal` | 客体线程发现并解除继承的 SIGUSR2 屏蔽 |
| `signal_redirect_to_host_entry` / `guest_fault_on_host_stack` | 通过信号返回离开 Darwin 回调，再在宿主栈处理客体缺页或抢占 |
| `signal_restore_without_guest_stack` | 通过信号上下文恢复寄存器，覆盖客体栈暂时不可写或已释放的场景 |
| `linux_musl_static_pie_processes` | 每轮运行 8 次 worker，检查 PID 1、PIE、参数、环境变量、pipe、exec、wait 与 2 MiB COW |
| `linux_musl_threads_preemption` | 每轮 56 个 pthread 线程、8 次释放栈后的退出；无系统调用的循环可被抢占 |
| `linux_compiled_elf_files_unchanged` | 指令改写后原始 ELF 内容保持一致 |
| `linux_userspace_stress` | 16 轮、128 次命令、896 个线程全部完成 |
| `linux_nce_context_lifetime` | 分配与释放各 1,264 个 NCE 上下文 |
| `linux_guest_events` | 记录抢占、缺页和栈恢复次数 |
| `guest_preemption_signal_mask` | 记录解除继承信号屏蔽的线程数 |
| `after_userspace` | 每轮结束采样物理占用与进程常驻内存峰值 |
| `result: passed` | 本轮检查及内核关闭完成 |

本包通过 N2b 的主线程与 GCD 两种入口回归、8 项 ELF 单测、5 项调试器断开
检查和执行异常恢复检查。此前 N0/N1 检查结果保留在原型记录。
iOS Release 构建、应用及扩展签名、IPA 完整性检查通过。安装包使用临时签名，
由 iLoader 重签。当前仍有用户指针隔离、Linux 信号语义和任意 ELF 等限制，
详见 N2b 记录；本次尚无 HyperOS 整机性能结论。

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
