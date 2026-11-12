# 决策记录: SDK36 原盘的 DM linear 映射

状态: implemented

## 问题

[SDK36 原 LP 与 loop 阶段](../../proposed/architecture/2026-11-12-SDK36原始分区与KeyMint启动.md)已在 CLI/GCD 完成五个分区、原摘要、只读访问、元数据和严格 loop 生命周期检查。原 policy 加载后，loop worker 在 `security_file_permission()` 对 backing `/dev/ncesystem` 的读取返回 -13，任务域为 `u:r:kernel:s0`，设备标签为 `u:object_r:tmpfs:s0`。原 init 的 file_contexts 与 property_contexts 读取随后返回 -5，未执行 KeyMint。正式分区映射需要保留原策略和块设备权限。

固定比较源码为 AOSP system/core `android-16.0.0_r2`、`68be0c2c0006a0740d0b1809abe4717308f90d15`，位于本树 `work/keymint-investigation/system-core`。`fs_mgr_dm_linear.cpp:92` 按原 LP extent 构造 `DmTargetLinear`，保留 readonly 属性，再由 `DeviceMapper::CreateDevice()` 加载表并激活设备。SDK 原盘的五个 extent 均为 device 0 的单一线性区间，可在既有 DM 配置下使用这一标准块设备映射。

## 决策

已获主会话授权，在 `work/stock-init-keymint`、`codex/stock-init-keymint` 从 `27358ed0ef87a6b052302eab9ddf5d63527952ef` 继续。slot 1 保留完整固定原盘的 O_RDONLY、共享锁和 Linux 只读设备；slot 0 继续用于现有数据盘接口，slot 2/3 继续用于独立 vendor/system_dlkm 原盘接口。SDK36 的五个分区共用 slot 1。正式五个分区改为标准 DM linear 表：逻辑起点为 sector 0，长度为已校验 sizelimit/512，底层起点为原盘绝对 offset/512，底层设备为 slot 1 对应原块设备。保留原分区字节、格式、标签、权限、rc、VINTF、原安全级别与 critical 重启规则。

通过标准 DM ioctl 创建、加载、激活和查询设备。宿主按 Linux 固定 ABI 传递 312-byte `dm_ioctl` 和 40-byte `dm_target_spec`，协商版本为 4.48.0。核对实际 table 的 linear 类型、起点、长度、底层设备与 offset、readonly 和 active 状态，再读取容量、尾端及关键原文件。DM linear 将 bio 交给 slot 1 块设备；原 guest 对文件和设备的 LSM 检查继续执行。lifecycle 严格卸载、关闭 fd、DM_DEV_REMOVE 并确认后续查询返回 ENXIO；original 保留原 policy 的显式卸载拒绝，分别核对 fd 关闭、全部 guest 上下文回收、内核 halt、host disk close 和内核映射释放。

本阶段交付 16 KiB DM 映射与原 init 的实际观察，另保存 4 KiB 首轮 EROFS 挂载失败和两种页大小的 iOS 编译结果。16 KiB 内核复用 `keymint-core16-v4`；4 KiB 内核在专属 `lkl-translation-kernel-4k-vmalloc` 中构建。主线源码、内核缓存与原系统盘保持原输入。观察副本增加成功 read 返回记录，本阶段没有性能结论。

### 主线接受

2026-11-13 主线依次接收 `c4ffb10`、`27358ed` 和 `ddc064b6` 的固定阶段，保留 `2567514` 的 4 KiB EROFS gbuf、原根目录准备与 bootstrap binfmt 注销。共同 init 验证器采用 `original_input`，继续检查单条实际布局、guest 下界/上界、历史 64 GiB lifecycle 及 HyperOS original 的 512 GiB 要求；生成 HyperOS contract 输入前清除 SDK 专用分区记录，再保留原地址边界检查。

主线独立审阅 DM 固定 ABI、绝对 LP 起点、容量、只读条件、fd 与设备移除顺序。固定 16 KiB 原报告通过 31 项 SDK contract 检查，共享 HyperOS 输入通过 27 项检查，C 格式、Python 编译与差异检查成功。两页主线 core 的 121/120 项输入仍一致；正式主线组合的两页原 init 和实际生成 iOS 副本继续验收，侧树的成功及首轮失败保持原构建身份。

### 原盘与实际映射

