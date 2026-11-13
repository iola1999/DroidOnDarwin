# 决策记录: SDK metadata 分区与 GPT 扫描

状态: implemented

本阶段在 `codex/sdk-metadata` 独立工作树完成实现与两页离线验收，基准为 `b2812f4295b0a21a5612eab0f4fcae5106be33da`。4 KiB、16 KiB 的 CLI/GCD lifecycle、original、同盘 recovery、标准 iOS runtime 和实际 init 对象编译成功。metadata 候选由原 SDK GPT 盘创建独立 APFS 克隆，两页各连续运行 15 次，generation 均从 0 到 15；首次失败后的盘继续使用。阶段源码、记录与紧凑归档在同一次提交交付，独立审阅及合入由主会话组织。

## 问题

[数据分区与回收](../../implemented/architecture/2026-11-13-原始init的数据分区与回收.md)已完成两种页大小的 CLI/GCD 原 init 观察及同盘恢复。两页原日志仍记录 `/metadata` 的 restorecon、`/metadata/vold` 和 `/metadata/apex` 创建因 EROFS 失败，apexd 随后无法打开 `/metadata/apex/sessions`。当前运行器只创建根文件系统中的 `/metadata` 目录，原 rc 将根目录重新挂载为只读。

固定 SDK 的 `encryptionkey.img` 是独立 GPT 磁盘，唯一分区名为 `metadata`；整盘 18,874,368 bytes，SHA-256 为 `522bd0b6a2c92e37de465059bb9487bb9597a9aae032b469a8ebe0b38146d8c3`。该分区位于 LBA 2048–34815，大小 16,777,216 bytes，使用 4096-byte ext4 块。原 `fstab.ranchu:16` 要求以 `noatime,nosuid,nodev` 挂载 `/metadata`，并设置 `wait,formattable,first_stage_mount`。第 15 行的 `/data` 要求 `keydirectory=/metadata/vold/metadata_encryption`。

阶段开始前，host 和块驱动按槽号决定读写权限，只有 slot 0 可写；SDK mounted-partition 入口的 slot 2/3 尚未使用。两页已启用 EFI partition，块驱动的 `GENHD_FL_NO_PART` 仍阻止标准 GPT 扫描。只读调查与 58 项固定输入摘要保存在主工作区的 `work/native-linux-nce/metadata-first-stage-scan-v1/`。比较 fs_mgr/init 源码的 HEAD 为 `68be0c2c0006a0740d0b1809abe4717308f90d15`，与 SDK 二进制的精确构建对应尚未确认。

## 决策

### 磁盘接口与模式

`nce_disk_open()` 增加显式 `writable` 参数，取值为 0 或 1，每槽只保存这一个模式。现有调用方明确保留原模式：data 可写，system/vendor/system_dlkm 只读。host 的 open、锁、write、flush 和 close 同步按该模式执行；Linux 块驱动通过一个 host_ops 查询读取同一模式并设置 `set_disk_ro()`。无效模式在开盘前拒绝，关闭错误继续返回且其余槽继续关闭。

新增 SDK `--metadata-image` 输入使用 slot 2，并明确选择可写；仅接受 SDK LP 入口，SDK metadata 与 HyperOS vendor 的槽位冲突在开盘前拒绝。Python 输入同时拒绝原 `encryptionkey.img` 本身、system/data 同文件。路径名仅标识输入文件。两页 kernel、host_ops header、host/runtime 和实际 init 副本均按新接口重新构建并核对来源。

### GPT 与 metadata 生命周期

统一移除 `GENHD_FL_NO_PART`，保留整盘的 `first_minor=index,minors=1`。固定 Linux 为超出 minors 的分区分配标准扩展 dev_t，注册期通过已打开的 host 磁盘读取 GPT，分区 bio 由标准块层增加起始 sector。运行器读取实际 `ncedisk2p1` 的 sysfs/dev_t、start、size 与 PARTNAME，创建 `/dev/block/vdd` 并挂载原 ext4 分区；元数据与范围须与固定原盘一致。

