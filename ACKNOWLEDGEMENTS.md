**DroidOnDarwin · 开源致谢**

DroidOnDarwin 在 Darwin 上集成 Linux/LKL、ARM64 原生执行和完整 Android 用户态。项目从 Husk 的代码仓库开始开发，保留并修改了其中的 iOS JIT 支持。感谢以下项目及其贡献者。

**Husk 与 iOS JIT**

[Husk](https://github.com/Leviidev/Husk) 的 JIT 双映射、断点请求汇编、调试脚本、内置 JIT 扩展和扩展通信代码，是本项目相关实现的来源。内置 StikJIT 集成包含 Dan Perks 的贡献，来源提交为 `d86ba33` 和 `0903066`。保留代码遵循 GPL-2.0-or-later；本项目的后续修改见 Git 历史。

双映射实现还沿用上游注明的 AetherPS4-iOS `src/core/ios/ios_jit_allocator.cpp` 来源及 shadPS4 Emulator Project 的 GPL-2.0-or-later 许可。调试脚本和汇编实现使用 StikDebug / BreakpointJIT 的断点协议。扩展启动方式参考 [LiveContainer](https://github.com/LiveContainer/LiveContainer) 使用的 NSExtension 接口。

iOS 包含 [StikJIT 1.9.0](https://github.com/StikDebug/StikJIT/tree/1.9.0) 的未修改框架，遵循 MPL-2.0；其内部的 [idevice](https://github.com/jkcoxson/idevice) 遵循 MIT 许可。框架上游归档的 SHA-256 为 `806664393770c68e75f2b6429955bfdd88cfaad09fec2ba70f8ed615ff90c060`。

**Linux、Android 与图形**

[Linux Kernel Library](https://github.com/lkl/linux) 提供 Linux 内核；[AOSP](https://source.android.com/) 提供 Android 系统及 ART、Bionic 等组件。用户态工具还使用 musl、dash、LLVM compiler-rt 等项目。各组件适用其自身许可，具体版本与构建输入见项目构建清单。

图形实现使用 [gfxstream](https://github.com/google/gfxstream) 和 [WebKit 中的 ANGLE](https://github.com/WebKit/WebKit/tree/main/Source/ThirdParty/ANGLE)。本仓库保存平台适配源码和补丁。

**实验执行器**

`experiments/native-linux/direct/` 的 ARM64 生成器使用 [Oaknut](https://github.com/merryhime/oaknut) 头文件，版本为 `94c726ce0338b054eb8cb5ea91de8fe6c19f4392`，版权归 merryhime，遵循 MIT 许可。独立对照和未覆盖指令的执行使用 [Unicorn](https://github.com/unicorn-engine/unicorn) 2.1.4。构建脚本下载的源码保留上游许可文件；这些实验组件尚未进入产品包。

项目源码中的 SPDX 标识和原有版权声明继续有效。仓库根目录 `LICENSE` 保存 GPL v2 全文；可修改版本范围以各文件的 GPL-2.0-or-later 标识及依赖自身许可为准。
