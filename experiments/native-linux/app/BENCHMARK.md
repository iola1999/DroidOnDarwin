# 性能实验

`scripts/benchmark_dod.py` 控制普通桌面启动、设备内操作序列和正常停止。两端共用请求与结果格式。默认交替比较 C++ 解释器与 ART nterp，运行三组；每轮窗口保持可见。测试数据与日志保存在项目 `work/native-linux-nce/benchmarks/`。

## Mac

先完成主 README 中的构建与宿主检查，再运行：

```sh
python3 scripts/benchmark_dod.py --platform macos --repeat 3
```

默认使用独立实验设备目录 `work/native-linux-nce/benchmarks/device`。首次使用时从包内种子创建数据盘，后续沿用；报告标注首次运行前数据盘是否已存在。可以通过 `--data-directory` 选择已有实验数据目录。不能同时由其他 App 或测试占用该目录。

## iPhone

手机安装当前包并配置好 JIT 后，在设置 → 高级中启用自动化测试。使用已配对、已解锁且可由 `devicectl` 访问的设备：

```sh
python3 scripts/benchmark_dod.py --platform ios --phone F17 --repeat 3
```

脚本先激活 App，检查是否有活动会话并读取设备列表；只有一个虚拟设备时自动选择。存在多个设备时，使用结果目录 `devices.json` 中的编号传入 `--device-id UUID`。运行期间 App 保持前台，JIT 沿用已配置的内置或外部方式。首次连接所需的解锁、信任、配对与签名仍由平台决定。

每轮正常停止后重新启动 App，解决 iOS 核心只能启动一次的限制。脚本每十秒收集小型状态文件，结束后收集本轮日志与请求。运行失败或收集失败时结束整组实验；连接中断后可以再次用 `devicectl device copy from` 读取 `Documents/Benchmarks/<运行编号>/`，无须复制系统镜像。

当前 iPhone 接口已完成构建，设备侧的 Deep Link、JIT、USB 收集组合尚待阶段真机验证。

## 参数与操作

通过 `--matrix 文件.json` 指定参数组合。每组 `parameters` 可只列出需要改变的字段，脚本补齐默认值后发送；App 只接受完整配置，未知字段会失败。实验配置仅用于本次运行，日常设置保持原值。

```json
{
  "configurations": [
    {"name": "baseline", "parameters": {}},
    {"name": "nterp", "parameters": {"nterp": true}},
    {"name": "expanded-jit", "parameters": {"nterp": true, "expandedJIT": true}},
    {"name": "nterp-profile", "parameters": {"nterp": true, "performanceLogging": true}}
  ],
  "actions": [
    {"kind": "wait", "milliseconds": 2000},
    {"kind": "tap", "x": 180, "y": 320},
    {"kind": "swipe", "x": 180, "y": 560, "endX": 180, "endY": 120, "milliseconds": 400},
    {"kind": "key", "key": "home"}
  ]
}
```

| 配置 | 默认值 | 行为 |
| --- | --- | --- |
| `nterp` | `false` | 使用已适配的汇编解释器执行符合入口条件的方法 |
| `expandedJIT` | `false` | 编译已适配的字符串构造、拼接、Min/Max 和多态调用路径 |
| `fastBaseline` | `false` | 优先尝试快速基线编译，未覆盖的方法继续进入优化编译器 |
| `directFault` | `true` | 在信号处理入口恢复符合条件的已有数据页 |
| `sparseScan` | `true` | 减少映射切换时的页表扫描 |
| `performanceLogging` | `false` | 记录映射、返回入口和 ART 编译过滤统计，增加计时开销 |
| `diagnostics` | `false` | 会话配置保留此字段；普通桌面组合测试要求关闭，专项回归另行运行 |

坐标固定为 Android 显示的 `360×640` 像素；导航键支持 `home`、`back`、`recent`。点击包含按下和抬起，滑动按约 16 ms 间隔发送移动事件。输入通过现有通道进入 Android，提交成功后系统仍可能继续处理与绘制。每组最多 128 个操作；等待为 0..60000 ms，滑动为 16..5000 ms，整轮超时由 `--timeout` 指定，范围 30..900 秒，默认 600 秒。

同一组按正序、反序交替运行。首轮初始化、宿主缓存、设备温度和后台任务会影响结果；报告保留全部样本，不自动删除慢样本。关注首次安装时单独使用新设备；关注重复启动时先完成初始化，记录预热条件。诊断开启和关闭的样本分别分析。

## 结果与接口

内存默认按一秒间隔采集，阶段变化时追加取样，原始序列保存为 `memory.jsonl`。`result.json` 的 `memory` 含整体、会话阶段和启动子阶段统计；每项提供 `latest_bytes`、`mean_bytes`、`sampled_peak_bytes`、样本数量与最近成功时间。`summary.json` 按参数组合比较这些指标的全部样本、中位数及范围。`--no-memory-sampling` 可关闭采集，用于比较测量开销；原始请求对应可选布尔字段 `memorySampling`，省略时开启。

