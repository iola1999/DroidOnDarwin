# 决策记录: 原 SDK 的 Zygote 反复退出

状态: proposed

## 问题

`sdk-plain-init-system-dmaheap-16k-v1` 使用正式 PID1 bootstrap 回收的候选内核和 system DMA heap allocator/mapper，完成 lifecycle、original、同盘 recovery 的 CLI/GCD 六入口。两个 original 各观察 180 秒，原观察与退出检查通过；两个 boot_id 的数据盘中均缺少完整系统服务观察结果，完整启动验收失败。结果见[本轮报告](../../../../../../../sdk-plain-validation-v1/system-dmaheap-original-init-v1/run/report.json)。

两个 original 的 init 输出各记录六次 Zygote `received signal 9`，随后重新启动。SurfaceFlinger 各有一条启动成功记录，观察期内没有其退出记录。该轮三标签采集和 liblog FATAL 采集没有取得 Zygote 的退出原因、SIGKILL 发送者或 system_server 的失败消息，不能据此确认具体根因。原 logd 的 audit EPERM 与析构采集继续由[审计初始化记录](2026-11-14-原SDK的logd审计初始化.md)跟进。

### 原日志与 SDK 控制流

[原日志调查](../../../../../../../sdk-plain-validation-v1/system-server-taglog-v1/original-log-review.json)保留两个 original 的逐行位置、启动 PID、退出通知和后续 process-group 清理。CLI 的六个退出 PID 为 `194、519、629、743、853、963`，GCD 为 `192、520、632、742、863、973`。每次 `Sending signal 9 ... process group` 均在 `received signal 9` 之后。原 syscall 采样只保存失败调用、成功 read 和成功 syscall 142；成功的 `kill/tkill/tgkill` 没有记录，退出通知也缺少信号发送者身份。上述 process-group 清理日志只能确认退出后的处理顺序。

[SDK 静态调查](../../../../../../../sdk-plain-validation-v1/system-server-taglog-v1/sdk-control-flow-review.json)核对了本轮 `android36-runtime.json`，所选 `libandroid_runtime.so`、`app_process64`、`framework.jar`、`services.jar` 和 `init.zygote64.rc` 均与固定 SDK36 r07 清单匹配。原 `libandroid_runtime.so` 在库内 `0x255728` 比较刚回收的子进程与 `gSystemServerPid`，`0x255744` 写出 `Exit zygote because system server (pid %d) has terminated`，随后 `0x255748` 调用 `getpid`、`0x25574c` 设置信号 9、`0x255750` 调用 `kill`。[固定参考源码](../../../../../../../sdk-plain-validation-v1/system-server-taglog-v1/upstream/manifest.json)的 `SigChldHandler` 对应这段控制流，也包含 USAP eventfd 写入失败后结束 Zygote 的分支。v1 六次实际退出所进入的分支和 system_server 的退出原因未取得；完整源码修订与 SDK36 构建尚未对应。

### 原 init v2 的 ashmem 失败

后续 `sdk-plain-init-system-dmaheap-16k-v2` 使用冻结七标签采集，CLI/GCD 的 lifecycle、original、同盘 recovery 六入口已经结束。[运行报告](../../../../../../../sdk-plain-validation-v1/system-dmaheap-original-init-v2/run/report.json)保存总耗时 `417.248` 秒、六入口顺序、观察、实际退出、清理、采集结构与来源保持成功；完整启动仍为 `failed`。两个 original 均取得首个 system_server 的同一 Java 异常，逐行记录见[最终原日志审阅](../../../../../../../sdk-plain-validation-v1/system-server-ashmem-review-v1/original-log-review-v2.json)。

| 入口 | Zygote PID | system_server PID/TID | system_server 创建行 | 邻近 ashmem open 行 | Java 异常行 |
| --- | --- | --- | --- | --- | --- |
| CLI | `194` | `491/491` | `36425` | `37468` | `37478、37479、37497` |
| GCD | `192` | `482/482` | `36272` | `37449` | `37460、37461、37473` |

异常原文为 `java.lang.RuntimeException: Failed to create ashmem: No such file or directory`，堆栈经过 `ApplicationSharedMemory.nativeCreate`、`ApplicationSharedMemory.create(ApplicationSharedMemory.java:92)` 和 `SystemServer.run(SystemServer.java:980)`。三份含该异常的消息均完整保存，每份 `621–625` bytes。随后 `RuntimeInit$KillApplicationHandler` 记录 crash-reporting 的 `NullPointerException`；固定原 DEX 与参考源码的该处理函数在 `finally` 调用 `Process.killProcess(Process.myPid())` 和 `System.exit(10)`。成功信号调用仍未采集，原日志未直接保存 system_server 的终止状态或 SIGKILL 发送者。

CLI 记录 30 次 `/dev/ashmem1a7b236c-c9b5-49aa-ac6d-d947388ed18d` 的 syscall `56` 打开失败，GCD 记录 30 次 `/dev/ashmem7233c54a-b3a8-439d-b3cb-d081136586be` 打开失败，结果均为 `-2`。两个后缀与各自实际 boot_id 相同。`stock_init_security` 保存了原路径字节和返回值，缺少 PID/TID；表中 open 与 Java 异常按邻近顺序关联，单次失败 open 的调用进程尚未直接取得。

