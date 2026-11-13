# 决策记录: SDK36 原始 vold 与 default-key

状态: proposed

## 问题

原 `fstab.ranchu` 的 `/data` 使用 ext4、quota、fileencryption、fsverity 和 `keydirectory=/metadata/vold/metadata_encryption`。现有[数据分区阶段](../../implemented/architecture/2026-11-13-原始init的数据分区与回收.md)验证普通 ext4 持久化、原 Keystore SQLite 的格式和完整性；原 vold 的加密、密钥 API、配额与内容校验仍待验收。完整 first-stage 和原 late-fs 需要按这些原始选项继续推进。

已完成两轮只读源码调查，结果位于 `work/native-linux-nce/sdk-vold-crypto-source-scan-v1/` 和 [v2 报告](../../../../work/native-linux-nce/sdk-vold-crypto-source-scan-v2/report.md)。v2 核对 29 份输入，前次 22 份文件的字节及两个源码提交保持。vold 固定为 `11760521a2e86389c56620ec50e780e8b17e40ca`，Android common 6.12 固定为 `fefe19f9fd971e83f58b475469116e21d5d795b3`；与 SDK build `BE2A.250530.026.F3`、incremental `13894323` 的精确源码对应尚未确认。

16 KiB raw v7 使用原 ramdisk init，已完成 metadata、五个 LP 挂载与 SwitchRoot。后续原 vold 请求创建 `userdata` 的 `default-key` target，长度 1,048,576 sectors，DM_TABLE_LOAD 返回 -22。原 `/data` 流程随后记录 mkdir 的 EROFS、user_keys 准备的 ENOENT；`vdc cryptfs init_user0` 退出 25，init 请求 recovery，缺少 `/misc` 后进入 `InitFatalReboot` 并调用 reboot。该阶段最终失败项为[原策略后的宿主 DM 回收](../../implemented/bug-fix/2026-11-13-原策略加载后的DM回收失败.md)，原 stdout 与候选 after 保持；vold 加密仍是完整 boot 的独立失败。

本次实际翻译内核关闭 `BLK_INLINE_ENCRYPTION`、`DM_CRYPT`、`FS_ENCRYPTION` 和 `FS_VERITY`，`default-key` 驱动也尚未移植。上述请求记录没有确认密钥服务操作成功，继续按原 API 返回、实际加密 I/O 与同盘恢复验证。原 v7 日志归属 Linux revision `d0f76a77e8c2ca68bd7d96190b8da09040eab072` 的 16 KiB 构建，CLI original stdout 的 SHA-256 为 `cb8fed0177931c48fb8d02b37c3b8f3ce40d131a329e2aef012edd4ed29ebc9a`。

v9 的 16 KiB 与 4 KiB first-stage、宿主 DM 回收、同盘 recovery 和 iOS 编译均已完成。四个 original 入口的原 vold 请求仍为起始 sector 0、长度 1,048,576 sectors 的 `default-key`，`DM_TABLE_LOAD=-22`；观察结果均为 `status=1`、`deadline_stop=False`，完整 boot 保持 pending。[阶段归档](../../../../experiments/native-linux/results-sdk-first-stage-20261113.json.zst)保存非敏感 target 属性与结果、原阶段身份和恢复结果。后续加密实验使用独立候选盘，继续保留这四盘的实际状态。

## 提案

2026-11-13 用户询问加密的必要性和性能成本。当前先按[SDK 无加密启动与性能基线](../testing/2026-11-13-SDK无加密启动与性能基线.md)移除独立候选的两项 `/data` 加密配置，验证完整启动和系统性能。本文保留原 fstab 的加密候选、实现选择和验收边界；其实际加密 I/O 与整机比较继续待验收。

已完成[内核移植比较](../../../../work/native-linux-nce/sdk-vold-kernel-compare-v1/report.md)，报告 SHA-256 为 `6bd12afcfee48d36d2d3b4cfb3e81ef94934b7ee713a87fc61d15ddc419e889d`。继续使用固定 LKL 6.12，移植 `default-key` 驱动、key type 验证与能力检查、bio skip 属性和 fscrypt 对应调用。保留现有 fscrypt 函数签名、设备枚举和 key 的 release 发布顺序。实施沿用已授权的离线开发范围，完整系统的验收条件保持。

必要修改涉及 14 份 Linux 文件，另在 CQHCI、UFS core 和 Exynos UFS 的原能力初始化位置显式声明 STANDARD，保持三项硬件路径的现有 key type 契约。profile 初始化继续清零；fallback 声明 STANDARD，DM 以全部类型开始与底层能力相交。当前 ncedisk 没有硬件 crypto profile，合法 wrapped key 按实际能力返回 EOPNOTSUPP。

固定 common 驱动作为独立上游输入保存，构建补丁适配当前 LKL 缺少的 `BLK_FEAT_ORDERED_HWQ` 声明，不引入 zoned operation 和 Android KABI/OEM 的其他修改。补丁对 IV 加法显式检查溢出，保留有效 target 的原 DUN 计算与底层偏移。`DM_DEFAULT_KEY` 保留 tristate，新增驱动需要的三项 blk-crypto API 继续使用 GPL 模块导出。

