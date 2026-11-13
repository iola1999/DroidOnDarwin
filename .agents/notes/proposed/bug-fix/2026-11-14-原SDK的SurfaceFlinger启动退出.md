# 决策记录: 原 SDK 的 SurfaceFlinger 启动退出

状态: proposed

## 问题

2026-11-14 的 `sdk-plain-init-16k-v13` 使用原 SDK36、无加密 data/metadata、16 KiB 客体和宿主页、1 GiB 客体 RAM、128 MiB 翻译缓存、单 CPU 与 direct 执行。宿主为 Apple M5 Ultra、macOS 27.0（26A428），original 观察上限为 180 秒。CLI/GCD 的 lifecycle、original 和同盘 recovery 六个入口分别通过观察、实际退出 0 和进程组回收；两卷最终为 clean。`first_stage_completed=true`，`full_boot_observed=false`，`full_boot_validation="failed"`。这些本机结果保留完整 boot 失败，iPhone 运行和整机性能继续待验证。最终结构化结果见 [v13 阶段结果](../../../../experiments/native-linux/results-sdk-plain-bpf-takeover-20261114.json)。

CLI 原日志 `work/native-linux-nce/sdk-plain-init-16k-v13/cli-original.jsonl` 有 114,716 条完整 JSON、19,137,826 bytes。实际顺序如下，行号均指这份原日志：

| 行号 | 原服务状态 |
| --- | --- |
| 20118 | `NetBpfLoad: success.` |
| 20119 | `bpfloader` pid 189 退出 0，原 init 记录等待 0.946000 秒 |
| 27257 | `surfaceflinger` pid 249 收到 signal 6（SIGABRT） |
| 27263 | init 向 `zygote` pid 193 的进程组发送 signal 9 |
| 27373 | init 向 `netd` pid 190 的进程组发送 signal 9 |
| 27423 | `netd` pid 190 收到 signal 9 |

原 `/system/etc/init/surfaceflinger.rc` 的 `onrestart restart --only-if-running zygote`、`init.zygote64.rc` 的 `onrestart restart netd` 和 `netd.rc` 的 `onrestart restart zygote`、`restart zygote_secondary` 与上述顺序相符。CLI 记录 35 次 SurfaceFlinger SIGABRT，以及各 35 次 Zygote/netd signal 9；末次 SurfaceFlinger 为 pid 2719、行 113897。GCD 同样记录 35 次 SurfaceFlinger SIGABRT，首次 pid 248、行 27315，末次 pid 2683、行 113532。原 rc 的 SHA-256 分别为 `c6f8279f4fff66ff9add26a521a01dbd563a38576c0f85e2c7d88de78e2c167d`、`7be8fde03ec2cc4417aecf1a79163e0e4088fdff02ee6cd2426d02f91127e479` 和 `f3649dbb95b5e57b7543213a07079d1e900d5cdbcf498f28334a2981a940c96e`，已与 v13 输入核对。

原 NetBpfLoad v0.47 已进入网络对象加载，35 条网络 `BPF_PROG_LOAD` 返回正 fd，随后整体 `bpfloader` 退出 0。可选 uprobe、gpuWork/gpuMem/memevents 仍有失败记录。上述加载结果只覆盖实际成功记录和整体退出状态。

### 原图形声明与 probe 配置

原 SDK 的 `vendor/etc/init/hwc3.rc` 启动 `vendor.hwcomposer-3`，可执行文件为 `/vendor/bin/hw/android.hardware.graphics.composer3-service.ranchu`，其 `onrestart` 重启 SurfaceFlinger。`vendor/etc/vintf/manifest/hwc3.xml` 声明 AIDL `android.hardware.graphics.composer3` version 4、`IComposer/default`。CLI 行 22121 开始启动该服务，随后 pid 215 在行 24530 查询并找到对应 device VINTF 声明。该日志尚未确认服务注册、`createClient`、显示设备初始化或 RenderEngine 初始化；日志中未找到该 composer 服务的退出记录。

实际 probe 设置 `NCE_TRANSLATION_WORKLOAD=bionic` 并进入 `stock_init_run()`；阶段结果的 `source_configuration.workload=core` 来自复用的 BPF core 配置。CLI/GCD 编译命令均未启用 `NCE_GRAPHICS`，未链接宿主 graphics 源码与 Apple 图形框架；二进制也没有 `nce_graphics_start`、`nce_host_input` 和 `nce_session_set_syscall` 导出。一般 runner 中的图形调用位于 `NCE_GRAPHICS` 条件编译及 desktop 分支。此配置尚未提供原 SDK 图形设备所需的宿主实现，SurfaceFlinger 本次 SIGABRT 的内部触发点仍缺少自身诊断。

