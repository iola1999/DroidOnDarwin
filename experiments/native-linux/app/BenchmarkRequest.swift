// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

struct BenchmarkRequest: Codable, Sendable {
    struct Action: Codable, Sendable {
        let kind: String
        let milliseconds: Int?
        let x: Int?
        let y: Int?
        let endX: Int?
        let endY: Int?
        let key: String?
    }
    let version: Int
    let runID: UUID
    let deviceID: String
    let configuration: RunConfiguration
    let actions: [Action]
    let timeoutSeconds: Int
    let memorySampling: Bool?

    var capturesSettingsOpen: Bool {
        guard let start = actions.firstIndex(where: { $0.kind == "profile" && ($0.key == "frames-start" || $0.key == "trace-start") }) else {
            return false
        }
        return actions.dropFirst(start + 1).contains { $0.kind == "profile" && $0.key == "settings" }
    }

    static func decode(_ data: Data) throws -> Self {
        guard data.count <= 32768,
              let object = try JSONSerialization.jsonObject(with: data) as? [String: Any],
              Set(object.keys).subtracting(["memorySampling"]) == ["version", "runID", "deviceID", "configuration", "actions", "timeoutSeconds"],
              let config = object["configuration"] as? [String: Any], Set(config.keys).subtracting(["translation"]) == RunConfiguration.keys,
              let actions = object["actions"] as? [[String: Any]] else {
            throw BenchmarkError.invalid("实验请求字段不完整或含未知字段")
        }
        if let translation = config["translation"] {
            guard let values = translation as? [String: Any], Set(values.keys) == TranslationConfiguration.keys else {
                throw BenchmarkError.invalid("翻译缓存配置字段不完整或含未知字段")
            }
        }
        if object["memorySampling"] is NSNull {
            throw BenchmarkError.invalid("memorySampling 需要布尔值")
        }
        for action in actions {
            let fields: Set<String>
            switch action["kind"] as? String {
            case "wait": fields = ["kind", "milliseconds"]
            case "tap": fields = ["kind", "x", "y"]
            case "swipe": fields = ["kind", "x", "y", "endX", "endY", "milliseconds"]
            case "profile": fields = ["kind", "key"]
            case "key": fields = ["kind", "key"]
            default: throw BenchmarkError.invalid("未知的实验操作")
            }
            guard Set(action.keys) == fields else {
                throw BenchmarkError.invalid("实验操作字段不匹配")
            }
        }
        let request = try JSONDecoder().decode(Self.self, from: data)
        guard request.configuration.schedulerSliceIsValid else {
            throw BenchmarkError.invalid("调度时间片需要为 0 或 100–100000 微秒")
        }
        guard request.version == 1, UUID(uuidString: request.deviceID) != nil,
              (30 ... 900).contains(request.timeoutSeconds), request.actions.count <= 128,
              !request.configuration.diagnostics else {
            throw BenchmarkError.invalid("实验版本、设备编号、时限或诊断配置无效")
        }
        var profileStarted = false
        var profileEnded = false
        var settingsOpened = false
        for action in request.actions {
            if action.kind == "profile" {
                guard let key = action.key, ["settings", "frames-start", "trace-start", "capture-end"].contains(key), !profileEnded else {
                    throw BenchmarkError.invalid("采集命令无效或窗口已经结束")
                }
                if key == "settings" {
                    guard !profileStarted || !settingsOpened else {
                        throw BenchmarkError.invalid("首次打开采集不能预热或重复打开 Settings")
                    }
                    settingsOpened = true
                } else if key == "capture-end" {
                    guard profileStarted else {
                        throw BenchmarkError.invalid("采集尚未开始")
                    }
                    profileEnded = true
                } else {
                    guard !profileStarted else {
                        throw BenchmarkError.invalid("每轮只允许一个采集窗口")
                    }
                    profileStarted = true
                }
            }
            if action.kind == "tap" || action.kind == "swipe" {
                guard action.x != nil, action.y != nil else {
                    throw BenchmarkError.invalid("操作缺少坐标")
                }
            }
            if action.kind == "swipe" {
                guard action.endX != nil, action.endY != nil else {
                    throw BenchmarkError.invalid("滑动缺少终点")
                }
            }
            if action.kind == "wait" || action.kind == "swipe" {
                guard action.milliseconds != nil else {
                    throw BenchmarkError.invalid("操作缺少时长")
                }
            }
            if action.kind == "key" {
                guard action.key != nil else {
                    throw BenchmarkError.invalid("操作缺少导航键")
                }
            }
            for x in [action.x, action.endX].compactMap({ $0 }) {
                guard (0 ..< 360).contains(x) else {
                    throw BenchmarkError.invalid("点击横坐标超出范围")
                }
            }
            for y in [action.y, action.endY].compactMap({ $0 }) {
                guard (0 ..< 640).contains(y) else {
                    throw BenchmarkError.invalid("点击纵坐标超出范围")
                }
            }
            if let duration = action.milliseconds {
                let range = action.kind == "swipe" ? 16 ... 5000 : 0 ... 60000
                guard range.contains(duration) else {
                    throw BenchmarkError.invalid("操作时长超出范围")
                }
            }
            if action.kind == "key", let key = action.key, !["home", "back", "recent"].contains(key) {
                throw BenchmarkError.invalid("未知的导航键")
            }
        }
        guard !profileStarted || profileEnded else {
            throw BenchmarkError.invalid("采集缺少结束动作")
        }
        return request
    }

    static func fromURL(_ url: URL) throws -> Self {
        guard url.scheme == "droidondarwin", url.host == "benchmark", url.path.isEmpty,
              url.user == nil, url.password == nil, url.port == nil, url.fragment == nil,
              let items = URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems,
              items.count == 1, items[0].name == "request", var encoded = items[0].value,
              encoded.count <= 45000 else {
            throw BenchmarkError.invalid("实验链接无效")
        }
        encoded = encoded.replacingOccurrences(of: "-", with: "+").replacingOccurrences(of: "_", with: "/")
        encoded += String(repeating: "=", count: (4 - encoded.count % 4) % 4)
        guard let data = Data(base64Encoded: encoded) else {
            throw BenchmarkError.invalid("实验链接编码无效")
        }
        return try decode(data)
    }
}

enum BenchmarkError: LocalizedError {
    case invalid(String)
    var errorDescription: String? {
        switch self { case .invalid(let message): return message }
    }
}
