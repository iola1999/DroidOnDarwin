# 决策记录: 原始 init 的 ASLR 与 metadata 主线组合

状态: implemented

## 问题

[PTE 执行地址随机化](../architecture/2026-11-13-PTE执行地址随机化.md)与[SDK metadata 分区](../architecture/2026-11-13-SDKmetadata分区与GPT扫描.md)分别完成侧树验收，主线还包含[数据分区与回收](../architecture/2026-11-13-原始init的数据分区与回收.md)及[namespace 停止](../bug-fix/2026-11-13-原始init的namespace停止.md)。合并后的真实运行需要同时满足 ELF 输入、执行序号、两卷持久状态、原 rc 属性和回收顺序，侧树报告回放只能覆盖已保存的观察记录。

## 决策

组合源码固定为主线 `b8d746a`。使用独立的两页内核输出目录，保留此前归档引用的内核字节；每页继续使用 metadata 阶段留下的同一份 data/metadata 候选。先运行 core，再运行 lifecycle、original、同盘 recovery 的 CLI/GCD，随后执行报告边界回归与 iOS 编译。完整 first-stage、原 vold 加密和完整 Android 启动继续单独验收。

### 实际组合结果

驱动为 `python3 -B work/native-linux-nce/init-aslr-metadata-main-acceptance-v1/run-main.py`，完整命令、结果和输入摘要保存于同目录 `main-run.json`。16 个命令全部成功，`passed=true`、`inputs_unchanged=true`。环境为 Apple M5 Ultra、macOS 27.0（`26A428`）、Xcode 27.1（`27A9275`）；单 CPU、1 GiB RAM、128 MiB translation cache，sparse/flat/PTE/TBI、双映射代码内存和 timer preemption，客体页大小分别为 4096/16384 bytes。报告覆盖功能与生命周期，未测量整机性能。

| 验收 | 4 KiB | 16 KiB |
| --- | --- | --- |
| core CLI/GCD | 各回收 270/270，全部成功 | 各回收 270/270，全部成功 |
| 原 init lifecycle/original/recovery | 六入口成功 | 六入口成功 |
| metadata 属性与边界 | 454 项成功 | 454 项成功 |
| 历史 metadata 兼容 | 66 项成功 | 66 项成功 |
| metadata 最终完成条件 | 11 项成功 | 11 项成功 |
| 停止边界 | 154 项成功 | 154 项成功 |
| SDK LP 与 ASLR 契约 | 成功 | 成功 |
| 标准 iOS runtime 与实际 init 对象 | 编译成功 | 编译成功 |

每个 original 入口实际观察到三次 `/system/bin/init` exec，security/layout 序号均为 `1..3`，终末总数为 3；lifecycle/recovery 总数为 0。原 ELF headers、auxv、实际 PTE 与执行权限符合已声明输入。namespace 回收先于 metadata 目录检查与 fsync、两卷 syncfs/close/umount、DM 移除或记录 deferred 并关闭分区 fd、slots 0/1/2 关闭、上下文回收和最终 shutdown。original 的未完成卸载与 deferred 返回按原值保存。

| 同盘状态 | 4 KiB | 16 KiB |
| --- | --- | --- |
| data generation | 29 → 35 | 24 → 30 |
| metadata generation | 15 → 21 | 15 → 21 |
| 最终两卷状态 | clean，needs_recovery/orphan 均为 false | clean，needs_recovery/orphan 均为 false |
| Keystore SQLite | header 正确，quick_check 为 `['ok']` | header 正确，quick_check 为 `['ok']` |

SQLite schema 均包含 19 项对象，七张表、十二个索引。本轮导出只有 `persistent.sqlite`，WAL、SHM 和 journal 均不存在；只读查询使用单独 APFS 克隆，连接关闭后按清单删除查询产物。密钥 API 尚未验证。

original 保留原 namespace 的 metadata 属性 `EACCES` 和卸载返回，同盘 recovery 的目录权限、uid/gid 与标签符合原 rc，APEX sessions open 成功。四个 original 入口均到达 30 秒观察期限，kill 返回 -13，host stop 返回 0，wait status 为 9，并完成资源回收。16 KiB 末段原日志进入 `odsign` 与 APEX lazy service，尚未观察完整 boot。历史 NetBpfLoad 失败属于此前报告，本轮选取的原日志没有对应消息。

### 归档与独立审阅

[完整组合归档](../../../../experiments/native-linux/results-stock-init-aslr-metadata-main-20261113.json.zst)为 1,828,886 bytes，SHA-256 为 `7251d008fe0f8d88761fb13f3840ca69f78723f264cad664c5e3de30a2e1f1ab`；解码为 107,004,139 bytes，SHA-256 为 `4478be4546ff3592e6b429879006e679e125f85d91be282b7f05b92dd1275d07`。归档保存 21 份完整原报告、全部结果行及顺序、32 份原始日志、951 项当前不变输入，以及五份私有脚本的内容和摘要。生成入口为 `work/native-linux-nce/init-aslr-metadata-main-acceptance-v1/archive.py`，结果为该目录的 `archive-result.json`；输出自身未进入输入集合，16 个实际入口从归档重放成功。

