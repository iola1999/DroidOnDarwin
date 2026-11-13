# 决策记录: SDK36 原始 first-stage 启动

状态: implemented

本阶段在 `codex/sdk-first-stage` 实施，包含主线 `6f041e4`。原 ramdisk 读取、部署、启动参数、设备模型、DM 回收和观察工具已完成源码实现。首次 16 KiB core 在客体执行前失败，第二轮 core 成功；[独立构建输入](../bug-fix/2026-11-13-独立构建的符号链接输入.md)与[Linux 大小写文件](../bug-fix/2026-11-13-Linux源码与构建的大小写文件.md)修正已通过最终来源核对。raw v7 的 CLI/GCD lifecycle 成功，首个原 `/init` 完成 first-stage，随后[原策略后的 DM 回收](../bug-fix/2026-11-13-原策略加载后的DM回收失败.md)返回 EACCES，使完整 raw 阶段失败。专用宿主清理及[客体 stdout 顺序](../bug-fix/2026-11-13-first-stage客体stdout回读顺序.md)修正已完成，宿主输入与验收回放成功；两页 v9 的 core、original CLI/GCD、同盘 recovery 与 iOS 编译均成功，完整 Android boot 继续待验收。

## 问题

主线 `b8d746a` 已完成 mounted-partition 入口的 ASLR、data/metadata 持久化和停止回收。该入口由宿主预挂原分区并直接执行 `/system/bin/init selinux_setup`，原 ramdisk 的设备发现、原 LP 创建、首阶段挂载与 SwitchRoot 尚未运行。

固定 SDK ramdisk 的完整两段 newc 含 29 项，原 `/init` 为 AArch64 ET_EXEC，无 PT_INTERP，入口为 `0x30c000`、PT_LOAD 对齐为 16384 bytes。第二段的原 `first_stage_ramdisk/fstab.ranchu` 提供五个 logical 分区、平台 by-name metadata 和 latemount data。只读调查与固定来源见主工作区 `work/native-linux-nce/sdk-first-stage-source-review-v2/`；system/core 固定为 `68be0c2c0006a0740d0b1809abe4717308f90d15`，与 SDK 二进制的精确对应尚未确认。

## 决策

在既有原 init 观察工具增加显式 SDK ramdisk 入口，继续使用同一 PID 1、限时观察、执行日志和 host stop。输入保持原 ramdisk、fstab、module 配置、system GPT/LP、原 rc 与 policy 字节。输入档案明确区分 mounted-partition 和 first-stage，成功观察与完整系统 boot 分别记录。

启动参数通过新增 `nce_lkl_start_cpus_cmdline(slice_ns, applied_slice_ns, cpus, extra_cmdline)` 传入内核，两个旧启动 ABI 保留。新入口固定生成单个 `androidboot.hardware=ranchu` 字面参数，供原 fstab 选择与[块设备模型](2026-11-13-SDKfirst-stage块设备模型.md)使用；默认入口保持原 cmdline。更高优先级 bootconfig/DT 来源按实际输入记录。Linux `next_arg()` 与 Android cmdline 解析在内部引号和非空格空白上有差异，本入口使用已核对一致的普通空格字面形式。

[ramdisk 读取模块](2026-11-13-SDK36原ramdisk输入读取.md)先检查固定 SHA，再内存解码并按原顺序返回 29 项。运行器按原 type、mode、uid/gid、mtime、rdev 和文件字节部署客体初始 ramfs，两个同名 dev 目录按原覆盖顺序处理。archive 的 dev 字段保存在输入清单。LKL 启动宿主 syscall 任务需要的空 `/init` 在内核启动后按当前合同核对并移除，再部署原文件。记录清单省略文件载荷，解码和展开产物不落入宿主根目录。

