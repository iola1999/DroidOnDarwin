# Android 可交互桌面实施计划

更新日期：2026-10-10。用户已选择方案 A，任务由当前会话的 active goal 跟踪。

## 交付条件

在 iPhone 内运行 Linux、Android 系统服务及真实 Launcher。触摸、滑动、
返回、主页和应用切换能够操作 Android 窗口；至少两个真实 APK 可以启动与
切换；应用写入的数据在 Android 和宿主 App 重启后保留。完成本机功能回归、
可见窗口操作与构建检查后，交付一个 IPA 集中进行手机测试。

阶段交付前继续开发。需要用户决定产品范围，或者遇到必须在手机上确认、
本机无法继续验证的问题时，提供具体测试包和失败条件，再请求用户参与。
本阶段没有新增截止时间。

## 已有结果与实现边界

N4-G1 已在 iPhone 17 / iOS 27.2 通过 102 个命名测试。gfxstream 经 ANGLE
Metal 使用 Apple A19 GPU，360×640 的 120 帧提交共 2037.544 ms。当前 GPU
测试直接发送 GLES 与 renderControl 协议。随后在 Mac 上完成标准 EGL 驱动、
缓冲区分配、Composer 与 SurfaceFlinger 集成，已检查普通 Surface 的三组
绘图和合成像素。新增部分尚待后续整包手机测试。

原版 system_server 已进入 DisplayManager 初始化，正在等待 SurfaceFlinger。
原版 installd 的应用目录管理检查已通过，应用目录目前位于临时文件系统。
全系统启动时间、交互延迟、持续发热与原始 QEMU 方案的差异仍待测量。

## 实施顺序

1. 检查固定 SDK 中的图形驱动和上游 gfxstream 客户端。复用标准 EGL/GLES
   与 Android 缓冲区协议，连接现有 Linux socket 图形通道。先验证标准 EGL
   上下文、shader、纹理、共享缓冲区及像素结果，再运行系统合成服务。
2. 提供实际默认显示、buffer allocator/mapper 和 Composer，启动原版
   SurfaceFlinger。图形后端在整个 Android 会话中保持运行，窗口直接提交给
   CAMetalLayer；回归模式继续覆盖资源释放与多连接。
3. 推进 system_server、PackageManager、WindowManager 和 Launcher，按
   原始 APK、接口声明与运行日志处理缺少的系统输入。记录 Linux 内存、
   宿主内存和各服务启动状态。
4. 将应用数据目录接入持久 ext4 存储，加入两个 APK 的安装、启动、切换及
   数据重启检查。输入事件通过 Android 输入体系交给实际应用窗口。
5. 完成可见本机窗口交互、错误恢复、退出和回归，再构建与检查 IPA。

优先沿用 SDK 的原始程序和上游图形实现。新增适配代码只承担当前平台需要的
传输、显示、输入和生命周期差异。跨模块改动完成后使用 reclaim-code-entropy
审查真实使用方、必要状态、重复实现和可删除分支，并记录实际验证结果。

## 进展

- 已读取并核对 N4-G1 真机日志、GPU 输出和本机回归记录。
- 已从同一 SDK 提取 Launcher、SystemUI、系统应用和 vendor 图形组件，
  校验 1023 个文件，删除下载包与中间镜像。
- 原版 libEGL、GLES emulation 驱动通过 socket 连接 gfxstream。新增检查
  覆盖 EGL 初始化、4096 像素清屏、shader 与纹理绘图、共享上下文纹理。
  CLI/GCD/可见窗口分别通过 104/105/105 个命名测试。
- SDK gralloc 使用 goldfish_address_space 分配共享内存，原型继续复用
  上游 allocator/mapper 的格式、元数据及 Binder 接口，改用 Linux memfd
  共享存储，并通过 renderControl 传输 CPU 读写内容。正在构建和验证。

