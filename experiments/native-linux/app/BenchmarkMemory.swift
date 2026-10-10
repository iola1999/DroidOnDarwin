// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Darwin

struct BenchmarkMemoryReading {
    var bytes: [String: UInt64] = [:]
    var processPeaks: [String: UInt64] = [:]
    var error: Int32?
    var queryMicroseconds: Double = 0

    static func current() -> Self {
        let started = ProcessInfo.processInfo.systemUptime
        var info = task_vm_info_data_t()
        let capacity = MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<integer_t>.size
        var count = mach_msg_type_number_t(capacity)
        let status = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: capacity) {
                task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count)
            }
        }
        var reading = Self()
        reading.queryMicroseconds = (ProcessInfo.processInfo.systemUptime - started) * 1_000_000
        let returnedBytes = Int(count) * MemoryLayout<integer_t>.size
        let footprintEnd = MemoryLayout<task_vm_info_data_t>.offset(of: \.phys_footprint)! + MemoryLayout<UInt64>.size
        guard status == KERN_SUCCESS, returnedBytes >= footprintEnd else {
            reading.error = status == KERN_SUCCESS ? KERN_INVALID_ARGUMENT : status
            return reading
        }
        reading.bytes = ["footprint": info.phys_footprint, "resident": info.resident_size,
                         "compressed": info.compressed, "virtual": info.virtual_size]
        reading.processPeaks["resident"] = info.resident_size_peak
        let peakEnd = MemoryLayout<task_vm_info_data_t>.offset(of: \.ledger_phys_footprint_peak)! + MemoryLayout<Int64>.size
        if returnedBytes >= peakEnd, info.ledger_phys_footprint_peak >= 0 {
            reading.processPeaks["footprint"] = UInt64(info.ledger_phys_footprint_peak)
        }
        return reading
    }
}

private struct MemoryMetric {
    var count = 0
    var sum: Double = 0
    var latest: UInt64 = 0
    var peak: UInt64 = 0
    mutating func add(_ value: UInt64) {
        count += 1
        sum += Double(value)
        latest = value
        peak = max(peak, value)
    }
    var json: [String: Any] {
        ["samples": count, "latest_bytes": latest, "mean_bytes": sum / Double(count), "sampled_peak_bytes": peak]
    }
}

private struct MemoryStage {
    var attempts = 0
    var failures = 0
    var firstMS: Double?
    var latestMS: Double = 0
    var latestSuccessMS: Double?
    var maximumGapMS: Double = 0
    var metrics: [String: MemoryMetric] = [:]
    mutating func add(_ reading: BenchmarkMemoryReading, elapsedMS: Double) {
        if firstMS != nil {
            maximumGapMS = max(maximumGapMS, elapsedMS - latestMS)
        } else {
            firstMS = elapsedMS
        }
        attempts += 1
        latestMS = elapsedMS
        if reading.error != nil {
            failures += 1
        } else {
            latestSuccessMS = elapsedMS
            for (name, value) in reading.bytes {
                metrics[name, default: MemoryMetric()].add(value)
            }
        }
    }
    var json: [String: Any] {
        var result: [String: Any] = ["attempts": attempts, "failures": failures,
                                     "latest_attempt_ms": latestMS, "maximum_gap_ms": maximumGapMS,
                                     "metrics": metrics.mapValues(\.json)]
        if let firstMS {
            result["first_attempt_ms"] = firstMS
        }
        if let latestSuccessMS {
            result["latest_success_ms"] = latestSuccessMS
        }
        return result
    }
}

struct BenchmarkMemory {
    private var overall = MemoryStage()
    private var phases: [String: MemoryStage] = [:]
    private var bootSteps: [String: MemoryStage] = [:]
    private var processPeaks: [String: UInt64] = [:]
    private var queryTotalUS: Double = 0
    private var queryMaxUS: Double = 0
    private var pending = Data()

    mutating func add(_ reading: BenchmarkMemoryReading, elapsedMS: Double, phase: String, bootStep: Int) throws {
        overall.add(reading, elapsedMS: elapsedMS)
        phases[phase, default: MemoryStage()].add(reading, elapsedMS: elapsedMS)
        if phase == "booting" {
            bootSteps[String(bootStep), default: MemoryStage()].add(reading, elapsedMS: elapsedMS)
        }
        for (name, value) in reading.processPeaks {
            processPeaks[name] = max(processPeaks[name, default: 0], value)
        }
        queryTotalUS += reading.queryMicroseconds
        queryMaxUS = max(queryMaxUS, reading.queryMicroseconds)
        var sample: [String: Any] = ["elapsed_ms": elapsedMS, "phase": phase, "boot_step": bootStep,
                                     "query_us": reading.queryMicroseconds, "bytes": reading.bytes,
                                     "process_lifetime_peak_bytes": reading.processPeaks]
        if let error = reading.error {
            sample["error"] = error
        }
        pending.append(try JSONSerialization.data(withJSONObject: sample, options: [.sortedKeys]))
        pending.append(0x0A)
    }

    mutating func flush(to url: URL) throws {
        guard !pending.isEmpty else {
            return
        }
        if !FileManager.default.fileExists(atPath: url.path) {
            try Data().write(to: url)
        }
        let file = try FileHandle(forWritingTo: url)
        defer { try? file.close() }
        try file.seekToEnd()
        try file.write(contentsOf: pending)
        pending.removeAll(keepingCapacity: true)
    }

    var json: [String: Any] {
        ["enabled": true, "source": "task_info(TASK_VM_INFO), current host process",
         "pid": ProcessInfo.processInfo.processIdentifier, "target_interval_ms": 1000,
         "mean": "arithmetic mean of successful periodic and boundary samples",
         "peak_scope": "sampled peaks cover each observed phase; kernel peaks cover process lifetime",
         "overall": overall.json, "phases": phases.mapValues(\.json), "boot_steps": bootSteps.mapValues(\.json),
         "process_lifetime_peak_bytes": processPeaks, "query_total_us": queryTotalUS,
         "query_max_us": queryMaxUS, "series": "memory.jsonl"]
    }
}
