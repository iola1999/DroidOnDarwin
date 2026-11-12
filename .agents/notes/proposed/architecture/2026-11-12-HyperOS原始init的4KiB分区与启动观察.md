# 决策记录: HyperOS 原始 init 的 4 KiB 分区与启动观察

状态: proposed

## 问题

主会话在收到[原始输入准备](../../implemented/architecture/2026-11-12-HyperOS原始init输入与元数据准备.md)后，授权从固定 `d17162f4a68ad4e6e6f7e21c71a38b3cc0702765` 在新独立树 `work/continue-hyperos-init` 继续原 HyperOS init。准备提交 `9c4b3942e678c355cfe084f12a20a798a64d3e8c` 已移入此树为 `aafac70`。原发布来源、438 个分区文件、86 个 APEX 样本、40 个容器和全部 179 条服务声明保持固定，原库、rc、policy、签名与 enforcing 保留。本轮只做 Mac 验证及 iOS 编译。

前提检查发现三个具体差异。`d17162f` 的正式 builder 已启用块设备、loop、device-mapper、verity 和 `BLK_DEV_WRITE_MOUNTED`，尚未启用 EROFS。原 vendor EROFS 使用 LZ4、big_pcluster 和安全标签；runtime APEX payload 也为 EROFS。现有宿主及内核原盘接口固定为 data/system 两槽，HyperOS vendor 和 system_dlkm 需要独立只读原盘。原 raw system.ext4 根目录还有 product、system_ext、init.environ.rc 与七个根链接，现有 SDK runner 只 bind `/system-image/system`。

`probe_stock_init.py` 的输入与 core 校验固定为 SDK36、16 KiB；其 PID 1 生命周期、祖先命名空间停止、wait4、原 init `selinux_setup`、guest write/security/block 观察可以继续使用。固定 `d17162f` 的 sparse 上限为 64 GiB，真实 ASLR 正由 stock_runtime 在另一树验证，生产提交尚未形成。本树先准备挂载和输入扩展，ASLR 实现以该会话后续固定提交为准。

## 提案

扩展现有原 init 入口，接收最终固定 HyperOS manifest；核对原身份、清单摘要、文件及原盘摘要后使用 4 KiB sparse/PTE、单 CPU、Direct flat、dual、TBI core。SDK36 保留 16 KiB 默认及固定来源校验。复用现有编译、trace 和回收流程，原 init 仍从 `selinux_setup` 执行。源码副本、可写缓存和新报告只放此树 `work/`。

原盘固定为 data、system、vendor、system_dlkm 四槽。新增共享 `NCE_DISK_COUNT` 供宿主、内核和边界测试使用，保持槽 0 的持久数据语义及其余槽的 O_RDONLY、共享锁、写入拒绝、内核只读标记。运行器在同一原 ext4 根内 bind system、product、system_ext，并以原 EROFS 挂载 vendor、system_dlkm。根 init.environ.rc 从原 ext4 bind，根链接保留实际路径、目标和所有者，原 inode mode/xattr 与 Linux 创建状态分别记录。APEX 继续由原 apexd 激活，使用原 runtime 内的 linkerconfig 路径。

EROFS 所需 `EROFS_FS`、`EROFS_FS_XATTR`、`EROFS_FS_SECURITY`、`EROFS_FS_ZIP` 加入现有 Android 内核配置检查。Linux Kconfig 的 ZIP 默认选择 LZ4 解压器，按实际原镜像压缩算法验证最终生成配置。先记录 SDK 与 HyperOS 原挂载路径的差异，不增加镜像 provider 或分区注册框架。

首次运行前核对 `NCE_EXEC` 的既有 ELF/解释器登记、动态代码窗口、信号特殊映射和新 ASLR 地址边界。ASLR 源码由负责会话提供固定提交；本树验证 EROFS/原盘改动之后的 core，随后执行 CLI/GCD 的 lifecycle 与 original 两种观察。保留原启动失败、重启请求、期限停止和资源释放结果。原输入缺失的实际路径停止并记录；六条静态缺失服务声明继续保留。

### 当前准备范围

固定来源还有原 userdata.img、encryptionkey.img、VerifiedBootParams.textproto 和 kernel_cmdline.txt。最终清单已保存其发布身份，userdata/encryptionkey 尚未提取进 init 运行输入。`selinux_setup` 观察的 mounted-partition 前提与完整 first-stage 分开记录。后续进入原 data/fstab 或启动属性路径前，核对这些实际输入及依赖；原 ramdisk 中 `/init`、`/init.real`、KernelSU 和模块也保持发布原字节，尚不声明已适配 LKL。