固定原盘为 SDK `android-36/google_apis_ps16k/arm64-v8a/system.img`，4,415,553,536 bytes，SHA-256 为 `54a96f82a138e0919686cefffead8861e10c6189bf94188fef4fa08e24dc3ed1`。底层设备为 `/dev/ncesystem`，本轮实际 major/minor 为 254:1，Linux 编码值为 65025。DM 编码值为 64768..64772。查询 flags 为 49，包含 readonly、active 和 TABLE_STATUS 请求位；每张表从逻辑 sector 0 开始。

| 分区 | 原盘 offset，bytes | sizelimit/capacity，bytes | sectors | 原盘起始 sector | 格式 |
| --- | ---: | ---: | ---: | ---: | --- |
| system | 3145728 | 940838912 | 1837576 | 6144 | ext4 |
| product | 1196425216 | 2345381888 | 4580824 | 2336768 | ext4 |
| system_ext | 953155584 | 242716672 | 474056 | 1861632 | ext4 |
| vendor | 3542089728 | 101539840 | 198320 | 6918144 | ext4 |
| system_dlkm | 944766976 | 7393280 | 14440 | 1845248 | EROFS |

五个原单 extent 的摘要沿用前阶段固定值。宿主用完整原盘作为底层设备，因此 DM 使用包含 GPT/super 起点的绝对 sector；该表与只使用 super 分区设备的相对 sector 不同。实际查询、容量、最后一个字节读取及下一次 EOF=0 均成功。原 KeyMint 文件用写方式 open=-30；DM 设备用写方式 open=3，write=-1。

### 16 KiB 本机结果

`work/native-linux-nce/keymint-sdk36-dm16-original-v1/summary.json` 为 117,112,589 bytes，SHA-256 为 `d1e1f44e65d911e5c43318be40e026a29b2e9aee8790f638bda2eb1859e90be6`，138 项输入摘要与本阶段源码及产物一致。四入口观察校验全部成功，页表告警为零。

| 入口 | 上下文 allocated/freed | 实际停止与资源结果 |
| --- | --- | --- |
| CLI lifecycle | 2/2 | 五个 DM REMOVE=0、后续 STATUS=-6；system_dlkm 等待 2 轮，其余 0 轮 |
| GCD lifecycle | 2/2 | 五个 DM REMOVE=0、后续 STATUS=-6；system_dlkm 等待 2 轮，其余 0 轮 |
| CLI original | 197/197 | PID1 status=1，deadline_stop=false；原卸载拒绝=-1，关闭 5 个分区 fd |
| GCD original | 189/189 | PID1 status=1，deadline_stop=false；原卸载拒绝=-1，关闭 5 个分区 fd |

lifecycle 保留原 19 项 inode 元数据和七个根链接检查，关闭 fd 后按反向顺序移除 DM。original 的 kernel 域任务卸载 `/init.environ.rc` 返回 EPERM，五个分区 fd 均关闭；显式 DM 移除未执行。四入口随后分别记录 halt=0、host_disks_closed=true、kernel_release_result=0。原策略后的卸载拒绝和标准停止过程保持独立记录。

CLI/GCD 的 `/system/etc/selinux/plat_property_contexts` open=9，首次同 fd read(count4096)=4096，后续不再出现 read=-5。原 `/vendor/bin/hw/android.hardware.security.keymint-service` exec=0。两个入口中该服务均为 pid 84、uid 9999、`u:r:hal_keymint_default:s0`，servicemanager 对 IKeyMintDevice/default、ISecureClock/default、ISharedSecret/default、IRemotelyProvisionedComponent/default 的 device VINTF 检查通过。bootstrap APEX 激活 5 个包，完整 apexd 激活 48 个包。

keystore2 的首个数据库创建 open `/data/misc/keystore/persistent.sqlite` 返回 -30，后续只读 open 返回 -2；原 SQLite 记录错误码 14，随后 panic 和 signal6。`/data` 与 `/metadata` 此阶段还未准备实际可写挂载。后续原 bpfloader 执行 `/apex/com.android.tethering/bin/netbpfload`，CLI pid 191、GCD pid 188；NetBpfLoad v0.47 的 open `/proc/sys/net/core/bpf_jit_enable`（O_WRONLY|O_CLOEXEC）返回 -2，进程 status=1。原 `reboot_on_failure` 随后执行 `reboot,bpfloader-failed`。