每种页大小从唯一 SDK 原盘创建独立 APFS 可写克隆，持续使用该候选盘验证写入和重启恢复。原盘保持只读输入。guest 使用非敏感 generation 文件检查实际设备、ext4、文件/目录 fsync 和跨入口读取。父任务保留目录 fd 到 namespace 的 wait 回收完成，再执行 syncfs、目录关闭与卸载，随后清理 DM、停止内核、关闭 host 磁盘及释放映射。lifecycle 要求严格卸载；original 保存原策略下的实际返回，再用同盘 recovery 检查标准 ext4 journal 恢复和 generation。

原 policy、rc、标签和系统分区字节保持。分别记录原 rc 的 metadata 目录与标签、APEX sessions 访问、首个系统失败及完整启动状态。目录检查只读取已声明的非敏感路径和属性；普通持久文件使用本轮生成的路径。

当前阶段沿用[离线开发授权](../../implemented/process/2026-11-12-离线开发目标与持续推送.md)，实现与本机验证继续推进。完整 first-stage 的原 ramdisk、早期 fstab、boot-device 和平台/by-name 设备模型，以及原 vold 加密、FBE、quota、fsverity、checkpoint 分别验收。本阶段的挂载结果按 mounted-partition 范围记录。

## 曾考虑的替代方案

- 用 tmpfs 提供 `/metadata`：无法验证原 GPT/ext4 分区及跨重启持久化。
- 把 metadata 分区提取为新的 raw ext4 运行盘：增加一份运行表示，也不能验证标准 GPT 扫描和分区偏移。
- 扩大固定 minor 范围：会改变现有整盘设备号，固定 Linux 的动态 dev_t 已能分配分区设备号。
- 根据槽号或路径推断读写权限：SDK metadata 与 HyperOS vendor 共用 slot 2，却有不同模式；显式参数能由实际调用方决定模式。
- 预先增加分区扫描开关：历史未记录保留关闭扫描的要求，先运行验证 raw ext4、EROFS 与 SDK GPT/LP 的兼容。
- 同时实现完整 first-stage 和数据加密：需要另行核对原启动输入、设备模型、内核配置和 vold；先完成当前两页日志中的 metadata 文件系统缺失。

### 两页交付结果

完整结果、来源 SHA-256、过滤记录与原始日志引用保存在[阶段归档](../../../../experiments/native-linux/results-stock-init-sdk-metadata-20261113.json)。原始构建与日志位于独立工作树的 `work/native-linux-nce/`。环境为 Apple M5 Ultra、macOS 27.0（build `26A428`）；客体页大小分别为 4096/16384 bytes，宿主页大小 16384 bytes、单个在线 CPU、1 GiB RAM、128 MiB translation cache、Direct sparse/flat 与 timer preemption。完整参数和命令以报告为准。本阶段覆盖文件系统和生命周期，未测量整机性能。

| 验收 | 4 KiB | 16 KiB | 实际范围 |
| --- | --- | --- | --- |
| 新 core CLI/GCD | 各 84 项成功 | 各 84 项成功 | 新 kernel/header/host 组合与标准回收 |
| metadata init v3 | 六入口成功 | 六入口成功 | lifecycle、original、同盘 recovery 各 CLI/GCD |
| metadata 报告边界 | 266 项成功 | 266 项成功 | 设备、范围、generation、权限与同步顺序负例 |
| 停止边界 | 154 项成功 | 154 项成功 | wait 回收后目录检查/同步与历史停止契约 |
| SDK LP 边界 | 31 项成功 | 31 项成功 | 原 extent、只读 DM、元数据与移除 |
| raw EROFS CLI/GCD | 两入口成功 | 两入口成功 | 原 vendor/system_dlkm、整盘设备、挂载与只读写入拒绝 |
| 标准 iOS runtime | 成功 | 成功 | 新 static library、header、kernel 与 host 来源一致 |
| 实际 init iOS 对象 | 成功 | 成功 | v3 生成的 runner/execution、arm64、IOS 平台与 `_nce_lkl_run` |

host 模式回归 10 项成功，保留原五项历史检查；覆盖可写 slot 2、多可写槽同步、锁冲突、越界与关闭失败后继续关闭其余槽。关闭日志的 `sync_attempted` 由实际模式决定。输入边界回归 11 项成功，原 metadata 输入拒绝与槽冲突均在开盘前发生；既有无 metadata 的两页六入口报告按原字段继续通过。

