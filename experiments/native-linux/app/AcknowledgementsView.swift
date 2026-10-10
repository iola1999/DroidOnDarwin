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
            Text(text)
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(24)
        }.navigationTitle("开源致谢")
    }
}
