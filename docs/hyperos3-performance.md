# HyperOS 3 本地性能优化记录

## 目标与当前状态

目标是在 iPhone 17 上，通过 Husk 本地运行完整 HyperOS 3，改善触摸响应、桌面
滑动、设置及应用切换速度。测试设备为 iPhone18,3、iOS 27.2，日志记录物理内存
7672 MiB。每项修改分别验证，记录运行配置、测量结果和功能变化。

2026-10-08 已启动[原生 Linux 与 NCE 原型](native-linux-nce-prototype.md)。
Mac 上的直接执行、地址空间与原生 Linux 子系统检查通过。完整用户进程、
手机端运行和 HyperOS 整机收益仍待验证。

此前结束 QEMU 优化。硬件虚拟化仍缺少当前侧载环境下的可用路径；完整
Linux 原生移植的运行条件与性能尚未确认。未验证原型和可重新生成的大文件
已清理。路线判断与保留内容见[最终评估](hyperos3-performance-final-assessment.md)。

## 实验与设备记录

2026-10-07 已完成系统导入和手机启动。用户反馈 HyperOS 与此前的 LineageOS
均明显卡顿。P1 build 30 已在手机运行；用户再次反馈操作卡顿、手机明显发热，
随后关闭 Husk。2026-10-07 继续调研能够大幅改善可用性的架构改造。

用户要求完整 HyperOS 的可用性大幅提升，并以此决定是否继续投入。现有结果
确认系统可以启动、JIT 可执行、旧构建已使用 Mesa/virtio 与宿主 Metal，但
交互仍明显卡顿。恢复 GPU 有机会改善本轮软件渲染的表现，尚无可复现结果
支持其达到用户要求的可用性。

G1 已实现 gfxstream GLES 命令转发、iOS ANGLE Metal 适配、QEMU 显卡选择和
客体驱动更新。Mac 上完整 HyperOS 的 SurfaceFlinger 与独立 EGL 程序均使用
Metal；iOS 库、应用及 build 36 IPA 已构建。单核 TCG 下，固定绘制负载的三次
结果为 10.960、16.169、10.636 ms/帧；软件对照在 600 秒内未完成，稳定绘制
倍数未知。Metal 下两轮 Settings 冷启动分别耗时约 96 秒和 174 秒，后者已使用
ART speed 产物，尚未达到基础可用性。两轮未组成重复的 ART 开关对照。
手机端初始化、交互速度与温度尚待验证。

E6 已在 E5 的跨页连接、向量和原子指令基础上加入原生间接调用缓存。Linux
微测中间接调用约快 7.1 倍，访存、系统调用、原子操作和指针认证仍有退步。
E6、Metal 与 ART speed 产物组合的结束截图已显示桌面，但启动查询超时，
实际启动耗时未知。新安装默认
关闭 ARM64 Direct；内核 Direct 实验也继续关闭。最新结果见
[开发进展](execution-progress.md)。
本次读取的手机日志来自 build 33，仍为单核、软件图形，交互未达到基础可用性。

2026-10-08 的 M28–M30 继续修改 CPU 执行路径：地址解析参数缓存、TCG 内核
间接跳转查询，以及软件 TLB 索引。M30 的大步长访存在 Mac Linux 测试中约快
14 倍，连续页访问约慢 9%；完整 HyperOS 在 842.160 秒确认启动，设置启动
耗时 154.251 秒，帧完成时间无效。该结果尚未满足可用性目标。M31 已重写
Direct 编译缓存的分配与淘汰，固定压力负载约快 2.07 倍，整机收益仍需验证。
M32 复用有效的只读映射，M33 为淘汰代码增加有容量限制的普通内存缓存。
M33 的固定压力负载约快 4 倍，完整 HyperOS 仍需 744.865 秒确认启动，设置
启动命令耗时 182.346 秒，帧时间检查失败。该组合仍未达到基础可用性。
完整数据与失败记录见[开发进展](execution-progress.md)。

