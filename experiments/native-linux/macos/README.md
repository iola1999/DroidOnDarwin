# DroidOnDarwin for macOS

SwiftUI 界面与 AppKit 窗口宿主，在 Apple Silicon Mac 上运行仓库现有的 Linux / Android 与 NCE。当前包使用 1 GiB Linux RAM、demand 映射、360×640 Android 画面和 gfxstream / ANGLE Metal。最低系统为 macOS 27.0，由当前 gfxstream 构建的部署目标决定。

## 构建与检查

在仓库根目录运行：

```sh
python3 scripts/package_native_linux_macos.py
python3 scripts/test_native_linux_macos.py
```

打包前要求 `work/native-linux-nce/processes/lkl-summary.json` 中的 CLI、GCD、窗口桌面检查成功，且源码、内核、生成物和动态库哈希仍匹配。需要重新生成这些输入时使用 `--rebuild-kernel`。输出已存在时，先退出应用，再使用 `--replace` 重建；应用数据独立保存。

输出为 `work/native-linux-nce/macos/DroidOnDarwin.app`，可以从 Finder 打开。内核、系统镜像、空白数据盘种子、图形动态库和第三方许可随包提供。`package.json` 记录构建标识、输入与包内文件哈希，`host-checks.json` 记录宿主检查结果。

## 使用

打开应用进入设备列表。可以添加内置 Android 设备、导入镜像包、查看详情和删除已停止的设备。每次只运行一台设备，点击启动后打开 Android 窗口；停止会话后保留设备列表，可以再次启动或切换设备。删除前需要确认，并同时删除该设备的数据与导入镜像。

鼠标点击、拖动对应 Android 触摸操作。底部提供返回、主页和最近应用按钮，画面顶部菜单提供运行详情和停止入口，设备详情提供日志与数据目录入口。缩放窗口时保持画面比例和逻辑分辨率。启动时显示当前阶段与等待时间，桌面首帧就绪后启用输入。日志预览按需打开，最多保留 80 行，每秒读取量上限为 256 KiB，完整日志保存在设备目录。

数据默认保存在 `~/Library/Application Support/app.droidondarwin.mac/Devices/<id>/`。每台设备首次启动创建独立的 `android-data.img`，后续启动沿用该文件；每次运行生成 `Logs/session-*.log`。设备目录锁持续到会话进程退出，设备列表也有独立的进程锁。开发验证可通过 `--data-directory PATH` 指定隔离目录。

关闭 Android 窗口或在设备列表选择停止，会请求 Android 结束，等待进程和图形资源释放、数据盘转为只读后关闭会话。关闭设备列表会先停止当前 Android。启动失败时在设备详情显示失败状态，运行日志保留失败原因。

设置提供手动诊断启动、性能统计和原始镜像导入。普通启动保留运行必要的初始化与校验；合成画面、截图、Binder 往返及独立运行库测试通过诊断入口执行。镜像格式与生成方式见[设备镜像说明](../app/README.md)。

## 当前边界

当前交付为本机 ad-hoc 签名包，开启 Hardened Runtime，并使用 `allow-unsigned-executable-memory` 和 `disable-library-validation` 例外。前者用于现有 `vm_remap` 代码映射，后者用于本机签名图形库的加载。Developer ID、公证与下载到其他机器后的启动尚待发布验证。

桌面沿用 SDK Android、Launcher3、AOSP DeskClock 和 DocumentsUI。HyperOS 3、物理键盘、中文输入和触控板滚轮仍需各自适配。执行核心、主要页面、设备操作和会话状态与 iOS 共用；Mac 入口负责窗口与运行进程生命周期。