新 PID 1 helper 仅准备隔离的 mount/proc namespace、核对基本进程与信号合同，随后无 stage 参数执行原 `/init`。原 init 自行重挂 dev/proc/sys、coldboot、创建 metadata/super 节点与 by-name、挂 metadata、使用 super 分区内的原 LP、创建 DM、挂载 system 并 SwitchRoot，随后挂载其余四分区。super 的 GPT offset 为 2097152 bytes，LP 的内部 offset 继续按分区坐标计算。data 属于 latemount，普通首阶段挂载范围单独检查。

原 `FreeRamdisk()` 删除初始 ramfs 文件，观察输出通过父任务已有的 Linux fd 1/2 以 pread 读取。helper 的文件表、fs_struct 和 mount namespace 与父任务分开；父任务不持有原 first-stage 挂载、目录或 namespace 的引用。实际 wait4 回收 PID 1 后，检查 owned PID 已不存在，再执行全局 sync。sync syscall 固定返回 0，卷错误仍由日志、host fsync/close、退出后的状态和同盘恢复检查。

DM 对象的生命周期独立于 mount namespace。父任务在部署 ramdisk 前临时打开 misc device-mapper control，保留唯一的 CLOEXEC fd 并删除临时节点；该 fd 只引用控制设备。回收与 sync 完成后执行标准 DM_REMOVE_ALL，再用 DM_LIST_DEVICES 检查实际列表为空。remove_all 返回 0 仍可能跳过 busy 设备，必须核对列表。实际 wait4 回收与 reaped 检查成功后，sync、DM 检查和输出读取的失败会保留返回值，并继续日志、halt 和 host close。setup、wait4 或 reaped 的 REQUIRE 失败仍立即结束宿主进程；本阶段没有增加通用错误回收。 v7 原系统加载策略后，普通 REMOVE_ALL、LIST_DEVICES 均返回 EACCES；当前修复在保留这两项实际返回后调用专用宿主 syscall248，核对内核注册树已空，再关闭 control fd。调用边界、互斥和新日志契约见 DM 回收记录。

ramdisk 的普通文件载荷和元数据表生成到本轮 runner 的头文件，按原顺序部署；部署后用 statx 和 pread/memcmp 核对实际内容与属性。archive 的 inode/dev 字段保留在输入清单中，ramfs 的 inode 号和目录 nlink 由 Linux 创建。父任务读取实际 cmdline 后卸载已有 proc，部署目录并恢复其 mtime，再恢复父 proc 供观察。helper 去除继承 proc，临时挂载自身 PID namespace 的 proc 完成检查，执行原 `/init` 前卸载，由原程序自行挂载。

原入口在部署核对后临时挂载 sysfs，读取实际 class 链接、父设备和 subsystem、整盘及 GPT 的 dev/uevent/ro/size、分区编号和起始扇区。读取函数每项保存 open/read/readlink 的实际返回序列和 close 返回，错误后继续查询；首个非预期错误进入汇总。预期缺失的链接仍输出 -2。caller 在读取完成后先卸载 sysfs，再检查读取与卸载结果，成功后才输出 ready。recovery 使用已有 sysfs 挂载并读取默认 ncedisk 模式。

Python 按 class 链接取得 canonical 目录后解析 device/subsystem 相对链接，核对 dev 与 uevent、整盘槽号和容量、GPT 的 PARTN/PARTNAME/起始与大小。整盘 major 和 GPT 的扩展 minor 来自实际读取。进度检查要求原 PID 1 创建的 super/metadata 节点与这些设备号一致；原 LP 的 readonly linear 范围使用相对 super 的坐标，mapper 链接和挂载来源关联到实际 dm-N 节点。五个 mapper 共用非零动态 major，且该 major 与实际 NCE/GPT major 不同。metadata 挂载先于 LP table，table 先于对应挂载，system 挂载先于 SwitchRoot，其余四分区挂载晚于 chroot、早于 PID 1 的动态 init exec。首阶段进度只采用该 exec 之前的成功 PID 1 操作。