源码核对、HyperOS ELF 对齐检查和架构原型的实验顺序见
[执行架构调研](hyperos3-execution-research.md)。重点评估保留客体 Linux 内核、
为 Android 用户态增加专用 ARM64 后端的路径，并单独验证 gfxstream 图形后端。

## 已确认的运行情况

来源为用户提供的 `/Users/fan/Downloads/husk-log/` 四份日志。

| 项目 | 记录 | 判断边界 |
| --- | --- | --- |
| 应用版本 | 0.8.0，build 27，`9978afa-dirty` | 本地源码基于 `82e50bd`；手机旧构建的全部改动未知 |
| JIT | 512 MiB 预分配，执行自检成功，调试器正常退出 | 已经能够执行生成的机器码 |
| CPU | `max,pauth-impdef=on,sve=off,sme=off`，4 vCPU | TCG 系统模拟仍有指令转换、软件 MMU 和跨核同步开销 |
| TCG 参数 | `tb-size=256,thread=multi,split-wx=on` | 分配器返回 512 MiB，缓存实际布局和清空频率需单独测量 |
| 显示 | `virtio-gpu-gl-pci,xres=360,yres=800` | 导入时准备的 CPU 渲染偏好与本次运行参数不同 |
| GPU | ANGLE Metal Renderer: Apple A19 GPU；后期 `native=METAL` | 已进入原生 Metal 纹理显示；客体各应用的渲染器和各阶段耗时仍需检查 |
| 内存 | 客体 5120 MiB，`memory-backend-file,share=on,prealloc=off` | 进程 jetsam 余量约 2.3 GiB；缺页与文件回写尚未采样 |
| 启动 | QEMU 约 43 秒启动，约 410 秒报告 `boot_completed` | 两者差值约 367 秒，包含完整冷启动 |
| 帧数 | 每五秒统计显示回调次数 | 缺少用户操作时间标记，静止画面样本无法衡量连续滑动 |

日志中的 HyperOS 配置问题：

- 客体只有 CPU 0–3，部分 cpuset 配置使用 CPU 4–7，写入失败。
- `dynbpfloader` 查询缺失的厂商内核接口，出现 231 条 `BpfMonitor` 记录。
- 多个厂商内存管理和调度接口缺失。

这些记录可以确定兼容配置存在问题。各进程的 CPU 占用和重复查询耗时还需要
采样，随后决定停用或适配哪些服务。

### P1 实机结果与测量边界

原始记录为 `work/performance/P1-cpu-husk.log`，时间与温度摘要为
`work/performance/P1-cpu-summary.json`。以下时间均从 Husk 本次启动开始计算。

| 项目 | 本次结果 | 对分析的影响 |
| --- | --- | --- |
| JIT | 执行自检成功，返回 42 | 继续检查 TCG 执行开销 |
| 图形配置 | `DISPLAY MODE: software (CPU framebuffer)`；`virtio-gpu-pci`；客体触发 `ro.vendor.graphics=swiftshader` | 本轮图形配置与旧 GPU 构建不同，须先恢复并确认 GPU 路径 |
| 温度 | 14.175 秒为 nominal，140.298 秒为 fair，453.647 秒为 serious；最后 987.994 秒仍为 serious | 持续运行性能受到温度状态影响；未测得具体频率或降速比例 |
| 采样停顿 | 一次采样间隔为 75.874 秒，恢复后四个客体 CPU 同时报 69 秒 soft lockup | 用户确认当时手机自动锁屏；该时段须从有效运行测量中排除 |
| Android 启动 | 496.450 秒收到 `boot_completed` | 冷启动含锁屏停顿，缺少与旧构建比较所需的控制条件 |
| Metal 统计 | 显示提交相关计数均为 0 | CPU 模式未进入 Metal presenter，尚无 drawable 等待耗时可供判断 |

旧日志同时触发 `ro.vendor.graphics=mesa && ro.vendor.graphics.gpu=virtio_gpu`，
并进入 ANGLE Metal 与原生纹理显示；当前日志明确选择 SwiftShader。
后续 GPU 测试还需检查客体 EGL/Vulkan renderer、HWUI 和 SurfaceFlinger 的
实际配置，排查个别渲染步骤回退到软件实现的情况。

