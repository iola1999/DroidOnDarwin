// SPDX-License-Identifier: GPL-2.0-or-later
import SwiftUI

struct AcknowledgementsView: View {
    private let text: AttributedString = {
        guard let url = Bundle.main.url(forResource: "Acknowledgements", withExtension: "md"),
              let source = try? String(contentsOf: url, encoding: .utf8),
              let text = try? AttributedString(markdown: source, options: .init(interpretedSyntax: .inlineOnlyPreservingWhitespace)) else {
            return AttributedString("致谢文件无法读取，请重新安装应用。")
        }
        return text
    }()

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                Text(text).textSelection(.enabled)
                if let url = Bundle.main.url(forResource: "ThirdPartyNotices", withExtension: "txt") {
                    ShareLink("导出许可文本", item: url)
                }
                if let url = Bundle.main.url(forResource: "NOTICE", withExtension: "xml.gz") {
                    ShareLink("导出 Android 组件声明", item: url)
                }
            }.frame(maxWidth: .infinity, alignment: .leading).padding(24)
        }.navigationTitle("开源致谢")
    }
}
