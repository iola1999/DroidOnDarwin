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

实际 probe 设置 `NCE_TRANSLATION_WORKLOAD=bionic` 并进入 `stock_init_run()`；阶段结果的 `source_configuration.workload=core` 来自复用的 BPF core 配置。CLI/GCD 编译命令均未启用 `NCE_GRAPHICS`，未链接宿主 graphics 源码与 Apple 图形框架；二进制也没有 `nce_graphics_start`、`nce_host_input` 和 `nce_session_set_syscall` 导出。一般 runner 中的图形调用位于 `NCE_GRAPHICS` 条件编译及 desktop 分支。此配置尚未提供原 SDK 图形设备所需的宿主实现。后续生成副本已取得 SurfaceFlinger 自身 FATAL，见下文。

### 停止后的 tombstone

主会话在 v13 结束并完成同盘 recovery 后，只读导出 `/data/tombstones`。命令为 `debugfs -R 'rdump /tombstones "<输出目录>"' <data-16k.img>`，未启用写入选项。导出前后 data 盘 SHA-256 均为 `352d4fbf236555d5808a45351663dfad87416f5492c59b11dde36aea7a33550f`，与 v13 最终结果相同。原盘与导出清单位于 `work/sdk-plain-validation-v1/surfaceflinger-tombstones-v1/manifest.json`。

导出的 50 份文本与 50 份 protobuf tombstone 共 16,551,175 bytes。50 份文本都记录 `media.swcodec`，没有 SurfaceFlinger 的文本 tombstone。优先检查的 `tombstone_18` 为 pid 2502/tid 2506，`tombstone_19` 为 pid 2574/tid 2578；二者均为 SIGABRT，Abort message 为 `GoldfishAddressSpaceHostMemoryAllocator failed to open`。对应 v13 GCD 原日志的服务启动/退出行分别为 107269/109727 和 109862/112319。

两份回溯依次经过 `GoldfishMapper::GoldfishMapper()`、`AIMapper_loadIMapper`、`android::Gralloc5Mapper`、`GraphicBufferMapper`、`GraphicBuffer`、`AHardwareBuffer_isSupported`，再进入 codec2 软件解码器接口构造。原 SDK fingerprint 为 `google/sdk_gphone16k_arm64/emu64a16k:16/BE2A.250530.026.F3/13894323:userdebug/dev-keys`，页大小为 16384 bytes。原 `mapper.ranchu.so` 的 SHA-256 为 `311ccf0653ab04154fa3c6acbfa99b81f0dc904f501228dee4dc8725e0a6b870`，与原 desktop manifest 相同；其 BuildId `5126b31f9f2b5a6acd2edf15be421276` 与回溯一致。这些记录确认 media.swcodec 的 mapper 初始化失败；本次只读导出未取得 SurfaceFlinger 的 Abort message 和回溯。结构化解析保存在同目录的 `analysis.json`。

## 提案

先保存 v13 原失败、停止后的 tombstone、来源摘要与服务重启顺序，再在新生成副本增加有界诊断。主会话已决定复用 v13 输入，以 APFS 克隆的独立 data/metadata 盘分别运行 CLI/GCD；v13 原盘保持现状。下一轮诊断继续沿用完整 boot 与回收验收条件。

### 采集原 liblog 的 FATAL 消息

现有 [write/writev 跟踪](../../../../experiments/native-linux/translation/stock-init-trace.h) 每个向量最多捕获 2048 bytes、每次最多读取八个向量。无加密 probe 启用 `NCE_STOCK_INIT_LOG_TEXT_ONLY`，只输出首字节为「<」的向量。常规 liblog 消息使用二进制 header、priority、tag 和 message 四个向量，因此原 SurfaceFlinger 内部消息未由这条文本筛选输出。

保留的 `platform/system/logging` 源码修订为 `d78b713380007d3c0dde14712cbcbec27f491ad9`，来源校验见 [Android 源码核对结果](../../../../experiments/native-linux/android-source-verification-2026-11-12.json)。本次复核的 `logd_writer.cpp`、`logger_write.cpp` 和 `private/android_logger.h` 与该修订的归档文件逐字节相同。`logd_writer.cpp` 连接 `/dev/socket/logdw`，在 `writev` 前添加 packed `android_log_header_t`；header 由 1-byte id、2-byte tid 和 8-byte realtime 组成。`logger_write.cpp` 追加 1-byte priority、NUL 结尾的 tag 和 NUL 结尾的 message。

