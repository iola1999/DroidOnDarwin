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
| `directFault` | `true` | 在信号处理入口恢复符合条件的已有数据页 |
| `sparseScan` | `true` | 减少映射切换时的页表扫描 |
| `performanceLogging` | `false` | 记录映射与返回入口统计，增加计时开销 |
| `diagnostics` | `false` | 会话配置保留此字段；普通桌面组合测试要求关闭，专项回归另行运行 |

坐标固定为 Android 显示的 `360×640` 像素；导航键支持 `home`、`back`、`recent`。点击包含按下和抬起，滑动按约 16 ms 间隔发送移动事件。输入通过现有通道进入 Android，提交成功后系统仍可能继续处理与绘制。每组最多 128 个操作；等待为 0..60000 ms，滑动为 16..5000 ms，整轮超时由 `--timeout` 指定，范围 30..900 秒，默认 600 秒。

同一组按正序、反序交替运行。首轮初始化、宿主缓存、设备温度和后台任务会影响结果；报告保留全部样本，不自动删除慢样本。关注首次安装时单独使用新设备；关注重复启动时先完成初始化，记录预热条件。诊断开启和关闭的样本分别分析。

## 结果与接口

每轮保存 `request.json`、`result.json` 和 `session.log`，整个矩阵保存 `summary.json`。运行编号为 UUID，重复编号失败。结果含构建编号、硬件与系统、宿主内存、完整配置、阶段与操作时间、低电量模式和热状态。脚本检查会话日志里的配置，并检查 Java 崩溃及 ANR。

计时使用宿主单调时钟。`start_to_ready_observed_ms` 包含准备和 JIT 连接；`core_to_ready_observed_ms` 从运行核心开始计时。桌面就绪由现有一秒日志轮询观察，两个指标包含观察延迟。`actions_ms` 统计操作提交序列，等待也在范围内；帧完成、目标应用就绪与 FPS 需要另外的图形或 Android 侧测量。日志中保留 SystemServer、SystemUI 和 Activity 的原有计时。

Mac 原始入口为运行进程的 `--benchmark-request 请求文件`，请求文件所在目录用于保存结果。iPhone URL 为 `droidondarwin://benchmark?request=<base64url 编码的 JSON>`，完整请求包含 `version: 1`、`runID`、`deviceID`、`configuration`、`actions` 和 `timeoutSeconds`。`droidondarwin://benchmark-status` 导出当前状态与设备列表；`droidondarwin://benchmark-stop?runID=UUID` 正常停止匹配的实验。普通网页不能使用这些入口，除非 App 设置或本次开发工具启动参数已启用自动化。

运行进程通过 `--allow-benchmark` 可为本次进程开启入口。请求不能指定任意文件路径、shell 命令或下载地址。App 进入后台时，已经开始运行核心的实验记为失败并请求正常停止；iOS 暂停进程时外部脚本可能只能先收集最后状态。

ART 和 Linux 专项回归的同构建对照继续使用 `scripts/benchmark_nce_art.py`。其中整段诊断包含测试程序，时间范围与普通桌面启动不同。
