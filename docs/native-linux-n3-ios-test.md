# N3-A1 原生 Linux + NCE 手机测试

日期：2026-10-09。适用设备：当前 iPhone 17 / iOS 27.2。

本包将真实 Bionic、Android 动态链接器、Linux Binder 和 ART Java 负载合并
到一次设备测试中。运行所需 Linux 内核、共享库和 Java 文件均随包提供。
运行过程全部在 iPhone 内完成。

## 安装包

```text
/Users/fan/project/app/husk_hyos/work/native-linux-nce/ios/Husk-NativeLinux-N3-A1.ipa
```

构建编号 `N3-A1-iOS-20261009-121807`，版本 `0.10.0 (70)`，大小 40.29 MiB。
SHA-256：

```text
7b006ed68a599df073826a64b18a4457681c7cca0521ca150a81e4a309a13be7
```

## 操作

1. 在 iLoader 中导入 IPA，沿用当前账号覆盖安装，保留 JIT Helper 扩展。
2. 在手机上连接 LocalDevVPN，打开 Husk NCE，确认构建编号。
3. 点“启用内置 JIT 并测试”，保持应用在前台，等待“全部测试通过”。
4. 导出日志。本次只需运行合并包一次。

应用测试期间会禁用自动锁屏。若退回桌面，重新打开后直接导出上次日志；
再次启动测试会覆盖日志。若 Java 测试状态持续超过 120 秒，导出日志后关闭
应用。Java 启动程序本身设有 90 秒期限，正常收到定时信号时会记录超时并退出。

## 本轮检查

Mac 的 CLI 与 GCD 入口已完成同一组回归。此次确认设备上的大地址预留、
JIT 助手断开后的代码执行，以及 Bionic/ART 在线程与 GC 场景中的运行条件。

| 日志项目 | 成功条件 |
| --- | --- |
| `android_bionic_threads_allocator_tls` | Android 动态链接、线程、分配器和 TLS 正常 |
| `linux_binder_cross_process_roundtrip` | 两个 Linux 进程完成 32 次往返，最大负载 65,536 字节 |
| `linux_binder_fd_transfer` | 文件描述符传递后共享写入正确 |
| `art_high_reference_heap` | 高地址堆分配、引用编码、释放复用和越界检查通过 |
| `android_art_java_workload` | 16 MiB 数组分配、弱引用回收、64 个存活循环对象、两个线程和三类异常通过 |
| `android_art_vm_destroyed` | VM 销毁成功，记录创建至销毁的耗时 |
| `linux_nce_context_lifetime` | 上下文分配与释放数量相同 |
| `result: passed` | 整轮结束，包括既有 Linux、动态库和 shell 回归 |

Linux 用户态压力检查固定为 3 轮，共 24 次命令、168 个 pthread。测试材料
使用 16 GiB 稀疏客体地址窗口与 256 MiB Linux RAM；虚拟地址预留按需提交物理页。

本包仍是功能原型。ART 采用解释执行，HyperOS 界面和 Android 系统服务尚未
接入。应用中的内置 JIT 用于授权 Linux/NCE 原生代码执行。完整 Android 的
性能需要在后续系统集成后测量。

实现、限制和本地测试结果见 [Android 用户态阶段记录](native-linux-android-stage.md)。
