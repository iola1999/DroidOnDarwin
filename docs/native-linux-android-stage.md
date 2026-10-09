# 原生 Linux + NCE：Android 用户态阶段

更新日期：2026-10-09。N3-A1 已完成本地回归和打包。后续实现了动态执行区
和 ART ARM64 JIT，N3-J1 合并包已完成构建与归档检查。后续引用数组、接口、
同步和预编译入口扩展已通过 CLI/GCD 回归。用户外出期间继续本机开发，手机
执行结果保留为待确认。

## 阶段验收与验证安排

本阶段交付需要同时完成以下结果，并形成可复现的本地回归和一个合并的
手机测试包：

1. 真实 Bionic 和 Android 动态链接器运行实际用户态程序。
2. Linux Binder 驱动完成跨进程请求与回复，检查数据、资源生命周期和失败返回。
3. ART 运行包含对象分配与回收、线程和 Java 异常的基础 Java 负载。

这组结果已在本机完成。用户于 2026-10-09 要求继续开发至北京时间 20:00
前，期间持续完成可独立推进的实现、失败场景和集成测试。设备测试留待用户
方便时集中进行；只有全部可推进路径均受设备条件或必要用户决策阻断时，
才报告阻塞。最终验收仍需完整 Android 的持续交互数据。

## 实现顺序与当前状态

已完成 Linux 信号、musl 静态和动态用户态、dash、Binder 跨进程通信，以及
Bionic 动态程序。Bionic 测试覆盖线程、TLS、分配释放和 setjmp/longjmp。
采用高地址对象堆的 ART 已完成 VM 创建、Java 负载和 VM 销毁。
CLI 与 GCD 两种入口均通过，N3-A1 合并手机测试包已生成并校验。

后续根据实际程序需求扩展执行区、ELF 装载和 ABI。先保留 Linux 的进程、
文件、内存与 Binder 语义，宿主仅处理原生执行和 Darwin 所需映射。ART 的
执行区申请、代码更新、线程暂停和异常处理由实际负载驱动实现。

本地构建可使用 Mac，手机运行所需内容随包交付。源码、下载内容和构建目录
放在当前项目的 work 下。每次本地提交记录验证结果；文档随实现同步更新。

## 已知边界

当前接受构建时登记、宿主页兼容布局的 ELF。x18 作为客体状态保存；目前
支持已遇到的 MOV/ORR、两条 Bionic setjmp 掩码指令，以及含 x18 的 64 位
LDP/STP 偏移寻址。构建检查会拒绝其他 x18 指令。客体地址窗口为 16 GiB，
Linux RAM 为 256 MiB。执行区在 JIT 准备期间确定。ART 动态代码使用额外的
32 MiB 预授权范围，通过 memfd 可写映射和显式提交更新；通用可执行 mprotect
仍受限制。

Bionic 的预编译 ELF 是否保留指令映射信息、共享库布局和 x18 使用方式，
需要逐个检查。ART 的内存需求、启动文件和内核接口由源码与实际运行确认。
当前 ART 已包含 ARM64 JIT。编译器检查优化后的指令，只接管已经适配的路径；
其他方法继续解释执行。完整 Android 服务、图形显示和持续交互仍需实现。

## Binder 与内核回归（2026-10-09）

`binder-test.c` 使用真实 binderfs 和 Binder ioctl，两个 Linux 进程完成
32 次请求与回复，负载包含 0、128、16,380、65,536 字节。测试检查内容、
校验和、对端 PID、缓冲区释放，以及文件描述符传递后的共享文件写入。
无效句柄和无效用户指针的返回也已检查。

首次运行发现 LKL 释放 PTE 页时缺少 `pagetable_pte_dtor`，Binder 复用该
物理页会被 `vm_insert_page` 拒绝。已补齐页表析构。内核日志检查又发现
NCE 在预取 ELF 页和生成 SIGSEGV 原因时缺少 mmap 读锁，已按 Linux
接口要求修复。每轮完整回归现在检查内核日志中的 WARNING、BUG 和 Bad page。

CLI 与 GCD 两种入口均通过完整回归，用户态压力轮数为 1，包含已有
fork、exec、信号、线程、动态链接与 shell 测试。结果保存在
`work/native-linux-nce/processes/lkl-results.jsonl` 和
`dispatch-results.jsonl`。此结果只覆盖 Mac 原生执行环境；Binder 单项完成后
继续开发 Bionic 和 ART。

已检查本机 NDK 29 的 Bionic 静态库：最小静态 PIE 保留指令映射符号，
使用 16 KiB 段对齐，初始化代码包含一次 `mov x18, x19`。Bionic 源码为
Android 16 提交 `09a271af557444c9a6b3f3146d6d474156fd6cdb`。另已检查官方
runtime APEX 提交 `070571b455076f77a01c7b07154a15e545d2b428`；其 linker64
采用 4 KiB 布局，代码与可写数据共用宿主页，需要重新构建或扩展映射实现。

## Bionic 动态执行与地址空间（2026-10-09）

运行材料来自 Android 16 的 16 KiB SDK 系统镜像
`arm64-v8a-ps16k-36_r07.zip`，SHA-1 为
`dda632745023567113d9954101709c6526a3c250`。提取脚本校验下载内容和分区表，
保留 runtime、ART、i18n、conscrypt APEX 以及系统依赖，删除大体积中间镜像。
逐文件 SHA-256 保存在 `work/native-linux-nce/android/android36-runtime.json`。

Android 动态链接器由固定提交的源码和 NDK 29 构建。它通过架构专用系统调用
245 预留 ELF 已登记的执行地址；段加载、重定位、依赖查找、TLS 和构造函数
仍由 Android 链接器完成。普通 ELF 元数据 mmap 保留 Linux 的地址选择行为。

Scudo 初始化会申请约 8.25 GiB 虚拟空间，因此将地址窗口扩展到 16 GiB。
页表遍历跳过缺失的 PMD，宿主映射更新只处理有记录的 1 MiB 区段；Linux
RAM 仍为 64 MiB。新增测试预留 8 GiB，只提交三个相隔较远的页面，再验证
fork 后的写时复制。

预编译 Scudo 会使用带标签的内部指针，在当前 Darwin 执行环境中触发访问
异常。构建使用上游 `SCUDO_DISABLE_TBI` 配置。当前 libc 由 NDK 29 的通用
Bionic 对象、固定版本的 Scudo 源码和 Bionic 共享库启动/分配器代码链接而成。
该组合的已验证范围是本节列出的负载；完整 Android ABI、GWP-ASan、ICU 和
平台服务仍需随实际运行检查。源码提交与下载校验值记录在
`experiments/native-linux/android-sources.json`，二进制与构建输入校验值记录在
各构建目录的 `manifest.json`。

CLI 和 GCD 完整回归均通过 Bionic 动态程序、8 GiB 稀疏预留、既有 Binder、
信号、进程、线程和 shell 测试，未发现内核 WARNING、BUG 或 Bad page。
这些结果来自 Mac 原生执行，手机合并验证安排在 ART 负载完成后。

### 当前实现审查