### 停止后的 tombstone

主会话在 v13 结束并完成同盘 recovery 后，只读导出 `/data/tombstones`。命令为 `debugfs -R 'rdump /tombstones "<输出目录>"' <data-16k.img>`，未启用写入选项。导出前后 data 盘 SHA-256 均为 `352d4fbf236555d5808a45351663dfad87416f5492c59b11dde36aea7a33550f`，与 v13 最终结果相同。原盘与导出清单位于 `work/sdk-plain-validation-v1/surfaceflinger-tombstones-v1/manifest.json`。

导出的 50 份文本与 50 份 protobuf tombstone 共 16,551,175 bytes。50 份文本都记录 `media.swcodec`，没有 SurfaceFlinger 的文本 tombstone。优先检查的 `tombstone_18` 为 pid 2502/tid 2506，`tombstone_19` 为 pid 2574/tid 2578；二者均为 SIGABRT，Abort message 为 `GoldfishAddressSpaceHostMemoryAllocator failed to open`。对应 v13 GCD 原日志的服务启动/退出行分别为 107269/109727 和 109862/112319。

两份回溯依次经过 `GoldfishMapper::GoldfishMapper()`、`AIMapper_loadIMapper`、`android::Gralloc5Mapper`、`GraphicBufferMapper`、`GraphicBuffer`、`AHardwareBuffer_isSupported`，再进入 codec2 软件解码器接口构造。原 SDK fingerprint 为 `google/sdk_gphone16k_arm64/emu64a16k:16/BE2A.250530.026.F3/13894323:userdebug/dev-keys`，页大小为 16384 bytes。原 `mapper.ranchu.so` 的 SHA-256 为 `311ccf0653ab04154fa3c6acbfa99b81f0dc904f501228dee4dc8725e0a6b870`，与原 desktop manifest 相同；其 BuildId `5126b31f9f2b5a6acd2edf15be421276` 与回溯一致。这些记录确认 media.swcodec 的 mapper 初始化失败；SurfaceFlinger 的 Abort message 和回溯仍未取得。结构化解析保存在同目录的 `analysis.json`。

## 提案

先保存 v13 原失败、停止后的 tombstone、来源摘要与服务重启顺序，再在新生成副本增加有界诊断。主会话已决定复用 v13 输入，以 APFS 克隆的独立 data/metadata 盘分别运行 CLI/GCD；v13 原盘保持现状。下一轮诊断继续沿用完整 boot 与回收验收条件。

### 采集原 liblog 的 FATAL 消息

现有 [write/writev 跟踪](../../../../experiments/native-linux/translation/stock-init-trace.h) 每个向量最多捕获 2048 bytes、每次最多读取八个向量。无加密 probe 启用 `NCE_STOCK_INIT_LOG_TEXT_ONLY`，只输出首字节为「<」的向量。常规 liblog 消息使用二进制 header、priority、tag 和 message 四个向量，因此原 SurfaceFlinger 内部消息未由这条文本筛选输出。

保留的 `platform/system/logging` 源码修订为 `d78b713380007d3c0dde14712cbcbec27f491ad9`，来源校验见 [Android 源码核对结果](../../../../experiments/native-linux/android-source-verification-2026-11-12.json)。本次复核的 `logd_writer.cpp`、`logger_write.cpp` 和 `private/android_logger.h` 与该修订的归档文件逐字节相同。`logd_writer.cpp` 连接 `/dev/socket/logdw`，在 `writev` 前添加 packed `android_log_header_t`；header 由 1-byte id、2-byte tid 和 8-byte realtime 组成。`logger_write.cpp` 追加 1-byte priority、NUL 结尾的 tag 和 NUL 结尾的 message。

v13 实际 `root/system/lib64/liblog.so` 与保留的原 SDK `android36/lib64/liblog.so` SHA-256 相同，为 `efcce90970ed85d61b9c05791ad76ff5f573035b1c0f7d2a3839063c7db5d1a0`，BuildId 为 `3ac8a0de13e46ddadf98d89d4e7cd24e`，也与 media.swcodec 回溯一致。实际 `__android_log_logd_logger_with_timestamp` 的 `0xa80c` 设置 header 长度 11、`0xa830` 设置 priority 长度 1、`0xa864` 设置向量数 4、`0xa880` 调用 `writev`；tag/message 长度分别由 `strlen+1` 构造。反汇编、命令、来源摘要和核对结果保存于 `work/sdk-plain-validation-v1/surfaceflinger-tombstones-v1/liblog-four-vector-aarch64.txt` 与 `liblog-writev-protocol.json`。

