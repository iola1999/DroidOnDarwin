# N3-J1 原生 Linux + ART JIT 手机测试

日期：2026-10-09。适用设备：当前 iPhone 17 / iOS 27.2。

本包包含真实 Bionic、Android 动态链接器、Linux Binder 和 ART ARM64 JIT。
运行所需内核、共享库、编译器与 Java 文件均随包提供，全部在 iPhone 内执行。

## 安装包

```text
/Users/fan/project/app/husk_hyos/work/native-linux-nce/ios/Husk-NativeLinux-N3-J1.ipa
```

构建编号 `N3-J1-iOS-20261009-134627`，版本 `0.11.0 (71)`，大小 41.86 MiB。
SHA-256：

```text
66330c7d1943fa8eb862d433ab38d109dfe3eeafe08bac4d8d45f7149ff2510d
```

本包已完成本地回归、iOS 构建、归档和签名完整性检查，手机执行待确认。
用户外出期间保留此包，具备侧载条件后再做一次合并测试。

## 操作

1. 在 iLoader 中导入 IPA，沿用当前账号覆盖安装，保留 JIT Helper 扩展。
2. 在手机上连接 LocalDevVPN，打开 Husk NCE，确认构建编号。
3. 点“启用内置 JIT 并测试”，保持应用在前台，等待“全部测试通过”。
4. 导出日志。本次只需运行合并包一次。

应用测试期间会禁用自动锁屏。若退回桌面，重新打开后直接导出上次日志；
再次启动测试会覆盖日志。若 Java 测试状态持续超过 120 秒，导出日志后关闭
应用。Java 启动程序设有 90 秒期限，正常收到定时信号时会记录超时并退出。

## 本轮检查

Mac 的 CLI 与 GCD 入口均已通过同一组回归。本次手机测试重点确认动态执行区
在 JIT 助手断开后仍能更新并执行，以及编译代码的 GC、异常和调用行为。

| 日志项目 | 成功条件 |
| --- | --- |
| `prepared_fixed_address_code_updates` | 含新增动态执行区在内的代码范围，各完成 8 次更新和执行 |
| `linux_dynamic_code` | memfd 代码映射、128 次更新、fork、信号返回、权限拒绝与释放检查通过 |
| `android_bionic_threads_allocator_tls` | Android 动态链接、线程、分配器和 TLS 正常 |
| `linux_binder_cross_process_roundtrip` | 两个 Linux 进程完成 32 次往返，最大负载 65,536 字节 |
| `android_art_java_workload` | 分配、弱引用回收、存活循环对象、线程和异常通过 |
| `android_art_jit_arithmetic` | 解释/JIT 均得到同一独立校验值，记录两百万次循环耗时 |
| `android_art_jit_objects` | 数组更新、对象字段读写、GC 和两类异常通过 |
| `android_art_jit_calls` | 编译代码、解释器与 JNI 的对象及空引用返回正确 |
| `android_art_jit_allocations` | 对象/数组分配、虚方法调用与异常通过 |
| `android_art_jit_automatic` | 普通热点方法自动编译，不支持的方法仍可解释执行 |
| `linux_nce_context_lifetime` | 上下文分配与释放数量相同 |
| `result: passed` | 包括既有 Linux、动态库和 shell 回归在内的整轮结束 |

Linux 用户态压力检查固定为 3 轮，共 24 次命令、168 个 pthread。测试使用
16 GiB 稀疏客体地址窗口与 256 MiB Linux RAM，虚拟地址按需提交物理页。
新增动态执行区为 32 MiB；ART 代码缓存上限为 32 MiB，代码和元数据各占一半。

## 结果边界

当前使用 MarkSweep/RosAlloc。ARM64 JIT 按指令支持范围编译，尚未适配的方法
继续解释执行。完整 HyperOS 界面、系统服务、图形显示和持续交互尚未接入。

本机最终一轮整数微基准：CLI 解释/JIT 为 160.917/2.642 ms，GCD 为
156.771/2.648 ms，约 59–61 倍。该结果只适用于这段计算负载；手机和完整
Android 的性能仍需实测。

实现、进展和后续方向见 [Android 用户态阶段记录](native-linux-android-stage.md)。
