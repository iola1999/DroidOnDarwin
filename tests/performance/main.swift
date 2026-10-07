import Foundation

func require(_ condition: @autoclosure () -> Bool, _ message: String) {
    if !condition() { fatalError(message) }
}

var timing = HuskTimingHistogram()
require(timing.p95Upper == nil, "empty windows have no percentile")
for _ in 0..<95 { timing.record(milliseconds: 2) }
for _ in 0..<5 { timing.record(milliseconds: 1500) }
require(timing.count == 100 && timing.p95Upper == 2, "p95 must preserve the boundary rank")
require(timing.maximum == 1500, "long stalls must remain visible")
for value in [Double.nan, .infinity, -1] { timing.record(milliseconds: value) }
require(timing.count == 100, "invalid clock values must be ignored")
var overflow = HuskTimingHistogram()
overflow.record(milliseconds: 2000)
require(overflow.p95Upper == 2000, "overflow uses the observed maximum")

let metrics = HuskFrameMetrics()
metrics.recordCallback(start: 1, lockWait: 0, drawableWait: 0.05,
                       encode: 0.002, elapsed: 0.053, submitted: true, noDrawable: false)
let first = metrics.takeWindow()
require(first.callbacks == 1 && first.submitted == 1, "submission must be counted")
require(first.drawable.maximum == 50 && first.encode.maximum == 2, "seconds must convert to milliseconds")
require(first.interval.count == 0, "the first callback has no predecessor")
// GPU 完成可能发生在下一采样窗口，两个事件独立计数。
metrics.recordCompletion(elapsed: 0.004, gpu: 0.001, failed: false)
metrics.recordDisplay(elapsed: 0.016)
metrics.recordCallback(start: 1.1, lockWait: 0, drawableWait: 1,
                       encode: nil, elapsed: 1, submitted: false, noDrawable: true)
let second = metrics.takeWindow()
require(second.submitted == 0 && second.completed == 1 && second.displayed == 1,
        "completion in the next window must survive reset")
require(second.noDrawable == 1 && second.encode.count == 0, "dropped frames have no encoding time")
require(abs(second.interval.maximum - 100) < 0.001, "callback history must span windows")
require(metrics.takeWindow().callbacks == 0, "sampling must consume a window once")

DispatchQueue.concurrentPerform(iterations: 4000) { _ in
    metrics.recordCompletion(elapsed: 0.01, gpu: 0.005, failed: true)
}
let concurrent = metrics.takeWindow()
require(concurrent.completed == 4000 && concurrent.failed == 4000,
        "concurrent completion handlers must retain every event")
print("Frame metrics tests passed")
