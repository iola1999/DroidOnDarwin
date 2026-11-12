# 决策记录: 原始 HyperOS ART 标准 JNI 实验

状态: implemented

## 问题

[4 KiB、稀疏地址与 TBI 组合](../../proposed/architecture/2026-11-12-受限4KiB客体与原始HyperOSlinker.md)已完成原始 HyperOS Bionic 的 CLI/GCD 组件检查。下一步需要运行同一系统输入的原始 ART 与类库，核对标准 VM 创建、JNI、GC 和线程行为。[SDK36 ART 实验](../../proposed/testing/2026-11-12-原始SDK36ART标准JNI实验.md)提供现有 fixture 与装载流程，本阶段复用其标准 JNI 契约。

## 决策

### 固定原始输入

使用现存 `NEORUAA/HyperOS-AVD` 发布 `v0.1.0-a16-hyperos3-fuxi-r1` 的系统镜像。来源提交 `9d7ddd3f046e89d33cb205b5fbbf6dc5b728155c`，仍为第三方 MysticGSI 移植输入。`system.ext4` 的 SHA256 再次核对为 `dd65f33741bfce77b22b5c58b8d696b16da52f15a825acafcbcede47a3cfa3ca`，原输入报告摘要为 `c9ec384e1eee4c754152fddaae8d351ff5f1adaafb86e5cdc88fb02f5f712d0a`。

system 指纹为 `Xiaomi/missi/missi:16/BP2A.250605.031.A3/OS3.0.2.0.WMCCNXM:user/release-keys`，Android 16、API 36，安全补丁声明 `2025-11-01`。vendor 指纹为 `google/sdk_gphone64_arm64/emu64a:16/BE2A.250530.026.D1/13818094:user/release-keys`；本组件不运行 vendor 服务。`init.environ.rc` 指定 `/apex/com.android.art`，选择同名 ART APEX 版本 `360499999`；镜像另含 `com.android.art.compatible` 版本 `340090000`，其文件不进入本轮运行输入。

所需 ART、runtime、i18n、conscrypt、tzdata 和 statsd APEX 均来自该 system 镜像，保留原 archive/payload/file 摘要。普通 ART 的 `libart.so` SHA256 为 `bc853e5574df75a348ca4e3e3f332d5a61673aa5d89b22a43908a49315e7895a`，与早期扫描一致；七个 boot classpath JAR 均通过 ZIP 完整性及 `classes.dex` 检查。ICU 使用同一 i18n APEX 的 `icudt76l.dat`，时区文件来自同一 tzdata APEX。

按现有组件搜索顺序展开标准 ART 根库、原始公共库预加载清单和 `DT_NEEDED` 闭包，得到 287 个库、299 个原始文件、164642225 字节，缺失为零。初次只搜索 SDK 的目录缺少五个库：`libjpeg-hyper.so`、`libdynamicaudiolog.so`、`libmisight.so`、`libmediastub.so`、`libMiVideoFilter.so`；它们全部位于同一镜像 `/system_ext/lib64`，继续展开后得到最终闭包。组件 linker 配置追加该目录，保持原库字节。

[固定依赖清单](../../../../experiments/native-linux/stock-art/hyperos-dependencies.json)保存路径、摘要、原始符号链接目标、候选库顺序和实际依赖。准备入口只接受这一固定来源并逐文件校验，复用现有 C/Java fixture 构建、目录 manifest、装载与 maps 导出。ELF LOAD 验证显式使用 4 KiB，SDK 默认仍使用 16 KiB。最终 staging 共 303 个文件，原始文件含 linker 别名共 166946345 字节，另含组件配置与 C/DEX fixture；盘点目录目前约 406 MB，完整 Android 镜像未复制。扫描阶段曾用 ext4 工具读取 EROFS runtime payload，未得到文件；改按文件系统 magic 选择工具后继续，未采用该失败输出。

### 运行与验收顺序

继续限定 Mac、4 KiB、64 GiB sparse、TBI、单 CPU、Unicorn/PTE、dual 代码内存。原始 ELF、JAR、公共库清单和数据文件逐项保留。fixture 使用现有 `-Xnoimage-dex2oat` 与不存在的 boot image 路径，从匹配 boot JAR 创建 VM，不复制 boot image 或完整系统盘。

