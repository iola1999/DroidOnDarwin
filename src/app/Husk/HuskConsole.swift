// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Darwin

/// 实验构建通过客体串口执行诊断命令；须在 Documents 创建 husk-enable-console。
enum HuskConsole {
    private static let queue = DispatchQueue(label: "husk.console", qos: .utility)
    private static var documents: URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
    }
    static var enabled: Bool {
        FileManager.default.fileExists(atPath: documents.appendingPathComponent("husk-enable-console").path)
    }

    private struct Request: Decodable {
        let id: UUID
        let command: String
        let timeout: Double
    }

    static func poll() {
        guard enabled else { return }
        queue.async { processRequest() }
    }

    private static func processRequest() {
        let fm = FileManager.default
        let input = documents.appendingPathComponent("husk-console-request.json")
        let running = documents.appendingPathComponent("husk-console-running.json")
        let output = documents.appendingPathComponent("husk-console-result.json")
        guard !fm.fileExists(atPath: running.path),
              let data = try? Data(contentsOf: input),
              let request = try? JSONDecoder().decode(Request.self, from: data),
              request.timeout.isFinite, (1...300).contains(request.timeout),
              request.command.utf8.count <= 65536 else { return }
        // 先移动请求，应用退出后由调用方决定是否重试，防止重复执行命令。
        do { try fm.moveItem(at: input, to: running) } catch { return }
        let start = ProcessInfo.processInfo.systemUptime
        HuskLog.log("console", "request \(request.id) started")
        var result: [String: Any] = ["id": request.id.uuidString]
        do {
            let response = try execute(request)
            result["output"] = response.output
            result["exitCode"] = response.status
        } catch {
            result["error"] = error.localizedDescription
        }
        result["elapsedSeconds"] = ProcessInfo.processInfo.systemUptime - start
        do {
            let json = try JSONSerialization.data(withJSONObject: result, options: [.prettyPrinted])
            try json.write(to: output, options: .atomic)
            try fm.removeItem(at: running)
            HuskLog.log("console", "request \(request.id) finished: \(result["exitCode"] ?? result["error"] ?? "unknown")")
        } catch {
            HuskLog.log("console", "could not save result: \(error.localizedDescription)")
        }
    }

    private static func execute(_ request: Request) throws -> (output: String, status: Int) {
        func failure(_ text: String) -> NSError {
            NSError(domain: "husk.console", code: 1, userInfo: [NSLocalizedDescriptionKey: text])
        }
        let fd = socket(AF_INET, SOCK_STREAM, 0)
        guard fd >= 0 else { throw failure("socket failed: \(errno)") }
        defer { close(fd) }
        var noSignal: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, socklen_t(MemoryLayout<Int32>.size))
        var wait = timeval(tv_sec: 1, tv_usec: 0)
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &wait, socklen_t(MemoryLayout<timeval>.size))
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &wait, socklen_t(MemoryLayout<timeval>.size))
        var address = sockaddr_in()
        address.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        address.sin_family = sa_family_t(AF_INET)
        address.sin_port = UInt16(5560).bigEndian
        address.sin_addr.s_addr = inet_addr("127.0.0.1")
        let connected = withUnsafePointer(to: &address) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard connected == 0 else { throw failure("console connect failed: \(errno)") }
        let token = "__HUSK_CONSOLE_\(request.id.uuidString)__"
        let command = "\nstty -echo\n{\n\(request.command)\n}\nprintf '\\n\(token)%s\\n' \"$?\"\n"
        let bytes = Array(command.utf8)
        try bytes.withUnsafeBytes { raw in
            var sent = 0
            while sent < raw.count {
                let count = write(fd, raw.baseAddress!.advanced(by: sent), raw.count - sent)
                if count < 0 && errno == EINTR { continue }
                guard count > 0 else { throw failure("console write failed: \(errno)") }
                sent += count
            }
        }
        let pattern = try NSRegularExpression(pattern: "(?:^|\\n)" + token + "(-?[0-9]+)\\r?\\n")
        let deadline = ProcessInfo.processInfo.systemUptime + request.timeout
        var received = Data()
        var buffer = [UInt8](repeating: 0, count: 16384)
        while ProcessInfo.processInfo.systemUptime < deadline {
            let count = read(fd, &buffer, buffer.count)
            if count < 0 && [EINTR, EAGAIN, EWOULDBLOCK].contains(errno) { continue }
            guard count > 0 else { throw failure("console closed: \(errno)") }
            received.append(contentsOf: buffer.prefix(count))
            guard received.count <= 4 * 1024 * 1024 else { throw failure("console output exceeds 4 MiB") }
            let text = String(decoding: received, as: UTF8.self)
            if let match = pattern.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)),
               let statusRange = Range(match.range(at: 1), in: text),
               let end = Range(match.range, in: text), let status = Int(text[statusRange]) {
                return (String(text[..<end.lowerBound]), status)
            }
        }
        throw failure("console request timed out after \(request.timeout)s")
    }
}
