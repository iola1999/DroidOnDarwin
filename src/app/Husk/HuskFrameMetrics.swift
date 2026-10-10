// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// 固定区间的耗时统计，单位为毫秒；空间用量与运行时长无关。
struct HuskTimingHistogram {
    static let bounds: [Double] = [0.1, 0.5, 1, 2, 4, 8, 16.7, 33.3, 50, 100, 250, 1000]
    private(set) var buckets = Array(repeating: 0, count: bounds.count + 1)
    private(set) var count = 0
    private(set) var total = 0.0
    private(set) var maximum = 0.0

    mutating func record(milliseconds: Double) {
        guard milliseconds.isFinite, milliseconds >= 0 else {
            return
        }
        let index = Self.bounds.firstIndex { milliseconds <= $0 } ?? Self.bounds.count
        buckets[index] += 1
        count += 1
        total += milliseconds
        maximum = max(maximum, milliseconds)
    }

    /// 返回 p95 所在区间的上界；超过最后一个区间时使用本次最大值。
    var p95Upper: Double? {
        guard count > 0 else {
            return nil
        }
        let rank = Int(ceil(Double(count) * 0.95))
        var seen = 0
        for (index, number) in buckets.enumerated() {
            seen += number
            if seen >= rank {
                return index < Self.bounds.count ? Self.bounds[index] : maximum
            }
        }
        return nil
    }

    var summary: String {
        guard count > 0 else {
            return "n=0"
        }
        return String(format: "n=%d avg=%.3f p95<=%.3f max=%.3f ms",
                      count, total / Double(count), p95Upper!, maximum)
    }
}

/// QEMU、Metal 完成回调与采样线程共享。每次只在锁内累计数值。
final class HuskFrameMetrics: @unchecked Sendable {
    static let shared = HuskFrameMetrics()

    struct Window {
        var callbacks = 0
        var submitted = 0
        var completed = 0
        var failed = 0
        var displayed = 0
        var noDrawable = 0
        var lockWait = HuskTimingHistogram()
        var drawable = HuskTimingHistogram()
        var encode = HuskTimingHistogram()
        var callback = HuskTimingHistogram()
        var interval = HuskTimingHistogram()
        var gpu = HuskTimingHistogram()
        var completion = HuskTimingHistogram()
        var displayLatency = HuskTimingHistogram()

        var summary: String {
            "metal callbacks=\(callbacks) submitted=\(submitted) completed=\(completed) "
                + "failed=\(failed) displayed=\(displayed) noDrawable=\(noDrawable) "
                + "lock{\(lockWait.summary)} drawable{\(drawable.summary)} "
                + "encode{\(encode.summary)} callback{\(callback.summary)} "
                + "interval{\(interval.summary)} gpu{\(gpu.summary)} "
                + "completion{\(completion.summary)} displayLatency{\(displayLatency.summary)}"
        }
    }

    private let lock = NSLock()
    private var window = Window()
    private var lastCallback: Double?

    func recordCallback(start: Double, lockWait: Double, drawableWait: Double?,
                        encode: Double?, elapsed: Double, submitted: Bool, noDrawable: Bool) {
        lock.lock()
        defer { lock.unlock() }
        window.callbacks += 1
        if submitted {
            window.submitted += 1
        }
        if noDrawable {
            window.noDrawable += 1
        }
        window.lockWait.record(milliseconds: lockWait * 1000)
        if let drawableWait {
            window.drawable.record(milliseconds: drawableWait * 1000)
        }
        if let encode {
            window.encode.record(milliseconds: encode * 1000)
        }
        window.callback.record(milliseconds: elapsed * 1000)
        if let previous = lastCallback {
            window.interval.record(milliseconds: (start - previous) * 1000)
        }
        lastCallback = start
    }

    func recordCompletion(elapsed: Double, gpu: Double?, failed: Bool) {
        lock.lock()
        defer { lock.unlock() }
        window.completed += 1
        if failed {
            window.failed += 1
        }
        window.completion.record(milliseconds: elapsed * 1000)
        if let gpu {
            window.gpu.record(milliseconds: gpu * 1000)
        }
    }

    func recordDisplay(elapsed: Double) {
        lock.lock()
        defer { lock.unlock() }
        window.displayed += 1
        window.displayLatency.record(milliseconds: elapsed * 1000)
    }

    /// 完成和显示事件按回调到达时间计入窗口，可能对应前一窗口提交的帧。
    func takeWindow() -> Window {
        lock.lock()
        defer { lock.unlock() }
        let result = window
        window = Window()
        return result
    }
}