Apple 对 `serious` 的定义包含会降低性能的温度控制措施。因此，需要比较
启动后的交互和持续运行后的交互。手机外壳温度、CPU 频率和各线程耗时尚无
测量结果，当前只能确认系统报告的温度状态。

日志没有连续操作的时间标记，显示更新次数包含静止时段。尚未取得可用的
实机 CPU profile；ADB 和客体命令服务本轮也未成功返回。各组件耗时占比、
ART 编译状态和实际滑动帧率仍待测量。

## 主要提速路径与投入顺序

优先减少由 TCG 执行的工作量，同时降低持续运行的功耗。下表中的收益条件
用于决定是否继续投入；目前各项都没有同配置的实机提速结果。

| 路径 | 收益来源与条件 | 实现范围 | 优先级 |
| --- | --- | --- | --- |
| 恢复并核对 GPU 绘制 | 将软件光栅化工作交给 A19 GPU；当前 SwiftShader 配置值得优先对照 | Husk 参数、客体图形配置、渲染器检查 | 最高 |
| HyperOS 虚拟机适配与 ART 预编译 | 减少无效后台工作、解释执行和运行时编译；收益取决于实际占用与编译状态 | init、cpuset、相关服务、系统与应用编译产物 | 高 |
| 2/3/4 vCPU 与持续功耗对照 | 减少争用、同步和发热；核数减少也可能降低吞吐 | QEMU 配置、客体 cpuset、宿主线程采样 | 高，先做小范围实验 |
| 图形命令与合成转交宿主 | 减少客体图形驱动和合成的 CPU 工作；适合图形占用较高的情况 | gfxstream/rutabaga 或专用后端、Android 驱动、iOS Metal | 高投入候选 |
| 针对 ARM64 的系统模拟优化 | 减少软件 MMU、辅助函数、翻译块调度、代码失效和跨核同步开销 | QEMU TCG，必要时配合客体内核修改 | 高投入候选，先定位热点 |
| 硬件虚拟化 | 大量客体指令可直接执行，减少 CPU 系统模拟 | iOS 系统权限与虚拟化接口 | 当前设备与安装方式缺少可用路径 |

旧 GPU 构建也有卡顿反馈，恢复 GPU 后仍需继续检查 CPU 工作。仅修改最后
一段显示提交，收益取决于该步骤占总延迟的比例；当前测量不足以优先投入
显示线程重构。

## 优化顺序与判定方法

### 0. 建立可复现基线

用可识别的测试构建补充运行参数和耗时统计。
每轮记录构建标识、镜像版本、CPU、RAM、渲染模式、温度状态与测试开始时间。

固定操作为进入桌面、连续左右滑动、打开设置、上下滚动设置列表、返回桌面并
再次进入设置。记录冷启动和热启动耗时、帧耗时分布及触摸反馈，至少重复三轮。
手机前台保持 Husk，按同一操作顺序比较。

宿主侧测量 drawable 等待、Metal 编码与提交、显示回调、内存占用和温度状态。
客体侧检查进程 CPU、ART 编译状态、FrameTimeline、缺页及磁盘等待。统计数据
以短周期汇总，避免逐帧日志干扰测试。

### 1. GPU 绘制与显示同步

先固定 GPU 模式，并在同一构建中核对 QEMU 设备、客体 renderer 与宿主
ANGLE Metal。检查 HWUI、RenderEngine、gralloc 和合成配置是否一致。
当前 CPU 模式经过 SwiftShader，相关 CPU 工作又由 TCG 执行，可能同时增加
绘制时间和发热；具体占比待采样。

`HuskMetalPresenter.present` 当前在 QEMU 回调线程内持锁调用
`CAMetalLayer.nextDrawable()`。Apple 文档说明，drawable 耗尽时该调用会等待。
当前代码存在阻塞条件，实际等待时长尚未测量。

