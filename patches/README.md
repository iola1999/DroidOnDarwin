# 上游补丁

当前补丁用于 Android 桌面的图形构建，构建脚本检查补丁能否正向应用或已应用。

| 文件 | 使用方 | 用途 |
| --- | --- | --- |
| `gfxstream-ios.patch` | `scripts/build_gfxstream.py` | iOS 构建、Darwin 平台接口与 EGL 适配 |
| `gfxstream-ios-serial-gles.patch` | 同上 | GLES 命令的串行处理 |
| `gfxstream-angle-shaders.patch` | 同上 | ANGLE 的 shader 处理适配 |
| `nce-gralloc-memfd.patch` | `scripts/build_android_gralloc.py` | Android gralloc 使用 memfd 共享缓冲区，并处理分配失败时的释放 |

平台适配源文件位于 `src/gfxstream-ios/`，构建时复制到对应的 gfxstream 源目录。上游版本由各脚本固定，补丁与适配文件的哈希写入构建清单。更新这些文件后重新构建图形库并运行桌面检查，再打包应用。

许可证与来源见[开源致谢](../ACKNOWLEDGEMENTS.md)。
