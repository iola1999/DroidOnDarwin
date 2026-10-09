# 原生 Linux + NCE 路线复核

日期：2026-10-09。依据：用户提供的 `nce.md`、当前源码、B1/B2/C1 手机日志、
N2c 的 Mac 对照、N2d/N2e 本地回归，以及文中链接的官方资料。目标仍是在当前 iPhone 内运行
完整 Linux / Android，并达到基本可用的持续交互表现。

## 判断与优先级

目前已有继续验证原型的依据，完整 HyperOS 的可用性仍未确认。纯计算直接
执行、Linux 原生编译和小型进程测试已经通过部分设备检查；多进程地址空间
切换、ART 动态代码和硬件图形决定最终收益。当前数据不足以承诺接近 HVF
的整机性能，也无法给出可信的成功概率。

线程抢占停顿已修复并通过手机检查。N2c 的进程地址空间切换已在手机验证，
稀疏访问与共享页面通信取得较大局部收益，密集私有访问仍有较高成本。
N2d/N2e 已在本地通过 Linux 信号、musl 动态链接器、共享库、动态 TLS 与
dash shell。当前继续 Bionic / ART 集成，集中设备验证按
[Android 阶段目标](native-linux-android-stage.md) 安排。

## 对用户所附分析的核对

| 分析中的问题 | 当前实现与验证 | 尚需解决 |
| --- | --- | --- |
| Linux 与 Darwin 的系统调用 ABI 不同 | 改写 ELF 中的 SVC 和 TLS 指令，系统调用进入本机编译的 LKL Linux 内核；手机已通过 fork、COW、futex、pipe、exec、wait 和用户指针检查 | N2d 信号帧、异常、中断与重启已在本地通过，设备待确认；Bionic 所需的扩展语义仍待接入 |
| iOS 可执行内存与签名限制 | 当前侧载与 JIT 条件下，R3 和 B1 已通过调试器断开后的固定地址执行、代码更新和数据重映射 | 任意布局、动态代码及地址池容量；结果仅适用于已经测试的设备与环境 |
| ARM64 ABI 与 ELF 装载 | Mac 已运行 musl 静态/动态 PIE、共享库、dlopen、动态 TLS、pthread 和动态 dash | Bionic 动态链接、第三方 ELF、x18 约定、4 KiB 布局、ART 代码生成 |
| Android 系统服务依赖复杂 | 当前原型保留 Linux 内核职责，后续计划接入 Android 用户态 | Binder、Zygote、ART、系统服务与 HyperOS 厂商组件的实际启动和运行 |
| 图形转换和同步影响帧率 | 旧 QEMU 路线已有 gfxstream / ANGLE Metal 的 Mac 开发结果 | NCE 路线中的 gralloc、共享缓冲区、fence、SurfaceFlinger 与 iOS GPU 集成 |
| 计算性能无法预测整机体验 | 现有计算测试与完整 Android 测试分别记录 | 启动、设置、应用切换、帧延迟、持续发热的同负载对照 |

所附分析对 Android Runtime、内存与图形的担忧成立。它给出的架构主要围绕
APK 加载与 Android 用户态兼容。当前项目选择真实 Linux 内核管理进程、
页表、文件和 futex，宿主实现执行与内存映射。这减少了逐项重写 Linux
语义的范围，仍需完成 Darwin 宿主和 ARM64 用户态的连接。

