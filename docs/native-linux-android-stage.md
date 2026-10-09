# 原生 Linux + NCE：Android 用户态阶段

更新日期：2026-10-09。goal 状态：进行中。

## 验收目标与暂停条件

本阶段交付需要同时完成以下结果，并形成可复现的本地回归和一个合并的
手机测试包：

1. 真实 Bionic 和 Android 动态链接器运行实际用户态程序。
2. Linux Binder 驱动完成跨进程请求与回复，检查数据、资源生命周期和失败返回。
3. ART 运行包含对象分配与回收、线程和 Java 异常的基础 Java 负载。

达到这组结果后，如确实需要确认设备执行条件，可以按用户授权暂停并安排
一次集中验证。提前请求设备验证仅限于设备特有条件阻断全部可推进路径，
且已经完成本地复现尝试、替代路径调查与问题记录。

局部实现继续使用本地回归，N2-E1 日志可以并行分析。交付前持续推进剩余
实现、失败场景和集成测试。最终验收仍需完整 Android 的持续交互数据。

## 实现顺序与当前状态

已完成 Linux 信号、musl 静态和动态用户态、dash、Binder 跨进程通信，以及
Bionic 动态程序。Bionic 测试覆盖线程、TLS、分配释放和 setjmp/longjmp。
下一步接入 ART 的共享库、启动文件和低地址堆。

后续根据实际程序需求扩展执行区、ELF 装载和 ABI。先保留 Linux 的进程、
文件、内存与 Binder 语义，宿主仅处理原生执行和 Darwin 所需映射。ART 的
执行区申请、代码更新、线程暂停和异常处理由实际负载驱动实现。

本地构建可使用 Mac，手机运行所需内容随包交付。源码、下载内容和构建目录
放在当前项目的 work 下。每次本地提交记录验证结果；文档随实现同步更新。

## 已知边界

当前接受构建时登记、宿主页兼容布局的 ELF。x18 作为客体状态保存；目前
支持已遇到的 MOV/ORR、两条 Bionic setjmp 掩码指令，以及含 x18 的 64 位
LDP/STP 偏移寻址。构建检查会拒绝其他 x18 指令。客体地址窗口为 16 GiB，
Linux RAM 为 64 MiB。执行区在 JIT 准备期间确定，通用可执行 mprotect、
ART 动态代码与运行时执行区复用仍需实现。

Bionic 的预编译 ELF 是否保留指令映射信息、共享库布局和 x18 使用方式，
需要逐个检查。ART 的内存需求、启动文件和内核接口由源码与实际运行确认。
当前还没有 ART 执行结果。

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

后续需要解除 ART ELF 的 2 MiB 单文件上限、支持其更大段对齐，并提供
满足压缩对象引用要求的低地址堆。代码更新与信号恢复仍按 Linux 状态验证。
