# 决策记录: 原 SDK 的 ashmem 设备

状态: proposed

## 问题

`sdk-plain-init-system-dmaheap-16k-v2` 的 CLI 和 GCD 原启动均记录 `ApplicationSharedMemory.nativeCreate` 抛出 `Failed to create ashmem: No such file or directory`，随后 Zygote 退出。原失败 openat 日志各有 30 次 `/dev/ashmem<boot_id>` 返回 `-2`，后缀与本入口实际 boot_id 相同；该日志没有 PID/TID，Java 日志的 PID/TID 与 system_server 相同。具体控制流与消息对应由[Zygote 退出记录](../bug-fix/2026-11-14-原SDK的Zygote反复退出.md)保存。

原 SDK `init.rc` 将 `sys.use_memfd` 设为 `false`。当前 LKL 6.12.0 支持 memfd、seals 和 libcutils 探测所需的大小查询，但源码与配置缺少 ashmem 驱动。原 ueventd 的 `HandleAshmemUevent` 对名为 `ashmem` 的 misc uevent 创建相同 major/minor 的 boot_id 后缀节点，普通节点同时保留。原 `/dev/ashmem*` 的权限为 `0666 root root`。

## 提案

接入固定 Android common 的 C ashmem 实现，保持原 SDK 的属性、rc、libcutils、JNI 和 SELinux 策略。原始驱动和 UAPI 保留许可证与固定修订，构建时复制到独立内核源码树；原始文件、适配脚本、生成文件、Kconfig/Makefile 和实际编译依赖进入构建标识。当前 common 6.12 已改用 Rust，先检查同版本的历史 C 实现；历史 C 实现需要适配 LKL 6.12 API 后，记录具体差异并验证实际行为。

### 固定来源与最小适配

选定 Android common 的 `android16-6.12-2025-06_r1`、修订 `2d954fcf3d1b73a41d0fa498324da357ec96cbdf`，来源内核版本为 6.12.23。原始三份源码、Kconfig、Makefile、页仿真头、COPYING 和两份许可证正文放在 [third_party/android-ashmem](../../../../third_party/android-ashmem/)，[sources.json](../../../../third_party/android-ashmem/sources.json)保存实际 URL、字节数与摘要。驱动为 GPL-2.0，两个头为 GPL-2.0 OR Apache-2.0，版权与 SPDX 原文保持。固定来源的只读核对见[研究报告](../../../../../../../sdk-plain-validation-v1/ashmem-kernel-research-v1/report.json)，Apache 原文的实际 `LICENSES/dual` 路径见[追加报告](../../../../../../../sdk-plain-validation-v1/ashmem-kernel-research-v1/license-addendum-v2/report.json)；首次 `LICENSES/preferred` 的 HTTP404 保留。

生成的 C 文件只移除 `linux/page_size_compat.h`，将唯一的 `__PAGE_ALIGN` 改为 `PAGE_ALIGN`。该头的动态页仿真依赖 X86_64，LKL 的两种页大小均使用各自的实际 PAGE_SHIFT。生成文件预期 SHA-256 为 `9ced943b04893f79a7f9a80aaeada68e02971a180d4d1447176b482d8e130892`；原始 C 文件摘要为 `63bd518d3e739ae6a190b4fc8ebf9359c8745d5e579c6cdf562a8c8776f56089`。

[lkl_ashmem_patch.py](../../../../scripts/lkl_ashmem_patch.py)由 NCE 的共同 apply 入口调用，复制到 `drivers/android/`。Kconfig 保留原 ASHMEM bool、SHMEM 依赖和帮助正文，Makefile 直接选择固定 C 对象。native 与 translation 构建均启用并读回 SHMEM/ASHMEM；复制输入、适配脚本、生成文件、配置、实际对象、`.cmd` 命令与同命令导出的依赖进入清单。复用翻译源码前检查已保存的生成文件摘要。