v13 实际 `root/system/lib64/liblog.so` 与保留的原 SDK `android36/lib64/liblog.so` SHA-256 相同，为 `efcce90970ed85d61b9c05791ad76ff5f573035b1c0f7d2a3839063c7db5d1a0`，BuildId 为 `3ac8a0de13e46ddadf98d89d4e7cd24e`，也与 media.swcodec 回溯一致。实际 `__android_log_logd_logger_with_timestamp` 的 `0xa80c` 设置 header 长度 11、`0xa830` 设置 priority 长度 1、`0xa864` 设置向量数 4、`0xa880` 调用 `writev`；tag/message 长度分别由 `strlen+1` 构造。反汇编、命令、来源摘要和核对结果保存于 `work/sdk-plain-validation-v1/surfaceflinger-tombstones-v1/liblog-four-vector-aarch64.txt` 与 `liblog-writev-protocol.json`。

主会话采用的生成副本诊断只接受上述四向量布局，并严格校验 11-byte header、1-byte priority 与 tag/message 的终止和长度；只记录 FATAL(7)，每个入口最多输出 512 条。记录包含 PID/TID、fd、syscall 返回值、tag/message hex、原长度与捕获长度，结束后输出匹配总数和省略数。原「<」文本、first-stage、设备及系统观察器继续保留。当前计划只增加已有 writev 位置的诊断，不新增线程、常驻服务或日志盘格式。

### 后续接口调查

生成副本取得 FATAL 消息后，下一步按实际返回调查 composer3 的服务注册、`createClient`、Gralloc mapper 和 RenderEngine 初始化。media.swcodec 已确认的失败另按原 `GoldfishMapper` 与 `GoldfishAddressSpaceHostMemoryAllocator` 的 open/ioctl/mmap 调用核对设备契约。现有自建 gralloc 与原 SDK mapper 的实现和使用入口分别核对。

原 `logd` pid 72、`tombstoned` pid 104 已启动。CLI 行 15058 记录 `getsockopt`（209）fd 25、SOL_SOCKET=1、option=17 返回 -88（ENOTSOCK），下一行是 `logd: SocketListener: Failed to start unbound listener`。具体 listener、调用方与 SurfaceFlinger 日志缺失的关系仍待确认。现有 [系统服务观察器](../../../../experiments/native-linux/translation/stock-init-system-observer.rc) 由 `sys.boot_completed=1` 触发，采集 boot 与服务状态；v13 两入口均缺少该轮 boot-id 的完整导出。新增 guest 诊断服务暂缓，先使用生成副本的原 liblog 采集与停止后只读 tombstone 导出。

### 生成副本的 RenderEngine FATAL

`work/sdk-plain-validation-v1/surfaceflinger-log-v1/` 复用 v13 的已核对输入与独立 APFS 克隆盘，CLI/GCD 各观察 30 秒。每个入口捕获 181 条 FATAL，其中五条的 tag 为 `RenderEngine`，消息为 `no suitable EGLConfig found, giving up`，并带有 `format: 1`、`vendor: Android`、`version: 1.4 Android META-EGL`。原消息长度为 407 bytes，本次全部捕获，`write_result=432`。随后原 init 对相同 PID 记录 signal 6。CLI 的 PID/TID 分别为 250/326、376/386、446/456、516/526、585/595；GCD 分别为 255/331、383/391、458/465、529/535、600/606。原日志及源码摘要保留在该目录，完整消息与解析结果由结构化报告记录。

初次诊断的 `result.json` 保留 `status=failed`。新增 FATAL 行使用 `trace_index`，被 `_validate_syscall_trace` 的全局筛选纳入，导致原启动两入口 `validate=False`。`revalidation.json` 保留这个失败，在原日志中分别去除独立诊断行、仅将诊断字段改为 `log_index`，两种方式的完整启动观察 validator 均成功；实际原行与使用 `log_index` 的协议及限额 fixture 一起验证也成功。fixture 覆盖 21 项无效协议条件、2048-byte 捕获边界、16-bit header tid，以及 520 次匹配中记录 512 次、省略八次的计数。复核没有重新启动 Android，也没有修改生产 probe。