两个入口继续各记录六次 Zygote signal 9。CLI PID 为 `194、520、632、744、858、968`，GCD 为 `192、520、631、740、853、965`；每次 process-group 清理均在退出通知之后。SurfaceFlinger 分别以 PID `253、252` 启动一次，观察期内没有其退出记录。七标签每入口均录满 `128` 条，其中 `Zygote=123、AndroidRuntime=3、RanchuHwc=2`；CLI `seen=472、omitted=344`，GCD `seen=531、omitted=403`。后续各代 system_server 的 Java 原因受该共享预算限制，尚未取得。Zygote 终止日志也未取得，首代退出发生在采集预算用完之前；其实际日志协议和调用路径仍需核对。

[ashmem 静态审阅](../../../../../../../sdk-plain-validation-v1/system-server-ashmem-review-v1/static-review.json)核对原 `libandroid_runtime.so` 与 `libcutils.so` 的清单摘要、Build ID、四项 JNI 方法表及六条实际 LLVM 命令。原 JNI 表 `0x2bdf40` 的 `nativeCreate()I` 指向 `0x243cfc`；`0x243d14` 设置 `11304` bytes，`0x243d18` 调用 `ashmem_create_region("ApplicationSharedMemory", 11304)`。负返回分支读取 `errno`，使用上述固定异常文本抛出 `RuntimeException`。原 DEX 的 `SystemServer.run` 也调用 `ApplicationSharedMemory.create()`。

原 libcutils 先检查 `ro.treble.enabled` 与 `sys.use_memfd`，随后探测 `memfd_create`、seals、`ftruncate` 和 `ASHMEM_GET_SIZE`，并缓存探测结果。原 `init.rc:1055` 设置 `sys.use_memfd=false`，`build.prop:64` 设置 `ro.treble.enabled=true`。ashmem 分支读取 boot_id，以 `O_RDWR|O_CLOEXEC` 打开上述设备，要求字符设备且 `rdev` 非零，随后执行 `ASHMEM_SET_NAME=0x41007701` 和 `ASHMEM_SET_SIZE=0x40087703`。本轮内核已有 `CONFIG_MEMFD_CREATE/SHMEM/TMPFS` 和 memfd 的 `ASHMEM_GET_SIZE`，缺少 ashmem 配置与驱动。运行时属性 getter 和缓存值尚未直接采集；实际设备分支已由原失败路径确认。

## 提案

本轮独立采集候选采用已经核对原 SDK 实现的四个新增标签，保留原三标签、packed header、PID/TID、原 writev 返回值、消息原始字节和停止后的汇总。原 Android 策略、rc、系统 ELF、APK 签名和完整启动验收条件保持。

| 新增标签 | priority | 实际使用方 |
| --- | --- | --- |
| `AndroidRuntime` | `6` | `RuntimeInit.LoggingHandler/Clog_e` 的未捕获异常与错误报告；原 DEX 使用 log id 4、priority 6。 |
| `SystemServer` | `6` | 已有错误日志及 `reportWtf`；原 `Slog.wtf/Log.wtf` 使用 priority 6。 |
| `Zygote` | `4、5、6` | 原 native fork 的 system_server PID、waitpid 子进程退出状态、SIGCHLD 异常及 system_server 终止日志。 |
| `System` | `6` | 原 `SystemServer.run` 启动服务 catch 的两条 `Slog.e`，随后重新抛出异常。 |

新增采集只用于确认退出原因。每条消息继续最多保存 2048 bytes，每个入口最多 128 条；截取、遗漏和未取得消息分别记录。已有日志先由新 checker 复核，实际 CLI/GCD 用新的 core、生成源码和编译依赖运行。原因确认后再选择修复，后续完整系统服务、桌面、应用及性能分别验收。

### 采集实现与必要状态

[最终标签计划](../../../../../../../sdk-plain-validation-v1/system-server-taglog-v1/tag-plan.json)记录原 ELF/DEX 的标签、调用位置和固定字节。`SystemServer.run` 顶层 catch 实际使用 `System`，本轮因此增加该标签。实际 DEX 的 `Log.wtf` 使用 priority 6，准备期间按该结果调整新增范围；v1、v2 的本机准备来源和结果保持，下一组合使用[冻结 v3 头文件](../../../../../../../sdk-plain-validation-v1/system-server-taglog-v3/stock-init-taglog.h)和[validator](../../../../../../../sdk-plain-validation-v1/system-server-taglog-v3/taglog_validation.py)。

新增标签继续共用原 `seen/recorded` 两个计数，没有新增运行线程或 guest 日志文件。采集头由生成 execution 的 syscall 后处理和停止后的汇总使用，validator 由本机 fixture、原日志复核和下一组合的外层检查使用。移除四个新增标签会缺少上述 Java 启动错误和 Zygote 子进程退出记录。原全标签 FATAL 采集继续保留。