提交归档保留每个最终入口的 88–91 条设备、目录、generation、同步与停止记录，12 个过滤入口回放成功。完整原始报告和 stdout/stderr 通过路径、大小及 SHA-256 关联，编译命令与阶段私有验证源码随归档保存；275 个记录输入的摘要在 `33ce79d1fa19c2d593fabb91c992969030cd7b60` 原阶段核对一致。

两页整盘设备均为 `254:2`，分区为 `259:2`；sysfs 的 PARTNAME=`metadata`、start=2048、size=32768 sectors。分区与整盘相同偏移的 ext4 超级块一致，容量、块设备 rdev 与挂载 st_dev 一致，EOF/越界读取均返回 0。SDK LP 原 system/product/system_ext/vendor 的 ext4 和 system_dlkm 的 DM-EROFS 仍通过原只读检查；原 SDK system.img 的输入摘要保持。

标准扫描后的原 raw vendor/system_dlkm 仍分别使用 `254:2`/`254:3` 整盘节点，无子分区。两页 CLI/GCD 均完成 EROFS 挂载、实际 st_dev/模式/uid/gid 核对、只读创建与块写入拒绝、目录关闭/卸载和标准 shutdown；各入口的 84 项原 core 检查继续成功，两个原 raw 输入的 SHA-256 保持。

候选初始清单保存在主工作区 `work/native-linux-nce/sdk-metadata-disks-v1/initial.json`，创建命令为 `cp -c`。连续性审计保存在侧树 `work/native-linux-nce/sdk-metadata-candidate-audit-v1/result.json`：两页各串联 v1/v2/v3 的 15 次运行，包含 v1 original CLI 失败。每次 after SHA-256 等于后次 before SHA-256；metadata generation 两页均 0→15，data 分别 14→29、9→24。最终两卷 ext4 clean、needs_recovery=false、orphan_present=false；metadata UUID、分区范围与 GPT 外围摘要保持。原 SDK metadata 和主线两份 data 输入的 SHA-256 均保持。

两页 v3 原 init 均观察到 `Loading SELinux policy`、`init second stage started!` 与 `SetupMountNamespaces done`，原 APEX 进程成功打开 `/metadata/apex/sessions`。四个声明目录在原 namespace 中的 stat/lgetxattr 均返回 -13；同盘 recovery 核对 `/metadata` root/root 0755、`/metadata/vold` root/root 0700、`/metadata/apex` 和 sessions root/system 0700，标签分别为 metadata_file、vold_metadata_file、apex_metadata_file。原 rc 与标签来源保持 `android36/etc/init/hw/init.rc:595–613`、`android36/etc/selinux/plat_file_contexts:691–693`，固定 fstab SHA-256 为 `b4572d0245a6bd33fec9b3fcee224a314e4ad62d44dd4f88d636983e6d47b144`。

两页选择的首个系统失败仍为 `ReadDefaultFstab(): failed to find device default fstab`，还记录 `init.unknown.rc` 缺失。原策略、rc、标签和只读分区字节保持。本次观察按既有 30 秒期限停止，完整 Android boot 继续待验收。

标准 iOS 两库分别为 SHA-256 `2b340d3d7b13b3e96dc8b331e3f51181f71cb6263a8dda48926182309e10b272`、`175e52c663cccff0b2290776e3592f751e84ea7991ca30d8dc3dc970d6d55e25`；host_ops header 均为 `087fcecaa3b4df112c8bdd0b285ae20063ef73aad458e9b00027a3e0e36ee234`。实际 init runner/execution 用 `arm64-apple-ios26.0` 与 iPhoneOS27.1 编译，runner 定义 `_nce_lkl_run`，全部输入与记录摘要一致。手机运行继续待验证。

### 原验收条件

