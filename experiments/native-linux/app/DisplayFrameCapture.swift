// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Metal
import QuartzCore

// ANGLE owns command submission; attaching to its existing drawable records
// display completion without adding a readback or a second presentation.
final class ProfiledMetalLayer: CAMetalLayer, @unchecked Sendable {
    private let captureID = UUID().uuidString
    override func nextDrawable() -> (any CAMetalDrawable)? {
        let capture = DisplayFrameCapture.shared
        guard let ticket = capture.acquiring(layer: captureID) else {
            return super.nextDrawable()
        }
        let drawable = super.nextDrawable()
        capture.acquired(ticket, drawableID: drawable.map { UInt64($0.drawableID) })
        drawable?.addPresentedHandler { value in
            capture.presented(ticket, time: value.presentedTime)
        }
        return drawable
    }
}

final class DisplayFrameCapture: @unchecked Sendable {
    static let shared = DisplayFrameCapture()
    struct Ticket: Sendable {
        let generation: UInt64
        let index: Int
    }
    struct ClockSample: Codable {
        let host_before_ns: UInt64
        let media_seconds: Double
        let host_after_ns: UInt64
    }
    struct Frame: Codable {
        let layer: String
        let acquire_begin_ns: UInt64
        var acquire_end_ns: UInt64?
        var drawable_id: UInt64?
        var drawable_unavailable = false
        var presented_time_seconds: Double?
        var callback_ns: UInt64?
    }
    struct Snapshot: Codable {
        let version: Int
        let capacity: Int
        let begin: ClockSample
        let end: ClockSample?
        let overflow: UInt64
        let duplicate_callbacks: UInt64
        let clock_errors: UInt64
        let frames: [Frame]
    }
    private let lock = NSLock()
    private let capacity: Int
    private var generation: UInt64 = 0
    private var active = false
    private var beginSample: ClockSample?
    private var endSample: ClockSample?
    private var frames: [Frame] = []
    private var overflow: UInt64 = 0
    private var duplicateCallbacks: UInt64 = 0
    private var clockErrors: UInt64 = 0

    init(capacity: Int = 2048) {
        precondition(capacity > 0)
        self.capacity = capacity
    }
    private func now() -> UInt64 {
        var time = timespec()
        guard clock_gettime(CLOCK_MONOTONIC, &time) == 0 else {
            clockErrors += 1
            return 0
        }
        return UInt64(time.tv_sec) * 1_000_000_000 + UInt64(time.tv_nsec)
    }
    private func clockSample() -> ClockSample {
        let before = now(), media = CACurrentMediaTime(), after = now()
        return ClockSample(host_before_ns: before, media_seconds: media, host_after_ns: after)
    }
    func begin() throws {
        lock.lock()
        defer { lock.unlock() }
        guard !active else {
            throw BenchmarkError.invalid("显示采集已经开始")
        }
        generation &+= 1
        frames.removeAll(keepingCapacity: true)
        frames.reserveCapacity(capacity)
        overflow = 0
        duplicateCallbacks = 0
        clockErrors = 0
        endSample = nil
        beginSample = clockSample()
        active = true
    }
    func end() {
        lock.lock()
        defer { lock.unlock() }
        guard active else {
            return
        }
        endSample = clockSample()
        active = false
    }
    func acquiring(layer: String) -> Ticket? {
        lock.lock()
        defer { lock.unlock() }
        guard active else {
            return nil
        }
        guard frames.count < capacity else {
            overflow += 1
            return nil
        }
        let ticket = Ticket(generation: generation, index: frames.count)
        frames.append(Frame(layer: layer, acquire_begin_ns: now()))
        return ticket
    }
    func acquired(_ ticket: Ticket, drawableID: UInt64?) {
        lock.lock()
        defer { lock.unlock() }
        guard ticket.generation == generation, frames.indices.contains(ticket.index) else {
            return
        }
        frames[ticket.index].acquire_end_ns = now()
        frames[ticket.index].drawable_id = drawableID
        frames[ticket.index].drawable_unavailable = drawableID == nil
    }
    func presented(_ ticket: Ticket, time: Double) {
        lock.lock()
        defer { lock.unlock() }
        // Callbacks may arrive after end or after another capture has begun.
        // Keep late callbacks for the same generation so export can distinguish
        // pending, skipped, and presentations outside the measured interval.
        guard ticket.generation == generation, frames.indices.contains(ticket.index) else {
            return
        }
        guard frames[ticket.index].callback_ns == nil else {
            duplicateCallbacks += 1
            return
        }
        frames[ticket.index].callback_ns = now()
        frames[ticket.index].presented_time_seconds = time
    }
    func snapshot() -> Snapshot? {
        lock.lock()
        defer { lock.unlock() }
        guard let beginSample else {
            return nil
        }
        return Snapshot(version: 1, capacity: capacity, begin: beginSample, end: endSample,
                        overflow: overflow, duplicate_callbacks: duplicateCallbacks,
                        clock_errors: clockErrors, frames: frames)
    }
    func write(to file: URL) throws {
        guard let value = snapshot() else {
            return
        }
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        try encoder.encode(value).write(to: file, options: .atomic)
    }
}
