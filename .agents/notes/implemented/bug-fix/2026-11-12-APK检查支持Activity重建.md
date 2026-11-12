# 决策记录: APK 检查支持 Activity 重建

状态: implemented

## 问题

2026-11-12 完整 Direct 桌面验证在 JNI fixture 首次启动后失败。原始日志 `work/native-linux-nce/standard-jit-profile-desktop-apk/cli-0.log` 第 26006、26284 行分别记录同一进程、同一包和 marker 的成功结果，`version=1/value=101/created_version=1`，`launch_count` 为 1 和 2，`created_now` 为 true 和 false。其间第 26226 行记录 `ApplicationInfo updating`，后续 WindowManager 记录 Activity relaunch。JNI 库已加载并返回 101；`NceApkInstall.application()` 将第二条记录判为 `Duplicate application JSON after launch`，导致检查失败。

fixture 在 `MainActivity.onCreate()` 中每次同步保存 `launch_count + 1`。该字段实际统计 Activity 的 `onCreate` 次数，一次启动命令可能产生多次创建。现有助手要求每次命令只产生一条应用 JSON；宿主也固定每个 phase/artifact 只有一条，两个假设均需修改。原始签名 APK 和 fixture 源码保持不变。

## 决策

在独立工作树 `work/offline-apk-recreation`、分支 `codex/offline-apk-recreation` 基于 `4a58459` 实现，主工作区仅新增本记录，不修改产品源码或启动完整系统。

助手继续以启动前偏移开始读取日志，在首条合法结果后固定 `scanEnd`，逐条保留快照内已完成的应用记录。每条记录沿用现有 `android_apk_fixture` 字段和原始 `application` 对象；同一 phase/artifact 可以连续出现多条。首条首次安装记录必须为 `launch_count=1/created_now=true`，同一窗口后续记录必须计数恰好增加 1、`created_now=false`，版本、返回值、marker 和创建版本全部一致。重复计数、跳变、错误版本、失败记录及私有数据重建均拒绝。

宿主按现有 phase/artifact 顺序接收连续记录，独立核对同一窗口的完整条件。旧的单条结果格式继续有效，无需转换已归档记录。更新及 kernel 重启后要求 marker、创建版本保持一致，计数严格递增。窗口之间没有覆盖全部 Activity 创建，跨窗口不要求计数恰好增加 1；派生汇总 `unobserved_creations` 列出未采集的计数区间，原始行继续保留。快照之后新增或跨越终点的半行仍未纳入本轮，范围与已有有界读取约定一致。

本轮不新增固定等待，也不通过 force-stop 推定日志已写完。结束采样前停止应用曾作为候选，但其异步进程退出和日志完成边界尚未验证，当前保留已有观测窗口。后续如要求完整生命周期计数，需另行建立可验证的结束条件。

## 曾考虑的替代方案

- 只保留首条或最后一条成功记录。中间失败、错误返回值和数据变化会丢失，未采用。
- 为每轮结果增加 applications 数组，同时保留 application。两种表示需要持续保持一致；本次沿用每条应用记录一行的现有格式。
- 修改 fixture 的 Manifest 来屏蔽重建。该方式改变原签名 APK，也削弱 Android 生命周期覆盖，未采用。

## 影响

从实际失败日志提取两条成功 JSON 及对应上下文，回归接受这一窗口，保留来源路径、行号和摘要。宿主回归覆盖旧单条格式、新多条格式、更新和重启，拒绝重复、窗口内计数跳变、缺少阶段、不同 marker/创建版本、错误版本或返回值、首条创建标记错误及后续数据重建。跨窗口的合法计数间隔必须保留为未采集范围。编译生产 Java 助手与 DEX，继续运行现有真实文件追加边界检查。

完整 Android 安装更新及重启已由主会话合并后复验，结果见下文。

### 本机修复结果

生产助手改为保留快照内的全部已完成应用记录，逐条验证后按原格式输出。宿主使用相邻 phase/artifact 分组，要求组序列仍为原来的四组安装更新或两组重启；同一组后续再次出现、结束记录提前出现、重复计数和窗口内跳变均拒绝。每轮 `apk_fixture` 汇总增加派生的 `unobserved_creations`：校验成功后列出 `mode/after_stage/before_stage/first_launch_count/last_launch_count/count`，没有间隔为 `[]`，校验失败为 `null`。该字段只记录未采集的计数范围，未推断这些调用的原因或结果。旧报告的单条应用行可直接通过现有验证。

