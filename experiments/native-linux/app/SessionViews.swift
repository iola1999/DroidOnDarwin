// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct SessionView<Display: View>: View {
    @ObservedObject var session: SessionModel
    @ViewBuilder let display: () -> Display
    let returnToLibrary: () -> Void
    @State private var logs = false
    @State private var stopping = false
    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Button(action: returnToLibrary) { Label("设备", systemImage: "chevron.left") }
                Spacer()
                Text(session.target?.name ?? "Android").font(.headline).lineLimit(1)
                Spacer()
                Menu {
                    Text(session.status)
                    Button("运行详情") { logs = true }
                    Button("停止设备…", role: .destructive) { stopping = true }.disabled(!session.canStop)
                } label: { Image(systemName: "ellipsis.circle") }.accessibilityLabel("运行操作")
            }.padding(.horizontal, 16)
            #if os(macOS)
                .frame(height: 48)
            #else
                .padding(.vertical, 12)
            #endif
            Divider().frame(height: 1)
            ZStack {
                #if os(macOS)
                    display().frame(maxWidth: .infinity, maxHeight: .infinity)
                        .allowsHitTesting(session.phase == .running)
                #else
                    display().aspectRatio(360.0 / 640.0, contentMode: .fit)
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                        .background(.black).allowsHitTesting(session.phase == .running)
                #endif
                if session.phase != .running {
                    Rectangle().fill(.background)
                    BootProgressView(session: session, details: { logs = true },
                                     stop: { stopping = true }, back: returnToLibrary)
                }
            }
            Divider().frame(height: 1)
            HStack {
                navigation("返回", key: 158)
                Spacer()
                navigation("主页", key: 102)
                Spacer()
                navigation("最近应用", key: 580)
            }.padding(.horizontal, 20)
            #if os(macOS)
                .frame(height: 40)
            #else
                .padding(.vertical, 12)
            #endif
                .disabled(session.phase != .running)
        }.background(.background)
            .sheet(isPresented: $logs) {
                if let url = session.target?.log {
                    SessionLogView(url: url)
                }
            }
            .confirmationDialog("停止设备？", isPresented: $stopping, titleVisibility: .visible) {
                Button("停止设备", role: .destructive) { session.stop() }
                Button("取消", role: .cancel) {}
            } message: { Text("Android 将关闭，应用数据会保留。") }
    }
    private func navigation(_ title: String, key: Int32) -> some View {
        Button(title) { GuestInput.send(Int32(NCE_INPUT_KEY), key) }.buttonStyle(.plain)
    }
}

struct SessionLogView: View {
    let url: URL
    @StateObject private var model = SessionLogModel()
    @Environment(\.dismiss) private var dismiss
    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 6) {
                    if let error = model.error {
                        Text(error).foregroundStyle(.secondary)
                    }
                    if model.lines.isEmpty && model.error == nil {
                        Text("等待运行日志…").foregroundStyle(.secondary)
                    }
                    ForEach(model.lines) { line in Text(line.text).textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading) }
                }.font(.system(.caption, design: .monospaced)).padding(20)
            }
            .navigationTitle("运行详情")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { ShareLink(item: url) { Label("导出日志", systemImage: "square.and.arrow.up") } }
                ToolbarItem(placement: .confirmationAction) { Button("完成") { dismiss() } }
            }
        }.frame(idealWidth: 640, idealHeight: 450).task(id: url) { await model.observe(url) }
    }
}
