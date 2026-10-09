// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UniformTypeIdentifiers

// The existing JIT helper uses this logger in both app targets.
enum HuskLog {
    static func log(_ category: String, _ message: String) {
        nce_log("[\(category)] \(message)")
    }
}

@MainActor
final class ProbeModel: ObservableObject {
    @Published var status = "准备测试"
    @Published var logLines: [ProbeLogLine] = []
    @Published var busy = false {
        didSet {
            if !busy { polling?.invalidate(); polling = nil }
        }
    }
    @Published var didRun = false
    @Published var importing = false
    @Published var graphics: UIImage?
    private let logReader = ProbeLogReader()
    private var phase: String?
    private var refreshing = false
    private var waiting = false
    private var attachDeadline = Date.distantPast
    private var polling: Timer?
    private var startedAt = Date()
    let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
    var logURL: URL { documents.appendingPathComponent("native-linux-n2-ios.log") }
    var graphicsURL: URL { documents.appendingPathComponent("framework.png") }
    var paired: Bool { JITPairingFileStore.exists }

    init() {
        husk_ios_jit_install_trap_handler()
        Task {
            await refresh()
            if !busy && !didRun && !logLines.isEmpty { status = "已保留上次日志" }
        }
    }

    private func refresh() async {
        if let snapshot = try? await logReader.read(from: logURL) {
            if snapshot.lines != logLines { logLines = snapshot.lines }
            phase = snapshot.phase
        }
    }
    private func tick() async {
        guard busy && !refreshing else { return }
        refreshing = true
        defer { refreshing = false }
        await refresh()
        if waiting {
            if UIApplication.shared.applicationState == .active && nce_debugger_attached() {
                waiting = false
                run()
            } else if Date() > attachDeadline {
                waiting = false
                busy = false
                UIApplication.shared.isIdleTimerDisabled = false
                status = "JIT 连接超时，请检查 LocalDevVPN 后重新打开应用"
                HuskLog.log("probe", status)
            }
        } else if busy && didRun {
            let seconds = Int(Date().timeIntervalSince(startedAt))
            let label = phase.flatMap { ProbeLogReader.phaseLabels[$0] } ?? "正在准备 JIT 代码区域"
            let nextStatus = "\(label)（\(seconds) 秒）"
            if status != nextStatus { status = nextStatus }
        }
    }
    private func beginAttempt() -> Bool {
        guard !busy && !didRun else { return false }
        guard nce_open_log(logURL.path) == 0 else { status = "日志文件无法写入"; return false }
        busy = true
        logLines = []
        phase = nil
        Task { await logReader.reset(); await refresh() }
        polling = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            Task { @MainActor in await self?.tick() }
        }
        UIApplication.shared.isIdleTimerDisabled = true
        startedAt = Date()
        let build = Bundle.main.object(forInfoDictionaryKey: "NCEBuildID") as? String ?? "unknown"
        HuskLog.log("probe", "N3 iOS; build=\(build); iOS=\(UIDevice.current.systemVersion); pid=\(getpid())")
        return true
    }
    private func script() -> Data? {
        guard let path = Bundle.main.url(forResource: "husk-jit", withExtension: "js") else { return nil }
        return try? Data(contentsOf: path)
    }
    func builtIn() {
        guard let pairing = try? JITPairingFileStore.data() else {
            status = "先导入这台 iPhone 的配对文件，或使用 StikDebug"
            return
        }
        guard let js = script(), beginAttempt() else { return }
        status = "正在连接手机内的 JIT 助手"
        waiting = true
        attachDeadline = Date().addingTimeInterval(90)
        HuskBuiltInJIT.send(.enable(targetPID: getpid(), pairingData: pairing,
                                   scriptBase64: js.base64EncodedString())) { [weak self] result in
            guard let self else { return }
            var failure: String?
            switch result {
            case .success(let response):
                HuskLog.log("jit-helper", response.message)
                if !response.success { failure = response.message }
            case .failure(let error): failure = error.localizedDescription
            }
            if let failure {
                HuskLog.log("jit-helper", failure)
                if !didRun {
                    waiting = false
                    busy = false
                    UIApplication.shared.isIdleTimerDisabled = false
                    status = failure
                }
            }
        }
    }
    func external() {
        guard let js = script(), let bundleID = Bundle.main.bundleIdentifier else { return }
        var components = URLComponents(string: "stikjit://enable-jit")!
        let encoded = js.base64EncodedString().replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_").replacingOccurrences(of: "=", with: "")
        components.queryItems = [URLQueryItem(name: "bundle-id", value: bundleID),
                                 URLQueryItem(name: "script-data", value: encoded)]
        guard let url = components.url, UIApplication.shared.canOpenURL(url) else {
            status = "未找到 StikDebug"
            return
        }
        guard beginAttempt() else { return }
        status = "请在 StikDebug 中启用 JIT，然后返回 Husk"
        waiting = true
        attachDeadline = Date().addingTimeInterval(180)
        UIApplication.shared.open(url)
    }
    func runAttached() {
        guard nce_debugger_attached(), beginAttempt() else { return }
        run()
    }
    private func run() {
        guard !didRun else { return }
        guard let path = Bundle.main.path(forResource: "lkl-image", ofType: "bin") else {
            busy = false
            UIApplication.shared.isIdleTimerDisabled = false
            status = "安装包缺少 Linux 内核"
            return
        }
        do {
            let manager = FileManager.default
            var directory = manager.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
                .appendingPathComponent("NativeLinux", isDirectory: true)
            try manager.createDirectory(at: directory, withIntermediateDirectories: true)
            var attributes = URLResourceValues()
            attributes.isExcludedFromBackup = true
            try directory.setResourceValues(attributes)
            let disk = directory.appendingPathComponent("data.img")
            if !manager.fileExists(atPath: disk.path) {
                guard let seed = Bundle.main.url(forResource: "storage-seed", withExtension: "img") else {
                    throw CocoaError(.fileNoSuchFile)
                }
                try manager.copyItem(at: seed, to: disk)
            }
            guard let system = Bundle.main.path(forResource: "system", ofType: "img") else {
                throw CocoaError(.fileNoSuchFile)
            }
            setenv("NCE_SYSTEM_IMAGE", system, 1)
            setenv("NCE_STORAGE_IMAGE", disk.path, 1)
        } catch {
            busy = false
            UIApplication.shared.isIdleTimerDisabled = false
            status = "存储初始化失败：\(error.localizedDescription)"
            HuskLog.log("storage", status)
            return
        }
        didRun = true
        setenv("NCE_MM_MODE", "lazy", 1)
        setenv("NCE_STRESS_ROUNDS", "3", 1)
        setenv("NCE_GRAPHICS_OUTPUT", graphicsURL.path, 1)
        startedAt = Date()
        status = "正在准备 JIT 内存"
        // The guest switches stacks and pthreads. Keep the Swift UI on its own queue.
        DispatchQueue.global(qos: .userInitiated).async {
            let result = nce_lkl_run(path)
            DispatchQueue.main.async {
                self.busy = false
                UIApplication.shared.isIdleTimerDisabled = false
                self.status = result == 0 ? "全部测试通过" : "测试失败，请导出日志"
                Task { await self.refresh() }
                self.graphics = result == 0 ? UIImage(contentsOfFile: self.graphicsURL.path) : nil
            }
        }
    }
    func importPairing(_ result: Result<[URL], Error>) {
        do {
            guard let url = try result.get().first else { return }
            try JITPairingFileStore.importFile(from: url)
            status = "配对文件已导入"
        } catch { status = error.localizedDescription }
    }
}

