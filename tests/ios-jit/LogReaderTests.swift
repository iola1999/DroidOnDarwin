// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

@main
struct LogReaderTests {
    static func main() async throws {
        let directory = FileManager.default.temporaryDirectory
            .appendingPathComponent("nce-log-reader-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let url = directory.appendingPathComponent("probe.log")
        let reader = ProbeLogReader()
        let initial = "{\"phase\":\"linux_init\"}\n" + (0 ..< 16000).map { "line \($0)\n" }.joined()
        try Data(initial.utf8).write(to: url)
        let snapshot = try await reader.read(from: url)!
        precondition(snapshot.lines.count == 80)
        precondition(snapshot.lines.first?.text == "line 15920")
        precondition(snapshot.lines.last?.text == "line 15999")
        precondition(snapshot.phase == "linux_init")
        let unchanged = try await reader.read(from: url)
        precondition(unchanged == nil)
        print("PASS large log: bounded preview, retained phase, unchanged file skipped")

        let writer = try FileHandle(forWritingTo: url)
        defer { try? writer.close() }
        try writer.seekToEnd()
        let chinese = Data("测试完成\n".utf8)
        try writer.write(contentsOf: chinese.prefix(2))
        let partial = try await reader.read(from: url)!
        precondition(partial.lines == snapshot.lines)
        try writer.write(contentsOf: chinese.dropFirst(2))
        let appended = try await reader.read(from: url)!
        precondition(appended.lines.last?.text == "测试完成")
        precondition(appended.lines.dropLast().map(\.id) == snapshot.lines.dropFirst().map(\.id))
        precondition(appended.phase == "linux_init")
        print("PASS split UTF-8 writes: complete line and stable existing row identities")

        try writer.write(contentsOf: Data(repeating: 120, count: 20000))
        _ = try await reader.read(from: url)
        try writer.write(contentsOf: Data("\n{\"phase\":\"android_zygote_launch\"}\n".utf8))
        let long = try await reader.read(from: url)!
        precondition(long.lines.allSatisfy { $0.text.count <= 513 })
        precondition(long.phase == "android_zygote_launch")
        print("PASS oversized partial line: bounded preview and following phase parsed")

        try writer.truncate(atOffset: 0)
        let empty = try await reader.read(from: url)!
        precondition(empty.lines.isEmpty && empty.phase == nil)
        try writer.seek(toOffset: 0)
        try writer.write(contentsOf: Data("first\n".utf8))
        let first = try await reader.read(from: url)!
        precondition(first.lines.last?.text == "first")
        try writer.seek(toOffset: 0)
        try writer.write(contentsOf: Data("other\n".utf8))
        try FileManager.default.setAttributes([.modificationDate: Date().addingTimeInterval(2)],
                                              ofItemAtPath: url.path)
        let rewritten = try await reader.read(from: url)!
        precondition(rewritten.lines.map(\.text) == ["other"])
        print("PASS truncation and same-size replacement reset preview")

        try Data("replacement\n".utf8).write(to: url, options: .atomic)
        let replaced = try await reader.read(from: url)!
        precondition(replaced.lines.map(\.text) == ["replacement"])
        await reader.reset()
        let reset = try await reader.read(from: url)!
        precondition(reset.lines.map(\.text) == ["replacement"])
        let retained = try Data(contentsOf: url)
        precondition(retained == Data("replacement\n".utf8))
        print("PASS file replacement and new attempt; source bytes preserved")
        let backlog = Data(repeating: 120, count: 600000) + Data("\n{\"phase\":\"android_desktop_running\"}\n".utf8)
        try backlog.write(to: url, options: .atomic)
        let chunk1 = try await reader.read(from: url)!
        let chunk2 = try await reader.read(from: url)!
        let chunk3 = try await reader.read(from: url)!
        precondition(chunk1.phase == nil && chunk2.phase == nil && chunk3.phase == "android_desktop_running")
        let tail = try await reader.tail(from: url)!
        precondition(tail.phase == "android_desktop_running" && tail.lines.count <= 80)
        print("PASS per-poll byte budget and on-demand tail of a large log")
        let events = "{\"phase\":\"android_system_server_boot\"}\n"
            + "10-10 00:00:01 D SystemServerTiming: StartPackageManagerService\n"
        try Data(events.utf8).write(to: url, options: .atomic)
        let starting = try await reader.read(from: url)!
        precondition(starting.step == 2 && starting.activity == "正在启动应用管理服务")
        let noisy = events + String(repeating: "E CameraManagerGlobal: Camera service is unavailable\n", count: 100)
        try Data(noisy.utf8).write(to: url)
        let noise = try await reader.read(from: url)!
        precondition(noise.activity == starting.activity && noise.activitySequence == starting.activitySequence)
        try Data((noisy + "{\"test\":\"android_system_boot_completed\",\"passed\":true}\n").utf8).write(to: url)
        let desktop = try await reader.read(from: url)!
        precondition(desktop.step == 3 && desktop.activity == "系统初始化完成，正在打开桌面")
        try Data((noisy + "{\"test\":\"android_system_boot_completed\",\"passed\":false}\n").utf8).write(to: url, options: .atomic)
        let failedCheck = try await reader.read(from: url)!
        precondition(failedCheck.step == 2)
        print("PASS boot milestones, selected service activity, duplicate warning filtering and failed-check boundary")

    }
}