独立 16 KiB BPF JIT 内核已编译，CONFIG_SHMEM/ASHMEM/LKL_BPF_JIT 均为 y，724 项输入进入清单。原 LKL 输入与旧正式内核的七项保护文件前后保持，结果见[内核报告](../../../../../../../sdk-plain-validation-v1/ashmem-production-kernel-v1/result.json)。新 core 的全部入口与回调重新生成，CLI/GCD 各实际退出 0，196 项来源保持；结果见[core summary](../../../../work/native-linux-nce/ashmem-production-core16k-v1/summary.json)。这些结果覆盖本机核心行为；ashmem 独立共享与引用测试、原 init 和新 iOS 编译继续验收。

原 SDK 的两个节点使用不同标签。`/dev/ashmem` 为 `u:object_r:ashmem_device:s0`，boot_id 后缀节点为 `u:object_r:ashmem_libcutils_device:s0`；两者权限均为 `0666 root root`。原 ueventd 在 early-init 启动，`sys.use_memfd=false` 在 post-fs-data 设置；实际节点与原 Android 域下的访问结果继续由新原 init 检查。

原 init 副本只增加有界 ashmem 采集。实际 guest 的设备 openat、ashmem ioctl 和 11304 字节共享 mmap 保留 PID/TID、全部参数、原调用结果和共享内存名称，关联由原序列中的 PID/FD 与名称确认。采集代码不维护 FD 状态，dup、继承和引用释放由独立 fixture 验证。宿主在 launch 前持有初始 PID namespace 的 proc 目录 FD，随后通过相对路径 `<pid>/root/dev/ashmem*` 打开两个节点，以 AT_EMPTY_PATH statx 和 fgetxattr 读取设备属性及 SELinux 标签，每个临时 FD 关闭一次；guest 回收后关闭 proc FD。记录保留 boot_id、设备号、权限及实际返回值，原 Android 的设备、属性和策略保持。六入口、两次完整 boot 条件与同盘恢复保持原验收。

独立 ashmem guest、三种 variant 和 CLI/GCD 宿主已编译并完成实际地址、逐 C 参数和依赖读回。六项实际行为与回收均成功，结果见[汇总](../../../../../../../sdk-plain-validation-v1/ashmem-fixture-v1/hosts-v1/runs-summary.json)与[独立结束审阅](../../../../../../../sdk-plain-validation-v1/ashmem-fixture-review-v1/end-review.json)。两个 normal 的宿主创建、结束与 join 均为 28/28/28，content-failure 为 27/27/27，child-failure 为 28/28/28；故意失败后的 fixture FD/VMA 数均为 0。共享映射、fork/dup、保护限制与两种关闭顺序检查成功，最终内核页释放和内存压力下的 purge 尚未验证。

新原 init 的采集解析器以 23 项合成检查覆盖失败返回、缺失记录、PID/FD 关联、节点字段及 256 条采集上限。初份准备的 AST 预览缺少实际 probe 使用的 `source` 绑定，发生 NameError，原[失败报告](../../../../../../../sdk-plain-validation-v1/ashmem-original-init-v1/prepare-01/report.json)与生成物保留，未开始本机构建或 guest。独立 v2 只补充 `source=CORE/summary.json` 的预览绑定，C 采集头保持，新的[准备报告](../../../../../../../sdk-plain-validation-v1/ashmem-original-init-v2/prepare-01/report.json)通过，1347 项输入与版本前后保持；新的[边界报告](../../../../../../../sdk-plain-validation-v1/ashmem-original-init-v2/validation-v1/report.json)继续成功。

v2 实际完成 CLI/GCD lifecycle 和 CLI original 三个入口，宿主均退出 0；CLI original 的原日志校验失败，后续三个入口未执行，[运行报告](../../../../../../../sdk-plain-validation-v1/ashmem-original-init-v2/run/report.json)保留 failed。PID 479 的 system_server 在同一 FD 75 上完成后缀节点 openat、`ApplicationSharedMemory` SET_NAME、11304 字节 SET_SIZE 和可写 MAP_SHARED mmap，返回地址为 280152866816。21 条 ashmem 调用全部保存，LogAudit 实际检查 complete，原停止与回收检查成功；[独立三入口审阅](../../../../../../../sdk-plain-validation-v1/ashmem-original-init-runtime-review-v2/report.json)核对了 1356 项来源、原始日志、实际编译输入和这些结果。Java 采集范围内没有 AndroidRuntime/SystemServer/System 的 ERROR 消息，完整系统启动仍未确认。

