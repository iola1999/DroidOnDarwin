# N3-S5 原生 Linux 与 Android 合并测试

日期：2026-10-09。适用设备：当前 iPhone 17 / iOS 27.2。

本包包含 Linux 6.12 LKL、Bionic、ART ARM64 JIT、Android Framework 和基础
系统服务。内核、程序和只读系统盘随包提供，运行全部在 iPhone 内完成。
当前界面用于执行合并回归并显示 Canvas 绘图结果，完整 Android 桌面仍待实现。

## 安装包

```text
/Users/fan/project/app/husk_hyos/work/native-linux-nce/ios/Husk-NativeLinux-N3-S5.ipa
```

版本 `0.11.1 (76)`，构建编号 `N3-S5-iOS-20261009-213113`，大小 319.22 MiB。
SHA-256：

```text
f13d8222563b5b1e09f1dce11a234528a7cbc4fef7a1905ceeac620ba44450d3
```

CLI/GCD 回归、ELF 检查、iOS 构建、归档与签名完整性检查均已通过。
完整构建记录保存在同目录的 `package.json`。
N3-S3 在手机上申请约 16.58 GiB 连续虚拟地址失败。此包将总预留缩至约
4.58 GiB，并调整 Scudo、ART 对象堆及 CFI 检查表。N3-S4 手机已通过预留，
但 JIT 准备工具截断高地址，执行时卡住。N3-S5 修复完整地址传递、每页回复
检查和异常退出，10 组协议回归通过；手机执行待确认。

## 操作

1. 在 iLoader 中导入 IPA，沿用当前账号覆盖安装，保留 JIT Helper 扩展。
2. 在手机上连接 LocalDevVPN，打开 Husk NCE，确认构建编号以 N3-S5 开头。
3. 点“启用内置 JIT 并测试”，保持应用在前台，等待“全部测试通过”。
4. 导出日志，并保留 Android 绘图结果。

应用测试期间会禁用自动锁屏。若闪退，重新打开后先导出上次日志；再次启动
测试会覆盖日志。日志文件继续使用 `native-linux-n2-ios.log` 这一名称。
辅助进程的详细记录位于其独立容器 `Documents/husk-jit-helper.log`，可通过
已连接的 Mac 直接读取。JIT 准备逐页检查回复，初次准备耗时待测。
ART 单进程检查设有 90 秒期限，Framework 与基础服务阶段设有 60 秒期限。
若长时间停在同一阶段，保留日志即可，无需反复运行。

## 覆盖范围

| 检查 | 本包包含的行为 |
| --- | --- |
| JIT 准备与更新 | 助手断开后更新固定地址代码；mmap/mprotect、fork、信号与动态执行权限 |
| Linux 进程和存储 | COW、线程、抢占、PID namespace、shell、ext4 持久数据与只读系统盘 |
| Bionic 与 Binder | 动态链接、TLS、48 MiB 同尺寸分配、CFI 无效目标拒绝、跨进程事务、文件描述符传递和服务退出通知 |
| ART 对象和调用 | 分配与 GC、数组、接口调用、同步、类型转换、异常、解释/JIT/JNI 调用 |
| ART 新执行路径 | 字符串及数值 intrinsic、方法内联、内联异常栈、Java 动态代理和注解 |
| Framework | Parcel、Looper、SharedMemory、JCA、Canvas/PNG、FileObserver 通知与清理 |
| 系统基础服务 | 属性权限、28 个 APEX 模块查询、26 组 aconfig 两次初始化、SystemSuspend 唤醒锁、installd 应用目录权限与清理删除 |
| Zygote | 原版命令协议、派生两个 Java 进程、UID、seccomp、cgroup、GC 与线程 |
| 内存与退出 | 空闲 RAM 归还、64 MiB 页面再次分配、上下文释放和内核异常检查 |

最终成功日志必须出现 `"result":"passed"`。Framework 图片由 Android Canvas
绘制，通过 PNG 解码检查后交给 iOS 显示。

使用 4 GiB 稀疏客体地址窗口、512 MiB Linux RAM、512 MiB 静态 ELF 地址
范围和 64 MiB 动态执行区。虚拟地址按需提交物理页。ART 对象堆使用
MarkSweep/RosAlloc，堆地址预留上限为 1 GiB，每个 ART 实例的代码缓存上限为 32 MiB。
CFI 检查表占约 8 MiB，覆盖 1 TiB 以下的宿主地址；启动前校验客体范围。
Linux 用户态压力检查固定为 3 轮，包含 24 次命令与 168 个 pthread。

## 结果与边界

Mac 的 CLI 与 GCD 入口分别通过 97 和 98 个命名测试，另有 15 项 ELF 检查。
GCD 多出继承信号屏蔽状态的回归。实际结果及全部构建输入校验值保存在
`work/native-linux-nce/processes/lkl-summary.json`，手机结果仍待确认。

单独的 system_server 检查已推进到 DisplayManager，当前等待 SurfaceFlinger。
PackageManager、系统窗口合成、桌面与持续交互尚未完成。该检查通过环境变量
`NCE_SYSTEM_SERVER_PROBE=1` 在 Mac 单独运行，手机合并包执行上表的已实现项目。

本轮本机局部对照包含 AES-GCM 指令扩展、ART 解释/JIT、关闭/启用方法内联。
这些结果用于检查对应实现；完整 Android 的启动速度、交互和发热需要在
系统功能完成后测量。实现记录与后续工作见
[Android 用户态阶段记录](native-linux-android-stage.md)。
