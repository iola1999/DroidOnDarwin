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
        try config.validateResources(directory)
        precondition(RunConfiguration.fromEnvironment(config.environment) == config)
        for count in [0, 3] {
            config.translation!.onlineCPUs = count
            rejected { try config.validateResources(directory) }
        }
        config.translation!.onlineCPUs = 2
        try config.validateResources(directory)
        precondition(RunConfiguration.fromEnvironment(config.environment) == config)
        try manifest(["execution": ["engine": "direct", "cpus": 1]])
        rejected { try config.validateResources(directory) }
        config.translation!.onlineCPUs = 1
        try manifest(["execution": ["engine": "direct", "cpus": 2]])
        config.translation!.linuxFastpath = true
        rejected { try config.validateResources(directory) }
        try manifest(["execution": ["engine": "direct", "cpus": 1]])
        try config.validateResources(directory)
        precondition(RunConfiguration.fromEnvironment(config.environment) == config)
        config.translation!.linuxFastpath = false
        try manifest(["execution": ["engine": "direct", "cpus": 2]])
        config.faultPrefetch = true
        rejected { try config.validateResources(directory) }
        config.faultPrefetch = false
        config.translation!.cacheMiB = 32
        rejected { try config.validateResources(directory) }
        config.translation!.cacheMiB = 64
        config.translation!.retainRoutes = true
        config.translation!.routeEpochs = true
        rejected { try config.validateResources(directory) }
        try manifest(["execution": ["engine": "direct", "cpus": 1]])
        try config.validateResources(directory)
        config.translation!.codeWatch = false
        rejected { try config.validateResources(directory) }
        config.translation!.codeWatch = true
        config.aot = true
        rejected { try config.validateResources(directory) }
        config.aot = false
        config.bootImage = true
        rejected { try config.validateResources(directory) }
        config.bootImage = false
        try manifest([:])
        rejected { try config.validateResources(directory) }
        var native = config
        native.translation = nil
        try native.validateResources(directory)
        try manifest(["execution": ["engine": "direct", "cpus": 1]])
        rejected { try native.validateResources(directory) }
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