原校验器把新增 ashmem 调用的 `result` 字段选入终末结果，随后在 `probe_stock_init.py` 的终末集合检查返回 false。仅在内存中过滤 21 条新增采集行，保留其余原日志和原参数后，同一校验器返回 true；原文件和失败报告保持。独立 v3 已将新增字段改为 `syscall_result`，同步新解析器与边界检查，并用冻结的原始 CLI 日志检查完整原校验器，覆盖这次已复现的冲突。

三次宿主读取 `/proc/sys/kernel/random/boot_id` 均返回 `-2`，设备 statx 与 getxattr 未执行；原 guest observer 已实际读到本次 boot_id。后续调查确认旧 proc 挂载被 detach，v3 改为持有启动前的目录 FD。两个 original 继续要求三次节点采样、11304 字节创建与映射及原完整 boot 条件；v2 同盘 lifecycle 恢复、generation、SQLite 和只读文件系统检查的结果见下文，原盘与失败保持。

[完整原校验器回归](../../../../../../../sdk-plain-validation-v1/ashmem-field-regression-v1/report.json)与[独立读回](../../../../../../../sdk-plain-validation-v1/ashmem-field-regression-review-v1/review.json)成功：冻结 CLI 日志共 55333 条 JSON，仅 21 条新增调用在内存中改为 `syscall_result`；原调用值、全部参数和其他行保持。全部改名后的原 validate 返回 true，保留一条原字段或破坏原 waited 条件仍返回 false；原三入口及完整 boot 的失败保持。

[宿主 proc 调查](../../../../../../../sdk-plain-validation-v1/ashmem-node-host-proc-review-v1/report.json)确认，原 init 的 MS_MOVE、chroot 与旧 rootfs 清理删除了宿主旧根目录的 `/proc`，该目录的挂载随后 detach。启动前打开的 proc 目录 FD 可以继续引用初始 PID namespace 的挂载；guest 首次 exec 的 CLOEXEC 关闭其继承副本，宿主副本保留。相对 root magiclink 继续检查 PTRACE_MODE_READ_FSCREDS 和原 LSM，实际权限由 v3 检查。新的[来源记录](../../../../../../../sdk-plain-validation-v1/ashmem-original-init-v3/ashmem-composition.json)、29 项[采集边界](../../../../../../../sdk-plain-validation-v1/ashmem-original-init-v3/validation-v1/report.json)及[准备报告](../../../../../../../sdk-plain-validation-v1/ashmem-original-init-v3/prepare-01/report.json)通过，1354 项输入、原函数及 14+2 入口机器码保持；实际六入口、节点访问及新 iOS 采集对象继续验收。

v2 的同盘 CLI/GCD lifecycle 恢复实际退出均为 0，原 validator、停止、进程组、代次连续性与 metadata 原 rc 属性成功。data 为 52→53→54，metadata 为 43→44→45，两盘超级块 clean。新增 data 只读 e2fsck 返回 4，三类 quota ID 0 各相差 131072 bytes 和一个 inode，[恢复报告](../../../../../../../sdk-plain-validation-v1/ashmem-original-init-v2-recovery/v2/result.json)保留 failed。[只读补检](../../../../../../../sdk-plain-validation-v1/ashmem-original-init-v2-recovery/readonly-v1/result.json)确认，同一固定输入原盘已包含相同差值；metadata e2fsck 返回 0，SQLite generation 54、文件头与 quick_check 成功，来源及盘字节保持。[quota 调查](../../../../../../../sdk-plain-validation-v1/ashmem-data-quota-audit-v1/report.json)确认，盘内三类 ID 0 记录均比 e2fsck 重算多 131072 bytes 和一个 inode，差额与 orphan inode 12 的占用相同。e2fsprogs 1.47.4 重算会排除该 inode，盘内最初计入的步骤尚未确认；原盘与候选盘保持。