原启动两入口实际退出 0，宿主进程组为空；随后 CLI/GCD 的同盘 mounted-partition recovery 验证成功，两盘最终 clean。清理前检查盘使用方，删除本轮十项候选盘、程序与 dSYM，逻辑大小共 575,211,769 bytes；206 项生产源码、原盘与 v13 输入在运行、复核和清理后保持。清单为 `work/cleanup-2026-11-14/surfaceflinger-log-v1.json`，复现副本的源码和命令保存于 `revalidation/`。原启动及完整 boot 的失败继续保留；下一阶段的图形适配见[原 SDK 图形服务与宿主传输](../architecture/2026-11-14-原SDK图形服务与宿主传输.md)。

### 实际 renderer 接入后的原 init 诊断

本轮使用新 `sdk-plain-graphics-core16k-v2`、vendor 图形候选 v3、独立 APFS data/metadata 克隆盘、gfxstream/ANGLE Metal 和有界 liblog 模块，CLI/GCD 的 original 各观察 30 秒。六个 lifecycle/original/recovery 入口均实际退出 0，观察 validator 成功，宿主进程组回收成功；first-stage 完成，两盘最终 clean。实际图形候选和报告在运行后重新读回校验，原盘和外层冻结输入保持。完整系统观察器继续失败，结果见[图形原 init 诊断](../../../../experiments/native-linux/results-sdk-graphics-init-20261114.json)。

