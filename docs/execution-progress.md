# HyperOS 执行与图形后端开发进展

更新日期：2026-10-08。

## 目标与当前决策

在 iPhone 内运行完整 HyperOS 3，大幅改善持续交互速度和发热。技术背景见
[执行架构调研](hyperos3-execution-research.md)。用户允许修改 Husk、执行后端、
客体内核和图形实现。开发与功能验证集中在 Mac，形成有实际收益的阶段产物后
再安排手机测试。E2 iOS 试用发现 SELinux 编译阶段长时间停顿；E3 已在 Mac
复现并修正这处退化，扩大整数和访存指令覆盖。完整 Android 启动已通过，
E3 手机日志和用户体验确认当前交互速度仍未达到基础可用性。
E4/E5 已扩大执行覆盖，浮点、向量和跨页调用有微基准收益，完整启动未测到
稳定提升。内核 Direct 实验继续默认关闭。

用户明确指出约 1.5 倍的局部提升不足以满足目标。E1 的整数测试约快 1.7 倍，
短访存测试存在波动。E2 已加入直接执行后端，当前整数循环约快 3 倍，固定
地址的访存循环与 TCG 接近；完整 HyperOS 的性能尚待验证。G1 已实现 gfxstream
GLES → ANGLE Metal，并完成 Mac 上完整 HyperOS 的驱动和绘制验证；build 36
及客体驱动更新包已生成，手机端尚未验证。E6 的 Linux 间接调用测试约快 7.1 倍，
完整 HyperOS 的结束截图已显示桌面，但 1,000 秒内的启动查询未成功，实际启动
耗时未知。M2/M3 已接入共享 RAM 映射、异常恢复、跨片段映射复用和 QEMU
失效通知，完整 Linux 验证通过；完整 HyperOS 冷启动仍需约 16 分钟。M4 的真实
Bionic 对照显示当前镜像已使用 MOPS，全局替换 libc 未获收益。M5 已实现普通
间接调用入口的原生缓存与准确异常恢复，Linux 微基准通过，完整 HyperOS 正在
检查。ARM64 User-Mode Linux 的 Darwin 移植列为更高投入的候选。收益判断采用完整 HyperOS 的桌面、设置、应用切换和持续运行表现。

## 阶段 E1：已完成的本机原型

状态：单 vCPU、整数指令子集的完整 Linux 功能验证通过。后端尚未接入 iOS
构建，也未用于 HyperOS 操作测试。

Dynarmic 固定提交为 `a46601580d5512d324104f985b5f0209dc980ddc`，
QEMU 使用项目现有的 10.0.12-utm 源码。内核、系统调用与未接管的指令继续
由 QEMU 执行；符合条件的 EL0 指令进入新后端。`HUSK_ARM64_EXEC=1` 显式
启用，要求单 vCPU 和墙钟计时。

实现包括：

- QEMU CPU 执行循环中的切换入口，通用寄存器、SP、PC 和 NZCV 同步。
- 每次进入时校验当前 4 KiB 代码页，代码变化或换页后清理翻译缓存。
- 访存失败时停在原指令，由 QEMU 执行并产生客体异常。
- 256 项访存缓存，区分读取与写入许可，记录完整虚拟页编号以检查哈希冲突。
  第一次访问通过 QEMU 校验整页，后续访问在生成代码中查询。每个执行片段
  结束后，映射在下次进入前失效，覆盖内核期间可能发生的页表变化。
- 写入当前代码页及其物理别名交回 QEMU；带标签、非对齐、MMIO 和被监视的
  访问继续走检查路径。
- 按实际填充的缓存项失效，减少纯计算路径中的清理工作。

本轮修复了 Dynarmic ARM64 的两个内存异常问题：普通访存回调没有检查
`MemoryAbort`，会继续修改寄存器与 PC；异常标记为 32 位，原代码使用
64 位 `LDAR`，在仅满足 4 字节对齐的地址上触发宿主异常。测试已复现这两个
问题并验证修改。寄存器优化新增访存边界，保留异常前必须提交的寄存器和标志。

### 功能验证

独立后端测试通过：计算与条件标志、读写、失败读写的准确 PC、写回地址、
代码修改、地址空间更换、执行预算、指令回退、映射权限更新、缓存哈希冲突。

完整 Linux 客体测试在 TCG 和混合后端均通过：系统调用、fork/COW、父子进程
隔离、写保护引发 SIGSEGV、跨页读取引发 SIGSEGV、修改可执行代码后执行新值。
每轮 QEMU 使用可见 Cocoa 窗口，客体测试结束后关机。

### 性能对照

同一 Mac、单 vCPU，每组对照使用同一 QEMU 二进制，按 TCG/混合后端交替运行三轮。
表格为客体计时中位数；整数循环 1 亿次，访存循环对同一位置读、加一、写回
200 万次。内存循环较短，调度和缓存状态会影响结果。

| 实现 | 整数 TCG / 混合后端 | 访存 TCG / 混合后端 | 判断 |
| --- | --- | --- | --- |
| 每次访存调用 C++ 回调 | 93.838 / 63.434 ms | 2.355 / 29.581 ms | 整数约快 1.48 倍；访存慢约 12.6 倍 |
| 访存缓存，每片段清空全部缓存项 | 94.303 / 129.561 ms | 2.280 / 3.046 ms | 访存回退缩小；清理成本导致计算退化 |
| 访存缓存，仅清理已填充项 | 93.944 / 55.073 ms | 2.428 / 2.467 ms | 整数约快 1.71 倍；访存接近 TCG |

相同汇编循环通过 macOS 原生程序运行，三次中位数为整数 22.470 ms、访存
1.753 ms。这提供本机执行参照，包含不同的调度与计时环境，无法用于预测
HyperOS 或 iPhone 的提速倍数。

原始结果分别位于 `work/performance/arm64-linux-callbacks/`、
`work/performance/arm64-linux-cache/`、
`work/performance/arm64-linux-cache-selective/`。
原生参照为 `work/performance/architecture-research/native-bench.jsonl`。
最终构建另做一轮功能回归，记录在 `work/performance/arm64-linux-final/`，
TCG/混合后端均通过。该轮访存为 2.364/3.854 ms，短循环结果存在明显波动，
因此尚不能确认访存性能与 TCG 相当。
首次较短循环的结果保留在 `work/performance/arm64-linux/`，不与扩大后的
循环直接比较。

### 重建与运行

需要 Apple Silicon Mac、Xcode 命令行工具、CMake、Ninja、Boost 头文件、
pkg-config、QEMU 所需的 GLib/Pixman/Slirp，以及构建客体 ELF 的 `ld.lld`。
使用项目现有的 `work/qemu10-src`；可通过 `--qemu-source` 指定其他已准备好的
QEMU 10.0.12-utm 源码目录。构建脚本校验 Dynarmic 提交，按需取得子模块、
检查补丁是否已应用，构建后运行后端测试，并生成输入摘要。

```sh
python3 scripts/build_arm64_exec.py
python3 scripts/build_arm64_test_guest.py
python3 scripts/test_arm64_linux.py --runs 3 --output work/performance/arm64-linux-latest
work/arm64-exec-guest/native-bench
```

完整 Linux 测试复用已有内核
`work/performance/build-inputs/upstream/Payload/Husk.app/vmlinuz-virt`。
构建脚本日志位于 `work/performance/architecture-research/`；输入摘要为
`work/arm64-exec-build/manifest.json`。运行脚本保存命令、串口日志、宿主日志与
结果 JSON；验证失败时返回非零状态。上面的首次环境准备仍依赖项目现有
QEMU 源码与 Linux 内核，脚本不包含这些大文件的下载。

## 阶段 E2：直接执行与循环映射

状态：Mac 上的原型已接入完整 Linux，单后端测试和完整客体测试均通过。
iOS 测试包的集成进展见下文 E2 iOS 阶段。完整 HyperOS 的运行结果仍待手机验证。实现为
`src/arm64-exec/husk-arm64-direct.cpp` 与 `husk-arm64-direct-entry.S`，
复用 E1 的 QEMU 接口和客体权限检查。

### 执行方式与约束

`HUSK_ARM64_EXEC=1 HUSK_ARM64_BACKEND=direct` 启用新后端。寄存器可直接
对应的普通整数指令保留原编码；涉及宿主保留寄存器时改写操作数。客体
`x16`–`x18`、`x26`–`x30` 和 SP 保存在独立状态中，宿主 `x18` 与栈保持有效。
PC 相对地址和跳转重新生成；动态跳转返回 QEMU 重新检查目标执行权限。
系统调用和未覆盖指令通过 QEMU 执行，Linux 内核继续使用 TCG。

数据访问有两种路径：

- 一般访问查询本片段的 256 项页缓存，分别检查读写许可、虚拟页编号、标签
  和对齐。失败时保留原 PC、寄存器和条件标志，由 QEMU 执行客体异常。
- 对最多 32 条指令、以条件跳转结束、基址保持不变、全部访问落在同一页的
  循环，先验证整页所需权限，再把宿主 RAM 指针保存在寄存器中。循环内部
  直接执行访存指令。基址变化、跨页或验证失败时使用一般访问路径。

这个映射原型使用 QEMU 已验证的 RAM 指针，尚未为整个客体地址空间创建
宿主虚拟地址别名。每个执行片段结束后重新检查映射；多核和并发页表修改仍
需要进一步处理。执行预算按代码块检查，属于软上限。当前 QEMU 传入 4096。

`HUSK_ARM64_LOOP_MAP=0` 关闭循环映射优化，供性能消融和正确性对照使用。
两个后端共用 C ABI，未引入后端注册框架或持久化配置。

### 正确性检查

- 独立测试分别运行 Dynarmic、直接执行及关闭循环映射的直接执行路径。
  检查内存错误、地址写回、代码变化、映射变化、哈希冲突和执行预算。
- 16,384 条随机整数指令与 Dynarmic 对照，核对全部通用寄存器、SP、PC 和
  NZCV；另外检查 930 组循环数据寄存器与基址寄存器组合及内存结果。
- 测试覆盖需要重定位的长距离 `TBZ`、`BL`/`RET`、改变基址的循环及跨页
  访问。实现中发现并修复了保留寄存器写回遗漏和生成代码的 `TBNZ` 距离限制。
- 完整 Linux 测试覆盖系统调用、fork/COW、父子进程隔离、写保护信号、跨页
  信号、可执行代码修改，以及两个进程通过管道往返 1000 次。

独立测试共 4 项；上述不同路径均通过。完整 Linux 对照按 TCG、Dynarmic、
直接执行逐次检查、直接执行循环映射的顺序运行三轮，共 12 次，全部通过。
每次 QEMU 使用可见窗口，完成后由客体关机。

### 性能结果

同一 Mac、同一 QEMU 二进制、单 vCPU、TCG 内核。以下为三次中位数，单位 ms。
整数与访存循环各运行 1 亿次；访存反复读取、加一、写入同一地址。系统调用
为 1 万次 `getpid`；进程切换为两个进程通过管道完成 1000 次往返。

| 后端 | 整数循环 | 访存循环 | 系统调用 | 进程往返 |
| --- | ---: | ---: | ---: | ---: |
| TCG | 93.538 | 115.206 | 9.353 | 49.221 |
| Dynarmic 混合执行 | 76.669 | 115.712 | 10.050 | 51.359 |
| 直接执行，逐次检查 | 31.148 | 164.326 | 9.765 | 51.074 |
| 直接执行，循环映射 | 31.178 | 110.750 | 9.851 | 49.557 |

直接执行的整数循环约快 3.00 倍。循环映射让该访存测试相对逐次检查约快
1.48 倍，与 TCG 的差距较小。系统调用增加约 5.3% 耗时，进程往返增加约
0.7%；这里只有三次样本，后两项尚不足以确定较小差异的稳定性。

本轮相同汇编循环的 macOS 原生程序中位数为整数 22.452 ms、访存 32.115 ms。
原生程序使用自身进程和栈上的内存，客体使用 Linux 的 mmap 内存，执行环境
不同。这个参照提示访存仍有较大改善空间，无法用于预测手机收益。

原始结果为 `work/performance/arm64-direct-loops/`，含三轮 JSON、串口日志、
宿主日志和原生程序结果。E2 增加了测试程序内容和访存迭代数；E1 的历史数据
保留用于说明当时的实现，性能比较以本轮同一客体程序的四种模式为准。

最终构建的单轮回归功能通过，但直接执行出现较大波动：整数 39.799 ms、
访存 126.737 ms、系统调用 15.042 ms、进程往返 94.703 ms，记录在
`work/performance/arm64-direct-final/`。随后倒转四种模式的执行顺序再测三轮，
全部通过；直接执行中位数为 31.242 / 111.368 / 9.908 / 49.311 ms，对应
TCG 为 93.727 / 113.752 / 9.254 / 48.570 ms，记录在
`work/performance/arm64-direct-reverse/`。两组主要对照的整数中位数都约快
3 倍；那次较差结果的原因尚未确定，系统调用和进程切换的稳定性仍需继续测量。

### 重建与复测

```sh
python3 scripts/build_arm64_exec.py
python3 scripts/build_arm64_test_guest.py
python3 scripts/test_arm64_linux.py --runs 3 --modes tcg hybrid direct-checked direct --output work/performance/arm64-direct-latest
work/arm64-exec-guest/native-bench
```

运行脚本默认比较 TCG、Dynarmic 和直接执行，使用 `--modes` 增加消融对照。
新增的运行摘要保存 QEMU、Linux 内核及 initramfs 的 SHA-256；构建输入摘要
继续保存于 `work/arm64-exec-build/manifest.json`。

### 当前决定与后续方向

保留直接执行后端继续开发。它已在完整 Linux 中验证了用户态执行与内核异常
处理的切换，并降低了该整数循环的耗时。当前覆盖范围和访存结果仍不足以
安排手机性能验收。