独立审阅结果为 [combined-review-v1/result.json](../../../../work/native-linux-nce/init-aslr-metadata-main-acceptance-v1/combined-review-v1/result.json)，884,279 bytes，SHA-256 为 `02e048e86dd627290af37805a6277d266ff74b62ba02ca13c0a494e9fb3a2c1c`。归档、完整报告、日志、输入及私有脚本均相符；16 个归档入口独立重放成功。审阅登记 1,524 项文件摘要，包含前述 951 项输入；四份当前候选另以只读 inspector 核对，均等于各自最终 after 摘要。两库各 95 个 arm64/iOS 对象与 libtool 输入按字节相符，实际四个 runner/execution 对象为 iOS 26.0、SDK 27.1，runner 定义 `_nce_lkl_run`。本阶段无剩余阻塞项。

复验源码 `combined-review-v1/review.py` 为 26,043 bytes，SHA-256 为 `f0204ff07d0878160ab3d0c8482a80e905f0ed9c25ff68f7abd4adc5ef6aeca3`；复用的只读 helper 为 `work/native-linux-nce/init-data-stop-archive-review-v3/check.py`，SHA-256 为 `45cfb94abea968e4245cba574ea64dc4c31f08e740468b3282ce038dc3e12b5a`。小型复验源码与结构化结果继续保留。旧 ASLR/metadata 侧树和本阶段中间产物在审阅通过后按清单清理，原盘、当前候选及新 first-stage 工作树保留。

### 阶段产物清理

2026-11-13 执行 `python3 work/cleanup-2026-11-13/aslr-metadata-stage-cleanup-v1.py --apply`，exit code 为 0。独立核对后的固定计划 SHA-256 为 `c5d6bed96289bf107e68cd4139023d10cfa917626d57f6f8c5c6658e5a987281`，执行脚本为 `5d59e088e77306a132f344229d99b3aa4d499c9b0e3f2ce8414b73caef0e5411`。[执行记录](../../../../work/cleanup-2026-11-13/aslr-metadata-stage-execution-v1.jsonl)共 9,657 行，末行是 `completed`，没有失败事件，SHA-256 为 `9ff7c20193ff7f8dd7bd53d625ed656cd07de53cfc5f2ad50de34cb16948e17b`。

8,332 个文件以原文件移动到 [aslr-metadata-retained-v1](../../../../work/native-linux-nce/aslr-metadata-retained-v1/)，普通文件逻辑字节共 4,453,257,571。原 `work/exec-aslr/` 和 `work/sdk-metadata/` 的相对目录分别保存在该目录的 `exec-aslr/` 和 `sdk-metadata/`；主线阶段的移动项位于 `main/`。历史报告内的输入路径与摘要保持，当前路径由固定计划的 `retention_moves` 对照。三份 ASLR processes 镜像、六份 SQLite 导出、首次失败、原日志、报告及复验源码继续保留。61 个内核生成源码与头文件保留原路径，供 first-stage 生命周期和 ABI 来源核对。

保留文件逐项核对后删除 1,320 个固定清单项目，包括中间构建和展开源码；普通文件逻辑字节共 14,694,975,114，APFS 实际回收空间未测量。`git worktree remove` 完成两棵已合入侧树的删除，分支与原提交保留。执行前后均核对原盘、当前 data/metadata 候选、长期内核和 SDK first-stage 工作树；清理完成时主线仍为 `513b1cf`，工作区干净。进程与打开文件检查没有匹配使用方，`lsof` 的 TimeMachine SMB stat 警告原样保存在执行结果中。

### 复杂度审查与偏离

合并沿用 `exec_elf_images`、实际执行计数、metadata 输入和已有回收记录，未增加另一份运行状态或报告表示。metadata volume/completion 与 stop 回归显式传入报告所属的 ELF 输入，原 SDK 缺参数负例保留；十九处调用的静态清点、八个回归命令及三十二个历史入口回放均成功。移除显式 ELF 输入会破坏 ASLR 的完整执行关联检查，移除目录 fd 或停止顺序会失去 namespace 回收后的同步保证。

为保护既有归档的内核来源，本阶段使用新的两页内核目录。init 报告级 `kernel_stderr` 按 CLI/GCD 保存最后 recovery 日志；每个 `run.stderr` 和 32 份 raw logs 均完整，独立来源核对与重放成功。该诊断字段按实际范围记录，原归档保持原字节。

## 曾考虑的替代方案

- 只回放侧树报告后提交：无法检查合并后的真实 producer、内核与 host 组合，增加两页实际运行。
- 复用已有内核输出原地重建：此前归档仍引用旧内核字节，使用独立输出保留来源。
- 每个入口重新创建候选盘：无法检查本阶段真实连续写入和恢复，沿用当前同盘候选。
- 扩展归档字段后重新采集：逐 run 日志与原始日志已经完整，保留冻结来源并说明诊断字段范围。

## 影响

主线完成 ASLR、两卷持久化、原 rc 属性和停止回收的组合验收。后续原 SDK ramdisk first-stage 将自行发现块设备、创建原 LP、挂载并执行 SwitchRoot，需要新的运行候选保护当前持久状态。iPhone 执行、JIT 授权、资源、持续交互性能和热状态继续待真机验证；本阶段未安排手机连接或安装。
