// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

private let inputLock = NSLock()
private var inputEvents: [[Int32]] = []
private var inputFailure = false
@_cdecl("nce_host_input_send")
func benchmarkInput(_ action: Int32, _ x: Int32, _ y: Int32) -> Int32 {
    inputLock.lock()
    defer { inputLock.unlock() }
    inputEvents.append([action, x, y])
    return inputFailure ? -5 : 0
}

@MainActor
final class BenchmarkRuntime: SessionRuntime {
    var restartRequired = false
    var configuration: RunConfiguration?
    private var event: ((RuntimeEvent) -> Void)?
    func start(_ target: RunTarget, configuration: RunConfiguration, event: @escaping (RuntimeEvent) -> Void) {
        self.configuration = configuration
        self.event = event
        event(.booting)
        try! Data("{\"phase\":\"android_desktop_running\"}\n".utf8).write(to: target.log)
    }
    func stop() {
        event?(.finished(nil))
    }
    func openDisplay() {}
}

@main
struct BenchmarkChecks {
    @MainActor static func main() async throws {
        var memory = BenchmarkMemory()
        try memory.add(.init(bytes: ["footprint": 100], processPeaks: ["footprint": 900], queryMicroseconds: 2), elapsedMS: 0, phase: "preparing", bootStep: 0)
        try memory.add(.init(bytes: ["footprint": 300], processPeaks: ["footprint": 900], queryMicroseconds: 4), elapsedMS: 1000, phase: "preparing", bootStep: 0)
        try memory.add(.init(error: 5, queryMicroseconds: 1), elapsedMS: 2000, phase: "preparing", bootStep: 0)
        try memory.add(.init(bytes: ["footprint": 50], processPeaks: ["footprint": 900]), elapsedMS: 2100, phase: "booting", bootStep: 2)
        let stages = memory.json["phases"] as! [String: [String: Any]]
        let preparing = stages["preparing"]!
        let footprint = (preparing["metrics"] as! [String: [String: Any]])["footprint"]!
        precondition(preparing["attempts"] as? Int == 3 && preparing["failures"] as? Int == 1)
        precondition(preparing["latest_attempt_ms"] as? Double == 2000 && preparing["latest_success_ms"] as? Double == 1000)
        precondition(footprint["samples"] as? Int == 2 && footprint["mean_bytes"] as? Double == 200)
        precondition(footprint["sampled_peak_bytes"] as? UInt64 == 300 && footprint["latest_bytes"] as? UInt64 == 300)
        precondition((memory.json["process_lifetime_peak_bytes"] as? [String: UInt64])?["footprint"] == 900)
        let live = BenchmarkMemoryReading.current()
        precondition(live.error == nil && live.bytes["footprint"]! > 0 && live.bytes["resident"]! > 0)
        let configuration = RunConfiguration(nterp: true, expandedJIT: true, fastBaseline: true, aot: false, bootImage: true, directFault: false, faultPrefetch: true, sparseScan: false)
        precondition(RunConfiguration.fromEnvironment(configuration.environment) == configuration)
        let config = try JSONSerialization.jsonObject(with: JSONEncoder().encode(configuration))
        let id = UUID().uuidString
        var object: [String: Any] = ["version": 1, "runID": UUID().uuidString, "deviceID": id,
                                     "configuration": config, "timeoutSeconds": 30,
                                     "actions": [["kind": "key", "key": "home"],
                                                 ["kind": "tap", "x": 10, "y": 20],
                                                 ["kind": "swipe", "x": 50, "y": 400, "endX": 50, "endY": 50, "milliseconds": 32]]]
        func encoded(_ object: [String: Any]) throws -> Data {
            try JSONSerialization.data(withJSONObject: object)
        }
        let data = try encoded(object)
        let request = try BenchmarkRequest.decode(data)
        var components = URLComponents(string: "droidondarwin://benchmark")!
        components.queryItems = [URLQueryItem(name: "request", value: data.base64EncodedString())]
        let decodedURL = try BenchmarkRequest.fromURL(components.url!)
        precondition(decodedURL.runID == request.runID)
        var invalid = object
        invalid["unknown"] = true
        do { _ = try BenchmarkRequest.decode(encoded(invalid))
            preconditionFailure("unknown field accepted")
        } catch {}
        invalid = object
        invalid["actions"] = [["kind": "tap", "x": 360, "y": 20]]
        do { _ = try BenchmarkRequest.decode(encoded(invalid))
            preconditionFailure("invalid coordinate accepted")
        } catch {}
        invalid = object
        invalid["actions"] = [["kind": "tap", "x": 10, "y": 20, "key": "home"]]
        do { _ = try BenchmarkRequest.decode(encoded(invalid))
            preconditionFailure("extra field accepted")
        } catch {}
        invalid = object
        invalid["deviceID"] = "../../escape"
        do { _ = try BenchmarkRequest.decode(encoded(invalid))
            preconditionFailure("invalid ID accepted")
        } catch {}
        invalid = object
        invalid["actions"] = [["kind": "tap", "x": NSNull(), "y": 20]]
        do { _ = try BenchmarkRequest.decode(encoded(invalid))
            preconditionFailure("null coordinate accepted")
        } catch {}
        for value: Any in [NSNull(), "true", 1] {
            invalid = object
            invalid["memorySampling"] = value
            do { _ = try BenchmarkRequest.decode(encoded(invalid))
                preconditionFailure("invalid memory sampling setting accepted")
            } catch {}
        }
        let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: root) }
        try RunConfiguration().validateResources(root)
        do { try configuration.validateResources(root)
            preconditionFailure("missing boot image accepted")
        } catch {}
        let metadataURL = root.appendingPathComponent("Runtime.json")
        for files in [["boot.art": "image"], ["boot.art": "image", "boot.oat": "code", "boot.vdex": "dex"]] {
            try JSONSerialization.data(withJSONObject: ["build": ["art_boot": ["files": files]]]).write(to: metadataURL)
            if files.count == 3 {
                try configuration.validateResources(root)
            } else {
                do { try configuration.validateResources(root)
                    preconditionFailure("incomplete boot image accepted")
                } catch {}
            }
        }
        try Data("{\"runtime\":\"test-runtime\",\"build\":{\"art_build_inputs\":\"test-art\"}}".utf8).write(to: root.appendingPathComponent("Runtime.json"))
        let target = RunTarget(id: id, name: "test", directory: root, resources: root, log: root.appendingPathComponent("unused.log"))
        let runtime = BenchmarkRuntime()
        let session = SessionModel(runtime: runtime)
        var completionCalls = 0
        session.onFinish = {
            let path = root.appendingPathComponent(request.runID.uuidString + "/result.json")
            let saved = try! JSONSerialization.jsonObject(with: Data(contentsOf: path)) as! [String: Any]
            precondition(saved["status"] as? String == "completed")
            let memory = saved["memory"] as! [String: Any]
            precondition(memory["enabled"] as? Bool == true)
            let phases = memory["phases"] as! [String: Any]
            precondition(phases["stopping"] != nil && phases["stopped"] != nil)
            let raw = try! String(contentsOf: path.deletingLastPathComponent().appendingPathComponent("memory.jsonl"), encoding: .utf8)
            precondition(raw.contains("\"phase\":\"stopped\""))
            completionCalls += 1
        }
        let run = try BenchmarkSession(request: request, session: session, target: target, output: root)
        var finished = false
        run.onFinish = { finished = true }
        try run.start()
        for _ in 0 ..< 100 {
            if finished {
                break
            }
            try await Task.sleep(for: .milliseconds(50))
        }
        precondition(finished && !session.active && runtime.configuration == configuration && completionCalls == 1)
        let result = try JSONSerialization.jsonObject(with: Data(contentsOf: root.appendingPathComponent(request.runID.uuidString + "/result.json"))) as! [String: Any]
        precondition(result["status"] as? String == "completed")
        let resources = result["resources"] as! [String: Any]
        let runtimeManifest = resources["Runtime.json"] as! [String: Any]
        precondition(runtimeManifest["runtime"] as? String == "test-runtime")
        let events = inputLock.withLock { inputEvents }
        precondition(events.first == [Int32(NCE_INPUT_KEY), 102, 0])
        precondition(events.last == [Int32(NCE_INPUT_UP), 50, 50])
        precondition(events.count == 7)
        do { _ = try BenchmarkSession(request: request, session: session, target: target, output: root)
            preconditionFailure("duplicate run accepted")
        } catch {}
        object["runID"] = UUID().uuidString
        object["memorySampling"] = false
        session.onFinish = nil
        let failingRequest = try BenchmarkRequest.decode(encoded(object))
        inputLock.withLock { inputFailure = true }
        let failing = try BenchmarkSession(request: failingRequest, session: session, target: target, output: root)
        finished = false
        failing.onFinish = { finished = true }
        try failing.start()
        for _ in 0 ..< 100 {
            if finished {
                break
            }
            try await Task.sleep(for: .milliseconds(50))
        }
        let failed = try JSONSerialization.jsonObject(with: Data(contentsOf: root.appendingPathComponent(failingRequest.runID.uuidString + "/result.json"))) as! [String: Any]
        precondition(finished && !session.active && failed["status"] as? String == "failed" && failed["error"] != nil)
        precondition((failed["memory"] as? [String: Bool])?["enabled"] == false)
        precondition(!FileManager.default.fileExists(atPath: root.appendingPathComponent(failingRequest.runID.uuidString + "/memory.jsonl").path))
        let series = root.appendingPathComponent("memory.jsonl")
        try memory.flush(to: series)
        try memory.flush(to: series)
        try memory.add(.init(error: 3), elapsedMS: 3000, phase: "stopping", bootStep: 2)
        try memory.flush(to: series)
        let lines = try String(contentsOf: series, encoding: .utf8).split(separator: "\n")
        precondition(lines.count == 5)
        for line in lines {
            _ = try JSONSerialization.jsonObject(with: Data(line.utf8))
        }
        print("benchmark checks passed: configuration, URL validation, IDs, ordered inputs, completed report, input failure and cleanup")
    }
}