## 曾考虑的替代方案

将 vendor 文件复制到 tmpfs：会丢失原文件系统提供的所有者、标签和只读属性，采用原 EROFS。把 vendor EROFS 复制到 guest RAM 再配 loop：可复用两槽，但额外复制整盘并引入另一套 image/loop 准备，采用固定只读槽。重复编写 HyperOS probe 的编译与回收流程：现有原 init 流程已有真实 SDK 使用方，保留共同入口。直接执行 second_stage：会跳过原策略加载与域转换，首轮仍使用原 selinux_setup。新增通用镜像框架：固定来源的差异可以由明确原输入和挂载参数表达。重复实现 ASLR：已有侧会话负责相同接口，等待固定提交后复用。

## 验收标准

- 固定 HyperOS manifest、原盘、原文件和 metadata 校验成功；错误身份或内容、缺失必要输入被拒绝，失败目录保留。
- 只读槽读取成功，写入与越界被拒绝；SDK36 两槽行为保持。4 KiB EROFS 实际挂载、原文件内容与标签由 guest Linux 读取验证，相关代码通过 iOS 编译。
- 变更后的 4 KiB core CLI/GCD 成功，信号特殊映射、fork/exec、页表计数和上下文回收检查成功。实际 ASLR 参数与 mmap/stack 行为采用已提交实现。
- 原 init 的四种入口保留原 policy、enforcing、rc 与 APEX 容器；分别报告实际到达阶段、首个失败、exec、重启、wait4 和资源释放，原库及策略运行前后摘要一致。
- 完整 first-stage、AArch32 执行、完整 HyperOS 桌面、通用 APK、整机性能和 iPhone 执行继续独立验收。

### 挂载与输入阶段的本机结果

已实现四槽原盘、EROFS 配置、固定 HyperOS manifest 校验、原分区挂载路径和 13 项 guest 原 metadata 检查。原 init 观察入口接收 `--hyperos-manifest`，SDK36 保留现有参数与 16 KiB 默认。地址检查允许既有 64 GiB 和后续 512 GiB sparse；HyperOS 要求 512 GiB 及 core 的 ASLR bounds/minimum/maximum 成功记录。当前 ASLR 实现由 stock_runtime 负责，本树尚未移入该提交。

Mac 的四槽读取、只读、越界、锁与释放检查成功，宿主线程信号 mask 检查成功。格式化后的 stock-init guest 按 AArch64 musl/4 KiB 编译成功，lkl-host 按 iPhoneOS27.1 SDK、`arm64-apple-ios26.0` 编译成功。SDK36 已有原 init 四入口报告在当前 parser 下重新检查成功。固定 HyperOS 输入成功准备 438 个原文件、七个根链接和 13 项 metadata；清单改动、原 vendor 不可用、原文件摘要改变、挂载记录缺失、文件系统不同、可写状态、链接目标不同、原标签/所有者/mode/type 不同、数值替代布尔值、64 GiB 输入及地址越界均被拒绝。23 个 contract 检查使用实际 SDK lifecycle 报告和固定 HyperOS 输入构造检查数据；其中 HyperOS 数据只验证 parser 条件。

检查命令为 `python3 scripts/test_stock_init_hyperos.py --manifest /Users/fan/project/github.com/iola1999/DroidOnDarwin/work/continue-hyperos-jit/work/native-linux-nce/hyperos-init-input-v6/manifest.json --sdk-init-report /Users/fan/project/github.com/iola1999/DroidOnDarwin/work/native-linux-nce/stock-init-apex-main-original-v2/summary.json`，输出保存在本树 `work/native-linux-nce/hyperos-init-build-checks-v2/`。另执行 `python3 -m unittest discover -s scripts -p test_hyperos_init.py`、Python 编译检查、改动 C 文件的格式检查和 `git diff --check`，均成功。结构化阶段结果见 [results-hyperos-init-mount-input-20261112.json](../../../../experiments/native-linux/results-hyperos-init-mount-input-20261112.json)。结果保存实际命令、源码及对象摘要。