[LKL 项目](https://github.com/lkl/linux)说明了将 Linux 内核作为库、通过
宿主操作接口运行的方式。当前原型扩展了它的用户进程执行部分，完整
Android 支持需要在这些扩展上继续验证。

## 首先验证的风险：进程地址空间切换

N2c 已将完整撤销与逐页恢复改为比较实际映射，合并连续区域；数据按需恢复，
并记录因进程切换而重新访问的页面。代码继续使用最初准备的执行地址，
相同源页与权限可以保留，变化时更新内容。

同一 Mac 构建的三次交替对照中，16 MiB 工作集、只访问少量页面的进程通信
快约 78–87 倍；访问全部共享页面快约 35 倍。访问全部私有页面仅快约 1.82
倍。实现、28 种场景的测量条件、范围与原始记录见
[N2c 实现记录](native-linux-n2c-progress.md)。C1 手机各运行一次两种策略，
对应的稀疏访问快约 77–103 倍、全共享密集访问约 56 倍、全私有密集访问
约 2.04 倍；原有 C 程序总耗时仅相差约 3%。温度与调度条件未控制，
这些局部测量尚未覆盖 Android 实际负载。

单 Linux CPU 仍依次执行各进程；不同地址空间共享同一个宿主地址窗口，
频繁扫描私有页面时仍需要大量宿主映射。现有测试窗口为 64 MiB，尚需
测量扩大窗口后的页表扫描开销、访问模式变化、Binder 负载和 Zygote 派生
进程的实际页面共享比例。

## 动态代码与程序兼容

当前 ELF 在构建时登记布局，使用 `$x` / `$d` 映射符号或 AArch64 PLT 的
PURECODE 标记识别指令；使用 16 KiB 段对齐和 `-ffixed-x18`。这些条件用于
受控验证静态与动态 PIE。HyperOS 中的预编译 ELF 尚未逐个验证。

Apple 的 [ARM64 ABI 文档](https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms)
规定了 x18 等平台约定。Android 的
[16 KiB 支持文档](https://source.android.google.cn/docs/core/architecture/16kb-page-size/16kb?hl=en)
提供了重新构建系统及原生组件的要求。当前镜像里的每个组件是否满足这些
条件，需要实际检查；通用 ELF 和动态代码的改写也需覆盖缺少映射符号的情况。

musl 动态链接与共享库已完成本地检查，代码改写仅作用于宿主执行视图，
Linux 文件内容保持原样。接下来需要验证 Bionic、运行时分配和复用执行区、
代码失效及重新改写。Linux 信号和异常恢复已通过本地检查，栈展开、线程
暂停及 Bionic 扩展仍需与执行上下文配合。
[ART 文档](https://source.android.com/docs/core/runtime)说明了其编译、
垃圾回收和线程相关职责；本项目尚未运行 ART。应先做受控的 Bionic / ART
实验，验证所需语义与开销，再扩大到 Zygote 和系统服务。

## 图形与长期运行

完整 Android 的显示需要缓冲区分配、生产与消费、同步和 GPU 命令执行。
[AOSP 图形文档](https://source.android.com/docs/core/graphics)描述了相关
组件。CPU 直接执行的收益需要与这些路径一起测量。

旧路线的 ANGLE / Metal 开发结果可以复用部分实现，但 NCE 的共享内存和
Linux 驱动接口需要重新集成。优先验证一个完整的共享缓冲区与同步流程，
记录 CPU 复制量和帧延迟，再扩大 SurfaceFlinger 与应用绘制。

短时原型结果尚未覆盖 iOS 持续调度、功耗、温度、内存增长和后台行为。最终
判断需使用目标设备持续运行完整 Android 的数据。

## 本次停顿与修复范围

N2-B1 的日志通过了 Linux 启动及原有进程检查，随后停在 C 程序第一轮。
Mac 按 iOS 的 `DispatchQueue.global(qos: .userInitiated)` 入口运行时，
复现了相同停顿，并记录到 `SIGUSR2` 被屏蔽。

[libdispatch 源码](https://github.com/apple-oss-distributions/libdispatch/blob/main/src/init.c)
中的工作线程信号初始化与本地 SDK 的 `dispatch/queue.h` 文档说明了这一
条件。新建 pthread 继承信号屏蔽状态，导致计算线程收不到抢占信号。原来的
命令行主线程回归没有覆盖 GCD 入口，这是本次测试遗漏。

N2-B2 在客体线程入口启用 `SIGUSR2`。GCD 回归恢复了全部 16 轮执行，
累计 1,147.064 ms，记录 143 次抢占。打包前检查现在包含该入口和三十秒
超时。修复后的手机完整线程流程已通过，16 轮累计 742.691 ms、139 次抢占，
记录在 `experiments/native-linux/results-ios-n2-b2.json`。B1 设备结果、修复前复现和
修复后回归分别保存于 `results-ios-n2-b1.json` 和
`results-preemption-macos.json`，均位于 `experiments/native-linux/`。

B2 处理了线程入口继承抢占屏蔽的问题，C1 已完成地址空间策略的设备对照。
N2d 进一步修复宿主上下文恢复影响其他线程信号屏蔽的问题，并实现 Linux
信号；N2e 运行动态 shell 与共享库。两阶段已经合入 N2-E1，完整记录见
[N2e 进展](native-linux-n2e-progress.md)。Android 的持续交互表现仍需完整
系统验证。
