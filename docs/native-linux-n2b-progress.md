# N2b：Linux 用户态实现记录

更新日期：2026-10-09。长期目标是在 iPhone 内运行完整 Linux / Android，CPU
通过 ARM64 NCE 执行。前一阶段的 iOS 条件与 R3 验收见
[原型记录](native-linux-nce-prototype.md#r3-手机验收与实现范围2026-10-09)。

## 当前阶段

N2-B1 在手机通过 JIT 准备、固定地址代码更新、Linux 启动和原有 32 轮进程
检查，随后停在第一轮 C 程序测试。日志已到达 Darwin 信号返回、客体栈恢复
及宿主栈缺页处理。N2-B2 已在手机完成完整 pthread 与抢占流程。

Mac 已复现同一停顿：应用从 GCD 工作线程启动内核，客体 pthread 继承被屏蔽
的 `SIGUSR2`，无系统调用的计算线程因此无法被抢占。N2-B2 在客体线程入口
启用该信号。主线程与 GCD 两种入口均已通过完整回归；修正版的 iOS Release
构建、签名完整性和 IPA 检查通过，手机最终结果为 `passed`。安装步骤见
[手机测试说明](native-linux-n2-ios-test.md)。

长期可行性与接下来的验证顺序见
[NCE 路线复核](native-linux-nce-feasibility-review.md)。内置程序使用 musl 1.2.5，
采用 16 KiB 对齐的 ARM64 静态 PIE。

手机 B2 的 16 轮 C 用户态累计 742.691 ms，中位数 46.2565 ms，139 次
抢占、896 个线程和 1,264 个上下文释放均通过。设备记录见
`experiments/native-linux/results-ios-n2-b2.json`。后续开发进入
[N2c：地址空间与用户态扩展](native-linux-n2c-progress.md)。

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

N2-B1 打包所用的 Mac 运行中，16 轮用户态流程共 1,170.796 ms，每轮中位数
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

### N2-B2：GCD 信号屏蔽修复

手机日志的 Linux 启动耗时为 17.232 ms，32 轮旧进程检查为 26.269 ms，
最后阶段为 `linux_userspace_round: 0` 和 `linux_userspace_tests`。
用户观察到等待超过 100 秒仍未结束；日志自身没有记录这段等待的总时长。
设备结果保存在 `experiments/native-linux/results-ios-n2-b1.json`。

新增 `dispatch-test.c` 从 `QOS_CLASS_USER_INITIATED` 队列调用同一核心，
对应 iOS 应用的启动入口。修复前，它记录 `sigusr2_blocked: 1`，随后也停在
第一轮 C 程序测试；本地运行由十秒超时结束。修复后，客体线程安装信号栈后
调用 `pthread_sigmask(SIG_UNBLOCK, ...)`，仅启用 `SIGUSR2`。GCD 工作
线程的信号屏蔽状态保持原样。

| Mac 回归入口 | 16 轮累计耗时 | 抢占次数 | 发现并解除继承屏蔽的线程 |
| --- | ---: | ---: | ---: |
| 命令行主线程 | 1,142.183 ms | 152 | 0 |
| GCD 工作线程 | 1,147.064 ms | 143 | 48 |

两种入口均完成 128 次 worker、896 个 pthread 和 1,264 个上下文的分配与
释放。8 项 ELF 单测、5 项调试器断开检查、执行异常与失败重映射检查通过。
这些耗时只包含 C 用户态的 16 轮运行；JIT 准备与其他预检另计。

`probe_lkl.py --processes` 现在要求 GCD 回归在三十秒内完成，并确认确实
处理过继承的信号屏蔽；结果及源文件哈希进入打包清单。该检查覆盖本次遗漏
的宿主入口差异。源码、运行记录与包信息见
`experiments/native-linux/results-preemption-macos.json`。构建日志为
`work/native-linux-nce/n2b2-validation.log` 和 `n2b2-package.log`。

复现命令：

```sh
python3 scripts/probe_native_linux.py
python3 scripts/probe_lkl.py
python3 scripts/package_native_linux_ios.py --rebuild-kernel
```

第三条包含 N2b 本地运行和 ELF 单测，输入通过后再构建 IPA。只检查已经
生成的 ELF 可运行 `python3 scripts/test_nce_userspace.py`。

## 后续顺序

1. N2-B2 手机抢占与全部线程检查已通过。
2. 测量不同工作集、共享页比例和进程切换频率下的映射、代码复制及首次访问
   成本，先判断当前地址空间设计的性能边界；比较映射合并、共享页保留与
   按差异更新的收益。具体安排见路线复核。
3. 实现用户指针检查及 Linux 信号帧、返回和中断语义；加入
   无效指针、客体异常、信号处理、线程取消等失败场景。
4. 扩展动态加载、共享库和执行区分配，优先验证 Bionic、ART 动态代码及
   图形共享缓冲区，再扩大完整 Android 用户态集成。

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

N2-B2 的运行修改集中在客体线程入口。GCD 回归复用原有测试与核心，只增加
实际启动方式；移除它会再次遗漏本次缺陷。新增统计仅记录继承屏蔽的线程数，
用于检查回归确实经过目标条件。本次无需新增调度状态或修改 iOS 启动队列。

相对 N2b 初始计划，本次先交付静态用户态、线程与异常返回阶段；共享库、
完整 shell、动态代码和 Linux 信号语义仍在后续。这个顺序用于先确认新的
iOS 执行条件，再扩大依赖它的用户态范围。
