// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct TranslationSettings: View {
    let active: Bool
    @AppStorage("translationOnlineCPUs") private var onlineCPUs = max(RunConfiguration.translationCPUs, 1)
    @AppStorage("translationCacheMiB") private var cacheMiB = 128
    @AppStorage("translationCodeWatch") private var codeWatch = true
    @AppStorage("translationCrossPage") private var crossPage = true
    @AppStorage("translationReuseCodeChecks") private var reuseCodeChecks = false
    @AppStorage("translationRouteEpochs") private var routeEpochs = false
    @AppStorage("translationRetainRoutes") private var retainRoutes = false
    @AppStorage("translationMemoryGroups") private var memoryGroups = false
    @AppStorage("translationVectorTable") private var vectorTable = false
    @AppStorage("translationSimdMath") private var simdMath = false
    @AppStorage("translationLinuxFastpath") private var linuxFastpath = false

    var body: some View {
        Section {
            LabeledContent("运行库", value: "Direct · \(RunConfiguration.translationCPUs) CPU")
            if RunConfiguration.translationCPUs == 2 {
                Picker("启用的 CPU", selection: $onlineCPUs) {
                    Text("1 个").tag(1)
                    Text("2 个").tag(2)
                }
            }
            Picker("每类执行器的代码缓存", selection: $cacheMiB) {
                ForEach([16, 64, 128], id: \.self) { Text("\($0) MiB").tag($0) }
            }
            Toggle("代码页写入跟踪", isOn: $codeWatch)
            Toggle("跨页访存", isOn: $crossPage)
            Toggle("复用代码内容检查", isOn: $reuseCodeChecks)
            Toggle("跳转序号检查", isOn: $routeEpochs)
            if RunConfiguration.translationCPUs == 1 {
                Toggle("跨调用复用跳转记录", isOn: $retainRoutes)
                Toggle("访存快速路径", isOn: $linuxFastpath)
            }
            Toggle("连续访存合并", isOn: $memoryGroups)
            Toggle("直接执行向量查表", isOn: $vectorTable)
            Toggle("扩展 SIMD 计算", isOn: $simdMath)
        } header: { Text("翻译执行实验") } footer: {
            Text("各项下次启动生效。每个 CPU 的 Direct 与 Unicorn 分别使用所选缓存容量；总内存还包含 Android 系统和应用。")
            if RunConfiguration.translationCPUs == 2 {
                Text("启用 1 个 CPU 用于同一运行库的性能对照；两个 CPU 的代码缓存仍会准备。")
            }
            if RunConfiguration.translationCPUs == 1 {
                Text("跨调用复用需要同时开启代码页写入跟踪和跳转序号检查。")
                Text("访存快速路径减少 Unicorn 回退中的退出检查，缺页仍立即处理。")
            }
        }.disabled(active)
    }
}
