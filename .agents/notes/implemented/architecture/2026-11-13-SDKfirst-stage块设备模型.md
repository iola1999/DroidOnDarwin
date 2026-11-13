# 决策记录: SDK first-stage 块设备模型

状态: implemented

本阶段已获主会话授权，在 `codex/sdk-first-stage` 独立树完成驱动源码，初始基准为 `b8d746a6364596e8815c7e3ba11f3962cb0da5cd`，包含主线 `6f041e4`。驱动与 bridge 的源码已集成，四个实验候选已准备；首次 16 KiB core 在客体执行前失败，第二轮 core 成功。[独立构建输入](../bug-fix/2026-11-13-独立构建的符号链接输入.md)与[Linux 大小写文件](../bug-fix/2026-11-13-Linux源码与构建的大小写文件.md)修正已通过最终来源核对。两页 v9 的原 `/init`、实际设备与生命周期、同盘 recovery 和 iOS 编译均成功，设备运行继续待真机验证。

## 问题

[SDK metadata 分区与 GPT 扫描](2026-11-13-SDKmetadata分区与GPT扫描.md)已提供标准分区、PARTNAME 和扩展 dev_t。此前块驱动使用 `add_disk(disk)`，没有真实 platform 父设备；固定 Android `DeviceHandler::FindPlatformDevice()` 沿 sysfs 的 `subsystem` 链接查找父设备，原 `/metadata` 平台 by-name 路径需要 `a003c00.virtio_mmio`。原 data fstab 指定 `/dev/block/vdc`，`HandleBlockDeviceEvent()` 从 DEVPATH 末段创建节点，partition_map 不能替代实际 `disk_name`。

来源调查保存在 `work/native-linux-nce/sdk-first-stage-source-review-v2/`。其中 `report.md` 的 SHA-256 为 `9e97d89917c4778af445bdffd1b979fb4438321afd81dc645f9e9624018a977b`，`sources.json` 为 `f4b84ad8b0b7524f56ee27d80816c52c6d4a3606015b611c678693b991456b97`。固定 system-core 为 `68be0c2c0006a0740d0b1809abe4717308f90d15`；其与 SDK 原二进制的精确构建对应继续待确认。

原 ramdisk 的两个串联 newc archive 已含 `first_stage_ramdisk/fstab.ranchu`，SHA-256 为 `b4572d0245a6bd33fec9b3fcee224a314e4ad62d44dd4f88d636983e6d47b144`。`androidboot.hardware=ranchu` 能选择该副本；无优先级更高的 boot-device 输入时，fs_mgr 从 metadata 平台路径提取 boot device。system 与 metadata 共用该父设备，可由原 DeviceHandler 创建全局 super 链接。SDK emulator 的原 system 父地址尚未固定，本阶段提供原 fstab 与设备链接所需的兼容模型。

## 决策

### 参数与设备身份

块驱动读取实际 `saved_command_line`，复用 Linux `next_arg()`，按完整键 `androidboot.hardware` 和完整值 `ranchu` 选择设备模式。首次匹配后结束查找，与固定 `GetKernelCmdlineFromString()` 的首值规则保持一致。临时 cmdline 副本和模式只存在于初始化期间；分配失败在设备注册前返回错误。新入口使用空格分隔的固定字面值 `androidboot.hardware=ranchu`。

ranchu 模式注册一个共享 platform 对象，名称为 `a003c00.virtio_mmio`，使用 `PLATFORM_DEVID_NONE` 保留完整名称。各盘通过 `device_add_disk(&parent->dev, disk, NULL)` 注册；仅 slot 0 的真实名称为 `vdc`，其余继续为 `ncedisk1`、`ncedisk2`、`ncedisk3`。缺失或其他 hardware 值保持现有名称及无显式父设备的注册。host 槽号、读写查询、BIO 偏移、容量、minor 和 GPT 扫描继续使用现有字段与标准块层。

`experiments/native-linux/lkl-bridge.c` 已新增 `nce_lkl_start_cpus_cmdline(slice_ns, applied_slice_ns, cpus, extra_cmdline)` 并保存当前启动实现；`nce_lkl_start_cpus()` 传空参数，`nce_lkl_start()` 保留现有包装和 ABI。新 full first-stage 使用固定 hardware 字面值，参数通过 `%s` 传给 Linux ABI 一侧的 `lkl_start_kernel()`，旧入口保留原 mem/loglevel/maxcpus、CPU 校验与 scheduler slice 行为。非空额外参数按实际 4096-byte cmdline 缓冲区检查长度，计入 mem/loglevel/maxcpus，预留当前空 BUILTIN_CMDLINE 增加的空格和结尾 NUL。`scripts/probe_lkl.py` 的导出集合和生成头、运行器的类型化函数指针已消费新符号；native 与 translation 使用的两个旧符号继续保留。实际 `/proc/cmdline` 和 fstab 选择随入口验收。

### 注册与释放

