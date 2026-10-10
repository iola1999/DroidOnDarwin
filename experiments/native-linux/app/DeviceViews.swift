// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UniformTypeIdentifiers

struct DeviceHome: View {
    @ObservedObject var library: DeviceLibrary
    @ObservedObject var session: SessionModel
    @State private var sheet: LibrarySheet?
    @State private var importing = false
    @State private var rawImport = false
    @State private var pendingImport: Bool?
    @State private var deleting: NceDevice?
    @State private var stopping = false
    var body: some View {
        NavigationSplitView {
            List(selection: $library.selection) {
                ForEach(library.devices, id: \.identifier) { device in
                    NavigationLink(value: device.identifier) {
                        Label {
                            VStack(alignment: .leading, spacing: 4) {
                                Text(device.name).lineLimit(1)
                                Text(library.state(device)).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                            }.padding(.vertical, 6)
                        } icon: { Image(systemName: "iphone").foregroundStyle(.secondary) }
                    }
                    .contextMenu {
                        Button("重命名") { sheet = .rename(device) }.disabled(session.active && session.target?.id == device.identifier)
                        Button("删除设备…", role: .destructive) { deleting = device }.disabled(session.active && session.target?.id == device.identifier)
                    }
                }
            }
            .navigationTitle("设备")
            .navigationSplitViewColumnWidth(min: 200, ideal: 230, max: 300)
            #if os(iOS)
                .toolbar {
                    ToolbarItemGroup(placement: .primaryAction) {
                        Button { sheet = .settings } label: { Label("设置", systemImage: "gearshape") }
                        Button { sheet = .add } label: { Label("添加设备", systemImage: "plus") }.disabled(library.busy)
                    }
                }
            #endif
                .safeAreaInset(edge: .bottom) {
                    VStack(spacing: 12) {
                        #if os(macOS)
                            Button { sheet = .add } label: { Label("添加设备", systemImage: "plus").frame(maxWidth: .infinity, alignment: .leading) }.disabled(library.busy)
                            Button { sheet = .settings } label: { Label("设置", systemImage: "gearshape").frame(maxWidth: .infinity, alignment: .leading) }
                        #endif
                        if let working = library.working {
                            HStack(spacing: 10) { ProgressView().controlSize(.small)
                                Text(working).font(.caption)
                            }
                            .frame(maxWidth: .infinity).padding().background(.bar)
                        }
                    }.buttonStyle(.plain).padding(16)
                }
        } detail: {
            if let device = library.selected {
                detail(device)
            } else {
                ContentUnavailableView {
                    Label(library.devices.isEmpty ? "添加你的第一台设备" : "选择设备", systemImage: "iphone")
                } description: {
                    Text(library.devices.isEmpty ? "使用内置 Android，或导入已有镜像。" : "查看系统信息，或打开 Android。")
                } actions: {
                    if library.devices.isEmpty {
                        Button("添加设备") { sheet = .add }.buttonStyle(.borderedProminent).disabled(library.busy)
                    }
                }
            }
        }
        .navigationSplitViewStyle(.balanced)
        .tint(.blue)
        .sheet(item: $sheet, onDismiss: {
            if let raw = pendingImport {
                rawImport = raw
                pendingImport = nil
                importing = true
            }
        }) { value in
            switch value {
            case .add:
                AddDeviceView(library: library) { sheet = nil } importImage: {
                    pendingImport = false
                    sheet = nil
                }
            case .rename(let device):
                DeviceNameView(title: "重命名设备", initialName: device.name, actionTitle: "保存") { name in
                    library.rename(device, name: name)
                    sheet = nil
                } cancel: { sheet = nil }
            case .settings:
                SettingsView(library: library, session: session) { sheet = nil } importRaw: {
                    pendingImport = true
                    sheet = nil
                }
            case .log(let url): SessionLogView(url: url)
            }
        }
        .fileImporter(isPresented: $importing, allowedContentTypes: rawImport ? [.data] : [.folder, UTType(importedAs: "app.husk.image", conformingTo: .package)], allowsMultipleSelection: false) {
            library.importImage($0, raw: rawImport)
        }
        .alert("操作未完成", isPresented: Binding(get: { library.message != nil }, set: {
            if !$0 {
                library.message = nil
            }
        })) {
            Button("好") { library.message = nil }
        } message: { Text(library.message ?? "") }
        .alert("删除设备？", isPresented: Binding(get: { deleting != nil }, set: {
            if !$0 {
                deleting = nil
            }
        })) {
            Button("取消", role: .cancel) { deleting = nil }
            Button("删除", role: .destructive) {
                if let deleting {
                    library.delete(deleting)
                }
                deleting = nil
            }
        } message: { Text("\(deleting?.name ?? "此设备")的应用、数据和导入镜像将一并删除。此操作无法撤销。") }
        .confirmationDialog("停止设备？", isPresented: $stopping, titleVisibility: .visible) {
            Button("停止设备", role: .destructive) { session.stop() }
            Button("取消", role: .cancel) {}
        } message: { Text("Android 将关闭，应用数据会保留。") }
    }
    private func infoRow(_ title: String, _ value: String) -> some View {
        HStack(alignment: .firstTextBaseline) { Text(title).foregroundStyle(.secondary)
            Spacer(minLength: 20)
            Text(value).multilineTextAlignment(.trailing)
        }
    }
    private func detail(_ device: NceDevice) -> some View {
        let active = session.active && session.target?.id == device.identifier
        let failed = session.phase == .failed && session.target?.id == device.identifier
        return ScrollView {
            VStack(alignment: .leading, spacing: 28) {
                HStack(spacing: 18) {
                    Image(systemName: "iphone").font(.system(size: 30, weight: .light))
                        .foregroundStyle(.tint).frame(width: 64, height: 72)
                        .background(Color.accentColor.opacity(0.10), in: RoundedRectangle(cornerRadius: 15))
                    VStack(alignment: .leading, spacing: 7) {
                        Text(device.name).font(.largeTitle.weight(.semibold)).textSelection(.enabled)
                        Text(active || failed ? session.status : "已停止")
                            .font(.subheadline).foregroundStyle(failed ? Color.red : Color.secondary)
                    }
                }
                HStack(spacing: 12) {
                    Button { library.start(device) } label: {
                        Label(active ? "打开画面" : "启动设备", systemImage: active ? "rectangle.on.rectangle" : "play.fill")
                    }.buttonStyle(.borderedProminent).controlSize(.large).disabled(!library.canStart(device))
                    if active {
                        Button("停止") { stopping = true }.controlSize(.large).disabled(!session.canStop)
                    }
                }
                if session.active && !active {
                    Text("请先停止 \(session.target?.name ?? "当前设备")，再启动此设备。")
                        .font(.subheadline).foregroundStyle(.secondary)
                }
                if session.restartRequired && !session.active {
                    Label("再次启动前，请关闭并重新打开 Husk。", systemImage: "info.circle")
                        .font(.subheadline).foregroundStyle(.secondary)
                }
                Divider()
                VStack(alignment: .leading, spacing: 18) {
                    Text("系统与数据").font(.headline)
                    infoRow("系统镜像", device.imageName)
                    infoRow("镜像来源", device.builtin ? "内置" : "导入")
                    infoRow("应用数据", "独立保存")
                    #if os(macOS)
                        Button("在 Finder 中查看") { NSWorkspace.shared.open(device.directory) }.buttonStyle(.link)
                    #endif
                }.font(.subheadline)
                Divider()
                VStack(alignment: .leading, spacing: 15) {
                    Text("运行日志").font(.headline)
                    Text("查看启动详情，或导出日志以排查问题。")
                        .font(.subheadline).foregroundStyle(.secondary)
                    Button("查看日志") {
                        if let url = latestLog(in: device.directory) {
                            sheet = .log(url)
                        } else {
                            library.message = "此设备尚未生成运行日志。"
                        }
                    }
                }
                Spacer(minLength: 20)
            }.frame(maxWidth: 660, alignment: .leading).padding(32).frame(maxWidth: .infinity)
        }
        .navigationTitle(device.name)
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Menu {
                    Button("重命名") { sheet = .rename(device) }.disabled(active || library.busy)
                    Button("删除设备…", role: .destructive) { deleting = device }.disabled(active || library.busy)
                } label: { Label("管理设备", systemImage: "ellipsis") }
            }
        }
    }
}

