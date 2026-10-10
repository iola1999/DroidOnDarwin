// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit
import SwiftUI

@MainActor
final class ProcessRuntime: SessionRuntime {
    private var process: Process?
    private var stopping = false
    var restartRequired: Bool {
        false
    }
    func start(_ target: RunTarget, diagnostics: Bool, performance: Bool, event: @escaping (RuntimeEvent) -> Void) {
        stopping = false
        let process = Process()
        self.process = process
        process.executableURL = Bundle.main.executableURL
        process.arguments = ["--run-device", "--data-directory", target.directory.path,
                             "--resources", target.resources.path, "--device-id", target.id,
                             "--device-name", target.name, "--session-log", target.log.path,
                             "--library-pid", String(getpid())]
        var environment = ProcessInfo.processInfo.environment
        environment.removeValue(forKey: "NCE_DIAGNOSTICS")
        environment.removeValue(forKey: "NCE_MAP_PROFILE")
        if diagnostics {
            environment["NCE_DIAGNOSTICS"] = "1"
        }
        if performance {
            environment["NCE_MAP_PROFILE"] = "1"
        }
        process.environment = environment
        process.terminationHandler = { task in
            DispatchQueue.main.async {
                self.process = nil
                event(.finished(task.terminationStatus == 0 || (self.stopping && task.terminationReason == .uncaughtSignal && task.terminationStatus == SIGTERM) ? nil : "Android 运行失败，请查看日志"))
                NSApp.activate(ignoringOtherApps: true)
            }
        }
        do { try process.run()
            event(.booting)
        } catch { self.process = nil
            event(.finished(error.localizedDescription))
        }
    }
    func stop() {
        stopping = true
        if process?.isRunning == true {
            process?.terminate()
        }
    }
    func openDisplay() {
        guard let process, let app = NSRunningApplication(processIdentifier: process.processIdentifier) else {
            return
        }
        app.unhide()
        app.activate(options: [.activateAllWindows])
    }
}

@MainActor
final class MacEnvironment {
    static let shared = MacEnvironment()
    let session: SessionModel
    let runtime: LocalRuntime?
    let library: DeviceLibrary?
    let workerTarget: RunTarget?
    private let libraryPID: pid_t?
    private var stopSignal: DispatchSourceSignal?
    private init() {
        let args = CommandLine.arguments
        func value(_ flag: String) -> String? {
            guard let index = args.firstIndex(of: flag), args.indices.contains(index + 1) else {
                return nil
            }
            return args[index + 1]
        }
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("app.husk.nce.mac", isDirectory: true)
        let root = value("--data-directory").map { URL(fileURLWithPath: $0, isDirectory: true) } ?? base
        let resources = value("--resources").map { URL(fileURLWithPath: $0, isDirectory: true) } ?? Bundle.main.resourceURL!
        libraryPID = value("--library-pid").flatMap(Int32.init)
        if args.contains("--run-device") {
            let runtime = LocalRuntime()
            self.runtime = runtime
            session = SessionModel(runtime: runtime)
            workerTarget = RunTarget(id: value("--device-id") ?? root.lastPathComponent,
                                     name: value("--device-name") ?? "Android", directory: root, resources: resources,
                                     log: value("--session-log").map { URL(fileURLWithPath: $0) } ?? root.appendingPathComponent("Logs/session-\(UUID().uuidString).log"))
            library = nil
        } else {
            runtime = nil
            workerTarget = nil
            session = SessionModel(runtime: ProcessRuntime())
            library = DeviceLibrary(root: root, resources: resources, session: session)
        }
    }
    func launchWorker() {
        guard let workerTarget, session.target == nil else {
            return
        }
        signal(SIGTERM, SIG_IGN)
        stopSignal = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .main)
        stopSignal?.setEventHandler {
            // AppKit 等待异步退出时运行嵌套事件循环；先离开主 GCD 队列，让核心完成回调能继续执行。
            NSApp.perform(#selector(NSApplication.terminate(_:)), with: nil, afterDelay: 0)
        }
        stopSignal?.resume()
        session.start(workerTarget, diagnostics: ProcessInfo.processInfo.environment["NCE_DIAGNOSTICS"] == "1",
                      performance: ProcessInfo.processInfo.environment["NCE_MAP_PROFILE"] == "1")
    }
    func returnToLibrary() {
        guard let libraryPID, let app = NSRunningApplication(processIdentifier: libraryPID) else {
            NSApp.terminate(nil)
            return
        }
        app.unhide()
        app.activate(options: [.activateAllWindows])
        NSApp.hide(nil)
    }
}