1. 两页 CLI/GCD 验证标准 GPT 的 metadata 名称、范围、扩展 dev_t、块读取与分区边界；现有 raw ext4/EROFS 和 SDK GPT/LP 仍可使用，整盘设备号保持。
2. host 回归覆盖显式模式、只读写入拒绝、可写 slot 2、锁冲突、越界、flush 与关闭同步、失败后关闭其余槽。现有系统分区保持只读且输入摘要不变。
3. 两页 lifecycle、original 和同盘 recovery 各覆盖 CLI/GCD，generation 连续衔接，原盘摘要保持；原 rc 目录、标签和 APEX sessions 的实际结果明确记录。
4. 验收要求 namespace 回收先于 metadata/data 同步及 DM/host 清理，保留实际失败；删除记录、改值和打乱顺序的必要负例被拒绝。已有无 metadata 的历史报告按其声明的范围验收。
5. 两页 core、host/kernel/header、iOS runtime 和实际 init 对象构建成功，输入摘要与命令可核对。产物保存于 `work/`，阶段结束清理中间镜像、展开根目录和编译产物，保留原盘、当前数据与复现所需记录。主会话明确要求本阶段日志与库在独立审阅前保留；当前尚未执行产物删除，审阅后按项目清理约定执行。
6. 按 reclaim-code-entropy 核对模式的两个使用方、目录 fd 与候选盘生命周期；有重复代码时按真实职责判断共用范围。记录移除后的影响、实际备选和实现偏离。技术文字按 humanizer-zh 复核。

## 影响

标准扫描已经通过本阶段两页 raw ext4、EROFS 与 SDK GPT/LP 实测；其他未使用磁盘格式继续待验证。host_ops 变更要求新内核与 header/host 一起使用，旧构建身份继续归属原阶段。metadata 克隆已成为本轮持久数据，运行失败后继续从同盘恢复。

标准 GPT 可以创建真实 sysfs、dev_t 和 PARTNAME；当前 ncedisk 缺少原 init 用于生成平台 by-name 链接的父设备模型，完整 first-stage 仍需独立实现和验收。两页当前关闭 FS_ENCRYPTION、FS_VERITY、QUOTA 与 DM_CRYPT，原 vold 密钥存储与加密生命周期尚未核对。主会话另行比较固定 Android16 vold，`11760521a2e86389c56620ec50e780e8b17e40ca` 的二进制精确对应继续待确认；当前记录的源码对象名 `default-key` 与完整配置覆盖继续单独核对。完整 Android boot、通用 APK、iPhone 运行、资源、持续交互性能与热状态继续待验证。


### 实际失败与偏离

两页首轮 `sdk-metadata-init-*-v1` 的 CLI/GCD lifecycle 均成功，标准 GPT 给整盘分配 254:2、分区分配 259:2。首个 original CLI 的原策略允许 fd 4 目录 fsync、syncfs 和 close，拒绝 metadata held/path statx 与四个声明目录的 stat/lgetxattr，均返回 -13；umount 返回 -1。原 APEX 进程成功打开 `/metadata/apex/sessions`。v1 的观察检查因属性要求失败，日志与候选继续保留。

metadata 的 original 清理改为保存成对 EACCES 和零属性字段，身份仍由 launch 前 retained 记录及同盘 recovery 核对；data 原条件保留，lifecycle 要求 stat、同步、关闭和卸载成功。原 namespace 中的目录属性记为未读取到，模式、uid/gid 与标签在后续同盘 recovery 检查。这里的权限处理来自本次实际返回，不改变原 SELinux 策略。


两页同盘 v2 的 lifecycle/original/recovery 各两入口均成功，metadata generation 从失败 v1 的 3 继续到 9；原 APEX sessions 打开成功，同盘恢复的四个目录模式、uid/gid 与标签符合原 rc。两页 data 的 SQLite 格式与 quick_check 成功。报告变体回归各 266 项成功，旧无 metadata 的六入口继续通过。

独立预审发现 metadata 目录记录缺少 `label_result` 会使 rc 汇总在保存当前 run 前抛出 KeyError。汇总现在返回明确的字段错误与未验证属性，增加缺字段回归；该调整保留有效记录的行为。另检查实际生成副本发现继承的 Android/services 开盘分支在 metadata 槽冲突检查之前，正常 bionic 调用不使用这个分支。生成器将该分支替换为唯一的本阶段开盘段，槽冲突检查先于副本内全部开盘。标准 core/runtime 的原 Android/services 入口保留，最终源身份由同盘 v3 核对成功。

输入边界首轮曾因测试预设的宏输出前缀与实际 `runner:<line>: <expression>` 不同而失败。修正只涉及匹配方式，v1 结果保留；同一生成源码的 v2 检查成功。

raw EROFS 私有检查的 v1 两页 CLI 在挂载前失败：设备号、只读模式、容量和无子分区均成功，普通块节点以 O_WRONLY 打开返回 fd，原检查预设 -13。固定 Linux 普通块节点的 fops 在 write_iter 拒绝实际写入；v2 保存 open 成功后 write=-1（EPERM）的返回，文件创建=-30（EROFS），两页 CLI/GCD 完成全部检查。原 host fd 始终为 O_RDONLY、writable=0；v1/v2 原盘摘要保持。该验证只使用既有 raw 盘，不执行原 HyperOS init，也未增加跟踪的运行入口。