private enum LibrarySheet: Identifiable {
    case add, rename(NceDevice), settings, log(URL)
    var id: String {
        switch self {
        case .add: "add"
        case .rename(let device): "rename-\(device.identifier)"
        case .settings: "settings"
        case .log(let url): url.path
        }
    }
}

private struct AddDeviceView: View {
    @ObservedObject var library: DeviceLibrary
    let close: () -> Void
    let importImage: () -> Void
    @State private var creating = false
    var body: some View {
        NavigationStack {
            VStack(alignment: .leading, spacing: 12) {
                Text("添加设备").font(.title2.weight(.semibold)).padding(.bottom, 12)
                Button { creating = true } label: {
                    option("使用内置 Android", subtitle: "创建一台独立设备", icon: "iphone")
                }
                Divider()
                Button(action: importImage) {
                    option("导入镜像包", subtitle: "选择 .huskimage 镜像包", icon: "folder.badge.plus")
                }
            }.buttonStyle(.plain).padding(28)
                .toolbar { ToolbarItem(placement: .cancellationAction) { Button("取消", action: close) } }
                .navigationDestination(isPresented: $creating) {
                    DeviceNameView(title: "新建 Android 设备", initialName: "我的 Android", actionTitle: "创建设备") { name in
                        library.create(name: name)
                        close()
                    } cancel: { creating = false }
                }
        }.frame(idealWidth: 440, idealHeight: 300)
    }
    private func option(_ title: String, subtitle: String, icon: String) -> some View {
        HStack(spacing: 18) {
            Image(systemName: icon).font(.title2).foregroundStyle(.tint).frame(width: 32)
            VStack(alignment: .leading, spacing: 6) {
                Text(title).font(.headline)
                Text(subtitle).font(.subheadline).foregroundStyle(.secondary)
            }
            Spacer()
            Image(systemName: "chevron.right").font(.caption).foregroundStyle(.tertiary)
        }.padding(.vertical, 16).contentShape(Rectangle())
    }
}

