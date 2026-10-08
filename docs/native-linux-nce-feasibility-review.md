# 原生 Linux + NCE 路线复核

日期：2026-10-09。依据：用户提供的 `nce.md`、N2-B1 手机日志、当前源码、
N2-B2 的 Mac 回归，以及文中链接的官方资料。目标仍是在当前 iPhone 内运行
完整 Linux / Android，并达到基本可用的持续交互表现。

## 判断与优先级

目前已有继续验证原型的依据，完整 HyperOS 的可用性仍未确认。纯计算直接
执行、Linux 原生编译和小型进程测试已经通过部分设备检查；多进程地址空间
切换、ART 动态代码和硬件图形决定最终收益。当前数据不足以承诺接近 HVF
的整机性能，也无法给出可信的成功概率。

先修复本次可复现的线程抢占停顿，再测量地址空间切换随工作集增长的成本。
这项评估提前到继续扩大用户态接口之前。如果现有映射设计无法承受频繁
进程通信，应先调整地址空间设计，再决定是否扩大 Android 集成。

## 对用户所附分析的核对

| 分析中的问题 | 当前实现与验证 | 尚需解决 |
| --- | --- | --- |
| Linux 与 Darwin 的系统调用 ABI 不同 | 改写 ELF 中的 SVC 和 TLS 指令，系统调用进入本机编译的 LKL Linux 内核；R3 手机已通过 fork、COW、futex、pipe、exec、wait | Linux 信号帧、异常、中断语义及用户指针检查 |
| iOS 可执行内存与签名限制 | 当前侧载与 JIT 条件下，R3 和 B1 已通过调试器断开后的固定地址执行、代码更新和数据重映射 | 任意布局、动态代码及地址池容量；结果仅适用于已经测试的设备与环境 |
| ARM64 ABI 与 ELF 装载 | Mac 已运行 musl 静态 PIE、重定位、BSS、函数指针、TLS 和 pthread | Bionic 动态链接、第三方 ELF、x18 约定、4 KiB 布局、ART 代码生成 |
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

`experiments/native-linux/lkl-nce-mmu.c` 中，`nce_switch_mm` 每次切换
不同的 `mm_struct`，都会撤销客体窗口的映射，再遍历新进程的 VMA 与页表。
现实现按页恢复已驻留页面；代码页需要复制到 iOS 最初准备的执行地址。
线程共享同一 `mm_struct` 时可以跳过这一步。

因此，普通指令可以直接执行，频繁跨进程通信仍可能耗费大量时间。Zygote
派生进程与 Binder 服务通信都需要进一步评估。当前单 Linux CPU 设计也
无法让不同地址空间在同一宿主窗口中同时执行。

已有 N0 Mac 实验中，连续 2 MiB 区域重映射并访问每页约需 48.350 μs；
128 个分散的 16 KiB 页面合计同样为 2 MiB，约需 193.982 μs。仅访问首页
的对应结果为 2.057 μs 和 103.203 μs。数据见
`experiments/native-linux/results-macos.json`。这些是较早的独立映射探针，
本次未重测，不能直接用于预测 Android 延迟；它们已显示出页面分布和重新
访问工作集的成本。

下一组本地实验应通过真实 Linux 进程反复通信，改变驻留页数、私有与共享页
比例、访问范围，分别记录调度次数、映射次数、代码复制字节数、耗时分布和
物理内存。先在现有 64 MiB 客体窗口内测试，再按需要扩大窗口与 Linux RAM。
对照项包括同地址空间线程通信，以及相同计算量下的直接执行。

优化候选是保留仍然相同的映射、合并连续页面、只更新改变的代码区，并测量
推迟映射到首次访问的收益与缺页代价。这些修改都需保留 fork/COW、共享页、
权限变化、exec 和页面回收语义。若工作集增大后开销仍随全部驻留页面增长，
需要评估更换地址空间设计的成本；增加普通系统调用覆盖无法消除该问题。

## 动态代码与程序兼容

当前 ELF 在构建时登记布局，保留 `$x` / `$d` 映射符号，以区分指令与同值
常量；使用 16 KiB 段对齐和 `-ffixed-x18`。这些条件使静态 PIE 原型可以
受控验证。HyperOS 中的预编译 ELF 尚未逐个验证。

Apple 的 [ARM64 ABI 文档](https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms)
规定了 x18 等平台约定。Android 的
[16 KiB 支持文档](https://source.android.google.cn/docs/core/architecture/16kb-page-size/16kb?hl=en)
提供了重新构建系统及原生组件的要求。当前镜像里的每个组件是否满足这些
条件，需要实际检查；通用 ELF 和动态代码的改写也需覆盖缺少映射符号的情况。

接下来需要验证 Bionic 动态链接与共享库、运行时分配和复用执行区、代码失效
及重新改写。Linux 信号处理、异常恢复、栈展开和线程暂停需要与执行上下文
一致。[ART 文档](https://source.android.com/docs/core/runtime)说明了其编译、
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
超时。修复后的手机完整线程流程仍需一次验证。设备结果、修复前复现和
修复后回归分别保存于 `results-ios-n2-b1.json` 和
`results-preemption-macos.json`，均位于 `experiments/native-linux/`。

本次改动处理线程抢占缺陷。路线层面的性能问题仍按上述顺序验证。相对上一
阶段的计划，地址空间切换评估已提前；完整 shell 的扩展放到后面。
