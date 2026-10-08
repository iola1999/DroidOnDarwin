# N2-C1 原生 Linux + NCE 手机测试

日期：2026-10-09。适用设备：当前的 iPhone 17 / iOS 27.2。
实现、性能对照与后续安排见 [N2c 实现记录](native-linux-n2c-progress.md)。

## 安装包

```text
/Users/fan/project/app/husk_hyos/work/native-linux-nce/ios/Husk-NativeLinux-N2-C1.ipa
```

构建编号 `N2-C1-iOS-20261009-034855`，版本 `0.9.1 (41)`，包大小 14.50 MiB。
SHA-256：

```text
24932182c628b83e664421a831c4e34efb3b454135785277ce94bbde5e2eccb2
```

应用使用 `com.husk.app`，通过 iLoader 沿用当前账号覆盖安装后显示为 Husk NCE。
手机中的配对文件沿用原有路径。运行内容为 Linux 6.12 与 musl 编译的 ARM64
程序；HyperOS 用户界面尚未接入。运行过程全部在 iPhone 内完成。

## 操作

1. 在 iLoader 中导入这个 IPA，沿用原来安装 Husk 的账号，保留 JIT Helper 扩展。
2. 打开手机上的 LocalDevVPN 并连接，然后打开 Husk NCE。
3. 确认构建编号为 `N2-C1-iOS-20261009-034855`，保持默认的“优化版”，点
   “启用内置 JIT 并测试”。应用复用已有配对文件。
4. 保持 Husk 在前台，等待“全部测试通过”，导出日志。测试包含内存检查、
   28 个进程通信场景，以及原有进程、线程和 JIT 检查。运行时禁用自动锁屏，
   完成后恢复。
5. 若要取得手机上的速度对照，先保存上述日志，再从多任务界面关闭 Husk，
   重新打开并选择“基线版”，运行同一组测试后另存日志。两轮都使用这个包。

若应用退回桌面，重新打开后直接导出上次日志。再次运行会覆盖本轮日志。
若进入“正在测试 C 程序和线程”或“正在测量进程通信”后超过 60 秒没有完成，
先导出日志再关闭应用。60 秒是本轮停止等待条件，设备正常耗时尚待确认。

## 本轮需要确认什么

N2-B2 已在手机通过全部线程与抢占检查，16 轮 C 用户态累计 742.691 ms。
N2-C1 修改了映射保留、合并、按需恢复及 Linux 用户内存复制路径；Mac 已经
完成三次交替对照和 256 轮压力测试。现在集中确认新路径在 iOS 上的权限、
缺页恢复、资源释放和耗时。Linux 信号、动态链接与 Android 仍按阶段记录
继续开发。

日志检查项：

| 日志项目 | 验证内容 |
| --- | --- |
| `prepare_jit_pool` | 保留 208 MiB 虚拟地址，准备 16 MiB 主代码池和合计 272 KiB 的四个程序执行区 |
| `debugger_detached` | JIT 助手已断开 |
| `prepared_fixed_address_code_updates` | 五个代码范围各执行八次更新 |
| `data_fixed_remap_after_detach` / `linux_ram_remap` | 代码区两侧的数据切换，64 MiB Linux RAM 映射与共享写入 |
| `linux_mapping_policy` | 优化版为 `lazy`，基线版为 `full` |
| `native_linux_boot` / `linux_uname` | 原生启动 Linux 6.12 |
| `linux_nce_fork_cow_shared_futex_pipe_exec_wait` | 原有 32 轮受控进程检查 |
| `linux_userspace_stress` | 16 轮、128 次命令、896 个 pthread 线程完成 |
| `linux_user_memory_boundaries` | 错误指针、权限、跨页读写，以及 64 轮共享、COW 与地址复用 |
| `linux_mm_exchange` | 每个工作集、共享比例与访问范围下的请求回复耗时，单位为 ns |
| `linux_mm_exchange_integrity` | 28 个场景及数据一致性检查通过 |
| `linux_mm_host_operations` | 内存检查与通信阶段的数据映射次数、代码复制量和撤销次数 |
| `linux_nce_context_lifetime` | 分配与释放各 1,294 个 NCE 上下文 |
| `linux_guest_events` | 抢占、缺页和栈恢复次数 |
| `guest_preemption_signal_mask` | 解除继承信号屏蔽的线程数 |
| `after_userspace` | 每轮结束时的应用物理占用与常驻内存峰值 |
| `result: passed` | 检查及内核关闭完成 |

本包通过两种 Mac 入口的默认策略回归、三种策略的重复对照、8 项 ELF 单测、
5 项调试器断开检查和执行异常恢复检查。iOS Release 构建、应用及扩展签名、
IPA 完整性检查通过；使用临时签名，由 iLoader 重签。设备执行待确认。

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
