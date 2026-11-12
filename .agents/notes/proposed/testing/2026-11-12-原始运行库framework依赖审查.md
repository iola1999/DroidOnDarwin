# 决策记录: 原始运行库 framework 依赖审查

状态: proposed

## 问题

主会话正在把原始 SDK36 ART、linker 与 Bionic 接入完整 services/desktop。首轮在项目加密检查固定要求 `FIPS_mode()==0` 处停止。需要按实际调用顺序检查原库替换后的剩余前提，特别是项目私有 ART 接口、高地址引用和运行选项。本记录为只读审查，代码参考主线 `b0a1961` 及主会话在该提交后的加密检查修正；未修改主工作树。

## 提案

保持完整 framework、Zygote、桌面与 APK 检查。先运行已修正加密契约的候选；后续失败按进程、信号、运行库路径和原始日志定位。下表按实际调用顺序列出检查点，表内未复现项保留为风险。

| 执行阶段 | 调用方与当前契约 | 审查结论与后续检查 |
| --- | --- | --- |
| 服务准备 | [android-services-test.cpp](../../../../experiments/native-linux/userspace/android-services-test.cpp) 初始化 SELinux、属性、Binder、APEX registry、输入设备和加密检查 | 原库首轮已进入加密检查。Bionic 私有属性符号由 [property-service.cpp](../../../../experiments/native-linux/userspace/property-service.cpp) 用 `dlsym` 取得，包括 `__system_property_area_init`、`__system_property_add` 和 `__system_property_update`；该轮已经通过其初始化和读写检查 |
| 普通 framework 进程 | `nce_framework_test()` 初始化 aconfig、派生 classpath，再由 `nce_framework_check()` 启动原始 `app_process64` 运行 [NceFramework.java](../../../../experiments/native-linux/services/NceFramework.java) | helper 使用标准 Java/Android API 和 JNI 平台服务，没有调用 `nce_art_*`。这里首次组合真实框架、JCA、Binder/Parcel、Looper、共享内存、FileObserver 与软件 Canvas；各项必须继续实际运行 |
| 图形与平台守护进程 | [display-service-test.cpp](../../../../experiments/native-linux/userspace/display-service-test.cpp) 启动 allocator、composer、SurfaceFlinger；桌面阶段的 `nce_zygote_test()` 还启动 vold、artd、netd、音频、KeyMint 等服务 | 原库清单替换部分共享依赖，图形设备适配和若干项目库继续保留。需要核对实际加载路径和版本，不能仅凭文件同名判断 ABI 一致 |
| Zygote 与 fork | [framework-test.cpp](../../../../experiments/native-linux/userspace/framework-test.cpp) `nce_zygote_test()` 传入 `-Xzygote`、标准 Zygote 参数，设置 socket/fd、classpaths，再启动真实 Zygote | 普通 services 使用 lazy preload 并 fork 两个 [NceZygoteChild](../../../../experiments/native-linux/services/NceZygoteChild.java)。子进程检查 UID、静态状态隔离、seccomp、cgroup、4096 个字符串跨 GC 存活、线程和 Parcel。这些检查覆盖原库 fork 后的实际行为，尚不能从独立 JNI 成功推断其结果 |
| system_server、桌面与 APK | desktop 分支使用 `--start-system-server`，等待真实系统服务和 boot completed，再运行应用检查 | `services` 成功后仍需单独验证这一步。现行安装使用原签名 APK 和真实 PackageManager/ActivityManager；不同应用、原生库及启动生命周期继续依赖实际结果 |

### 私有 ART 接口与高地址引用

[linux-runner.c](../../../../experiments/native-linux/translation/linux-runner.c) 的 services 分支只启动 `/system/bin/android-services-test`；`/system/bin/art-test` 位于另一个 android 组件分支。后者在 [art-test.c](../../../../experiments/native-linux/userspace/art-test.c) 里直接查找 `nce_art_compile_method`、`nce_art_keep_interpreted`、`nce_art_nterp_enabled`、`nce_art_reference_base` 等符号，原 SDK `libart.so` 的动态符号表没有这些项目符号。services/desktop 调用方未发现这组私有接口依赖。

高地址引用约定集中在项目 [reference_heap.h](../../../../experiments/native-linux/art/reference_heap.h)、ART 补丁和项目生成 OAT/boot image 中。引用值是高地址 4 GiB 区域内偏移；原 ART 使用自身压缩引用与低地址堆。候选将 libart、libartbase、libdexfile、libprofile、libsigchain、libopenjdk 及匹配库替换为同源原文件，services helper 未发现引用编码操作。原库必须继续运行在支持低客体地址的 sparse/PTE 环境。

Direct 的 services 初始化将 `art-aot`、`art-boot-image`、`art-compiler-profile` 固定为 0；探针也将 `NCE_ART_AOT` 与 `NCE_ART_BOOT_IMAGE` 设为 0。该路径没有读取项目高地址 OAT/image。后续若启用编译缓存，必须生成与原运行库匹配的产物。[art-compiler-test.c](../../../../experiments/native-linux/userspace/art-compiler-test.c) 的 `--nce-image-offset`、`--nce-inspect-image` 属于项目 dex2oat 扩展，当前 services/desktop 不执行该程序。

