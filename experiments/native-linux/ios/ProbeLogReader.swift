// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

struct ProbeLogLine: Identifiable, Equatable {
    let id: UInt64
    let text: String
}

struct ProbeLogSnapshot {
    let lines: [ProbeLogLine]
    let phase: String?
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
    static let lineLimit = 80
    static let phaseLabels = [
        "android_desktop_running": "Android 正在运行",
        "android_desktop_stopped": "Android 已停止",
        "android_system_server_boot": "正在启动 Android 系统",
        "android_activity_service_registered": "正在启动 Android 桌面",
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
        // Bound each read and stop at the captured size even while the guest writes.
        while offset < size {
            let data = try file.read(upToCount: Int(min(size - offset, 65536))) ?? Data()
            if data.isEmpty {
                break
            }
            offset += UInt64(data.count)
            pending.append(data)
            while let newline = pending.firstIndex(of: 10) {
                let line = String(decoding: pending[..<newline], as: UTF8.self)
                pending.removeSubrange(...newline)
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
        return ProbeLogSnapshot(lines: lines, phase: phase)
    }
}
