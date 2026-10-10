# DroidOnDarwin 设备与镜像

macOS 与 iOS 共用 SwiftUI 的设备列表、详情、添加、设置、启动页与日志页。`DeviceLibrary` 管理设备操作，`SessionModel` 管理会话状态，`DeviceStore` 负责设备记录、导入、重命名和删除。系统盘只读，每台设备使用独立的数据盘。应用打开后进入设备列表；首次使用建立内置设备记录，首次启动时复制数据盘种子。

## 完整镜像包

通过设备列表的导入入口选择 `.droidimage` 目录包。构建完成后可生成镜像包：

```sh
python3 scripts/package_native_linux_image.py work/Android-16.droidimage --name 'Android 16'
```

目录内固定包含 `manifest.json`、`system.img` 和 `android-data-seed.img`。配置格式为：

```json
{
  "format": 1,
  "name": "Android 16",
  "runtime": "运行组件 SHA-256 标识",
  "files": {
    "system.img": "文件 SHA-256",
    "android-data-seed.img": "文件 SHA-256"
  }
}
```

`runtime` 由内核、已准备的客体代码和程序布局生成，与 App 内的 `Runtime.json` 比较。导入时检查配置、运行组件版本、固定文件名、常规文件类型、ext4 大小及文件 SHA-256，复制和校验完成后才发布设备记录。内核使用 App 内对应版本。镜像包中的初始数据会成为新设备的数据；分发前应确认种子只包含打算交付的内容。

## 原始镜像

设置的高级入口可导入完整 ext4 `system.img`。数据盘种子与内核沿用 App 内版本。Android sparse 镜像需要先转换为完整 ext4 文件。原始镜像没有运行组件版本和校验清单，格式检查仅确认 ext4 头部与声明大小，Android ABI 和预处理代码兼容性由镜像制作者确认。

## 数据与停止

设备目录包含 `device.json`、`android-data.img` 和 `Logs/`；导入设备另有 `Images/`。UUID 作为目录名，显示名称不参与路径拼接。导入通过隐藏临时目录完成，中断导入的目录在下次打开设备列表时清理。设备列表持有目录锁，运行会话另持数据目录锁，禁止删除运行中的设备。

Mac 每次运行使用独立宿主进程，停止后可以再次启动。iOS 首版停止后返回列表，再次启动会提示从多任务界面关闭 DroidOnDarwin 后重新打开；后续范围见[iOS 连续重启 Android](../../../.agents/notes/proposed/feature/2026-10-10-iOS连续重启Android.md)。

## 两端开发检查

页面与共用状态位于当前目录，Mac 窗口和运行进程位于 `../macos/Application.swift`，iPhone 的显示与 JIT 设置位于 `../ios/`。`LocalRuntime` 负责直接运行核心，共用准备、停止与资源持有逻辑；iPhone JIT 连接由条件编译保留在对应平台。Mac 管理进程使用 `ProcessRuntime` 启动独立会话进程。

修改共用页面或行为时，完成两端构建与宿主回归：

```sh
python3 scripts/package_native_linux_macos.py --replace
python3 scripts/test_native_linux_macos.py
python3 scripts/package_native_linux_ios.py
```

验收覆盖设备创建、命名、切换、导入失败、运行中删除限制、启动阶段、停止中按钮状态、运行日志和退出。Mac 连续运行使用新进程；iPhone 的停止后重开提示和 JIT 仍需真机检查。