@main
struct NativeLinuxApp: App {
    @StateObject private var model = ProbeModel()
    var body: some Scene {
        WindowGroup {
            NavigationStack {
                List {
                    Section("N3 · Android 运行时") {
                        Text(Bundle.main.object(forInfoDictionaryKey: "NCEBuildID") as? String ?? "N3")
                            .font(.caption).foregroundStyle(.secondary)
                        Text(model.status).font(.headline).accessibilityIdentifier("probe-status")
                        Text("运行 Android 运行库、Binder、Java 与编译执行测试。请保持应用在前台。")
                            .foregroundStyle(.secondary)
                        if model.busy { ProgressView() }
                    }
                    Section {
                        Button("启用内置 JIT 并测试") { model.builtIn() }
                        Button("通过 StikDebug 启用并测试") { model.external() }
                        if nce_debugger_attached() {
                            Button("使用已连接的 JIT 测试") { model.runAttached() }
                        }
                        Button(model.paired ? "更换配对文件" : "导入配对文件") { model.importing = true }
                    }.disabled(model.busy || model.didRun)
                    if model.didRun && !model.busy {
                        Section { Text("再次测试前，请从多任务界面关闭 Husk 后重新打开。") }
                    }
                    if let graphics = model.graphics {
                        Section("Android 绘图") {
                            Image(uiImage: graphics).resizable().interpolation(.none)
                                .scaledToFit().frame(maxWidth: 384)
                            ShareLink("导出图片", item: model.graphicsURL)
                        }
                    }
                    if !model.logLines.isEmpty {
                        Section("日志") {
                            ShareLink("导出测试日志", item: model.logURL)
                            Text("显示最近 80 行，导出可查看完整日志。")
                                .font(.caption).foregroundStyle(.secondary)
                            ForEach(model.logLines) { line in
                                Text(line.text).font(.system(.caption, design: .monospaced))
                                    .lineLimit(4)
                            }
                        }
                    }
                }
                .navigationTitle("Husk NCE")
                .fileImporter(isPresented: $model.importing, allowedContentTypes: [.data], allowsMultipleSelection: false) {
                    model.importPairing($0)
                }
            }
        }
    }
}