raw v2 的独立日志审阅拒绝 2,796 项字段/顺序变体，另发现私有验证器接受负的 node_mode 与 filesystem_flags。原 C 记录的对应字段为 uint16/uint64，四个入口的实际值均有效。原 v2 源码、运行报告与最初审阅保留；严格验证模块补充 ABI 范围检查后，原四入口日志及 16 项负值、16 项上界变体复查成功，异常数为 0。该复查只读取已保存日志，未新增内核或系统运行。

### 复杂度审阅

实际调用方通过 `rg` 与编译命令核对。`nce_disk_open()` 的生产调用位于 `lkl-main.c`、标准 `translation/linux-runner.c` 和 `probe_stock_init.py` 生成的原 init 副本；host 回归调用位于 `host-context-test.c`。原 data/system/vendor/system_dlkm 模式由这些调用明确传入，新增 metadata 也由同一入口传入。删除 `writable` 参数会恢复按槽号推断，slot 2 的两种用途无法同时保留原规则；每槽一个模式字段已满足 open、锁、write、flush、close 与 Linux ro 查询。

`stock-init-volume.h` 只有 data 与 metadata 两个实际使用方，共用 statx、retained fd 与清理职责。状态为 name/fd/device；fd 从 launch 前保留到 namespace wait 回收结束，随后同步、关闭与卸载。移除共用函数会重复两卷的 ABI、身份与同步顺序；删除 retained fd 会失去原 namespace 结束后对同一挂载进行同步的依据。分区识别继续由标准 Linux 完成，运行器只核对既有固定输入和实际设备。

两卷 guest 共用 generation 格式检查，data 的旧路径、文件格式与 JSON 字段保持。Python 只在本阶段入口列出实际两卷并按顺序检查，未增加通用磁盘注册表。生成副本移除继承的 Android/services 开盘段后，只剩本阶段使用的开盘路径；标准 core/runtime 的消费关系保持。候选归属由初始 `cp -c` 清单与逐轮 before/after 摘要记录，失败继续使用同盘，清理须由主会话独立审阅后执行。

与初始提案的偏离集中在实际 SELinux EACCES 的观察方式、生成副本的开盘顺序以及原产物暂缓清理。挂载、持久化、原目录恢复与系统字节契约保持；完整 first-stage 和原 vold 的验收范围继续单独推进。

### 合入前验收修正

独立审阅在 `33ce79d1fa19c2d593fabb91c992969030cd7b60` 归档上确认 37 份来源、275 个输入、两页实际运行、候选连续性和 iOS 身份一致，另发现两项验收遗漏。目录 mode/uid/gid 只有类型检查，负值与超出 ABI 上界的内存变体仍能通过；CLI 计算 `metadata_rc_validated=false` 后仍可能成功退出。原审阅保存在侧树 `work/sdk-metadata-final-review-v1/result-final.json`，SHA-256 为 `0ade43da66538fe9866295c9cf35c7ead18cda5566a5b6980b315982c1b33818`，字节保持。

目录验证现要求 mode 在 0..07777、uid/gid 在 0..UINT32_MAX，来源为 `stock-init-metadata.h` 的 `status.mode & 07777` 及 `stock-init-volume.h` 的 uint32 uid/gid。类型检查、错误后的零值字段和 EACCES 处理保持。完整 metadata 阶段在保存 summary 后必须完成原目录与 APEX sessions 验收；lifecycle-only 的实际入口没有 original/recovery，继续按持久化和原盘摘要验收，原 rc 状态保留 false。CLI 与报告回归共用完成条件检查，删除重复的完成 flag 判断；该函数仅处理 metadata 最终结果，不增加状态或运行入口。

本次修正只涉及 Python 验证与报告回放，不重跑内核、客体或 iOS 编译。旧实际构建报告继续使用原来源 hash，原失败结果保留；当前验证源码和派生报告分别记录新 hash。日志、候选与库继续保留。

