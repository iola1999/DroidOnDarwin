// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Combine

struct RunTarget: Sendable {
    let id: String
    let name: String
    let directory: URL
    let resources: URL
    let log: URL
    init(device: NceDevice) {
        id = device.identifier
        name = device.name
        directory = device.directory
        resources = device.resources
        log = directory.appendingPathComponent("Logs/session-\(UUID().uuidString).log")
    }
    init(id: String, name: String, directory: URL, resources: URL, log: URL) {
        self.id = id
        self.name = name
        self.directory = directory
        self.resources = resources
        self.log = log
    }
}

enum SessionPhase: Equatable {
    case idle, preparing, connecting, booting, running, stopping, stopped, failed
    var active: Bool {
        [.preparing, .connecting, .booting, .running, .stopping].contains(self)
    }
}

enum RuntimeEvent {
    case connecting(String), booting, finished(String?)
}

@MainActor
protocol SessionRuntime: AnyObject {
    var restartRequired: Bool { get }
    func start(_ target: RunTarget, configuration: RunConfiguration, event: @escaping (RuntimeEvent) -> Void)
    func stop()
    func openDisplay()
}

@MainActor
final class SessionModel: ObservableObject {
    @Published private(set) var phase: SessionPhase = .idle
    @Published private(set) var status = "已停止"
    @Published private(set) var elapsed = 0
    @Published private(set) var bootStep = 0
    @Published private(set) var stepElapsed = 0
    @Published private(set) var activity = "正在准备运行资源"
    @Published private(set) var activityAge = 0
    private var stepStarted = Date()
    private var activityUpdated = Date()
    private var activitySequence: UInt64 = 0
    @Published private(set) var target: RunTarget?
    @Published var showDisplay = false
    private let runtime: any SessionRuntime
    private let reader = ProbeLogReader()
    private var timer: Timer?
    private var refreshing = false
    private var started = Date()
    var onFinish: (() -> Void)?
    var active: Bool {
        phase.active
    }
    var restartRequired: Bool {
        runtime.restartRequired
    }
    var canStop: Bool {
        active && phase != .stopping
    }

    init(runtime: any SessionRuntime) {
        self.runtime = runtime
    }
    func start(_ target: RunTarget, configuration: RunConfiguration = .init()) {
        guard !active, !runtime.restartRequired else {
            return
        }
        self.target = target
        phase = .preparing
        status = "正在准备设备"
        elapsed = 0
        started = Date()
        stepStarted = started
        activityUpdated = started
        bootStep = 0
        stepElapsed = 0
        activityAge = 0
        activitySequence = 0
        activity = "正在准备运行资源"
        showDisplay = true
        Task { await reader.reset() }
        startPolling()
        runtime.start(target, configuration: configuration) { [weak self] event in
            guard let self else {
                return
            }
            switch event {
            case .connecting(let text):
                guard phase != .stopping else {
                    return
                }
                phase = .connecting
                status = text
                activity = text
                activityUpdated = Date()
            case .booting:
                guard phase != .stopping else {
                    return
                }
                phase = .booting
                status = "正在启动 Linux 内核"
            case .finished(let error):
                phase = error == nil ? .stopped : .failed
                status = error ?? "Android 已停止"
                timer?.invalidate()
                timer = nil
                if error == nil {
                    showDisplay = false
                }
                onFinish?()
            }
        }
    }
    func openDisplay() {
        showDisplay = true
        runtime.openDisplay()
    }
    func stop() {
        guard canStop else {
            return
        }
        phase = .stopping
        status = "正在停止 Android"
        started = Date()
        elapsed = 0
        startPolling()
        runtime.stop()
    }
    private func startPolling() {
        guard timer == nil else {
            return
        }
        timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            Task { @MainActor in await self?.poll() }
        }
    }
    private func poll() async {
        guard active, !refreshing, let target else {
            return
        }
        refreshing = true
        defer { refreshing = false }
        let snapshot = try? await reader.read(from: target.log)
        guard active, self.target?.log == target.log else {
            return
        }
        elapsed = Int(Date().timeIntervalSince(started))
        stepElapsed = Int(Date().timeIntervalSince(stepStarted))
        activityAge = Int(Date().timeIntervalSince(activityUpdated))
        if phase == .booting, let snapshot {
            if let next = snapshot.step, next > bootStep {
                bootStep = next
                stepStarted = Date()
                stepElapsed = 0
            }
            if snapshot.activitySequence != activitySequence, let detail = snapshot.activity {
                activitySequence = snapshot.activitySequence
                activity = detail
                activityUpdated = Date()
                activityAge = 0
            }
        }
        guard phase == .booting, let key = snapshot?.phase else {
            return
        }
        if key == "android_desktop_running" {
            phase = .running
            status = "Android 正在运行"
            timer?.invalidate()
            timer = nil
        } else if let label = ProbeLogReader.phaseLabels[key] {
            status = label
        }
    }
}

// 日志页单独持有读取任务；关闭页面后释放，运行中的画面不轮询日志。
@MainActor
final class SessionLogModel: ObservableObject {
    @Published var lines: [ProbeLogLine] = []
    @Published var error: String?
    private let reader = ProbeLogReader()
    func observe(_ url: URL) async {
        do {
            while !FileManager.default.fileExists(atPath: url.path) {
                try await Task.sleep(for: .seconds(1))
            }
            lines = try await reader.tail(from: url)?.lines ?? []
            while !Task.isCancelled {
                try await Task.sleep(for: .seconds(1))
                if let snapshot = try await reader.read(from: url) {
                    lines = snapshot.lines
                }
            }
        } catch is CancellationError {} catch { self.error = error.localizedDescription }
    }
}

// Store 与 Storage 仅在串行队列操作；完成后才将结果交给主线程。
extension NceDevice: @unchecked Sendable {}
extension NceDeviceStore: @unchecked Sendable {}
extension NceStorage: @unchecked Sendable {}