此阶段尚未在 guest Linux 挂载 EROFS，也尚未执行 13 项 metadata 检查或 HyperOS init。变更后的 4 KiB core、共享 16 KiB core/init、生成的 init runner/execution 与完整 iOS runtime 编译继续作为剩余验收。ASLR 固定后在此树独立构建，先执行 lifecycle CLI/GCD，再运行 original CLI/GCD，首次失败保留。

### 复杂度审查

真实使用方为已有原 init probe 和正式 translation builder。宿主与内核共享四槽计数，空槽保持既有未登记状态，固定可写槽继续承担 data；增加的 vendor/system_dlkm 不引入分区注册或镜像选择接口。原 metadata 检查输入从固定 manifest 产生，checker 从 guest Linux 独立读取 inode 状态，两份表示用于检查挂载结果。移除检查后，挂载成功但标签、所有者或 bind 路径不同的结果会被接受。

`stock_init_raw_path` 仅在同一个 setup 中为目录和文件共同判断原分区路径；移除该判断会在原只读分区写入 staged 文件。HyperOS 的固定摘要校验保留外部输入边界，替代路径仍是明确的三个原盘。拆出独立 HyperOS probe 会重复编译、trace、PID 1 停止与回收流程，现有共同入口继续持有这些职责。此次没有扩展 AArch32 执行或配置框架。

### 原盘独立验收的入口

主会话要求在 ASLR 固定前继续可独立完成的 guest EROFS/metadata 检查。阶段 `bd43b83` 的 HyperOS CLI 前提同时限制 lifecycle 和 original，原盘检查被 ASLR 前提阻止。lifecycle 只启动检查 guest、验证原 metadata/信号、fork 子进程并停止回收，不执行原 init。

本次增加 `--lifecycle-only`，仅运行现有 lifecycle CLI/GCD，允许已成功的 4 KiB/64 GiB 或 512 GiB sparse core。完整 original 观察继续要求 512 GiB 及 ASLR bounds/minimum/maximum 成功记录。该参数的结果 scope 与运行 mode 明确记录原 init 尚未执行。替代的独立 mount probe 会重复运行器生成、停止和回收；复用现有 lifecycle 保持相同 guest 检查及失败输出。验收包括两入口的实际 EROFS 挂载、13 项 metadata、信号特殊映射、2/2 上下文回收、原盘摘要前后一致及原 init exec 记录缺失。

四槽只读生命周期审阅确认盘在 Linux 初始化前打开，完成 Linux 停止后统一关闭；只读槽使用 `LOCK_SH`，槽 0 使用 `LOCK_EX` 并在关闭前 fsync。内核忽略未打开的空槽，登记失败逐项删除已登记的 gendisk，清空指针并注销 major。现有 `watch_disk_io` 对所有槽读取到客体 RAM 的写入执行代码缓存失效检查。此次追加只读共享锁可共存、阻止排他锁及关闭后锁释放的宿主检查。

### 首次 4 KiB 构建失败

使用 `--guest-page-bytes 4096 --guest-layout sparse --mapping pte --execution direct --direct-memory flat --code-memory dual --data-tbi 1 --reuse-engine-build` 构建本树 core。正式 builder 在 `olddefconfig` 后因四项 EROFS 配置缺失停止，未生成新内核，也未执行 Linux。原 console、kernel build log 与配置摘要保存在 `work/native-linux-nce/hyperos-mount-core-4k-v1/` 的 `failure.json` 与 `kernel-build.log`，console 为同目录名前缀的 `.console.log`。

基础配置的 `MISC_FILESYSTEMS=n` 关闭了 `fs/Kconfig` 中包含 `fs/erofs/Kconfig` 的菜单；四项请求配置被 Kconfig 删除。最小修复是在现有 `BLOCK_OPTIONS` 同时启用并检查 `MISC_FILESYSTEMS`，保留 EROFS 原配置和正式 builder 的失败检查。后续使用独立 v2 输出复验实际生成配置、LZ4 解压器与 EROFS 挂载。

### 首次原盘挂载失败

修复配置后，`hyperos-mount-core-4k-v2/summary.json` 的 CLI/GCD 均成功，各 70/70 上下文释放，实际 sparse 上限为 64 GiB，ASLR 实现尚未组合。生成配置包含全部 EROFS 请求项及 `LZ4_DECOMPRESS=y`，编译日志包含 EROFS 读盘、xattr 与解压源码。该 core 的 iOS 运行库编译成功，记录为 `hyperos-mount-runtime-ios-v1/build.json`。

