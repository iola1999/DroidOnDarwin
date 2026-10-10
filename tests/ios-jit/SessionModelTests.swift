// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

@MainActor
final class RuntimeFixture: SessionRuntime {
    var restartRequired = false
    var launches = 0
    var stops = 0
    var nterp = false
    var event: ((RuntimeEvent) -> Void)?
    func start(_ target: RunTarget, configuration: RunConfiguration, event: @escaping (RuntimeEvent) -> Void) {
        launches += 1
        self.nterp = configuration.nterp
        self.event = event
    }
    func stop() {
        stops += 1
    }
    func openDisplay() {}
}

@main
struct SessionChecks {
    @MainActor static func main() async throws {
        let runtime = RuntimeFixture()
        let session = SessionModel(runtime: runtime)
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        let target = RunTarget(id: "one", name: "Android", directory: root, resources: root, log: root.appendingPathComponent("session.log"))
        session.start(target, configuration: .init(nterp: true))
        session.start(target)
        precondition(runtime.launches == 1 && runtime.nterp && session.phase == .preparing)
        runtime.event?(.connecting("连接 JIT"))
        session.stop()
        session.stop()
        runtime.event?(.booting)
        precondition(session.phase == .stopping && runtime.stops == 1)
        runtime.event?(.finished(nil))
        precondition(!session.active && !session.showDisplay)
        session.start(target)
        precondition(!runtime.nterp)
        runtime.event?(.booting)
        try Data("{\"phase\":\"android_desktop_running\"}\n".utf8).write(to: target.log)
        try await Task.sleep(for: .milliseconds(1300))
        precondition(session.phase == .running)
        runtime.event?(.finished("failed"))
        precondition(session.phase == .failed && !session.active && session.status == "failed")
        runtime.restartRequired = true
        session.start(target)
        precondition(runtime.launches == 2 && session.phase == .failed)
        print("session checks passed: duplicate launch, stop during preparation/JIT, late boot event, ready log, failure and restart boundary")
    }
}