新正式 iOS 运行库已从本轮 core 交叉编译，983 项构建输入前后保持，结果见[运行库报告](../../../../../../../sdk-plain-validation-v1/ashmem-production-runtime-ios-v1/report.json)。[独立运行库审阅](../../../../../../../sdk-plain-validation-v1/ashmem-production-runtime-ios-review-v1/review-v3.json)通过，24 个 host/Direct 对象和 72 个 Unicorn 归档成员逐项相等，2045 项审阅输入保持；前两份失败审阅保留。原 init v2 的六个 iOS 对象也已编译成功，1347 项准备输入和 345 项实际编译输入保持，结果见[六对象报告](../../../../../../../sdk-plain-validation-v1/ashmem-original-init-ios16k-v1/plan-v2/result.json)与[独立读回](../../../../../../../sdk-plain-validation-v1/ashmem-original-init-ios-review-v2/report.json)。v3 的六对象编译和读回已通过，1354 项准备输入、369 项编译输入和 417 项读回输入保持；[摘要](../../../../../../../sdk-plain-validation-v1/ashmem-original-init-ios16k-v3/summary.json)保存实际 argv、依赖和首次 baseline 导入失败，实际对象为 arm64 iOS、最低版本 26.0、SDK 27.1。这些结果覆盖 arm64 iOS 编译；App 链接、iOS guest、设备 JIT 授权与持续性能继续待验证。旧原 init v1 已按精确清单完成阶段清理，报告、原盘、数据库导出和失败保持，清单为主工作区的 `work/cleanup-2026-11-14/system-dmaheap-original-init-v1.json`，复现需要重新生成原中间产物。

[正式变更复杂度审阅](../../../../../../../sdk-plain-validation-v1/ashmem-production-complexity-review-v1/review.json)通过，helper、来源和生成摘要、配置读回、实际对象及编译依赖均有使用方；724 项内核输入和 607 项实际 ashmem 编译依赖一致。builder 的 25 项 BPF 来源同时在生成文件列表中出现，集合去重后的路径与摘要相同；本轮保留该已冻结来源，按当前构建继续验证。

### v3 六入口与后续采集

v3 的 [prepare 独立审阅](../../../../../../../sdk-plain-validation-v1/ashmem-original-init-review-v3/prepare-review.json)通过，实际[运行报告](../../../../../../../sdk-plain-validation-v1/ashmem-original-init-v3/run/report.json)保留 failed。lifecycle、original、recovery 的 CLI/GCD 六入口按原顺序执行，每个宿主退出 0、原 validator 返回 true、停止后进程组清空；两个 original 各观察 180 秒。first-stage、同盘 data/metadata 持久化、metadata 原 rc 属性和 Keystore SQLite 文件头及 quick_check 成功，冻结来源与版本保持。两盘最后超级块 clean，完整系统观察器仍缺少完整结果，`full_boot_validation=failed`。

结束审阅发现，原 core、prepare 与内外两份运行报告记录了九个 Direct 预存对象的链接路径，运行前后摘要没有覆盖这些对象。当前对象、core 到实际链接的路径和最终 CLI/GCD 构建摘要可读回；该范围不能补录运行前对象内容。下一轮 prepare 将这九个实际对象加入冻结输入，并在实际链接后逐项读回，原报告保持。

两个 original 各保存 21 条 ashmem 调用且未达到 256 条上限。CLI 的 system_server PID 493、FD 79 和 GCD 的 PID 489、FD 75 均完成后缀节点 openat、`ApplicationSharedMemory` SET_NAME、11304 字节 SET_SIZE 和可写 MAP_SHARED mmap；返回地址分别为 430644723712 和 289094270976。LogAudit 在两个 original 均为 complete。启动前 proc FD 的文件系统与 CLOEXEC 检查、三次 boot_id 读取和 guest 回收后的单次 close 均成功；每入口六次 `<ancestor>/root/dev/ashmem*` open 均为 `-13/EACCES`，statx 与 fgetxattr 未执行，节点合同保留 incomplete。