`--lifecycle-only` 的首轮 CLI 在 vendor EROFS 挂载断言退出，returncode 为 -6；system、product、system_ext 已挂载，尚未完成 vendor 挂载、13 项 guest metadata 或 PID 1 生命周期，也未执行原 init。失败目录为 `work/native-linux-nce/hyperos-original-mount-4k-v1/`。既有 setup 的断言直接结束宿主进程，此次未运行计划中的 Linux 停止及上下文释放检查。首轮 stderr 未保存 mount 的实际返回码，失败原因尚未确定。

后续先运行同一生成二进制的 GCD lifecycle，保留失败；再为挂载失败输出返回码和 Linux syslog。syslog 读取与正常结束后的既有 kernel log 读取共用一个函数，保留缓冲区生命周期及原 JSON 记录。该诊断调整只定位当前原盘失败，不跳过挂载或策略。

GCD 的同一原盘入口也在相同断言结束，returncode 为 -6，独立失败结果为 `hyperos-original-mount-4k-v1-gcd-diagnostic/summary.json`；三项原盘 SHA-256 重新读取后与固定输入一致。诊断 v2 的 CLI 返回 vendor EROFS mount `-12`（ENOMEM），Linux syslog 未记录 EROFS 的具体失败位置。源码中的 LZ4 global buffer 使用普通 4 KiB 页数组和 `vmap`；现有 4 KiB VMALLOC 要求完整 16 KiB 宿主组及组内物理连续，需继续核对这条实际分配路径。当前尚未确认 ENOMEM 的具体返回位置。

此阶段固定结果见 [results-hyperos-init-erofs-lifecycle-20261112.json](../../../../experiments/native-linux/results-hyperos-init-erofs-lifecycle-20261112.json)。MISC 配置修复、lifecycle-only、共享锁和失败诊断已完成；24 项 contract 检查及宿主共享锁/关闭后释放检查成功，格式与 Python 编译检查成功。原盘 lifecycle 仍失败，13 项 guest metadata、最终 ASLR 组合与原 HyperOS init 留在当前提案的剩余验收中。

### EROFS 全局缓冲区的 4 KiB 分配前提

`z_erofs_load_lz4_config` 在读 superblock 时调用 `z_erofs_gbuf_growsize`。后者使用 `alloc_pages_bulk_array` 分配普通页并 `vmap`。现有 4 KiB LKL 会拒绝不足完整 16 KiB 宿主页组的长度，以及组内物理地址不连续的页数组。源码中的普通页分配无法保证这些条件。该调用失败会返回 ENOMEM，与实际 mount 返回一致；当前未直接记录到此函数的返回，具体原因仍待同输入复验确认。

最小实现放在现有 4 KiB VMALLOC 生成补丁：全局缓冲区的容量对齐到四个 4 KiB 页，新增 backing 按 order 2 分配，split 后仍保存原页数组、引用计数和释放方式。已有缓冲区增长、mutex/spinlock 与数据读写顺序保留。buffer 的容量允许超过请求值，`z_erofs_get_gbuf` 仍检查真实请求页数；镜像的 pcluster 参数和数据保持原值。生成的 `fs/erofs/zutil.c` 纳入正式 builder 摘要。

放宽 VMALLOC 的宿主页组检查会允许不完整共享别名，未采用。把缓冲区改为 vmalloc 还需改变数组所有权、增长及退出释放，当前保留原表示。只把页数对齐仍无法保证组内物理连续；只按 order 2 分配仍需保证映射长度完整，两项同时作为这个实际使用方的前提。验收使用同一原盘的 CLI/GCD lifecycle、13 项 metadata 与回收，并复验新 4 KiB core；后续 EROFS 解压页数组的其他映射调用按实际失败继续核对。

适配后的 `hyperos-erofs-buffer-core-4k-v1/summary.json` 通过 CLI/GCD，各 70/70 上下文释放，页表告警为 0，4 KiB guest 与 16 KiB host 保持原值。报告 SHA-256 为 `94a21661e343a1095bdd85ce6040310f635242694e3a564040a8f46669595a85`，内核 image SHA-256 为 `c976ec172a9f0033df6c6d39819305a41953d191368b889b58ee48951b78913c`。该构建尚未组合 ASLR，实际 sparse 上限为 64 GiB。

