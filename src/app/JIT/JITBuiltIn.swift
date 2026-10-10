// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted from Leviidev/Husk, with contributions by Dan Perks. See ACKNOWLEDGEMENTS.md.
import Foundation

/// The private NSExtension API (ExtensionFoundation) that starts an app
/// extension of DroidOnDarwin's own: the same calls LiveContainer uses to start its
/// LiveProcess.
@objc private protocol NSExtensionClassShim {
    @objc(extensionWithIdentifier:error:)
    func `extension`(withIdentifier identifier: String) throws -> AnyObject
}

@objc private protocol NSExtensionShim {
    @objc(beginExtensionRequestWithInputItems:completion:)
    func beginExtensionRequest(withInputItems items: [Any], completion: @escaping (NSUUID?) -> Void)
    @objc(pidForRequestIdentifier:)
    func pid(forRequestIdentifier identifier: NSUUID) -> Int32
    @objc(setRequestCompletionBlock:)
    func setRequestCompletionBlock(_ block: @escaping (NSUUID?, [Any]?) -> Void)
    @objc(setRequestCancellationBlock:)
    func setRequestCancellationBlock(_ block: @escaping (NSUUID?, NSError?) -> Void)
    @objc(setRequestInterruptionBlock:)
    func setRequestInterruptionBlock(_ block: @escaping (NSUUID?) -> Void)
}

/// Starts the helper process that debugs the app. The bundle identifier is read
/// from its installed Info.plist because sideloaders can rewrite identifiers.
/// Each request remains active until the app detaches or the extension fails.
@MainActor
enum BuiltInJIT {
    static let helperFile = "JITHelper.appex"

    /// Requests that have started and not finished, kept alive until they do.
    private static var running: [HelperRequest] = []

    static var unavailableReason: String? {
        #if targetEnvironment(simulator)
            return "Built-in JIT is available only on a physical device."
        #else
            guard #available(iOS 26.0, *) else {
                return "Built-in JIT needs iOS 26 or later. Use StikDebug instead."
            }
            if getenv("LC_HOME_PATH") != nil {
                return "Built-in JIT is unavailable inside LiveContainer. Use StikDebug instead."
            }
            return nil
        #endif
    }

    static var isAvailable: Bool {
        unavailableReason == nil
    }

    /// The helper's bundle identifier in this installation; nil when a sideloader left it out.
    static var helperIdentifier: String? {
        guard let url = Bundle.main.builtInPlugInsURL?.appendingPathComponent(helperFile) else {
            return nil
        }
        return Bundle(url: url)?.bundleIdentifier
    }

    static func send(_ request: JITRequest,
                     started: @escaping () -> Void = {},
                     completion: @escaping (Result<JITRequest.Response, Error>) -> Void) {
        func fail(_ code: Int, _ message: String) {
            AppLog.log("jit-helper", message)
            completion(.failure(HelperRequest.error(code, message)))
        }
        guard unavailableReason == nil else {
            fail(1, unavailableReason ?? "Built-in JIT is unavailable.")
            return
        }
        guard let identifier = helperIdentifier else {
            fail(2, "DroidOnDarwin's JIT helper is missing from this installation. Reinstall DroidOnDarwin, and keep "
                + "its app extensions if your sideloader asks, or use StikDebug.")
            return
        }
        if NSClassFromString("NSExtension") == nil {
            dlopen("/System/Library/Frameworks/ExtensionFoundation.framework/ExtensionFoundation", RTLD_NOW)
        }
        guard let extensionClass = NSClassFromString("NSExtension") else {
            fail(3, "iOS did not provide the extension API DroidOnDarwin's JIT helper needs.")
            return
        }
        let factory = unsafeBitCast(extensionClass as AnyObject, to: NSExtensionClassShim.self)
        let found: AnyObject
        do { found = try factory.extension(withIdentifier: identifier) }
        catch {
            fail(2, "DroidOnDarwin's JIT helper (\(identifier)) could not be found: \(error.localizedDescription) "
                + "Reinstall DroidOnDarwin.")
            return
        }
        guard let data = try? JSONEncoder().encode(request) else {
            fail(4, "The request for DroidOnDarwin's JIT helper could not be encoded.")
            return
        }
        let helper = HelperRequest(extension: found,
                                   completion: completion)
        running.append(helper)
        helper.begin(data: data, identifier: identifier, started: started)
    }

    fileprivate static func finished(_ helper: HelperRequest) {
        running.removeAll { $0 === helper }
    }
}

/// One request to the helper, from its start to its one result.
@MainActor
private final class HelperRequest {
    private let shim: NSExtensionShim
    private let `extension`: AnyObject // the NSExtension, kept alive for the request
    private var completion: ((Result<JITRequest.Response, Error>) -> Void)?

    init(extension found: AnyObject,
         completion: @escaping (Result<JITRequest.Response, Error>) -> Void) {
        self.extension = found
        self.shim = unsafeBitCast(found, to: NSExtensionShim.self)
        self.completion = completion
    }

    func begin(data: Data, identifier: String, started: @escaping () -> Void) {
        // The blocks can arrive on any queue; everything below runs on the main thread.
        shim.setRequestCompletionBlock { [weak self] _, items in
            let info = (items?.first as? NSExtensionItem)?.userInfo
            let payload = info?[JITRequest.Response.itemKey] as? Data
            DispatchQueue.main.async { MainActor.assumeIsolated { self?.returned(payload) } }
        }
        shim.setRequestCancellationBlock { [weak self] _, error in
            let message = error?.localizedDescription ?? "the request was cancelled"
            DispatchQueue.main.async { MainActor.assumeIsolated {
                self?.end(.failure(Self.error(5, "DroidOnDarwin's JIT helper stopped: \(message)")))
            } }
        }
        shim.setRequestInterruptionBlock { [weak self] _ in
            DispatchQueue.main.async { MainActor.assumeIsolated {
                self?.end(.failure(Self.error(6, "DroidOnDarwin's JIT helper stopped unexpectedly. Try again.")))
            } }
        }
        let item = NSExtensionItem()
        item.userInfo = [JITRequest.itemKey: data]
        shim.beginExtensionRequest(withInputItems: [item]) { [weak self] uuid in
            DispatchQueue.main.async { MainActor.assumeIsolated {
                guard let self else {
                    return
                }
                guard let uuid else {
                    self.end(.failure(Self.error(7, "DroidOnDarwin's JIT helper did not start. Reinstall DroidOnDarwin, and "
                            + "keep its app extensions if your sideloader asks.")))
                    return
                }
                AppLog.log("jit-helper", "enable: started \(identifier) as pid "
                    + "\(self.shim.pid(forRequestIdentifier: uuid))")
                started()
            } }
        }
    }

    private func returned(_ payload: Data?) {
        guard let payload,
              let response = try? JSONDecoder().decode(JITRequest.Response.self, from: payload) else {
            end(.failure(Self.error(8, "DroidOnDarwin's JIT helper finished without an answer.")))
            return
        }
        AppLog.log("jit-helper", "enable: finished success=\(response.success ? 1 : 0)")
        end(.success(response))
    }

    private func end(_ result: Result<JITRequest.Response, Error>) {
        guard let completion else {
            return
        } // the first result counts
        self.completion = nil
        if case .failure(let error) = result {
            AppLog.log("jit-helper", "enable: \(error.localizedDescription)")
        }
        BuiltInJIT.finished(self)
        completion(result)
    }

    nonisolated static func error(_ code: Int, _ message: String) -> NSError {
        NSError(domain: "BuiltInJIT", code: code, userInfo: [NSLocalizedDescriptionKey: message])
    }
}