地址窗口扩大由 Scudo 的实际申请触发；去掉区段记录会让进程切换扫描全部
16 GiB。x18 只补充实际程序使用且有回归的形式，其他形式明确拒绝。ELF
预留增加一个专用调用，避免在普通 mmap 中猜测 Android 链接器的意图。
构建脚本按具体产物组织，未引入通用构建框架。

后续需要修改 ART 压缩对象引用和堆地址计算。代码更新与信号恢复仍按 Linux
状态验证。

## ART 共享库装载（2026-10-09）

ELF 现在按实际段范围连续安排地址，支持最多 2 MiB 段对齐。ART 的
`libart.so` 占用 12,730,368 字节虚拟空间；构建检查从 `.gnu_debugdata`
取得函数范围并定位需要改写的指令，共生成 8,559 处补丁。缺少指令范围信息
时构建失败，相关边界已有回归。ELF 区域与手写内核测试的数据区已分开。

预编译平台库需要 `_Unwind_Resume` 等共享库导出，Bionic 构建因此加入
LLVM 20.1.8 libunwind 源码。NDK 静态归档中这些符号具有隐藏可见性，无法
直接作为平台共享库接口使用。源码归档 SHA-256 随构建校验。

Linux RAM 增至 256 MiB，保留 16 GiB 稀疏地址窗口。CLI 与 GCD 完整回归
均通过：Android 链接器成功装载 ART 及其依赖，调用 `JNI_GetCreatedJavaVMs`
返回成功且 VM 数量为零。该轮测试确认装载、重定位和构造函数执行；后续
Java 结果见 N3-A1 本地回归结果。内核 WARNING、BUG、Bad page 检查均通过。

## ART 地址限制与实现调整（2026-10-09）

已检查 ART 提交 `ed6c006bd06ae060bd9698fd2cb25c4865512ec3`。
`runtime/mirror/object_reference.h` 将对象地址直接压缩为 32 位，
`libartbase/base/mem_map.cc` 因此要求对象堆位于 4 GiB 以下。

本地最小程序使用缩小的 PAGEZERO 后被系统终止；保留默认布局的程序调用
`vm_deallocate` 后，再向低地址申请内存，实际返回 `0x7000000000`。
[XNU 的 ARM64 装载检查](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/mach_loader.c)
要求 64 位 ARM 进程具有 4 GiB hard page zero，并通过 VM map 下界实施。
独立低地址数据区的原计划因此调整。

实现覆盖空引用、GC、解释器、JNI 和运行时汇编，后续编译执行还需同步
更新代码生成。单独修改对象指针的压缩函数不足以完成此适配。修改后的
ART 已完成下文记录的 Java 回归。

### 高地址堆与 ART 源码构建

`scripts/build_android_art.py` 固定 ART 和依赖的源码提交、归档校验值，并将
补丁保存在 `experiments/native-linux/art/android16.patch`。构建重新生成汇编
偏移、枚举输出和解释器汇编，产出 `libart.so` 与 `libartbase.so`。编译使用
NDK 29；C++ 头文件配置采用平台库的 `__1` 命名空间。当前组合的 ABI 检查
范围为下文列出的实际负载，完整 Android 集成还需覆盖更多平台接口。

`reference_heap.c` 预留 4 GiB 高地址区域，对象引用保存区域内的 32 位偏移，
零表示空引用。第一页保持保留，释放页面后重新映射为 `PROT_NONE`，继续保留
地址所有权。分配位图由互斥锁保护。宿主测试已通过空引用、最大偏移、映射
冲突、末页越界、局部释放、失败后的重试和四线程并发分配。

当前补丁接入 MemMap、对象引用、解释器虚拟寄存器、JNI 引用表和 GC card
table。验证配置采用上游受限执行模式与 mark-sweep GC，通过 `-Xint` 运行。
源码构建将 aconfig 标志全部设为 false，统计上报使用上游宿主空实现；独立
测试环境缺少 APEX 更新元数据，版本查询使用空标记。这些属于本阶段的配置
调整，后续完整 Android 集成需重新确认。JIT 入口明确终止；机器码对象访问
尚未完成地址适配。

Java 负载分配 16 MiB 临时数组、验证弱引用回收、两个线程通过 monitor 共享
计数，并捕获三类异常。JNI 启动程序创建 VM、调用负载并销毁 VM。
最终回归结果见下节。

### VM 初始化中的修复与检查

ART 设置主线程的栈保护页时，会先访问尚未提交的栈页面。NCE 的缺页处理
已接入 Linux `expand_stack()`，由 Linux 检查 `VM_GROWSDOWN`、栈大小限制和
相邻映射的保护间隔。新增回归验证约 1 MiB 的递归栈可用，同时确认设置
128 KiB 栈限制的子进程收到 `SIGSEGV`。该回归已通过。

高地址堆测试也已在 Bionic 程序中通过。类表、JNI、启动解释器和栈检查中的
对象地址转换继续使用同一偏移编码；越界编码会输出地址并终止。宿主回归
已验证三个越界值均触发 `SIGABRT`。

独立环境通过 `ld.config.txt` 提供 ART、i18n 和 conscrypt 的可见命名空间，
这些命名空间从 default 加载同一组依赖。运行库统一存放于 `/system/lib64`，
完整 Android 的 APEX 隔离配置仍需单独集成。ART 日志使用上游 stderr 输出，
测试入口完整读取错误日志，便于检查初始化失败。

VM 已进入核心类装载。调试器确认方法链接链表的索引 0 指向自身，循环持续
占用 CPU。源码中的 `BitVector` 构造函数复用调用者提供的存储，未主动清零；
独立构建遗漏了 Android Soong 默认的 `-ftrivial-auto-var-init=zero`。另外，
ART `build/art.go` 默认启用 `USE_D8_DESUGAR`，该配置影响 String 的启动布局。
构建已补齐这两项配置，并保留上游类布局检查。Java 执行、GC 和 VM 销毁仍须
全部通过后，才安排合并真机验证。

补齐构建参数后，VM 通过核心类装载，进入原生库初始化。独立文件系统增加
`/system/etc/public.libraries.txt`，声明已随包提供的 libc、libdl、libm 和
liblog。JNI 测试入口设置 90 秒期限，超时记录原因并以状态 124 退出。

VM 随后创建成功，执行到 `NceWorkload.main` 的数组分配。第一次 GC 在
`HeapBitmap::Test` 拒绝大对象地址：`DiscontinuousSpace` 的两个位图仍覆盖
原来的低 32 位地址区间。补丁将其起点移到高地址对象堆，保持原来的容量和
每页一位布局。弱引用回收测试继续检查这条路径。

## N3-A1 本地回归结果

2026-10-09 的最终 CLI 与 GCD 回归均通过，两个入口使用相同 Linux 内核、
客体 ELF 和 Java DEX。GCD 入口另外验证从宿主继承信号屏蔽的场景。

