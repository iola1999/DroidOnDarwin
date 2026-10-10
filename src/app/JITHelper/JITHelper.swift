// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted from Leviidev/Husk, with contributions by Dan Perks. See ACKNOWLEDGEMENTS.md.
import Foundation
import StikJIT

/// The debugger process for the built-in StikJIT connection.
///
/// A process cannot synchronously debug itself, so DroidOnDarwin starts this extension,
/// sends it its PID, the pairing file and jit.js, and StikJIT attaches over
/// LocalDevVPN and services the script's trap requests until DroidOnDarwin detaches.
private enum JITWork {
    /// One request at a time: enabling JIT holds this queue while the debugger is attached.
    static let queue = DispatchQueue(label: "app.droidondarwin.ios.jit-helper")

    static func handle(_ request: JITRequest) -> JITRequest.Response {
        let manager = FileManager.default
        let root = manager.urls(for: .libraryDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("StikJIT", isDirectory: true)
        let paths = DDIPaths.default(in: root)
        let documents = manager.urls(for: .documentDirectory, in: .userDomainMask)[0]
        try? manager.createDirectory(at: documents, withIntermediateDirectories: true)
        let logURL = documents.appendingPathComponent("dod-jit-helper.log")
        manager.createFile(atPath: logURL.path, contents: nil)
        let logFile = try? FileHandle(forWritingTo: logURL)
        defer { try? logFile?.close() }
        func report(_ message: String) {
            NSLog("[DroidOnDarwinJIT] %{public}@", message)
            try? logFile?.write(contentsOf: Data((message + "\n").utf8))
        }

        do {
            try manager.createDirectory(at: root, withIntermediateDirectories: true)
            let pairingData = request.pairingData
            guard !pairingData.isEmpty else {
                throw NSError(domain: "JITHelper", code: 1,
                              userInfo: [NSLocalizedDescriptionKey: "The pairing file was not provided."])
            }
            let pairingURL = root.appendingPathComponent("pairingFile-\(UUID().uuidString).plist")
            try pairingData.write(to: pairingURL, options: .atomic)
            defer { try? manager.removeItem(at: pairingURL) }

            guard request.targetPID > 0, !request.scriptBase64.isEmpty else {
                throw NSError(domain: "JITHelper", code: 2,
                              userInfo: [NSLocalizedDescriptionKey: "The target process or JIT script was not provided."])
            }
            try StikJIT.enableJIT(
                targetPID: request.targetPID,
                pairingFile: pairingURL,
                ddiPaths: paths,
                script: .customBase64(request.scriptBase64),
                forceScript: true,
                preparationProgress: { report("prepare: \(String(describing: $0))") },
                progress: { report($0) })
            return .init(success: true, message: "DroidOnDarwin detached cleanly from its built-in JIT helper.")
        } catch {
            report("failed: \(error.localizedDescription)")
            return .init(success: false, message: error.localizedDescription)
        }
    }
}

/// NSExtensionPrincipalClass (Info.plist). The request arrives as JSON in the
/// first input item; the response goes back as JSON in the item the request
/// completes with. Enable completes only once DroidOnDarwin's script has detached, so
/// the request, and this process, last as long as the debugger does.
@objc(JITHelperHandler)
final class JITHelperHandler: NSObject, NSExtensionRequestHandling {
    func beginRequest(with context: NSExtensionContext) {
        let info = (context.inputItems.first as? NSExtensionItem)?.userInfo
        let data = info?[JITRequest.itemKey] as? Data
        JITWork.queue.async {
            let response: JITRequest.Response
            if let data, let request = try? JSONDecoder().decode(JITRequest.self, from: data) {
                response = JITWork.handle(request)
            } else {
                response = .init(success: false, message: "DroidOnDarwin's JIT helper received no request.")
            }
            let item = NSExtensionItem()
            item.userInfo = [JITRequest.Response.itemKey: (try? JSONEncoder().encode(response)) ?? Data()]
            context.completeRequest(returningItems: [item], completionHandler: nil)
        }
    }
}