下一步优先扩大连续地址访问的快速映射和常用指令覆盖，处理多页代码缓存及
失效，再用真实 Android 进程检查覆盖率。客体内核配合主要用于通知页表、
进程地址空间和代码权限变化，减少反复校验。gfxstream 与宿主合成继续作为
改善图形开销的独立方向。

Mac 原型保留 TCG 内核和 QEMU 权限检查；iOS JIT 集成记录见下文。
这保留了已有的异常处理路径；直接执行的收益和正确性可以单独比较。完整
HyperOS 的交互、持续温度和多核效果仍待验证。

## 尚待解决的主要限制

- 仅接管基线 ARMv8 的部分整数与标量访存指令；SIMD、浮点、成对访存、TLS、
  原子操作和系统指令回到 TCG。E2 的整数覆盖目前小于 E1，Android 的实际
  覆盖比例尚未采集。
- 单代码页策略会在换页时丢弃翻译缓存；复杂程序的开销可能明显高于短循环。
  多页代码缓存需要处理执行权限、跨页跳转和代码失效后才可启用。
- 每次回退只由 TCG 执行一条指令，频繁切换的代价尚未在 Android 中测量。
- 访存映射只在当前执行片段内有效。多 vCPU、迁移、设备热插拔以及并发映射
  变化尚未验证。当前接口要求宿主映射在片段内保持有效。
- E2 已接入 iOS 预分配的 RX/RW 双映射，真机运行仍待验证。Dynarmic 保留为
  Mac 对照实现，当前分配器尚未适配 iOS。
- 完整 Linux 测试仅覆盖本轮所列场景。SIMD、futex、多核和完整 HyperOS 的
  正确性、性能及持续温度仍待验证。

## 下一阶段的优先级

| 优先级 | 方向 | 要先验证的结果 |
| --- | --- | --- |
| 1 | 用户态直接执行与快速映射 | E2 已通过部分整数、异常和进程测试；继续扩大连续地址访问与常用指令覆盖，验证多页代码缓存及 iOS 映射条件 |
| 2 | 修改客体内核以配合执行后端 | 主动通知地址空间、页映射与代码变化，减少扫描、失效和进程切换开销；检查 16 KiB 页与 HyperOS 二进制兼容性 |
| 3 | gfxstream GLES 与宿主合成 | 完成客体驱动、命令传输、缓冲区共享和 Metal 输出；测量客体图形 CPU 与帧耗时 |
| 4 | ARM64 UML / 半虚拟化内核移植 | 在 iOS 条件下替代 Linux 宿主的进程、系统调用拦截与内存接口；先启动小型完整 Linux，再考虑 HyperOS |

路线的来源与限制见调研文档新增的“进一步改变执行方式”。当前 Dynarmic
原型作为功能和耗时对照继续保留。其局部测试结果不足以安排手机性能验收。

## 本轮取舍与复杂度检查

后端通过一个 C ABI 接入 QEMU，生命周期由 ARM CPU 实例持有。内存与异常
规则以 QEMU 的检查为基础。没有新增通用后端注册框架、独立 RPC 服务或持久化
配置。访存缓存为消除实测回调开销而增加，移除后会恢复约 12.6 倍的访存回退；
清理已填充项针对实测的缓存清理退化。异常修复、寄存器优化边界和哈希校验均
有失败场景验证。构建脚本统一复制接入源码，减少手工副本遗漏。

原计划在 E1 后立即准备 iOS 集成；依据本机实测和用户要求，下一阶段优先
验证足以改善整体可用性的执行与图形路径。手机测试推迟到这些路径形成可测
产物后。本轮所有测试在 Mac 完成，手机安装版本未变。

E2 的复杂度检查保留了三项必要部件：宿主寄存器与栈的保存、权限失败时的
准确退出，以及循环映射前的地址不变量检查。去掉任一项会破坏宿主状态或
客体内存语义。关闭循环映射的实测把访存耗时从 110.750 ms 增至 164.326 ms，
因此保留该优化和对应的消融开关。两个路径共用整数发射代码；测试复用原有
Linux 客体，未增加独立运行服务或另一套虚拟机生命周期管理。

## 阶段 E2 iOS：首个手机试用包

用户要求先打包试用，因此本阶段先完成已有整数后端的 iOS 集成，扩大指令覆盖、
多页缓存、客体内核配合和图形改造继续留在后续阶段。首个包的整机收益仍待测量，
单 vCPU、频繁返回 TCG 和代码页重新编译都可能限制实际表现。

版本标识为 `0.8.1 (32)` / `E2-direct-ios`。对已导入的 HyperOS，默认启用
`Settings → Performance → ARM64 Direct (Experimental)`，使用 1 个虚拟 CPU。
关闭该选项并完全退出、重开 Husk 后，改用标准 TCG 和原有的 4 个虚拟 CPU。
CPU 选项在启动时固定，本次运行的快照标识随之确定。导入客体的自动保存已关闭，
切换到实验配置后使用冷启动。

### 代码与构建变化

- JIT 在调试器分离前一次申请 257 MiB，前 256 MiB 供 TCG 使用，末尾 1 MiB
  供 Direct 后端使用。两块区域共享一次预分配，各自限制写入范围；Direct
  通过 RW 地址写代码，通过 RX 地址运行。释放借用区域不会解除整个分配。
- iOS 静态库只编译 Direct 实现和 C ABI，复用已有 QEMU 执行入口。首次创建
  时执行整数循环自检，成功日志为 `backend=direct-ios self-test PASS sum=5050`。
  进入客体执行后另有 `integer EL0 engine active` 日志。
- Direct 将 `BR` / `BLR` 留给 QEMU，保留 BTYPE 更新与 BTI 落点检查；待检查的
  非零 BTYPE 也交给 QEMU。启用 BTI 的进程可以在检查完成后进入 Direct。
  Dynarmic 在 BTI 启用时继续使用原有的整段回退条件。
- iOS 依赖与 QEMU 已从源码重新构建。构建配置明确禁止在 Mac 上运行 iOS
  探测程序，并只构建应用需要的 QEMU 动态库。ANGLE 使用已有 iOS 动态库；
  libepoxy 与 virglrenderer 从项目固定提交重新编译。

增量构建命令：

```sh
python3 -B scripts/build_arm64_ios.py
bash scripts/integrate_husk.sh
bash scripts/build_ios.sh qemurebuild
DD="$PWD/work/performance/derived-data" bash scripts/package_ipa.sh "$PWD/work/performance/ipas/Husk-E2-Direct.ipa"
```

首次配置 QEMU 时，在前两步后运行
`HUSK_ARM64_EXEC_BUILD=1 bash scripts/build_ios.sh qemu`。
构建需要 `meson`、`ninja` 和 Python `PyYAML`；本机使用
`work/qemu10-src/build-macos/pyvenv/bin` 中的 Meson。iOS 开发依赖安装在
`build/ios-arm64/sysroot`。本机补充的两份 EGL/KHR 头文件来源记录于
`work/performance/architecture-research/ios-angle-headers.json`。

### 已完成的验证

5 项宿主测试全部通过，包括 iOS 包使用的 C ABI、间接跳转保留 PC/LR，以及
原有整数差分和访存边界测试。`tests/performance/run.sh` 的帧统计与导入镜像
保护测试通过；测试仍输出已有的 Swift actor 隔离警告。

完整 Linux 使用与 HyperOS 相同的 `max,pauth-impdef=on,sve=off,sme=off`
CPU 配置，在 TCG 与 Direct 下都通过 fork/COW、写保护、跨页异常、自修改代码、
系统调用、管道往返，以及 BTI 合法落点和违规落点的 SIGILL 检查。
结果在 `work/performance/arm64-ios-package-bti/`。本轮用于功能回归，只有一轮，
不据此更新性能结论。

iOS 动态库的 Mach-O 平台为 IOS，最低版本 16.0，SDK 27.0。库内已确认包含
Direct 自检与 JIT 分区日志，依赖列表未引用 Mac 的 Homebrew 动态库。
手机端双映射执行、完整 HyperOS 启动、交互速度和温度仍待试用。

### 复杂度检查与取舍

保留一个界面开关和两个必要的 JIT 借用函数。移除开关会让用户失去同包切换
TCG 的入口；合并两个 JIT 写入区会让 TCG 与 Direct 覆盖彼此的代码；移除
C ABI 自检会失去安装后确认双映射执行结果的检查点。iOS 构建直接复用原有
代码发射器，未加入第二套执行实现。新测试覆盖 BTI 和 PC/LR 状态边界。

### 交付记录

IPA：`work/performance/ipas/Husk-E2-Direct.ipa`，45,017,764 字节。
SHA-256：`fcc418c6c6c3db02a6bd005cc88f75c12b3f1fe4891d361a98d6668c80915234`。
相邻的 `Husk-E2-Direct.json` 保存版本、摘要和校验结果。

Release 构建成功；IPA 的 ZIP CRC、版本标识、Direct 日志标记均通过检查。
包内 QEMU 与 Husk 的可执行代码段分别与本轮构建产物一致。打包脚本已在压缩前
执行 `codesign --verify --deep --strict`，验证应用及嵌入组件的 ad-hoc 签名。
安装仍由 iLoader / SideStore 使用用户当前账号重新签名，覆盖现有 Husk。

试用时启用 JIT，启动 HyperOS 后比较桌面滑动、打开设置和应用切换。记录发热
变化与是否发生停顿；运行日志用于检查 Direct 自检、执行入口和帧统计。
截至交付，手机尚未安装或运行 build 32。

## E2 iOS 首次真机结果：启动停在 SELinux 编译

2026-10-07 20:21（Asia/Shanghai）通过 `devicectl` 直接读取 F17 上的
`com.husk.app.DD88LFYYNK` 日志。原始记录为
`work/performance/e2-phone-live/`，含 `observation.json`。

已确认 build 32 在手机执行了 Direct 自检，结果为 5050；日志同时出现
`integer EL0 engine active, vCPU=0`。JIT 申请总计 257 MiB，TCG 与 Direct
分别取得自己的分区。QEMU 初始化、内核启动和系统分区挂载都有后续记录。

启动日志在宿主启动后 72.555 秒停于 SELinux 策略编译，最后一行客体时间为
47.635 秒，内容是选择 `plat_sepolicy_genfs_202504.cil`。截至采集，宿主日志已
运行至 582.781 秒，客体日志连续 510.227 秒没有更新。显示更新数持续为 0，
日志采样线程仍约每 5 秒工作一次；最近可用内存约 2.8 GiB，温度状态值为 1。
当前记录未显示内核 panic 或进程退出。

对照 `work/performance/P1-cpu-husk.log`，旧版从 Compiling SELinux 到 Loading
SELinux 的宿主耗时约 3.87 秒。两次运行的后端和 vCPU 数量不同；本次足以确认
启动表现严重退化，具体原因仍需控制变量复现。短整数循环和小型 Linux 测试
未覆盖实际 SELinux 编译工作负载。

尝试对正在运行的 Husk PID 18054 做 15 秒 Time Profiler 采样，Xcode 工具返回
`Timed out waiting for device to boot`。同期 `devicectl` 仍能读取应用日志，
因此该提示仅记录采样连接失败；没有获得可分析的 CPU 调用栈。

接下来先在 Mac 使用同一 SELinux 输入复现，记录代码页编译次数、编译耗时、
Direct 执行片段与 TCG 回退数量，再做同为单 vCPU 的 TCG/Direct 对照。单页
缓存反复重编译、频繁切换和指令语义问题均待排查。新包的整体性能验收暂未通过，
本轮手机日志已收集完成。

## 阶段 E3：代码缓存、执行切换与指令覆盖

### 复现与改动选择

在 Mac 复用 `work/hyperos3-phone-profile/` 的实验磁盘，以单 vCPU、
`max,pauth-impdef=on,sve=off,sme=off`、256 MiB TCG 缓存和可见 Cocoa 窗口
比较 Direct 与 TCG。运行脚本为 `scripts/test_hyperos_boot.py`，各次运行保存
命令、QEMU SHA-256、宿主日志、串口日志和阶段耗时。磁盘复用会保留客体写入，
完整启动比较还受到 Android 缓存与首次启动工作的影响。

E2 的单页缓存每次跨页都重新编译整页。90 秒观察结束时仍未加载 SELinux 策略；
最后一条统计累计调用 7,760,019 次、编译 1,079,129 次，编译耗时 77.018 秒，
后端总耗时 77.979 秒。反复编译占该统计中后端耗时约 98.8%。原始记录：
`work/performance/e3-boot-baseline-direct-cocoa/`。

本轮实现：

- 使用 32 MiB 有容量上限的多页代码缓存。进入缓存页时比较当前代码内容，
  QEMU 检查执行权限；数据页映射在每次执行开始时重新验证。缓存空间不足时
  清空并重新编译，保留地址空间切换、自修改代码和代码别名写入的检查。
- 对同一代码页，首次、第二次尝试进入 Direct 时继续使用 TCG，第三次起才允许编译。
  使用固定 1024 项表记录进入次数，哈希冲突只会延迟编译；代码内容和权限检查
  仍由原来的缓存与 QEMU 完成。该表占用固定空间，减少一次性启动代码的编译。
- Direct 返回 TCG 后，宿主时间经过 2 ms 才再次尝试 Direct。
  TCG 保留代码块连接和中断处理。Direct 修改寄存器或 PC 后清除旧 `last_tb`，
  防止 QEMU 根据已改变的执行状态连接旧块。
- 增加逻辑立即数、位域、提取、字节反转、计数、除法、可变移位、条件选择、
  乘加/乘减、整数成对读写和寄存器偏移访存。非法编码、越页访问和未满足条件
  的指令仍由 QEMU 执行。成对访问先检查两个元素所在范围，再执行宿主成对指令；
  基址写回只在访问成功后发生。
- iOS 预分配调整为 TCG 256 MiB + Direct 32 MiB，共 288 MiB。两者继续共享
  一次 JIT 双映射分配，各自使用独立写入区。build 33 记录 Direct 编译次数、
  编译耗时、执行耗时、缓存页数和清空次数，供后续手机日志分析。

