// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct BootProgressView: View {
    @ObservedObject var session: SessionModel
    let details: () -> Void
    let stop: () -> Void
    let back: () -> Void
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.scenePhase) private var scenePhase
    private let steps = ["准备设备", "启动内核", "启动系统服务", "准备桌面"]
    private var starting: Bool {
        session.active && session.phase != .stopping
    }
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 24) {
                VStack(alignment: .leading, spacing: 8) {
                    Text(starting ? "正在启动 Android" : session.status).font(.title2.weight(.semibold))
                    if session.active {
                        Text("已等待 \(session.elapsed) 秒").font(.subheadline).foregroundStyle(.secondary).monospacedDigit()
                    }
                }
                if starting {
                    VStack(alignment: .leading, spacing: 20) {
                        ForEach(steps.indices, id: \.self) { index in
                            HStack(spacing: 14) {
                                Image(systemName: index < session.bootStep ? "checkmark.circle.fill" : index == session.bootStep ? "circle.inset.filled" : "circle")
                                    .foregroundStyle(index <= session.bootStep ? Color.accentColor : Color.secondary.opacity(0.4))
                                    .symbolEffect(.pulse, options: .repeating, isActive: index == session.bootStep && !reduceMotion && scenePhase == .active)
                                    .accessibilityHidden(true)
                                Text(steps[index]).foregroundStyle(index <= session.bootStep ? .primary : .secondary)
                                Spacer(minLength: 0)
                                if index == session.bootStep {
                                    Text("\(session.stepElapsed) 秒").font(.caption).foregroundStyle(.secondary).monospacedDigit()
                                }
                            }.accessibilityElement(children: .combine)
                                .accessibilityLabel("\(steps[index])，\(index < session.bootStep ? "已完成" : index == session.bootStep ? "进行中" : "等待中")")
                        }
                    }
                    Divider()
                    VStack(alignment: .leading, spacing: 8) {
                        Text("最近活动").font(.caption).foregroundStyle(.secondary)
                        Text(session.activity).font(.subheadline).fixedSize(horizontal: false, vertical: true)
                        if session.activityAge >= 30 {
                            Text("最近 \(session.activityAge) 秒没有新的启动活动。可以继续等待，或停止后重试。")
                                .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                        }
                    }
                } else if session.phase == .stopping {
                    ProgressView().controlSize(.small)
                }
                HStack {
                    Button("查看详情", action: details)
                    Spacer()
                    if session.canStop {
                        Button("停止…", role: .destructive, action: stop)
                    }
                    if !session.active {
                        Button("返回设备", action: back)
                    }
                }
            }.padding(28).frame(maxWidth: 440).frame(maxWidth: .infinity)
        }.defaultScrollAnchor(.center)
    }
}
