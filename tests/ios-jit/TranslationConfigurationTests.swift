// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

@main
struct TranslationConfigurationChecks {
    static func main() throws {
        let directory = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let runtime = directory.appendingPathComponent("Runtime.json")
        func manifest(_ value: [String: Any]) throws {
            try JSONSerialization.data(withJSONObject: value).write(to: runtime)
        }
        func rejected(_ body: () throws -> Void) {
            do { try body()
                preconditionFailure("invalid configuration accepted")
            } catch {}
        }
        let defaults = UserDefaults(suiteName: "simd-math-" + UUID().uuidString)!
        precondition(!TranslationConfiguration.saved(defaults).simdMath)
        defaults.set(true, forKey: "translationSimdMath")
        precondition(TranslationConfiguration.saved(defaults).simdMath)
        defaults.removeObject(forKey: "translationSimdMath")
        var config = RunConfiguration(aot: false, translation: .init())
        config.translation!.simdMath = true
        try manifest(["execution": ["engine": "direct", "cpus": 2]])
        let legacyEnvironment = try config.environment(validating: directory)
        precondition(legacyEnvironment["NCE_TRANSLATION_GUEST_LAYOUT"] == "host")
        precondition(legacyEnvironment["NCE_TRANSLATION_DATA_TBI"] == "0")
        precondition(RunConfiguration.fromEnvironment(config.environment) == config)
        for count in [0, 3] {
            config.translation!.onlineCPUs = count
            rejected { _ = try config.environment(validating: directory) }
        }
        config.translation!.onlineCPUs = 2
        _ = try config.environment(validating: directory)
        precondition(RunConfiguration.fromEnvironment(config.environment) == config)
        try manifest(["execution": ["engine": "direct", "cpus": 1]])
        rejected { _ = try config.environment(validating: directory) }
        config.translation!.onlineCPUs = 1
        try manifest(["execution": ["engine": "direct", "cpus": 2]])
        config.translation!.linuxFastpath = true
        rejected { _ = try config.environment(validating: directory) }
        try manifest(["execution": ["engine": "direct", "cpus": 1]])
        _ = try config.environment(validating: directory)
        precondition(RunConfiguration.fromEnvironment(config.environment) == config)
        config.translation!.linuxFastpath = false
        try manifest(["execution": ["engine": "direct", "cpus": 2]])
        config.faultPrefetch = true
        rejected { _ = try config.environment(validating: directory) }
        config.faultPrefetch = false
        config.translation!.cacheMiB = 32
        rejected { _ = try config.environment(validating: directory) }
        config.translation!.cacheMiB = 64
        config.translation!.retainRoutes = true
        config.translation!.routeEpochs = true
        rejected { _ = try config.environment(validating: directory) }
        try manifest(["execution": ["engine": "direct", "cpus": 1]])
        _ = try config.environment(validating: directory)
        config.translation!.codeWatch = false
        rejected { _ = try config.environment(validating: directory) }
        config.translation!.codeWatch = true
        config.aot = true
        rejected { _ = try config.environment(validating: directory) }
        config.aot = false
        config.bootImage = true
        rejected { _ = try config.environment(validating: directory) }
        config.bootImage = false
        for layout in ["host", "low", "sparse"] {
            for tbi in [false, true] {
                try manifest(["execution": ["engine": "direct", "cpus": 1, "guest_layout": layout, "data_tbi": tbi]])
                let environment = try config.environment(validating: directory)
                precondition(environment["NCE_TRANSLATION_GUEST_LAYOUT"] == layout)
                precondition(environment["NCE_TRANSLATION_DATA_TBI"] == (tbi ? "1" : "0"))
                precondition(RunConfiguration.fromEnvironment(environment) == config)
            }
        }
        for layout: Any in [NSNull(), 1, true, "unknown", ["sparse"]] {
            try manifest(["execution": ["engine": "direct", "cpus": 1, "guest_layout": layout]])
            rejected { _ = try config.environment(validating: directory) }
        }
        for tbi: Any in [NSNull(), 0, 1, "1", [true]] {
            try manifest(["execution": ["engine": "direct", "cpus": 1, "data_tbi": tbi]])
            rejected { _ = try config.environment(validating: directory) }
        }
        for fields: [String: Any] in [["cpus": true], ["cpus": "1"], ["cpus": 2, "guest_layout": "low"],
                                      ["cpus": 2, "guest_layout": "sparse"], ["cpus": 2, "data_tbi": true]] {
            try manifest(["execution": fields.merging(["engine": "direct"]) { original, _ in original }])
            rejected { _ = try config.environment(validating: directory) }
        }
        precondition(TranslationConfiguration.environmentKeys.contains("NCE_TRANSLATION_GUEST_LAYOUT"))
        precondition(TranslationConfiguration.environmentKeys.contains("NCE_TRANSLATION_DATA_TBI"))
        try manifest([:])
        rejected { _ = try config.environment(validating: directory) }
        var native = config
        native.translation = nil
        _ = try native.environment(validating: directory)
        try manifest(["execution": ["engine": "direct", "cpus": 1]])
        rejected { _ = try native.environment(validating: directory) }
        let parameters = try JSONSerialization.jsonObject(with: JSONEncoder().encode(config)) as! [String: Any]
        var request: [String: Any] = ["version": 1, "runID": UUID().uuidString, "deviceID": UUID().uuidString,
                                      "configuration": parameters, "actions": [], "timeoutSeconds": 30]
        let decoded = try BenchmarkRequest.decode(JSONSerialization.data(withJSONObject: request))
        precondition(decoded.configuration == config)
        for invalid: Any in [NSNull(), ["cacheMiB": 64], ["unknown": true]] {
            var values = parameters
            values["translation"] = invalid
            request["configuration"] = values
            rejected { _ = try BenchmarkRequest.decode(JSONSerialization.data(withJSONObject: request)) }
        }
        print("translation settings: build compatibility, dependencies, cache capacity, environment and request boundaries passed")
    }
}
