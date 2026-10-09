# 决策记录: Android 与 Metal 合并手机测试

状态: implemented

## 问题

提供 N4-G1 合并包的安装、回归范围与结果说明。

## 决策

保留实际构建、签名及设备结果；后续交付时按新包内容同步维护。

### 原有记录：N4-G1 原生 Linux、Android 与 Metal 合并测试

日期：2026-10-09。适用设备：当前 iPhone 17 / iOS 27.2。

本包包含 Linux 6.12 LKL、Bionic、ART ARM64 JIT、Android Framework 和基础
系统服务。内核、程序和只读系统盘随包提供，运行全部在 iPhone 内完成。
当前界面显示 Metal GPU 画面，并执行已有 Canvas、ART 和系统服务回归。
完整 Android 桌面仍待实现。

### 安装包

```text
/Users/fan/project/app/husk_hyos/work/native-linux-nce/ios/Husk-NativeLinux-N4-G1.ipa
```

版本 `0.11.1 (78)`，构建编号 `N4-G1-iOS-20261009-225459`，大小 322.80 MiB。
SHA-256：

```text
e4d5846d46bfd12df38aa6d38e79d485be258987d22eead7424b68b11bb2e644
```

CLI/GCD/可见窗口回归、ELF 检查、iOS 构建、归档与签名完整性检查均已通过。
完整构建记录保存在同目录的 `package.json`。本轮真机已通过全部 102 个
命名测试，最终结果为 `passed`；见 [真机结果](../../../../experiments/native-linux/results-ios-n4-g1.json)。
本机测试摘要见 [N4-G1 结果](../../../../experiments/native-linux/results-macos-n4-g1.json)。
N3-S3 在手机上申请约 16.58 GiB 连续虚拟地址失败。此包将总预留缩至约
4.58 GiB，并调整 Scudo、ART 对象堆及 CFI 检查表。N3-S4 手机已通过预留，
但 JIT 准备工具截断高地址，执行时卡住。N3-S5 修复完整地址传递、每页回复
检查和异常退出。S5 已在手机上完成全部 97 个不同名称的测试，最终结果为
`passed`。S6 调整日志界面：后台增量读取、只显示最近 80 行、结束后停止刷新。
日志读取的 5 组回归及 iOS 构建通过，用户已确认 S6 页面滚动恢复流畅。

### 操作

1. 在 iLoader 中导入 IPA，沿用当前账号覆盖安装，保留 JIT Helper 扩展。
2. 在手机上连接 LocalDevVPN，打开 Husk NCE，确认构建编号以 N4-G1 开头。
3. 点“启用内置 JIT 并测试”，保持应用在前台，等待“全部测试通过”。
4. 检查顶部是否出现深色背景与渐变三角形。动画提交 120 帧后保留最后画面，
   后续继续执行 ART 和系统服务测试。
5. 导出日志；如画面异常，同时导出 GPU 图片。

应用测试期间会禁用自动锁屏。若闪退，重新打开后先导出上次日志；再次启动
测试会覆盖日志。日志文件继续使用 `native-linux-n2-ios.log` 这一名称。
辅助进程的详细记录位于其独立容器 `Documents/husk-jit-helper.log`，可通过
已连接的 Mac 直接读取。S5 本次从首次 JIT 探测到调试器断开约 15.4 秒。
N4-G1 沿用 S6 的增量日志预览。GPU 视图固定在列表上方，滚动日志时保持显示。
ART 单进程检查设有 90 秒期限，Framework 与基础服务阶段设有 60 秒期限。
若长时间停在同一阶段，保留日志即可，无需反复运行。

### 覆盖范围