| 项目 | 已验证结果 |
| --- | --- |
| Bionic | 16 KiB 页面、动态链接、分配器、TLS 与 pthread |
| Binder | 32 次跨进程往返，最大负载 65,536 字节，文件描述符传递与无效句柄处理 |
| ART 对象与 GC | 16 MiB 临时数组内容正确，弱引用已回收，64 个循环关联对象在 GC 后仍保持身份与字段值 |
| ART 线程 | 两个 Java 线程通过 monitor 更新共享值，结果为 4,002,000 |
| ART 异常 | NullPointerException、ArrayIndexOutOfBoundsException、IllegalStateException 均被预期处理 |
| VM 生命周期 | JNI 创建、Java 调用和销毁全部完成 |
| Linux 回归 | 3 轮、24 次命令、168 个 pthread；信号、栈增长、地址边界、动态链接与 shell 均通过 |
| 资源与内核 | 两种入口均分配并释放 407 个 NCE 上下文；内核 WARNING、BUG、Bad page 检查通过 |
| ELF 检查 | 12 项单测通过，覆盖布局、映射符号、代码中的常量与实际 x18 指令 |

本机测得 JNI 创建至 VM 销毁分别为 94.654 ms（CLI）、86.774 ms（GCD）。
该计时包括 Java 负载，未包含 Linux 启动、文件准备和 ART 的 dlopen。
这些是 Mac 上的小型功能负载数据；手机性能、完整 Android 启动与交互耗时待测。

复现命令：

```sh
NCE_STRESS_ROUNDS=3 python3 scripts/probe_lkl.py --processes
python3 scripts/package_native_linux_ios.py
```

构建需要 Apple Silicon Mac、Xcode、NDK 29、OpenJDK 17、Android build-tools
36.0.0 和 android-35 平台文件。源码与 SDK 文件的校验值由构建记录保存。
结果位于 `work/native-linux-nce/processes/lkl-summary.json`；打包校验其中的
输入、内核和生成文件校验值。验证期间源文件发生修改时，脚本拒绝生成成功记录。

### 关键选择与复杂度审查

高地址适配集中在一个对象堆分配器和 ART 中实际使用的对象引用转换处。
ART 原有解释器、类链接、JNI、GC、线程与异常实现继续负责运行语义。
测试已经确认：大对象位图保留低地址范围时，首次 GC 失败；
位图和 card table 的地址范围需要同步调整。

源码构建脚本按固定 ART 模块读取上游源文件列表，生成所需汇编偏移和枚举。
对象文件复用依据编译命令、编译器与实际头文件的内容校验值；该检查用于缩短
调试期间的构建时间。文件系统配置仅声明本阶段实际提供的依赖与命名空间。

与最初低地址预留计划相比，当前使用高地址区域加 32 位偏移；这是 Darwin
地址下界要求产生的调整。当前 ART 使用 imageless 启动、受限解释执行与
mark-sweep GC。编译代码、boot image、完整 APEX 隔离和 Android 系统服务
仍需要后续实现。下一次集中设备验证检查 16 GiB 虚拟地址预留、增加后的固定
代码范围，以及真实 Bionic/ART 在线程和 GC 场景中的 iOS 执行条件。

当前合并包的安装说明见 [手机测试](native-linux-n3-ios-test.md)。N3-A1 安装包为 40.29 MiB，
版本 `0.10.0 (70)`，构建编号 `N3-A1-iOS-20261009-121807`。
Xcode Release 构建、代码签名完整性、IPA 归档、JIT 助手断开与执行异常恢复
检查均通过。手机尚未运行本包。

## 后续开发：动态代码

用户当前仅有 Mac 上的 iLoader。N3-A1 归档只有本地 ad-hoc 签名，缺少
TeamIdentifier 和 provisioning profile。HTTPS 托管可提供下载，当前归档
仍需侧载工具签名并安装。此次继续开发的授权不依赖远程安装是否可用。

先增加在 JIT 准备期间授权的动态执行区域。Linux 保持文件、映射、进程和
权限语义，客体代码通过可写别名生成指令，显式提交后更新宿主执行地址。
ART ARM64 编译器的 `SetupBlockedRegisters()` 已保留 x18；动态区域先接收
遵守 NCE 调用约定的机器码，系统调用和 TLS 操作继续通过已适配的运行库。

回归需要覆盖更新后执行、共享文件别名、fork 和进程切换、取消映射后的访问，
以及无效地址和权限拒绝。然后接入 ART 代码缓存，再修改 ARM64 编译器和
相关汇编中的对象引用访问。现有解释执行负载继续作为功能回归。

本机当前没有可用的 Xcode 模拟器运行时。先使用已有 CLI/GCD 原生回归；
需要验证 iOS 界面与生命周期时再安装模拟器。设备的 JIT 授权与内存限制
继续由后续合并真机测试确认。

### 动态执行区实现与首轮回归

动态代码使用预先授权的 32 MiB 执行区，首个 16 KiB 页面供宿主恢复寄存器。
相邻的 32 MiB 地址范围留给 ART 代码元数据；代码头使用向后的 32 位偏移访问
stack map，因此元数据地址必须低于代码地址。这两段位于预注册 ELF 之后，
普通 mmap 从其后开始分配。

架构系统调用 243 将 memfd 的指定文件范围映射到执行区；244 将 Linux 页面
内容复制到对应宿主执行地址并同步指令缓存。写入由执行区外的共享可写映射
完成，调用者负责协调代码写入和执行。当前接口接收遵守 NCE 约定的 ARM64
机器码；动态代码中的 x18、SVC 和 TLS 指令尚未提供自动改写。

首轮 CLI 与 GCD 回归通过了 128 次代码更新、文件偏移、fork 共享更新、子进程
同地址替换后的父进程恢复、权限拒绝、取消映射和一千万次动态代码循环。
原有 ART Java、Binder、Linux 进程与内存回归也通过。后续回归加入了动态代码
内部等待 Linux 信号、处理后返回同一段代码的测试，两种入口均通过。

执行恢复入口需要位于目标代码的直接跳转范围内。动态区首个页面提供相邻
恢复入口，保留完整客体寄存器；内核分配器跳过该页面。memfd 的 pwrite 测试
另发现内核需要启用 TMPFS，配置已加入构建脚本。

### ART JIT 集成进展

已构建 Android 16 上游 ARM64 编译器及其固定版本 VIXL 依赖，并让代码缓存
使用系统调用 243/244。方法头、机器码与 stack map 继续采用 ART 格式。
运行时保留压缩引用，编译器在访问对象字段或数组时才计算高地址。

第一轮计算方法回归已经通过 CLI 和 GCD 两种入口。CLI 的两百万次整数循环
结果为：解释执行 168.682 ms、JIT 执行 2.688 ms，首次编译 0.289 ms。
两种执行方式均与独立 C 计算得到的 `de0adba4164b790e` 一致。测试还确认方法
入口确实位于 JIT 代码缓存。这里记录的是本机计算负载，完整 Android 和手机
性能仍待后续验证。

动态代码的指令缓存同步由宿主在提交时完成。ART 原有刷新函数读取 `CTR_EL0`
会在本机触发非法指令异常，代码缓存已改用宿主同步；共享数据页保留普通内存
一致性语义。

已加入对象字段读写、GC card table、高地址数组访问和 NEON 数组寻址适配。
新增负载包含 32,768 次数组更新、两千万次对象字段遍历、64 个循环对象、
8 次 GC 请求，以及编译代码抛出的空引用和越界异常。GC 扫描寄存器引用时，
现在先解码 32 位偏移，扫描完成后编码回写。CLI 和 GCD 回归均通过上述负载。
该轮计算方法的解释/JIT 耗时分别为 CLI 167.104/2.807 ms、
GCD 167.385/2.772 ms，均属于本机微基准。

