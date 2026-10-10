// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit

// 窗口操作栏高度固定，等比约束只作用于 Android 显示区域。
final class AndroidWindow: NSWindow {
    static let chromeHeight: CGFloat = 90
    func fittedFrame(_ proposed: NSRect) -> NSRect {
        let content = contentRect(forFrameRect: proposed)
        let screenHeight = (screen ?? NSScreen.main)?.visibleFrame.height ?? 900
        let titleHeight = proposed.height - content.height
        let maxWidth = max(240, (screenHeight - titleHeight - Self.chromeHeight) * 360 / 640)
        let width = min(max(240, content.width), maxWidth)
        let size = frameRect(forContentRect: NSRect(x: 0, y: 0, width: width,
                                                    height: width * 640 / 360 + Self.chromeHeight)).size
        return NSRect(x: proposed.minX, y: proposed.maxY - size.height, width: size.width, height: size.height)
    }
    override func setFrame(_ frameRect: NSRect, display flag: Bool) {
        super.setFrame(fittedFrame(frameRect), display: flag)
    }
}
