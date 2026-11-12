# 决策记录: SDK36 原盘的 DM linear 映射

状态: proposed

## 问题

[SDK36 原 LP 与 loop 阶段](2026-11-12-SDK36原始分区与KeyMint启动.md)已在 CLI/GCD 完成五个分区、原摘要、只读访问、元数据和严格 loop 生命周期检查。原 policy 加载后，loop worker 在 `security_file_permission()` 对 backing `/dev/ncesystem` 的读取返回 -13，任务域为 `u:r:kernel:s0`，设备标签为 `u:object_r:tmpfs:s0`。原 init 的 file_contexts 与 property_contexts 读取随后返回 -5，未执行 KeyMint。正式分区映射需要保留原策略和块设备权限。

固定比较源码为 AOSP system/core `android-16.0.0_r2`、`68be0c2c0006a0740d0b1809abe4717308f90d15`，位于本树 `work/keymint-investigation/system-core`。`fs_mgr_dm_linear.cpp:92` 按原 LP extent 构造 `DmTargetLinear`，保留 readonly 属性，再由 `DeviceMapper::CreateDevice()` 加载表并激活设备。SDK 原盘的五个 extent 均为 device 0 的单一线性区间，可在既有 DM 配置下使用这一标准块设备映射。

## 提案

已获主会话授权，在原独立 KeyMint 树继续。slot 1 保留完整固定原盘的 O_RDONLY、共享锁和 Linux 只读设备；其他槽职责保持。正式五个分区改为标准 DM linear 表：逻辑起点为 sector 0，长度为已校验 sizelimit/512，底层起点为原盘绝对 offset/512，底层设备为 slot 1 对应原块设备。保留原分区字节、格式、标签、权限、rc、VINTF、原安全级别与 critical 重启规则。

通过标准 DM ioctl 创建、加载、激活和查询设备。核对实际 table 的 linear 类型、起点、长度、底层设备与 offset、readonly 和 active 状态，再读取容量、尾端及关键原文件。lifecycle 严格卸载、关闭 fd、DM_DEV_REMOVE 并确认后续查询返回 ENXIO；original 保留原 policy 的显式卸载拒绝，核对 fd 关闭、全部 guest 上下文回收、内核 halt、host disk close 和内核映射释放。

先复用已成功的 16 KiB core，完成变更后 lifecycle 与 original 的 CLI/GCD，再复验共享 SDK/HyperOS 输入契约和实际生成 init 副本的 iOS 编译。原 init 越过 property 准备后记录 KeyMint 首次 exec、原服务连接和下一处真实失败。4 KiB 原 init 检查依据本轮改变的执行边界决定，构建配置按页大小顺序运行。

## 曾考虑的替代方案

- 修改原策略或为 backing 选择额外读取许可：会改变本轮要求保留的权限检查。
- 将原盘复制为 RAM 文件并供 loop 使用：增加原分区的客体内存占用，重复已有原盘内容。
- 为正式分区启用 loop Direct I/O：其内部读路径直接调用 file read_iter，无法继续覆盖本轮已定位的 `vfs_iter_read()` 权限检查。采用原 Android LP 的 DM linear 分区实现。
- 增加 product/system_ext 宿主槽：会产生另一组资源分配；固定单 extent 可以继续使用 slot 1 原盘。

## 验收标准

1. 原盘与五个分区摘要保持，实际 DM table 与原 extent、设备、readonly 和 active 状态一致，容量、尾端与写拒绝符合原只读输入。
2. lifecycle CLI/GCD 的五个原分区挂载、19 项原元数据、七个原根链接及标准移除成功，页表告警为零，全部上下文回收。
3. original CLI/GCD 实际越过 property_contexts 读取；保留原 policy、域转换、APEX、ASLR 和原失败/reboot 规则，记录 KeyMint 实际进入程度与下一处失败。
4. 原策略后的宿主卸载拒绝、标准内核 halt、host close 和内核释放分别记录并校验。必要回归检查错误 table、越界、写许可及未完成释放。
5. 实际生成 init runner/execution 通过 iOS 编译；按 reclaim-code-entropy 检查已失去使用方的 loop 准备代码并记录移除影响，按 humanizer-zh 审阅技术文字。

## 风险

此阶段继续使用已挂载分区的 selinux_setup/second_stage 观察入口。原 first-stage fstab、设备发现与完整 Android 启动尚未通过。完整原分区会执行此前未进入的服务，后续失败依据实际系统调用与原日志定位。iPhone 执行、资源、JIT 条件和持续性能仍待设备验收。
