# Husk

开发前阅读 [AGENTS.md](AGENTS.md) 和[改动记录约定](.agents/notes/README.md)。当前完整系统开发进展见[轮次工作记录](.agents/notes/implemented/process/2026-10-07-轮次工作记录.md)。

C、C++ 与 Objective-C 使用 clang-format 22.1.7，Swift 使用 SwiftFormat 0.63.1；macOS 可通过 `brew install clang-format swiftformat` 安装。运行 `bash scripts/format.sh` 格式化，或运行 `bash scripts/format.sh --check` 检查；命令末尾可指定文件或目录。配置与范围见 [C 系列格式化约定](.agents/notes/implemented/process/2026-10-10-C代码格式化与文档段落约定.md)和 [Swift 格式化约定](.agents/notes/implemented/process/2026-10-10-Swift存量与提交格式化.md)。

克隆后运行 `git config --local core.hooksPath .githooks` 启用提交前自动格式化。Hook 只处理本次暂存的 C 系列及 Swift 文件；同一文件还有未暂存修改时会中止，保留原有暂存范围。详见[提交前格式化约定](.agents/notes/implemented/process/2026-10-10-提交前自动格式化.md)。

<p align="center">
  <a href="https://trendshift.io/repositories/233057?utm_source=trendshift-badge&amp;utm_medium=badge&amp;utm_campaign=badge-trendshift-233057" target="_blank" rel="noopener noreferrer">
    <img src="https://trendshift.io/api/badge/trendshift/repositories/233057/daily?language=Swift" alt="Leviidev/Husk | Trendshift" width="250" height="55"/>
  </a>
</p>

[![Husk Downloads](https://img.shields.io/github/downloads/leviidev/husk/total?style=for-the-badge&color=5865F2&labelColor=111111)](https://github.com/leviidev/husk/releases)

Android app launcher for iOS.

Drop in an APK, tap it, and the Android app opens full-screen.

## JIT

Husk needs JIT, which on iOS takes an attached debugger. Use StikDebug, or Husk's built-in StikJIT helper (iOS 26+), which on iOS 27 can pair with your iPhone from Settings with no computer. The app walks you through it; see [the built-in JIT guide](.agents/notes/implemented/process/2026-10-05-内置JIT与设备配对.md).

## Builds

Every push builds an unsigned `Husk.ipa` in GitHub Actions (`build-ipa.yml`). It is attached to the run as an artifact, ready for AltStore, SideStore or TrollStore to sign and install. The first run builds QEMU and its dependencies from scratch, which takes a couple of hours; after that they are cached.

## Licence

GPL-2.0-or-later. Husk links QEMU, which is GPLv2, so the shipped binary is a combined GPLv2 work and the full source is public. It cannot go on the App Store — both because of that and because it needs `get-task-allow` plus a debugger attaching at runtime. See [the licensing record](.agents/notes/implemented/process/2026-09-11-许可与分发.md).