两页 `boundary-v4.json` 各 454 项 metadata 检查成功，包含原 266 项和新增 188 项；各 66 项历史检查成功，包含原六入口、omitted/null 输入和意外 metadata 记录。新增属性变体直接调用观察验证，分别核对 ABI 上下界、bool/string 类型、拒绝后的零值和成对 EACCES，恢复阶段的原 rc 判定单独检查。这样可以区分 ABI 有效的属性与符合原 rc 的属性。原 `boundary-v3.json` 和原独立失败报告保持。

两页完成条件回归各 11 项成功，原无 metadata 的六入口也各自成功。回归从 `probe_stock_init.py` 选择原 metadata 汇总 AST 代码块，执行真实的汇总与完成条件；输入来自已保存报告，原盘摘要函数使用固定返回。mode/uid/gid/label 与原 rc 不同的有效属性及 sessions open 失败均先保存 `metadata_rc_validated=false` 再失败；lifecycle-only 保存同一状态后成功，原盘摘要失败继续拒绝。该回归覆盖 CLI 末尾的调用关系，未执行客体或磁盘操作。

[验收修正归档](../../../../experiments/native-linux/results-stock-init-sdk-metadata-validation-20261113.json)的 SHA-256 为 `81136092f7e8f89b46c5c4187330f89fdd8e931600eb35dc417612ab0c039458`。归档保存两页专项结果、当前四份验证源码的摘要和派生生成器，重新回放原 12 个过滤入口成功；30 份原 JSON 来源保持，118 份跟踪源码只出现预期三个既有验证脚本的变化，新增完成条件回归单独登记。原运行归档 SHA-256 继续为 `fad2aae39031cf9086454fdb55f852bddd9348d1fb1a277dc07aa3d5fefdc752`，旧报告的字段与来源身份保持。全部专项只读取报告和源码；旧 275 输入登记中的 work 产物沿用原身份，本次未重新读取镜像或候选。追加提交后由主会话组织专项独立复验。

### ASLR 合入与报告回放

专项独立复验在 `eb9f9f14086414a2ab2e97225a772e09fb02921b` 完成，1564 项内存检查成功。原 12 个非法 ABI 变体被拒绝，两份合法 `mode=0o754` 变体通过结构校验后使完整阶段失败；summary 先保存实际失败状态。结果位于侧树 `work/sdk-metadata-final-review-v2/result.json`，SHA-256 为 `2a21dd51c48cae37d7e1006654da97e4667d10a8dd15a2adbe927a80dbeff3e7`。原报告、失败和来源登记保持。

主线在 `d071bd1` 合入 ASLR 后继续合入 metadata。冲突处理保留 data/stop 的回收顺序、ASLR 的显式 ELF 输入、exec 序号与终末总数，以及 metadata 的 GPT、两卷属性和最终完成条件。metadata volume/completion 与 stop 回归从各自报告的 input 显式传入 `exec_elf_images`；历史报告继续传入 None，SDK 回归中故意省略该参数的负例保持。这里沿用 ASLR 的输入契约，未增加新的报告表示或运行入口。

[合并报告回放](../../../../work/native-linux-nce/init-aslr-metadata-main-acceptance-v1/merged-report-replay-v1/result.json)完成八个实际回归命令及 32 个入口验证。两页各 454 项 metadata、66 项历史兼容、11 项完成条件和 154 项停止边界成功，原 SDK LP 回归成功。32 个入口分别来自 metadata 的 12 个、无 metadata data/stop 的 12 个和 ASLR 的八个实际报告。回放只读取既有报告、源码及 SDK LP 元数据；当前候选盘和原日志保持。相关 C 格式与差异检查成功。

两页主线实际组合使用新的独立内核目录，core、data/metadata/ASLR 六入口及 iOS runtime/实际 init 对象均成功，完整归档和独立来源审阅通过，详见[主线组合记录](../testing/2026-11-13-原始init的ASLR与metadata主线组合.md)。metadata 两页 generation 均从 15 到 21，data 的 4 KiB 为 29 到 35、16 KiB 为 24 到 30；lifecycle/recovery 的原 rc 属性、original 的 APEX sessions 访问和两卷最终 clean 状态成功，original 属性读取的 EACCES 保留。当前候选继续保留，阶段清理已完成，侧树保留文件的当前位置见[清理记录](../testing/2026-11-13-原始init的ASLR与metadata主线组合.md#阶段产物清理)。