| 检查 | 本包包含的行为 |
| --- | --- |
| JIT 准备与更新 | 助手断开后更新固定地址代码；mmap/mprotect、fork、信号与动态执行权限 |
| Linux 进程和存储 | COW、线程、抢占、PID namespace、shell、ext4 持久数据与只读系统盘 |
| Bionic 与 Binder | 动态链接、TLS、48 MiB 同尺寸分配、CFI 无效目标拒绝、跨进程事务、文件描述符传递和服务退出通知 |
| ART 对象和调用 | 分配与 GC、数组、接口调用、同步、类型转换、异常、解释/JIT/JNI 调用 |
| ART 新执行路径 | 字符串及数值 intrinsic、方法内联、内联异常栈、Java 动态代理和注解 |
| GPU | Bionic 程序通过 Unix socket 调用 gfxstream/ANGLE Metal；并发上下文、着色器、顶点缓冲区、120 帧提交与像素校验 |
| Framework | Parcel、Looper、SharedMemory、JCA、Canvas/PNG、FileObserver 通知与清理 |
| 系统基础服务 | 属性权限、28 个 APEX 模块查询、26 组 aconfig 两次初始化、SystemSuspend 唤醒锁、installd 应用目录权限与清理删除 |
| Zygote | 原版命令协议、派生两个 Java 进程、UID、seccomp、cgroup、GC 与线程 |
| 内存与退出 | 空闲 RAM 归还、64 MiB 页面再次分配、上下文释放和内核异常检查 |

最终成功日志必须出现 `"result":"passed"`。本轮重点检查以下记录：

- `native_metal_display_created`：原生显示表面已创建。
- `linux_native_gles_color_buffer`：23.04 万像素复制和读回通过。
- `linux_native_gles_shader_frames`：120 帧绘制与提交完成，包含计时。
- `linux_native_gles_metal`：5 次连接及客体绘图通过。
- `native_graphics_channels_released`：5 条连接全部释放。

图形阶段发生崩溃、停顿或黑屏时，保留本次日志即可。客体绘图程序设有
45 秒 alarm，宿主或 GPU 线程卡住时仍可能无法自动结束；此时结束应用后
导出保留的日志，无需反复测试。

Framework 图片由 Android Canvas 绘制，通过 PNG 解码检查后交给 iOS 显示。

使用 4 GiB 稀疏客体地址窗口、512 MiB Linux RAM、512 MiB 静态 ELF 地址
范围和 64 MiB 动态执行区。虚拟地址按需提交物理页。ART 对象堆使用
MarkSweep/RosAlloc，堆地址预留上限为 1 GiB，每个 ART 实例的代码缓存上限为 32 MiB。
CFI 检查表占约 8 MiB，覆盖 1 TiB 以下的宿主地址；启动前校验客体范围。
Linux 用户态压力检查固定为 3 轮，包含 24 次命令与 168 个 pthread。

### 结果与边界

Mac 的 CLI、GCD 与可见窗口分别通过 101、102、102 个命名测试，另有
15 项 ELF 检查。宿主渲染器还通过 202 个上下文、100 组并发颜色检查。
实际结果及全部构建输入校验值保存在
`work/native-linux-nce/processes/lkl-summary.json`。S5 真机已通过此前的 97 个
命名测试；真机结果与日志校验值见
`experiments/native-linux/results-ios-n3-s5.json`。

单独的 system_server 检查已推进到 DisplayManager，当前等待 SurfaceFlinger。
PackageManager、系统窗口合成、桌面与持续交互尚未完成。该检查通过环境变量
`NCE_SYSTEM_SERVER_PROBE=1` 在 Mac 单独运行，手机合并包执行上表的已实现项目。

本轮 GPU 程序直接生成 gfxstream 协议；标准 Android `libEGL`、图形缓冲区
allocator/mapper 与 Composer 尚未接入。Mac 简单三角形的耗时只能用于当前
场景，Android 桌面与旧 HyperOS 的整体性能差异仍待后续测量。

已有本机局部对照包含 AES-GCM 指令扩展、ART 解释/JIT、关闭/启用方法内联。
这些结果用于检查对应实现；完整 Android 的启动速度、交互和发热需要在
系统功能完成后测量。实现记录与后续工作见
[Android 用户态阶段记录](../architecture/2026-10-09-Android用户态与Metal阶段记录.md)。

## 曾考虑的替代方案

通过合并产物集中检查运行时和图形，避免单独模块反复占用用户测试时间。

## 影响

通过底层与图形回归后，完整桌面、应用和持续运行仍按方案 A 单独验收。