同一固定 manifest 的 `hyperos-erofs-buffer-mount-4k-v1` 已成功挂载原 system/product/system_ext、vendor 与 system_dlkm；此前的 vendor ENOMEM 消失。CLI 和同一生成二进制的独立 GCD 检查均随后在原 `/bin` 根链接创建处断言退出，returncode 为 -6。三项原盘运行后摘要与清单一致；13 项 guest metadata、PID 1 停止回收和原 init 尚未执行。首个失败与此次两个失败目录均保留，阶段结果见 [results-hyperos-init-erofs-buffer-20261112.json](../../../../experiments/native-linux/results-hyperos-init-erofs-buffer-20261112.json)。这一结果确认全局缓冲区分配调整使原 EROFS mount 通过，尚未覆盖后续文件解压中的其他页数组映射。

### 原根链接与 core shell 的准备差异

通用 `linux-runner.c` 在原 init setup 前创建 `/bin` 并部署实验用 dash 到 `/bin/sh`，原 HyperOS 清单要求根 `/bin -> /system/bin`。本次原盘检查到达原链接创建后，`symlinkat` 因已存在目录失败。最小方案只调整 `probe_stock_init.py` 生成的原 init runner，在部署原根链接前省略两项 core shell 准备；原 HyperOS `/system/bin/sh` 继续从原只读分区使用，原 SDK36 init 输入采用其实际库与 bootstrap linker，通用 core 流程保持原有 shell 验收。

删除已创建的 `/bin/sh` 和目录会增加 setup 中间状态及删除操作，当前不采用。更改原根链接或替换原服务路径会改变原输入，当前保留原清单。验收先检查两种原 init 输入生成流程及两入口生命周期，再执行实际 13 项 metadata；最终 ASLR 组合后继续原 HyperOS init 与 SDK36 原 init 回归，失败目录保留。

### 固定 ASLR 的组合边界

stock_runtime 已提供固定 `964984508d27c2b120b7636df2d490e0243ab51e`，包含实际 4 KiB/16 KiB core、SDK 原 init 与 iOS 编译结果。本树从已提交的 EROFS 阶段组合该实现，当前共同冲突位于 `probe_stock_init.py` 的页大小、PC 与 HyperOS 原 metadata 校验。保留固定 ASLR 的源码、构建摘要和随机源检查，HyperOS 原 init 仍要求 4 KiB 与成功的 512 GiB ASLR core；SDK36 保留 16 KiB 默认，并采用该固定提交已实际验证的 4 KiB 入口。

现有 64 GiB core/init 报告仍由原盘准备和 contract 检查使用，布局读取器保留明确的 64 GiB/512 GiB 报告校验。新构建的默认 sparse 上限继续为 512 GiB，这项报告兼容不增加 runtime 地址配置。原 init validator 从单条实际布局记录读取地址范围，删除与该记录重复的 `address_limit` 参数；PC 同时检查 guest 下界与上界。修改 synthetic HyperOS contract 数据的实际页大小和布局字段，继续注明这些数据只检查 parser。组合源码重新构建并完成两入口 core、原盘 metadata/init 与最终 iOS 检查后，才记录本树实际运行结论。

合并后的固定 HyperOS contract 检查通过 27 项，包含原 SDK36 四入口报告复核、64/512 GiB PC 范围、guest 下界、缺失布局及 metadata/只读条件；输出为 `work/native-linux-nce/hyperos-aslr-merge-contract-v1.console.log`。布局读取器的有效配置与 203 项错误条件检查成功，Python 编译与差异检查成功。当前未使用 stock_runtime 的旧组合摘要作为本树 core 的实际运行结果。

### LKL bootstrap 文件与原 `/init` 链接

组合后的 `hyperos-erofs-aslr-core-4k-v1/summary.json` 已通过 CLI/GCD，各 116/116 上下文释放，九组 ASLR 检查成功，4 KiB 实际位数为 18..26，sparse 上限为 512 GiB，页表告警为 0。SHA-256 为 `952e5b6cbc165436c0ce81c797969ac5a4c64aff545a69e2c227902984b1f7ba`。省略 core shell 的新原盘检查成功创建 `/bin` 原链接，CLI 和独立 GCD 随后在原 `/init` 链接创建处退出，returncode 均为 -6。失败目录为 `hyperos-erofs-aslr-lifecycle-4k-v1` 及其 `-gcd-diagnostic`；三项原盘摘要保持一致。

