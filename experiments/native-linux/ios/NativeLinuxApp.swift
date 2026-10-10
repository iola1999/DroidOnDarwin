// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

// 现有 JIT 助手共用此日志入口。
enum AppLog {
    static func log(_ category: String, _ message: String) {
        nce_log("[\(category)] \(message)")
    }
}

@main
struct DroidOnDarwinApp: App {
    @StateObject private var library: DeviceLibrary
    private let runtime: LocalRuntime
    init() {
        let runtime = LocalRuntime()
        self.runtime = runtime
        let session = SessionModel(runtime: runtime)
        let root = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("DroidOnDarwin", isDirectory: true)
        _library = StateObject(wrappedValue: DeviceLibrary(root: root, resources: Bundle.main.resourceURL!, session: session))
    }
    var body: some Scene {
        WindowGroup { IOSRoot(library: library, session: library.session, runtime: runtime) }
    }
}

private struct IOSRoot: View {
    @ObservedObject var library: DeviceLibrary
    @ObservedObject var session: SessionModel
    let runtime: LocalRuntime
    var body: some View {
        ZStack {
            DeviceHome(library: library, session: session)
            if session.target != nil {
                SessionView(session: session) {
                    GuestMetalDisplay(metalLayer: runtime.metalLayer)
                } returnToLibrary: { session.showDisplay = false }
                    .opacity(session.showDisplay ? 1 : 0)
                    .allowsHitTesting(session.showDisplay)
                    .accessibilityHidden(!session.showDisplay)
            }
        }
        .onOpenURL { library.openBenchmarkURL($0) }
    }
}