`HUSK_ARM64_STATS=1` 启用统计。`HUSK_ARM64_TCG_QUANTUM_US=0` 可关闭 TCG
连续运行时间，用于消融对照；默认值为 2000。Mac 的指令支持检查现在与 iOS
Direct 一致，Dynarmic 差分参考使用自己的解码过滤器。

### 已测结果与未采用的实现

以下均为单次 SELinux 阶段测量，耗时从日志中的 Compiling 到 Loading 计算，
扫描间隔为 0.25 秒。带统计的不同实现用于定位大幅退化，细小差异还需要重复测量。

| 实现 | SELinux 编译耗时 | 结果目录 |
| --- | ---: | --- |
| 原单页缓存、逐条返回 TCG | 超过 57 秒仍未完成，观察到启动后 90 秒结束 | `e3-boot-baseline-direct-cocoa` |
| 多页缓存、逐条返回 TCG | 91.478 秒 | `e3-boot-cache` |
| 多页缓存、每次最多 64 条 TCG 指令 | 104.039 秒 | `e3-boot-cache-batch64` |
| 恢复 TCG 块连接，立即重新检查 Direct | 42.950 秒 | `e3-boot-chaining` |
| 多页缓存 + TCG 连续运行 2 ms | 3.046 秒 | `e3-boot-quantum` |
| E3 扩大指令覆盖后的 Direct | 3.761 秒 | `e3-full-boot-direct-long` |
| 同阶段纯 TCG 对照 | 3.778 秒 | `e3-full-boot-tcg` |

目录均位于 `work/performance/`。早期纯 TCG 的单次结果为 2.815 秒
（`e3-boot-baseline-tcg`），运行间存在波动。E3 在这一阶段恢复到接近 TCG
的耗时；当前数据尚不能给出超越 TCG 的整体启动增益。

64 条指令分段仍导致大量调度和代码页比较。对应 2 秒宿主采样中，CPU 执行线程
有 1,066 个样本，`memcmp` 占 430 个，Mac 指令支持检查中的环境变量读取也占有
较多样本。改为静态读取后端设置，并允许 TCG 建立、复用连续代码块。未保留
64 条分段的配置和执行分支。采样文件为
`work/performance/e3-boot-cache-batch64/sample.txt`。

三轮完整 Linux 测试全部通过，中位耗时如下；这些是局部负载，未测量 Android
桌面响应或 iPhone 温度。

| 负载 | TCG | Direct | 关闭循环映射的 Direct |
| --- | ---: | ---: | ---: |
| 1 亿次整数循环 | 93.927 ms | 32.496 ms | 32.180 ms |
| 1 亿次固定地址访存循环 | 113.416 ms | 108.218 ms | 141.431 ms |
| 1 万次 getpid | 13.021 ms | 13.299 ms | 13.997 ms |
| 1000 次管道往返 | 55.436 ms | 57.231 ms | 57.291 ms |

原始结果：`work/performance/e3-linux/results.json`。功能测试包括 fork/COW、
写保护、跨页访问异常、自修改代码、BTI 合法落点及违规落点的 SIGILL。

加入按执行频次编译后，又执行三轮相同测试，全部通过。结果为
`work/performance/e3-linux-admission/results.json`，中位数如下。整数循环仍有
收益；开始进入 Direct 的时间变晚，耗时比上一版增加约 3 ms。

| 负载 | TCG | Direct | 关闭循环映射的 Direct |
| --- | ---: | ---: | ---: |
| 1 亿次整数循环 | 95.273 ms | 35.404 ms | 34.863 ms |
| 1 亿次固定地址访存循环 | 113.822 ms | 114.591 ms | 138.507 ms |
| 1 万次 getpid | 14.279 ms | 14.008 ms | 14.024 ms |
| 1000 次管道往返 | 60.242 ms | 56.591 ms | 58.288 ms |

带标签直接访存实验增加了对应测试，测试程序重新编译，并在三个后端各执行三轮，
全部通过。下表只比较该实验，结果为 `work/performance/e3-linux-release/results.json`。
普通访存和系统调用仍接近 TCG；短基准存在波动，手机交互收益还需要实际测试。

| 负载 | TCG | Direct | 关闭循环映射的 Direct |
| --- | ---: | ---: | ---: |
| 1 亿次整数循环 | 95.643 ms | 37.472 ms | 34.756 ms |
| 1 亿次固定地址访存循环 | 117.948 ms | 117.742 ms | 119.315 ms |
| 1 亿次带标签地址访存循环 | 117.117 ms | 103.989 ms | 127.885 ms |
| 1 万次 getpid | 14.254 ms | 14.752 ms | 14.077 ms |
| 1000 次管道往返 | 59.943 ms | 61.029 ms | 59.095 ms |

该实验尝试让 QEMU MMU 按客体配置检查高位标签，Direct 保留原始地址作为
缓存键和基址写回值。宿主测试和 Linux 带标签循环、写保护测试通过，但完整
HyperOS 运行 601.699 秒仍未确认启动完成，最后串口记录在客体约 443 秒。
运行波动和指令兼容性问题尚未区分，故当时将实验保存为独立补丁，E3 默认源码恢复到此前完成桌面启动的实现。
原始补丁随 E3 提交保存；E4 已将标签访问纳入后端。后续验证同时采集 CPU
调用栈与客体状态。

默认源码恢复后，Mac QEMU 的 SHA-256 与完成桌面启动的二进制完全一致：
`16495c2c370d45fb8cc0fe790f3a9d183bf832b91bdc39b5bc416b06c4b1b594`。
再次执行三轮 Linux 测试，全部通过，结果在
`work/performance/e3-linux-stable/results.json`。最终默认版本的中位数如下；
带标签地址在这版通过 TCG 执行。

| 负载 | TCG | Direct | 关闭循环映射的 Direct |
| --- | ---: | ---: | ---: |
| 1 亿次整数循环 | 94.082 ms | 34.607 ms | 34.634 ms |
| 1 亿次固定地址访存循环 | 115.355 ms | 115.327 ms | 133.748 ms |
| 1 亿次带标签地址访存循环 | 117.444 ms | 114.720 ms | 114.001 ms |
| 1 万次 getpid | 13.911 ms | 13.911 ms | 13.173 ms |
| 1000 次管道往返 | 57.498 ms | 56.870 ms | 56.547 ms |

5 项宿主测试通过，包括 65,536 条整数差分指令、930 种循环寄存器组合、
成对访问和寄存器偏移访问的差分、成对访问越页和写保护、跨页缓存重访与代码
变化。iOS C ABI 使用同一组宿主测试。帧统计及导入镜像保护测试也通过，仍有
已有的 Swift actor 隔离警告。

### 完整启动与阶段产物

第一轮 Direct 在 240 秒内未观测到 `sys.boot_completed=1`；纯 TCG 在 300 秒
内也未观测到。两者都越过 SELinux 并启动 Android 服务。延长后的 Direct
观察在 536.685 秒读到 `sys.boot_completed=1`，完整 Android 启动通过；图形
缓冲区分配服务也已注册。早期服务未就绪提示属于启动过程，不能据此判断驱动
故障。该轮使用 Mac 软件渲染，尚未验收桌面操作速度和手机的 Metal 路径。

此次长测试位于 `work/performance/e3-full-boot-direct-long/`。最后一条统计中，
Direct 累计编译 59,249 次，清空缓存 146 次，编译耗时 18.791 秒，后端总耗时
19.180 秒。大量短暂经过的代码页仍触发编译，因此随后加入按执行频次编译。
加入按执行频次编译后的完整启动观察位于 `work/performance/e3-full-boot-final/`：
宿主第 407.310 秒首次观测到串口的 `sys.boot_completed=1` 动作，第 513.616 秒
通过网络查询确认该属性。已保存桌面截图。脚本当时用查询时间覆盖了同名事件，
原先的首次时间仍保留在 `e3-full-final.log`；现已将网络查询事件单独记录。

该轮最后统计为编译 40,966 次、清空缓存 100 次、编译耗时 11.679 秒。磁盘
复用、查询间隔和 Android 启动缓存也发生变化，这些结果只用于观察编译行为，
暂不能将整机启动时间变化归因于编译频次控制。
带标签直接访存实验的完整启动记录位于
`work/performance/e3-full-boot-tagged/`。

Mac 验证还修复了 QEMU Cocoa 显示缓冲区的生命周期：Core Graphics 延迟读取
像素时，QEMU 可能已经替换原 surface。显示回调现在在锁内复制像素，由
`CGDataProvider` 保持数据有效。补丁为 `patches/qemu-cocoa-surface-lifetime.patch`，
只用于 Mac 构建。修改后全部启动与 Linux 测试均使用可见窗口，测试结束关闭。

阶段包为 `work/performance/ipas/Husk-E3-Direct.ipa`，版本 `0.8.1 (33)`，
标识 `E3-direct-cache`，45,022,413 字节。SHA-256：
`9b6a5edea581a9cf61714adb41224b7bdf88184832bc56fdfda671d5c94896a9`。
Release 构建、ZIP CRC、版本标识、代码标记、应用与 QEMU 可执行代码段一致性、
ad-hoc 深层严格签名检查均通过。摘要保存在同目录 JSON。后续手机测试结果见 E4 的手机现状。

### 复杂度审查与后续重点

缓存使用一个有容量上限的分配区和标准哈希表，清空策略先采用整体清空。
没有引入 LRU、跨页机器码连接或代码页权限代理。删除多页缓存会恢复已复现的
重复编译；删除内容检查会漏掉代码修改和同地址不同进程的代码；删除 TCG
连续运行时间会恢复频繁切换。保留循环映射的依据是上述消融结果。

本轮优先修复真实启动退化，并扩大可验证的指令范围。原计划中的客体内核配合、
SIMD/浮点直接执行、快速地址映射和 gfxstream 尚未实现。后续先取得完整 Android
启动与固定操作的可比较结果，再按执行占比选择 SIMD/浮点、内存映射或图形改造。
当前默认版本的整数循环约快 2.7 倍，仍不足以验收用户要求的整机可用性目标。
下一轮优先评估普通访存发射代码中的 NZCV 保存/恢复和寄存器复制，再扩展
SIMD/浮点覆盖。访存错误、权限变化和异常地址必须继续交给 QEMU 处理。

## E4：扩大实际执行覆盖

### 手机现状

E3 阶段代码已提交为 `9738ee4`，未推送。2026-10-07 从 F17 读取的日志保存在
`work/performance/e3-phone-review/`，确认运行 `0.8.1 (33)`、`E3-direct-cache`、
1 vCPU、5120 MiB 客体内存。SELinux 编译从客体 1.596 秒到 5.319 秒，约
3.724 秒；这处启动退化已修复。

日志更新至宿主约 580 秒时，画面每 5 秒更新 0–4 次，热状态为 2；这些计数
记录显示缓冲区更新，无法直接换算为动画 FPS。Direct 累计 179,373 次调用、
31,994 次编译、78 次清空代码缓存；总耗时 7.441 秒，其中编译 6.437 秒。
扣除编译约 1.004 秒，仍包含映射与调度。当前统计无法直接计算客体指令覆盖率。
用户反馈操作远未达到基础可用性，与低画面更新频率一致。

实际图形为软件路径：命令行 `virtio-gpu-pci`，客体 `-virgl`，
`ro.vendor.graphics=swiftshader`，Metal 回调计数为 0。读取的偏好中
`husk.gpuMode=false`、`husk.glProven=true`。ANGLE Metal 探测成功说明宿主接口
可用；本次启动继续采用用户选择的软件显示。已修改 GL 初始化：软件模式直接
跳过创建上下文；GPU 模式创建失败会清除探测成功记录，日志明确本次选择。

### 实现

- 保存、恢复全部 V0–V31、FPCR、FPSR，扩展到常见 SIMD 整数、FP32/FP64 运算、
  转换和向量访存。宿主 D8–D15 与浮点环境在退出时恢复。FP16、未覆盖的指令
  和受限浮点模式继续由 QEMU 执行。C++ 与汇编共用状态布局常量，编译时校验偏移。
- 增加整数扩展加减、带进位运算、条件比较、长乘法和高半乘法、有符号加载、
  LDAR/STLR、TLS 与部分状态读取。保留 CPU 功能、异常、BTI、MTE、SVE/SME
  和单 vCPU 条件检查。FP/SIMD 访问受限时，整数指令可以继续执行；到达受限
  指令才回到 QEMU 产生异常。代码缓存按访问许可区分版本，许可恢复时可复用已有代码。
- 访存检查使用不修改 NZCV 的指令，减少标志保存与寄存器复制。QEMU 校验
  带标签地址，Direct 保留标签用于缓存键和地址写回；普通非对齐访问须由
  客体 SCTLR 允许且完整落在同一页。SP、LDAR/STLR 继续执行各自的对齐检查。
- 同页 RET 通过页内入口表继续执行，并检查预算；跨代码页跳转有独立退出原因，
  重新校验执行权限后可继续 Direct。TCG 的
  间接块连接每 64 次检查一次 Direct 调度，观察到内核返回时也检查。
- 通过实验开关 `HUSK_ARM64_TCG_BRIDGE=1`，可让指针认证、BTI、间接跳转和
  屏障在 QEMU 执行一个指令后重试 Direct。该设置默认关闭：完整 Android 中
  频繁切换增加了开销。默认保留 2 ms TCG 连续运行时间。
- 新增指针认证回调，调用 QEMU 原有密钥、认证和异常处理。回调执行前同步
  完整客体状态，异常使用当前客体 PC；成功后同页 Direct 继续执行。已经通过
  入口检查的块内 BTI 提示也可继续执行。该回调只处理已列出的指令，保持代码页
  和内存映射有效。
- 统计增加跨页退出、预算退出、认证回调和高频回退指令，供完整系统采样使用。

### 本地验证边界

6 项宿主测试通过，覆盖 65,536 条随机整数指令、930 种循环寄存器组合、
131,072 条 SIMD/浮点指令、2,048 种向量访存组合，以及标签、非对齐、越页、
写保护、代码变化、状态保存和宿主浮点环境。