当前固定 LKL 的 `arch/lkl/kernel/setup.c:fs_setup` 创建空的 0700 `/init`，`lkl_run_init` 用该路径执行 `begin_new_exec`、初始化 host syscall tasks，再通知启动完成。`syscalls_init`、后续 `new_host_task` 与退出回收通过 `host0`/task/TLS 状态运行，源码中没有再次打开该路径的操作。本树生成源码没有 `lkl_syscall_host_init_task` 符号；实际对应初始化入口为 `syscalls_init`。`begin_new_exec` 持有的 executable inode 引用按 Linux 文件生命周期释放，移除目录项不提前释放该引用。

最小处理位于现有 HyperOS setup：Linux 启动成功后、原根链接部署前，以 O_NOFOLLOW 打开 `/init`，通过 fd 的 statx 确认普通文件、mode=0700、uid/gid=0、一个硬链接及 size=0，再确认读取返回 EOF 并移除这个 LKL bootstrap 目录项。已有非空文件、目录、链接或不同元数据会使检查停止。statx 的固定 256 字节 ABI 前缀来自当前 Linux UAPI，保留完整输出缓冲区；没有向宿主编译额外加入 Linux 头文件目录。随后按固定 manifest 创建原 `/init -> /system/bin/init`，原盘字节和链接声明保持。更改 LKL 启动路径或 binfmt 的全局行为会影响其他 core 与 SDK 入口，当前保留这些行为。当前 original 通过 `/system/bin/init` 执行 `selinux_setup`；完整 first-stage 经 `/init` 执行及其 binfmt 行为继续单独验收。后续验收包括原 13 项 metadata、两入口信号/fork/停止回收及原 init 实际失败。

仅使用 EOF 检查的 `hyperos-erofs-aslr-lifecycle-4k-v2/summary.json` 已通过 CLI/GCD，两入口各完成 13 项实际 metadata 和 2/2 上下文释放，原 `/init` 的目标、uid/gid 与 SELinux xattr 符合固定输入。原 init 尚未执行。native_signal 的只读审阅确认启动后 pathname 不再用于宿主任务创建，并提出增加 mode/type 检查；按该建议扩展原 bootstrap 的 statx guard，再以独立 v3 输出复验。

加入 statx 检查后的 `hyperos-erofs-aslr-lifecycle-4k-v3/summary.json` 同样通过 CLI/GCD，各完成 13 项实际 metadata、信号特殊映射、fork、期限停止、wait4 和 2/2 上下文释放。报告 SHA-256 为 `5e4d984f2db205bc9a10a50d2cd5446130a8b089766e00a7d196fe2808f377de`。原 `/init` 链接目标为 `/system/bin/init`，uid=0、gid=2000、Linux 链接 mode=0777 与原 init_exec 标签符合固定输入；原 inode mode 的差异继续保存在清单内。

### 首次原 HyperOS init 与 CIL 读取失败

`hyperos-original-erofs-aslr-4k-v1/summary.json` 已完成默认四入口，报告 SHA-256 为 `6e553aa900075e836400ae4828886446afa9fb9b3b4307f411e0c734ebbe1d9a`。lifecycle CLI/GCD 各完成 13 项原 metadata 及 2/2 上下文释放；original CLI/GCD 实际加载原 `/system/bin/init`、bootstrap linker64 和 `/system/bin/secilc`，各完成 3/3 上下文释放。观察报告的 `observation_validated=true` 只覆盖执行观察和回收契约，系统启动实际失败。两份 original 报告的 wait status=1、deadline_stop=false，均保存原 reboot syscall。

两入口的原 init 都因 system/vendor precompiled 摘要不同进入原策略编译。原 secilc 日志为 `Failure reading file: /vendor/etc/selinux/plat_pub_versioned.cil`，退出状态为 255，原 init 随后记录 `Unable to open SELinux policy` 和 `InitFatalReboot: signal 6`。Linux 同时在原 vendor EROFS 的 nid 755063 记录 `z_erofs_read_folio: read error -12`；CLI 的 folio index 为 27，GCD 为 0。实际文件解压读取仍失败，原 policy 未加载，task label 仍为 `kernel`，second_stage 尚未到达。原盘与全部清单文件运行后摘要保持一致。