编译代码、解释器与 JNI 之间的对象参数和返回值已通过 CLI/GCD 回归。测试
关闭内联，检查五种对象返回和四种空引用返回，并通过 JNI 比较对象身份。
C++ 运行时的 `JValue` 保留原始指针；ARM64 quick 调用约定使用压缩引用，
在进入与离开 C++ 时转换。

JIT 已改为检查优化后的指令。普通方法可按热点计数自动编译，方法名不再
决定能否编译。20 万次调用的自动编译回归通过，测试确认方法入口位于代码
缓存，并检查不支持的方法仍可解释执行。未适配指令会使整个方法继续解释
执行；当前也暂不编译 OSR 和 JNI 专用机器码入口。

编译代码中的对象/数组分配和虚方法调用已通过 CLI/GCD 回归。JIT root table 的表项仍是
32 位压缩引用，表项地址需要完整的 64 位地址；分配入口接收和返回原始指针，
调用两端需要转换。RosAlloc 的汇编快速路径也需要在对象头和分配记录中保存
编码后的引用。回归覆盖 4 万轮对象/数组分配负载、两个虚方法目标、负数组
长度与空接收者异常，以及分配对象在 GC 后的内容检查。

### 当前 JIT 范围与后续工作

| 范围 | 当前结果 |
| --- | --- |
| 动态代码映射、更新、fork、信号返回、释放 | CLI/GCD 通过；手机预授权执行区待合并验证 |
| 整数计算、基本控制流、原始类型数组、对象字段 | 已进入 ARM64 JIT 执行并核对结果 |
| 对象参数、对象返回、JNI、虚方法调用 | 已检查实际调用边界；调用回归关闭内联 |
| 对象和数组分配、GC 扫描编译栈 | 已通过当前 MarkSweep/RosAlloc 配置的负载 |
| 热点自动编译、未适配方法继续解释执行 | CLI/GCD 通过；不再依赖特定方法名 |
| 引用数组、接口调用、编译后的 monitor 与 catch/throw | CLI/GCD 通过类型错误、接口冲突、重入和异常释放锁检查 |
| 简单构造函数、字段访问的预编译入口 | 11 个入口已实际安装并通过对象身份、字段值和 GC 检查 |
| 字符串长度、字符访问、常量引用、普通字符串方法调用 | CLI/GCD 通过 ASCII、UTF-16、空字符串、空引用和越界检查 |
| 首次类初始化与初始化失败 | CLI/GCD 通过静态对象、静态计数及两次失败访问检查 |
| 类型检查专用指令、OSR、JNI 专用机器码入口 | 尚需适配；包含这些未支持指令的方法保留解释执行 |
| AOT、Zygote 与多应用代码缓存 | 尚未完成集成验证 |
| 完整 Android 服务、图形显示和持续交互 | 尚需实现 |

本轮 ART 代码缓存上限设为 32 MiB，其中代码和元数据各占一半。当前预授权
执行范围为 32 MiB；其中首个宿主页由执行恢复入口使用。后续 Zygote 与应用
同时使用代码缓存时，需要再次检查容量和进程切换成本。

接下来的开发顺序是扩展上述 Java 编译路径、补齐实际 Android 程序所需的
ELF/系统调用支持，再接入系统服务与图形。当前的约 60 倍来自 Java 整数循环
的 JIT/解释器对照；整套 Android 的启动、交互、发热和能耗还没有测量结果。

### 复杂度审查

本轮使用上游 ART 编译器、代码缓存和 Linux memfd，继续由 Linux 管理地址
分配与页面生命周期。两个架构系统调用分别负责执行映射和代码提交，宿主
只维护 iOS 所需的预授权执行映射及指令缓存同步。

已删除原来的解释器构建替代文件和生成的简化 intrinsics 输出，改用上游
编译器实现。保留动态执行区专用恢复入口：普通恢复入口到动态代码的距离
曾超过 ARM64 直接跳转范围，取消这部分会使动态代码的信号返回失败。
保留 GC 引用转换和 JIT root table 地址宽度适配：对象引用与表项地址的存储
契约不同，两者分别由 GC/对象回归和编译分配回归检查。

指令过滤在优化之后执行，覆盖内联产生的指令；已关闭会绕过过滤的快速编译
入口。遇到尚未适配的指令时，保留该方法的解释执行并停止反复尝试编译。
后续扩大支持范围时，应同时增加对应行为回归并调整过滤条件。

### N3-J1 合并回归与产物

最终 CLI/GCD 回归均通过。两百万次整数循环的解释/JIT 时间分别为
160.917/2.642 ms 和 156.771/2.648 ms；独立 C 校验值一致。自动编译测试
先确认方法尚未编译，再执行 20 万次调用并检查 JIT 入口。不支持的 monitor
方法在拒绝编译后再次执行，返回对象身份保持一致。

12 项 ELF/构建检查、JIT 助手断开检查和执行异常恢复检查均通过。iOS 构建、
签名完整性和 IPA 归档检查通过。合并包为 `Husk-NativeLinux-N3-J1.ipa`，
版本 `0.11.0 (71)`；构建编号和 SHA-256 见手机测试文档。真机结果待确认。

Xcode Simulator 暂未安装：当前 CLI/GCD 已覆盖主要执行路径，模拟器仍无法
确认设备上的 JIT 授权和预授权映射限制。若后续需要验证 UIKit 生命周期、
界面或系统 API 差异，再按需安装运行时。当前仅有电脑 iLoader，HTTPS 网页
可提供 IPA 下载，实际安装仍需有效签名和设备安装通道。

## 2026-10-09 持续开发安排

用户外出期间继续本机开发与验证，目标是在北京时间 20:00 前持续推进，届时
汇总实现、回归结果、产物和剩余问题。真机安装暂时不作为开发前置条件。

当前可以独立推进三部分工作：先扩大 ART JIT 的实际 Java 语义覆盖，包括
引用数组、接口调用、同步与异常处理，并加入组合负载；然后检查 Android
动态库加载和进程运行所需的限制；再按依赖推进系统服务。根据每阶段的运行
结果调整顺序，代码与结果在本仓库记录，可本地提交。

设备相关问题保留到合并验证：新增预授权执行区在调试器断开后的执行权限、
iOS 内存限制，以及持续运行时的温度、耗时和交互表现。CLI/GCD 使用本机
ARM64 执行同一份 Linux、Bionic、ART 与生成代码，适合继续检查调用约定、
内存访问、GC 和异常语义。现有测试不能确认上述设备条件。

### 引用数组、接口与同步路径回归

引用数组的类型检查、存储和慢速调用已适配到高地址对象堆，同时保留
ART 的类型错误和 GC card table 处理。接口调用需要展开接收者与类对象
地址，进入 C++ 解析函数之前也需要转换接收者；保存到托管调用栈的引用
继续使用 32 位编码。

同步代码依赖异常处理。生成代码进入 monitor 和抛出异常的运行时入口前
转换对象指针；读取 Thread 的异常字段时使用完整指针并编码返回，清除时
写入完整字段宽度。新增回归已检查数组协变、错误类型、接口返回值、重入
同步、异常释放锁和并发更新。