`footprint` 使用 Darwin 的 `phys_footprint`，`resident` 为驻留内存，`compressed` 为压缩内存，`virtual` 为虚拟地址空间，单位均为 byte。选择 RAM、缓存和编译参数时优先看 footprint 及其变化，虚拟地址预留不能直接换算为实际 RAM。平均值为成功取样的算术平均，包含阶段边界样本；阶段很短或调度延迟较大时，须结合样本数量和 `maximum_gap_ms` 判断覆盖情况。

采样峰值属于本轮观察区间，可能遗漏两次取样之间的短峰值；`process_lifetime_peak_bytes` 是内核维护的整个宿主进程生命周期峰值，可能包含实验开始前的占用，单独保留。查询失败记录错误码与失败次数，最新成功值保留其时间，不补零。每十秒、阶段变化和结束时写入状态与序列；异常退出可能缺少最后一个批次。查询总耗时和单次最大耗时记为微秒，文件写入及统计开销另通过开启/关闭对照测量。

Mac 的范围是 Android 运行进程，iPhone 的范围是 App 进程，包含同进程 Linux、ART、图形及宿主界面。独立的 JIT 辅助进程和系统进程未计入。采集从接受实验开始，设备创建、资源导入等先于该时刻的工作未覆盖。启动子阶段来自已有日志观察，带有轮询延迟；暂不按 Android PID 分摊共享内存。

会话阶段 `preparing`、`connecting`、`booting`、`running`、`stopping`、`stopped` 分别表示准备、JIT 连接、核心启动、桌面操作、停止中和停止后。启动子阶段沿用进度界面编号：0 准备、1 内核、2 系统服务、3 桌面、4 就绪。仅记录本轮实际观察到的阶段，极短阶段可能只有边界样本。

每轮保存 `request.json`、`result.json` 和 `session.log`，整个矩阵保存 `summary.json`。运行编号为 UUID，重复编号失败。结果含构建编号、资源清单中的内核/ART/系统哈希、硬件与系统、宿主内存、完整配置、阶段与操作时间、低电量模式和热状态。脚本检查会话日志里的配置，并检查 Java 崩溃及 ANR。

计时使用宿主单调时钟。`start_to_ready_observed_ms` 包含准备和 JIT 连接；`core_to_ready_observed_ms` 从运行核心开始计时。桌面就绪由现有一秒日志轮询观察，两个指标包含观察延迟。`actions_ms` 统计操作提交序列，等待也在范围内；帧完成、目标应用就绪与 FPS 需要另外的图形或 Android 侧测量。日志中保留 SystemServer、SystemUI 和 Activity 的原有计时。

Mac 原始入口为运行进程的 `--benchmark-request 请求文件`，请求文件所在目录用于保存结果。iPhone URL 为 `droidondarwin://benchmark?request=<base64url 编码的 JSON>`，完整请求包含 `version: 1`、`runID`、`deviceID`、`configuration`、`actions` 和 `timeoutSeconds`。`droidondarwin://benchmark-status` 导出当前状态与设备列表；`droidondarwin://benchmark-stop?runID=UUID` 正常停止匹配的实验。普通网页不能使用这些入口，除非 App 设置或本次开发工具启动参数已启用自动化。

运行进程通过 `--allow-benchmark` 可为本次进程开启入口。请求不能指定任意文件路径、shell 命令或下载地址。App 进入后台时，已经开始运行核心的实验记为失败并请求正常停止；iOS 暂停进程时外部脚本可能只能先收集最后状态。

ART 和 Linux 专项回归的同构建对照继续使用 `scripts/benchmark_nce_art.py`。其中整段诊断包含测试程序，时间范围与普通桌面启动不同。

ART 编译过滤日志在每类计数达到 1、2、4、8… 时输出，前两次附带方法样例；普通日志汇总中的计数为下限。单独请求 SIGQUIT 后得到当时完整快照，但线程暂停会影响该轮耗时。`accepted` 只说明过滤通过；方法仍可能在后续编译步骤失败。fork 子进程继承父进程当时的计数，分析时分别查看 PID 与运行期编号，不能相加作为系统总方法数。

脚本从目标 App 查询默认配置，随后覆盖 matrix 指定的参数。Mac 入口为 `--benchmark-configuration`，iPhone 状态文件包含 `configuration_defaults`；脚本没有另一份默认值。新增参数只在支持该参数的构建中运行，旧构建返回明确错误。快速基线编译的配置键为 `fastBaseline`，默认关闭；可与 `nterp`、`expandedJIT` 分别对照。按需开启性能日志时，`NCE_ART_FAST_BASELINE` 按编译线程报告已安装方法数量的下限，分析时不合并不同运行或线程的计数。

iPhone 状态查询可使用 `droidondarwin://benchmark-status?requestID=<UUID>`；响应原样返回规范化的 `requestID`。脚本通过编号确认本次响应，手机与 Mac 的时钟差异不会影响判断。App 仍在加载设备时会在加载完成后写出响应；状态中的 active 包括已接受、尚未开始的实验。
