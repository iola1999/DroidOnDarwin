# 决策记录: first-stage 客体 stdout 回读顺序

状态: proposed

## 问题

[原 SDK first-stage](../architecture/2026-11-13-SDK36原始first-stage启动.md)使用父任务持有的 `/stdout` 文件保存客体 printf。`stock_init_run()` 先等待 namespace PID1、检查 reaped、sync 与 DM 清理，再通过 fd 1 的 pread 将文件内容写入宿主报告。实时 syscall/security/layout 记录在客体执行期间写入宿主 stdout；两种记录的报告位置不同。

固定 raw v7 的 CLI lifecycle 在第 168 行输出 observation，第 174 行输出 DM cleanup，第 177、178 行才回读 lifecycle 和 ordinary-ready，第 179 行输出 fd 1 的读取完成记录。CLI original 在第 64280 行输出 observation，第 64286 行输出 DM cleanup，第 64288 行回读 `stock_init_exec`，第 64289 行输出 fd 1 的读取完成记录。`/init` 的实际 security exec 位于第 131 行，在 observation 前。[只读行序结果](../../../../work/sdk-first-stage-dm-host-review-v1/stdout-order-v1.json)保存两份输入摘要与原运行器 Git 来源，SHA-256 为 `208ae49fab67e642681a1aff9b806195bf4828890afe61c7d2a0c6c974100624`。

原验证器要求 `stock_init_exec` 位于实际 exec 和 observation 之前；合成 raw fixture 也按这个假设排列。原 v7 已因[DM 回收失败](2026-11-13-原策略加载后的DM回收失败.md)被拒绝，这个独立顺序问题需要在新实际运行前修正。新增 guest syscall248 拒绝诊断同样使用客体 printf，必须按回读位置检查。

## 提案

raw first-stage 的客体 `stock_init_exec` 和 guest 拒绝诊断均要求位于 DM cleanup 后、fd 1 的 `stock_init_output` 前。实际 `/init` security exec 继续要求在 ramdisk ready 后、observation 前，`validate_exec_layout()` 的镜像、序号、地址和终末总数条件保持。两类记录分别核对自己的实际输出顺序。

修正 raw 合成 fixture 的排列，保留真实 mounted-partition 历史入口的验收。提前的客体标记、fd 1 回读完成后的标记，以及 observation 后的实际 security exec 均拒绝。原失败报告、stdout 与候选盘状态保持，修复后的实际运行使用新检查点和独立输出。

## 曾考虑的替代方案

- 按宿主 DM 新版本区分两种客体标记顺序：完成 wait/reaped 检查的 raw 收尾路径在 DM 清理后回读，统一按实际顺序检查可以省去无实际运行方的旧排列分支。停止失败的诊断路径仍在 REQUIRE 退出前读取输出，按原失败合同保留。
- 提前回读客体 stdout：会改变当前输出和收尾安排，既有实时 security/layout 记录已能检查执行顺序，本次只修正验证器与 fixture。

## 验收标准

1. 固定 v7 的只读行序核对成功，原报告继续保持失败。
2. 两页历史入口与修正后的合成 raw fixture 回归成功；客体回读和实时 exec 的提前、延后与缺失边界被拒绝。
3. 两页实际 raw original CLI/GCD 完成日志、exec 布局、DM 清理、同盘恢复与 iOS 编译验收。
4. 本次只修改相关验证和 fixture，first-stage 完成条件、原 init 状态范围及完整 boot 的验收要求保持。

### 本机回归

Python v4 的既有 467 项与宿主 DM 新契约 253 项均成功。真实 mounted-partition 历史入口保持，合成 raw 的客体标记按清理后回读排列；提前/延后的客体标记与 observation 后的实际 security exec 均被拒绝。追加 8 项 stdout 缺失、fd 缺失、fd 为 2 或 True 的 original/lifecycle 案例，均返回 False。生产校验器先完成两个 output 的字段、类型与顺序检查，再调用新宿主边界检查；追加回归保持已有顺序。来源摘要在运行前后保持。[既有结果](../../../../work/sdk-first-stage-dm-host-review-v1/python-contract-v1/legacy-result-v4.json) SHA-256 为 `002b83c20ece52821c902feba1e3720b2026d842a8c21690e035c6b22c7f8066`，[新契约结果](../../../../work/sdk-first-stage-dm-host-review-v1/python-contract-v1/host-result-v4.json) SHA-256 为 `bb4334b2f6c5d2bbd0fc9605fbccf2afde85824c60c28b1d0fa0ce7672e20a51`，原各版结果保持。

## 风险

客体文件输出的报告位置只能说明回读顺序。执行发生的位置继续由实时 syscall/security/layout 记录确认。并发客体写入仍遵守已有日志回归与完整记录检查，本次没有改变写入或采样实现。
