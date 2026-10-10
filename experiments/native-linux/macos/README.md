# Husk NCE for macOS

原生 AppKit 应用，在 Apple Silicon Mac 上运行仓库现有的 Linux / Android 与 NCE。当前包使用 1 GiB Linux RAM、demand 映射、360×640 Android 画面和 gfxstream / ANGLE Metal。最低系统为 macOS 27.0，由当前 gfxstream 构建的部署目标决定。

## 构建与检查

在仓库根目录运行：

```sh
python3 scripts/package_native_linux_macos.py
python3 scripts/test_native_linux_macos.py
```

打包前要求 `work/native-linux-nce/processes/lkl-summary.json` 中的 CLI、GCD、窗口桌面检查成功，且源码、内核、生成物和动态库哈希仍匹配。需要重新生成这些输入时使用 `--rebuild-kernel`。输出已存在时，先退出应用，再使用 `--replace` 重建；应用数据独立保存。

输出为 `work/native-linux-nce/macos/Husk NCE.app`，可以从 Finder 打开。内核、系统镜像、空白数据盘种子、图形动态库和第三方许可随包提供。`package.json` 记录构建标识、输入与包内文件哈希，`host-checks.json` 记录宿主检查结果。

## 使用

鼠标点击、拖动对应 Android 触摸操作。底部提供返回、主页和最近应用按钮，Android 菜单提供启动状态、日志目录、数据目录和停止入口。缩放窗口时保持画面比例和逻辑分辨率。输入入口在桌面就绪后启用。

数据默认保存在 `~/Library/Application Support/app.husk.nce.mac/`。首次启动创建 `android-data.img`，后续启动沿用该文件；每次运行生成独立的 `Logs/session-*.log`。两个应用副本不能同时占用同一个数据目录。开发验证可通过 `--data-directory PATH` 指定隔离目录。

关闭窗口或 Cmd-Q 会请求 Android 停止，等待进程和图形资源释放、数据盘转为只读后退出。重新打开应用可再次启动 Android。启动失败时通过弹窗和日志报告原因，已有数据盘保持原位。

## 当前边界

当前交付为本机 ad-hoc 签名包，开启 Hardened Runtime，并使用 `allow-unsigned-executable-memory` 和 `disable-library-validation` 例外。前者用于现有 `vm_remap` 代码映射，后者用于本机签名图形库的加载。Developer ID、公证与下载到其他机器后的启动尚待发布验证。

桌面沿用 SDK Android、Launcher3、AOSP DeskClock 和 DocumentsUI。HyperOS 3、物理键盘、中文输入和触控板滚轮仍需各自适配。执行核心与 iOS 共用，此入口只负责 Mac 窗口、数据管理和应用生命周期。
