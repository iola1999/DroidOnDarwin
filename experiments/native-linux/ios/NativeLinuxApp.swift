// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UniformTypeIdentifiers
import Metal

// The existing JIT helper uses this logger in both app targets.
enum HuskLog {
    static func log(_ category: String, _ message: String) {
        nce_log("[\(category)] \(message)")
    }
}

@MainActor
final class ProbeModel: ObservableObject {
    @Published var status = "准备启动 Android"
    @Published var desktopReady = false
    @Published var stopping = false
    @Published var logLines: [ProbeLogLine] = []
    @Published var busy = false {
        didSet {
            if !busy {
                polling?.invalidate()
                polling = nil
            }
        }
    }
    @Published var didRun = false
    @Published var importing = false
    @Published var performanceLogging = false
    let metalLayer = CAMetalLayer()
    private let logReader = ProbeLogReader()
    private var phase: String?
    private var refreshing = false
    private var waiting = false
    private var attachDeadline = Date.distantPast
    private var polling: Timer?
    private var startedAt = Date()
    let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
    var logURL: URL {
        documents.appendingPathComponent("native-linux-n2-ios.log")
    }
    var paired: Bool {
        JITPairingFileStore.exists
    }

    init() {
        husk_ios_jit_install_trap_handler()
        metalLayer.device = MTLCreateSystemDefaultDevice()
        metalLayer.pixelFormat = .bgra8Unorm
        metalLayer.framebufferOnly = false
        metalLayer.drawableSize = CGSize(width: 360, height: 640)
        nce_graphics_set_window(Unmanaged.passUnretained(metalLayer).toOpaque())
        Task {
            await refresh()
            if !busy && !didRun && !logLines.isEmpty {
                status = "已保留上次日志"
            }
        }
    }