### 运行选项与配置残留

`framework-test.cpp` 无条件设置 `dalvik.vm.jitthreshold=1024`、`-Xjitwarmupthreshold:1024`、`-Xnoimage-dex2oat`、不存在的 boot-image 路径和 `dalvik.vm.gctype=MS`。本地对应 ART 的 `parsed_options.cc` 定义了前述 `-X` 选项，未发现其属于项目自定义参数。1024 阈值会改变原始 ART 的编译时机，完整系统性能比较必须记录该值；当前独立组件的默认阈值结果不能直接套用。

MS 来自项目无 read barrier 的 ART 构建，属于需要整理的配置残留。本地对应 `runtime.cc` 会在 `gUseReadBarrier` 为真时选择 CC 和 CCBackground；因此该属性未必引起原库失败。初步仅检查 `Heap` 断言曾产生「MS 可能立即阻断」的判断，进一步跟踪 Runtime 的实参后修正。原 SDK 组件已经实际输出 concurrent copying GC；完整 framework 仍需从本轮启动日志确认实际选择。

`NCE_ART_NTERP`、`NCE_ART_EXPANDED_JIT`、`NCE_ART_FAST_BASELINE` 被写入 `/data/nce/` 文件，项目 ART 会读取这些开关；原 SDK 库没有这组项目读取逻辑。候选的 requested 字段只能描述探针请求，解释器与编译器的实际行为需要原库日志、完成 PC 或方法入口检查。

`pm.dexopt.*=verify`、禁用后台 dexopt 与 ART service dexopt 的属性是当前启动适配的一部分。候选 v1 的 ext4 实际检查确认 `/apex/com.android.art/bin/dex2oat64`、`/apex/com.android.art/bin/dex2oat`、`/system/bin/dex2oat` 与 `/system/bin/nce-dex2oat` 均缺失。原 artd 已被打包，但生成 OAT、启用正常后台优化和处理强制 dexopt 仍需匹配编译器。该缺失尚未阻断此前禁用相关任务的项目 APK 流程，原库 APK 流程需独立验证。

### linker 与 Bionic 的边界

[ld.config.txt](../../../../experiments/native-linux/art/ld.config.txt) 为 `/system/bin` 创建非隔离 namespace，主要搜索 `/system/lib64`，APEX namespace 再连接 default；桌面图形进程另设置 vendor 搜索路径。原库候选同时保留 APEX 原路径和 system 别名文件，文件字节相同仍可能对应不同 inode。后续应从进程 maps 核对 libc、libart 和 libsigchain 的实际来源，特别是按绝对 APEX 路径加载与按 soname 加载并存的场景；当前尚未复现重复装载错误。

[framework-log.c](../../../../experiments/native-linux/services/framework-log.c) 仍通过 `LD_PRELOAD` 重定向 logd logger，并替换 SDK 的 `_Z19_set_seccomp_filter10FilterType`，保留原 ARM64 策略并允许 243–245 三个项目代码映射系统调用。原库 Direct 执行使用标准 mmap/mprotect，后续产品适配需要明确这份附加策略的使用方。当前保留真实 seccomp 和 Zygote 子进程检查，不预先删除策略来通过运行。

原 Bionic 恢复原始 TBI、Scudo 大范围预留和 SCS 布局，项目缩小 Scudo/SCS 预留的配置不再生效。独立原始 Bionic 与 ART 已验证部分线程/内存行为；多进程系统压力、Zygote 后的信号、分配失败和回收仍需真实负载检查。

### 原 ART 恢复的行为

[android16.patch](../../../../experiments/native-linux/art/android16.patch) 曾将项目 JIT 的隐式空指针检查关闭，并拒绝 native stub 与 OSR 编译。原 ART 恢复这些行为。已有标准 JNI 主要检查显式 Java 异常和原生 JNI 异常；已确认编译的热方法是整数计算。编译后隐式 NullPointerException、StackOverflowError、捕获后继续执行和 OSR 尚缺实际覆盖，可能在 framework 或 APK 首次运行相关方法后暴露信号/寄存器恢复缺陷。主会话已授权下一实验补上这些行为，保持原库、标准 JNI 和原执行语义。

## 曾考虑的替代方案

提前删除 MS、改变阈值、停用 seccomp 或关闭原 ART 的隐式异常。当前尚无对应实际失败，提前更改会丢失可比较配置或改变待验行为。将整个 desktop 成功归因于已经成功的独立 JNI 组件。两者的进程、服务和图形范围不同，需要分别执行。

## 验收标准

主会话的候选依次完成普通 framework、原 Zygote 两次 fork、system_server、桌面、Java/JNI APK 安装更新和持久化检查。日志记录实际 GC、运行库映射、JIT 参数和首次失败进程；保留原输入摘要和失败输出。另用原始 ART 验证编译后隐式异常与继续运行，再按实际结果推进 OSR。本文只完成静态审查与候选文件检查，整机验收尚未完成。

## 风险

非隔离 namespace 和手工服务启动继续限制对原系统启动流程的推断。原库恢复的信号与编译行为可能带来新故障；本轮没有完整 framework、HyperOS 或 iPhone 的新增运行结论。候选 v1 的文件检查随镜像版本记录，新候选需再次核对。