先测量各阶段。若显示等待占用显著，再将显示调度与 QEMU 回调分开，限制待显示
帧数量，并处理纹理生命周期、ANGLE 与 Metal 命令队列的完成顺序。检查连续
滑动、旋转、前后台切换和停止虚拟机时的资源释放。

保留条件：同一操作的延迟或帧耗时改善，且画面、输入和生命周期检查成功。

### 2. HyperOS 服务和 ART

确认具体后台进程的占用，逐项修正 cpuset 和持续查询无效厂商接口的服务。
保留桌面、通知、设置、安装和应用切换功能。

检查桌面、SystemUI 和设置的 ART 编译状态，根据现有 profile 选择
`speed-profile` 或 `speed` 做独立实验。记录编译耗时、产物容量、启动和交互
变化。检查混合系统镜像的预编译产物是否被 ART 接受；依赖或 boot classpath
变化可能使产物失效。Boot classpath、system server 和普通应用各有编译配置，
按对象处理。预编译生成的 ARM64 代码仍由 TCG 执行。

虚拟机适配保留完整 Android 内核与 framework。先处理占用明显且依赖缺失
硬件的后台任务；每项服务修改都检查通知、安装、桌面与设置。减少模糊、
动态效果等属于可选的界面取舍，单独测量和记录功能变化。

保留条件：减少持续后台开销或改善固定应用操作，核心系统功能检查成功。

### 3. TCG 与 CPU 调度

在相同镜像和显示配置下比较 2、3、4 个 vCPU；记录吞吐、响应延迟和温度变化。
Apple 公布 A19 有 2 个性能核心和 4 个能效核心。4 个 vCPU 之外，宿主还需
运行图形、显示与 iOS 线程，因此先比较 2 个与 4 个 vCPU 有实际意义。
线程由 iOS 调度，2 个 vCPU 能否持续运行于性能核心尚无保证。

先修正客体 cpuset 以匹配测试核数。分别比较相同操作的初始状态和运行一段
时间后的状态；目标为持续交互延迟与功耗。限制客体刷新率可作为后续实验，
须确认它减少了客体绘制工作；仅限制宿主最终显示次数可能继续消耗相同 CPU。

检查翻译缓存实际容量、清空次数、软件 MMU、原子操作及跨核同步占用，再对
主要耗时修改 QEMU。CPU 功能调整需要重新验证内核、ART 和应用兼容性。

TCG 已启用多线程。ARM64 客体与 ARM64 宿主仍需处理客体地址转换、特权状态
和异常。深度修改从频繁执行的访存路径、辅助函数调用、翻译块跳转及失效
处理入手。扩大缓存仅在清空和重译占用较高时有明确理由；当前 256 MiB 参数
与 512 MiB 映射的关系需测量，尚未确认它导致卡顿。

专用 ARM64 Direct 翻译器已能在完整 Linux 与 HyperOS 中运行，覆盖范围、
回退和局部测量见开发进展。内核 Direct 的系统寄存器测试仍明显慢于 TCG，
保持默认关闭。下一步按内核、用户态和缓存管理的实际耗时选择修改对象，继续
检查页表、权限、异常、原子操作和自修改代码的语义。

保留条件：相同操作改善，重复运行成功；缓存大小调整须同时记录内存代价。

### 4. 内存与磁盘

比较 3、4、5 GiB 客体内存，检查宿主缺页与回写、Android 回收和应用重启。
比较压缩与非压缩 qcow2 的冷启动及应用首次启动；保持文件内容和持久状态一致。

保留条件：启动或交互改善，磁盘容量代价可接受，持续运行期间内存稳定。

### 5. 图形协议与驱动

GPU 模式的链路为 Android Mesa/virgl、宿主 virglrenderer、ANGLE、Metal。先确定
图形命令处理、资源传输和同步是否占据主要耗时，再验证 host blob 等资源路径。
旧 GPU 日志已经记录原生 Metal 纹理显示；最后显示步骤已有纹理传递，后续
要测量前面各段的命令处理、资源拷贝和同步。