本轮本机执行发现 ART 的 `SmallPatternMatcher` 会为简单构造函数和字段
访问安装预编译入口。其原有参数使用原始对象指针，NCE 调用方传入压缩
引用，导致构造函数写入低地址。已同步适配这些入口的对象参数、字段值
和对象返回，并通过实际安装入口后的行为检查覆盖这条执行路径。

本轮 `NCE_STRESS_ROUNDS=3 python3 scripts/probe_lkl.py --processes` 的 CLI/GCD
均通过；iOS 目标编译、12 项 ELF/构建检查和原有 Linux/Binder/ART 回归
也通过。每种入口均检查了 256 个引用数组元素、两个接口实现、256 次带
方法表冲突的调用、4,000 次同步调用产生的 20,001 次计数更新，以及六种
异常处理。两种入口的 NCE 上下文均分配 415 个、释放 415 个。

预编译入口检查覆盖 11 个实际入口：两种构造函数，整数、长整数、浮点数
和对象字段的读写，以及静态对象字段读取。静态长整数字段读取另由 ARM64
JIT 执行。测试在安装静态入口前等待 ART 的类初始化可见性检查点，原有
安装条件得到保留。未适配类型检查的方法继续解释执行，自动编译回归通过。

本轮复杂度审查保留两个具体转换位置：生成代码访问对象时计算地址，预编译
函数按托管调用约定转换参数与返回值。删除任一部分都会使对应实际负载访问
错误地址。复用 ART 原有 monitor、异常分派、接口表和字段访问实现；新增的
入口检查函数只用于回归。构建顺序调整为 CLI 回归通过后再编译 iOS 目标，
使运行失败更早返回，完整验证内容保持一致。

手机交付包仍为 N3-J1；这轮扩展继续本机开发，后续合并打包。下一步检查
字符串访问、首次类初始化和实际 Java 组合负载。

### 字符串与类初始化回归

ARM64 生成代码已适配字符串的 ASCII 压缩和 UTF-16 存储，支持常量及变量
索引。字符串根对象继续由 ART 的 JIT root table 保存。首次类初始化的
快速检查、慢速初始化调用、类/字符串解析返回，以及方法入口的初始化检查
均按高地址对象引用处理。运行时空结果检查使用完整指针宽度。

受限模式已关闭专用 intrinsic 代码生成。编译过滤现在检查实际的
`Intrinsified()` 状态，允许通过普通方法入口调用已识别的 intrinsic 方法。
仍未适配的专用指令保持原来的拒绝条件。

CLI/GCD 回归均通过八种字符串、10 万轮字符串/数组/接口组合负载和八次 GC
请求。首次类初始化只执行一次，静态对象在 GC 后保持身份；初始化抛出异常
后，再次访问得到预期错误，初始化计数仍为一。空字符串、零字符、非 ASCII
字符、UTF-16 代理项，以及空引用和两种越界情况均得到检查。

本轮继续运行 3 轮 Linux 用户态压力回归和 12 项 ELF/构建检查，iOS 目标
编译通过。CLI/GCD 的 NCE 上下文均分配和释放 416 个。JNI 创建至 VM 销毁
分别耗时 693.441 ms、708.030 ms，包含已有全部 ART 功能负载；这两个数值
来自 Mac，手机耗时待集中验证。日志为
`work/native-linux-nce/art-strings-clinit-probe.log`。

复杂度审查移除了 ArraySet 中单独处理原始类型的 NCE 分支，原始类型和引用
类型共用已经适配的地址计算。原有数组更新回归继续通过。新增引用转换函数
用于运行时返回到托管代码的具体调用边界，未引入新的执行后端或调度层。

下一步接入平台 Binder 库、`servicemanager` 和 Unix socket，使用实际服务
注册与跨进程调用检查 Android 系统服务所需行为。SDK 提取范围增加 `bin`
和 `framework`；下载与提取完成后清理中间镜像，运行材料仍放在项目 work
目录。

## Android 平台服务接入（2026-10-09，进行中）

下一组验证使用 SDK 镜像中的原版 `servicemanager`、`libbinder.so` 和
`libbinder_ndk.so`。新增测试进程通过平台 AIBinder/AParcel API 注册服务、
查找服务、发送不同长度的数据、传递文件描述符，并接收服务退出通知。
现有直接操作 Binder ioctl 的测试继续检查驱动边界。

平台 `Access` 构造函数会调用 `selinux_status_open` 和 `getcon`。因此本轮
启用 Linux 的 NET、UNIX、INET、AUDIT、SECURITY 和 SELinux，并挂载
sysfs、selinuxfs。当前使用内核的开发配置，完整 Android 策略仍待接入。
这些设置只作用于进程内的 Linux 客体。Unix socket 测试另行检查跨进程
SCM_RIGHTS、SCM_CREDENTIALS 和接收端 FD_CLOEXEC。

系统镜像提取扩展到 bin 和 framework；构建仍只打包实际测试需要的文件和
共享库依赖。平台头文件固定到 frameworks/native 提交
`2827a4a16b0340ecd07c2d5a6c89991799b362bb`，下载时校验 SHA-256。
servicemanager 的 `/system/bin/bootstrap/linker64` 指向已适配的 Android
动态链接器，ELF 检查只增加这个已登记的解释器路径。

首次编译发现 SELinux 的宿主生成工具需要 Linux UAPI 的 asm 类型头文件。
已将头文件导出移到内核编译之前，并通过 `-idirafter` 提供给宿主编译器。
运行结果将在本轮验证完成后记录。

平台服务运行已确认以下前置条件：

- binderfs 会自动创建配置中的 `binder` 节点。测试改用独立的 `nce-platform`
  节点，并将 `/dev/binder` 指向该节点。
- Unix socket 凭据和文件描述符传递已通过。SELinux 状态映射也已进入运行。
- `BackendUnifiedServiceManager` 会等待 `servicemanager.ready=true`。
  当前缺少属性区时，Bionic 的属性等待反复读取时钟，服务注册无法继续。
  因此增加属性服务原型，由 Bionic 创建和更新共享属性区，提供属性协议 v2
  的 Unix socket。servicemanager 通过原有 `SetProperty` 发布就绪状态。
- 属性信息文件由 AOSP `libpropertyinfoserializer` 生成。当前原型使用一个
  属性 context，只接受 root 客户端；名称上限 255 字节，值上限 4095 字节，
  普通属性仍受 `PROP_VALUE_MAX` 限制。`ro.*` 创建后只读，`ctl.*` 返回控制
  消息错误。完整的 init 属性权限、持久化和控制消息处理仍待实现。
- 特殊文件的 mmap 会经过 ELF 布局识别。新增 `read_iter` 检查，跳过无法
  使用 `kernel_read` 的文件，保留其原有 mmap 实现。

跟踪日志中反复出现的时钟调用已汇总计数，原始大日志已清理。

