# 决策记录: HyperOS 原始 init 输入与元数据准备

状态: implemented

## 问题

主会话授权在独立 `work/continue-hyperos-jit` 调查完整 HyperOS 原 init 的可执行输入，基线为 `2dea3bf1c93844a2f219a2f947e4526a4db31c3d`。本记录交付固定输入准备；HyperOS init 尚未运行。开始调查时，`probe_stock_init.py` 固定 SDK36 清单、系统镜像摘要和 16 KiB core，`stock_init_metadata.py` 从 GPT/liblp 提取 system，运行器只 bind 原 ext4 的 `/system`。HyperOS 使用 raw system.ext4、vendor.erofs，product 和 system_ext 同处 system.ext4 根目录，原发布 ramdisk 尚未加入当前提取记录。

固定来源是 `NEORUAA/HyperOS-AVD` 的提交 `9d7ddd3f046e89d33cb205b5fbbf6dc5b728155c`、发布 `v0.1.0-a16-hyperos3-fuxi-r1`。该来源为第三方 MysticGSI 组装，发布构建脚本组合 HyperOS system 与 ranchu vendor/system_dlkm，并已有 rc、系统属性、policy 和 ramdisk 修改。本阶段保留固定发布包的字节及来源身份，原库、policy、rc、容器签名均按该输入保存。

## 决策

新增 `scripts/prepare_hyperos_init.py`，限定上述固定发布清单、分卷摘要和已保存扫描清单。工具读取 system/root/product/system_ext、vendor、原 ramdisk 的 init、rc、SELinux、fstab、APEX 与服务 ELF，在本树 `work/native-linux-nce/` 保存原文件、内容摘要、inode 元数据、链接目标、静态依赖和缺项。输出目录已存在则拒绝执行；新的失败目录保留错误标记与已经产生的日志。

`stock_init_metadata.metadata` 原先同时负责 SDK 的 GPT/liblp 切片和 ext4 inode 读取。现在抽出 `read_partition_metadata`，SDK 入口继续使用原切片、路径筛选和 `/etc` fast symlink 契约；HyperOS 直接读取已有 raw ext4。`links` 参数仅保留指定路径最终组件的原链接，文件内容校验继续解析链接。所有者、模式和 `security.selinux` 字节来自原 inode，复制文件的宿主所有者不作为原元数据。

EROFS 与 ramdisk 读取分别保留在 `hyperos_erofs_metadata.py`、`hyperos_ramdisk.py`。EROFS 读取原 inode、共享及内联 xattr，并与 `dump.erofs` 的 uid/gid/mode 独立比较，接受此输入实际使用的 4 KiB EROFS 格式。ramdisk 从原 LZ4 legacy 解压为单个 newc，保留 inode、uid/gid、mode、nlink、mtime、设备号、链接目标和内容摘要。原 `system_dlkm`、ramdisk 与 ranchu kernel 复用已有发布成员读取函数提取。

APEX 保存原 `.apex/.capex`、内层 `original_apex`、manifest、pubkey 与 payload。payload 文件样本单独存放，未加入 legacy `files.tsv`；正式激活仍由原 apexd 管线执行。rc 解析处理原续行，保存全部 179 条服务、class、disabled、原选项、imports 和声明的 `exec_start/start/class_*` 等触发命令。静态声明匹配尚未验证实际 import、属性与 namespace。

### 固定清单与原分区

最终输出为 `work/native-linux-nce/hyperos-init-input-v6/manifest.json`，4,683,214 字节，SHA256 `1ece2c7f87a9064a28a823309476d10044a795bad25b887e5e07a8859137ad06`。结构化阶段结果见 [输入准备结果](../../../../experiments/native-linux/results-hyperos-init-input-20261112.json)。完整清单记录每项读取命令及日志摘要；原来源目录为主树 `work/project-audit-2026-11-12/references/hyperos3-input`，准备工具只读该目录。

| 原输入 | 字节 | SHA256 |
| --- | ---: | --- |
| system.ext4 | 6724648960 | `dd65f33741bfce77b22b5c58b8d696b16da52f15a825acafcbcede47a3cfa3ca` |
| vendor.erofs | 52797440 | `0549061f9afe13c94baaa942eb08319b0498b66ac38b2cfd73cbbba541153fbd` |
| system_dlkm.erofs | 7393280 | `dd55dc42d416bcbd1510fc5a91d8e2fb1b08752b4d87e6de73afacae96ee6639` |
| ramdisk.img | 2285114 | `15760b3106f9cf92f4acd00aeaaab47151dd802aeed7fe19231ba1d918b29d51` |