- allocator/mapper 已完成编译，正在运行实际 `AHardwareBuffer` 检查：37×23
  RGBA 缓冲区的 CPU 写入、EGLImage 导入、GPU 清屏、CPU 读回，以及通过
  Unix socket 向 exec 后的另一进程传递句柄。
- 联调发现两项启动条件：设备 VINTF 主清单必须存在，mapper 原生服务必须
  配置 SELinux 服务标签。另发现缺少 `binder_shell.h` 会改变 `ICInterface`
  虚函数表，导致原版 AIDL 库调用错误的方法；已加入同一固定版本的头文件。
  这轮回归完成后继续检查 Composer 与 SurfaceFlinger。

- 上述共享缓冲区检查及完整回归已通过：CLI/GCD/可见窗口分别通过 101/101/102 个命名测试。原始记录位于 `work/native-linux-nce/processes/`，摘要保存为
  `experiments/native-linux/results-macos-desktop-graphics.json`。
- 复杂度检查：复用 SDK EGL/GLES 和上游 gralloc 的格式、元数据及 Binder
  实现；memfd 改动后删除 mapper 中已无调用的物理地址转换状态与辅助函数。
  标准驱动检查、共享缓冲区检查和底层协议检查覆盖不同的接口，继续保留。

### Composer 与 SurfaceFlinger 联调

原版 Composer3 和 SurfaceFlinger 已完成显示初始化，发布 `SurfaceFlinger`
及 `SurfaceFlingerAIDL` 服务。显示配置为 360×640、160 dpi；Skia GL 经
gfxstream 和 ANGLE Metal 初始化成功。图形 socket 已允许 Android 服务 UID
访问，Framework 子进程继承相同的 EGL 驱动路径。

显示服务运行后，Zygote 曾超过原有 20 秒测试期限；临时使用 1 Hz 后恢复通过。
宿主采样显示代码复制、清零和指令缓存刷新占用较多时间。已加入代码权限切换
检查：系统允许撤销、恢复已准备代码页的权限时，保留代码内容，并逐页比较
Linux 原始字节。内容与权限相同的页面直接恢复访问，发生变化的页面重新复制、
改写及刷新缓存。系统拒绝权限切换时继续使用复制路径。

恢复 60 Hz 后，CLI/GCD/可见窗口分别通过 103/103/104 个命名测试，包含
SurfaceFlinger 注册、Zygote 请求和 Java 子进程。CLI 本轮复用代码内容
10,104,766,464 字节，实际复制 81,608,704 字节。原有动态代码检查继续覆盖
128 次内容更新、同址替换、fork、执行权限与信号。手机上的权限切换结果仍待
下一次整包测试。系统桌面和应用交互继续列为后续验收项。

### Surface 窗口与合成像素

Java 测试通过原版 `SurfaceControl` 建立 360×640 窗口，连续提交三组左右
不同颜色的 Canvas 内容；每帧通过 Android `ScreenCapture` 获取
SurfaceFlinger 合成结果，核对大小和两侧像素。最后一帧保存为 PNG。
CLI/GCD/可见窗口分别通过 104/104/105 个命名测试；可见窗口也确认显示
左白右黑。摘要为 `experiments/native-linux/results-macos-surfaceflinger-composition.json`。

本轮发现测试退出时的库生命周期问题：`DisplayControl` 需要加载
`libandroid_servers.so`，其依赖与已加载的 `libmediautils.so` 含有同名
`IBatteryStats::descriptor`。服务库卸载后，Framework 的静态字符串仍引用
已解除映射的数据。图形测试改为启动时加载服务库，使相关静态对象在进程
退出前保持有效；`app_process` 正常返回和退出检查随后通过。此加载设置仅
用于独立图形测试。图形像素检查通过后继续启动完整系统服务。

复杂度检查：使用 Android 原有窗口、截图和缓冲区接口；未增加宿主截图协议。
该检查同时覆盖图形更新、跨进程合成和进程退出，与直接 EGL 清屏检查的范围
不同。调试时加入可选 `NCE_FAULT_TRACE`，只输出失败的客体访存位置与寄存器。