后续独立候选将设备读取放在原 PID 1 的当前 namespace 中，沿用已验证的 seccomp 模式查询与完整临时 `pt_regs`，只在模式 0 执行有界诊断调用，随后恢复原帧。固定策略允许 init 对两个标签执行 getattr，普通 `ashmem_device` 的设备 open 仍受限制。两节点均采用 O_PATH、AT_EMPTY_PATH statx、本进程 `/proc/self/fd/<fd>` 路径 getxattr 和单次 close，属性与标签引用同一 inode。O_PATH 使用 VFS 的 empty_fops，元数据 FD 支持 statx；fgetxattr 对 O_PATH 返回 EBADF，标签按上述路径读取。采样继续保留 boot_id、普通与后缀节点、原返回值、设备号、权限、标签和三次采样；每次临时 mmap、FD、单次关闭与 munmap 的结果分别记录。原 Android 属性、rc、SELinux 策略、SystemServer 的调用及完整 boot 条件保持。现有 `proc_pid_get_link` 将权限失败返回 EACCES，宿主 magiclink 的具体拒绝条件仍待确认。

SystemServer 的原 ERROR 范围没有显示共享内存创建后的阶段。下一轮保留原标签和共享 128 条预算，增加 `SystemServer` INFO、少量 `SystemServerTiming` DEBUG 阶段名及 VERBOSE 完成消息，以及 `SystemServiceManager` 的 `Starting phase 100` INFO。[固定 SDK 进度调查](../../../../../../../sdk-plain-validation-v1/ashmem-system-server-progress-audit-v1/report.json)核对了阶段顺序、13 个精确开始消息和四类完成文本，实际进度由下一轮日志确认。system_server 后续服务、完整启动、桌面、APK 与持续性能继续逐项验收。

### 路径类型修正、暂停恢复与 iOS 最终链接

v4 在 CLI 构建的 Direct 对象摘要调用处抛出 `AttributeError: 'str' object has no attribute 'open'`，guest 尚未运行。独立 v5 将两处回调参数转换为 `Path`，保留原失败；路径类型回归、准备与实际 CLI/GCD 编译成功，九个 Direct 对象的准备与链接前后摘要一致。用户暂停 goal 后，CLI original 被 SIGTERM 中断，实际退出 `-15`，进程组清空；两项 lifecycle 成功，后续三个入口未执行。原外层报告仍为 `running`，[独立中断审阅](../../../../../../../sdk-plain-validation-v1/ashmem-original-init-v5-interruption-review/report.json)记录真实终止状态，原报告保持。

中断前两次 PID1 节点采样的 O_PATH、statx、getxattr 与 close 均成功；两节点设备号为 `10:126`、权限为 `0666 root root`，普通和 boot_id 后缀节点分别使用 `u:object_r:ashmem_device:s0` 和 `u:object_r:ashmem_libcutils_device:s0`。临时映射释放与完整寄存器帧恢复成功。Zygote 最后记录 `Preloading classes...`，尚未出现 SystemServer 阶段或 `ApplicationSharedMemory` 创建；完整 boot 继续失败。

恢复 goal 后使用同一 v5 CLI/GCD 程序与中断盘完成[独立恢复](../../../../../../../sdk-plain-validation-v1/ashmem-original-init-v5-recovery/result.json)。两入口实际退出 0、原 validator 成功、进程组清空，data generation 为 52→53→54、metadata 为 43→44→45；两盘 inode、UUID 与范围保持，最终 clean，metadata 原 rc 属性与 SQLite 文件头、generation 54 和 quick_check 成功。data 只读 e2fsck 返回 4，三类 quota ID 0 各差 131072 bytes 和一个 inode；metadata 返回 0，原报告保留 failed。该 quota 差额的固定输入调查见上文。独立 v6 将保持原行为与六入口条件，在本阶段提交后重新准备和运行。