先构建和校验 staging，执行输入缺失、原字节变化、来源身份变化与额外文件等回归；随后运行 CLI/GCD。VM 创建、DEX 装载、双向 JNI 参数、global/weak reference、数组、Java/pthread 线程、异常、确定性热循环与 VM 销毁均按现有标准契约检查。保存原始输出与各阶段 maps，失败也保留实际退出、信号、超时和已有诊断。

SDK JIT 观察由另一独立会话实施，本轮先保持原 fixture 契约，未修改它的 JIT 观察实现。报告中的 JIT 状态继续为未确认；完整 framework、原系统 namespace、HyperOS 桌面与 iOS 均不在本次组件验收内。

## 曾考虑的替代方案

- 复用 SDK 的 ART 依赖库。会混合来源，无法验证该 HyperOS 自带 runtime，本轮使用同一 system/APEX 的依赖闭包。
- 复制完整 Android 磁盘。组件只需要固定文件集合，本轮使用已有镜像逐文件提取。
- 使用 compatible ART。该模块有不同包名与版本，当前初始化配置指向普通 ART，先验证对应输入。

## 验收标准

- 原 system/APEX 与原始文件摘要可追溯到固定输入，准备前后逐项一致。
- staging 验证核对来源、文件集合、权限、内容和 fixture 源码，拒绝缺项或变化。
- CLI/GCD 分别通过现有标准 JNI、GC 与线程契约，正确报告 4096 字节客体页。
- 失败日志与 maps 完整保存，JIT 实际执行只按独立观察结果判断。

## 影响

静态依赖闭包未覆盖任意运行时动态加载。公共库预加载、系统属性、namespace、ART 私有低地址分配与额外 syscalls 仍可能产生新运行条件。本机组件成功仅适用于当前固定输入和受限执行配置；iOS 宿主地址摆放、Direct 4 KiB 与完整 HyperOS 继续单独验证。

## 本机结果

本阶段基于 `d28f396cf982ef7612550704d4f17f7723edab8b`，仅增加固定 HyperOS 输入准备与页大小核对，C/Java fixture、Linux runner 和 ART 参数保持该提交版本。宿主为 Apple M5 Ultra、macOS 27.0（26A428），宿主页 16384 字节。客体为 4096 字节页、64 GiB sparse、1 GiB RAM、单 CPU、Unicorn/PTE、TBI 开启、dual 代码内存。主工作区同期有其他功能构建，本轮没有性能对照。fixture 继续使用 NDK `29.0.14206865` 的 API35 driver、SDK35 Java stubs、build-tools `36.0.0` 与 javac `17.0.19`；被加载的 ART、Bionic 和七个 boot JAR 保持上述 Android 16 原始字节。

首次 `guest-4k-sparse-hyperos-art` 的 CLI/GCD 均成功。收尾增加准备目录与运行页大小不匹配的拒绝检查后，重新准备输入并运行 `guest-4k-sparse-hyperos-art-v2`，两入口均 `returncode=0/validated=true`。每入口 13 项标准 JNI 检查成功，VM 创建和销毁返回 0，GC 第一次请求后观察到 weak reference 清除并入队，Java 线程总值为 2000，pthread 返回 58，数组写回为 47，热循环返回 340011506336。进程无信号或超时，11 个执行上下文全部回收；运行前后原始输入与 fixture 摘要一致。

每轮均保存 `before-vm`、`after-vm`、`after-gc`、`after-warmup` 四份 maps，文件字节数和摘要与导出记录一致。stderr 留有 `cutils-trace: Error opening trace file: No such file or directory (2)`，本组件没有配置 tracefs；其余标准 JNI 检查完成。报告继续要求 `stock_art_jit: "unverified"` 与 `jit_execution_confirmed: false`，确定的热循环返回值仅用于功能检查。