差分参考 Dynarmic 有两处限制：64 位无符号向量比较会进入其未实现分支；
双通道 FP32 除法会对未使用的高通道计算 `0/0`，额外设置异常标志。两项改用
独立 Linux 客体测试检查返回值与 FPSR。参考测试保留其余受支持组合。

首轮完整 HyperOS 运行 600.448 秒仍未确认 `sys.boot_completed=1`，日志继续推进。
路径 `work/performance/e4-full-boot-first/`，二进制 SHA-256 为
`048e3250b0cdff21c332b6bc1228f4e25d9694a1b24d138dac832feaec496f90`。
该轮高频回退为 AUTIASP、BR 和屏障，因此随后增加调度和认证回调。

已完成的调度实验 `e4-full-boot-memory` 在 900.191 秒内未确认启动完成。
最后统计为 Direct 调用 145,340,914 次、编译 276,505 次、缓存清空 591 次；
编译耗时 89.787 秒。该轮开启短 TCG 切换，尚未加入认证回调和页内返回。
这项结果促使短切换改为显式实验选项，默认采用连续 TCG 与认证回调。

加入认证回调与页内返回后，`e4-linux-local-return` 的三轮 TCG/Direct/
关闭循环映射对照共 9 次通过，包括 BTI、有效认证和认证失败到 Linux 信号。
日志确认执行过认证回调。该轮在调整 FP 访问检查之前，计时中位数如下。

| 负载 | TCG | Direct | 关闭循环映射的 Direct |
| --- | ---: | ---: | ---: |
| 整数循环 | 98.866 ms | 39.245 ms | 38.834 ms |
| 固定地址访存 | 124.760 ms | 128.613 ms | 128.470 ms |
| 带标签访存 | 129.913 ms | 128.458 ms | 137.436 ms |
| getpid | 15.438 ms | 15.406 ms | 14.706 ms |
| 管道往返 | 68.787 ms | 64.547 ms | 62.264 ms |
| 向量整数 | 12.215 ms | 8.352 ms | 8.246 ms |
| 浮点加法 | 56.965 ms | 9.590 ms | 8.760 ms |
| 向量访存 | 62.372 ms | 15.116 ms | 14.352 ms |
| 跨页函数调用 | 1.470 ms | 1.675 ms | 1.602 ms |
| 认证循环 | 20.074 ms | 20.928 ms | 21.040 ms |

浮点与向量访存的局部收益清楚；普通访存仍未改善。较早的个别轮次曾出现普通
访存约 72 ms，后续未稳定复现，暂不将其列为已确认收益。系统调用、函数调用
和认证循环也未体现显著改善。该表无法推导手机交互速度或温度变化。