keystore2 已记录当前 KeyMint 版本 Some(400) 和请求级别 TRUSTED_ENVIRONMENT。固定比较源码及原二进制参数对应 SOFTWARE；实际 `getHardwareInfo()` 响应仍未直接记录。本阶段保留该安全级别检查的待确认范围。

### 4 KiB 首轮与 iOS 编译

`keymint-dm-core4-v1/summary.json` 的 CLI/GCD 各 116/116 回收，121 项输入一致，SHA-256 为 `c1a20fc4d9d911be4e6937cbbc1f352e1072e6324a0dada2dd728bb5699efe99`。`keymint-sdk36-dm4-original-v1/summary.json` 保存首次 CLI lifecycle：五张 DM 表、容量及 EOF 成功，四个 ext4 挂载返回 0，`/system_dlkm` 的 EROFS mount=-12。宿主 REQUIRE 随后触发 SIGABRT，returncode=-6；尚未 exec 原 init，观察与回收没有完成。该失败报告 SHA-256 为 `f487e536a21872d0f386c6873ee36a92af8b206db6dbc6aded8b2b78cb396a75`。

此侧树尚未包含已固定的 `5391c16` gbuf 分组分配。主线 `25675143b45288714dd9e2b8ba275d7447e0614c` 已接收该实现，后续组合以新的主线 core 复验。本阶段保留首轮输入与全部分区，不继续新的 HyperOS 解压调查。

本机为 Apple M5 Ultra、macOS 27.0（26A428），iOS 编译使用 Xcode 27.1、iPhoneOS 27.1 SDK、arm64-apple-ios26.0 目标。两种 runtime 和实际生成 runner/execution 均编译成功。

| 报告 | 输入数 | SHA-256 |
| --- | ---: | --- |
| keymint-dm-runtime16-ios-v1/build.json | 113 | `3e35b5b1cbfa01e19acdc9d1c84ca5ab11f1b9aaa953d6e25d5d3be51856e88d` |
| keymint-dm-init-copy16-ios-v2/init-copy-build.json | 169 | `0fc2842d0974553969430f7da169d33a9ca4733c26ae0842afcd0354d8c84e57` |
| keymint-dm-runtime4-ios-v1/build.json | 113 | `f402ce9d3b6340af8f230d16c4102b41ef814ab4874c12af48b7616c8d8606f0` |
| keymint-dm-init-copy4-ios-v1/init-copy-build.json | 170 | `2060b36dae068a5c6a2e0228be45fff87c5c711a3fc6249a69518bdb77d25252` |

实际 init 副本编译报告分别保存 `source_observation_validated=true/false`，4 KiB 的编译成功和 EROFS 运行失败分别保留。iPhone 执行与持续资源表现继续待验证。

### 最短复验与回归

在本独立树执行，使用新的输出目录：

```sh
python3 -u scripts/probe_stock_init.py \
  work/native-linux-nce/keymint-core16-v4/summary.json \
  --runtime /Users/fan/project/github.com/iola1999/DroidOnDarwin/work/native-linux-nce/android/android36 \
  --desktop /Users/fan/project/github.com/iola1999/DroidOnDarwin/work/native-linux-nce/android/android36-desktop \
  --root-metadata /Users/fan/project/github.com/iola1999/DroidOnDarwin/work/offline-stock-bionic/work/init-investigation/system-root \
  --system-image /Users/fan/project/github.com/iola1999/DroidOnDarwin/work/native-linux-nce/android-reference/sdk/system-images/android-36/google_apis_ps16k/arm64-v8a/system.img \
  --sdk-partitions \
  --output work/native-linux-nce/keymint-sdk36-dm16-original-v2

python3 -u scripts/test_stock_init_sdk.py \
  --report work/native-linux-nce/keymint-sdk36-dm16-original-v2/summary.json \
  --output work/native-linux-nce/keymint-sdk36-dm16-contract-v5.json
```

`--lifecycle-only` 仅运行严格卸载与 DM 移除。4 KiB 首轮命令采用 `keymint-dm-core4-v1/summary.json` 和独立输出；本阶段不覆盖该失败目录。

