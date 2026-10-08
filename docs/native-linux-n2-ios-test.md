# N2 原生 Linux + NCE 手机测试

日期：2026-10-09。适用设备：当前的 iPhone 17 / iOS 27.2。

## 安装包

`work/native-linux-nce/ios/Husk-NativeLinux-N2.ipa`

完整路径：

```text
/Users/fan/project/app/husk_hyos/work/native-linux-nce/ios/Husk-NativeLinux-N2.ipa
```

构建编号 `N2-iOS-20261009-001629`，包大小 14.29 MiB。
SHA-256：

```text
f459d47eef92ae3c43ff5e6c1ad7ff27b8db1c94049ecefdd3af2933e57de215
```

这是原生 Linux 与 NCE 的独立测试界面。它使用 `com.husk.app`，通过 iLoader
沿用当前账号覆盖安装后显示为 Husk NCE。手机中的配对文件使用原有路径。
运行内容是内置 Linux 内核和受控 ARM64 程序；HyperOS 用户界面尚未接入。

## 操作

1. 在 iLoader 中导入这个 IPA，沿用原来安装 Husk 的账号，保留 JIT Helper 扩展。
2. 打开手机上的 LocalDevVPN 并连接，然后打开 Husk NCE。
3. 点“启用内置 JIT 并测试”。应用会复用已有配对文件；若提示缺少文件，导入
   这台手机已有的 remote pairing 文件，或使用“通过 StikDebug 启用并测试”。
4. 保持 Husk 在前台。应用会准备 80 MiB JIT 内存，断开调试器后运行检查，
   完成时显示“全部测试通过”。运行时禁用自动锁屏，完成后恢复。
5. 点“导出测试日志”，将 `native-linux-n2-ios.log` 放到 Mac 的
   `/Users/fan/Downloads/husk-log/`，用于分析本轮结果。

手机测试执行全部在 iPhone 内完成；安装与取回日志可以使用 Mac。

若应用退回桌面，重新打开 Husk 后直接导出上次日志。若停在某一步，先保留
当前日志再关闭应用。再次测试需要从多任务界面关闭 Husk 后重新打开；开始
新一轮会覆盖本轮日志。

## 这次检查什么

| 日志项目 | 验证内容 |
| --- | --- |
| `prepare_jit_pool` | 调试器是否能准备 80 MiB 可执行内存，原有 JIT 分配器是否能创建 RW 别名并执行代码 |
| `debugger_detached` | 调试器已断开，`P_TRACED` 已清除 |
| `prepared_jit_fixed_remap_after_detach` | 同一地址交替重映射两页代码，八次执行均返回正确数值 |
| `prepared_nce_compute` | 同一段 400 万次计算/访存的直接执行与 NCE 耗时，五轮结果一致性检查 |
| `native_linux_boot` / `linux_uname` | Linux 6.12 内核原生装载、启动和系统调用 |
| `linux_nce_fork_cow_shared_futex_pipe_exec_wait` | 32 轮独立 Linux 进程、COW、共享页、futex、pipe、ELF exec 和 wait |
| `linux_exec_file_unchanged` | 改写指令后，原始 ELF 文件内容保持一致 |
| `linux_nce_context_lifetime` | 分配与释放各 64 个 NCE 上下文 |
| `result: passed` | 本轮全部检查和内核关闭完成 |

日志文件逐步写入。失败前最后一个阶段可帮助定位 JIT 分配、代码重映射、
内核装载或 Linux 进程执行的问题。

## 已完成的检查与边界

共用内存池的 Mac 运行通过八次代码页重映射、五轮计算对照、Linux 启动和
32 轮进程检查；N0/N1 回归通过。iOS Release 构建、ARM64 架构、动态库依赖、
应用及扩展的签名完整性、IPA ZIP 完整性检查通过。包中包含内核映像、JIT
脚本、HuskJITHelper 和 StikJIT。

IPA 使用临时签名，需要 iLoader 重签。真机安装、应用界面与原生 Linux 执行
尚未验证。这次手机结果决定后续内存实现能否继续沿用。独立用户态 init、
完整 Linux 信号语义、任意 ELF、Android 和 GPU 仍在后续范围内；当前测试
无法给出 HyperOS 的整机加速倍数。