gfxstream 可将 GLES/Vulkan 调用传给宿主。QEMU 当前文档将 Android 的
`x-gfxstream-gles` 和 `x-gfxstream-composer` 标为实验性功能。候选设计是在
手机进程内处理图形命令，并使用宿主 GPU 完成绘制或图层合成，减少客体侧
图形驱动与合成工作。

需要适配 Android EGL/Vulkan、gralloc/Hardware Composer、QEMU 后端、
iOS 渲染实现及资源共享与 fence 同步。当前 Husk QEMU 10.0.12-utm 的编译
内容与依赖版本须单独核对；QEMU master 文档中的功能不能直接视为本机动态库
已有的功能。Linux 的 host blob 示例也需要针对 iOS 实现内存和纹理映射。
按可启动、可显示、功能检查、性能对照的顺序推进。

保留条件：在同一系统操作上有实际改善，且图形与输入功能检查成功。

### 6. 硬件虚拟化的当前限制

本机 iPhoneOS SDK 未提供公开的 Hypervisor.framework，MacOSX SDK 提供该
框架；Husk 的 iOS 构建使用 `--disable-hvf`。UTM 官方安装文档也将普通侧载
版本的 Hypervisor 支持列为 No，特殊安装路径另有设备和系统限制。

当前 iPhone 17、iOS 27.2、iLoader 安装环境没有已验证可用的硬件虚拟化路径。
修改 Husk 或 QEMU 源码无法自行授予应用所需的系统权限。若未来系统接口和
权限条件改变，应重新评估；当前工作按 TCG 系统模拟继续规划。

### 暂缓的参数调整

分辨率已为 360×800，继续降低会损失可读性。日志未显示接近 jetsam 限额，
增加客体 RAM 缺少依据；缺页和回收问题需要另外测量。非压缩磁盘、编译器
LTO/PGO、日志裁减可按热点安排，当前数据不足以把它们列为大幅提速的主路径。

## 构建、安装和回退

手机使用 iLoader 2.3.6 的导入 IPA 功能安装。2026-10-07 本机查询到的有效 iOS
签名身份数量为 0；测试 IPA 由 iLoader 签名。覆盖安装前核对应用标识和容器，
保留原磁盘及 JIT 配对资料。

HyperOS 初始化镜像位于
`work/hyperos3-import/`。新的构建、采样与中间文件使用当前项目的 `work/`。

每项实验记录：修改、控制变量、检查方法、原始结果路径、结论和回退方式。
性能退化或功能失败的修改先回退，再进行下一项实验。

## 实验记录

| 编号 | 项目 | 状态 | 结果 |
| --- | --- | --- | --- |
| P0 | 当前运行与历史日志核对 | 已完成 | 旧构建启用 JIT、Mesa/virtio 与宿主 Metal；客体为 5 GiB、4 vCPU |
| P1 | 耗时统计与固定操作基线 | 已采到部分数据 | build 30 实际为 SwiftShader，进入 serious 温度状态；含自动锁屏停顿，固定操作基线未完成 |
| P1.1 | 前台保持唤醒与诊断入口 | 已构建，未安装 | build 31 实机行为待验证；串口诊断入口仍需核对客体控制台设备 |
| P2 | 显示同步优化 | 待验证 | |
| P3 | HyperOS 服务和 ART | 待验证 | |
| P4 | TCG 与 CPU | 待验证 | |
| P5 | 内存与磁盘 | 待验证 | |
| P6 | 图形协议与驱动 | 待验证 | |

### P1：显示耗时统计与导入保护

测试包为 `work/performance/ipas/Husk-P1.ipa`，0.8.1 build 30，实验标识
`P1-display-metrics`。IPA 的 SHA-256 为
`eadfac296e1096922bb60b34de44b368da709267ff4f56a7caa6870db5fa3496`。

应用源码基于本仓库 `82e50bd`，QEMU 和 ANGLE 复用官方 0.8.0 IPA 的动态库。
依赖来源和摘要记录于 `work/performance/build-inputs/provenance.json`。本次采用
这个构建方式，是为了先测量应用层显示耗时；修改 QEMU 时再构建对应动态库。
手机先前使用 build 27，本次 build 30 包含两个版本间已有的上游应用层改动。
后续性能对照以 build 30 实测结果为基线。

