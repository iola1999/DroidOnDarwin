# 决策记录: 原 SDK 的 ashmem 设备

状态: proposed

## 问题

`sdk-plain-init-system-dmaheap-16k-v2` 的 CLI 和 GCD 原启动均记录 `ApplicationSharedMemory.nativeCreate` 抛出 `Failed to create ashmem: No such file or directory`，随后 Zygote 退出。原失败 openat 日志各有 30 次 `/dev/ashmem<boot_id>` 返回 `-2`，后缀与本入口实际 boot_id 相同；该日志没有 PID/TID，Java 日志的 PID/TID 与 system_server 相同。具体控制流与消息对应由[Zygote 退出记录](../bug-fix/2026-11-14-原SDK的Zygote反复退出.md)保存。

原 SDK `init.rc` 将 `sys.use_memfd` 设为 `false`。当前 LKL 6.12.0 支持 memfd、seals 和 libcutils 探测所需的大小查询，但源码与配置缺少 ashmem 驱动。原 ueventd 的 `HandleAshmemUevent` 对名为 `ashmem` 的 misc uevent 创建相同 major/minor 的 boot_id 后缀节点，普通节点同时保留。原 `/dev/ashmem*` 的权限为 `0666 root root`。

## 提案

接入固定 Android common 的 C ashmem 实现，保持原 SDK 的属性、rc、libcutils、JNI 和 SELinux 策略。原始驱动和 UAPI 保留许可证与固定修订，构建时复制到独立内核源码树；原始文件、适配脚本、生成文件、Kconfig/Makefile 和实际编译依赖进入构建标识。当前 common 6.12 已改用 Rust，先检查同版本的历史 C 实现；历史 C 实现需要适配 LKL 6.12 API 后，记录具体差异并验证实际行为。

先用独立内核和 CLI/GCD 检查普通 misc 节点、名称/大小、共享 mmap、fork/dup、只读限制、pin/unpin、失败清理与最后引用解除。重新生成 core、全部内核入口和回调地址，再运行原 init 六入口。普通与 boot_id 后缀节点由原 ueventd 生成，权限和原域下打开结果在实际启动中核对。system_server 创建共享内存后的服务进度、完整启动、桌面与 APK 继续逐项验收。

## 曾考虑的替代方案

- 将 `sys.use_memfd` 设为 `true`：现有 Framework 测试已经采用该配置，原 SDK 和部分原始 JNI 调用仍可能使用 ashmem ioctl；本轮先实现原设备接口。
- 为打开路径建立普通文件或只修改节点名称：原 libcutils/JNI 还需要 ashmem ioctl、保护范围和共享映射，路径本身无法提供这些行为。
- 为最新 Rust 驱动增加 LKL Rust 构建支持：这项选择需要扩大工具链与架构适配范围，先检查可用的固定 C 驱动。

## 验收标准

1. 固定源码、许可证、适配差异、内核配置和实际对象的编译依赖可核对；历史 core、原盘及失败报告保持。
2. 独立 CLI/GCD 检查 ashmem 的大小、名称、共享与引用生命周期、保护限制和必要失败场景；重新导出的全部内核地址与本轮 ELF/bin 对应。
3. 原 init 实际创建设备的普通及 boot_id 后缀节点，权限与 SELinux 标签保持原契约；原 `ApplicationSharedMemory.nativeCreate` 的结果、Zygote/system_server 和后续服务由实际日志核对。
4. 两个 original 继续要求 `sys.boot_completed=1`、真实进程、Binder 服务和 `cmd` 查询；退出、同盘恢复、SQLite 与 iOS 构建继续检查。缺少任一完整启动条件保留 failed。

## 风险

固定历史驱动对 Linux 6.12 的 shrinker、VMA 和 shmem API 依赖需要逐项核对。当前失败日志的 openat 记录缺少 PID/TID，源码控制流和相邻 Java 消息只能支持其已记录的范围。实际节点、内核回收、Android 域和 iOS 运行资源继续由后续结果确认；本阶段不安排手机验证。