父对象在子磁盘之前注册，使用 Linux 的动态 platform 分配、名称复制和 release。父对象只需要 init 局部指针；成功注册后的设备注册引用及子设备 parent 引用维持其生命周期。

platform 注册失败由标准 API 释放未注册对象，本驱动注销已申请的 block major。后续磁盘大小检查、分配或 `device_add_disk()` 失败，先释放当前未成功注册的磁盘，再对已注册磁盘执行 `del_gendisk()` 和 `put_disk()`，随后 `platform_device_unregister()`，最后注销 major。父对象始终后于已注册子设备退出。

成功设备沿用现有内建块驱动的实例生命周期。LKL 的 `lkl_sys_halt()` 在 CPU 停止后清理 syscall tasks 和 threads，再调用 `free_mem()` 释放 Linux RAM；host 随后关闭磁盘和释放执行映射。当前 halt 没有逐个调用磁盘或 platform unregister，本阶段保留这一整体释放合同。标准 shutdown 和 coldboot/remove 链接的实际结果仍需运行验收。

### 源码依据

驱动源码交付时，本地 LKL 引用根为 `work/native-linux-nce/lkl-source`，revision stamp 为 `d0f76a77e8c2ca68bd7d96190b8da09040eab072`，Linux 6.12.0。该阶段只读已有文件，未重建该树。

| 来源 | 核对内容 |
| --- | --- |
| `include/linux/blkdev.h:728–733`、`block/genhd.c:449–458,509–521` | `add_disk()` 传 NULL；显式 parent 在设备注册与标准分区扫描前保存 |
| `include/linux/platform_device.h:173–200`、`drivers/base/platform.c:663–675,791–795,840–851` | 动态注册、无附加实例名称、失败 put 和成功 unregister |
| `block/genhd.c:547,696`、`drivers/base/core.c:3595–3602,3725,3884`、`arch/lkl/kernel/setup.c:157–164`、`arch/lkl/mm/bootmem.c:75–81` | 添加/删除设备的 parent 引用，以及 CPU/tasks/threads 结束后的整实例内存释放 |
| system-core `init/devices.cpp:275–322,488–545,758–793` | 真实 platform 识别、PARTNAME/by-name 与 DEVPATH 节点命名 |
| system-core `fs_mgr/libfstab/boot_config.cpp:143–164`、`fstab.cpp:553–574,913–950` | hardware 首值、原 early fstab 查找与 boot-device 回退 |

### 验收标准

驱动源码交付时已完成定向格式化、`git diff --check`、完整差异与引用/失败顺序审阅，记录实际源码摘要。该阶段的构建与运行结果留待主会话安排，源码审阅不替代实际行为验收。

1. 两页 CLI/GCD 新 full first-stage 入口记录实际 cmdline、early fstab、sysfs platform 对象及 subsystem 链接；真实整盘名称仅 slot 0 改为 vdc，DEVPATH/DEVNAME、MAJOR/MINOR 与原 host 槽号一致。
2. 原 init 冷启动实际生成 `/dev/block/vdc`、`/dev/block/platform/a003c00.virtio_mmio/by-name/metadata` 与 `/dev/block/by-name/super`，目标 dev_t/PARTNAME 与 GPT 分区一致。metadata 挂载、原 LP/DM、SwitchRoot 与 selinux_setup 分别按实际记录验收。
3. hardware 缺失、其他值、前缀相近键/值、裸键或空值、首值冲突和常规整值引号分别检查；默认 core、storage、原生/window、mounted-partition 与其他原盘入口保持现有设备合同。验证输入不得通过更高优先级 DT/property/bootconfig 改变 hardware 或 boot devices。
4. 在无盘、部分槽、platform 注册失败、磁盘分配/注册失败及正常停止后，检查实际设备与引用释放。保留标准 GPT、只读拒绝、BIO 分区偏移、namespace/mm/任务回收及 halt→host disk close 顺序；同盘恢复继续沿用既有 data/metadata 验收。
5. 新内核来源包括本驱动的实际 hash，两页 core、原库/原 init 与 iOS runtime/实际 init 对象按后续组合构建验收。原模块 ABI、完整 Android boot 和手机执行分别保留未验证边界。

### 本轮实现与检查

`nce_ranchu_devices()` 只读取 cmdline 副本，选择首个完整值后释放副本。`nce_block_init()` 用局部指针注册父对象，slot 0 名称分支与 `device_add_disk()` 使用同一局部结果；platform 注册失败直接到 major 注销，其余失败先释放已注册磁盘，再注销父对象。当前 patch 在一个 C 文件新增 45 行、替换 2 行，未新增外部依赖或持久模式字段。

已执行 `bash scripts/format.sh experiments/native-linux/lkl-nce-block.c`、`bash scripts/format.sh --check experiments/native-linux/lkl-nce-block.c` 和 `git diff --check`，均成功。完整源码差异只覆盖上述参数、父对象、名称与释放路径。驱动源码交付时，参数边界按控制流和固定 Linux parser 只读核对，设备与错误路径尚未构建或运行。humanizer-zh 审阅保留程序名称、来源、不确定性和生命周期合同，注释只补充首值规则、原 fstab 名称及实例内存释放。