清单保留 438 个分区文件，共 101,096,395 字节，另有 86 个 APEX 文件样本，共 31,486,748 字节。分区文件包括 139 份 rc、68 份 SELinux 文件。40 个原 APEX 中有 39 个 ext4 payload，runtime payload 为 EROFS。原启动静态依赖集合有 53 个文件；原 linkerconfig 及其依赖中的 7 个文件需要 APEX 激活后路径，静态依赖没有未找到项。

### 服务与策略边界

五个分区内 AArch32 服务均保存原文件、解释器 `/system/bin/linker`、直接 `DT_NEEDED` 和候选原库。60 次直接库引用全部找到了原 ARM32 文件。当前执行器的 AArch32 支持尚未实现，地址与 4 KiB 同余仅记录 ELF 布局。

| 服务 | 原路径 | 原 rc | class / disabled | 直接 DT_NEEDED 数量 |
| --- | --- | --- | --- | ---: |
| boringssl_self_test32 | `/system/bin/boringssl_self_test32` | `/system/etc/init/hw/init.rc:546` | default / false | 5 |
| zygote_secondary | `/system/bin/app_process32` | `/system/etc/init/hw/init.zygote64_32.rc:3` | main / false | 14 |
| media | `/system/bin/mediaserver32` | `/system/etc/init/hw/mediaserver.64bit_false.rc:1` | main / false | 17 |
| sigmahal | `/system_ext/bin/sigma_miracasthalservice` | `/system_ext/etc/init/com.qualcomm.qti.sigma_miracast@1.0-service.rc:1` | main / true | 10 |
| wfdservice | `/system_ext/bin/wfdservice` | `/system_ext/etc/init/wfdservice.rc:6` | main / true | 14 |

boringssl 原声明由 `on init && property:ro.product.cpu.abilist32=*` 的 `exec_start` 触发，zygote_secondary 在 `on zygote-start` 中由 `start` 声明。`on nonencrypted` 有 `class_start main`。sigmahal 原选项包括 interface、oneshot、disabled，静态扫描未找到直接 start action；disabled 服务的实际启动仍需执行检查。wfdservice 有 `vendor.wfdservice=enable/disable` 对应的 start/stop action。各项完整原选项、触发命令、库候选与哈希写入结果和清单。

APEX 内 `boringssl_self_test_apex32` 的 32/64 位原文件均存在，标签为 `boringssl_self_test_exec`，需正式激活后验证。`midfs` 原服务声明使用相对路径 `system_ext/bin/midfs`；原 `/system_ext/bin/midfs` 存在，实际 cwd 解析待验证。六条服务路径尚未找到：`/system/bin/adbd`、`/system/bin/traced_relay`、`/system_ext/bin/miuibpfservice`、`/system_ext/xbin/mqsasd`、`/system/xbin/su`、`/vendor/bin/boringssl_self_test32`。这些原声明继续保留。

vendor 的 `plat_sepolicy_vers.txt` 为 `202504`，system 同时有 `202404.cil` 和 `202504.cil`。plat、system_ext、product 三份 vendor precompiled 摘要均与 system 对应文件不同，完整值保存于 `selinux_compatibility`。原 `/system/bin/secilc` 及其依赖已保存，尚未执行 SELinux setup，policy 是否可加载待实际检查。

### 对原 init 管线的最小必要扩展

后续入口需接收固定 HyperOS manifest 并核对原来源摘要，允许其 4 KiB core；SDK36 保留 16 KiB 默认。运行器需使用已有 raw system.ext4，另 bind 根目录内原 product、system_ext，并按实际 EROFS 挂载 vendor，保留所有者、模式、链接和原标签。`extracted/vendor.img` 是 GPT 磁盘，EROFS 输入需使用已校验 logical vendor 分区。

原 init 字节引用 `/apex/com.android.runtime/bin/linkerconfig`，该 runtime EROFS payload 内文件已保存，`/system/bin/linkerconfig` 缺失。后续按原引用及正式 APEX 激活路径处理。原 ramdisk、fstab、模块和 first-stage 设备路径仍需核对，当前旧 4 KiB JIT 内核未启用块设备与 EROFS；后续先检查固定主线 core 的实际配置及 `NCE_EXEC`、ASLR 前提，再执行原 init。静态读取阶段没有改变挂载、policy、签名或 enforcing。

