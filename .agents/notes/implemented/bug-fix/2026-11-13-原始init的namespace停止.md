# 决策记录: 原始 init 的 namespace 停止

状态: implemented

## 问题

原 init 数据盘阶段的 4 KiB、16 KiB lifecycle CLI/GCD 均已完成挂载、generation 写入和回收。original CLI 随后进入 post-fs-data，原 Keystore/KeyMint 注册、APEX 激活、cryptfs init_user0、derive_classpath 和 art_boot 有实际成功记录；30 秒观察期限结束前正在执行 odsign/odrefresh/dex2oat64。原始报告分别为 `work/native-linux-nce/init-data-main-4k-v3/summary.json` 与 `init-data-main-16k-v2/summary.json`，均因 `killed == 0` 断言 abort。

最初的停止请求未记录实际 errno。补充记录的 `work/native-linux-nce/init-data-main-4k-v5/summary.json` 保存了用户 kill(pid,9) 返回 `-13`（EACCES）；原 Keystore 日志包含 `Initialized Keystore key.`。旧断言使 wait、数据盘同步、host 关闭和映射释放未按标准流程完成。固定 Linux 的 namespace PID1 信号忽略通常返回 0，祖先 PID namespace 的 SIGKILL 可强制送达；本次停止在用户信号权限检查处失败。

## 决策

沿用离线开发授权，用户 kill 的实际返回、guest 输出与内核日志已经保存。ESRCH 后重新执行 wait(WNOHANG)，未完成回收则继续阻塞等待，按真正回收的 pid 与 status 判断退出。

syscall 247 提供 host 停止入口，params[0] 是调用者 PID namespace 中的正 PID，params[1..5] 必须为 0。入口拒绝带有 current->thread.nce_state 的调用；目标必须是调用者当前的直接子任务、已启动的 NCE 线程组首线程，并且在独立的直接子 PID namespace 中具有 PID1 身份。参数错误返回 EINVAL，目标不存在返回 ESRCH，guest 调用返回 ENOSYS，目标关系不符返回 EPERM。RCU 和 tasklist 读锁覆盖查找、关系检查与 group_send_sig_info(SIGKILL, SEND_SIG_PRIV, target, PIDTYPE_TGID)。原用户 kill 的权限检查、原 policy 和 rc 继续保留；仅在用户请求返回 EACCES/EPERM 后调用该 host 入口，分别记录两个实际返回。原 init 运行器在同一宿主线程执行 launch、stop 和 wait；父子关系按调用时的 real_parent 检查，线程退出后的 Linux 父任务重新指定规则继续生效。

PID1 身份使用 is_child_reaper(task_pid(target))，不依赖可被同组存活线程替换的 namespace child_reaper 指针，也不按线程组首线程的 exit_state 拒绝。固定 Linux 会保留仍有线程的组首线程，SIGKILL 经 shared_pending 和 complete_signal 停止线程组；目标已释放 sighand 的竞态返回 ESRCH，再按实际 wait 完成回收。该线程组边界已做源码审阅，独立运行验证尚未完成。

回收必须等待原 namespace PID1 的 wait 返回。固定 Linux 的 zap_pid_ns_processes 会停止并等待该 namespace 的全部任务，随后父任务才能执行 syncfs、目录关闭与卸载，再完成 kernel halt、宿主各槽关闭和映射释放。未完成回收的入口继续报告失败。

## 曾考虑的替代方案

- 延长观察期限：能够覆盖更长的原系统工作，期限到达后的停止错误仍需处理。
- 修改原 policy 或 rc：会改变本阶段观察的原始启动输入，采用独立的 host 生命周期请求。
- 停止失败后立即释放磁盘和内核：任务可能继续发起 I/O，回收前不能执行这些步骤。

## 影响

1. 首次失败、补充实际返回的诊断结果和修复后结果分别保留，明确区分用户 kill、host 停止与最终 wait。
2. 只允许停止调用者当前直接拥有的 namespace PID1 子任务，非法 pid、普通任务、其他父任务及 guest 调用须拒绝；已退出的任务按实际 wait 状态处理。
3. 两页 CLI/GCD 覆盖原策略已加载后的停止、namespace 全部任务回收、data 同步、DM 关闭、kernel halt 和全部映射释放。
4. 保持原用户信号、fork/exec、PID namespace、COW、Binder 和已有无数据 init 验收，完成对应 iOS 构建。
5. 按 reclaim-code-entropy 核对调用边界与状态所有权，技术文字按 humanizer-zh 审阅。

### 实现与验证进度

