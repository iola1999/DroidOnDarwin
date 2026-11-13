# 决策记录: 原系统观察器与 first-stage 版本检查

状态: implemented

## 问题

接续后的 `sdk-plain-init-16k-v11` 在宿主准备阶段失败：`stock_init_system_observer.py` 生成 contract 2，`stock_init_first_stage.prepare()` 仍只接受 contract 1，返回「原系统服务观察器需要无加密实验入口」。原 init 未执行。失败见主工作区 `work/native-linux-nce/sdk-plain-init-16k-v11/failure.json` 与 `work/sdk-plain-validation-v1/init-16k-v11.json`；运行前两份数据候选均为 clean。

## 决策

观察器模块导出当前 CONTRACT，由 first-stage 检查同一常量，保留无加密入口要求。部署回归使用真实 SDK ramdisk 和观察器生成清单调用 first-stage 准备，检查启动命令行确实引用生成的 wrapper。原模块的部署、权限、脚本参数、同一 boot_id 和服务检查保持。

## 曾考虑的替代方案

取消版本检查：会接受与部署和验收格式不同的输入。只把调用方字面量改为 2：后续格式调整仍需维护两处版本，采用生成模块的版本常量。

## 验证

- 使用真实 SDK ramdisk 的观察器与 first-stage 准备组合成功，原无加密 fstab、overcommit 与 wrapper 命令行保持。
- 原部署回归继续成功，并拒绝不同版本。
- 新运行保存实际 first-stage、原加载器、服务及停止回收结果；完整启动独立验收。

## 风险

准备检查只覆盖生成输入关系，原系统执行、服务启动与设备性能继续由实际运行验证。

## 影响

`system-observer-takeover-v2/result.json` 的真实 ramdisk 准备与旧版本拒绝回归成功，原 90 个 C 场景和共 62 项顺序、参数与组合检查成功。`sdk-plain-init-16k-v12` 已完成实际准备和 lifecycle 两入口，original 后续因 32 MiB 诊断限额停止；完整 boot 继续由原系统观察记录验收。没有修改系统库、原策略或原 rc。