属性共享区及 `ro.*` 拒绝更新已通过，随后真实服务注册返回 `EX_SECURITY`。
日志确认原因为 SELinux 尚未加载 `service_manager` 类定义。现增加原型
CIL 策略：复用 SDK 的类、权限和初始 SID 声明，给测试进程配置 `nce_t`
域，分别定义允许注册的 `nce_service` 和拒绝注册的 `nce_denied`。
Android 自带 secilc 在客体内编译策略，测试写入 selinuxfs 后启用 enforcing。

策略加载和 enforcing 状态检查已通过。首次运行发现属性文件改标签还需要
文件类型到 tmpfs 的 `filesystem associate` 权限，已加入该规则。当前策略
只服务于这组原型进程；Android 的应用域划分和完整平台策略仍待实现。

进一步检查发现通用文件 ABI 错误：LKL 的 fcntl 头文件沿用了 asm-generic
定义，AArch64 用户态的 `O_DIRECTORY`、`O_NOFOLLOW`、`O_DIRECT`、
`O_LARGEFILE` 值与之不同。`service_manager/index` 可读，权限目录的
`opendir` 则返回 EINVAL。已加入 AArch64 fcntl 定义，由 LKL 内核直接
采用正确标志值。

新增同一组文件边界测试，分别用 musl 和 Bionic 编译执行：目录打开、普通
文件加 O_DIRECTORY 返回 ENOTDIR、符号链接加 O_NOFOLLOW 返回 ELOOP、
O_PATH 保留符号链接类型、FD_CLOEXEC，以及 readdir 的目录内容。SELinux
测试另行检查 service_manager 的编号和三个权限文件。平台测试为 PID 1
显式安装 SIGALRM 处理，超时以 124 退出并记录原因。

文件测试还触发了 `brk` 布局问题。musl 的 `opendir` 首次分配小块内存时，
堆从程序尾部增长，进入相邻程序预先准备的代码区；Linux 缺页处理返回成功，
宿主仍保留代码页的只读权限，导致同一写指令不断缺页。现将 brk 起点移至
动态执行区之后，普通 mmap 起点再后移 64 MiB。该间隔只占虚拟地址，实际
内存仍由 Linux 按需分配。新增 8 页堆增长、读写、fork COW 和缩回测试。
堆测试及 musl/Bionic 文件测试已通过，原有 ART 负载继续通过。

平台服务现已完成 SELinux enforcing 下的允许和拒绝注册、服务查找、32 轮
跨进程数据校验（最大 64 KiB）和文件描述符传递。退出通知测试曾在无事件时
调用 `ABinderProcess_handlePolledCommands`，该函数会阻塞；现仅在 poll
报告可读后调用。

同次排查发现 Bionic 的信号处理未提供 SA_RESTORER，原型内核原先只接受
libc 提供返回入口，定时器信号因此变成 SIGSEGV。新增固定登记的信号返回
ELF，每次 exec 由 Linux 映射，fork 继承映射和返回地址。SA_RESTORER 存在
时继续使用 libc 的入口。共享信号测试同时编译给 musl 和 Bionic，覆盖嵌套
信号、备用栈、寄存器恢复、EINTR/SA_RESTART、缺页恢复、非法信号帧及定时器。
Bionic 未实现 pthread_cancel，取消线程测试只在 musl 中运行。本轮完整验证
仍在进行。

本轮 CLI、GCD 完整回归通过，包含 3 轮用户态压力测试、13 项 ELF/构建检查
和 iOS 目标编译。两种入口均分配并释放 434 个 NCE 上下文；平台 Binder
服务退出通知、资源清理、6 种属性协议拒绝请求和跨进程属性等待唤醒均通过。
ART 的全部既有负载继续通过。运行日志为
`work/native-linux-nce/android-services-probe.log`，完整结果和源文件校验值见
`work/native-linux-nce/processes/lkl-summary.json`。本轮未运行手机测试。

复杂度审查保留了 Bionic 的属性区实现和原版 Binder 库，原型只提供当前缺少
的属性 socket 服务。信号返回入口共用已登记 ELF 的加载与指令改写流程。
删除了未使用的 mm context `end_brk` 字段；程序文件改在测试开始时写入一次，
压力循环继续检查文件内容，省去每轮重复写入。Binder 客户端的 ProcessState
在退出前持有驱动文件，因此测试使用 MNT_DETACH 分离挂载点，随后由进程
退出释放文件和映射。

下一步运行原版 `app_process64`，接入 framework Java 类和 JNI 初始化。
初步依赖检查找到 236 个可执行文件及共享库，当前提取材料均已包含；部分
库的函数符号描述超出 text 段，需先核对 CFI 跳转表等实际指令布局。后续仍
在 Mac 上完成 Linux 客体集成，设备验证留到集中测试阶段。

## app_process 与 framework 接入（2026-10-09，进行中）

新增目标使用 SDK 原版 `app_process64`、`libandroid_runtime.so` 及其依赖，
启动 `NceFramework` Java 测试。测试准备检查 Parcel 序列化、HandlerThread
延时消息、SharedMemory，以及 Java IBinder 到平台 NDK 服务的跨进程调用。
属性服务和 servicemanager 在这组测试期间保持运行，完成后统一回收进程。
当前还未验证 framework 测试完成。