该次启动还保留 boot hardware/fstab 选择输入缺失和 `/linkerconfig/ld.config.txt` 警告。当前入口由 runner 先挂原盘，再明确执行 `/system/bin/init selinux_setup`。完整 first-stage、原 userdata/encryption 输入、启动属性、正式 APEX 激活及后续服务启动继续独立验收。

后续先定位 `-12` 的真实返回位置。LZ4 的输入和输出数组均可能调用 `erofs_vm_map_ram`，该接口不能保证完整宿主页组与组内物理连续；当前 read_folio 日志未区分这两项映射和普通页分配。最小诊断限于实际 LZ4 失败处，保存请求页数、字节数和调用位置。确认后在实际分配或解压使用方处理，保留 VMALLOC 的完整组检查、原镜像字节、CIL 与策略摘要。

### 本阶段共享与 iOS 检查

同一组合的 `hyperos-shared-sdk-core-16k-v1/summary.json` 通过 CLI/GCD，各 116/116 上下文释放、九组 ASLR 和页表检查。报告 SHA-256 为 `f898b0d4afbbb48dc085cf3ecfe3c2242a557857526979fae41a62ddda6bd8b2`，实际 sparse 上限为 512 GiB。固定 SDK36 原 init 输入按此前报告中的实际 source 路径复用。首个准备尝试从 runtime manifest 文件名推导了错误目录，在加载 `bin/init` 前停止；该 `hyperos-shared-sdk-original-16k-v1/failure.json` 保留。原始文件仍存在，后续 v2 使用原报告的 `files[/system/bin/init].source` 确定 runtime 目录。

共享 SDK36 的 `hyperos-shared-sdk-original-16k-v2/summary.json` 已完成四入口，报告 SHA-256 为 `3e4acd27084abdb486b1c3da88001cc29a16cb72c8a25de2a65ba0da8f4856c9`。lifecycle CLI/GCD 各完成 2/2 上下文释放；original CLI/GCD 都实际加载原策略、完成 init 域转换、进入 second_stage 和 `SetupMountNamespaces`，各完成 148/148 上下文释放。两入口随后因原 `keystore2` 四次 critical 退出请求 reboot，wait status=1、deadline_stop=false；完整 SDK36 启动仍失败。原 system 挂载只读，156 项原 ext4 metadata 在输入准备中重新读取。该固定输入尚未部署 vendor/product/system_ext 的服务二进制，后续原 vendor/KeyMint 完整输入由 stock_runtime 在独立树处理。

4 KiB/ASLR/EROFS 的完整 iOS runtime 编译成功，`hyperos-erofs-aslr-runtime-ios-v1/build.json` 的 SHA-256 为 `fa0a2ea36cb9d3e35e0b98b6f231eef6902d9ed9467115553a751c380f1990cc`，`libtranslation.a` 的 SHA-256 为 `cc42e7ece7c7f5acc56890e4194c618e096b3ab867de8fe6666cb74758ba229e`。实际 original 生成的 `linux-runner.c` 和 `linux-execution.c` 另以 iPhoneOS27.1 SDK、`arm64-apple-ios26.0`、`-Wall -Wextra -Werror` 编译成功，命令、源码和对象摘要保存在 `hyperos-original-runner-ios-v2/build.json`。首次编译调用将 `-iquote` 放在 xcrun 工具名之前，调用失败，v1 失败目录保留；v2 将该参数交给 clang。编译结果未覆盖 iPhone 执行。

共享 SDK36 实际生成的 runner/execution 也采用对应 16 KiB 内核和 core 头文件，以同一 iOS SDK 和目标编译成功，记录为 `hyperos-shared-sdk-runner-ios-v1/build.json`。这一检查覆盖共同的原 init 生成代码在两种页大小下的编译；完整 runtime 使用前述 4 KiB 组合结果。

本阶段的 27 项固定输入/parser contract 检查、三项清单工具检查、Python 编译、C 格式与 `git diff --check` 均成功。删除 core shell 准备仅作用于现有 original/lifecycle 生成流程，通用 core 的 shell 行为继续由两种页大小实际 core 检查。bootstrap 节点在 start_kernel 完成后只承担已结束的启动输入，现有 host0/task/TLS 生命周期与后续宿主任务创建保持原接口；新增 statx 检查仅约束即将移除的真实节点，没有增加启动状态或另一套任务入口。