Linux 客体的 PAC 检查使用 `prctl(PR_PAC_GET_ENABLED_KEYS)`，常量 61 来自
[Linux UAPI 头文件](https://github.com/torvalds/linux/blob/master/include/uapi/linux/prctl.h)。
测试会分别记录平台支持、认证回调是否执行及错误认证的信号结果。

完整系统对照 `e4-full-boot-assist` 随后在客体约 519 秒停止推进。CPU 采样
持续停在同一生成代码地址，LLDB 显示该地址出现执行权限 `EXC_BAD_ACCESS`。
源码核对发现 QEMU 的 `tb_add_jump`、`tb_gen_code` 可让当前线程进入 MAP_JIT
写模式；Direct 命中缓存时直接执行，缺少恢复执行模式的步骤。现在 Mac 的
每次原生入口都恢复执行权限，包括 MMU 回调返回后的重新进入。回归测试主动
在缓存命中前和映射回调内切到写模式，验证 Direct 仍完成读取。iOS 沿用 RX/RW
双映射，其入口不调用这项 Mac API。

`e4-full-boot-assist/results.json` 的 758.444 秒结束值发生在 LLDB 附加期间，
不能作为正常退出或启动计时结果。完整记录保留在同目录的 `sample-mid.txt`
和 `lldb-threads.txt`。该故障修复后重新进行整组验证。

最终构建再次通过 6 项宿主测试和 9 次 Linux 客体运行，记录在
`work/performance/e4-linux-final/`。三轮中位数如下；关闭循环映射的对照也已保存。

| 负载 | TCG | Direct |
| --- | ---: | ---: |
| 整数循环 | 97.501 ms | 39.945 ms |
| 固定地址访存 | 119.203 ms | 132.948 ms |
| 带标签访存 | 119.493 ms | 132.166 ms |
| 浮点加法 | 56.552 ms | 9.209 ms |
| 向量访存 | 60.827 ms | 15.647 ms |

该轮普通访存约慢 11%，浮点约快 6.1 倍、向量访存约快 3.9 倍。收益取决于代码
实际进入 Direct 的范围，整机收益需要另行测量。

最终完整 HyperOS 验证已通过：宿主第 587.550 秒观察到串口启动完成动作，
第 588.264 秒通过网络读取 `sys.boot_completed=1`。截图已确认桌面、应用图标
和导航区域完成显示，记录在 `work/performance/e4-full-boot-final/`。QEMU
SHA-256 为 `d494013fc08349691120b8eb51bd88b423ef15d893d64eec68353b110c7b4410`。
测试结束后已关闭窗口与进程。

最后统计为 Direct 调用 5,112,926 次、编译 148,177 次、清空缓存 318 次、
认证回调 11,333,285 次；编译耗时 38.201 秒，后端总耗时 47.287 秒。
缓存清空和编译开销仍高，间接跳转仍是主要回退指令。该轮确认了完整系统
兼容性，尚未测量桌面交互延迟。与 E3 的启动记录相比也未体现加速；磁盘与
系统缓存复用、宿主负载和实现变化同时存在，需固定条件后比较整体性能。

iOS ARM64 静态库、QEMU 动态库和 Release 应用均已构建通过，版本
`0.8.1 (34)`，标识 `E4-simd-assist`。应用内嵌 QEMU 与构建库的 `__text`
SHA-256 均为 `582b219fc67268082ef00291aa23e99d17e1667f7ce00024f752f8f0ee0920b8`。
摘要位于 `work/performance/architecture-research/e4-build-summary.json`。
当前产物为本地构建，尚未安装到手机。

### 后续方向与取舍

当前 SIMD/浮点覆盖是计算路径的阶段改动。整机可用性仍按完整 HyperOS 的
启动、桌面、设置、应用切换和持续发热验收。继续开发按以下顺序处理：

1. **减少执行切换和整页比较。** 真实启动采样出现大量进入/退出与 4 KiB
   `memcmp`。先验证认证回调与调度改动的净收益，再研究连接代码块及接入
   QEMU 代码失效通知。失效通知必须覆盖页表切换、写入代码和物理别名。
2. **快速内存访问与客体内核配合。** 普通内存循环仍未获得明确收益。候选为
   批量验证循环访问范围、保持有效映射和由客体通知地址空间变化。需要验证
   4 KiB 客体页与宿主页的权限差异、Binder 进程切换成本及失效开销。
3. **将图形命令和合成交给宿主。** 当前手机使用 SwiftShader；gfxstream 或
   定制 GLES/HWC 传输可能减少客体绘图工作。需要实现 iOS surface、共享缓冲区、
   同步和客体驱动，再与现有 virgl 路径比较完整操作耗时。
4. **客体内核的宿主用户态执行。** 继续保留 ARM64 UML/Darwin 方向，先做
   最小 Linux 进程、异常、调度与内存映射原型；已有 Linux 宿主实现的接口
   需要针对 iOS 重做。平台可行性与 HyperOS 兼容性尚待验证。

### 复杂度审查

SIMD 扩展继续使用现有指令分类与生成器；共享状态偏移消除了 C++/汇编中的
两套常量。指针认证复用 QEMU 实现，回调入口与原有映射回调共用上下文。
取消认证回调会恢复同页认证指令的 TCG 切换；取消调度检查会使长时间的 TCG
间接连接绕过 Direct 入口。两项均提供环境变量用于消融，性能结论按测试结果
保留。页内 RET 入口表复用现有代码页，表项指向已有预算检查；另对每次动态
返回计数，覆盖跳到基本块中间和自循环的情况。此阶段没有加入新的调度框架。

所有 QEMU 验证使用可见窗口，结束后关闭。本轮读取已有手机日志，未安装
新构建或安排新的手机操作。新的执行语义先通过宿主和 Linux 验证，再用于完整
Android；达到可比较的阶段结果后再安排真机验证。

## E5：按需编译与连续用户态执行（进行中，2026-10-08）

目标是在函数和代码页之间保留原生寄存器及已检查的内存映射，减少每次返回
QEMU 时的状态复制、整页比较和重新映射。图形命令传输及 ARM64 UML/Darwin
继续作为独立架构方向评估。用户允许继续扩大改动，本阶段先做本地验证。

已实现按实际入口遍历代码页中的控制流，仅编译可达区域；后续入口增量编译。
BR/BLR 通过 QEMU 回调保留 BTYPE、目标地址标签和 BTI 检查。着陆指令需要
保留异常状态时，由 TCG 执行该指令。新增补丁复用 QEMU 的目标检查函数。

增量编译最初漏掉了条件分支指向已编译区域的情况，Linux 管道测试停在原生
`b.ne` 自循环。已修复，并加入同页多入口、条件跳转和代码修改回归测试。
修复后的 `e5-linux-links` 三种模式全部通过。较早的 `e5-linux-demand` 使用了
构建失败前的 E4 二进制，已在 manifest 标记排除；测试脚本现在要求源码输入
和 QEMU 哈希匹配成功构建的 manifest。

首次完整系统实验 `e5-full-demand` 最后记录编译 55,185 次、清空缓存 10 次、
编译耗时 4.289 秒；Direct 调用仍高达 35,865,625 次。客体约 141 秒时，
`hypsys_system` 的 BootMonitor 请求重启到 recovery，未完成 Android 启动。
此结果只能确认编译开销下降，整体性能结论仍待验证。

跨页代码连接已实现。首次进入目标页检查执行权限，后续通过入口表跳转，
保留原生寄存器及数据映射。每个执行片段开始时清空入口表并重新检查代码；
新代码页加入时撤销此前的数据写映射，再由 QEMU 检查所有已访问代码页的
物理别名。执行权限变化、片段间修改代码和先写后执行的别名测试均通过。

`e5-full-chain` 已启动完整 HyperOS：串口在 690.803 秒记录启动完成，网络
在 695.345 秒确认属性。最后记录 Direct 调用 16,764,460 次、编译 569,633 次、
清空缓存 162 次，编译耗时 44.265 秒、后端总耗时 99.122 秒。与 E4 的
588.264 秒记录相比，本轮更慢。磁盘持续复用，宿主还承担开发工作；这些记录
足以否定当前已有整机明显提速的结论，精确回退幅度仍需固定条件测量。

随后加入 FEAT_LSE 原子操作直接执行，保留客体特性、宿主特性、访问权限、
自然对齐和代码页写保护检查；未满足条件时交回 QEMU。独立测试覆盖四种宽度、
十种操作、内存顺序编码、寄存器重叠与零寄存器，共 10,240 组数值检查，另验
证特性缺失、错位地址和写保护均不改变状态。SVC 之后取消额外的 2 ms TCG
等待，系统调用仍完整进入客体内核。

更新后的 Linux 测试 `e5-linux-lse` 在 TCG、Direct 和关闭循环映射的 Direct
三种模式通过，包含 LSE 写保护引发 Linux SIGSEGV。测试脚本现在自动重建
initramfs，并记录二进制摘要。`e5-linux-atomics` 使用较早的客体程序，未覆盖
新增 LSE 测试；该目录仅保留当时已有检查的结果。

| 负载 | TCG | Direct |
| --- | ---: | ---: |
| 跨页函数调用，1,000 万次 | 136.798 ms | 16.844 ms |
| 浮点加法 | 54.703 ms | 7.328 ms |
| 向量访存 | 60.955 ms | 13.908 ms |
| LSE 原子循环 | 39.826 ms | 64.347 ms |
| PAC 认证循环 | 19.935 ms | 77.351 ms |

跨页调用约快 8.1 倍，原子和认证循环仍有退化。原子操作的价值还需检查其能否
减少实际 Android 负载的回退时间。上述结果来自单轮 Mac 测试，手机交互、温度
和整机启动收益均需另行确认。

### 内核执行实验

`HUSK_ARM64_PROFILE=1` 通过 QEMU 的异常级切换通知与线程 CPU 时钟，分别
统计用户态 TCG、内核 TCG 和 Direct 时间。Direct 时间包含编译和回调；计时
从后端第一次建立上下文开始，未包含此前启动阶段，也不计休眠和宿主调度等待。
`e5-full-lse` 的启动中间采样记录用户态 TCG 93.065 秒、内核 TCG 145.896 秒、
Direct 66.675 秒。内核约占这段 vCPU CPU 时间的 48%，用户态优化受到这一占比
限制。

已接入 `HUSK_ARM64_KERNEL=1` 实验选项，让 EL1 的普通计算、访存和控制流
使用同一后端。每次映射继续使用 QEMU 当前异常级的 MMU 权限。系统寄存器、
异常、MMIO 和未支持指令由 TCG 执行；片段结束后经过现有中断检查。该选项
当前默认关闭。高地址代码连接及 8 项宿主测试通过。修复用户态与内核共用
回退等待时间的问题后，`e5-linux-kernel-quantum` 三种模式均通过，日志确认
内核 Direct 实际执行。完整 HyperOS 测试 `e5-full-kernel` 在 900.237 秒内
未完成启动，尚未测到整机收益。此前开启线程计时的 `e5-full-lse` 同样在
900.13 秒内未完成；最终记录用户态 TCG 372.596 秒、内核 TCG 271.923 秒、
Direct 196.155 秒。计时本身有额外开销，不能直接比较这两次耗时。

### 图形方向

gfxstream 源码已取得固定提交 `07ee40efb0e7037a9a9b7fe59071e7c4997e7cbe`，
已加入 iOS 适配补丁，处理桌面框架、系统 API、EGL 动态加载及离屏 surface。
Rutabaga 固定提交为 `ced6af5b1135173c269a037e06213e8a263de271`。两者的
macOS 动态库、iOS gfxstream 动态库与 Rutabaga 静态库均构建成功；
`scripts/build_gfxstream.py` 记录
源码、依赖锁文件与产物摘要。iOS EGL 使用 Husk 已有的 ANGLE Metal 实现。

`scripts/test_gfxstream.py` 已通过两层测试：宿主 Rutabaga C API，以及
当前 Android 内核的 DRM ioctl → QEMU virtio-gpu → Rutabaga → gfxstream
→ ANGLE Metal。两者均检查 GLES 创建上下文、清屏、裁剪、6,144 个像素读回
及 GL 错误；宿主测试另检查 fence 和销毁。日志确认 `ANGLE Metal Renderer`。
结果保存在 `work/performance/gfxstream-metal-v1`。这是基本渲染正确性验证，
尚未测量 Android UI 性能。

### G1：完整 HyperOS 的 gfxstream GLES

已将 HyperOS AVD 的九个 GLES 编码库加入现有客体。启动检测程序通过 DRM 的
`VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs` 检查 GLES capset 8，再设置 EGL、
minigbm 和 virtio-gpu-pipe。它在既有 `virtgpu_detect` 前执行。其他 GPU 设备
继续使用客体原有的驱动选择，Mesa 库保留在原路径。

`gfxstream-hyperos-cold` 在单 vCPU HVF 下冷启动通过，42.756 秒查询到
`sys.boot_completed=1`。SurfaceFlinger 和独立 EGL 程序均报告
`Android Emulator OpenGL ES Translator (ANGLE ... Metal Renderer: Apple M5 Ultra)`。
桌面图标、导航栏可见。壁纸在 gfxstream 与后续软件渲染对照实例中均为黑色，
其原因仍需检查。HVF 用于驱动兼容性开发，
iPhone 运行时的 QEMU、gfxstream、ANGLE 和 Android 均在手机内执行。

`tests/gfxstream/android-bench.c` 使用 360×800 pbuffer、全屏三角形及每像素
16 次纹理采样。预热后测量 60 帧，每帧 `glFinish`；最后读回一个像素并检查颜色。
上述 HVF 测试得到 5.281 ms/帧。此前 llvmpipe 测试为 29.635 ms/帧，但使用四个
vCPU；两者只能用于功能检查。
此负载用于测量绘制成本，手机桌面帧率、输入延迟及持续发热需要另外测试。

`g1-tcg-gfxstream` 使用单 vCPU、4,096 MiB、标准 TCG，关闭 ARM64 Direct，
完整 HyperOS 在 545.581 秒查询到启动完成。三次 60 帧测试均通过，分别为
10.960、16.169、10.636 ms/帧，中位数 10.960 ms/帧。SurfaceFlinger 与测试
进程均报告 Metal。此前同一 QEMU 和测试二进制的软件组 `g1-tcg-software`
在 692.656 秒启动，renderer 为 ANGLE/SwiftShader；其图形测试超过 180 秒
仍未完成。超时包含 EGL 初始化、着色器编译和绘制，无法由此计算稳定绘制
速度的倍数。`g1-tcg-software-long` 将上限延长到 600 秒，仍然超时，且只输出了
SwiftShader renderer。这轮在 895.172 秒查询到系统启动，测试期间另做了两次
只读进程查询，分别耗时 10.375 和 13.233 秒；主线程等待 futex，一个工作线程
处于可运行状态。现有记录仍不足以区分编译、预热及绘制阶段，后续版本已加入
阶段计时输出。软件组的稳定帧耗时与提速倍数继续保留为未知。

两组复用同一份磁盘，顺序启动，过程中没有其他 QEMU 实例。磁盘缓存与 Android
持久状态没有复位，启动时间只能作为本轮观察。更早的 `gfxstream-baseline-tcg`
测试受到人工 ADB 重连影响，其绘制结果已排除；后续自动测试期间不重连或重启
adbd。测试脚本现已保留超时前输出，便于区分初始化与运行失败。

QEMU 的 iOS 动态库及 Husk build 35（`G1-gfxstream-metal`）均构建成功。
Rutabaga 的 Rust 动态库触发 Xcode 27 链接器的 LINKEDIT 字符串表对齐检查，
当前将其静态链接到 QEMU；gfxstream 和 ANGLE 随应用作为动态库加载。
应用新增 `Metal GLES (Experimental)`，初始关闭，要求先更新客体驱动。
启用后使用 `virtio-gpu-rutabaga-pci`，跳过旧 virgl 的窗口探测，并禁止恢复和保存
该显卡的 VM 状态。当前显示经过 GPU 读回、Pixman 和 Husk 已有显示组件，仍有
复制成本。iPhone 上的初始化、画面与性能尚未验证。当前仅加速 GLES，Vulkan 应用兼容性
未验证。`work/performance/ipas/Husk-G1.ipa` 已通过包内资源、动态库与签名检查。

为检查 iOS 专用代码的运行行为，新增 `macos-ios-api` 构建目标：在 Mac 执行
相同的 EGL 动态加载、无窗口平台实现及 GLES 转发。首次加载暴露了残留的
macOS 视频纹理函数 `_nsConvertVideoFrameToNV12Textures`。已限定该调用只在
桌面后端编译，并去掉 iOS 后端的 `-undefined dynamic_lookup -flat_namespace`，
后续未解析符号会导致链接失败。修复后 iOS 库重新构建成功；
`g1-ios-api` 完成 Metal 渲染、6,144 个像素、fence 和资源销毁检查。
该检查运行于 Mac，仍需验证 iPhone 的实际加载和图形行为。

已检查图形缓冲区的分配入口：当前 `ExternalBlob` 和 `SystemBlob` 均关闭，
ring blob 使用进程内的对齐内存。命名 POSIX 共享内存位于另一条分配路径，
当前 GLES 配置未启用该路径。此项为源码与初始化日志核对；iOS 沙盒中的运行
结果仍待确认。

当时的 build 35 安装包 SHA-256 为
`fd99110e066780f6a5f159e84e959ea21fb07348b56fa00bc995d054cab0ea71`。
该构建的主应用及 JIT helper 构建号均为 35，新增图形依赖的许可证随应用打包。
同一路径现已替换为下文记录的 build 36。

驱动包由 `scripts/prepare_gfxstream_guest.py` 生成，包含文件摘要、检测程序及
安装脚本，压缩后约 600 KB。`scripts/install_gfxstream_guest.py` 通过指定 ADB
连接原地写入 vendor。库文件先写入同目录临时文件，再用 rename 替换，运行中的
进程继续引用旧 inode，重启后加载新库。安装脚本限定当前 SELinux permissive
HyperOS profile，检查既有 init 入口后插入一次检测调用。`g1-app-driver-check`
连续安装两次成功；九个库及检测程序的目标摘要全部匹配，init 调用仅出现一次，
安装临时文件已清理。当前 QEMU 的客体内重启
曾停在 UEFI，后续测试使用关闭进程后重新启动。

### G1 完整界面检查

`g1-ui-gfxstream` 在单核 TCG 下使用 Metal，Settings 冷启动的 Android
`TotalTime` 为 96,042 ms、`WaitTime` 为 97,982 ms。六次 600 ms 滑动后，
`gfxinfo` 在服务内部超过 10 秒查询上限，未取得有效帧统计。截图显示进入了
通知与状态栏子页面，输入负载也未保持为预期列表滑动，因此本轮没有有效的
界面帧率对照。后续采集延长滑动时长和查询上限，仍需检查操作结果。
软件界面对照 `g1-ui-software` 在测量前人工结束，排除其性能结果。

`g1-app-driver-check` 使用同一磁盘、单核 HVF，Settings 冷启动为 5,461 ms。
进程映射显示 Settings 加载 `/vendor/lib64/egl/libEGL_emulation.so` 和
GLES 编码库；`debug.hwui.renderer=skiagl`，`Pipeline=Skia (OpenGL)`。
系统为 `zygote64`，32 位 ABI 列表为空。这些检查排除了 Settings 仍使用
SwiftShader 或 32 位驱动的推测。此时约 1,520 MB 内存可用，未启用交换空间。
HVF 与 TCG 的启动数字包含不同缓存与后台状态，仅说明当前观察到的差异。

加入初始化阶段计时后，同一测试程序、单核 HVF、每组三帧的绘制检查均完成。
软件组 `g1-software-setup` 为 531.938 ms/帧；Metal 组为 5.305 ms/帧。
两组均通过像素检查，但样本短，且宿主使用 HVF；手机收益仍待确认。
此前 TCG 软件组 600 秒超时的具体阶段仍未知。

这些结果确认图形路径已接入宿主 GPU，完整界面仍有很长延迟。下一阶段同时
检查 Direct 与新图形路径的组合、CPU 时间分布和界面等待，继续以实际交互判断
可用性。

### G1 构建和复测

```sh
python3 scripts/build_gfxstream.py --platform ios
python3 scripts/build_arm64_ios.py
PATH="$PWD/work/qemu10-src/build-macos/pyvenv/bin:$PATH" bash scripts/build_ios.sh qemurebuild
```

图形库要求 CMake、Ninja、Rust 与 `aarch64-apple-ios` target。`scripts/ci_build.sh`
也已加入图形库步骤。客体包的构建参数为已有 HyperOS AVD EROFS vendor 分区、
Android NDK 路径及一个新输出目录：

```sh
python3 scripts/prepare_gfxstream_guest.py \
  --vendor work/boot-lab/images/os3-partitions/vendor.img \
  --ndk /opt/homebrew/share/android-commandlinetools/ndk/27.2.12479018 \
  --output work/performance/gfxstream-guest-package
python3 scripts/install_gfxstream_guest.py \
  --serial 127.0.0.1:16255 \
  --package work/performance/gfxstream-guest-package.tar.gz
```

上述 ADB 地址属于 Mac 验证实例；安装到手机时须使用手机客体的 ADB 连接。
更新完成后关闭模拟器，再启用 Metal GLES 并重启 Husk。后续日常运行在手机内完成。

Android 绘制测试程序可使用 Android NDK 的 `aarch64-linux-android26-clang`
构建，源文件为 `tests/gfxstream/android-bench.c`，参数为
`-O2 -Wall -Wextra -Werror -lEGL -lGLESv2`。验证完整 HyperOS 时使用
`scripts/test_gfxstream_hyperos.py --gpu software|gfxstream --accel tcg`，并传入
`--angle-path`、`--bench` 和独立的 `--output` 目录。默认绘制 60 帧、重复三次，
每次上限 180 秒；`--bench-seconds` 可以延长慢速软件渲染的等待时间。

### G1 复杂度审查

- QEMU 使用上游 Rutabaga C API，GLES 编码使用现有 AVD 驱动。未新增一套产品
  图形协议；测试中的少量命令编码用于独立检查传输和像素结果。
- 显示沿用已有 Pixman 回调。删除本轮最初加入的 UIKit 子图层创建与跨线程生命周期
  代码，iOS 的 native-window 必需接口仅拒绝窗口请求。实际调用使用离屏 surface。
- 保留旧 virgl、软件显示及原有 Mesa 库：现有镜像和设置仍使用这些路径，新路径
  的手机验证也尚未完成。
- 内核 Direct 实验继续默认关闭，完整启动尚未测到收益。GPU 改动与该实验独立。
- 新增依赖固定源码提交，Rust 依赖固定锁文件。iOS 构建始终包含 gfxstream，运行
  时只保留一个实验选项；编译开关未再增加第二份可用状态。
- `macos-ios-api` 仅供构建验证，复用 iOS 平台源码；其运行检查已经发现并帮助
  修复一处动态加载问题，因此保留。手机运行使用打包的 iOS 库。


## E6：间接调用继续在 Direct 内执行（2026-10-08）

G1 与 E5 组合的启动采样中，Direct 曾进入 25,192,159 次，其中 24,752,075 次
以 branch 状态返回 QEMU。此前 BR/BLR 的辅助函数总会结束执行段，后续调用
重新复制完整寄存器状态、核对代码页并建立数据映射。函数内的微基准收益未能
覆盖这种频繁调用。

新增 `HUSK_ASSIST_CHAIN` 返回值。QEMU 仍执行目标地址规范化、BTI 和指针认证
检查；无待处理 BTYPE，或 BTI/PAC 入口已完成时，Direct 可继续执行目标代码。
需要保留首条指令异常语义的其他入口仍返回 QEMU。目的页继续检查执行权限，
同一执行段内已验证且保持不变的代码页可复用入口。每次间接调用消耗指令预算，
即使目标落在同一基本块中，也会定期返回 CPU 循环。

同一段执行中的 BLR 目标第一次经过上述检查后，可缓存宿主入口。后续命中直接
由生成代码完成查表、更新 LR 和跳转。缓存只记录没有逐次 PAC 副作用的目标；
认证入口继续执行辅助函数。跨页末尾 BTI、带地址标签的目标等情况保留慢路径。
缓存随执行段结束或代码池清空而失效，目的代码页继续受物理别名写保护。

初版仅延长 C++ 执行循环，Linux 一百万次间接调用仍需 50.503 ms，TCG 为
13.764 ms。完成生成代码内的入口缓存后，三轮 Linux 对照中位数如下。测试
使用单 vCPU、`max,pauth-impdef=on,sve=off,sme=off`，数据见
`work/performance/e6-linux-native-calls/medians.json`。

| 固定负载 | TCG | Direct | TCG 耗时 / Direct 耗时 |
| --- | ---: | ---: | ---: |
| 一百万次间接调用 | 13.271 ms | 1.878 ms | 7.07 |
| 一千万次跨页直接调用 | 130.211 ms | 15.948 ms | 8.16 |
| 浮点循环 | 52.612 ms | 7.232 ms | 7.27 |
| 向量访存循环 | 58.321 ms | 13.256 ms | 4.40 |
| 整数循环 | 94.261 ms | 41.392 ms | 2.28 |
| 通用访存循环 | 114.763 ms | 137.476 ms | 0.83 |
| 系统调用 | 13.779 ms | 16.802 ms | 0.82 |
| 原子操作 | 39.012 ms | 62.104 ms | 0.63 |
| 指针认证 | 20.146 ms | 75.187 ms | 0.27 |

八组宿主测试及六次 Linux 完整功能运行通过。覆盖代码修改后的重新编译、
取消执行权限、代码物理别名写保护、间接自调用的预算退出、BTI 非法目标的
SIGILL 和 PAC 认证失败。`HUSK_ARM64_BRANCH_CHAIN=0` 仅供对照测量；应用界面
未增加开关。间接调用与计算测试的收益不能用于推算完整 HyperOS 速度，访存、
系统调用、原子操作和认证的退化仍需处理。

修改前的 E5 + G1 运行 `g1-direct-ui` 在 1,000 秒内未查询到启动完成，未取得
界面数据。这轮开启了逐段 Direct 统计和 CPU 分阶段计时，并在启动中进行过
宿主采样及本地构建。最终 CPU 分布记录于 `cpu-phases.json`；计时工具本身有
开销，该轮不用于严谨的启动速度比较。后续完整系统性能测试默认关闭这些统计，
通过 `--profile` 单独开启。

复杂度审查：复用现有 assist 和 map_code 所有者，新增两种完成状态和一张
仅在执行段内有效的调用入口表。跨执行段缓存与多 CPU 同步尚未实现，当前
映射仍在每段执行后失效。BTI 与指针认证继续使用 QEMU 原实现。

`g1-e6-art-ui` 使用 E6、Metal GLES 和 A1 预编译产物，关闭 Direct 统计与
分阶段计时，在 1,000 秒内仍未查询到启动完成，未取得设置和滑动数据。
复查退出前的 `screen.ppm`，桌面图标和导航区域已显示。该结果只能说明查询
超时，无法确认实际启动耗时，也不能据此判定启动失败。
本轮 QEMU 摘要为
`c275220a2d0ba8762fb80077ad234546cc6fa7e041691c6955f5147a1c2498c6`。
Linux 微测成功还不足以确认完整 HyperOS 的收益。

build 36（`G1-E6-native-calls`）已完成 iOS 构建、IPA 资源和签名检查，
文件为 `work/performance/ipas/Husk-G1.ipa`，SHA-256 为
`2d1334c7c12b93be47b56591a8b2ef10ab280da632aaca3dc64b132a1482ec61`。
同目录 JSON 记录库摘要、构建标识和手机验证状态。签名检查针对打包脚本的
已签名 staging bundle；DerivedData 中用于生成 IPA 的原始应用没有签名。
新安装默认关闭 ARM64 Direct，既有用户显式保存的选项保持有效。启用 Direct
或 Metal GLES 时使用单 vCPU，TCG 的线程参数与该核数保持一致。

## A1：SystemUI 与设置的 ART 预编译（2026-10-08）

`g1-art-inspection` 查询到 SystemUI 为 `verify`、设置为 `speed-profile`、
桌面为 `speed`。Settings 的进程映射显示 boot/framework OAT 和应用代码均已
加载；系统 boot image 可用。

新增 `scripts/prepare_hyperos_art.py`，逐个检查显式指定的应用。带 `--compile`
时，对尚未达到 `speed` 的应用执行 `pm compile -m speed -f`，再查询 ART 是否
接受生成产物；仅保存属性与执行日志。现有桌面产物无需重编译。SystemUI 和
设置已生成 `speed` 产物，结果记录于 `work/performance/g1-art-speed/`。
这些改动位于 Mac 验证镜像，手机客体尚未更新。`g1-art-cold-adoption` 已在
后续冷启动中确认三个应用仍为 `speed`，没有重新编译。完整界面收益继续验证，
后续将生成与当前手机镜像相匹配的更新方式。

```sh
python3 scripts/prepare_hyperos_art.py --serial 127.0.0.1:16255 \
  --package com.android.systemui --package com.android.settings \
  --package com.miui.home --compile --output work/performance/art-speed-run
```

省略 `--compile` 只查询编译状态。程序复用 Android ART 的编译和产物校验入口，
未另行实现 OAT 文件格式或拷贝活动应用数据。产物随 APK、ART 或 boot classpath
变化可能失效，更新镜像后需要重新核对。

## M1：Darwin 内存映射原型（2026-10-08）

新增独立程序 `tests/arm64-exec/darwin-memory.c`，在自身保留的地址范围内调用
`vm_remap` 和 `vm_protect`。Mac 检查通过：共享物理页的读写一致、两个别名
拥有独立权限、COW、同一地址替换物理页、撤销权限后的访问异常。保护错误和
未映射访问的信号恢复仅在独立 Mac 程序中运行，尚未接入 Husk 的异常处理。

本机宿主页为 16,384 字节；4、16、64、128、512 GiB 的虚拟地址保留均成功，
各次申请后立即释放，没有访问这些保留页。对一个 4 KiB 子区间设置只读后，
整个 16 KiB 宿主页都变为只读。2,000 次逐页重新映射并设置权限的平均耗时为
1,521 ns/页。映射后的原生汇编访存循环一亿次为 29.795 ms，结果正确。
这组数据用于检查宿主接口与映射成本；完整 Linux、HyperOS 和手机收益尚未测得。

源码通过 iOS arm64 编译，iPhone 上的地址保留、数据映射与异常行为尚未运行。
程序使用的临时 RAM 为六个宿主页；测试结束后释放全部映射。
构建及结果位于 `work/performance/m1-darwin-memory/`。

```sh
clang -O2 -Wall -Wextra -Werror -Wno-unused-function \
  tests/arm64-exec/darwin-memory.c \
  -o work/performance/m1-darwin-memory/probe
work/performance/m1-darwin-memory/probe
xcrun --sdk iphoneos clang -target arm64-apple-ios16.4 \
  -O2 -Wall -Wextra -Werror -Wno-unused-function -c \
  tests/arm64-exec/darwin-memory.c \
  -o work/performance/m1-darwin-memory/probe-ios.o
```

另有 `tests/arm64-exec/android-mapping-coverage.c`，只读指定进程的 maps 和
pagemap，统计同一 VMA 内的 16 KiB 对齐虚拟区间中，四个 4 KiB 页是否驻留、
物理连续且按 16 KiB 对齐。输出区分 VMA 边界、未驻留页面和内核隐藏 PFN。
采样不会暂停目标进程；采样期间页表仍可能变化。候选比例只用于选择设计方向，
尚未检查代码写别名、COW 权限和 DMA，一次采样不能用于建立可执行映射。

`m1-android-coverage` 在同一镜像的单核 HVF 实例中运行。五个目标进程的 PFN
均可读取；需要在完成图形检查后将 adbd 切换为 root。初步候选占已采样驻留页的
比例为 system_server 14.68%、SystemUI 13.14%、桌面 18.06%、设置 15.24%、
SurfaceFlinger 10.05%。这组统计按页面数量计算，尚无访存频次权重。
进程可读虚拟地址的最高位置接近 512 GiB，单进程完整线性窗口需要考虑此范围。

客体内核为 `6.12.81-4k-g4f6bf47200d9`，提供
`/sys/kernel/mm/transparent_hugepage/hugepages-16kB/enabled`，当前为 `never`。
Linux 的 mTHP 可以通过连续物理页承载匿名内存，继续使用 4 KiB PTE 和用户接口。
新增 `--allocation-sample` 模式，每次仅申请并写入 16 MiB 匿名内存，然后统计
自身映射。`never` 的三次候选比例为 0.08%、5.32%、8.29%；临时设为 `always`
后为 89.09%、89.51%、84.87%。统计包含该小程序的其他映射，详细数据在
`mthp-allocation.json`。测试后恢复 `never`，未修改持久启动配置。

这一结果支持先尝试 mTHP 与 fastmem 配合：现有内核可以增加连续匿名页，无需
先变更整个镜像的页大小。已有进程、文件映射、共享内存和 COW 后的情况仍需
处理。页面候选比例增加的幅度不能换算成应用提速倍数。
来源：[Linux THP 文档](https://docs.kernel.org/admin-guide/mm/transhuge.html)。

复杂度审查：本阶段仅增加两个独立的实验程序，使用现有 Mach 接口和 Linux
proc 接口。未在生产执行路径引入第二套内存状态，也未增加界面选项。后续接入
需要覆盖失效和故障恢复；当前测试不能替代完整客体的验证。

### 完整系统采集修正

`g1-tcg-art-ui` 已显示桌面，独立命令通道也返回 `sys.boot_completed=1`，但
ADB 持续显示 offline。暂停测试脚本后重连当前 ADB 连接即可恢复，未重启客体
或 adbd。随后 SurfaceFlinger 查询为空，脚本未取得交互数据。本轮包含人工
连接恢复与宿主采样，启动耗时排除比较。

`test_gfxstream_hyperos.py` 现使用既有命令服务确认启动，再建立 ADB 连接；
取消启动过程中反复断开连接的轮询。SurfaceFlinger 查询显式检查退出码和
renderer 字段，并延长服务内部上限，查询失败单独记录。新流程已在
`m1-android-coverage` 的 HVF 运行中通过启动、renderer 和绘制检查。
E6 的旧轮次只有结束截图与查询超时，继续保留为启动耗时未知。

`g1-tcg-art-ui-v2` 使用修正后的查询流程，单 vCPU、4 GiB RAM、TCG、
Metal GLES 和已接受的 ART speed 产物，723.678 秒后查询到启动完成。
SurfaceFlinger 返回 ANGLE Metal renderer。设置冷启动的 `TotalTime` 为
174,492 ms，`WaitTime` 为 178,249 ms。此前 `g1-ui-gfxstream` 的对应
`TotalTime` 为 96,042 ms；两个轮次没有组成重复的 ART 开关对照，不能据此
计算 ART 的提速或退化比例。完整系统的响应仍未达到目标。

本轮设置的 gfxinfo 返回 `Failure while dumping the app`，未取得帧耗时。
结束截图已进入通知与状态栏子页面，预定的六次输入未保持在设置主列表。
因此本轮只采用启动确认与设置冷启动数据，滑动结果无效。总运行时间为
1,101.45 秒，QEMU 已关闭，检查记录为同目录 `review.json`。

测量脚本继续保留已完成阶段的设置启动数据，后续滑动或服务查询失败时也会
保存。新增设置打开后的截图与有界 logcat 采集，用于检查页面和失败原因。
这两项记录改动在 v2 启动后完成，v2 的 Python 进程仍使用启动时的脚本内容。

### 阶段检查

本轮八组执行器宿主测试、六次 Linux 对照运行、Mac GLES 像素与同步检查、
iOS 库和应用构建均通过。Darwin 内存原型与 Android 进程映射采样已完成，
冷启动后 ART 仍接受预编译产物；完整界面测试仍未取得有效滑动数据。
本轮没有安装或启动手机应用。

复杂度审查保留已有 QEMU、ANGLE、Rutabaga 和 ART 接口。Direct 的调用缓存
限定在执行段内，内存探索使用两个独立程序；尚未为待验证架构增加运行时
状态或产品选项。新补丁的空白上下文标记属于 patch 格式，新增源码行的空白
检查与补丁反向应用检查均通过。

## M2/M3：直接内存映射与跨片段复用（2026-10-08）

状态：已实现并接入 Mac QEMU。独立测试与完整 Linux 验证通过，iOS 静态库
编译通过。完整 HyperOS 冷启动及 Metal 绘制检查通过，启动仍很慢；手机仍使用先前版本。

### 执行与失效处理

`HUSK_ARM64_FASTMEM=1` 启用实验实现，默认关闭。48 位客体地址按 1 GiB
划分，读写各有一个地址表，共约 4 MiB；未使用的地址段指向同一个无访问权限
的保留区，需要映射时才保留对应地址段。物理 RAM 通过 `vm_remap(copy=false)`
共享。Mac 宿主页为 16 KiB，因此四个客体 4 KiB 页必须具有相应访问许可、物理
连续且满足宿主页对齐。检查仍由 QEMU 的 MMU 完成。

正常访问从段表计算宿主地址后执行原生访存。保护异常通过 SIGBUS/SIGSEGV
定位到生成代码的访存指令，恢复客体寄存器和原 PC，再尝试映射。无法映射时，
这条生成指令改走原有页缓存检查路径，直到代码重新编译。这个选择减少反复
异常的开销，也会让后来满足条件的地址继续使用检查路径；收益与覆盖率仍需
在完整系统中观察。

QEMU 在 TLB 全量、单页、范围失效，以及代码写保护和脏页跟踪重置时递增映射
代次。进入 Direct 时比较代次与 MMU 索引，变化后撤销旧映射。代次相同时可以
跨执行片段复用。新执行代码页会撤销与其物理范围重叠的写入别名。
`HUSK_ARM64_FASTMEM_REUSE=0` 可关闭跨片段复用，供消融比较。内核 Direct
实验继续逐片段检查。当前实现要求单 vCPU；地址表、异常处理和失效策略尚未
扩展到多 vCPU。

补充的数据 TBI 路径根据客体 MMU 标志处理顶字节：只有 EL0 下半地址的数据
TBI 已启用且正常访问不使用 MTE 时才忽略顶字节，原始指针标签仍用于回调验证
和寄存器写回。跨 4 KiB 页的组合访问继续交由 QEMU 处理。该补充已通过主机
测试和 iOS 编译；完整 Linux 扩展回归也已通过。

### 验证中发现的问题

初版只覆盖 39 位地址，Linux 测试进程的高地址直接进入检查路径，未建立 RAM
别名。扩展到 48 位后，QEMU 工作线程屏蔽 SIGBUS，导致首次保护异常停顿。
初始化现在启用当前执行线程的同步异常信号，主机测试也覆盖了初始屏蔽 SIGBUS
的条件。修正后的 Linux 日志确认实际建立了 RAM 别名。

启用 16 KiB mTHP 后，逐片段重建映射的指针测试曾耗时 2,783.081 ms；日志
记录到至少 262,112 次映射安装。跨片段复用将同一负载的单次诊断耗时降至
47.221 ms。这组数字反映初版映射管理的开销；下面的重复对照用于比较现有后端。

13 项主机测试通过，包括批量内存、整数、SIMD、浮点和原子指令对照，以及 48 位地址、
权限撤销、物理别名、代码页切换、跨片段复用、代次更新、标签保留和异常状态
恢复。完整 Linux 测试覆盖 fork/COW、进程隔离、写保护信号、跨页信号、代码
修改、BTI/PAC 失败与 LSE 权限。`m3-linux-tags-cow` 的三种后端均通过已映射
指针环的 COW、父子进程隔离、4 KiB 写权限撤销、读写负载与定时中断检查。

### 性能对照与范围

同一 Mac、同一 QEMU、单 vCPU，全部启用测试客体的 16 KiB mTHP；TCG、
Direct 和 fastmem 顺序交替运行三轮。以下为中位数，单位 ms。

| 负载 | TCG | Direct | Direct + fastmem |
| --- | ---: | ---: | ---: |
| 整数循环 | 94.496 | 39.462 | 40.092 |
| 固定地址读写 | 115.172 | 135.113 | 136.329 |
| 浮点循环 | 53.132 | 7.461 | 7.388 |
| 跨页函数调用 | 131.889 | 16.375 | 18.260 |
| 跨页指针遍历 | 53.762 | 79.664 | 47.426 |

fastmem 的指针遍历约为现有 Direct 的 1.68 倍、同组 TCG 的 1.13 倍。
其他负载未显示 fastmem 的稳定改善。固定地址循环已有循环映射，不能用它
单独判断新映射的效果。指针测试访问 64 个间隔 16 KiB 的节点，共遍历一千万
次；TCG 个别轮次曾接近一秒，存在明显波动，不能据此宣称十几倍的稳定收益。

`HUSK_ARM64_SLICE_BUDGET=65536` 的另一组三轮对照中，TCG / Direct /
fastmem 整数耗时为 97.539 / 24.094 / 24.102 ms，固定地址读写为
115.919 / 107.588 / 107.968 ms，指针遍历为 58.613 / 63.895 / 46.562 ms。
较长片段减少进入执行器的次数，默认预算仍为 4096。另一次完整 Linux 检查中，
父进程能够在 2 ms 定时睡眠结束后终止持续执行原生循环的子进程；八次测得
最慢完成耗时为 TCG 5.201 ms、fastmem 3.631 ms。这只验证该负载能被抢占，
尚未覆盖完整 Android 的所有中断延迟。

数据目录：`work/performance/m3-linux-retained/`、
`work/performance/m3-linux-long-slices/`、
`work/performance/m3-linux-preemption/`。较早的排查轮次保留在
`work/performance/m2-linux-*/`，其中 39 位测试未实际使用直接映射。

### 客体配置与完整系统检查

`scripts/configure_hyperos_mthp.py` 通过指定的 ADB 连接检查内核接口，设置
16 KiB mTHP，并在 `/vendor/etc/init/husk-memory.rc` 写入早期启动动作。
Mac 测试客体已设置为 `always`，原策略为 `never`。每次仅修改这个配置文件，
核对写入结果；使用 `--policy never` 可恢复原分配策略。冷启动已确认早期配置生效；
已有进程的页面不会立即重新布局。

Linux 6.12 文档列出了 mTHP sysfs 和 `thp_anon` 启动参数。当前使用 init
配置沿用本镜像已有的 sysfs 接口，未变更整个系统的页大小。
来源：[Linux 6.12 THP 文档](https://docs.kernel.org/6.12/admin-guide/mm/transhuge.html)。

`m3-android-boot` 已检查单 vCPU、Metal GLES、fastmem、65536 预算和早期
mTHP 配置。确认 `sys.boot_completed=1` 用时 966.227 秒，10 帧 GLES 测试
平均 2.895 ms/帧，像素结果通过。早期 16 KiB mTHP 策略为 `always`。该运行
使用标签处理补充前的二进制，启动日志确认建立 RAM 别名。
已采集一次 2 秒宿主启动样本；同期还有主机测试和短暂 iOS 交叉编译，此轮
不作为严格的性能消融。Husk 源码已移除 Direct 启动时强制开启逐片段性能统计
的设置，统计仍可显式启用。现有 IPA 未更新。

### 复杂度审查与后续判断

本阶段使用一个内部映射类和现有执行器回调。代次由 QEMU 的失效入口维护，
代码许可继续由已有回调检查，未增加独立的页表解析器或产品配置界面。主机
随机指令测试复用现有测试程序，通过对齐内存和环境参数覆盖新路径。保留逐
片段模式用于消融，未保留第二套共享 RAM 管理实现。

完整系统仍未达到基础可用性。M4 随后检查客体与宿主协作的批量内存操作。
启动样本中存在 DC ZVA、MMU 查询和 PAUTH 处理成本；2 秒采样不能用于
推算整个桌面的收益。


## M4：批量内存接口及 Bionic 对照（2026-10-08）

完整 Linux 与 HyperOS 继续运行。新增接口只处理一段已验证 RAM 的复制、移动
或填充，目的是减少这类循环中的指令翻译和逐次地址查询。实验开关为
`HUSK_ARM64_BULK=1`，要求单 vCPU，默认关闭。

### 接口与拒绝条件

客体使用 `HLT #0x484d`：x0 为目标地址，x1 为源地址或填充值，x2 为字节数，
x3 为操作编号（copy=0、move=1、set=2）。x8 返回 0 表示完成，1 表示拒绝；
参数寄存器与其他架构状态保持不变。最大单次请求为 65536 字节。

宿主先逐页检查所有源和目标地址。页未驻留、权限不足、MMIO、观察点、待处理
代码脏页或当前 Direct 代码的物理别名均按现有 MMU 回调处理；拒绝时不修改
目标 RAM。MTE 活跃时也拒绝。客体继续调用原有 libc，由普通指令完成缺页、
COW 或权限异常。copy/move 先收集整段源数据再写入，覆盖虚拟地址不同但物理
页重叠的情况。临时缓冲区最大 64 KiB，未加入长期内存副本。

TCG 与 Direct 均识别该接口。Direct 按处理字节数扣除执行预算，每 16 字节
增加一次预算消耗，长循环会返回 QEMU 检查中断。普通 QEMU 不识别该 HLT，
因此客体必须先核对本次启动的宿主声明。

宿主只在单 vCPU TCG 且批量接口启用时发布版本声明。直接内核启动使用设备树
`/chosen/husk,bulk-memory`；当前 HyperOS 通过 UEFI/ACPI 启动，使用签名为
`HUSK`、OEM table ID 为 `BULKv001` 的 36 字节表。实验库核对表头、版本、
长度和校验和。HVF 和未启用接口的宿主不发布声明。

### Android 接入

`src/guest-bulk/` 提供 `libhusk_bulk.so`。大于等于 1024 字节的 memcpy/memset
分块请求宿主；memmove 仅在不超过 64 KiB 时使用一次请求，较大重叠区域继续
调用原 libc。小调用和关闭状态通过汇编入口尾调用原函数，省去额外栈帧和 PAC
认证。初始化期间尚未取得 libc 函数地址时，使用独立的字节循环，防止递归调用。
`HUSK_BULK_DISABLE=1` 用于关闭客体请求，`HUSK_BULK_STATS=1` 仅用于测试计数。

`scripts/build_guest_bulk.py` 使用现有 Android NDK 构建库和 Bionic 对照程序。
`scripts/install_guest_bulk.py` 仅修改 64 位 zygote 与 SurfaceFlinger 的
LD_PRELOAD 配置，并安装库与 ACPI 读取权限。它会拒绝覆盖其他 LD_PRELOAD；
`--remove` 删除本实验的配置和库。Mac 测试镜像的全局预加载已在对照后移除。
实验库检测到 `HWCAP2_MOPS` 时保留 libc 原有实现。手机尚未安装这一阶段。

### 已完成验证

- 13 项宿主测试通过。批量测试覆盖随机长度、重叠、反序物理别名、64 KiB
  上限、地址溢出、末页读写失败时整段保持不变，以及 Direct 辅助调用中的完整
  寄存器状态和执行预算。
- 完整 Linux 的 TCG、Direct、Direct+fastmem 各运行三次开启和三次关闭对照，
  均通过。包含 COW、4 KiB 权限撤销、信号、BTI/PAC、原子操作与定时中断。
  开启时每组 1024 次批量复制/清零均由宿主完成。
- 当前 64 KiB 块重复 1024 次，共 64 MiB 的中位数如下，单位 ms。关闭接口时
  使用 64 字节 NEON 循环，测试数据驻留并预热；整机收益还需单独测量。

| 后端 | 复制：关闭 / 开启 | 清零：关闭 / 开启 |
| --- | ---: | ---: |
| TCG | 4.866 / 1.736 | 2.250 / 0.767 |
| Direct | 3.665 / 1.748 | 1.598 / 0.847 |
| Direct + fastmem | 3.529 / 1.729 | 1.417 / 0.897 |

- iOS Direct 静态库和完整 iOS QEMU 动态库均已编译；IPA 尚未更新。
- HyperOS 在 HVF 下使用原 libc、关闭的实验库、自动检测的实验库，各运行三次
  Bionic 复制/填充、重叠、COW 与只读页异常检查，全部通过。自动检测均保持关闭。
- `m4-android-cold` 在 853.327 秒确认启动完成，之后 ADB 连接关闭，未完成
  Bionic/GLES 对照。ACPI 表已由内核识别，zygote 与 SurfaceFlinger 已读取。
  当前镜像原有 SELinux 状态为 Permissive；Enforcing 读取规则尚未实现。

数据：`work/performance/m4-linux-bulk-on/`、`m4-linux-bulk-off/`、
`m4-android-hvf-compat/`、`m4-android-install/`、`m4-android-cold/`。

### 复杂度审查

批量操作复用现有 MMU 回调和 QEMU 指令辅助入口，未增加客体物理页解析器。
宿主函数只负责有界 RAM 操作，客体库负责 libc 回退与较大请求的分块；异常
仍由客体 Linux 处理。取消完整预检会产生拒绝前的部分写入，取消收集缓冲区会
破坏物理别名重叠，二者继续保留。测试计数默认关闭。

能力声明使用 Linux 已有的设备树/ACPI 导出接口。当前镜像未启用 fw_cfg sysfs，
UEFI 启动又未导出设备树，因此同时支持两种固件入口有实际使用场景。没有新增
守护进程或常驻通信服务。安装脚本只保留固定目标与本实验新增行，未引入通用
配置编辑框架。

### 真实 Bionic 结果与停止部署的原因

从当前 HyperOS 提取 linker64、libc、libdl 和 libm，以小型完整 Linux 启动运行
相同 Android 测试程序。`scripts/test_guest_bulk.py` 比较原库、关闭实验库、开启
实验库三种情况，每种三次，并保留 COW、重叠和权限异常检查。

Bionic 的 IFUNC 已根据 `HWCAP2_MOPS` 选择 MOPS 实现。QEMU 的
`target/arm/tcg/helper-a64.c` 对这类指令按页调用宿主 memmove/memset。
`m4-bionic-leaf-valid` 中，实验库在大部分实际 Bionic 负载上更慢。因此取消
全局部署，M4 保持默认关闭，后续主线转向执行切换和原生调用。

也试过对 256 字节以内的操作使用普通 SIMD 指令。64 字节调用累计复制 16 MiB，
三次中位数：TCG 从 10.353 ms 降到 6.487 ms，Direct 从 10.721 ms 增至
24.641 ms，Direct+fastmem 从 11.179 ms 增至 27.121 ms。该短复制实现及
专用配置已从源码移除。保留失败实验的数据，供后续判断；无需继续维护第二套
小块 libc 实现。数据位于 `work/performance/m5-bionic-cached/`。

整理后的 `m5-bionic-final` 中，TCG、Direct、Direct+fastmem 各九组均通过；
当前 MOPS 客体不会启用批量替换。新增的函数入口异常检查也在这三组后端通过。

## M5：普通函数入口与间接调用缓存（2026-10-08）

### 问题与实现

`m5-android-cpu-profile` 的完整 HyperOS 运行累计进入 Direct 13,141,102 次，
其中分支退出 12,509,028 次。已完成入口验证的普通 BLR 目标仍频繁返回 QEMU：
首条指令可能缺页，必须在异常发生时保留 BTYPE；此前只缓存 BTI 等特定入口。

新增单条入口编译变体，复用现有指令生成、MMU 检查和异常恢复。普通入口通过
QEMU 的 BTI 检查后进入该变体，首条指令成功完成才清除 BTYPE，然后跳到正常
原生代码。缓存的 BLR 入口先恢复 BTYPE=2，随后检查预算并执行首条指令。
失败访存、预算耗尽和跨页取指仍返回准确的 PC 与完整状态。内核与完整 Android
继续由原执行架构运行。`HUSK_ARM64_LANDING=0` 用于关闭这一改动进行对照。

BTYPE 使用 `access_flags` 的第 4、5 位，编译变体使用独立的内部标记。入口
变体不会发布到普通跳转缓存，防止普通分支错误地恢复 BTYPE。代码页和调用缓存
仍遵循原有单片段有效期，未新增进程级缓存管理。

### 已完成验证

- 13 项宿主测试通过。新增缓存命中后首条读取失败、地址写回保持、首条成功后
  下一条失败、跨页取指、寄存器状态与预算检查。
- 完整 Linux 下 TCG、Direct、Direct+fastmem 的系统调用、fork/COW、权限、
  BTI/PAC、原子操作与定时中断检查通过。
- 使用真实 Bionic 的信号测试校验 `ucontext`：故障发生在入口首条指令时保留
  BTYPE=2，发生在第二条时为 0，并校验 SIGSEGV 地址与 PC。三个后端均通过。
- Mac QEMU、iOS Direct 静态库及完整 iOS QEMU 动态库已编译。手机尚未测试，
  现有 IPA 仍为 build 36。

一百万次普通间接调用，目标以 `stp x29,x30,[sp,#-16]!` 开始，三次中位数：

| 后端 | 关闭普通入口缓存 | 启用普通入口缓存 |
| --- | ---: | ---: |
| Direct | 438.486 ms | 2.721 ms |
| Direct + fastmem | 449.374 ms | 2.802 ms |
| TCG | — | 18.850 ms |

Direct 对该负载相对关闭状态约快 161 倍，相对 TCG 约快 6.9 倍。中间版本每次
首条指令完成后仍回到 C++，耗时 213.893 ms；最终版本让缓存调用在生成代码内
继续执行。这些数字仅描述该调用循环，完整 Android 和 iPhone 的幅度尚未确认。
原始数据位于 `m5-linux-landing-off`、`m5-linux-landing-on`、
`m5-linux-landing-cached`，均在 `work/performance/`。

### 完整 HyperOS 检查

修改前的 `m5-android-cpu-profile` 在 959.502 秒通过命令服务确认启动；客体
日志首次处理 `sys.boot_completed=1` 的时间为 811.498 秒。结束截图显示桌面，
SurfaceFlinger 使用 ANGLE Metal。GLES 程序创建上下文后以 255 退出，未完成
十帧绘制。测试期间存在后台构建，属于诊断运行。

该次累计用户态 TCG 444.644 秒、内核 TCG 365.902 秒、Direct 200.458 秒；
Direct 编译计时 87.800 秒，缓存清理 234 次。后续需继续减少频繁辅助调用和
翻译缓存重复编译，内核成本也仍占较大比例。

`m5-android-landing-cached` 在 244.170 秒确认启动，GLES 十帧平均 1.780 ms；
随后 QEMU 在 252 秒附近崩溃。该次运行未通过持续运行检查，不能据此认定整机
性能达标。原脚本未检查等待期间 QEMU 的退出状态，结果文件已按崩溃报告修正，
脚本也已加入异常退出检查。该 GLES 程序摘要与旧采样不同，两次绘制计时不可
直接比较。

崩溃原因已在宿主测试中单独复现：相邻两个地址都存在入口变体时，首条指令
完成后跳到另一入口变体；随后访存重试按已经清除的 BTYPE 查询普通代码，
取到空入口。修复后，首条指令的后续执行统一查询普通代码。新增测试在修复前
以 SIGSEGV 失败，修复后通过；全部 13 项宿主测试和两种 Direct 的 Linux
检查通过。`m5-android-continuation-fix` 在 804.439 秒确认启动，客体首次处理
`sys.boot_completed=1` 的时间为 770.428 秒。当前 GLES 程序十帧平均
30.050 ms，之后持续运行 300 秒，通过退出状态检查；总运行 1145.745 秒。
该次存在后台构建，属于诊断运行，尚未完成严格的整机性能对照。

最后一次 Direct 统计为 1,019,475 次进入、203,293 次分支退出、250,525,653
次辅助调用。编译累计 102.285 秒，代码池清理 264 次。普通调用缓存减少了
执行器切换，但辅助调用、用户态 TCG 和内核成本仍需处理。

这些运行复用现有磁盘，保持一个可见窗口，未新增镜像副本。M2–M5 的本地
阶段提交为 `147de64`，未推送；上述崩溃修复在下一次提交中记录。

## M6：MOPS 序列、短复制原生执行及 CRC（2026-10-08）

M5 的主要回退位置包含 MOPS、CRC 和 32 位扩展寄存器运算。新增八种
CRC32/CRC32C 的宿主指令执行，要求客体声明 FEAT_CRC32，且宿主
`hw.optional.armv8_crc32` 可用；能力变化会切换代码版本。已有 AddExt
实现扩大到 32 位加减和比较，继续使用原寄存器映射与条件标志处理。

### MOPS 调度对照

先试过每条 MOPS 完成后立即切回 Direct。同一二进制开关对照中，64 字节
Bionic 复制累计 16 MiB，Direct 从 11.233 ms 增至 144.427 ms；计算与复制
交替执行从 16.393 ms 降到 14.466 ms。逐条切换造成短复制退化，相关实现和
开关已从源码移除。数据保存在 `m6-bionic-bridge-on` 和 `m6-bionic-bridge-off`。

随后识别连续的 CPYP/CPYM/CPYE 或 SETP/SETM/SETE，复用有界 RAM 接口一次
完成三条指令。使用默认访问选项、相同寄存器和同一个不可写执行页；QEMU
检查 FEAT_MOPS、EL0 执行控制和 MTE 状态。最多处理 64 KiB，写入前验证全部
页面。拒绝时内存和寄存器保持原值，由 QEMU 执行并处理缺页、COW 与部分完成。
`HUSK_ARM64_MOPS=0` 可关闭这项实验。

在 `m6-bionic-sequence` 中，Direct 的混合负载为 13.554 ms，TCG 为
16.486 ms；纯短复制仍为 27.678 ms，TCG 为 10.428 ms。为减少 C++ 出入成本，
继续实现 1～64 字节的原生序列：复用页映射缓存，源和目标各在一个 4 KiB 页内；
先读取全部需要的数据再写入，保留重叠语义及全部客体 SIMD 寄存器。成功后按
QEMU Option A 更新地址、长度、NZCV 和 PC。跨页及其他不满足条件的请求保留
已有辅助处理。

两段相隔 1 MiB 的内存会争用原页缓存槽。索引加入较高地址位，减少这一常见
布局的冲突；仍发生冲突时直接使用辅助处理，防止反复映射无法继续。
`m6-bionic-native-hash` 中，Direct 的短复制为 13.618 ms，TCG 为 10.603 ms；
混合负载分别为 12.927 ms 和 15.960 ms。短复制仍有额外成本，下一阶段继续
处理 Bionic 调用中的认证和其他辅助指令。

上述 Bionic 数字均为三次原库运行的中位数，测试使用当前 HyperOS 的真实
linker64/libc/libdl/libm。源码、二进制摘要和启动参数随结果保存。测试在小型
完整 Linux 中运行；不能据此确定 Android 整体或手机收益。

### 验证与复杂度审查

13 项宿主测试通过。新增 CRC 能力切换与非法编码、32 位扩展运算差分检查；
MOPS 覆盖所有 1～64 字节长度、不同寄存器、重叠、页尾、拒绝访问、标签地址、
不匹配序列与缓存冲突。检查完整寄存器状态和内存，拒绝时不产生部分写入。
Bionic 三种后端各九组检查通过，包括 COW、权限异常和分支入口异常。

复用原页映射、预算与 QEMU 权限控制。序列编码校验由辅助实现和原生生成器
共用。辅助实现继续处理较大操作与边界情况；原生短路径针对重复出入执行器的
成本。没有新增客体库替换或常驻服务。

## M7：高频辅助指令的原生执行（2026-10-08，验证中）

M5 完整启动及持续运行累计辅助调用超过 2.5 亿次。M6 的短复制已经在生成
代码内完成，Bionic 调用仍包含指针认证、屏障和分支提示。本阶段继续减少这些
指令保存和恢复完整寄存器的次数。

- DMB/DSB 生成与原 QEMU 辅助处理相同的宿主完整内存屏障。设备访问已在到达
  屏障之前返回 QEMU。
- 已通过目标检查的 BTI 在执行片段内继续运行；入口检查仍由 QEMU 完成。
- PAC 提示指令首次由 QEMU 执行；仅在辅助处理明确允许时，复用同一指令、
  完整输入值和 modifier 的结果。固定大小缓存每次 `run()` 开始清空；密钥、
  权限或地址参数改变后必须重新验证。认证异常仍走原 QEMU 异常路径。

`HUSK_ARM64_INLINE_ASSISTS=0` 用于相同二进制的关闭对照。宿主测试覆盖完整
寄存器、缓存复用、执行片段之间的密钥变化与拒绝处理；13 项宿主测试通过。
完整 Linux 的真实 PAC、篡改指针异常、进程切换检查通过。同一二进制关闭/开启
对照各三轮，Direct 的认证循环中位数从 83.229 ms 降到 0.934 ms；TCG 同组为
21.102 ms。数据为 `m7-linux-inline-on` 与 `m7-linux-inline-off`。
真实 Bionic 的纯短复制仍未快于 TCG，`m7-bionic-inline-on` 保留了该结果。

`m7-android-inline` 完整 HyperOS 在 201.776 秒确认启动，客体日志首次
`sys.boot_completed=1` 为 172.082 秒。设置冷启动 TotalTime 为 24,886 ms，
六次滑动命令耗时 32.786 秒；采集到 16 帧，中位数 34.918 ms、P90 82.412 ms，
最长 154.998 ms。图形测试十帧平均 3.051 ms；随后持续运行 180 秒，全部测试
结束于 477.361 秒，未异常退出。

与上一修复版 804.439 秒启动相比，本轮有明显改善。磁盘持续复用，期间有后台
构建，这些数值记录本轮观察，尚不足以确定稳定的整机倍数。设置启动和交互仍
耗时很长，继续优化。退出前累计 Direct 65.301 秒，其中编译 35.890 秒；
辅助调用 60,165,249 次，包含 51,185,043 次分支辅助调用。其余主要回退含
LDAXR、MOPS、CNTVCT 与尚未覆盖的 SIMD 指令。

iOS Direct 静态库及完整 QEMU 动态库已编译，手机尚未更新。本轮 Mac 测试
只有一个可见窗口，结束后关闭。

## M8：间接尾调用和独占访问状态（2026-10-08，验证中）

根据 M7 的实际运行结果，继续处理大量 BR 尾调用与 LDAXR/STXR 锁循环。

BR x16/x17 使用与 BLR 分开的目标缓存，保留 BTYPE=1 和原 LR；普通函数
入口仍通过单条入口变体完成，成功后清除 BTYPE。其他寄存器的 BR 可能依赖
源页面的 guarded 状态，继续由 QEMU 检查。新增拒绝访存测试校验缓存命中
后的 BTYPE 和 LR，防止与 BLR 的 BTYPE=2 混用。

独占访问复用 QEMU 的软件 monitor，状态包含清理标签后的虚拟地址和读取值。
LDXR/LDAXR 的单元素读取保存这两个字段；STXR/STLXR 使用宿主 LSE compare-
exchange 比较原值并写入，随后清除 monitor。映射回调和 TCG 切换后继续使用
同一状态。双元素操作保留 QEMU 处理，原有高半部状态留在 QEMU。CLREX 清除
地址。宿主没有 LSE 时使用原路径；`HUSK_ARM64_EXCLUSIVE=0` 用于关闭对照。

宿主检查覆盖四种宽度、寄存器重叠、零寄存器、标签、成功/失败结果、跨执行
片段交接、CLREX，以及读写权限和地址不匹配。完整 Linux 的真实独占循环、
中间执行 TCG ISB 的循环及 BR x16 尾调用已通过三种后端各三轮检查。
`m8-linux-native` 的尾调用中位数为 TCG 18.068 ms、Direct 1.851 ms，独占循环
为 3.508 ms、4.091 ms。锁循环本身仍有额外开销；本阶段使它后面的代码能够
继续原生执行。系统调用与进程切换在该组测试中也略慢于 TCG。

`m8-bionic-native` 的三种后端各九组检查通过。以下为原装 Bionic 三轮中位数，
64 字节操作累计处理 16 MiB，单位为 ms：

| 后端 | 复制 | 填充 | 混合计算 |
| --- | ---: | ---: | ---: |
| TCG | 10.895 | 10.031 | 16.533 |
| Direct | 1.555 | 2.829 | 13.535 |
| Direct + fastmem | 1.636 | 2.811 | 13.189 |

完整 HyperOS 的 `m8-android-native` 在 1,203.173 秒结束，1,200 秒内未通过
客体命令服务确认启动完成，结果为失败。退出前编译累计 218.494 秒、缓存清空
533 次；vCPU 时间中用户态 TCG 403.811 秒、内核 TCG 368.577 秒、Direct
368.681 秒。新增的 BR 缓存和 exclusive 路径需要开关对照，局部收益尚未传到
完整启动。
此次 iOS Direct 静态库和完整 QEMU 动态库编译成功，手机未更新。

## M9：SIMD 列表访存与整数向量指令（2026-10-08，验证中）

M8 整机回退统计包含 `LD1 {v1.16b,v2.16b}`、CNT、CMEQ #0 与 UMINP。
新增 LD1～LD4/ST1～ST4 的完整寄存器列表、立即数和寄存器后索引，以及基础
位计数、位反转、零比较、ABS/NEG 和成对极值计算。访存复用现有页映射；先
校验整个跨度，成功后写回客体基址。跨 4 KiB 页、拒绝权限、SP 对齐和 FP
禁用继续返回 QEMU，异常前保留寄存器和内存。

13 项宿主检查通过。差分检查覆盖 131,072 条 SIMD/FP 指令、4,096 组向量
访存、全部向量和通用寄存器、FPCR/FPSR，另检查权限、页边界和非法编码。
完整 Linux 的三种后端各三轮检查通过，包含向量扫描、独占写权限 SIGSEGV、
进程切换及定时器抢占。向量扫描中位数为 TCG 15.486 ms、Direct 1.316 ms、
Direct + fastmem 1.487 ms。iOS Direct 静态库与 QEMU 动态库编译通过。
`m9-android-exclusive-off` 正在关闭独占访问路径后验证完整 HyperOS。

复杂度审查保留共用访存生成器，新增结构访存仅提供跨度、编码和基址写回差异。
入口变体按需分配两组 BTYPE 指针数组，共 16 KiB；普通代码页只保存普通入口。
下一步先完成同构建整机对照，再决定是否需要调整执行片段和代码缓存策略。