private struct DeviceNameView: View {
    let title: String
    let actionTitle: String
    let save: (String) -> Void
    let cancel: () -> Void
    @State private var name: String
    init(title: String, initialName: String, actionTitle: String, save: @escaping (String) -> Void, cancel: @escaping () -> Void) {
        self.title = title
        self.actionTitle = actionTitle
        self.save = save
        self.cancel = cancel
        _name = State(initialValue: initialName)
    }
    private var trimmed: String {
        name.trimmingCharacters(in: .whitespacesAndNewlines)
    }
    var body: some View {
        VStack(alignment: .leading, spacing: 24) {
            Text(title).font(.title2.weight(.semibold))
            VStack(alignment: .leading, spacing: 8) {
                Text("设备名称").font(.subheadline).foregroundStyle(.secondary)
                TextField("名称", text: $name).textFieldStyle(.roundedBorder).onSubmit {
                    if valid {
                        save(trimmed)
                    }
                }
            }
            HStack {
                Spacer()
                Button("取消", action: cancel).keyboardShortcut(.cancelAction)
                Button(actionTitle) { save(trimmed) }.buttonStyle(.borderedProminent)
                    .keyboardShortcut(.defaultAction).disabled(!valid)
            }
        }.padding(28).frame(idealWidth: 420, idealHeight: 230)
    }
    private var valid: Bool {
        !trimmed.isEmpty && trimmed.utf16.count <= 128
    }
}
