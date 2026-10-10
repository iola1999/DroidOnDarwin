// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI
import UniformTypeIdentifiers

struct JITSettingsView: View {
    let active: Bool
    @AppStorage("jitMethod") private var method = "builtin"
    @State private var importing = false
    @State private var paired = JITPairingFileStore.exists
    @State private var message: String?
    var body: some View {
        Form {
            Section {
                Picker("连接方式", selection: $method) {
                    Text("内置 JIT 助手").tag("builtin")
                    Text("StikDebug").tag("external")
                }.disabled(active)
            }
            if method == "builtin" {
                Section {
                    LabeledContent("配对文件", value: paired ? "已导入" : "未导入")
                    Button(paired ? "更换配对文件…" : "导入配对文件…") { importing = true }.disabled(active)
                } footer: { Text("使用这台 iPhone 的配对文件，并保持 LocalDevVPN 已连接。") }
            } else {
                Section { Text("启动时将打开 StikDebug。启用 JIT 后返回 DroidOnDarwin。").foregroundStyle(.secondary) }
            }
        }.navigationTitle("JIT 连接")
            .fileImporter(isPresented: $importing, allowedContentTypes: [.data], allowsMultipleSelection: false) { result in
                do {
                    if let url = try result.get().first {
                        try JITPairingFileStore.importFile(from: url)
                        paired = true
                    }
                } catch { message = error.localizedDescription }
            }
            .alert("导入未完成", isPresented: Binding(get: { message != nil }, set: {
                if !$0 {
                    message = nil
                }
            })) {
                Button("好") { message = nil }
            } message: { Text(message ?? "") }
    }
}