运行使用新的 APFS 实验候选，保护已验收的 data/metadata 盘。raw lifecycle 检查 ramdisk 部署、sysfs/DM、PID 1 信号、停止回收及候选 after；raw original 执行原 `/init`；最后按旧 mounted-partition 入口在同盘 recovery 检查实际持久状态。raw 的执行观察、首阶段完成、文件系统恢复和完整 boot 分别记录。原 vold 后续可能格式化或加密 data，候选初始摘要与每次 after 均记录。

执行观察复用成功 syscall 的序号、auxv/PTE 和终末总数，原 `/init` ET_EXEC 按链接地址核对，后续 `/system/bin/init` 按原 ET_DYN/interpreter 核对。两类入口均按实际成功执行序号与终末 `stock_init_exec_count` 核对执行对；raw 首对另要求 `/init` PID 1。

观察工具在验收或派生字段解析失败后继续检查候选盘，保存原 rows、stderr、原输出路径与摘要、已成功解析的字段和 observation_errors，再写 failure 与 summary。非法 write/path 十六进制等错误已有宿主回归；客体运行后的保存流程由独立 fixture 检查，v6 的结束记录筛选失败、v7 的 DM 回收失败和 v8 的 recovery 挂载失败均保留实际输出、候选 after、failure 与 summary。完整 raw 命令完成全部观察、同盘恢复与来源核对后，要求 original CLI/GCD 两份记录的 `first_stage.completed` 和观察验收均为 True；失败保存 `first_stage_completion_error`、`failure.json` 与 summary，并以非零状态退出。lifecycle-only 保留生命周期验收范围，完整 boot 继续单独记录。

### 验收标准

1. 固定 ramdisk、原 `/init`、early fstab、九模块、system GPT/LP 与原 rc/policy 摘要匹配；内存解析错误与双 archive 边界回归成功。旧前段清单保留原范围。
2. 默认与 ranchu 模式的实际 sysfs 父对象、subsystem、uevent、dev_t/PARTNAME、平台与全局 by-name 符合各自合同；缺失、相近及重复 hardware 值按固定参数语法检查。
3. 原 first-stage 的设备发现、metadata 挂载、super 分区与原 LP/DM、五分区挂载、SwitchRoot 和后续 exec 分别记录，缺失项与原错误保留。观察完成不替代完整 boot 条件。
4. 父观察 fd 在原初始文件删除后仍可读取，PID namespace 回收先于磁盘关闭和执行/内核映射释放；同盘恢复的实际设备、clean/journal 和非敏感文件状态可核对。
5. 按最终源码锁定输入，依次完成两页 core、原 first-stage CLI/GCD、必要失败回归，以及 iOS runtime 和实际生成 runner/execution 编译。日志和首次失败保持，iPhone 执行、授权、资源及持续性能待真机验证。
6. 使用 reclaim-code-entropy 检查新增入口、参数、状态、父 fd 和候选的真实所有者与移除影响；文字按 humanizer-zh 复核。阶段归档独立审阅通过后按清单清理中间产物。

### 构建前结果与输入准备