本阶段依赖原输入准备 `aafac70`、四槽原盘 `bd43b83`、EROFS 配置与诊断 `b46b945`、全局缓冲区分配 `5391c16`，以及原 ASLR 固定 `9649845` 在此树的组合提交 `828d4eb9d2fc9efbb1a186c54f3a4d990ef96d57`。结构化结果见 [results-hyperos-init-root-bootstrap-20261112.json](../../../../experiments/native-linux/results-hyperos-init-root-bootstrap-20261112.json)，保存固定输入、首次失败、两种页大小 core、原 init 四入口、iOS 实际生成源码编译、报告摘要和验证边界。该交付只包含原 root/bootstrap 准备与执行观察；EROFS 解压读取、binfmt 重入、完整 first-stage、APEX 正式激活、AArch32 与完整 HyperOS 继续作为剩余项。

### bootstrap binfmt 的后续生命周期检查

native_signal 同时确认 LKL 的 bootstrap binfmt 持续注册，仅按 `bprm->filename == /init` 选择。有效 ELF 由已注册的 ELF handler 处理；无效 `/init` 返回 ENOEXEC 后可继续进入 bootstrap handler，重新初始化 host0/TLS 并使用已释放的 init_sem。固定 `96cfd67` 阶段尚未修改该 handler，继续验收明确执行 `/system/bin/init` 的原观察入口。后续生命周期修复见[LKL 启动格式处理器生命周期](../../implemented/bug-fix/2026-11-13-LKL启动格式处理器生命周期.md)。

### 主线组合与生命周期修复后的观察

2026-11-13 主线从 `225b41e` 接收 `bd43b83`、`b46b945`、`5391c16` 和 `96cfd67` 的原盘与 root/bootstrap 准备，使用已在主线验证的 `185893b` mmap ASLR，随后完成一次性 binfmt 注销。原 ASLR 组合不重复接收；实际布局读取保留历史 64 GiB 报告校验，正式新构建默认仍为 512 GiB。固定 root/bootstrap 归档另经只读审阅，16 份报告、16 份原 CLI/GCD 日志及 14 组运行摘录保持原摘要。历史缓存与主线新缓存分别按各自报告身份使用。

主线 `init-bootstrap-main-core-4k-v1/summary.json` 和 `init-bootstrap-main-core-16k-v1/summary.json` 均完成 CLI/GCD 的 84/85 条命名检查及各 119/119 上下文回收。实际 HyperOS `init-bootstrap-main-hyperos-4k-v1/summary.json` 保留 134 项输入身份，四种入口观察成功。lifecycle 各完成 13 项原 metadata 和 2/2 回收；original 各完成 3/3 回收，原 init、bootstrap linker64 与 secilc 实际执行，随后仍在原 vendor `plat_pub_versioned.cil` 读取处失败，EROFS 日志为 `read error -12 @ 0 of nid 755063`。原 policy 和 second_stage 尚未到达，两入口均由原 signal 6/reboot 结束，PID 1 status=1、deadline_stop=false，原分区字节保持。

共享 SDK36 原 init 在 4 KiB、16 KiB 四入口完成策略、second_stage、`SetupMountNamespaces` 观察和回收，随后仍因此阶段的 vendor VINTF/KeyMint 输入缺失及 keystore2 四次 critical 退出请求 reboot。两页 iOS 完整 runtime 每页 15 条命令、114 项输入编译成功，实际 original runner/execution 各自核对 159/163 项独立输入后编译成功。16 KiB 正式原生完整 Android CLI/GCD 的 Clock、Files、主页、同盘数据恢复和退出检查成功；这些结果未改变 HyperOS 原 init 的失败结论。完整结果与固定阶段审阅边界见 [results-init-bootstrap-main-20261113.json](../../../../experiments/native-linux/results-init-bootstrap-main-20261113.json)。

## 风险

vendor/system 的原 precompiled 摘要有差异，原 secilc 已实际执行，vendor CIL 解压读取失败阻止原策略加载。原 AArch32 服务、六条静态缺失服务、厂商设备、动态 linker namespace 及原 fstab 均可能产生后续失败。四槽固定接口需要宿主与内核同时采用同一计数；新增源码纳入既有 builder 输入摘要。根链接的 Linux 创建 mode 与原 inode mode 不同，须保留该差异。静态读取和 iOS 编译未覆盖设备授权、资源预算与持续性能。