先构建两页独立候选，使用标准 AES-256-XTS 向量或独立宿主实现计算密文。16-byte IV 为 `LE64(DUN)` 接 8 个零字节；按底层分区、target 起点和 target 内 sector 分别计算物理位置与 DUN。覆盖 512/4096-byte 单元、页边界、相邻范围、未对齐和溢出拒绝、无硬件 wrapped key，以及 fallback 初始化失败后的 dtr/evict。随后验证软件和 inline FBE 的实际 I/O，再进入原 vold、Keystore2/KeyMint 与同盘恢复。

加密补丁与既有 NCE 架构补丁分别保存，共同进入 process 和翻译内核的来源摘要及生成文件清单。完成[阶段清理](../../implemented/process/2026-11-13-SDKfirst-stage阶段清理.md)后新建大小写敏感 APFS 构建卷和独立工作分支；本阶段重新登记输入，不沿用旧生成物的源码身份。

固定 vold 的 metadata 默认算法为 AES-256-XTS，创建 `default-key` target，当前 LKL 没有该驱动。common 的 `DM_DEFAULT_KEY` 依赖 `BLK_INLINE_ENCRYPTION`，`blk_crypto_init_key()` 与当前接口有 key type 差异。原 fstab 未声明 `wrappedkey_v0`，最终选项仍需在原二进制和实际属性中核对。fscrypt 的加密普通文件使用 `bi_skip_dm_default_key`，软件 FBE 和 inline FBE 的内容请求都需保留对应条件。

原 metadata 密钥路径使用 `kEmptyAuthentication`；`secret.empty()` 选择 Keystore2。原客户端等待 `IKeystoreService/default`，请求 `TRUSTED_ENVIRONMENT`，通过 BLOB domain 的 generateKey、createOperation、update 和 finish 完成操作。服务进入日志和 SQLite 完整性仍不足以说明这些调用成功，后续按原返回和跨重启文件访问验证。

原 KeyStorage 先写临时目录，目录 fsync 后重命名，再同步父目录。已有密钥与残留临时目录按原读取、删除流程处理。固定 `MetadataCrypt.cpp` 最后未使用 `mount_via_fs_mgr()` 的布尔返回；验收同时检查实际挂载设备、文件系统和文件内容。

先完成原 SDK ramdisk 的 first-stage、设备发现、原 LP 创建和 SwitchRoot，再用独立候选盘实施原 vold 加密。原盘及已有持久候选保留。每种页大小的实验连续使用其候选，失败后从实际状态恢复，保存原失败与数据摘要。

## 曾考虑的替代方案

- 单独启用 `DM_CRYPT`：它提供的 target 无法直接处理原 vold 的 `default-key` 请求。
- 仅复制 `dm-default-key.c`：当前接口还有 key type 和 FBE bio 条件差异，单文件编译无法验证这部分行为。
- 直接替换全部 blk-crypto 与 fscrypt 文件：尚未核对完整调用方和现有 LKL 修改，先比较必要依赖与实际改动。

比较后确认，整批 common 文件还会引入 Android KABI/OEM、zoned operation、`get_devices_new` 及 key 发布方式的其他变化，当前采用必要差异。仅适配驱动以调用旧 init_key 接口仍会缺少 key size/type、profile 能力交集、FBE skip 和失败 evict 的共同契约，这些判断分别保存在现有 blk-crypto、bio 和 fscrypt 的负责位置。

## 验收标准

1. 原 fstab、vold/Keystore2/KeyMint、policy、rc 和只读系统分区保持固定来源，实际属性和输入摘要可核对。公开比较源码继续注明其与二进制的对应边界。
2. 两页通过原 vold 创建和使用实际 `default-key` 设备，核对算法、target 范围、DUN、分区偏移、块读取与写入、flush/FUA 和实际启用的 discard 行为。
3. 原密钥生成或读取、KeyMint/Keystore2 操作、目录同步、同盘重启和普通文件内容检查成功；日志只记录验收所需的结果及非敏感属性。
4. 保留 target 创建、服务调用、挂载、I/O 和临时目录处理的实际失败，检查失败后的状态与恢复；停止和全部回收先于磁盘关闭及宿主映射释放。
5. FBE、quota 和 fsverity 分别验证原接口、跨重启访问、配额行为和实际内容校验，持续性能与内存成本由完整操作测量决定。
6. 两页 core、CLI/GCD 和实际生成的 iOS runtime/init 对象按新源码重建，必要回归覆盖已出现的故障。iPhone 授权、资源、持续性能与热状态继续待真机验证。
7. 按 reclaim-code-entropy 核对真实使用方、必要状态、替代实现与移除影响，文档和注释按 humanizer-zh 复核；按阶段记录清理中间产物，保护原盘、当前数据和失败结果。

## 风险

软件 blk-crypto fallback 会使用 CPU 与缓冲区，原 workload 的资源和持续性能尚未测量。KeyMint/Keystore2 的服务依赖、原文件系统选项和 checkpoint 可能分别阻断完整系统；调查与实现按实际进入范围记录。v7 已观察 first-stage 完成与原 vold 的 target 加载失败；原 vold 加密、完整 boot 和持续性能尚未验收。
