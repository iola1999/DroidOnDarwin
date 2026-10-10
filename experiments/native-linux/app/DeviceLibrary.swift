// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Combine

@MainActor
final class DeviceLibrary: ObservableObject {
    @Published private(set) var loaded = false
    @Published private(set) var devices: [NceDevice] = []
    @Published var selection: String?
    @Published private(set) var working: String?
    @Published var message: String?
    private let store: NceDeviceStore
    private let queue = DispatchQueue(label: "app.droidondarwin.devices", qos: .userInitiated)
    let session: SessionModel
    var selected: NceDevice? {
        devices.first { $0.identifier == selection }
    }
    var busy: Bool {
        working != nil || !loaded
    }
    init(root: URL, resources: URL, session: SessionModel) {
        store = NceDeviceStore(root: root, resources: resources)
        self.session = session
        mutate("正在读取设备") { _ in nil }
    }
    private func mutate(_ label: String, action: @escaping (NceDeviceStore) throws -> String?) {
        guard working == nil else {
            return
        }
        working = label
        let store = store
        queue.async {
            do {
                let selected = try action(store)
                let devices = try store.load()
                DispatchQueue.main.async {
                    self.devices = devices
                    self.loaded = true
                    if let selected {
                        self.selection = selected
                    }
                    if !devices.contains(where: { $0.identifier == self.selection }) {
                        #if os(macOS)
                            self.selection = devices.first?.identifier
                        #else
                            self.selection = nil
                        #endif
                    }
                    self.working = nil
                }
            } catch {
                DispatchQueue.main.async {
                    self.message = error.localizedDescription
                    self.working = nil
                }
            }
        }
    }
    func create(name: String) {
        guard !busy else {
            return
        }
        mutate("正在创建设备") { store in
            let device = try store.createBuiltinNamed(name)
            return device.identifier
        }
    }
    func rename(_ device: NceDevice, name: String) {
        guard !busy else {
            return
        }
        guard !session.active || session.target?.id != device.identifier else {
            return
        }
        mutate("正在保存名称") { store in
            try store.renameDevice(device, name: name)
            return device.identifier
        }
    }
    func delete(_ device: NceDevice) {
        guard !busy else {
            return
        }
        guard !session.active || session.target?.id != device.identifier else {
            return
        }
        mutate("正在删除设备") { store in try store.delete(device)
            return nil
        }
    }
    func importImage(_ result: Result<[URL], Error>, raw: Bool) {
        guard !busy else {
            return
        }
        do {
            guard let url = try result.get().first else {
                return
            }
            mutate("正在导入并校验镜像") { store in
                let access = url.startAccessingSecurityScopedResource()
                defer {
                    if access {
                        url.stopAccessingSecurityScopedResource()
                    }
                }
                return try store.importImage(url, raw: raw).identifier
            }
        } catch { message = error.localizedDescription }
    }
    func start(_ device: NceDevice, diagnostics: Bool = false) {
        guard !busy else {
            return
        }
        if session.active {
            if session.target?.id == device.identifier {
                session.openDisplay()
            }
            return
        }
        guard !session.restartRequired else {
            message = "请从多任务界面关闭 DroidOnDarwin，再重新打开后启动设备。"
            return
        }
        session.start(RunTarget(device: device), diagnostics: diagnostics,
                      performance: UserDefaults.standard.bool(forKey: "performanceLogging"))
    }
    func state(_ device: NceDevice) -> String {
        session.target?.id == device.identifier ? session.status : "已停止"
    }
    func canStart(_ device: NceDevice) -> Bool {
        !busy && (!session.active || (session.target?.id == device.identifier && session.phase != .stopping))
    }
}

func latestLog(in directory: URL) -> URL? {
    let logs = (try? FileManager.default.contentsOfDirectory(at: directory.appendingPathComponent("Logs"),
                                                             includingPropertiesForKeys: [.contentModificationDateKey])) ?? []
    return logs.filter { $0.pathExtension == "log" }.max {
        let a = (try? $0.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
        let b = (try? $1.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
        return a < b
    }
}