[iOS 最终链接](../../../../../../../sdk-plain-validation-v1/ashmem-ios-final-link-v1/report.json)的普通运行库及原 init CLI/GCD 三份诊断程序均成功，实际为 arm64 Mach-O EXECUTE、iOS 26.0 / SDK 27.1。链接使用真实 `ios/Bridge.c`、JIT 支持、正式运行库与原 init 六对象，并引用主工作区已核对来源的 iOS gfxstream/ANGLE 库；执行入口、JIT 与图形符号及实际库导入均已核对，输入前后保持。该检查覆盖最终执行入口链接；App/UI、运行资源、签名、设备授权和实际执行继续待验证，完整桌面打包条件保持。

本阶段提交固定驱动与构建接入，主题继续 proposed。独立 helper 的实际使用方为共同 NCE apply 与 native/translation 构建入口，负责固定源码适配和编译来源，删除后需要在两个入口重复实现。原 ioctl、共享映射与保护契约继续保留；16 KiB 局部结果和本次设备采样分别记录，native/SMP、4 KiB、压力 purge、最终物理页释放及完整系统继续验收。

### v6 六入口与系统阶段

独立 [v6 原六入口](../../../../../../../sdk-plain-validation-v1/ashmem-original-init-v6/run/report.json)已完成，候选 `a434013` 与准备输入前后保持。每入口实际宿主退出 0、原 validator 成功、进程组清空，停止与资源回收、同盘 data/metadata、metadata 原 rc 属性与 SQLite 检查成功，两盘最终 clean。CLI/GCD 均成功创建并映射 `ApplicationSharedMemory`，每入口三次 PID1 设备属性、标签和临时资源回收检查均成功，`ashmem_original_passed=true`。完整 boot 仍为 failed。

两个 system_server 均记录 `StartDisplayManager` 完成，随后到 `WaitForDisplay` 与 `Starting phase 100`；当前阶段过滤没有包含逐服务的 `OnBootPhase_100_` 文本，具体等待服务尚未确认。原失败、日志与阶段消息保留，下一步按[默认显示等待](../bug-fix/2026-11-14-原SDK默认显示等待.md)补采服务回调与线程状态。固定驱动与原设备调用已通过本轮本机组合，完整系统、桌面、APK 与持续性能继续验收。

## 曾考虑的替代方案

- 将 `sys.use_memfd` 设为 `true`：现有 Framework 测试已经采用该配置，原 SDK 和部分原始 JNI 调用仍可能使用 ashmem ioctl；本轮先实现原设备接口。
- 为打开路径建立普通文件或只修改节点名称：原 libcutils/JNI 还需要 ashmem ioctl、保护范围和共享映射，路径本身无法提供这些行为。
- 为最新 Rust 驱动增加 LKL Rust 构建支持：这项选择需要扩大工具链与架构适配范围，先检查可用的固定 C 驱动。

## 验收标准

1. 固定源码、许可证、适配差异、内核配置和实际对象的编译依赖可核对；历史 core、原盘及失败报告保持。
2. 独立 CLI/GCD 检查 ashmem 的大小、名称、共享与引用生命周期、保护限制和必要失败场景；重新导出的全部内核地址与本轮 ELF/bin 对应。
3. 原 init 实际创建设备的普通及 boot_id 后缀节点，权限与 SELinux 标签保持原契约；原 `ApplicationSharedMemory.nativeCreate` 的结果、Zygote/system_server 和后续服务由实际日志核对。
4. 两个 original 继续要求 `sys.boot_completed=1`、真实进程、Binder 服务和 `cmd` 查询；退出、同盘恢复、SQLite 与 iOS 构建继续检查。缺少任一完整启动条件保留 failed。

## 风险

固定历史驱动对 Linux 6.12 的 shrinker、VMA 和 shmem API 依赖需要逐项核对。当前失败日志的 openat 记录缺少 PID/TID，源码控制流和相邻 Java 消息只能支持其已记录的范围。实际节点、内核回收、Android 域和 iOS 运行资源继续由后续结果确认；本阶段不安排手机验证。