`app_process64` 的 236 项可执行文件及共享库依赖均已找到，函数与 x18 指令
检查通过。18 个共享库的动态符号指向 CFI 分支表入口，符号长度仍使用函数
体长度，部分范围越过 text 末尾。构建器仅在入口确认为 AArch64 无条件 B、
目标落在已有函数范围时，将该入口按 4 字节扫描；其他越界符号继续拒绝。
新增测试覆盖正常入口、替换为 NOP、跳到 text 之外三种情况，14 项 ELF
检查通过。CFI 跳转表的设计可参考
[Clang CFI 文档](https://clang.llvm.org/docs/ControlFlowIntegrity.html)。

启动参数对照 AOSP frameworks/base 提交
`99b01a65cc4c104933788b3143285ab6bae65827` 的
[AndroidRuntime.cpp](https://android.googlesource.com/platform/frameworks/base/+/99b01a65cc4c104933788b3143285ab6bae65827/core/jni/AndroidRuntime.cpp)
和 `app_main.cpp`。沿用已适配 ART，提供实际 boot class path、属性区和
动态链接器配置。日志测试库在 exec 后设置 liblog 的 stderr 输出，便于
读取平台 JNI 初始化失败的位置。

资源增加后，原有 256 MiB Linux 内存配置触及 rootfs 的 tmpfs 默认容量。
写入 ICU 文件时只完成 14,745,600 / 29,094,000 字节，测试在此停止。Linux
的 `shmem_default_max_blocks` 默认取可用页数的一半；现将客体内存配置改为
512 MiB，后续实际内存占用以运行测量为准。

程序和 Java 资源改由一个 `payload.bin` 与 Mach-O 汇编 `.incbin` 嵌入。
原来约 550 MiB 的两份 C 数组头文件缩至约 1 MiB；当前二进制资源约 185 MiB。
程序注册信息、指令改写表和资产路径仍采用原有结构。CLI、GCD、iOS 编译及
IPA 打包均已调整为链接同一份资源，校验清单同时记录生成汇编和二进制的
SHA-256，运行测试继续检查客体文件字节。

原版 `libcrypto.so` 在动态库初始化时执行 FIPS 代码完整性校验，NCE 的指令
改写会改变参与校验的 text 字节，导致初始化中止。现从 Android BoringSSL
提交 `1e3b727981831886a0910cd9e82d5bbee4cc7029` 构建 `FIPS=OFF` 的
libcrypto/libssl，保留 ARM64 汇编实现。源码归档与产物均记录 SHA-256；
该构建不具有原版库的 FIPS 认证状态。客体已通过 BoringSSL 全部内置已知
答案自检、SHA-256、AES-128-GCM 已知结果、解密和错误标签拒绝测试。

`app_process64` 已创建 ART 并进入 `AndroidRuntime::startReg`，HWUI 初始化
通过 dlopen 查找 `libandroid.so`，该库未出现在初始 DT_NEEDED 依赖集合。
现将其加入运行资源，继续验证后续 JNI 初始化。

进一步启动暴露了 `ro.build.version.sdk_full`、TrafficStats 和 DeviceConfig
依赖。版本属性与固定 SDK 的 `/system/build.prop` 对照，新增 tethering、
configinfrastructure、tzdata APEX 资源和对应的动态链接器命名空间。ART
增加 `-verbose:class` 下的缺失 boot class 名称日志，常规测试关闭详细输出。
原版 `RuntimeInit` 随后已调用 `NceFramework.main`，Parcel 和 HandlerThread
步骤已执行，共享内存创建触发了下一处内核兼容问题。

Android libcutils 通过 `ro.treble.enabled`、`sys.use_memfd` 和
`ASHMEM_GET_SIZE` 探测 memfd。原型内核已支持 memfd 与 seals，新增仅用于
memfd 的大小查询 ioctl，普通 tmpfs 文件和其他 ioctl 返回 ENOTTY。musl
和 Bionic 共用测试覆盖初始大小、两次 resize、未知命令、普通文件拒绝及
F_SEAL_FUTURE_WRITE。共享内存读写与只读别名由 Framework 测试继续验证。
AndroidRuntime 的退出路径还会打开 hwbinder，现由 binderfs 创建独立节点，
与平台 Binder 分别清理。当前尚未启动硬件服务管理器。

本阶段 CLI 与 GCD 完整回归均通过，包含 3 轮压力测试、14 项 ELF/构建检查
和 iOS 目标编译。两种入口均完成 Framework 的 Parcel、Looper、SharedMemory
和 Java→NDK Binder 测试，并分配、释放各 444 个 NCE 上下文。加密库检查及
全部既有 ART 测试继续通过。运行日志为
`work/native-linux-nce/android-framework-probe.log`，源文件与产物校验值记录于
`work/native-linux-nce/processes/lkl-summary.json`。当前资源为 201,850,832 字节。
本轮未运行手机测试；尚未验证 Android 桌面、系统服务启动及图形显示。

复杂度审查保留 SDK 的 app_process、Framework、JNI 和 Binder 实现，通过
真实调用检查依赖。内核只增加 libcutils 实际要求的 memfd 大小查询，其他
ashmem ioctl 仍返回 ENOTTY。资源改为单个二进制文件，减少生成 C 数组的
编译负担，路径和字节校验继续由原有程序注册信息完成。源码构建的密码库
保留汇编与已有接口。探索阶段重复提取的 APEX 已与正式文件逐项核对后删除。
下一步验证 Framework 图形绘制，并扩大 ART 类型检查等常用 JIT 路径。

## Framework 图形绘制（2026-10-09）

原版 Android Canvas / Paint / Path 已完成矩形、圆形、裁剪、平移和路径绘制，
测试逐点检查颜色，再进行 PNG 编码、解码和 Bitmap.sameAs 比较。CLI 与 GCD
生成的图片字节完全一致，图片见
`work/native-linux-nce/processes/framework.png`。iOS 测试界面增加绘图结果展示
与图片导出，运行时状态增加系统服务和 Framework 阶段。

3 轮完整回归、14 项 ELF/构建检查及 iOS 目标编译继续通过。N3-F1 IPA 已完成
Xcode 构建、签名结构和 ZIP 完整性检查，待设备运行。当前图形测试覆盖 CPU
绘制与图片编解码；窗口系统、GPU 后端和桌面合成尚未接入。此阶段继续本机
开发，设备测试安排在后续集中验证。

## ART 类型检查 JIT（2026-10-09）

`instanceof` 与类型转换现已纳入 ARM64 JIT。类指针、父类、接口表、数组元素
类型和类型位串读取使用高地址堆；调用运行时检查时先把对象和类参数还原为
原生指针。接口检查保留直接遍历接口表的实现，异常路径继续使用 ART 的
ClassCastException 处理。

12 个输入值与 10 种目标类型组合覆盖最终类、抽象父类、普通父类、继承接口、
对象数组、接口数组、原始类型数组、二维数组、Cloneable 和 Serializable。
12,012 次 instanceof 方法调用与反射结果一致；类型转换中 41 次合法、79 次
抛出 ClassCastException。另执行 64 次方法内部捕获，并穿插 GC 后检查存活
引用。原来用于检查编译拒绝的类型转换已可编译，回退测试改用尚未接入的
String 构造调用，并核对解释执行生成的 UTF-16 字符。

CLI、GCD 的 3 轮完整回归、14 项 ELF/构建检查和 iOS 目标编译均通过；Framework
与 Canvas 测试继续通过，日志为 `work/native-linux-nce/android-types-probe.log`。
本轮没有据此推算手机上的界面性能。复杂度审查复用了既有引用加载和运行时
参数转换函数；接口表的局部原生指针只在无运行时调用的遍历中使用，方法的
输入、输出和栈映射继续保留压缩引用格式。

下一步接入文件支持的 Linux 块设备与 ext4，验证卸载后重新挂载、内核重启后的
数据保留，再逐步迁移当前内存文件系统中的系统资源。Mac 只承担构建与本机
测试，设备运行时使用 iPhone 应用目录内的文件。

## ext4 持久化存储（2026-10-09）

新增 Linux 块设备 `ncedisk`，通过宿主应用目录内的普通文件读写扇区。Linux
负责 ext4、页缓存、日志和块请求；宿主回调处理 pread、pwrite、fsync、短读写
与 EINTR，并拒绝超过镜像边界的请求。块驱动提供 flush 与 FUA 的持久化语义。

64 MiB ext4 测试镜像通过 CLI 与 GCD 的两次独立内核启动。第一次写入 1 MiB
确定内容，第二次重新读取并逐字节校验，持久化计数分别为 1、2。测试覆盖
MAP_SHARED 写回、MAP_PRIVATE 隔离、扩容补零、截断、重命名、硬链接、用户
扩展属性、卸载后只读挂载和写入拒绝。完成后 e2fsck 检查通过；全部既有
进程、ART、Binder、Framework 和 Canvas 回归继续通过。

iOS 安装包携带空白 ext4 文件，首次运行复制到 Application Support/NativeLinux，
后续启动复用。该目录设置为不参与云备份。此阶段尚未将 Android 系统资源从
tmpfs 移出，内存占用的变化需在下一步迁移后测量。块设备当前采用同步 I/O，
后续是否增加异步队列取决于实际负载；当前实现只保留已经用到的单设备接口。

## 只读系统镜像与文件缓存回收（2026-10-09）

块设备扩展为两个固定用途：`ncedisk0` 保存可写数据，`ncedisk1` 读取安装包内
的只读系统镜像。宿主使用文件锁拒绝并发写入同一数据镜像，并在宿主与 Linux
两侧拒绝系统镜像写入。单测覆盖无效镜像大小、边界读写、偏移溢出、无效操作、
关闭重开、只读写入拒绝与文件锁。

`/system`、`/apex` 现通过 ext4 提供，ELF 和 JAR 按普通 Linux 文件装载。嵌入
宿主可执行文件的资源从约 192.5 MiB 缩减到约 2.5 MiB，只保留根目录测试程序、
小型运行配置和系统 ELF 的头部信息。完整系统文件由只读镜像保存；回归使用
构建时 SHA-256 核对每个 ELF，确认装载与指令改写没有改变文件。

CLI 与 GCD 的完整回归均通过。CLI 完成 Framework 后，文件缓存为 169,552 KiB；
执行同步和 drop_caches 后降至 3,072 KiB，随后再次读取、校验所有 ELF 成功。
这验证了系统资源可以从 Linux 页缓存回收后重新读入。此处还未接入将 Linux
空闲物理页交还 iOS 的处理，因此这些数字只描述 Linux 内部内存状态。CLI 的
宿主峰值 RSS 为 575,750,144 字节；手机上的内存与运行时间仍待集中验证。

复杂度审查采用固定的系统盘、数据盘两个槽位，复用原有 ELF 登记、挂载、哈希
校验与回归入口。没有增加通用存储插件或异步请求管理。系统资源的临时构建
目录在生成镜像后删除。下一步推进 Zygote 启动、命令处理和 Java 进程派生。

## Zygote 启动与命令处理（2026-10-09）

原版 `app_process64 -Xzygote` 已启动 ZygoteInit，并在独立挂载命名空间中处理
命令。测试客户端使用 Linux system UID 1000，连续查询 ABI、PID、ABI，校验
回复与实际 Zygote 进程一致，再终止并回收进程。CLI、GCD、3 轮压力回归及
14 项 ELF 检查通过。运行日志为 `work/native-linux-nce/zygote-probe.log`。

启动资源增加 statsd 的 framework-statsd.jar、JNI 库和链接器命名空间。
JDWP 使用原版 `dalvik.vm.jdwp-provider=none` 配置；当前测试没有调试连接
需求。启动参数与固定 SDK 的 init.zygote64.rc 对照。测试采用原版的延迟
预加载参数，后续还需验证 Java 进程派生、cgroup、seccomp 和应用初始化。
当前尚未运行完整类/资源预加载，系统桌面与 system_server 尚未启动。

构建增加 ELF 扫描缓存，键包含 ELF 路径、文件 SHA-256、构建脚本 SHA-256
和 llvm-objdump 版本。文件内容或检查规则变化会重新扫描指令；执行布局范围
仍在每次生成时检查。首次全扫描与后续缓存构建都完成了相同的运行回归。
缓存只保存解析结果，完整系统文件继续通过生成清单与运行时 SHA-256 校验。

## Zygote 派生与进程隔离（2026-10-09）

原版 Zygote 已调用 fork 产生子进程。首次派生触发堆压缩，暴露出 ART LockWord
转发地址仍保存原生指针低 32 位的问题。转发地址现复用堆内偏移编码；读取时
恢复原生地址，Zygote 已完成压缩与派生。

Linux 配置启用 cgroup v2、memory controller 与 seccomp filter。启动测试调用
SDK 的 CgroupSetup，按项目配置挂载 cgroup 并建立 system/apps 目录。单 CPU
原型的前台调度配置目前调整 timer slack；后续 CPU 拓扑与资源分配配置需要
随实际调度支持扩展。

seccomp 使用 Linux 原有 BPF 过滤与信号实现，LKL 架构层提供 ARM64 系统调用
号、参数、返回值、程序计数器和线程标志。Android 平台适配保留 SDK 导出的
ARM64 过滤表，并显式加入代码映射调用 243–245；客体自行安装的过滤器仍可
拒绝这些调用。测试覆盖 errno、非法调用号、SIGSYS 上下文、fork 继承、进程
终止和 TSYNC。首轮测试发现 LKL 的 KSTK_EIP 固定为零，已改为读取客体寄存器，
CLI 与 GCD 的完整回归均通过。宿主定时调度另使用 nce_user_preempt 入口，
直接调用 Linux 调度器；只有客体发出的系统调用经过 seccomp。

两个子进程均以 UID 1000 运行，检查自身 cgroup 路径和 seccomp mode 2；分别
创建并校验 4,096 个字符串，经过 GC 后继续读取，启动 Java 线程，完成 Parcel
往返，并同步写入结果。System properties 中的子进程标记没有出现在下一次
派生的进程中。测试 JAR 位于只读系统镜像，Binder 节点允许普通 UID 打开，
服务访问仍经过 SELinux 检查。

动态可执行区扩大为 64 MiB，使 Zygote 的共享 JIT 区与子进程的私有 JIT 区可
同时映射；每个 ART 实例的 JIT 容量配置保持 32 MiB。测试期间 CLI 宿主峰值
RSS 为 634,667,008 字节。代码缓存目前提前填充全部可执行页，后续将检查按需
填充与宿主物理页回收，降低手机内存压力。

本轮同时通过 iOS 目标编译、3 轮进程压力回归和 14 项 ELF/构建检查。日志为
`work/native-linux-nce/zygote-security-probe.log`。复杂度审查保留原版 Zygote、
libprocessgroup 与 BPF 解释器，只增加实际消费的架构入口和原型配置；尚未
建立 Android 应用间完整的安全策略，system_server 与系统桌面仍待实现。

## 宿主 RAM 回收（2026-10-09）

接入 Linux page reporting。内核先从 buddy allocator 暂时移出空闲块，宿主
替换 RAM 的存储映射与 Linux 直接映射，完成后由内核把页放回空闲列表。
最小块为 256 KiB；页选择、隔离、批处理和重复上报标记均复用 Linux 实现。
客体数据映射失效时同时释放宿主别名，避免旧别名继续持有已释放的物理页。
可执行映射保留既有的固定地址处理。

CLI 完成 Framework/Zygote 后的宿主 footprint 为 607,978,696 字节。清理文件
缓存并等待页上报后降至 297,665,640 字节，减少约 296 MiB。GCD 对应结果为
609,862,880 → 298,550,448 字节。峰值 RSS 仍约 635 MB；当前变化解决了释放后
的内存归还，启动阶段的峰值还需要继续优化。日志中的 reclaimed_bytes 为
累计处理字节数，同一范围再次分配、释放后会重复计入。

回收后再次执行两轮 64 MiB 分配、写入、释放与等待，检查新匿名页补零，另保留
一个存活页并逐轮校验其内容。CLI、GCD、iOS 编译、3 轮进程压力回归与 14 项
ELF/构建检查通过。日志为 `work/native-linux-nce/memory-reclaim-probe.log`。
本机单独实验中 madvise 返回成功但没有降低共享 RAM 的占用，因此实现采用了
已验证的映射替换。手机上的物理内存统计仍待后续集中验证。
