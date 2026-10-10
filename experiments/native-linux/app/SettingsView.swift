// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct SettingsView: View {
    @ObservedObject var library: DeviceLibrary
    @ObservedObject var session: SessionModel
    let close: () -> Void
    let importRaw: () -> Void
    @AppStorage("performanceLogging") private var performance = false
    @AppStorage("artNterp") private var nterp = false
    @AppStorage("directFault") private var directFault = true
    @AppStorage("sparseScan") private var sparseScan = true
    @AppStorage("benchmarkAutomation") private var automation = false
    @State private var diagnosticID = ""
    var body: some View {
        NavigationStack {
            Form {
                Section("通用") { LabeledContent("外观", value: "跟随系统") }
                #if os(iOS)
                    Section { NavigationLink("JIT 连接") { JITSettingsView(active: session.active) } }
                #endif
                Section {
                    NavigationLink("诊断与日志") {
                        Form {
                            Section {
                                Toggle("记录性能统计", isOn: $performance).disabled(session.active)
                            } footer: { Text("下次启动生效。计时会增加运行开销，结果保存在日志中。") }
                            Section {
                                Picker("设备", selection: $diagnosticID) {
                                    Text("选择设备").tag("")
                                    ForEach(library.devices, id: \.identifier) { Text($0.name).tag($0.identifier) }
                                }
                                Button("启动诊断") {
                                    if let device = library.devices.first(where: { $0.identifier == diagnosticID }) {
                                        close()
                                        library.start(device, diagnostics: true)
                                    }
                                }.disabled(diagnosticID.isEmpty || session.active || library.busy)
                            } header: { Text("设备诊断") } footer: { Text("运行系统检查，完成后可从设备详情导出日志。") }
                        }.formStyle(.grouped).navigationTitle("诊断与日志")
                    }
                    NavigationLink("高级") {
                        Form {
                            Section {
                                Toggle("汇编解释器", isOn: $nterp).disabled(session.active)
                                Toggle("快速恢复数据页", isOn: $directFault).disabled(session.active)
                                Toggle("稀疏页表扫描", isOn: $sparseScan).disabled(session.active)
                            } header: { Text("性能实验") } footer: {
                                Text("使用 ART nterp 执行尚未编译的 Java 方法。下次启动生效，可关闭后比较启动和应用运行速度。")
                            }
                            Section {
                                Toggle("自动化测试", isOn: $automation).disabled(session.active)
                                    .onChange(of: automation) { library.exportBenchmarkDevices() }
                            } footer: {
                                Text("允许实验链接启动已有设备、执行操作并保存测试日志。每次实验的参数单独生效。")
                            }
                            Section {
                                Button("导入 system.img…", action: importRaw).disabled(library.busy)
                            } footer: { Text("接受完整 ext4 镜像。镜像需要匹配当前运行组件，其他 Android ROM 可能无法启动。") }
                        }.formStyle(.grouped).navigationTitle("高级")
                    }
                }
                Section("关于") {
                    NavigationLink("开源致谢") { AcknowledgementsView() }
                    LabeledContent("DroidOnDarwin", value: Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "")
                    Text(Bundle.main.object(forInfoDictionaryKey: "DroidOnDarwinBuildID") as? String ?? "")
                        .font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
                }
            }.formStyle(.grouped).navigationTitle("设置")
                .toolbar { ToolbarItem(placement: .confirmationAction) { Button("完成", action: close) } }
        }.frame(idealWidth: 480, idealHeight: 460)
            .onAppear { diagnosticID = library.selection ?? "" }
    }
}