主会话采用的生成副本诊断只接受上述四向量布局，并严格校验 11-byte header、1-byte priority 与 tag/message 的终止和长度；只记录 FATAL(7)，每个入口最多输出 512 条。记录包含 PID/TID、fd、syscall 返回值、tag/message hex、原长度与捕获长度，结束后输出匹配总数和省略数。原「<」文本、first-stage、设备及系统观察器继续保留。当前计划只增加已有 writev 位置的诊断，不新增线程、常驻服务或日志盘格式。

### 后续接口调查

SurfaceFlinger 下一步需要自身 FATAL 消息或 tombstone，随后按实际返回调查 composer3 的服务注册、`createClient`、Gralloc mapper 和 RenderEngine 初始化。media.swcodec 已确认的失败另按原 `GoldfishMapper` 与 `GoldfishAddressSpaceHostMemoryAllocator` 的 open/ioctl/mmap 调用核对设备契约。现有自建 gralloc 与原 SDK mapper 的实现和使用入口分别核对。

原 `logd` pid 72、`tombstoned` pid 104 已启动。CLI 行 15058 记录 `getsockopt`（209）fd 25、SOL_SOCKET=1、option=17 返回 -88（ENOTSOCK），下一行是 `logd: SocketListener: Failed to start unbound listener`。具体 listener、调用方与 SurfaceFlinger 日志缺失的关系仍待确认。现有 [系统服务观察器](../../../../experiments/native-linux/translation/stock-init-system-observer.rc) 由 `sys.boot_completed=1` 触发，采集 boot 与服务状态；v13 两入口均缺少该轮 boot-id 的完整导出。新增 guest 诊断服务暂缓，先使用生成副本的原 liblog 采集与停止后只读 tombstone 导出。

## 曾考虑的替代方案

- 先把 netd 重启列为独立根因：CLI 首次 SurfaceFlinger SIGABRT 早于 Zygote/netd 的 signal 9，原 rc 明确规定重启关系；先调查 SurfaceFlinger 自身退出。
- 直接用 media.swcodec tombstone 解释 SurfaceFlinger SIGABRT：已导出的文本进程名、PID 和回溯只确认 media.swcodec；等待 SurfaceFlinger 自身诊断。
- 立即部署独立 guest 日志服务：现有 writev 位置可采集有界 FATAL 消息，主会话先选择生成副本诊断，减少本轮启动配置变化。
- 输出全部 writev 向量：此前原 ART 编译产物已触发日志阈值；本轮限定协议布局、FATAL 与记录上限，并保存结束计数。

## 验收标准

1. 原 v13 的完整 boot 失败、原日志、盘恢复、输入摘要和只读导出清单保持对应；导出前后原盘摘要一致，文本 tombstone 的进程名、PID、fingerprint 与二进制 BuildId 可核对。
2. 原 `liblog.so` 的四向量布局与固定源码核对；实际 C fixture 检查合法 FATAL、其他 priority、向量数、错误 header 长度、未终止文本、复制失败、捕获边界和超过 512 条的计数。日志输出继续在已有阈值内。
3. 新 CLI/GCD 使用独立克隆盘，取得 SurfaceFlinger 自身 FATAL/tombstone 或明确记录仍缺少的诊断；composer 注册、客户端初始化和设备调用按实际日志分别确认。退出、进程组回收和同盘 recovery 继续全部检查。
4. 完整 Android 启动仍要求本轮 `sys.boot_completed=1` 与系统观察器结果通过。图形显示、通用原始签名 APK、持续交互与热状态后续分别验证，iPhone 运行仍待真机验证。

本次实际运行 `python3 -B work/sdk-plain-validation-v1/surfaceflinger-tombstones-v1/analyze.py`，11 项来源检查全部成功，包含导出文件、原盘导出摘要、v13 二进制、原 rc、图形库 BuildId 与固定 liblog 源码。此脚本只读取停止后的 JSONL、已导出的 tombstone 与保留的源码/二进制，没有打开 data/metadata 盘。FATAL fixture、新原 init 诊断与完整 boot 验收尚未运行。

## 风险

FATAL 筛选可能遗漏退出前的非 FATAL 错误，已有原日志和结束计数继续保留；截断与省略必须由报告显式记录。packed header 的 tid 为 16-bit，需同时记录客体实际 PID/TID。四向量形状本身缺少 socket 目标确认，下一轮结果需按原二进制、tag、实际线程和 syscall 返回值交叉核对。当前导出文件只覆盖停止后保留的 tombstone，SurfaceFlinger 文本缺失原因仍待调查。诊断配置开启的启动时间与资源使用只作为本机排障结果。
