# 用户态执行后端开发进展

更新日期：2026-10-07。

## 目标与当前决策

在 iPhone 内运行完整 HyperOS 3，大幅改善持续交互速度和发热。技术背景见
[执行架构调研](hyperos3-execution-research.md)。用户允许修改 Husk、执行后端、
客体内核和图形实现。开发与功能验证集中在 Mac，形成有实际收益的阶段产物后
再安排手机测试。E2 iOS 试用发现 SELinux 编译阶段长时间停顿；E3 已在 Mac
复现并修正这处退化，扩大整数和访存指令覆盖。完整 Android 启动已通过，
桌面持续操作和手机整机收益仍待验证。

用户明确指出约 1.5 倍的局部提升不足以满足目标。E1 的整数测试约快 1.7 倍，
短访存测试存在波动。E2 已加入直接执行后端，当前整数循环约快 3 倍，固定
地址的访存循环与 TCG 接近；完整 HyperOS 的性能尚待验证。后续优先扩大
**用户态直接执行、宿主快速映射与客体内核配合**，同时研究 gfxstream 图形
命令转发。ARM64 User-Mode Linux 的 Darwin 移植列为更高投入的候选，先核对
平台条件。收益判断采用完整 HyperOS 的桌面、设置、应用切换和持续运行表现。

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
运行波动和指令兼容性问题尚未区分，故该实验保存为
`patches/experimental-direct-tagged-data.patch`，当前默认源码已恢复到此前
完成桌面启动的实现。下一次验证需要补采挂起时的 CPU 调用栈与客体状态。

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
ad-hoc 深层严格签名检查均通过。摘要保存在同目录 JSON。该包尚未进行手机测试。

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