final class MacApplicationDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate {
    private var terminating = false
    private var window: NSWindow?
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)
        let environment = MacEnvironment.shared
        environment.session.onFinish = { [weak self] in
            if self?.terminating == true {
                NSApp.reply(toApplicationShouldTerminate: true)
            } else if environment.workerTarget != nil {
                NSApp.terminate(nil)
            }
        }
        let worker = environment.workerTarget != nil
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: worker ? 380 : 960, height: worker ? 760 : 640),
                              styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        self.window = window
        window.title = environment.workerTarget?.name ?? "Husk"
        window.contentMinSize = worker ? NSSize(width: 300, height: 550) : NSSize(width: 720, height: 520)
        window.delegate = self
        window.isReleasedWhenClosed = false
        let content: AnyView
        if let runtime = environment.runtime {
            content = AnyView(SessionView(session: environment.session) {
                MacDisplay(runtime: runtime, session: environment.session)
            } returnToLibrary: { environment.returnToLibrary() })
        } else {
            content = AnyView(DeviceHome(library: environment.library!, session: environment.session))
        }
        if !worker {
            window.styleMask.insert(.fullSizeContentView)
            window.titlebarAppearsTransparent = true
            window.toolbarStyle = .unifiedCompact
        }
        window.contentViewController = NSHostingController(rootView: content)
        window.setContentSize(worker ? NSSize(width: 380, height: 760) : NSSize(width: 960, height: 640))
        window.center()
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        let menu = NSMenu()
        let appMenu = NSMenu()
        menu.addItem(withTitle: "Husk", action: nil, keyEquivalent: "").submenu = appMenu
        appMenu.addItem(withTitle: "关于 Husk", action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)), keyEquivalent: "")
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "退出 Husk", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        let edit = NSMenu(title: "编辑")
        menu.addItem(withTitle: "编辑", action: nil, keyEquivalent: "").submenu = edit
        edit.addItem(withTitle: "剪切", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
        edit.addItem(withTitle: "拷贝", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        edit.addItem(withTitle: "粘贴", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        edit.addItem(withTitle: "全选", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        NSApp.mainMenu = menu
        DispatchQueue.main.async { environment.launchWorker() }
    }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        let session = MacEnvironment.shared.session
        if !session.active {
            return .terminateNow
        }
        terminating = true
        session.stop()
        return .terminateLater
    }
    func applicationWillTerminate(_ notification: Notification) {
        if MacEnvironment.shared.workerTarget != nil && MacEnvironment.shared.session.phase == .failed {
            exit(1)
        }
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }
    func windowShouldClose(_ sender: NSWindow) -> Bool {
        NSApp.terminate(nil)
        return false
    }
    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        if !flag {
            sender.windows.first?.makeKeyAndOrderFront(nil)
        }
        return true
    }
}

@main
@MainActor
enum MacApplication {
    static func main() {
        let application = NSApplication.shared
        application.setActivationPolicy(.regular)
        let delegate = MacApplicationDelegate()
        application.delegate = delegate
        withExtendedLifetime(delegate) { application.run() }
    }
}

private struct MacDisplay: NSViewRepresentable {
    let runtime: LocalRuntime
    @ObservedObject var session: SessionModel
    func makeNSView(context: Context) -> NceDisplay {
        runtime.display
    }
    func updateNSView(_ view: NceDisplay, context: Context) {
        view.inputEnabled = session.phase == .running
    }
}

enum GuestInput {
    private static let queue = DispatchQueue(label: "app.husk.navigation", qos: .userInteractive)
    static func send(_ action: Int32, _ x: Int32, _ y: Int32 = 0) {
        queue.async { _ = nce_host_input_send(action, x, y) }
    }
}