成功信号采集的[独立建议](../../../../../../../sdk-plain-validation-v1/system-server-taglog-v1/successful-signal-plan.json)尚未实现。可在生成 execution 中原 `run_syscall(number, args)` 返回后、`user_return` 处理信号前，记录已识别 Zygote/system_server 的成功 `129/130/131` 调用及发送者 PID/TID、原参数、PC 和原结果。身份与记录上限须在实现前固定，并验证原寄存器、参数和结果保持。冻结标签采集已取得当前 ashmem 异常，成功信号诊断按后续实际缺少的记录决定是否增加。

### ashmem 的下一项修复

保持原 `sys.use_memfd=false`，按[ashmem 设备提案](../architecture/2026-11-14-原SDK的ashmem设备.md)核对并接入固定官方 Android ashmem 驱动，以及原 init/ueventd 的 boot_id 节点创建。移植只调整实际 6.12 内核 API 所需接口，保留原 ashmem ioctl、共享 mmap、只读保护和引用释放契约。字符设备注册、原节点权限与 SELinux 标签必须在新原 init 中验证；11304-byte JNI 请求成功后再继续调查后续服务启动。完整系统启动条件继续保留。

## 曾考虑的替代方案

- 延长原 init 的观察时间：当前已经出现重复退出，先取得具体失败消息，再按实际进度决定观察时间。
- 启动独立 logcat 收集全部日志：需要新增启动、文件写入和停止流程；先复用现有有界 writev 采集。现有采集无法取得所需消息后再评估该方式。

## 验收标准

1. 原六入口的报告、原始日志、同盘身份、SQLite 与恢复结果保持；完整启动失败继续保留。
2. 新采集保留原调用参数、原结果和原消息字节，区分标签、优先级、PID/TID、截取和遗漏。来源、实际 C 编译依赖、新内核与回调地址对应。
3. CLI/GCD 均取得 Zygote 退出或相关 Java 启动失败的实际消息；未取得则明确记录缺少项，继续按实际故障位置调查。
4. 修复后重新验证两个 `sys.boot_completed=1`、Zygote/system_server/SurfaceFlinger PID、六个 Binder 服务和真实 `cmd` 查询；退出与同盘恢复继续检查。

### 本机准备结果

执行 `python3 -B work/sdk-plain-validation-v1/system-server-taglog-v3/prepare.py`，结果见[prepare-result.json](../../../../../../../sdk-plain-validation-v1/system-server-taglog-v3/prepare-result.json)。普通与 ASan/UBSan 两次 fixture 均成功：21 项协议拒绝、70 项 priority、28 项相似标签、28 项消息边界，validator 拒绝 107 份反例。七标签合计 `seen=169、recorded=128、omitted=41`，原参数和输入字节保持。实际 `.d` 消费冻结 v3 头；13 项来源、3 项生成文件和 3 项工具前后保持。原三标签的生成基线与历史头文件逐字相同，validator 函数 AST 保持。

已有 CLI `75500` 行与 GCD `75393` 行经新 validator 复核，两个原汇总均保持 `seen=2、recorded=2、omitted=0`，原日志与报告摘要保持。上述准备未运行 guest，后续实际 v2 结果见问题章节；新增 Java 标签已通过[六个 arm64/iOS26 对象编译](../../../../../../../sdk-plain-validation-v1/system-dmaheap-original-init-ios16k-v2/result.json)与[独立审阅](../../../../../../../sdk-plain-validation-v1/system-dmaheap-original-init-ios16k-v2/independent-review.json)，实际 `.d` 消费 Java v3、LogAudit 七头和新 ABI。App 链接与 iOS guest 执行尚未验证，完整启动仍为 `failed`。SDK 静态调查首轮因 dexdump 文本包含严格 UTF-8 解码无法接受的字节而停止；[原失败及执行源码](../../../../../../../sdk-plain-validation-v1/system-server-taglog-v1/sdk-review-attempt-01/report.json)保持，后续只对筛选文本使用替换解码，原 SDK 字节未变。

### ashmem 修复的验证条件

原 ELF 静态调查的 `22` 项来源和 `2` 项工具前后保持，原日志增量审阅保持两份 JSONL 与首份报告的摘要。下一独立内核先核对新 ELF/bin、实际回调头与各编译依赖，再以 CLI/GCD 验证 ashmem 的 `11304` bytes 创建、`SET_NAME/SET_SIZE/GET_SIZE`、共享 mmap、dup、只读保护及最后引用释放。原 init 验证对应 boot_id 的字符设备、权限和标签，并确认 system_server 越过当前 `ApplicationSharedMemory.create()` 失败位置；完整启动仍按原验收标准检查。

## 风险

同一入口可能包含多次 Zygote 启动，日志必须按实际 PID/TID 关联。消息截取和记录上限可能缺少后续堆栈，原 syscall 参数和返回值也只能说明记录位置的行为。当前数据来自 macOS 离线诊断构建，iOS 执行、设备资源、持续性能和热状态继续待真机验证。