停止入口已写入现有 NCE process 和 syscall patch。停止请求、参数边界、直接拥有的普通 NCE 子任务及其他宿主线程检查放在独立 stock-init-stop.h；原 init 运行器持有 wait 和数据盘回收顺序，Python 的 stock_init_stop.py 核对实际请求、已回收 PID 的拒绝结果及请求到数据同步记录的顺序。历史成功报告沿用原格式，新增报告以 namespace_stop_contract=1 要求完整记录。两页新内核 core CLI/GCD 均成功，各入口 119/119 上下文回收，构建输入全部匹配；结果为 `work/native-linux-nce/init-stop-main-core-4k-v1/summary.json` 与 `init-stop-main-core-16k-v1/summary.json`。两页原数据盘六入口、iOS runtime 与实际 init 副本编译均已成功，数据盘与构建结果见[数据分区与回收](../architecture/2026-11-13-原始init的数据分区与回收.md#两页交付结果)。

新边界首次 lifecycle 失败保存在 `work/native-linux-nce/init-data-stop-main-4k-v1/summary.json`。检查依赖的 `/proc/PID/task/PID/children` 受 CONFIG_PROC_CHILDREN 控制，当前内核关闭该配置，打开检查触发 abort。该检查还同时检查父关系与 PID1 条件，后续移除，采用直接启动 flags=0 的普通 NCE 任务验证 namespace 限制，并用其他宿主线程请求同一个 PID1 验证父关系。普通任务从相同 ELF 的固定测试入口写出实际 getpid 后等待；父任务核对它与 launch 的 PID 一致，记录 host 请求拒绝、普通 kill 与实际 wait，最后删除临时就绪文件和测试入口。新 lifecycle 实际 3/3 上下文回收，原 init 阶段不添加此任务。该失败后的原数据盘继续用于标准挂载恢复。

独立审阅用新版停止校验只读检查了 21 条历史 observation，包括 6 条 original，全部通过；这些结果只说明停止报告的兼容，历史失败状态保持。PID1 线程组首线程先退出的边界仍仅有固定源码审阅。

复杂度审阅确认真实使用方是原 init 的期限停止。实现复用 Linux 的 PID 查找、信号与 namespace 回收，没有新增任务注册表、PID 缓存或通用信号参数。移除 host 入口会恢复原策略加载后的停止失败；移除父子或 namespace 检查会扩大调用范围；移除 wait 会允许未完成 I/O 的任务与磁盘关闭并发。独立停止头文件按请求和回收职责拆分，未增加转发接口。

### 两页接受检查

两页 original 的 CLI/GCD 均记录用户 kill=-13、host 247=0、wait=39/status=9，回收后的 host 247 返回 ESRCH。lifecycle 和 recovery 的普通直接 NCE 子任务实际 PID/ready PID 为 42；host 247 返回 EPERM，signal 0 成功，普通 kill 9 成功且 wait=42/status=9。非法 PID、额外参数、缺失 PID、宿主自身、其他宿主线程与 guest 调用的拒绝结果全部符合契约。

真正 wait 后才执行数据 statx、syncfs、目录 close、umount 和 DM 清理，随后 halt、host close、全部上下文与内核映射释放。original 的 umount=-1 按原策略保留，Linux 数据同步与目录关闭成功，同盘 recovery 随后完成 journal 回放与严格卸载。

`scripts/test_stock_init_stop.py` 在两页各检查 6 个实际入口、拒绝 148 个删除/改值/顺序变体，并通过 4 个历史入口兼容检查。结果为 `work/native-linux-nce/init-data-stop-report-contract-4k-v1/result.json` 与 `init-data-stop-report-contract-16k-v1/result.json`。独立源码与配置审阅记录在后者目录的 `review.json`，49 项摘要匹配；阶段归档及实际 iOS 构建见[数据分区与回收](../architecture/2026-11-13-原始init的数据分区与回收.md#两页交付结果)。当前交付保持原用户信号权限检查、原 policy 和 rc，新增 host 请求只承担观察器的期限停止。

独立归档复验的 16 个入口和 296 个停止负例再次通过，原报告、日志、输入和实际 iOS 对象身份已核对。[数据阶段记录](../architecture/2026-11-13-原始init的数据分区与回收.md#两页交付结果)保存最终复验与清理清单。首轮失败的 `init-data-main-4k-v3/summary.json`、`init-data-main-16k-v2/summary.json` 已压缩为相同路径加 `.zst`，解压后摘要保持；本轮中间镜像、展开根目录及编译对象已清理，复现使用归档中的构建命令、固定源码和输入摘要。原数据盘与观察日志保留。

### 未覆盖边界

kernel-generated signal 的调用边界必须保持 host 专用及父子关系限制。两页当前配置未启用 CONFIG_THREAD_INFO_IN_TASK，并启用 CONFIG_ARCH_THREAD_STACK_ALLOCATOR；put_task_stack 在该配置下不释放任务栈，NCE 状态由最终任务释放清理。组首线程先退出且同组线程仍运行的情况已按固定源码检查，独立运行样本仍待补充；sighand 释放竞态、ESRCH 后 WNOHANG/阻塞 wait 分支也仅做源码审阅。原系统 boot、数据加密与持续性能继续按[数据分区与回收](../architecture/2026-11-13-原始init的数据分区与回收.md)和离线目标验收。
