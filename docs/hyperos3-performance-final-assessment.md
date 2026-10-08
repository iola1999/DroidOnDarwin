# HyperOS 3 性能路线最终评估

日期：2026-10-08。范围：iPhone 17（iPhone18,3）、iOS 27.2、普通侧载 Husk，
在手机内运行完整 HyperOS 3。判断标准是持续交互获得接近硬件虚拟化量级的改善。

后续更新：用户要求继续实现可行性原型，已启动
[原生 Linux 与 NCE 实验](native-linux-nce-prototype.md)。本文件保留 QEMU
阶段的结束判断；新原型已通过 Mac 上的原生内核子系统测试，完整用户进程及
手机端表现仍待验证。

## 决定

结束本轮优化开发。重新核对硬件虚拟化、原生执行、Linux 内核移植、扩展进程、
静态编译和图形处理后，尚未找到满足当前平台条件、且有足够依据预期达到目标的
实施路径。源码改动规模没有作为排除条件。

完整 Linux 原生移植仍有理论上的大幅提速空间，其可行性和整机收益均未确认。
继续开发需要先解决宿主运行条件，随后验证完整 Linux 多进程环境。按用户本轮
提出的停止标准，项目在此结束，不再沿局部缓存与指令优化继续试验。

本次结论限定于已核对的接口、实现和测试。未在这台 iPhone 上新调用私有
Hypervisor 接口，也未复现下文第三方固件分析。

## 整机结果

最后一次完整 Mac 实验 M33 已使用 gfxstream → ANGLE Metal，以及 Direct、
代码共享、访存缓存和淘汰代码保留。测试结果为失败：

| 项目 | 结果 |
| --- | ---: |
| 确认 Android 启动 | 744.865 秒 |
| 设置启动命令 | 182.346 秒 |
| Android 报告的设置 TotalTime | 175,153 ms |
| 六次滑动命令 | 109.499 秒 |
| 帧完成时间 | 无有效结果 |
| 整轮实验 | 1,117.170 秒 |

记录：`work/performance/m33-android-cold/results.json`、`host.log`、
`boot-host-sample.txt`。该实验运行在 Mac，手机上的最后已测版本也仍明显卡顿。
M33 没有安装到手机。

M33 的固定代码缓存压力测试约快四倍，完整系统仍未达到目标。最后累计编译
耗时为 46.276 秒；即使继续减少这部分工作，也需要同时解决执行、访存、进程
切换与等待。各轮改动和磁盘状态存在差异，历史启动耗时不能构成单项改动的
加速比例。

## 重新筛选的路线

| 路线 | 大幅提速的来源 | 当前限制与判断 |
| --- | --- | --- |
| HVF 或其他硬件虚拟化后端 | 客体用户态和内核直接执行，硬件承担地址转换 | 收益基础明确；当前侧载环境缺少已验证可用的 iOS 内核接口和授权 |
| 系统级修改或直接启动 Linux/Android | 获得虚拟化访问条件，或直接使用硬件执行完整系统 | 已核对的越狱与裸机移植项目未提供 iPhone 17 / iOS 27.2 路径，还需相应硬件驱动 |
| 原生 Linux 内核 + Android 用户态 NCE | 同时消除大量内核和用户态动态翻译 | 可能减少两类执行开销；完整 Linux 的宿主进程、内存和异常实现尚未建立，整机收益未知 |
| 保留 QEMU 内核，扩大 NCE / fastmem | 减少用户态指令和访存开销 | 内核模拟仍存在；多进程映射与 4 KiB/16 KiB 页兼容继续产生开销，现有实现未获得目标收益 |
| 16 KiB 客体内核 + 每进程宿主地址窗口 | 增加直接映射比例，减少重复映射 | 只解决部分访存条件；仍要处理地址重写、权限、JIT 和闭源库兼容，手机可用地址范围未知 |
| iOS 扩展进程承载客体进程 | 利用独立宿主地址空间减少进程切换时的映射工作 | 新接口提供了扩展进程；同一扩展复用已有进程，现有代码没有完整 Android 多进程、共享内存和 JIT 管理实现 |
| 静态重编译、LLVM 优化、ART 或服务原生运行 | 减少运行时编译及被模拟的计算 | 完整系统仍需地址空间和系统调用语义；HyperOS 闭源组件及动态生成代码需要保留执行支持，缺少本项目整机收益依据 |
| HWC、共享纹理、图形协议改造 | 减少客体合成、GPU 读回和像素复制 | 已有 Metal 实验仍严重卡顿；尚未测得这些图形开销占据足够比例，无法据此预期 HVF 量级收益 |
| 多核 TCG、调度、镜像裁减与启动快照 | 增加并行吞吐，减少后台或启动工作 | 无法同时消除串行执行、地址转换及交互时的系统调用成本，现有数据不足以支持目标收益 |