实际日志的八条原文及来源行号保存为 [apk-activity-recreation.json](../../../../tests/native-linux/fixtures/apk-activity-recreation.json)。两条 JNI 应用对象保持原样；宿主回放把该窗口放入构造的完整阶段上下文，`4a58459` 验证器拒绝，新验证器接受。构造上下文中的更新和重启记录只用于解析测试，原始整机失败仍保留。

```sh
python3 scripts/test_apk_fixture_result.py
python3 scripts/test_apk_log_reader.py
```

本机解析检查接受八组阶段输入，拒绝 62 个错误变体，并检查缺少首次阶段的重启失败。覆盖旧单条结果、实际 JNI 双记录、更新与重启多记录、组顺序、组重复、结束记录位置、计数重复/跳变/溢出、错误 marker/版本/返回值/创建标记，以及跨窗口未采集范围。现有真实文件并发追加、固定快照、UTF-8 与行长边界检查成功。镜像输入来源检查的六项回归成功，包含修改 APK 助手后拒绝旧镜像。

OpenJDK 17 使用 SDK35 `android.jar` 对生产助手执行 `javac -Xlint:all -Werror -source 8 -target 8` 成功，build-tools36 D8 编译成功，DEX 为 12,928 字节。四个现存 APK 的摘要、包名版本及签名证书重新核对成功；fixture 源码、构建脚本与 APK 字节均未修改。[结构化回归结果](../../../../experiments/native-linux/results-apk-recreation-regression-20261112.json)保存编译命令、来源与产物摘要、旧解析器拒绝及新解析器接受的回放、宿主检查输出。工作产物为 `work/offline-apk-recreation/work/native-linux-nce/apk-recreation-build/`。上述独立检查没有执行 Android 助手主循环，合并后的实际 Android 运行结果见下文。

### 主工作区完整 Android 回归

修复合入后，主工作区在 Apple M5 Ultra、macOS 27.0（26A428）重新构建并验证用户态镜像。原生 NCE 使用 demand 模式、关闭 nterp，CLI/GCD 分别完成 231/232 条检查，最终结果均为 passed；上下文分别 3002/3002、2900/2900 回收。iOS 核心交叉编译成功，图形集成与设备执行不在该检查范围。

Direct 使用单 CPU、host 布局、16 KiB、TBI 关闭、flat、dual、128 MiB 缓存、timer 抢占，开启 code-watch/cross-page/native-exclusive，nterp 开启。CLI 完成四份原始 APK 的首次安装与 v2 更新，GCD 重新启动 kernel 后验证已安装的两个 v2 应用；分别完成 63/62 条检查，上下文 1316/1316、1122/1122 回收。Java 与 JNI 的返回值依次为 101、102、102，marker 和 created_version=1 保留，launch_count 为 1、2、3。四份已安装 APK 的 SHA256 与原始输入逐份一致，两轮数据盘均干净卸载，第二轮输入摘要等于第一轮输出摘要。

本次实际运行没有再次出现窗口内双重创建，两个阶段的 unobserved_creations 均为空。合法重建的接受行为由原始失败日志回放和助手文件读取检查覆盖；本次完整运行覆盖修复后助手的实际安装、更新与重启流程。首次失败输出继续保留，成功结果没有覆盖失败。

[结构化集成结果](../../../../experiments/native-linux/results-apk-recreation-integration-20261112.json)保存配置、源码与二进制摘要、每条功能检查、APK 行及完整原始报告摘要。原始报告位于 `work/native-linux-nce/processes/lkl-summary.json` 与 `work/native-linux-nce/apk-recreation-integrated-desktop/summary.json`。测试使用项目重建运行库，原始 SDK36 运行库桌面由独立候选继续验证。本轮没有交互性能、完整 HyperOS 或 iPhone 运行结论。

### 复杂度审查与范围差异

按 `reclaim-code-entropy` 检查调用方：Java 的内部数组由逐条输出循环消费，Python 的分组由现有阶段验收消费；新增的未采集范围函数仅由探针汇总和对应回归消费。该范围从已验证的原始记录推导，不保存另一份计数状态或应用数据。去掉分组会恢复每次启动只允许一条记录的错误假设；只取末条会失去窗口内异常记录。现有结果字段、APK 输入身份和 PackageManager 安装接口保持一致。

最初任务要求检查计数连续性；主会话随后明确区分采样窗口。最终窗口内要求加 1，跨窗口要求严格递增并报告缺少的观测范围。固定日志快照保持现有边界，未增加固定等待或未验证的停止条件。

## 风险

固定快照覆盖有限，日志窗口之间可能发生合法 Activity 重建。跨窗口的严格递增只能验证观察到的数据持续前进，无法确定未采集调用的版本和返回值。fixture 首帧仍由独立验证处理。
