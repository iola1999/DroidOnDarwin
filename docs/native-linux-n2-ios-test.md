# N2 原生 Linux + NCE 手机测试

日期：2026-10-09。适用设备：当前的 iPhone 17 / iOS 27.2。

## 安装包

```text
/Users/fan/project/app/husk_hyos/work/native-linux-nce/ios/Husk-NativeLinux-N2-R3.ipa
```

构建编号 `N2-R3-iOS-20261009-011553`，包大小 14.30 MiB。
SHA-256：

```text
fa42c8e1b4d0ef53a825008f9ceed937d79ddafbd5aac425e2af7268ddf68f5f
```

这是原生 Linux 与 NCE 的独立测试界面。它使用 `com.husk.app`，通过 iLoader
沿用当前账号覆盖安装后显示为 Husk NCE。手机中的配对文件使用原有路径。
运行内容是内置 Linux 内核和受控 ARM64 程序；HyperOS 用户界面尚未接入。

## 操作

1. 在 iLoader 中导入这个 IPA，沿用原来安装 Husk 的账号，保留 JIT Helper 扩展。
2. 打开手机上的 LocalDevVPN 并连接，然后打开 Husk NCE。
3. 点“启用内置 JIT 并测试”。应用会复用已有配对文件；若提示缺少文件，导入
   这台手机已有的 remote pairing 文件，或使用“通过 StikDebug 启用并测试”。
4. 保持 Husk 在前台。应用会准备 16 MiB 加 64 KiB JIT 代码区域，断开调试器后运行检查，
   完成时显示“全部测试通过”。运行时禁用自动锁屏，完成后恢复。
   界面上的构建编号应以 `N2-R3-iOS-` 开头。
5. 点“导出测试日志”，将 `native-linux-n2-ios.log` 放到 Mac 的
   `/Users/fan/Downloads/husk-log/`，用于分析本轮结果。

手机测试执行全部在 iPhone 内完成；安装与取回日志可以使用 Mac。

若应用退回桌面，重新打开 Husk 后直接导出上次日志。若停在某一步，先保留
当前日志再关闭应用。再次测试需要从多任务界面关闭 Husk 后重新打开；开始
新一轮会覆盖本轮日志。

## 这次检查什么

| 日志项目 | 验证内容 |
| --- | --- |
| `prepare_jit_pool` | 保留 208 MiB 虚拟地址，只为 16 MiB 加 64 KiB 代码区域做 JIT 准备 |
| `wait_debugger_detach` / `debugger_detached` | 等待 `P_TRACED` 清除，最长五秒；超时返回界面并保存日志 |
| `prepared_fixed_address_code_updates` | 主代码池和客体代码区各执行八次，分别返回 42 或 43 |
| `data_fixed_remap_after_detach` | 代码区两侧的真实数据地址各切换八次，读取符合预期 |
| `linux_ram_remap` | 在 Linux 物理内存的实际目标地址映射 64 MiB，检查首尾页面与共享写入 |
| `prepared_nce_compute` | 同一段 400 万次计算/访存的直接执行与 NCE 耗时，五轮结果一致性检查 |
| `native_linux_boot` / `linux_uname` | Linux 6.12 内核原生装载、启动和系统调用 |
| `linux_nce_fork_cow_shared_futex_pipe_exec_wait` | 32 轮独立 Linux 进程、COW、共享页、futex、pipe、ELF exec 和 wait |
| `linux_exec_then_fork_cow_wait` | exec 后再次 clone；固定代码地址继续执行，栈 COW、父子 TLS 和退出状态正确 |
| `linux_exec_file_unchanged` | 改写指令后，原始 ELF 文件内容保持一致 |
| `fixed_linux_code_view` | 记录固定执行地址从 Linux 私有页恢复代码的次数 |
| `linux_nce_context_lifetime` | 分配与释放各 96 个 NCE 上下文 |
| `result: passed` | 本轮全部检查和内核关闭完成 |

日志逐步写入。早期执行探针增加限定代码范围的异常恢复；Mac 上通过了
非法指令、访问异常、返回值错误、恢复执行与原信号处理器恢复检查。
iOS 签名异常的恢复效果仍待确认，后续 Linux 故障尚未全部接入恢复流程。

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

## 已完成的检查与边界

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

本轮暂时无需追加手机测试，可以进入普通 ELF、init、线程、信号和较大工作集
的实现。下一次手机验证集中使用该阶段产物，范围见[原型记录](native-linux-nce-prototype.md#n2b普通-linux-用户态)。
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