### 硬件虚拟化：需要可用的内核接口

Apple 当前 Hypervisor 文档元数据只列出 macOS。UTM 官方安装表将普通侧载版
的 Hypervisor 标为不可用，另列的虚拟化条件涉及特定安装方式和 M 系列 iPad。
修改 Husk 的构建选项或应用签名声明，仍需 iOS 内核接受对应请求。

本次发现 `ios26-hypervisor-research` 的新线索：作者对 iPhone 14 Pro、
iOS 26.4.1 的固件进行了静态分析，报告 Stage-2、VMID、客体进入/退出代码和
`has-virtualization=1`。仓库没有侧载应用成功创建并运行虚拟机的测试。
其 issue #1 进一步询问 XNU 的 `hv_support_available` 是否仍为 0，读取时尚无
回复。因此这些结果支持继续研究底层实现，当前应用访问条件仍未确认。

这里应保留的判断是：这台手机的普通侧载环境没有已验证的硬件虚拟化运行路径。
芯片和固件是否包含部分实现，需要单独核实。安全研究设备、系统级修改或将来
开放接口属于运行条件变化，Husk 源码本身无法提供这些条件。

来源：[Apple Hypervisor](https://developer.apple.com/documentation/hypervisor)、
[UTM 安装条件](https://docs.getutm.app/installation/ios/)、
[固件分析，提交 9604483](https://github.com/cloverfield11/ios26-hypervisor-research/tree/960448392da7f7c3b6b1f749e3fb80890a2e39a8)、
[尚未解答的启用条件](https://github.com/cloverfield11/ios26-hypervisor-research/issues/1)。

### 系统级修改与直接启动 Android

也检查了修改宿主系统和直接启动 Linux/Android 的路线。Dopamine 当前说明的
版本与芯片组合未覆盖 iPhone 17 / iOS 27.2；palera1n 的硬件范围为 A8–A11
及 T2。Project Sandcastle 的发布构建限于 iPhone 7/7+、iPod touch 7，功能表
仍将 GPU、音频和蜂窝支持列为未实现。

直接启动可消除模拟开销，但需要本机可用的启动方式及显示、GPU、输入、存储
等驱动。本次核对的项目未提供目标设备所需条件；增加 Husk 的后端无法完成
这部分宿主系统和硬件适配。以上判断限定于所列公开项目与本次查询时间。

来源：[Dopamine](https://github.com/opa334/Dopamine)、
[palera1n](https://github.com/palera1n/palera1n)、
[Project Sandcastle 支持状态](https://projectsandcastle.org/status)。

### 原生 Linux：潜力存在，运行条件仍缺失

ARM64 UML 的作者报告在 Linux/Android 宿主上启动了 Alpine、Debian，并提供
4 KiB、16 KiB 页模式。源码依赖 Linux 的 `clone`、`ptrace`、seccomp 和
`PTRACE_GETREGSET` 等接口。作者 RFC 还记录了单处理器限制和进程退出时的
内存统计问题。将其用于 Darwin，需要实现新的宿主接口和进程执行方式。

LKL 可把 Linux 的文件系统、网络等代码作为库使用，但它没有 UML 的完整用户
进程环境。NCE 项目提供 SVC、TLS、计数器和异常处理参考；MeloNX 研究分支的
`NcePatcher`、`NceThreadPal` 确实包含 Apple 平台分支。本次没有构建运行这些
上游原型，也没有找到它们启动完整 HyperOS 的结果。

单宿主进程中，两个客体进程可在相同虚拟地址映射不同内容。原样地址访问需要
切换宿主映射；保留不同宿主窗口则需要处理客体地址到窗口的转换。已有原型的
逐页 remap 与权限设置平均约 1.5 微秒；五个 Android 目标进程的初步检查中，
只有约 10%–18% 驻留页符合直接合并为 16 KiB 映射的条件。匿名分配实验中的
mTHP 改善只覆盖该分配负载。重建 16 KiB 客体仍需检查 ELF、APEX、厂商库和
权限边界。详细原始记录见[架构调研](hyperos3-execution-research.md)。

原生内核与 NCE 结合的路线尚未证明不可行。决定停止的依据是关键平台条件和
整机收益均未确认；目前无法给出足够可信的 HVF 量级改善预期。完整 Linux 的
init、fork/exec、COW、futex、信号、Binder 和原生执行全部通过，才具备进一步
讨论 HyperOS 性能的基础。

来源：[ARM64 UML 作者 RFC](https://lists.openwall.net/linux-kernel/2026/08/15/732)、
[已核对的 UML 源码](https://github.com/zalexdev/linux-um-arm64/tree/8897487c52233cd00cf2850008ca068892f1ae91)、
[LKL 项目说明](https://github.com/lkl/linux/blob/d0f76a77e8c2ca68bd7d96190b8da09040eab072/Documentation/lkl/lkl.txt)、
[MeloNX NCE 源码](https://github.com/bixinwei/melonx/tree/6d34f5bd65037983b8c5ae2111e11e3aecfa1fe6/src/Ryujinx.Cpu/Nce)。

### 新版 iOS 扩展进程：已检查的额外可能

iOS 26 的 `AppExtensionProcess` 可以启动应用扩展，旧的单进程描述需要加入
这一条件。Apple 文档明确：同一扩展已运行时，会取得已有进程。UTM 的新实现
利用它运行 `qemu-img`，源码限制一次运行一个 helper；QEMU 本身仍在主应用内。

可以设想把若干客体进程分配给不同扩展，但动态进程数量、共享页与 COW、每个
扩展的 JIT 条件及生命周期均没有本项目验证。现有接口和 UTM 实现尚不足以提供
Android 的进程环境；本轮没有据此增加新的运行后端。

来源：[Apple AppExtensionProcess](https://developer.apple.com/documentation/extensionfoundation/appextensionprocess)、
[UTM helper 源码，提交 7eadb05](https://github.com/utmapp/UTM/blob/7eadb056ae0f91d979059544d0ddcd2d5a40be92/Services/UTMHelperProcess.swift)、
[UTM 架构说明](https://github.com/utmapp/UTM/blob/7eadb056ae0f91d979059544d0ddcd2d5a40be92/Documentation/Architecture.md)。

### 编译和图形改造的判断边界

函数交叉编译与混合执行研究提供了选择性原生调用的实现思路，其测试针对
跨指令集应用与库。本项目还要执行完整内核、管理多进程和动态代码；论文的
应用加速结果无法给出这里的整机收益。SystemUI 与设置的 ART speed 产物已有
实验记录，Metal 图形路径也已运行。继续优化它们需要新的主要耗时占比结果。

来源：[Partial Cross-Compilation and Mixed Execution](https://arxiv.org/html/2512.00487v1)。

## 清理与保留内容

未验证的 M34 ASID 原型已撤回，五个已跟踪文件恢复到 `a130b8d`，新增源码与
补丁移除，生成的 QEMU 源码也已反向应用该补丁。已完成的 M1–M33 等源码、
补丁、测试和记录保留在 Git；本轮没有继续扩展运行时设计。

第一次清理涉及 155 个文件或目录目标，包括旧虚拟机磁盘、重复下载与解包镜像、镜像
构建中间文件、构建缓存、旧 IPA 及解包目录、可重新生成的反汇编和性能映射表。
按文件分配量累计约 93.28 GiB；项目目录由约 104.99 GiB 降至 11.71 GiB。
清理前后 `df` 的可用空间增加约 75.32 GiB。APFS 共享块及其他进程的磁盘活动
会影响两种统计，分别记录。

用户随后要求清空剩余本地产物。第二次清理涵盖全部镜像、下载源码、原始实验
日志和结果、构建缓存及打包目录，磁盘可用空间增加约 11.71 GiB。最终项目
占用约 42.61 MiB，保留 Git 历史及其管理的源码、测试、脚本、补丁和文档。
本文件中的实验结果和路径用于记录开发过程；恢复实验需要重新下载依赖和
镜像并构建。清理前确认相关虚拟机和构建任务均未运行，清理后确认所有 Git
管理的文件完整。两次清理均限定在 Mac 项目目录。

## 重新评估所需的新信息

后续若有新的平台条件，可依据下列结果重新判断：当前 iPhone/系统/签名下能
成功运行硬件虚拟机；或 Darwin 上原生 Linux 与 NCE 已通过完整多进程测试，
并测得持续交互所需路径的大幅改善。新增局部循环倍数不足以改变本轮决定。
