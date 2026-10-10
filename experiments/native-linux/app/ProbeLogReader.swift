// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

struct ProbeLogLine: Identifiable, Equatable {
    let id: UInt64
    let text: String
}

struct ProbeLogSnapshot {
    let lines: [ProbeLogLine]
    let phase: String?
    let step: Int?
    let activity: String?
    let activitySequence: UInt64
}

// File reads and parsing stay off the main actor. The exported file is unchanged.
actor ProbeLogReader {
    private var offset: UInt64 = 0
    private var fileID: UInt64?
    private var modified: Date?
    private var pending = Data()
    private var lines: [ProbeLogLine] = []
    private var nextLine: UInt64 = 0
    private var phase: String?
    private var step: Int?
    private var activity: String?
    private var activitySequence: UInt64 = 0
    static let lineLimit = 80
    static let phaseLabels = [
        "android_desktop_running": "Android 正在运行",
        "android_desktop_stopped": "Android 已停止",
        "android_system_server_boot": "正在启动 Android 系统",
        "android_activity_service_registered": "正在启动系统服务",
        "android_desktop_first_frame": "正在显示桌面",
        "native_graphics_start": "正在启动图形服务",
        "linux_shutdown": "正在停止 Linux",
        "android_zygote_launch": "正在启动 Android 进程",
        "android_framework_launch": "正在加载 Android 系统框架",
        "android_platform_services": "正在启动 Android 系统服务",
        "android_art_inlining": "正在检查 Java 方法内联",
        "android_art_load": "正在运行 Java 与编译执行测试",
        "android_art_reference_heap": "正在检查 Android 对象内存",
        "android_bionic_dynamic": "正在检查 Android 运行库",
        "linux_binder_program": "正在检查 Binder 进程通信",
        "linux_storage_program": "正在检查文件存储",
        "linux_dynamic_program": "正在检查动态库和线程",
        "linux_dash_shell": "正在运行 shell 脚本",
        "linux_mm_benchmark": "正在检查信号与内存",
        "linux_userspace_tests": "正在测试 C 程序和线程",
        "linux_process_tests": "正在测试 Linux 进程",
        "linux_init": "正在启动 Linux 内核",
        "check_fixed_code_and_data_remap": "正在检查代码执行和数据映射",
        "wait_debugger_detach": "正在等待 JIT 助手断开",
        "prepare_jit_pool": "正在准备 JIT 代码区域",
    ]

    func reset() {
        offset = 0
        fileID = nil
        modified = nil
        pending.removeAll()
        lines.removeAll()
        phase = nil
        step = nil
        activity = nil
        activitySequence = 0
    }

    func tail(from url: URL) throws -> ProbeLogSnapshot? {
        reset()
        let attributes = try FileManager.default.attributesOfItem(atPath: url.path)
        let size = (attributes[.size] as? NSNumber)?.uint64Value ?? 0
        fileID = (attributes[.systemFileNumber] as? NSNumber)?.uint64Value
        modified = attributes[.modificationDate] as? Date
        offset = size > 65536 ? size - 65536 : 0
        return try read(from: url)
    }

    func read(from url: URL) throws -> ProbeLogSnapshot? {
        let attributes = try FileManager.default.attributesOfItem(atPath: url.path)
        let size = (attributes[.size] as? NSNumber)?.uint64Value ?? 0
        let identity = (attributes[.systemFileNumber] as? NSNumber)?.uint64Value
        let date = attributes[.modificationDate] as? Date
        if identity != fileID || size < offset || (size == offset && date != modified) {
            reset()
        }
        guard size != offset || fileID == nil else {
            return nil
        }
        fileID = identity
        modified = date
        let file = try FileHandle(forReadingFrom: url)
        defer { try? file.close() }
        try file.seek(toOffset: offset)
        // Bound total work per poll, including while the guest keeps writing.
        let end = min(size, offset + 262144)
        while offset < end {
            let data = try file.read(upToCount: Int(min(end - offset, 65536))) ?? Data()
            if data.isEmpty {
                break
            }
            offset += UInt64(data.count)
            pending.append(data)
            while let newline = pending.firstIndex(of: 10) {
                let line = String(decoding: pending[..<newline], as: UTF8.self)
                pending.removeSubrange(...newline)
                recordActivity(line)
                if line.hasPrefix("{\"phase\":") {
                    let value = try? JSONSerialization.jsonObject(with: Data(line.utf8)) as? [String: Any]
                    if let key = value?["phase"] as? String, Self.phaseLabels[key] != nil {
                        phase = key
                    }
                }
                let preview = line.count > 512 ? String(line.prefix(512)) + "…" : line
                lines.append(ProbeLogLine(id: nextLine, text: preview))
                nextLine += 1
                if lines.count > Self.lineLimit {
                    lines.removeFirst(lines.count - Self.lineLimit)
                }
            }
            // A malformed line must not grow the preview buffer indefinitely.
            if pending.count > 4096 {
                pending = Data(pending.suffix(4096))
            }
        }
        return ProbeLogSnapshot(lines: lines, phase: phase, step: step, activity: activity, activitySequence: activitySequence)
    }
    private func recordActivity(_ line: String) {
        var update: String?
        var nextStep: Int?
        if line.hasPrefix("{") {
            let value = (try? JSONSerialization.jsonObject(with: Data(line.utf8))) as? [String: Any]
            let key = value?["phase"] as? String ?? ((value?["passed"] as? Bool == true) ? value?["test"] as? String : nil)
            switch key {
            case "linux_init": nextStep = 1
                update = "正在初始化 Linux 内核"
            case "native_graphics_start": nextStep = 2
                update = "正在连接图形服务"
            case "android_platform_services": nextStep = 2
                update = "正在启动 Android 基础服务"
            case "android_display_service_registered": update = "图形服务已注册"
            case "android_framework_launch": nextStep = 2
                update = "正在加载 Android 系统框架"
            case "android_zygote_launch": nextStep = 2
                update = "正在启动 Android 进程"
            case "android_system_server_boot": nextStep = 2
                update = "正在初始化系统服务"
            case "android_activity_service_registered": nextStep = 2
                update = "应用运行服务已注册"
            case "android_system_server_services": nextStep = 2
                update = "正在准备用户环境"
            case "android_system_boot_completed": nextStep = 3
                update = "系统初始化完成，正在打开桌面"
            case "android_desktop_first_frame": nextStep = 3
                update = "桌面已绘制，正在完成启动"
            case "android_desktop_running": nextStep = 4
                update = "桌面已就绪"
            default: break
            }
        } else {
            let events = ["StartPackageManagerService": "应用管理服务", "StartActivityManager": "应用运行服务",
                          "StartWindowManagerService": "窗口服务", "StartInputManagerService": "输入服务",
                          "StartSystemUI": "系统界面进程"]
            if let range = line.range(of: "SystemServerTiming: ") {
                let event = String(line[range.upperBound...])
                for (key, name) in events where event == key || event.hasPrefix(key + " took to complete:") {
                    update = event == key ? "正在启动" + name : name + "已完成初始化"
                }
            } else if let range = line.range(of: "SystemUIBootTiming: ") {
                let event = String(line[range.upperBound...])
                if event == "StartServices" {
                    update = "正在加载系统界面"
                }
                if event.hasPrefix("StartServices took to complete:") {
                    update = "系统界面已完成初始化"
                }
            }
        }
        if let nextStep {
            step = max(step ?? 0, nextStep)
        }
        if let update, update != activity {
            activity = update
            activitySequence += 1
        }
    }

}
