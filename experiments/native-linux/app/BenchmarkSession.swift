// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Combine
#if os(iOS)
    import UIKit
#endif

@MainActor
final class BenchmarkSession {
    private let request: BenchmarkRequest
    private let session: SessionModel
    private let directory: URL
    private let target: RunTarget
    private let resources: [String: Any]
    private let input = DispatchQueue(label: "app.droidondarwin.benchmark-input", qos: .userInitiated)
    private var task: Task<Void, Never>?
    private var watchdog: Task<Void, Never>?
    private var phaseObserver: AnyCancellable?
    private var stepObserver: AnyCancellable?
    private var memoryTask: Task<Void, Never>?
    private var memory = BenchmarkMemory()
    private var memoryPhase = "preparing"
    private var memoryBootStep = 0
    private var memoryLastWrite: Double = 0
    #if os(iOS)
        private var backgroundObserver: AnyCancellable?
    #endif
    private var started = ProcessInfo.processInfo.systemUptime
    private var events: [[String: Any]] = []
    private var inputTimings: [[String: Int64]] = []
    private var failure: String?
    private var finished = false
    var runID: UUID {
        request.runID
    }
    var onFinish: (() -> Void)?

    init(request: BenchmarkRequest, session: SessionModel, target: RunTarget, output: URL) throws {
        guard !session.active, !session.restartRequired, request.deviceID == target.id else {
            throw BenchmarkError.invalid("设备正在运行、需要重启 App 或编号不匹配")
        }
        directory = output.appendingPathComponent(request.runID.uuidString, isDirectory: true)
        guard !FileManager.default.fileExists(atPath: directory.path) else {
            throw BenchmarkError.invalid("运行编号已经存在")
        }
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        try JSONEncoder().encode(request).write(to: directory.appendingPathComponent("request.json"), options: .atomic)
        self.request = request
        self.session = session
        var manifests: [String: Any] = [:]
        for name in ["Runtime.json", "manifest.json"] {
            let path = target.resources.appendingPathComponent(name)
            if let data = try? Data(contentsOf: path), let value = try? JSONSerialization.jsonObject(with: data) {
                manifests[name] = value
            }
        }
        resources = manifests
        self.target = RunTarget(id: target.id, name: target.name, directory: target.directory,
                                resources: target.resources, log: directory.appendingPathComponent("session.log"))
    }

    func start() throws {
        started = ProcessInfo.processInfo.systemUptime
        try sampleMemory()
        try record("accepted")
        let previousCompletion = session.onFinish
        session.onFinish = { [weak self] in
            guard let self else {
                previousCompletion?()
                return
            }
            session.onFinish = previousCompletion
            // Mac 的原回调会退出进程，结果必须先保存完成。
            complete()
            previousCompletion?()
        }
        phaseObserver = session.$phase.dropFirst().sink { [weak self] phase in
            guard let self, !finished else {
                return
            }
            // 结束回调可同步退出；在 Published 通知中采样，保留极短的停止阶段。
            memoryPhase = String(describing: phase)
            do { try sampleMemory() } catch { failure = failure ?? error.localizedDescription }
            // Published 在赋值前通知；下一次主线程任务读取完整会话状态。
            Task { @MainActor [weak self] in
                guard let self, !finished else {
                    return
                }
                do { try record(String(describing: phase)) } catch { fail(error.localizedDescription) }
                if phase == .failed {
                    fail(session.status)
                }
            }
        }
        stepObserver = session.$bootStep.removeDuplicates().dropFirst().sink { [weak self] step in
            guard let self, !finished else {
                return
            }
            memoryBootStep = step
            do { try sampleMemory() } catch { failure = failure ?? error.localizedDescription }
        }
        #if os(iOS)
            backgroundObserver = NotificationCenter.default.publisher(for: UIApplication.didEnterBackgroundNotification).sink { [weak self] _ in
                Task { @MainActor [weak self] in
                    guard let self, session.phase == .booting || session.phase == .running else {
                        return
                    }
                    fail("测试期间 App 进入后台")
                }
            }
        #endif
        session.start(target, configuration: request.configuration)
        if request.memorySampling != false {
            memoryTask = Task { [weak self] in
                while !Task.isCancelled {
                    do { try await Task.sleep(for: .seconds(1)) } catch { return }
                    guard let self, !finished else {
                        return
                    }
                    do {
                        try sampleMemory()
                        if ProcessInfo.processInfo.systemUptime - memoryLastWrite >= 10 {
                            try writeState("running")
                        }
                    } catch { fail(error.localizedDescription) }
                }
            }
        }
        watchdog = Task { [weak self] in
            guard let self else {
                return
            }
            do { try await Task.sleep(for: .seconds(request.timeoutSeconds)) } catch { return }
            fail("实验超时")
        }
        task = Task { [weak self] in
            guard let self else {
                return
            }
            do {
                while session.active && session.phase != .running {
                    try await Task.sleep(for: .milliseconds(100))
                }
                guard session.phase == .running else {
                    throw BenchmarkError.invalid("桌面未就绪")
                }
                try record("ready_observed")
                for (index, action) in request.actions.enumerated() {
                    try Task.checkCancellation()
                    guard session.phase == .running else {
                        throw BenchmarkError.invalid("操作期间 Android 已停止")
                    }
                    inputTimings = []
                    try record("action_started", index: index)
                    try await perform(action)
                    try record("action_submitted", index: index)
                }
                try record("actions_completed")
                session.stop()
            } catch is CancellationError {} catch { fail(error.localizedDescription) }
        }
    }

