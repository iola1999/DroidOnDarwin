# 决策记录: first-stage 运行器的熵来源替换

状态: proposed

## 问题

[SDK36 原始 first-stage](../architecture/2026-11-13-SDK36原始first-stage启动.md)在检查点 `7f4434194b72a74429ce0eb06e759ed1ba2ac760` 完成 16 KiB v3 core 与 TCPMSS 来源、对象和符号核对后，首次 raw 命令在 `probe_stock_init.py:477` 生成运行器期间退出。原错误为「运行器启动参数或熵来源位置已变化」。本轮没有生成 CLI/GCD 可执行文件、summary 或观察行，客体尚未执行。

切除 `stock_bionic_setup()` 后的真实 `linux-runner.c` 中，启动声明和调用各出现一次，`(long)"/dev/random"` 出现两次：`seed_entropy()` 打开熵设备，core 分支的 `random_device` 创建随机设备节点。生成工具要求这个路径只出现一次，尚未执行替换就拒绝输入。熵初始化的完整 `long random[6]` 声明仅出现一次。

首次阶段记录 `work/sdk-first-stage-case-sensitive-validation-v1/raw_original_recovery-16k-v3-stage.json` 的 SHA-256 为 `71ce8656de251abb6ee1fa8674ff3556b8836db0e524fe71fd49cf844075ff33`，返回 1，23 项阶段来源保持，进程组为空且没有信号；原日志 SHA-256 为 `831f51757bfafd2845408afb427cacff1e741d6d13f18cc43f11b43f8d8c05c3`。诊断 `work/sdk-first-stage-generation-review-v1/first-failure-v1.json` 的 SHA-256 为 `cec9a606d8a985a2cd874592daea7d494c0df6c28d88e1337f7bc2fdb94ea689`，保存原来源、完整阶段记录、实际匹配次数和输出状态。上述文件位于原 SDK 工作树，当前 case-sensitive 工作树保留未完成的展开输入。

## 提案

将 first-stage 运行器的启动 ABI、调用和熵初始化转换放在一个局部函数，由原生成入口调用。熵设备替换限定到 `seed_entropy()` 的完整声明；原随机设备创建保留。转换前继续核对每个原片段只出现一次，再统一生成源码。原 ramdisk、内核、候选盘、恢复入口和完成条件保持现有合同。

回归直接读取仓库真实 `linux-runner.c`，验证含两个相同路径的输入可以完成转换，检查启动参数、熵设备选择和另一个设备声明。补充熵声明缺失和重复的拒绝检查；不增加通用 C 解析器或模板框架。实际生成源码继续由两页 raw CLI/GCD 与 iOS 编译验收。

新检查点使用新版本命令和 v4 输出。原 pristine/prepared 验收继续归属 `7f44341`；新命令明确登记该 Linux base 检查点，并重新核对原准备输入保持后复用。原准备输入来自主线 `6f041e4`，当前 CASE 的块设备模型与构建入口修改由新 kernel 的来源摘要单独核对；两组来源分别记录。原工作树、准备和失败报告的 HEAD 字段保持原值。

## 曾考虑的替代方案

- 允许路径出现两次后全量替换：会同时修改随机设备节点创建，当前修复只需要选择熵初始化的设备。
- 继续把转换留在 `main()`：宿主回归需要隔离其他准备与执行步骤；局部函数承担实际源码转换，回归可以直接调用。
- 为少量固定声明增加 C 解析器：当前唯一片段检查可以拒绝未知源码，额外解析器会增加与本次故障无关的维护内容。

## 验收标准

1. 真实运行器输入中的两个 `/dev/random` 用途分别核对，first-stage 熵初始化选择 `/dev/urandom`，恢复模式保留 `/dev/random`，随机设备节点声明保持原文。
2. 熵声明缺失或重复的输入在替换前被拒绝，原错误文本保留。
3. 原 v3 日志、阶段和诊断摘要保持；新检查点命令明确关联旧 Linux base 与实际未变的准备输入。
4. 两页 core、raw original CLI/GCD、同盘 recovery 和 iOS runtime/init 编译按新命令推进。原 first-stage 完成与观察验收继续要求两份严格 True；实际失败保持。
5. 按 reclaim-code-entropy 复核局部函数、调用方和回归范围，文字按 humanizer-zh 复核。

### 宿主源码回归

局部转换已实现，`probe_stock_init.py` 的 SHA-256 为 `9869e2734505724bffd6ce4968b07cb7c7752ff0830d8012b9d3205aadabdbee`，新回归 `test_stock_init_runner_source.py` 的 SHA-256 为 `b676d826bd3f7cdb1e6cf315657ca9ec4144ee3673202b25f702afbd468d33c5`。从 CASE 执行 `python3 scripts/test_stock_init_runner_source.py` 的 9 项检查成功，结果 `work/sdk-first-stage-generation-review-v1/source-tests-v1.json` 的 SHA-256 为 `dcf226471e2d2e0de488b925fdf29f9ea7a6ba7a743d1955cb4e018d565fe685`。真实输入含两个路径用途，转换保留设备声明，熵声明缺失和重复均被拒绝；三份输入摘要保持。该结果覆盖宿主源码转换，原 `/init` 与 C 编译按后续 raw 和 iOS 结果验收。

独立增量审阅核对了生产入口与回归两个实际调用方、四个原片段的唯一性预检及随机设备声明保持，未发现代码阻塞。审阅没有重新执行回归；结构化结果 `work/sdk-first-stage-generation-review-v1/independent-review-v1.json` 的 SHA-256 为 `76ce023068f7910890a585b3938280883e36a45c41b5fef8d4406ece51d5015e`。新增函数仅保存局部替换表，移除后源码转换和宿主回归需要分别维护；完整声明边界和原拒绝条件继续保留。

## 风险

源码生成仍依赖固定 C 声明；该声明调整后会触发显式拒绝，需要同步检查生成逻辑与实际输入。当前失败发生在客体执行前，原 `/init` 的设备、挂载、停止与恢复继续待验收。
