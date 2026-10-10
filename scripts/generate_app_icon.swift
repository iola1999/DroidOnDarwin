// SPDX-License-Identifier: GPL-2.0-or-later
// Regenerate the selected DoD wordmark at an exact 1024 x 1024 pixel size.
import AppKit

let output = CommandLine.arguments.dropFirst().first ?? "src/app/Resources/Assets.xcassets/AppIcon.appiconset/Icon-Default.png"
let mac = CommandLine.arguments.contains("--mac")
let context = CGContext(data: nil, width: 1024, height: 1024, bitsPerComponent: 8,
                        bytesPerRow: 4096, space: CGColorSpaceCreateDeviceRGB(),
                        bitmapInfo: (mac ? CGImageAlphaInfo.premultipliedLast : CGImageAlphaInfo.noneSkipLast).rawValue)!
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(cgContext: context, flipped: false)
if mac {
    context.translateBy(x: 96, y: 96)
    context.scaleBy(x: 832.0 / 1024, y: 832.0 / 1024)
    NSBezierPath(roundedRect: NSRect(x: 0, y: 0, width: 1024, height: 1024), xRadius: 190, yRadius: 190).addClip()
}
NSColor(srgbRed: 0.09, green: 0.14, blue: 0.20, alpha: 1).setFill()
NSRect(x: 0, y: 0, width: 1024, height: 1024).fill()
let foreground = NSColor(srgbRed: 0.73, green: 0.96, blue: 0.84, alpha: 1)
let text = "DoD" as NSString
let attributes: [NSAttributedString.Key: Any] = [
    .font: NSFont.monospacedSystemFont(ofSize: 260, weight: .bold), .foregroundColor: foreground,
]
let size = text.size(withAttributes: attributes)
text.draw(at: NSPoint(x: (1024 - size.width) / 2, y: (1024 - size.height) / 2 + 25), withAttributes: attributes)
foreground.setFill()
NSBezierPath(roundedRect: NSRect(x: 278, y: 238, width: 468, height: 18), xRadius: 9, yRadius: 9).fill()
NSGraphicsContext.restoreGraphicsState()
let bitmap = NSBitmapImageRep(cgImage: context.makeImage()!)
try bitmap.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: output))
