# 决策记录: DroidOnDarwin 命名与项目整理

状态: implemented

## 问题

当前应用已采用 Linux/LKL、ARM64 原生执行与 Android 系统集成，仓库仍包含早期执行路线的应用、构建工具和专用测试。用户选择 DroidOnDarwin，要求统一产品名称、包标识、界面和项目文档，清理闲置实现，保留实际来源与许可声明。用户确认开发阶段直接使用新数据目录，并授权修改 GitHub 仓库名称与 remote。

## 决策

应用显示名为 DroidOnDarwin，用户选择 A 版 DoD 字标。iOS 包标识为 `app.droidondarwin.ios`，扩展为 `app.droidondarwin.ios.JITHelper`；macOS 为 `app.droidondarwin.mac`。两端注册 `app.droidondarwin.image`，镜像目录包扩展名为 `.droidimage`，iOS URL scheme 为 `droidondarwin`。版本为 0.14.0，iOS 构建号 83，Mac 构建号 3。

普通界面使用 Android、JIT 连接和运行组件等名称。技术实现中的 NCE 类型、函数、环境变量及现有实验目录继续沿用。iOS 数据位于新容器的 Application Support/DroidOnDarwin，Mac 位于 Application Support/app.droidondarwin.mac；首个内置设备只创建记录，首次启动再复制数据盘种子。旧数据迁入分支及其专用检查已删除。

当前运行所需的 JIT 文件、扩展、资源和图形构建函数已迁出旧应用目录。旧 QEMU 应用、应用兼容层、Dynarmic/Direct、bulk 实验及其专用工具、脚本和补丁已删除。保留完整 Android 的内核、用户态、图形、设备管理与回归检查，图形构建直接生成 gfxstream 库。当前输出目录中的 9 个旧 app、IPA、生成工程及签名配置也已清理。

仓库以 [ACKNOWLEDGEMENTS.md](../../../../ACKNOWLEDGEMENTS.md) 记录 Husk、AetherPS4-iOS、StikJIT、LiveContainer 及 Linux/Android/图形组件的实际关联，原有版权与许可文本继续保留。两端设置及 Mac 关于窗口使用同一份致谢资源，当前展示方式见[开源致谢文本](2026-10-10-开源致谢文本.md)。

GitHub 仓库为 `iola1999/DroidOnDarwin`，描述更新为 Linux and Android on Darwin with native ARM64 execution。`origin` 和 gh 默认仓库已更新，旧 `upstream` 关联已移除；仓库可见性保持 private。本轮未推送代码。

当前工作区起点为 `c93cd7a`，开始时无未提交修改。最近交付的共享 SwiftUI、设备管理与 macOS 子进程生命周期继续保留。历史测量中的原始包名、日志、哈希与源码路径保留，相关旧路线记录明确注明历史范围。当前 README、JIT 指南、许可说明与组件说明按现有实现更新。

### 复杂度审查

按 reclaim-code-entropy 检查实际调用方、构建输入、动态入口和移除后的行为。

- 图形构建从旧执行实验脚本导入的 run/apply 两个函数，已放回实际使用它们的构建脚本；Rutabaga 构建分支及锁文件随旧 QEMU 路线删除。
- JIT 只保留固定 RX 区间准备、RW 别名、断点保护与分离。预分配 TCG/EL0 分区、MAP_JIT 探测、可用状态缓存及无人调用的公共入口已删除；外部断点请求协议继续由现有检查覆盖。
- 主应用只发送启用 JIT 请求。独立 prepare/resetDDI 消息、未使用的 TXM 返回字段、配对来源枚举和本机配对说明已删除。StikJIT.enableJIT 继续执行其所需的准备步骤。
- 当前构建入口、SwiftUI 页面、共享状态、目录锁和 Android 运行组件均有实际调用方。删除这些部分会影响导入、运行、停止或交付，因此保留。
- 两端 UI 共用一个致谢视图。图标由一个 AppKit 脚本生成两端所需资源，未增加第三方依赖。

### 验证

环境为 Apple Silicon / macOS 27.0，使用当前 Xcode 的 iOS 27.0 SDK；验证结果记录在 [results-droidondarwin-0.14.0.json](../../../../experiments/native-linux/results-droidondarwin-0.14.0.json)。

- 更改前 JIT 协议和 Mac 宿主检查成功。更改后原生执行检查、JIT 协议 10 项、用户态布局 21 项、分析工具 5 项与提交钩子 12 项检查成功。
- 重新构建 iOS/macOS ANGLE 和 gfxstream；完整桌面 CLI、GCD、可见窗口分别通过 138、138、139 项不同名称的检查，含实际桌面与应用像素、数据持久化和正常关机。
- 两端构建成功。Mac 宿主检查覆盖首次初始化、独立数据、导入失败、目录锁、状态、窗口生命周期、签名执行和动态库；包内资源、源码和文件哈希检查成功。
- IPA 和 app 的包标识、版本、镜像类型、图标资源、许可及深度签名检查成功；iOS 扩展的实际 Objective-C 类名与 Info.plist 一致，随包 JIT 脚本与源码一致，IPA 路径均使用新名称。
- Mac 实际打开关于窗口、设置中的致谢页及许可分享面板；初始窗口尺寸过小的问题已修正并重新检查。新应用从独立测试目录启动 Android，设备列表达到运行状态，并完成正常停止检查。
- 镜像打包脚本成功生成 `.droidimage`。Python 编译、格式检查及 Git 差异空白检查成功。
- 当前 IPA 的真机安装、重签后的扩展连接与 JIT 执行仍待集中验证。本机结果只覆盖上述构建、协议、宿主与资源检查。

## 曾考虑的替代方案

- 仅修改显示名称：包标识、扩展协议、资源、旧构建入口和文档仍会不一致，采用完整整理。
- 全仓库机械替换：会改变上游网址、版权说明和历史测量，按当前实现、来源说明与历史记录处理。
- 重写已验证的 JIT 实现：更名无需改变外部设备执行协议，保留必需实现及来源，删除无人调用的分支。
- 两端分别维护致谢文本：会产生重复维护，因此从仓库同一份文本打包。

## 影响

新包使用独立标识与数据目录。构建清单按整理后的实际源码重新生成。旧实现可通过 Git 历史查看；当前交付只包含现有运行组件。与初期计划相比，本轮还删除了已有的旧根目录数据迁入分支，并按用户补充要求完成 GitHub 改名和 remote 更新。核心执行架构及已交付的设备管理范围保持原定目标。
