# DroidOnDarwin

[GitHub 源码仓库](https://github.com/iola1999/DroidOnDarwin)

在 iPhone 和 Apple Silicon Mac 上独立运行 Linux 与 Android。Linux 内核通过 LKL 集成到宿主进程，ARM64 用户态代码使用原生执行，图形通过 gfxstream、ANGLE 和 Metal 显示。手机运行所需的内核、系统镜像和动态库随应用交付。

当前处于开发阶段：两端共用设备列表、镜像导入、启动状态、日志与设置界面；Android 桌面使用 SDK Android、Launcher3、AOSP DeskClock 和 DocumentsUI。HyperOS 3 集成仍在后续范围内。当前真机结果与限制见[轮次工作记录](.agents/notes/implemented/process/2026-10-07-轮次工作记录.md)。

## 构建

使用 Apple Silicon Mac、Xcode 命令行工具，以及 Python 3、Node.js、CMake、Ninja、XcodeGen、LLVM/LLD、GNU Make、GNU sed、e2fsprogs、Java 17 和 Android SDK 36。各构建脚本固定上游版本，下载与生成物位于 `work/`、`build/` 和被忽略的 `third_party/gpu/`。首次构建会下载源码与系统组件。

```sh
bash scripts/build_angle_ios.sh iphoneos
bash scripts/build_angle_ios.sh macosx
python3 scripts/build_gfxstream.py --platform ios
python3 scripts/build_gfxstream.py --platform macos-ios-api
python3 scripts/probe_native_linux.py
NCE_MM_MODE=demand NCE_SYSTEM_SERVER_PROBE=1 python3 scripts/probe_lkl.py \
  --processes --graphics-angle work/angle-macos/prefix/lib --graphics-window
python3 scripts/package_native_linux_macos.py
python3 scripts/test_native_linux_macos.py
python3 scripts/package_native_linux_ios.py
```

打包要求 CLI、GCD 和可见窗口检查成功，且源码、内核、图形库和生成物与验证清单一致。重新生成桌面检查可使用打包脚本的 `--rebuild-kernel`；替换已存在且未运行的 Mac 包使用 `--replace`。

产物：

- macOS：`work/native-linux-nce/macos/DroidOnDarwin.app`，包标识 `app.droidondarwin.mac`。
- iOS：`work/native-linux-nce/ios/DroidOnDarwin-0.14.0.ipa`，包标识 `app.droidondarwin.ios`，JIT 扩展为 `app.droidondarwin.ios.JITHelper`。

Mac 包使用本机 ad-hoc 签名；Developer ID、公证及其他机器的分发验证尚待完成。iOS 包需要通过支持应用扩展的侧载工具重新签名，运行时需要 JIT 连接。最低目标版本为 iOS 26.0；Mac 最低版本由图形动态库的部署目标决定，当前构建为 macOS 27.0。

## 使用与开发

打开应用后在设备列表添加或导入设备。镜像包扩展名为 `.droidimage`，格式与构建方式见[设备与镜像说明](experiments/native-linux/app/README.md)。Mac 操作、数据目录和退出行为见[macOS 说明](experiments/native-linux/macos/README.md)。iPhone 的配对文件、LocalDevVPN 和 StikDebug 设置见[JIT 连接说明](.agents/notes/implemented/process/2026-10-05-内置JIT与设备配对.md)。

执行核心、Android 集成和两端应用位于 `experiments/native-linux/`；iOS JIT 支持位于 `src/ios-jit/` 和 `src/app/`；图形适配位于 `src/gfxstream-ios/`。`scripts/` 保留当前构建、打包、诊断和检查入口，[patches/README.md](patches/README.md) 说明上游补丁。

开发约定见 [AGENTS.md](AGENTS.md)。提交前运行 `bash scripts/format.sh --check`；本地提交钩子通过 `git config --local core.hooksPath .githooks` 启用。

## 来源与许可

项目保留 GPL-2.0-or-later 源文件许可，全文见 [LICENSE](LICENSE)。复用与参考关系、上游项目和第三方许可见[开源致谢](ACKNOWLEDGEMENTS.md)，两端应用的设置和 Mac 关于窗口也提供致谢及许可导出。