原 ramdisk 读取的 18 项结果见[输入记录](2026-11-13-SDK36原ramdisk输入读取.md#本机输入结果)。`work/raw-first-stage-c-review-v1/summary.json` 保存部署、bridge、DM/trace、helper 和输出的 55 项宿主检查，SHA-256 为 `e0638fbe97bdc8ce8d2931ec107595e1853261ed809c4fe6e1b2ff097664f5c9`；28 个来源在执行前后保持。该结果对应新增 sysfs caller 之前的源码，随后 caller 的挂载、卸载和 ready 顺序另行只读复核。

`work/first-stage-device-source-review-v1/host-fixture-v3/result.json` 的 42 个 C 读取案例成功，SHA-256 为 `858654053ab319b8be96621551157385a757ce38f32bec6a146488238cb2ac28`。测试入口显式要求 `--archive` 和 `--gpt-report`，从主树 cwd 调用独立树脚本也成功。raw/default 两份完整 fixture 各输出 44 项查询与 1 项汇总；返回行由实际 C 生成，设备值来自明确标注的合成宿主输入，两份 baseline 与 v2 原字节相同。EINTR、短读、缺失、过长、分配失败和 close 错误均记录，结束后已打开 fd 全部关闭。实际 Linux sysfs 继续待验收。

`work/first-stage-validator-review-v1/schema-final-result.json` 的 307 项回放成功，SHA-256 为 `40dd03727203582b937c3a4d4c03a3fd0410bcf9954d3d8b834d45eef1272111`；包括 12 个旧入口、合成 raw、40 项实际 C DM 行及边界变体。`post-observation-final/result.json` 的 10 项失败保存检查成功，SHA-256 为 `0c37ebda0878505591b2baca6a9f234cbcddbb3ef092a6b01dfb9faa723b855d`。两份结果对应新增 sysfs 验收之前的源码。随后 sysfs 版的 423 项回放成功，`sysfs-result.json` 的 SHA-256 为 `82713700115cab018106ae7fbebf73d357c1d1b0ccf8ded20cea69606fe8f7d1`；`post-observation-sysfs/result.json` 的 10 项失败保存检查成功，SHA-256 为 `4122d9e736331453fd7819996208ce1273fc275d2cd714bdc3a6a7599b0cd460`。初始失败、fixture 预期修正和各版来源均保留在同目录。

最终回放保留上述 423 项，新增 24 项 mapper major 与 14 项完成报告检查，461 项全部成功。`completion-final-result.json` 的 SHA-256 为 `a3ea561bd417c79381263a60fd5ac833c9138944c243f01c816f35d5bea0d229`。实际生产尾段的 6 项 AST 调用检查成功，`completion-tail/result.json` 的 SHA-256 为 `3e439555c29164915225c5fade5e2fb57fa631c58da0ff3b63cba3036d896a1a`；三类完成失败均保存原 report/runs、completion error 与 failure stage 后非零退出。`post-observation-completion/result.json` 的 10 项原失败保存回放成功，SHA-256 为 `f43b8e03d91ed67393ef4a4207f529d6913177c00c75df28d35d909609ec6e8a`。测试与生产来源在执行期间保持，旧 307/423 版与 165 份历史产物摘要保持。

Python 设备值与完成条件的只读复核保存为 `work/first-stage-device-source-review-v1/python-values-review-v4.json`，SHA-256 为 `7a5fcad7cd5745761bb605cd3a28638f49455787328d2681dec61cc716937804`；15 项静态核对与固定 Linux/sysfs 来源相符。审阅促成 mapper major 与完整 raw 完成条件的修正，没有执行新增负例或客体运行。

本轮四个 APFS 候选位于 `work/native-linux-nce/sdk-first-stage-disks-v1/`，各页分别包含 data 和 metadata。来源为主线最终 clean 候选，源文件运行准备前后状态一致，候选的完整 inspector 字段与来源一致，inode 独立。清单 `work/cleanup-2026-11-13/sdk-first-stage-disks-v1.json` 的 SHA-256 为 `7aa7c31c549435b97dbd5560bc68ff30a4cf7ae2f87ac943e783cf8d68700d2c`。首次比较把不同的绝对 path 纳入内容检查，准备在第一个 clone 后停止；修正后保留并重新核对该 clone，仅创建其余三个，首次失败保存在 `sdk-first-stage-disks-v1-initial-failure.json`。

首次准备通过 10 项符号链接引用主树已有源码、工具和 Android 输入，清单 `work/cleanup-2026-11-13/sdk-first-stage-shared-inputs-v1.json` 的 SHA-256 为 `51d3ac5c2ee9ed216eadd9b321ca8444ad2a72c2ff399de9e507c85e745fe514`。该清单记录准备前后身份；首次内核构建随后写入共享 Linux 源码。恢复来源、独立 Unicorn 输入及目录检查见[符号链接缺陷记录](../bug-fix/2026-11-13-独立构建的符号链接输入.md)。

两页依次执行 core、raw lifecycle/original CLI/GCD 与同盘 mounted recovery、iOS runtime 和实际 init 对象编译。固定命令保存为 `work/sdk-first-stage-validation-v1/commands-v1.json`，SHA-256 为 `4220c9bd2ba9e33d39d17fcb7a8b2f4beaf6307135888cea8f54af7c51a17cae`。16 KiB core 在生产来源摘要冻结后启动，与仅修改测试文件的最后一轮回放并行；首次构建因源码链接和共享依赖绝对路径失败，原 `/init` 尚未运行。运行记录保留来源、命令、返回码和摘要检查，源码检查点在原 `/init` 运行前建立。

iOS 的首次准备通过另外两项引用读取 Unicorn 和 gfxstream include，清单为 `work/cleanup-2026-11-13/sdk-first-stage-ios-shared-inputs-v1.json`，SHA-256 为 `f4fbc8bd55d0cd5d78cc87e7847211779d7b49a40a3330034365dc619d46ed3d`。17 项 Unicorn 依赖摘要和 include 文件在建立引用前后保持。当前 `build_translation_engine.build()` 在写入前拒绝外部共享目标；本轮已移除三个 Unicorn 链接，并在本树重新完成 macOS/iOS 依赖构建。两份构建各自的 17 项输入摘要匹配，结果见[符号链接缺陷记录](../bug-fix/2026-11-13-独立构建的符号链接输入.md)。

生产源码的复杂度审阅保存为 `work/raw-first-stage-c-review-v1/complexity-review-v3.json`，SHA-256 为 `d6dcad76d7a9d0e6463df2b44c5a12f0a8ff5a7c1c5d8ca3caa8e06c1bff900d`。18 个 first-stage 生产来源和直接使用方已核对；同一轮的重复 sysfs 纯解析暂时保留，动态 init argv、其余分区相互顺序、全部子挂载迁移及 FreeRamdisk 删除仍待实际结果。新增构建输入修正的增量审阅见下文及[符号链接缺陷记录](../bug-fix/2026-11-13-独立构建的符号链接输入.md)。

`work/raw-first-stage-c-review-v1/build-input-review-v4.json` 的 SHA-256 为 `e218ae5a9447de402faa3e035cd56f114714abed5e9d980740822c52acb54759`；该版覆盖末级 source 链接、克隆和 Unicorn 目录/来源检查。输出目录祖先链接与 Linux 大小写文件随后发现，分别保留首次失败并继续增量复核。文字审阅 `work/sdk-first-stage-writing-review-v1/summary.json` 的 SHA-256 为 `92d2abe320f54517c6e6e235e613dcce191a2f9b3fdba3727e9a7820e2fae850`；挂载顺序、实际 exec 计数和共享输入的历史状态已据源码修正。

### 两页阶段结果

两页 v9 阶段代码固定为 `8d4308181ed0cc7c3f017d727840cf9014c3c874`，原 MAIN 准备输入仍以 `6f041e4` 的 21 项原路径为准。固定 Linux 为 `d0f76a77e8c2ca68bd7d96190b8da09040eab072`，完整 pristine/prepared 清单包含 86,901 项，构建在大小写敏感 APFS 的独立目录执行。验收环境为 Apple M5 Ultra、macOS 27.0（26A428），iOS 编译使用 SDK 27.1；本轮只验证本机功能、输入身份与交叉编译。

| 客体页大小 | core CLI/GCD | 独立内核核对 | raw/recovery | iOS 对象身份 | 最终 data / metadata generation |
| --- | --- | --- | --- | --- | --- |
| 16 KiB | 各 128 条成功，各 270/270 回收 | 726 项成功 | 六入口成功 | 16 个对象，31 项检查成功 | 32 / 23 |
| 4 KiB | 各 128 条成功，各 270/270 回收 | 728 项成功 | 六入口成功 | 16 个对象，31 项检查成功 | 37 / 23 |

四个 original 均 `observation_validated=True`、`first_stage.completed=True`，完成原 ramdisk 部署、设备发现、metadata 与五个 LP 挂载、SwitchRoot、原 ramdisk init 删除和动态 init exec。实际原始 init 的 exec 总数各为 4；观察状态均为 1、`deadline_stop=False`。原策略后普通 DM REMOVE_ALL/LIST_DEVICES 的 EACCES 保留，宿主 syscall248 清理成功并确认两棵注册树为空；guest 请求、六个非零参数和仍有 namespace 的拒绝边界通过。

两页各连续使用原候选，六入口 before/after 全字段相接。原 first-stage 后的同盘 recovery 读写挂载、generation、file/directory fsync、syncfs、close、umount、DM 清理、磁盘关闭及映射释放通过。最终四盘均 clean，独立 inspector 各读取两次，与最后 GCD recovery after 全字段一致。SQLite export 的 generation 分别为 32、37，quick_check 均为 ok，schema 各 19 项；本轮实际导出的 DB 没有 WAL/journal/SHM，带 WAL 的恢复继续按独立范围验证。metadata 目录属性的验收来源为 same_disk_recovery，raw 派生目录错误保持原记录。

两页 iOS runtime 和实际生成的 init runner/execution 均成功；14 个 runtime 对象与两个 init 对象各为 ARM64 iOS，LC_BUILD_VERSION 的 platform 为 2、minos 为 26.0、SDK 为 27.1，输入、配置、静态库及 init 对象摘要绑定通过。八项阶段均保持 105 项来源与 raw scope，进程组为空且没有清理信号。

[阶段归档](../../../../experiments/native-linux/results-sdk-first-stage-20261113.json.zst)保留验收结果、非敏感 trace、来源身份与实际失败。原 v3 至 v8 的生成、部署、筛选、DM 回收及配额挂载失败继续按对应记录保存；最终清单保留必要脚本、配置、生成源码、编译命令和对象身份，再清理可再生成的构建与展开目录。当前挂载卷仅用于 Mac 构建，已生成的 iOS 对象仍待整包与设备运行验证。

四份 original 仍观察到原 vold 的 default-key DM_TABLE_LOAD=-22，完整 Android boot 保持 pending。后续由[原 vold 与 default-key](../../proposed/architecture/2026-11-13-SDK36原始vold与default-key.md)继续处理加密与完整启动；原始签名 APK、HyperOS 3、设备授权、资源、持续交互性能与热状态按完整目标继续推进。

## 曾考虑的替代方案

- 从 vendor 补 early fstab：完整 ramdisk 第二段已包含原 fstab，直接使用原文件。
- 在宿主创建 by-name 或预挂五分区：该路径无法验证原 first-stage 的设备与挂载流程，继续由原 init 执行。
- 为每盘新增名称与平台属性 API：当前单个 hardware 参数和真实 platform 父对象已有具体使用方，先采用最小固定模型。
- 复制完整 init 观察器：PID 1、停止、日志、构建和执行关联已有共同使用方，扩展原入口并按职责增加 ramdisk 模块。
- 在当前已验收候选上运行原 late-fs：可能改变已有持久状态，创建新的实验候选并连续使用。

## 影响

原九模块为 `6.6.66-android15-8-gd0c43a640eab-ab13812146`，当前 LKL 使用 Linux 6.12；原 Modprobe、uname 和加载返回需实际观察，单个失败对原启动的影响依赖具体路径。共享平台父设备是满足固定原 fstab 的兼容模型，原 emulator 的 system 父地址尚未固定。原 metadata、vold/default-key、FBE、quota、fsverity 和 checkpoint 的运行行为分别验收，完整 boot 尚未确认。