每个入口的 renderer 均报告 `opened=closed=0`。CLI original 捕获 19 条 FATAL，其中四条来自本轮 SurfaceFlinger PID；GCD original 捕获 20 条 FATAL，其中五条来自 SurfaceFlinger PID。两入口仍出现完整的 `no suitable EGLConfig found, giving up` 消息。原日志同时记录 `dlkm_loader` 加载 `/vendor/lib/modules/goldfish_pipe.ko`，`insmod` 返回 `Function not implemented`。该轮尚未提供 `/dev/goldfish_pipe_dprctd` 字符设备，原 EGL 的实际传输路径继续由[图形提案](../architecture/2026-11-14-原SDK图形服务与宿主传输.md#原-egl-的设备路径)推进。

v1 诊断结束后，核对宿主使用方并与只读协作者确认依赖，清理 13 项展开 root、系统中间镜像、宿主程序、对象、dSYM 和本轮 data/metadata 克隆盘，文件逻辑大小共 5,017,979,897 bytes。原日志、summary、生成 runner 源码与 data-export 保留；v13 原系统盘、固定 data/metadata 原盘及新 v4 候选在清理前后摘要保持。清单为 `work/cleanup-2026-11-14/init-graphics-v1.json`，已在结构化结果中保存其摘要。复现 v1 需要使用原 wrapper 命令重新生成上述中间产物。

### 字符设备与原 qemupipe 的原 init 诊断

v2 使用包含字符设备的 core v3、保留原 qemupipe 的 vendor v4 与新的独立 APFS data/metadata 克隆盘，CLI/GCD 的 original 各观察 30 秒。六个 lifecycle/original/recovery 入口的观察 validator、实际退出 0 和进程组回收均成功，first-stage 完成，两盘经 recovery 后 clean；333 项来源与实际候选在运行前后复核成功。完整系统观察器仍未取得本轮 boot-id 的完整导出，`full_boot_validation=failed`。

两份 original 日志均记录 `opened=closed=4`，每份捕获七条 FATAL，未出现 `RenderEngine` FATAL。两份日志均只有一次 SurfaceFlinger 启动，CLI 的 PID 为 241、GCD 为 254；Zygote 的 PID 均为 192。30 秒观察中未记录这两个服务退出。原 `no suitable EGLConfig found, giving up` 的本轮计数为 0；上述结果只确认已测图形连接和退出状态，Composer 注册、RenderEngine 完成、显示及完整系统仍待观察。

每份日志的其余 FATAL 为四条 `crash_dump64` 的 `failed to get child pid via PTRACE_GETEVENTMSG: Invalid argument` 与三条 sensors HAL 的 `sensors transport is not opened`。原 logd 的 `SocketListener: Failed to start unbound listener` 继续出现。原日志保存在 `work/native-linux-nce/sdk-plain-init-graphics-16k-v2/`，外层结果为 `work/sdk-plain-validation-v1/init-graphics-v2/result.json`；失败原因继续分别调查。该诊断的约 111 秒外层耗时包含准备、六个入口与读回，不用于整机性能比较。

v3 保持同一 core、vendor、宿主 renderer 与策略，使用新的独立克隆盘，将 original 观察扩大到 180 秒。六个入口的观察、实际退出 0 和进程组回收均成功，两盘最终 clean；333 项来源在运行后复核一致，实际图形候选读回成功。两份系统观察器导出均缺少本轮 boot-id 目录，`full_boot_observed=false`、`full_boot_validation=failed`。外层结果保存于 `work/sdk-plain-validation-v1/init-graphics-v3/result.json`，摘要与原日志保存于 `work/native-linux-nce/sdk-plain-init-graphics-16k-v3/`。

两入口均报告 `opened=closed=16`，`no suitable EGLConfig found, giving up` 的 FATAL 计数继续为 0。CLI 记录 40 条 FATAL，分别为 21 条 `crash_dump64`、17 条 sensors HAL 和两条 `HwcComposer`；GCD 记录 41 条，分别为 21、18 和两条。两条新的 HwcComposer 消息均为 `Can't create AidlComposerClient`。完整消息已捕获，结束计数的 `omitted=0`。对应原日志行号如下：

| 入口 | HwcComposer FATAL 行号 | SurfaceFlinger PID/TID | 同 PID 的 signal 6 行号 |
| --- | --- | --- | --- |
| CLI | 30984 | 252/252 | 31153 |
| CLI | 35709 | 436/436 | 35900 |
| GCD | 30798 | 251/251 | 30964 |
| GCD | 35749 | 431/431 | 35925 |

每个入口共记录三次 SurfaceFlinger 启动和两次 SIGABRT，原 rc 继续重启 Zygote/netd。CLI 的第三个 SurfaceFlinger PID 为 556，GCD 为 553，观察结束前未记录这两个 PID 的退出。原 Composer 服务在 CLI 为 PID 215、GCD 为 216；两份日志均记录其查询到 device VINTF 声明，尚未取得客户端初始化成功的结果。30 秒观察的服务状态只覆盖该区间，180 秒结果暴露了后续失败。

下一轮按原 SDK ELF 和固定源码核对 `createClient` 的失败分支，补充有界的 Composer 初始化消息与实际设备调用采集。原 logd、sensors 和 crash_dump64 的失败继续分别保存；原策略、完整 boot、图形显示、通用 APK 与持续性能的验收条件保持。

原 Composer ELF 的 BuildId 为 `21d937e07bcf91617caebca8c200661d`，与固定 SDK r07 清单的文件摘要相同。该 ELF 包含 `RanchuHwc` tag，以及 `failed as composer client already exists`、`failed to init composer client`、`failed to initialize DrmClient` 消息。下一份生成副本仅额外采集该 tag 的 WARN/ERROR/FATAL（priority 5/6/7），每条最多捕获 2048 bytes、每个入口最多记录 128 条，并输出匹配、省略计数。现有 FATAL 采集保持；新增采集先验证 tag、priority、协议边界、输入保持与记录限额，再运行新的独立克隆盘。原 rc、策略和原图形 ELF 保持。

参考 `goldfish-opengl` 的固定修订为 `ad6692caa5eeda26fb0dde1a6ca44494f07712d5`，其 `createClient` 对已有客户端最多等待五秒，初始化失败返回原 error；该参考源码与 SDK r07 编译源码的对应尚未确认。原属性与实际分支继续核对，具体失败原因由新增日志确定。只读调查来源保存在 `work/sdk-plain-validation-v1/composer-client-review-v1/`。

### Composer 缓冲区分配诊断

`composer-taglog-v1` 已在生成副本完成两个 original 入口各 180 秒的观察。六个 lifecycle/original/recovery 入口的观察、实际退出 0 与进程组回收均成功，first-stage 完成，独立 data/metadata 克隆经 recovery 后 clean。生产来源、生成副本和实际 vendor 候选前后核对成功，停止后再次逐项核对 334 项来源。每个 original 入口的图形连接均为 `opened=closed=16`，各捕获 16 条 `RanchuHwc` ERROR，记录省略数为 0。现有 FATAL 采集继续通过协议复核，完整系统观察器仍缺少本轮 boot-id 的完整导出，`full_boot_validation=failed`。外层结果为 `work/sdk-plain-validation-v1/composer-taglog-v1/result.json`，原日志及摘要为 `work/native-linux-nce/sdk-plain-init-composer-taglog-16k-v1/`；入仓结果保存了本轮摘要与来源复核引用。

两入口两次客户端初始化均记录同一错误顺序：`create: Failed to allocate drm ahb`、`createHostComposerDisplayInfo: display:0 failed to allocate swapchain`、`onDisplayCreate failed to initialize host info for display:0`、`createDisplayLocked failed to register display:0 with composer`、`createDisplaysLocked failed to create display from config`、`init failed to create displays.`。首次 DRM 分配失败位于 CLI 原日志第 31039 行、GCD 第 30983 行；再次失败位于第 35806 行与第 35610 行。Composer 的 PID 均为 214，CLI 的 TID 为 254，GCD 为 260。空 vsync 属性使用默认 60 Hz 与设置 Vsync 线程优先级返回 `Invalid argument` 也已记录，这两项与分配失败的关系尚未确认。

该轮 fixture 已验证 `RanchuHwc` 的 priority 5/6/7、其他 tag 与 priority 的拒绝、错误 header、2049-byte 消息的 2048-byte 捕获及输入保持。134 次匹配记录 128 次、省略六次，结束计数一致。原 FATAL 模块、原 rc、策略和图形 ELF 保持；这些诊断结果只用于定位启动故障。

原 Composer ELF 的 Host 分支固定使用 format 1、layerCount 1、usage `0xa00` 和三张 swapchain 图像，宽高读取活动 display config；原 libui 在 `GraphicBufferAllocator` 的 priority 6 日志中输出实际宽高、layerCount、format、usage 与 `status_t`。v4 memfd allocator 支持上述固定 format、layerCount 与 usage，分配失败日志的 tag 为 `allocator-service.nce`、priority 为 6，包含函数及源码行号。实际宽高、返回码及本轮 allocator 失败分支尚未取得。

下一份独立生成副本保留 `RanchuHwc` 的 priority 5/6/7，再采集 `GraphicBufferAllocator` 与 `allocator-service.nce` 的 priority 6，三个 tag 共用每入口 128 条和每条 2048 bytes 的限额。先验证允许与拒绝条件、协议、输入保持和共享记录限额，再使用新克隆盘运行相同六入口、180 秒 original 和完整 boot 验收。按实际返回定位 allocator 内存、宿主缓冲区创建或 mapper 导入错误后，再决定实现改动。

该轮结束后核对实际使用方和后续诊断的依赖，清理 15 项展开 root、系统中间镜像、程序、对象、dSYM、data/metadata 克隆及 fixture 二进制，共 5,025,412,539 bytes 的文件逻辑大小，物理释放量未测。原日志、summary、生成 TU/头、导出、原盘、vendor v4 和 core v3 保持；清单为 `work/cleanup-2026-11-14/composer-taglog-v1.json`。334 项来源复核发生于清理前，复现需由该轮 wrapper 重新生成中间产物。

v2/v3 结束后，原 liblog fixture 与四份原日志的协议及 syscall 采样复核成功。核对全部入口停止状态和只读调查的实际依赖后，清理两轮 26 项展开 root、系统中间副本、程序、对象、dSYM 和 data/metadata 克隆盘，以及本轮 liblog fixture 二进制，共 27 项、10,036,009,642 bytes 的文件逻辑大小。未测物理释放量。原日志、summary、生成 TU/头、导出结果、固定原盘、vendor v4 和 core v3 来源保持；清单为 `work/cleanup-2026-11-14/init-graphics-v2-v3.json`。复现这两轮需使用相应 wrapper 重新生成中间产物；iOS 输入前后核对发生于清理前。

### allocate2 的 Binder 交易诊断

三标签诊断 `work/sdk-plain-validation-v1/composer-allocator-taglog-v1/result.json` 已完成。CLI/GCD 的 lifecycle、original 和 recovery 六入口均实际退出 0，进程组清空，未发送额外清理信号；同盘 recovery 结束后 data/metadata 均为 clean。生产与生成来源前后保持。两份 original 各记录 18 条，省略 0 条，均两次报告 `Failed to allocate (360 x 640) layerCount 1 format 1 usage a00: -129`。候选 allocator 的错误标签未出现，完整 boot 继续为 failed。

原 vendor `libui.so` 在 Gralloc5Allocator 的 allocate2 失败分支读取 `AStatus_getExceptionCode`。`EX_SERVICE_SPECIFIC=-8` 会转换服务错误，其他异常保留原值；NDK 的 `EX_TRANSACTION_FAILED=-129` 与本轮返回相同。mapper import 发生在 AStatus 成功之后。该三标签轮次尚未读取实际 Binder 错误结构。

原 Composer 在两入口分配错误前都调用 `BINDER_GET_EXTENDED_ERROR`，ioctl request 为 `0xc00c6211`，实际返回 0。该三标签轮次只保存用户结构的地址。实际内核 ABI 为 12 bytes 的 `{u32 id, u32 command, s32 param}`；驱动在该 ioctl 内复制并重置线程的 extended error。因此下一轮只在原 ioctl 成功后，用已有只读 `copy_user` 读取返回结构；不新增查询 ioctl。新生成副本继续保留三标签、FATAL、原 rc、策略、SDK 图形 ELF、180 秒 original 与六入口验收。每入口最多记录 64 份结构，同时汇总复制失败、身份获取失败及省略计数。先检查请求筛选、边界、输入保持、signed param 和记录限额，再运行新 APFS 克隆盘。

诊断来源与实际 vendor/core 在停止后逐项复核。取得实际 command/param 后按对应内核分支处理，并覆盖 CLI/GCD、资源回收与原策略行为。此诊断不改变完整系统、显示、APK 或持续性能的验收条件。

本轮 Binder 诊断 `work/sdk-plain-validation-v1/composer-binder-error-v1/result.json` 已结束。两份 original 各读到 2 份，复制失败、身份获取失败和省略均为 0；CLI 的交易 id 为 1113/1566，GCD 为 1295/1954，四份均为 `command=29201（BR_FAILED_REPLY）, param=-1（EPERM）`。六入口实际退出和恢复检查成功，生产、生成和工具来源前后保持，完整 boot 继续 failed。原系统停止后的内核日志读取均返回 `-13`，AVC 内容仍未取得。

该轮独立结束审阅核对 13 组、512 项摘要，其中内层来源为 335 项。CLI original 的图形连接为 `opened=closed=16`，GCD 为 `opened=closed=17`。确认六入口进程组均清空和后续诊断的实际使用方后，清理 14 项展开 root、中间镜像、程序、对象、dSYM 与克隆盘，文件逻辑大小为 5,025,379,547 bytes，物理释放量未测。原日志、summary、生成源码、导出、原盘、vendor v4 与 core v3 前后保持，清单为 `work/cleanup-2026-11-14/composer-binder-error-v1.json`。335 项内层来源的复核发生于清理前；复现需由该轮 wrapper 重新生成中间产物。

实际内核 `binder_transaction_log_failed` 已保存失败交易的 `debug_id`、调用/回复方向、双方进程/线程、数据大小、对象偏移表大小、`return_error_line`、command 和 param。当前 ELF 的该符号相对镜像基址地址为 `0x9f269c`，大小 `0x2708`；`CONFIG_DEBUG_INFO_NONE=y`，ELF 没有可用 DWARF 类型。下一步使用实际 Binder 编译命令的只读 layout 检查确认结构布局，再在新生成副本中按 extended error 的 id 读取对应失败记录。采集继续有界，检查记录完成标记，保留未找到与记录未完成计数，并核对整个读取范围位于实际内核镜像。该步骤保持内核、原策略与原 ioctl 的实际行为。

新增诊断只由生成 probe 和 C fixture 使用，分别保存实际复制失败、身份获取失败和达到记录限额的调用，未增加产品配置或新的公共查询接口。直接再发 GET 查询会读取已重置的线程错误；记录全部 Binder payload 尚无必要，先读取现有失败记录的步骤与行号。静态调查继续核对 `TF_ACCEPT_FDS`、memfd/refcount 的原标签和 Binder 文件传输权限。

实际 Binder 编译参数的只读 layout 检查已成功，未重建内核或启动 guest。`work/sdk-plain-validation-v1/binder-log-abi-review-v1/result.json` 核对八项来源前后保持：失败记录每槽 312 bytes、32 槽，数组从日志结构的 offset 8 开始，整个日志为 9992 bytes。`debug_id_done`、`return_error_line`、command 与 param 分别位于槽内 offset 4、44、48、52；日志范围 `[10430108, 10440100)` 位于当前 10485760-byte 内核镜像的 RW 段。

下一轮 `composer-binder-failed-v1` 保留原 extended error 读取及三标签诊断，额外扫描已有失败日志的 32 槽；按 id、command 与 signed param 匹配，并在复制固定 56 bytes 字段前后核对非零完成标记。未找到、未完成、字段不匹配和达到 64 条限额分别计数。扫描留在原 syscall 回调中，不增加 Linux 调用或等待；全部 guest 线程停止后输出一次汇总。日志的 from/to PID、TID 来自内核初始 PID 命名空间，原 ioctl 记录的 PID、TID 来自 guest 命名空间，两类编号均保留，验证不要求两类编号相同。目标 PID、TID 为零的记录保留，这些字段可能尚未写入。

生成前先检查未完成与变化的完成标记、首尾槽、command/param 不匹配、目标编号为零、两类编号不同、signed 字段边界、输入保持和记录限额；再复核实际内核来源与唯一基址设置位置，使用新 APFS 克隆盘完成相同六入口与 180 秒 original 观察。内核改变后必须按新 ELF 和实际编译参数重新确认布局与地址。具体 EPERM 步骤仍待此次动态记录，完整 boot 验收继续保持。

首次 prepare 的 Binder fixture 校验因新增失败汇总改变末尾位置而失败，已按事件种类筛选该 fixture 的输入。prepare v2 的独立审阅又发现，原 extended error 已达 64 条限额后仍可能输出失败交易，缺少对应的原记录；读取入口改为仅处理已输出的 extended error，原省略计数继续保留。prepare v3 的协议、字段关联与混合限额检查成功：前 64 次 `BR_OK` 后再出现一份匹配失败，extended error 记录 64 条、省略一条，失败交易没有单独输出。前三份 prepare 结果均保留；随后使用相同生成源码完成实际 guest 运行。


`composer-binder-failed-v1` 已完成六入口与两个 original 各 180 秒的观察。六入口实际退出均为 0，进程组均清空，未发送额外清理信号；同盘 recovery 后 data/metadata 均为 clean。两份 original 各记录 18 条 tag、2 份 extended error 和 2 份失败交易，复制、身份、槽匹配及省略失败均为 0，图形连接均为 `opened=closed=16`。336 项内层来源及生产、生成和工具来源保持，独立结束审阅的 99 项检查成功。first-stage 完成，完整 boot 仍为 failed。外层结果为 `work/sdk-plain-validation-v1/composer-binder-failed-v1/result.json`，原日志与 summary 为 `work/native-linux-nce/sdk-plain-init-composer-binder-failed-16k-v1/`；独立审阅为同轮 `independent-observed-review.json`。

四份失败均为 `call_type=2` 的回复，`data_size=168`、`offsets_size=16`，`return_error_line=3518`，command 为 `BR_FAILED_REPLY`，param 为 `-EPERM`。实际冻结的 `binder.c:3518` 对应 `BINDER_TYPE_FD` 的 `binder_translate_fd` 返回负值。原 GET 的 fd 为 4、request 为 `0xc00c6211`、返回 0；同线程随后均记录 360×640、layerCount 1、format 1、usage `0xa00` 的分配返回 `-129`。原 init 确认 allocator 的 guest PID 为 CLI 206、GCD 204，Composer 为 214、215。

| 入口 | 交易 ID | guest PID/TID | 内核 from→to | extended error / failed / 分配日志行 |
| --- | --- | --- | --- | --- |
| CLI | 1137 | 214/260 | 380:435→388:436 | 30935 / 30936 / 30938 |
| CLI | 1708 | 214/260 | 380:435→388:436 | 35953 / 35954 / 35956 |
| GCD | 1153 | 215/259 | 378:433→389:436 | 30959 / 30960 / 30962 |
| GCD | 1579 | 215/259 | 378:433→389:436 | 35740 / 35741 / 35743 |

失败记录保留 `target_handle=-1` 与 `to_node=0`。该记录没有 FD 对象序号、对象 offset 或原 FD 值，具体失败 FD 仍待采集。`binder_translate_fd` 的 EPERM 条件继续保留两项：目标不接受 FD，或 `security_binder_transfer_file` 失败后归一为 EPERM。原系统与 vendor 的 libbinder ELF 均设置 `TF_ACCEPT_FDS`，实际交易 flags 尚未读取。

固定源码与原 ELF 的有界反汇编确认，正常 `NativeHandle` 回复从数组索引 0 开始，先写入 memfd `bufferFd`，随后写入 goldfish 字符设备 refcount FD，来源为 `work/sdk-plain-validation-v1/composer-client-review-v1/allocator-followup/reply-fd-order-v2/report.json`。原 CIL 允许 Composer 使用 allocator 的 FD，并允许读写 qemu 字符设备；Composer 读写 `hal_graphics_allocator_default_tmpfs` 普通 file 的查询无匹配。实际 inode SID 尚未取得，当前无法确认具体 SELinux 条件。

下一轮只在已输出原 extended error 的成功回调中读取当前静态 printk 文本环，每入口最多四份、每份 131072 bytes，关联原交易 ID、guest PID/TID 和失败记录的内核编号。实际 ELF 的 `__log_buf`、`log_buf`、`log_buf_len`、`prb`、`printk_rb_static` 偏移分别为 `0x9b1300`、`0x716780`、`0x716788`、`0x716778`、`0x72e888`；读取前后核对活跃对象仍指向上述静态实例，同时读取 `binder_debug_mask` 的实际值。继续使用已有内核基址，不增加 Linux 查询、等待或内核重建。原字节包含记录 ID、填充、循环覆盖和旧内容；仅对可以核对的完整文本作结论，缺少文本仍保留上述两项 EPERM 条件。

本轮结束后核对宿主进程组、打开文件与保留来源，清理 15 项展开 root、中间镜像、程序、对象、dSYM、本轮 data/metadata 克隆盘和首次 prepare 遗留的 fixture 可执行文件，文件逻辑大小共 5,025,438,429 bytes，物理释放量未测。432 份保留文件摘要与 16 个符号链接目标前后保持；原盘、core v3、vendor v4、所有 prepare 失败与成功报告、生成源码、原日志、summary 和导出结果保留。清单为 `work/cleanup-2026-11-14/composer-binder-failed-v1.json`。336 项来源复核发生于清理前；复现需要按该轮 wrapper 重新生成中间产物。

静态 printk 调查来源为 `work/sdk-plain-validation-v1/composer-client-review-v1/allocator-followup/binder-fd-printk-static-review-v2.json`。22 项来源前后保持，27 段源码引用复核成功；ELF、bin、header 和 .config 与冻结 core/kernel manifest 匹配。Binder、printk 和 audit 源码保存本轮读取摘要，其原编译逐文件状态仍未核对。文本块含 8-byte 的记录 ID 前缀，有效文本长度另存 `printk_info.text_len`；去掉换行和级别前缀后，首个 NUL 可能位于残留字节之后。此次快照未采 descriptor/info，仅保存原字节与固定文本匹配，记录提交与有效文本边界待核对。v2 修正了最初报告的 ELF RW 范围说明，实际 Memsz 结束于 `0x9ffef9`；原报告及其摘要保持。

新增读取仅由 work 内生成 probe 和 C fixture 使用，原内核、宿主、策略、SDK ELF、三标签与 FATAL 的采集保持。先检查静态指针与长度、读取范围、输入保持、记录限额和原 extended error 关联，再用新 APFS 克隆盘运行相同六入口、180 秒 original 和完整 boot 验收。该步骤尚未修改共享缓冲区实现；DMA heap 的 PFNMAP、首次写、共享与释放仍待实际验证。


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

本次实际运行 `python3 -B work/sdk-plain-validation-v1/surfaceflinger-tombstones-v1/analyze.py`，11 项来源检查全部成功，包含导出文件、原盘导出摘要、v13 二进制、原 rc、图形库 BuildId 与固定 liblog 源码。此脚本只读取停止后的 JSONL、已导出的 tombstone 与保留的源码/二进制，没有打开 data/metadata 盘。FATAL fixture 与生成副本的原 init 诊断结果见下文；完整 boot 验收继续为 failed。

## 风险

FATAL 筛选可能遗漏退出前的非 FATAL 错误，已有原日志和结束计数继续保留；截断与省略必须由报告显式记录。packed header 的 tid 为 16-bit，需同时记录客体实际 PID/TID。四向量形状本身缺少 socket 目标确认，下一轮结果需按原二进制、tag、实际线程和 syscall 返回值交叉核对。当前导出文件只覆盖停止后保留的 tombstone，SurfaceFlinger 文本缺失原因仍待调查。诊断配置开启的启动时间与资源使用只作为本机排障结果。