    func cancel() {
        fail("实验被停止")
    }

    private func fail(_ message: String) {
        guard !finished else {
            return
        }
        if failure == nil {
            failure = message
        }
        task?.cancel()
        watchdog?.cancel()
        session.stop()
        guard !finished else {
            return
        }
        do { try writeState("stopping") } catch { NSLog("Benchmark result: %@", error.localizedDescription) }
        if !session.active {
            complete()
        }
    }

    private func complete() {
        guard !finished else {
            return
        }
        finished = true
        task?.cancel()
        watchdog?.cancel()
        memoryTask?.cancel()
        phaseObserver = nil
        stepObserver = nil
        #if os(iOS)
            backgroundObserver = nil
        #endif
        if failure == nil && session.phase == .failed {
            failure = session.status
        }
        if failure == nil && !events.contains(where: { $0["event"] as? String == "actions_completed" }) {
            failure = "实验操作尚未完成，会话已结束"
        }
        do {
            try sampleMemory()
            try record(failure == nil ? "completed" : "failed")
        } catch { NSLog("Benchmark result: %@", error.localizedDescription) }
        onFinish?()
    }

    private func record(_ event: String, index: Int? = nil) throws {
        var monotonic = timespec()
        clock_gettime(CLOCK_MONOTONIC, &monotonic)
        var entry: [String: Any] = ["event": event, "host_monotonic_ns": Int64(monotonic.tv_sec) * 1_000_000_000 + Int64(monotonic.tv_nsec), "elapsed_ms": (ProcessInfo.processInfo.systemUptime - started) * 1000,
                                    "thermal_state": ProcessInfo.processInfo.thermalState.rawValue,
                                    "low_power_mode": ProcessInfo.processInfo.isLowPowerModeEnabled]
        if let index {
            entry["action"] = index
        }
        var usage = rusage()
        if getrusage(RUSAGE_SELF, &usage) == 0 {
            entry["process_cpu_us"] = Int64(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1_000_000 + Int64(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec)
        } else {
            entry["process_cpu_error"] = errno
        }
        if event == "action_submitted", !inputTimings.isEmpty {
            entry["input_timings"] = inputTimings
        }
        events.append(entry)
        try writeState(finished ? event : "running")
    }

    private func writeState(_ status: String) throws {
        if request.memorySampling != false {
            try memory.flush(to: directory.appendingPathComponent("memory.jsonl"))
            memoryLastWrite = ProcessInfo.processInfo.systemUptime
        }
        var machine = utsname()
        uname(&machine)
        let capacity = MemoryLayout.size(ofValue: machine.machine)
        let hardware = withUnsafePointer(to: &machine.machine) {
            $0.withMemoryRebound(to: CChar.self, capacity: capacity) { String(cString: $0) }
        }
        let memorySummary: [String: Any] = request.memorySampling == false ? ["enabled": false] : memory.json
        var value: [String: Any] = ["version": 1, "runID": request.runID.uuidString, "status": status,
                                    "deviceID": target.id, "buildID": Bundle.main.object(forInfoDictionaryKey: "DroidOnDarwinBuildID") as? String ?? "unknown",
                                    "hardware": hardware, "os": ProcessInfo.processInfo.operatingSystemVersionString,
                                    "host_ram_bytes": ProcessInfo.processInfo.physicalMemory,
                                    "resources": resources,
                                    "memory": memorySummary,
                                    "configuration": try JSONSerialization.jsonObject(with: JSONEncoder().encode(request.configuration)),
                                    "events": events, "session_active": session.active,
                                    "log": "session.log", "timing": "monotonic; ready observed through 1-second log polling; action submission excludes guest rendering completion"]
        if let failure {
            value["error"] = failure
        }
        try JSONSerialization.data(withJSONObject: value, options: [.sortedKeys]).write(
            to: directory.appendingPathComponent("result.json"), options: .atomic)
    }

    private func sampleMemory() throws {
        guard request.memorySampling != false else {
            return
        }
        try memory.add(.current(), elapsedMS: (ProcessInfo.processInfo.systemUptime - started) * 1000,
                       phase: memoryPhase, bootStep: memoryBootStep)
    }

    private func send(_ action: Int32, _ x: Int32, _ y: Int32 = 0) async throws {
        let enqueued = ProcessInfo.processInfo.systemUptime
        let (result, began, ended): (Int32, Double, Double) = await withCheckedContinuation { continuation in
            input.async {
                let began = ProcessInfo.processInfo.systemUptime
                let result = nce_host_input_send(action, x, y)
                continuation.resume(returning: (result, began, ProcessInfo.processInfo.systemUptime))
            }
        }
        let resumed = ProcessInfo.processInfo.systemUptime
        // These boundaries separate queue delay, synchronous Linux entry and
        // resumption on the main actor; guest consumption remains asynchronous.
        inputTimings.append(["action": Int64(action), "queue_us": Int64((began - enqueued) * 1_000_000),
                             "call_us": Int64((ended - began) * 1_000_000), "resume_us": Int64((resumed - ended) * 1_000_000),
                             "result": Int64(result)])
        guard result == 0 else {
            throw BenchmarkError.invalid("输入提交失败：\(result)")
        }
        try Task.checkCancellation()
    }

    private func perform(_ action: BenchmarkRequest.Action) async throws {
        switch action.kind {
        case "profile":
            let commands: [String: Int32] = ["settings": Int32(NCE_PROFILE_SETTINGS), "frames-start": Int32(NCE_PROFILE_FRAMES),
                                             "trace-start": Int32(NCE_PROFILE_SCHEDULER), "capture-end": Int32(NCE_PROFILE_END)]
            let output = directory.appendingPathComponent("interaction", isDirectory: true)
            try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)
            let command = commands[action.key!]!
            let result: Int32 = await withCheckedContinuation { continuation in
                input.async { continuation.resume(returning: nce_session_profile(command, output.path)) }
            }
            guard result == 0 else {
                throw BenchmarkError.invalid("交互采集失败：\(result)")
            }
            try Task.checkCancellation()
        case "wait": try await Task.sleep(for: .milliseconds(action.milliseconds!))
        case "key": try await send(Int32(NCE_INPUT_KEY), ["home": Int32(NCE_KEY_HOME), "back": Int32(NCE_KEY_BACK), "recent": Int32(NCE_KEY_RECENT)][action.key!]!)
        case "tap":
            try await send(Int32(NCE_INPUT_DOWN), Int32(action.x!), Int32(action.y!))
            try await Task.sleep(for: .milliseconds(40))
            try await send(Int32(NCE_INPUT_UP), Int32(action.x!), Int32(action.y!))
        case "swipe":
            let count = max(1, action.milliseconds! / 16)
            try await send(Int32(NCE_INPUT_DOWN), Int32(action.x!), Int32(action.y!))
            for step in 1 ... count {
                try await Task.sleep(for: .milliseconds(action.milliseconds! / count))
                let x = action.x! + (action.endX! - action.x!) * step / count
                let y = action.y! + (action.endY! - action.y!) * step / count
                try await send(Int32(NCE_INPUT_MOVE), Int32(x), Int32(y))
            }
            try await send(Int32(NCE_INPUT_UP), Int32(action.endX!), Int32(action.endY!))
        default: throw BenchmarkError.invalid("未知实验操作")
        }
    }
}
