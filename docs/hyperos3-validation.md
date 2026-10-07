# HyperOS 3 启动验证

本次使用 HyperOS-AVD 的 `OS3.0.2.0.WMCCNXM` ext4 系统镜像，以及 Husk v12
发布的基础磁盘。在 Mac 上，QEMU 10.0.12 TCG 已进入桌面和设置，关于页面显示
HyperOS 3.0.2.0、Android 16。手机导入和启动结果记录在本文后续部分。

## 磁盘与启动流程

`scripts/prepare_hyperos3.py` 保留 Husk 的内核、vendor 和启动分区，在 vda 的
GPT 末尾追加 `hyperos` 分区，并将 HyperOS 系统分区写入其中。vdb 继续作为
userdata，现有 Husk 的两块磁盘参数可以直接使用。

脚本只接受 SHA-256 为
`a8dbaecc8fdcd682991b078d6bbb7df6972e459208634fa8dcbad24f17208d6a`
的原始 Husk v12 基础镜像。它会：

- 修改 vendor_boot v4 ramdisk 中的 GSI fstab，将 system 从 `/dev/block/vdc`
  改为 `/dev/block/by-name/hyperos`，保留 CPIO 权限、所有者和其余内容。
- 更新 GRUB 参数，选择 GSI 入口、360×800 分辨率、160 dpi 和低性能模式。
  此实验使用 SELinux Permissive 和 insecure ADB。
- 补充系统 CIL 中 vendor 所需的 `firmware_load`、`nlmsg` 权限声明。
- 在 Android init 阶段关闭 SwiftShader shader cache 预编译，并加入 Husk 的
  TCP 5599 shell 服务和 Ethernet feature 声明。
- 为新 userdata 清空基础盘的 metadata，生成主、副 GPT 和 CRC。

```sh
python3 scripts/prepare_hyperos3.py \
  --base /path/to/vda-v12.qcow2 \
  --system /path/to/system.img \
  --output /path/to/new-output-directory
```

脚本使用 macOS 的 `cp -c`，依赖 `qemu-img`、`lz4`、`mcopy` 和 `debugfs`。
输出目录必须尚未存在。输出中的 `work/` 保存构建中间文件，会占用额外磁盘空间。

## Mac 验证结果

测试使用 4 个 vCPU、4 GiB RAM、CPU 渲染，QEMU TCG 开启多线程和 256 MiB
translation buffer。QEMU 10.0.12 源码应用了 Husk 仓库的同版本补丁。

| 检查 | 结果 |
| --- | --- |
| 主、副 GPT 与分区表 CRC | 成功 |
| vendor_boot 解包后检查两条 fstab 设备路径 | 成功 |
| qcow2 结构检查 | 成功 |
| 初次启动到 `sys.boot_completed=1` | 约 197 秒 guest uptime |
| 进入桌面、设置和关于页面 | 成功 |
| 设置进入关于页面 | Android `TotalTime=9137 ms`，主机观察约 10.78 秒 |
| 关闭动画后切回已打开的设置 | Android `TotalTime=2165 ms`，主机观察约 3.42 秒 |
| 手机固件、360×800 初始显示、关闭声音、已初始化 userdata 冷启动 | 成功进入桌面 |
| 压缩导出与来源盘逐扇区比较 | 两块磁盘内容一致 |

这些测量覆盖的操作很少，交互仍然明显缓慢。Mac 的 HVF 运行明显更快；手机端
Husk 目前使用 TCG，需要单独测量。

首次初始化发生一次 SystemUI 锁屏杂志回调空指针异常。为手机准备的镜像来自
已经初始化、进入桌面并完成 `sync` 的系统盘和数据盘，设置了保持唤醒、关闭
锁屏和关闭窗口动画。使用手机固件再次冷启动时未复现该 SystemUI 异常；关闭
声音时，vendor 的 `setup_sound_card` 因缺少声卡发生一次 `stoi` 异常，系统仍
完成启动。

QEMU 11.0.1 的部分 TCG 测试出现 system_server/ART 崩溃。不同测试的初始化
状态和运行负载存在差异，目前尚未确定是否由 QEMU 版本本身引起。QEMU 10.0.12
已完成约 16 分钟的初次测试，并完成上述第二次冷启动。

Mac Cocoa 显示后端曾在切换分辨率时访问失效的图像缓冲区。测试用 QEMU 的
`ui/cocoa.m` 改为让 CoreGraphics 持有帧数据副本，补丁保存于
`work/qemu10-cocoa-lifetime.patch`。Husk iOS 使用自己的显示后端，本次未将该
Mac 补丁加入 iOS 源码。

## 手机导入文件

已初始化的磁盘导出到 `work/hyperos3-import/`，使用 qcow2 zlib 压缩，两块磁盘
均没有 backing file。`manifest.json` 记录文件大小和 SHA-256。

系统盘和数据盘必须成对使用：系统盘 metadata 包含对应 userdata 的加密信息。
导入版本标记分别为 `lineage-guest.version=v12` 和
`lineage-vdb.qcow2.seed=v10`。镜像摘要记录导出文件的实际 SHA-256。

build 30 使用 `husk-custom-guest` 标记保护导入镜像。当前实验配置已清除原
官方快照标记，使用 360×800 CPU 渲染冷启动。配对目录和共享目录保留。

## 变更范围

新增一个镜像准备脚本和一份 Android init 配置；分区检查工具允许超过八位的
十六进制偏移，以读取 4 GiB 以后的分区。GPT、ramdisk 和 ext4 编辑各自直接
调用标准库或现有命令行工具。移除其中任一部分会分别导致分区无法发现、fstab
仍查找第三块磁盘，或缺少 HyperOS 兼容配置，因此这些处理均予以保留。

后续的应用层改动和实机测量见 [性能优化记录](hyperos3-performance.md)。