每约五秒输出一组 `perf` 日志：

- `display updates`：QEMU 显示更新次数；速率使用实际采样间隔计算。
- `callbacks/submitted/completed/displayed`：Metal 显示回调、命令提交、GPU 完成
  回调和画面显示回调的次数。完成与显示可能发生在下一采样窗口。
- `lock/drawable/encode/callback`：等待显示锁、等待 drawable、编码与提交、整个
  显示回调的耗时。
- `interval`：相邻显示回调间隔，包含静止时段。
- `gpu`：最终画面提交命令的 GPU 执行耗时，覆盖范围仅为宿主显示步骤。
- `completion`：提交至 Metal 完成回调的耗时。
- `displayLatency`：QEMU 显示回调开始至 drawable 显示的耗时。
- `thermal`：`ProcessInfo.thermalState` 原始值，0/1/2/3 对应
  nominal/fair/serious/critical。

耗时单位为毫秒。`p95<=` 是固定直方图区间的上界；`avg`、`max` 为实际样本
统计值。累计数据使用固定数量的桶，运行时间增长时内存用量保持不变。

导入工具生成 `husk-custom-guest`，安装时放入手机 Husk 的 Documents 目录。
标记存在时保留导入的系统盘、数据盘及版本标记，校验失败时保留文件并提示
恢复。系统盘和数据盘须一起恢复；当前实例已清理原有官方快照标记。
首次写入标记或更换镜像前退出 Husk。

验证命令：

```sh
bash tests/performance/run.sh
DD="$PWD/work/performance/derived-data" bash scripts/package_ipa.sh \
  "$PWD/work/performance/ipas/Husk-P1.ipa"
```

Release 构建成功，IPA 内应用与嵌入组件的 ad hoc 签名校验成功。测试覆盖了
统计窗口切换、毫秒换算、长时间阻塞、无效计时值、4000 次并发完成回调，
以及导入镜像的旧版本标记、数据盘缺失和损坏系统盘保留。实机 JIT 自检与
Android 启动成功；连续滑动的定量对照、旋转和前后台切换仍待验证。

复杂度审查：保留固定直方图和单个共享统计对象，因为 QEMU、Metal 完成回调
与采样线程需要汇总同一组数据。现阶段沿用同步纹理提交流程；实际阻塞时间
确定后再改显示调度。导入保护通过一个文件标记生效，官方镜像保留原有管理
行为。本次已取得部分实机日志，性能改善尚未验证。

本轮复盘调整：先确认 GPU 和持续 CPU 负载，再决定是否改显示调度；已有
显示锁与 drawable 计时继续保留。build 31 增加运行时保持唤醒和生命周期日志，
用于识别锁屏导致的整段暂停。诊断入口尚未在实机验证，继续使用前须确认
`ttyAMA0` 与客体 `hvc0` 控制台的对应关系。本轮复盘仅更新分析和验证顺序。

## 资料

- [QEMU TCG 与软件 MMU](https://www.qemu.org/docs/master/devel/tcg.html)
- [QEMU 多线程 TCG](https://www.qemu.org/docs/master/devel/multi-thread-tcg.html)
- [Apple nextDrawable](https://developer.apple.com/documentation/quartzcore/cametallayer/nextdrawable())
- [Android ART 配置](https://source.android.com/docs/core/runtime/configure)
- [QEMU VirtIO GPU 后端](https://www.qemu.org/docs/master/system/devices/virtio/virtio-gpu.html)
- [Google gfxstream](https://github.com/google/gfxstream)
- [Apple serious 温度状态](https://developer.apple.com/documentation/foundation/processinfo/thermalstate-swift.enum/serious)
- [Apple iPhone 17 技术规格](https://www.apple.com/iphone-17/specs/)
- [Apple Hypervisor](https://developer.apple.com/documentation/hypervisor)
- [UTM iOS 安装与虚拟化限制](https://docs.getutm.app/installation/ios/)