### 本机验证与保留失败

环境为 Apple M5 Ultra、macOS 27.0（26A428）、Xcode 27.1（27A9275）。实际命令：

```sh
env PYTHONPYCACHEPREFIX=work/native-linux-nce/hyperos-init-metadata-regression-v2/pycache python3 scripts/test_hyperos_init.py
env PYTHONPYCACHEPREFIX=work/native-linux-nce/hyperos-init-metadata-regression-v2/pycache python3 scripts/prepare_hyperos_init.py --release-directory /Users/fan/project/github.com/iola1999/DroidOnDarwin/work/project-audit-2026-11-12/references/hyperos3-input --output work/native-linux-nce/hyperos-init-input-v6
env PYTHONPYCACHEPREFIX=work/native-linux-nce/hyperos-init-metadata-regression-v2/pycache python3 work/native-linux-nce/hyperos-init-input-v6/validate_fixed.py
```

3 类回归成功，覆盖 newc 原字段、6 类坏归档、rc 续行与 exec_start/import。SDK 完整 metadata wrapper 重新提取原固定分区，156 项与已保存 metadata 完全一致，分区摘要为 `993193cfffd9b1a2ea034bfce5ec18fbb2856e259398ad6c1eb10b689b524e7d`；内容哈希不符和普通文件按原链接读取均被拒绝。结果为 `work/native-linux-nce/hyperos-init-metadata-regression-v2/summary.json`。已存在输出目录被拒绝，原 manifest 摘要保持一致且未写入 failure 标记。

最终 v6 复核成功：524 个文件、40 个 APEX 容器、3 个分区、6,780 份命令日志的摘要一致。与首次 v1 共同的 400 个文件，其字节和原 metadata 全部一致；31 项原 ramdisk metadata 也与首次结果一致。检查的 ARM64 文件均通过已有 4 KiB LOAD 校验。

准备失败目录保留：v2 的 debugfs `Mode:` 原输出有多个空格，初始表达式只接受一个，已修正；v3 将 SDK 风格 `/system/bin/linkerconfig` 列为必要输入，核对原 init 字节后改用实际 APEX 路径。v5 保留 `validation-failed.json`，该验证脚本误要求独立 `--blkid_context` token，原 token 为 `--blkid_context=u:r:blkid:s0`，修改验证条件后完成复核。上述失败发生在准备或验证工具中，HyperOS init 尚无实际启动结果。

### 复杂度审查与偏离

按 reclaim-code-entropy 核对实际使用方：SDK metadata wrapper 与 HyperOS 固定读取器都需要 ext4 原 inode 读取，抽出该函数可保留 SDK 契约并复用已有校验。EROFS 与 newc 各自有格式边界和资源管理，单独脚本用于对应原输入，未新增 provider 注册、通用镜像配置或重复标签状态。移除独立读取函数会重新耦合 GPT/liblp 与 raw ext4；移除原 metadata 或静态缺项会丢失后续 init 所需信息。APEX 样本与 legacy staging 分开，以保留原激活阶段。

原计划先只读调查；主会话随后要求可重现的固定准备工具，并补充全部服务、AArch32 原库和 APEX 激活后 helper，阶段产物相应扩大。实际 init 管线扩展留在后续独立实施；本阶段验收为输入与元数据准备。

## 曾考虑的替代方案

直接把 HyperOS 输入传给当前 SDK36 参数：清单、页大小、分区格式和根目录布局有固定假设，先记录真实差异。复制完整镜像到新运行结构：准备阶段读取原输入并把提取产物限制在本树即可，实际运行的可写副本后续独立准备。新增通用镜像框架：当前只有两个固定来源，所需差异可由现有读取函数和明确挂载参数表达。预挂载 APEX 文件样本用于原 init：正式激活需要原 apexd、容器及设备语义，本阶段保存样本供核对。

## 影响

本阶段完成可供主线原 init 管线复用的固定文件、分区、原 metadata 与来源摘要清单，全部原服务声明保留。静态依赖仍未覆盖运行时 namespace、动态加载、实际 rc import 与属性选择。原 first-stage ramdisk、模块、设备、SELinux enforcing setup、APEX 激活、AArch32 执行、完整 HyperOS init 和桌面尚未运行验证；iPhone、持续性能与热状态也未验证。此阶段只改准备工具及 SDK 元数据读取的职责划分，未运行 iOS 构建，既有 JIT 阶段结果见原记录。
