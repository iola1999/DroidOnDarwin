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
测试直接发送 GLES 与 renderControl 协议；原版 SurfaceFlinger 和普通 Android
应用所需的标准 EGL 驱动、缓冲区分配与 Composer 接口尚待集成。

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