| 驱动阶段交付时的文件身份 | SHA-256 |
| --- | --- |
| 基准 `lkl-nce-block.c` | `3220fc3c498c79b85bb4e90ce575b15a1212e56521bafbd0fa6b48563a62873b` |
| `lkl-nce-block.c` | `2a33b0a7f9d5317dd066eb8ad554ecc54b3a870c2ffe469e5954b50dc25e6f05` |
| bridge 集成前的 `lkl-bridge.c` | `03db6e0a384ac151d7294eb47f6e147964c29a3f47b5ae443957495a014aed50` |
| 本轮新增引用 `lib/cmdline.c` | `18d94acd858d0cfbdbe4b4a6b06d14402c8ef12065fa9eda8a69aef581d58c8a` |
| 本轮新增引用 `arch/lkl/kernel/setup.c` | `aadea263bf2abcbe3940fe4bcb1cc60d52c0bef67f5295d3598e6cad49d714b6` |
| 本轮新增引用 `arch/lkl/mm/bootmem.c` | `b73a408789b9effd72dbf7a3748069a5b2c57c75e4028059a60613ed5a18db67` |

`block/genhd.c`、`drivers/base/platform.c`、`drivers/base/core.c` 和固定 Android devices/boot_config/fstab 的当前 SHA-256 与上述来源调查登记一致。本地 LKL 是已有生成源码，revision stamp 与所读文件摘要分别登记，上游原始字节对应仍未验证。

本轮与授权方案一致。主会话随后完成启动参数入口与运行器消费，并加入实际 sysfs 查询和 Python 验收，见[原始 first-stage 记录](2026-11-13-SDK36原始first-stage启动.md)。固定 Android/Linux 来源的 16 项检查成功，`work/first-stage-device-source-review-v1/result.json` 的 SHA-256 为 `b3c6f206d7c20119abe2be9546b635893592ab81927831b74e9a88f8fe38828d`；新 sysfs 模块的 42 个宿主案例成功。实际设备、原 by-name 与完整 first-stage 的运行验收继续待完成。

### 复杂度审阅

真实使用方为原 metadata fstab、DeviceHandler 的平台/by-name 生成、原 data 节点，以及默认 ncedisk 的现有 core/storage/native/mounted-partition。新增内容只处理 init 期间的参数选择、一个标准 platform 对象和 slot 0 名称；对象的注册引用是成功后的必要所有权，失败路径的局部指针负责 unregister。删除 parent 会使平台链接查找缺少真实 sysfs 来源，删除 slot 0 名称分支会使原 fstab 的 vdc 节点缺失，删除首值规则会使重复参数选择与原 fs_mgr 不一致。host_ops、磁盘表示、分区表示和停止状态均保持现有实现。

### 本轮阶段结果

两页 v9 的 raw CLI/GCD 均完成实际 sysfs、设备号、GPT/LP、metadata by-name、原分区挂载与 SwitchRoot 关联检查；同盘 recovery 与退出回收成功。raw 使用 vdc，mounted recovery 使用 ncedisk0，两类入口分别沿用自己的设备合同。

详见[原始 first-stage 两页阶段结果](2026-11-13-SDK36原始first-stage启动.md#两页阶段结果)。

## 曾考虑的替代方案

- 全部入口静态使用同一 platform 和 vdc：现有 core、storage 和原生准备入口使用 ncedisk 名称，改变这些名称需要额外适配；本阶段保留默认合同。
- 新增 host disk_name/platform 属性：需要新的 host/kernel 字段、复制与校验；当前固定 SDK 已有同时选择原 fstab 的 hardware 参数，未发现必须独立设置属性的使用方。
- data/system/metadata 全部改为 vdc/vdb/vdd：原 data 路径要求 vdc，system/metadata 的使用方要求 PARTNAME 与 by-name；保留其 ncedisk 名称已满足当前消费者。
- 只改 uevent 字符串或由运行器创建 vdc 别名：原 init 重挂 `/dev` 后需要再次生成节点，FindPlatformDevice 仍要真实 sysfs 父设备；本阶段使用 Linux 的父对象和真实 disk_name。
- 保存持久设备模式或增加新的 halt hook：初始化结果和已有设备引用已满足使用方，现有内建驱动以整个 LKL 实例释放内存；额外状态及回调尚无消费入口。

## 影响

新 full first-stage 已提供固定 hardware 参数，其实际内核 cmdline 尚未运行核对。共享父设备的兼容结论来自固定 fstab 与 Android 源码，实际生成、原 SDK 二进制流程及链接回收尚未运行。更高优先级 hardware/boot-device 输入、原模块 ABI 与完整启动条件须随新入口记录；本阶段保留已有输入和验证边界。
