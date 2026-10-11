// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

@main
struct DisplayFrameCaptureChecks {
    static func main() throws {
        let capture = DisplayFrameCapture(capacity: 3)
        precondition(capture.acquiring(layer: "off") == nil)
        try capture.begin()
        do {
            try capture.begin()
            preconditionFailure("duplicate capture start accepted")
        } catch {}
        let pending = capture.acquiring(layer: "first")!
        let skipped = capture.acquiring(layer: "first")!
        let unavailable = capture.acquiring(layer: "first")!
        precondition(capture.acquiring(layer: "first") == nil)
        capture.acquired(pending, drawableID: 7)
        capture.acquired(skipped, drawableID: 8)
        capture.acquired(unavailable, drawableID: nil)
        capture.end()
        precondition(capture.acquiring(layer: "ended") == nil)
        capture.presented(skipped, time: 0)
        capture.presented(skipped, time: 0)
        let ended = capture.snapshot()!
        precondition(ended.end != nil && ended.frames.count == 3 && ended.overflow == 1)
        precondition(ended.duplicate_callbacks == 1 && ended.clock_errors == 0)
        precondition(ended.frames[0].callback_ns == nil && ended.frames[1].presented_time_seconds == 0)
        precondition(ended.frames[2].drawable_unavailable)
        // An old drawable can complete after a new capture uses the same slot.
        try capture.begin()
        let current = capture.acquiring(layer: "second")!
        capture.acquired(current, drawableID: 9)
        capture.presented(pending, time: 100)
        precondition(capture.snapshot()!.frames[0].callback_ns == nil)
        capture.end()
        capture.presented(current, time: 200)
        let final = capture.snapshot()!
        precondition(final.frames[0].presented_time_seconds == 200 && final.duplicate_callbacks == 0 && final.overflow == 0)
        let data = try JSONEncoder().encode(final)
        let decoded = try JSONDecoder().decode(DisplayFrameCapture.Snapshot.self, from: data)
        precondition(decoded.frames[0].drawable_id == 9)
        print("display capture: inactive path, overflow, pending/skipped drawables, late callbacks and generation isolation passed")
    }
}
