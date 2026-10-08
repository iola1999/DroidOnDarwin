# N2b：Linux 用户态实现记录

更新日期：2026-10-09。长期目标是在 iPhone 内运行完整 Linux / Android，CPU
通过 ARM64 NCE 执行。前一阶段的 iOS 条件与 R3 验收见
[原型记录](native-linux-nce-prototype.md#r3-手机验收与实现范围2026-10-09)。

## 当前阶段

N2-B1 的运行核心已在 Mac 完成 16 轮用户态流程：128 次 worker 命令、896 个
pthread 线程、1,264 个 NCE 上下文的创建与释放。内置程序使用 musl 1.2.5，
采用 16 KiB 对齐的 ARM64 静态 PIE。N2-B1 的 iOS Release 构建、签名完整性
和 IPA 检查通过；手机执行结果待确认。安装步骤见[手机测试说明](native-linux-n2-ios-test.md)。

这次手机测试有新的必要条件：Darwin 信号返回需要恢复客体寄存器与 PC，
并将缺页和定时抢占转到宿主栈继续执行。R3 只覆盖固定代码地址和数据映射，
尚未验证这条异常返回路径。

## 实现与取舍

| 部分 | 本次实现 | 当前范围 |
| --- | --- | --- |
| ELF 装载 | Linux `binfmt_elf` 装载程序；静态 PIE 自行处理重定位；按 `PT_LOAD` 准备执行区 | 构建时登记程序与执行布局，运行时校验路径、ELF 头和段表 |
| 指令改写 | 根据 AArch64 `$x` / `$d` 映射符号区分指令与常量，单独分配 SVC/TLS 跳板 | 内置程序保留映射符号，编译时预留 Darwin 的 x18 |
| 代码容量 | worker 包含 4,096 个独立函数与函数指针表，执行区为 112 KiB | 单程序地址范围上限 4 MiB，用户地址窗口 64 MiB |
| init 与进程 | 独立 PID namespace 内的用户态 PID 1 运行批处理命令，覆盖 pipe、fork、exec、wait、参数和环境变量 | LKL 原有初始化任务保留在外层 PID namespace |
| 线程 | musl pthread、TLS、互斥锁、futex、join 与 detached 退出 | 单个 Linux CPU 依次执行客体线程 |
| 异步调度 | 2 ms 宿主定时线程发送 SIGUSR2；客体执行期间保存通用寄存器、NEON、NZCV 和浮点状态，再由 Linux 调度 | x18 保留给 Darwin；宿主及跳板执行期间暂不抢占 |
| 缺页 | 客体代码中的访存缺页先退出 Darwin 信号处理器，再进入 Linux 缺页处理 | 内核访问用户地址的缺页仍保留原有路径 |
| 栈边界 | 返回客体时可通过信号上下文恢复寄存器；支持 musl 释放线程栈后执行退出系统调用 | 非法客体访问的完整 Linux 信号语义待实现 |

本阶段使用小型批处理程序组织子进程测试。完整 shell、共享库、动态生成代码
以及 Linux 信号帧与 `rt_sigreturn` 留在下一步。用户指针隔离、4 KiB ELF
布局、通用 x18 改写、多 CPU、Android 与 GPU 也需要继续实现。

## 验证与回归

共用运行核心保留 R3 的 JIT、重映射和 32 轮受控进程检查。新增 C 程序覆盖
静态 PIE 重定位、BSS、函数指针、长双精度运行库、每线程 TLS、阻塞唤醒、
无系统调用的计算循环、2 MiB 数据的 fork/COW、线程自行释放栈和 ELF 文件
内容一致性。循环运行 16 轮后检查所有 NCE 上下文释放。

`scripts/test_nce_userspace.py` 检查构建阶段的 ELF 布局：代码中的 SVC/TLS
同值常量、代码与数据共用宿主页、可写执行段、入口落在段外填充区、缺少
映射符号、暂未实现的解释器、段对齐和 ELF 表截断。单测随内核验证脚本运行，
失败时不生成可供打包使用的新清单。

构建清单记录源码、生成头文件、ELF 与内核映像的 SHA-256。打包脚本验证这些
值，防止将未通过本地运行的输入装入 IPA。iOS 特有的签名与信号返回行为仍
需要设备执行确认。

本次打包所用的 Mac 运行中，16 轮用户态流程共 1,170.796 ms，每轮中位数
72.273 ms，范围 69.230–80.957 ms。发生 138 次客体抢占、36,315 次转到
宿主栈的缺页处理，完成 128 次释放栈后的退出。首轮与末轮采样物理占用分别
为 22,938,152 和 23,839,296 字节；末轮记录的常驻内存峰值为 27,492,352
字节。短时运行中仍有内存增长，需要后续延长运行并区分分配器缓存与泄漏。
这些数据不包含 iOS 的运行表现。

源码与生成文件哈希、全部运行记录、测试包信息保存于
`experiments/native-linux/results-userspace-macos.json`。详细日志在
`work/native-linux-nce/processes/`；构建与回归日志在
`work/native-linux-nce/n2b-package.log`、`ios/detach-test.log` 和
`ios/execute-test.log`，后两个路径均相对于 `work/native-linux-nce/`。

复现命令：

```sh
python3 scripts/probe_native_linux.py
python3 scripts/probe_lkl.py
python3 scripts/package_native_linux_ios.py --rebuild-kernel
```

第三条包含 N2b 本地运行和 ELF 单测，输入通过后再构建 IPA。只检查已经
生成的 ELF 可运行 `python3 scripts/test_nce_userspace.py`。

## 后续顺序

1. 用已构建的 N2-B1 在手机集中验证新执行布局、信号返回、抢占与线程退出。
2. 设备通过后，实现用户指针检查及 Linux 信号帧、返回和中断语义；加入
   无效指针、客体异常、信号处理、线程取消等失败场景。
3. 扩展动态加载、共享库和执行区分配，运行完整 shell 与更大的用户态程序。
4. 记录进程切换时复制的字节数、映射次数和耗时分布，再针对实际开销调整
   映射恢复；之后评估 Android 用户态集成。

当前数据用于功能与短时压力检查。持续运行、温度和 HyperOS 整机可用性尚未
验证。

## 复杂度审查

进程、线程、页表、文件与 futex 继续由 Linux 管理。宿主保留执行区元数据、
页偏移和 ARM64 上下文；执行区元数据同时用于 JIT 准备、改写与清理，页偏移
用于将改写后的代码写回 Linux 私有页。去掉这些记录会破坏 exec 后切换与
fork 的内容一致性。

定时线程只负责向当前客体线程发送信号，Linux 决定任务顺序。去掉定时抢占，
无系统调用的计算线程会阻塞等待它的测试；保留该检查以覆盖调度需求。
本次没有新增独立调度器或通用后端接口。

相对 N2b 初始计划，本次先交付静态用户态、线程与异常返回阶段；共享库、
完整 shell、动态代码和 Linux 信号语义仍在后续。这个顺序用于先确认新的
iOS 执行条件，再扩大依赖它的用户态范围。