`test_stock_init_sdk.py` 的 31 项检查成功，覆盖实际原 property 读取、KeyMint exec/VINTF、错误底层设备、起点/长度、readonly、容量尾端、元数据、DM 移除和原策略后的结束条件。共享 HyperOS 契约的 24 项检查使用既有 ASLR 4 KiB SDK 报告，新的 SDK 分区输入通过 `sdk_partitions` 选择对应观察参数；构造 HyperOS 输入前替换各自的分区、链接与元数据行。HyperOS 实际执行仍由其独立阶段验收。`test_hyperos_init.py` 的 3 项检查、C 格式检查、Python 编译检查与 `git diff --check` 均成功。

runtime 和实际 init 副本编译可在上述复验之后执行，使用新的输出目录：

```sh
python3 -u scripts/build_translation_runtime.py \
  --probe-report work/native-linux-nce/keymint-core16-v4/summary.json \
  --target ios \
  --output work/native-linux-nce/keymint-dm-runtime16-ios-v2

python3 -u work/keymint-investigation/build-init-copy-ios.py \
  --source-report work/native-linux-nce/keymint-sdk36-dm16-original-v2/summary.json \
  --runtime-report work/native-linux-nce/keymint-dm-runtime16-ios-v2/build.json \
  --output work/native-linux-nce/keymint-dm-init-copy16-ios-v3
```

实际 init 副本编译脚本、执行命令和输入摘要随结构化结果保存。

### 复杂度审阅与偏离

按 reclaim-code-entropy 核对实际使用方：`stock_init_sdk_mount()` 创建五张表并保存分区 fd/name/dev；`stock_init_sdk_cleanup()` 消费这些资源以反向卸载与移除；`probe_stock_init.validate()` 根据原固定 extent 独立核对观察结果。分区 fd、设备身份和原范围分别承担生命周期与输入校验职责。DM ioctl 准备函数集中固定 ABI 初始化，没有新增通用资源注册或多后端接口。

正式 loop 准备、临时 vendor loop 检查、LOOP_CLR_FD/LOOP_CTL_REMOVE 状态及 temporary 标记已删除。APEX 的标准 loop 路径继续有实际使用方。思想实验与失败回归分别检查移除影响：删除 table/EOF 检查会接受错误区间；删除 DEV_REMOVE/ENXIO 检查会留下注册设备；删除原策略后的结束分支会在 EPERM 后触发宿主断言并中断标准 halt。现有 SDK 回归覆盖这些边界。

执行中增加了原 KeyMint 服务连接检查和 4 KiB 首轮对照。4 KiB 完整原 init 原定复验受已知 gbuf 前提影响，本阶段固定失败与编译范围，后续在包含该既有修复的主线输入上组合。原 policy、rc、安全级别、critical 和 reboot_on_failure 要求保持。

## 曾考虑的替代方案

- 修改原策略或为 backing 选择额外读取许可：会改变本轮要求保留的权限检查。
- 将原盘复制为 RAM 文件并供 loop 使用：增加原分区的客体内存占用，重复已有原盘内容。
- 为正式分区启用 loop Direct I/O：其内部读路径直接调用 file read_iter，无法继续覆盖本轮已定位的 `vfs_iter_read()` 权限检查。采用原 Android LP 的 DM linear 分区实现。
- 增加 product/system_ext 宿主槽：会产生另一组资源分配；固定单 extent 可以继续使用 slot 1 原盘。

## 影响

此阶段继续使用已挂载分区的 selinux_setup/second_stage 观察入口。原 first-stage fstab、设备发现、实际可写 userdata/metadata、BPF JIT 和完整 Android 启动尚未通过。完整原分区已执行原 KeyMint，后续失败依据实际系统调用与原日志继续定位。iPhone 执行、资源、JIT 条件和持续性能仍待设备验收。

[结构化阶段结果](../../../../experiments/native-linux/results-stock-init-sdk36-dm-20261113.json)为 1,057,598 bytes，SHA-256 为 `f06cf30c20d88f2b510f7f794c408b060a7c6ced69321afc5ebf060104cd25e9`。它保存原 16 KiB 关键返回与事件索引、原始日志路径和摘要、完整 4 KiB 首次失败、两种 core/iOS 报告和检查结果。117 MB 原始报告与大日志保留在本树 work，前一 loop 失败阶段的固定归档继续保留。技术文字按仓库约定与 humanizer-zh 审阅。
