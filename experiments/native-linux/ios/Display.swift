// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import Metal

enum GuestInput {
    static let queue = DispatchQueue(label: "app.husk.android-input", qos: .userInteractive)
    static func send(_ action: Int32, _ x: Int32, _ y: Int32 = 0) {
        queue.async { _ = nce_host_input_send(action, x, y) }
    }
}

struct GuestMetalDisplay: UIViewRepresentable {
    let metalLayer: CAMetalLayer
    final class DisplayView: UIView {
        var metalLayer: CAMetalLayer?
        private var activeTouch: UITouch?
        private func send(_ touch: UITouch, action: Int32) {
            guard bounds.width > 0, bounds.height > 0 else {
                return
            }
            let point = touch.location(in: self)
            let x = Int32(max(0, min(359, point.x * 360 / bounds.width)))
            let y = Int32(max(0, min(639, point.y * 640 / bounds.height)))
            GuestInput.send(action, x, y)
        }
        override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
            guard activeTouch == nil, let touch = touches.first else {
                return
            }
            activeTouch = touch
            send(touch, action: Int32(NCE_INPUT_DOWN))
        }
        override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
            guard let touch = activeTouch, touches.contains(touch) else {
                return
            }
            send(touch, action: Int32(NCE_INPUT_MOVE))
        }
        override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
            guard let touch = activeTouch, touches.contains(touch) else {
                return
            }
            send(touch, action: Int32(NCE_INPUT_UP))
            activeTouch = nil
        }
        override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
            touchesEnded(touches, with: event)
        }
        override func layoutSubviews() {
            super.layoutSubviews()
            CATransaction.begin()
            CATransaction.setDisableActions(true)
            metalLayer?.frame = bounds
            CATransaction.commit()
        }
    }
    func makeUIView(context: Context) -> DisplayView {
        let view = DisplayView()
        view.metalLayer = metalLayer
        view.layer.addSublayer(metalLayer)
        return view
    }
    func updateUIView(_ view: DisplayView, context: Context) {}
}