    private func refresh() async {
        if let snapshot = try? await logReader.read(from: logURL) {
            if snapshot.lines != logLines {
                logLines = snapshot.lines
            }
            phase = snapshot.phase
            desktopReady = busy && didRun && snapshot.phase == "android_desktop_running"
        }
    }
    private func tick() async {
        guard busy && !refreshing else {
            return
        }
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
            if stopping {
                status = "正在停止 Android"
                return
            }
            if desktopReady {
                status = "Android 正在运行"
                return
            }
            let seconds = Int(Date().timeIntervalSince(startedAt))
            let label = phase.flatMap { ProbeLogReader.phaseLabels[$0] } ?? "正在准备 JIT 代码区域"
            let nextStatus = "\(label)（\(seconds) 秒）"
            if status != nextStatus {
                status = nextStatus
            }
        }
    }
    private func beginAttempt() -> Bool {
        guard !busy && !didRun else {
            return false
        }
        guard nce_open_log(logURL.path) == 0 else {
            status = "日志文件无法写入"
            return false
        }
        busy = true
        logLines = []
        phase = nil
        Task { await logReader.reset()
            await refresh()
        }
        polling = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            Task { @MainActor in await self?.tick() }
        }
        UIApplication.shared.isIdleTimerDisabled = true
        startedAt = Date()
        let build = Bundle.main.object(forInfoDictionaryKey: "NCEBuildID") as? String ?? "unknown"
        HuskLog.log("probe", "N4 iOS; build=\(build); iOS=\(UIDevice.current.systemVersion); pid=\(getpid())")
        return true
    }
    private func script() -> Data? {
        guard let path = Bundle.main.url(forResource: "husk-jit", withExtension: "js") else {
            return nil
        }
        return try? Data(contentsOf: path)
    }
    func builtIn() {
        guard let pairing = try? JITPairingFileStore.data() else {
            status = "先导入这台 iPhone 的配对文件，或使用 StikDebug"
            return
        }
        guard let js = script(), beginAttempt() else {
            return
        }
        status = "正在连接手机内的 JIT 助手"
        waiting = true
        attachDeadline = Date().addingTimeInterval(90)
        HuskBuiltInJIT.send(.enable(targetPID: getpid(), pairingData: pairing,
                                    scriptBase64: js.base64EncodedString())) { [weak self] result in
            guard let self else {
                return
            }
            var failure: String?
            switch result {
            case .success(let response):
                HuskLog.log("jit-helper", response.message)
                if !response.success {
                    failure = response.message
                }
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
        guard let js = script(), let bundleID = Bundle.main.bundleIdentifier else {
            return
        }
        var components = URLComponents(string: "stikjit://enable-jit")!
        let encoded = js.base64EncodedString().replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_").replacingOccurrences(of: "=", with: "")
        components.queryItems = [URLQueryItem(name: "bundle-id", value: bundleID),
                                 URLQueryItem(name: "script-data", value: encoded)]
        guard let url = components.url, UIApplication.shared.canOpenURL(url) else {
            status = "未找到 StikDebug"
            return
        }
        guard beginAttempt() else {
            return
        }
        status = "请在 StikDebug 中启用 JIT，然后返回 Husk"
        waiting = true
        attachDeadline = Date().addingTimeInterval(180)
        UIApplication.shared.open(url)
    }
    func runAttached() {
        guard nce_debugger_attached(), beginAttempt() else {
            return
        }
        run()
    }
    private func run() {
        guard !didRun else {
            return
        }
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
            let disk = directory.appendingPathComponent("android-data.img")
            if !manager.fileExists(atPath: disk.path) {
                guard let seed = Bundle.main.url(forResource: "android-data-seed", withExtension: "img") else {
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
        setenv("NCE_DESKTOP", "1", 1)
        setenv("NCE_MM_MODE", "demand", 1)
        setenv("NCE_MAP_PROFILE", performanceLogging ? "1" : "0", 1)
        startedAt = Date()
        status = "正在准备 JIT 内存"
        // The guest switches stacks and pthreads. Keep the Swift UI on its own queue.
        DispatchQueue.global(qos: .userInitiated).async {
            let result = nce_lkl_run(path)
            DispatchQueue.main.async {
                self.busy = false
                UIApplication.shared.isIdleTimerDisabled = false
                self.desktopReady = false
                self.stopping = false
                self.status = result == 0 ? "Android 已停止" : "启动失败，请导出日志"
                Task { await self.refresh() }
            }
        }
    }
    func stop() {
        guard busy && didRun && !stopping else {
            return
        }
        stopping = true
        status = "正在停止 Android"
        DispatchQueue.global(qos: .userInitiated).async {
            let result = nce_session_request_stop()
            if result != 0 {
                DispatchQueue.main.async {
                    self.stopping = false
                    self.status = "启动准备尚未完成，请稍后停止"
                    HuskLog.log("session", "stop request returned \(result)")
                }
            }
        }
    }
    func importPairing(_ result: Result<[URL], Error>) {
        do {
            guard let url = try result.get().first else {
                return
            }
            try JITPairingFileStore.importFile(from: url)
            status = "配对文件已导入"
        } catch { status = error.localizedDescription }
    }
}

private enum GuestInput {
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

@main
struct NativeLinuxApp: App {
    @StateObject private var model = ProbeModel()
    @State private var showDetails = false
    var body: some Scene {
        WindowGroup {
            VStack(spacing: 0) {
                GuestMetalDisplay(metalLayer: model.metalLayer)
                    .aspectRatio(360.0 / 640.0, contentMode: .fit)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .background(.black)
                HStack {
                    Group {
                        Button("返回") { GuestInput.send(Int32(NCE_INPUT_KEY), 158) }
                        Spacer()
                        Button("主页") { GuestInput.send(Int32(NCE_INPUT_KEY), 102) }
                        Spacer()
                        Button("最近应用") { GuestInput.send(Int32(NCE_INPUT_KEY), 580) }
                    }
                    .disabled(!model.desktopReady || model.stopping)
                    Spacer()
                    Menu {
                        Button("连接与日志") { showDetails = true }
                        if model.busy && model.didRun {
                            Button("停止 Android", role: .destructive) { model.stop() }
                                .disabled(model.stopping)
                        }
                    } label: { Image(systemName: "ellipsis.circle") }
                        .accessibilityLabel("更多操作")
                }
                .padding()
                if !model.desktopReady || model.stopping {
                    VStack(spacing: 12) {
                        Text(model.status).font(.headline).accessibilityIdentifier("probe-status")
                        if model.busy {
                            ProgressView()
                        }
                        if !model.busy && !model.didRun {
                            Button("启动 Android") { model.builtIn() }
                                .buttonStyle(.borderedProminent)
                            Button("连接与日志") { showDetails = true }
                        }
                        if model.didRun && !model.busy {
                            Text("重新启动前，请从多任务界面关闭 Husk 后再打开。")
                                .font(.caption).foregroundStyle(.secondary)
                        }
                    }.padding()
                }
            }
            .sheet(isPresented: $showDetails) {
                NavigationStack {
                    List {
                        Section {
                            Text(model.status).font(.headline)
                            Text(Bundle.main.object(forInfoDictionaryKey: "NCEBuildID") as? String ?? "NCE")
                                .font(.caption).foregroundStyle(.secondary)
                        }
                        Section("连接") {
                            Button("启用内置 JIT 并启动") { model.builtIn()
                                showDetails = false
                            }
                            Button("通过 StikDebug 启动") { model.external()
                                showDetails = false
                            }
                            if nce_debugger_attached() {
                                Button("使用已连接的 JIT 启动") { model.runAttached()
                                    showDetails = false
                                }
                            }
                            Button(model.paired ? "更换配对文件" : "导入配对文件") { model.importing = true }
                        }.disabled(model.busy || model.didRun)
                        Section {
                            Toggle("性能统计", isOn: $model.performanceLogging)
                                .disabled(model.busy || model.didRun)
                        } footer: {
                            Text("启动前开启，统计会写入导出日志。计时会增加运行开销。")
                        }
                        if !model.logLines.isEmpty {
                            Section("日志") {
                                ShareLink("导出日志", item: model.logURL)
                                Text("显示最近 80 行，导出可查看完整日志。")
                                    .font(.caption).foregroundStyle(.secondary)
                                ForEach(model.logLines) { line in
                                    Text(line.text).font(.system(.caption, design: .monospaced)).lineLimit(4)
                                }
                            }
                        }
                    }
                    .navigationTitle("连接与日志")
                    .navigationBarTitleDisplayMode(.inline)
                    .toolbar { ToolbarItem(placement: .confirmationAction) { Button("完成") { showDetails = false } } }
                    .fileImporter(isPresented: $model.importing, allowedContentTypes: [.data], allowsMultipleSelection: false) {
                        model.importPairing($0)
                    }
                }
            }
        }
    }
}