[结构化结果](../../../../experiments/native-linux/results-hyperos-art-standard-jni-20261112.json)保存初轮记录、最终完整报告、固定准备输入、输入负例、解析检查及来源复核。最终运行报告的 61 项源码和构建摘要全部与实际文件一致，四次运行的 stdout/stderr 与 maps 摘要均重新核对。原始大日志保留在独立树 `work/native-linux-nce/guest-4k-sparse-hyperos-art` 和 `guest-4k-sparse-hyperos-art-v2`。

来源复核重新计算 7 份实际 system/APEX payload 的摘要，从固定 system 镜像重新读取 7 个 APEX archive，展开并核对其原始 APEX/payload 摘要，再从对应文件系统重新读取全部 299 个原始文件，长度与 SHA256 全部匹配固定清单。该检查覆盖原始软链接解析后的目标字节；staging 按组件已有流程保存普通文件。`com.android.art.compatible` 仅核对归档身份，未装载其 payload 或类库。

### 复现命令

以下命令从独立工作树执行。`work/hyperos-art-inventory/runtime` 是按固定清单提取的 299 个原始文件；提取位置与原镜像中的 `resolved_path` 保存在清单，ext4 使用 `debugfs -R 'cat <resolved_path>' <source_image>`，EROFS 使用 `dump.erofs --cat --path=<resolved_path> <source_image>`。镜像、APEX 和目标文件均核对清单中的摘要后再准备。

```sh
python3 -B scripts/prepare_stock_art.py --runtime-source hyperos --runtime work/hyperos-art-inventory/runtime --manifest ../../experiments/native-linux/results-hyperos-input-20261112.json --output work/hyperos-art-prepared-v3
python3 -B scripts/test_stock_art_inputs.py work/hyperos-art-prepared-v3
python3 -B scripts/test_stock_art_report.py
python3 -B scripts/probe_lkl_translation.py --guest-page-bytes 4096 --guest-layout sparse --data-tbi 1 --mapping pte --execution unicorn --code-memory dual --reuse-engine-build --workload stock-art --stock-art-directory work/hyperos-art-prepared-v3 --entry both --output work/native-linux-nce/guest-4k-sparse-hyperos-art-v2
python3 -B scripts/prepare_stock_art.py --runtime ../../work/native-linux-nce/android/android36 --manifest ../../work/native-linux-nce/android/android36-runtime.json --output work/sdk-art-prepared-page-regression
python3 -B scripts/test_stock_art_inputs.py work/sdk-art-prepared-page-regression
```

准备入口要求新的输出目录，复现已有目录需要选择新的名称。`--reuse-engine-build` 复用有固定摘要的既有 Unicorn 2.1.4 dual 构建；新工作树先运行对应引擎构建。源码发生变化后重新准备 fixture，保留旧 manifest 的构建身份。

### 回归与复杂度审查

HyperOS 输入检查拒绝 14 个负例，SDK 默认准备检查拒绝 13 个负例，包括文件缺失或变化、权限、原始 manifest、额外文件、来源、依赖摘要和运行页大小不匹配。HyperOS 另检查同时删除原文件和输出 manifest 条目，固定依赖文件集合仍要求完整。解析器接受 4096 与 16384 字节页的对应记录，拒绝 258 组错误记录，缺失或长度变化的 maps 检查成功。直接使用原始 HyperOS linker 验证 4 KiB LOAD 条件成功，默认 16 KiB 拒绝；SDK 原始 linker 的默认 16 KiB 检查成功，8192 参数被拒绝。

SDK 默认重新准备得到 294 个文件，原始文件及 linker 别名共 147750805 字节。本轮只复验其准备和输入检查，SDK ART 实际运行沿用既有独立结果。新 `runtime_source` 只区分两个固定输入，准备、fixture 编译、文件复制、运行和结果导出继续复用现有流程。`hyperos_inputs` 负责校验该来源清单及原始字节；通用 provider、运行时插件和第二套 JNI fixture 均无当前使用方，未引入这些接口。删除来源分支会失去原 HyperOS 固定文件集合校验，删除显式页大小会重新按 16 KiB 拒绝原库，保留两项必要状态。

阶段验收已完成。后续 JIT 观察采用独立记录与源码版本；完整 HyperOS、Direct 4 KiB 和 iOS 的验收条件仍未覆盖。
