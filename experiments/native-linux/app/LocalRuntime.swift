// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Metal
#if os(iOS)
    import UIKit
#endif

@MainActor
final class LocalRuntime: SessionRuntime {
    private enum Stage { case idle, preparing, waiting, core, finished }
    private var stage = Stage.idle
    private var storage: NceStorage?
    private var event: ((RuntimeEvent) -> Void)?
    private var target: RunTarget?
    private var stopRequested = false
    private var generation = 0
    private(set) var restartRequired = false
    private var jitTimer: Timer?
    private let queue = DispatchQueue(label: "app.droidondarwin.prepare", qos: .userInitiated)
    #if os(macOS)
        let display = NceDisplay(frame: CGRect(x: 0, y: 0, width: 360, height: 640), metalLayer: ProfiledMetalLayer())
        var metalLayer: CAMetalLayer {
            display.metalLayer
        }
    #else
        let metalLayer = ProfiledMetalLayer()
    #endif
    init() {
        #if os(iOS)
            dod_ios_jit_install_trap_handler()
            metalLayer.device = MTLCreateSystemDefaultDevice()
            metalLayer.pixelFormat = .bgra8Unorm
            metalLayer.framebufferOnly = false
            metalLayer.drawableSize = CGSize(width: 360, height: 640)
        #endif
    }
    func start(_ target: RunTarget, configuration: RunConfiguration, event: @escaping (RuntimeEvent) -> Void) {
        guard !restartRequired else {
            return
        }
        generation += 1
        self.event = event
        self.target = target
        stopRequested = false
        stage = .preparing
        let prepared = NceStorage()
        queue.async {
            do {
                // Executable layouts and embedded ART assets belong to the installed runtime.
                try configuration.validateResources(Bundle.main.resourceURL ?? target.resources)
                try prepared.prepareResources(target.resources, directory: target.directory)
                DispatchQueue.main.async {
                    self.storage = prepared
                    if self.stopRequested {
                        self.finish(nil)
                        return
                    }
                    guard self.metalLayer.device != nil else {
                        self.finish("无法初始化 Metal 显示")
                        return
                    }
                    guard nce_open_log(target.log.path) == 0 else {
                        self.finish("日志文件无法写入")
                        return
                    }
                    setenv("NCE_STORAGE_IMAGE", prepared.disk.path, 1)
                    setenv("NCE_SYSTEM_IMAGE", target.resources.appendingPathComponent("system.img").path, 1)
                    setenv("NCE_DESKTOP", "1", 1)
                    setenv("NCE_MM_MODE", "demand", 1)
                    if configuration.translation != nil {
                        setenv("NCE_TRANSLATION_FILES", (Bundle.main.resourceURL ?? target.resources).appendingPathComponent("Translation").path, 1)
                        setenv("NCE_TRANSLATION_SYSTEM", target.resources.appendingPathComponent("system.img").path, 1)
                        setenv("NCE_TRANSLATION_DATA", prepared.disk.path, 1)
                        setenv("NCE_TRANSLATION_WORKLOAD", "desktop", 1)
                    }
                    for key in RunConfiguration.environmentKeys {
                        unsetenv(key)
                    }
                    for (key, value) in configuration.environment {
                        setenv(key, value, 1)
                    }
                    if let data = try? JSONEncoder().encode(configuration), let json = String(data: data, encoding: .utf8) {
                        nce_log("{\"configuration\":\(json)}")
                    }
                    nce_graphics_set_window(Unmanaged.passUnretained(self.metalLayer).toOpaque())
                    #if os(iOS)
                        UIApplication.shared.isIdleTimerDisabled = true
                        self.connectJIT()
                    #else
                        setenv("ANGLE_DEFAULT_PLATFORM", "metal", 1)
                        self.run()
                    #endif
                }
            } catch { DispatchQueue.main.async { self.finish(error.localizedDescription) } }
        }
    }
    private func run() {
        guard !stopRequested, !restartRequired, let target else {
            return
        }
        stage = .core
        restartRequired = true
        jitTimer?.invalidate()
        jitTimer = nil
        event?(.booting)
        // 客体会切换线程与栈；核心调用始终在 Swift 主线程以外执行。
        let kernelResources = RunConfiguration.translationCPUs > 0 ? (Bundle.main.resourceURL ?? target.resources) : target.resources
        DispatchQueue.global(qos: .userInitiated).async {
            let result = nce_lkl_run(kernelResources.appendingPathComponent("lkl-image.bin").path)
            fflush(stdout)
            fflush(stderr)
            DispatchQueue.main.async { self.finish(result == 0 ? nil : "Android 运行失败，请查看日志") }
        }
    }
    private func finish(_ error: String?) {
        stage = .finished
        jitTimer?.invalidate()
        jitTimer = nil
        #if os(iOS)
            UIApplication.shared.isIdleTimerDisabled = false
        #endif
        // 核心异常返回可能保留映射与文件引用；保持目录锁到进程退出。
        if !restartRequired || error == nil {
            storage = nil
        }
        event?(.finished(error))
    }
    func stop() {
        stopRequested = true
        switch stage {
        case .waiting: generation += 1
            finish(nil)
        case .core: requestStop()
        case .preparing: break
        default: break
        }
    }
    private func requestStop() {
        DispatchQueue.global(qos: .userInitiated).async {
            let result = nce_session_request_stop()
            DispatchQueue.main.async {
                // Linux 初始化早期尚未注册停止入口。
                if result != 0 && self.stage == .core && self.stopRequested {
                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
                        if self.stage == .core {
                            self.requestStop()
                        }
                    }
                }
            }
        }
    }
    func openDisplay() {}
    #if os(iOS)
        private func connectJIT() {
            if nce_debugger_attached() {
                run()
                return
            }
            guard let url = Bundle.main.url(forResource: "jit", withExtension: "js"),
                  let script = try? Data(contentsOf: url) else {
                finish("安装包缺少 JIT 脚本")
                return
            }
            stage = .waiting
            let attempt = generation
            let external = UserDefaults.standard.string(forKey: "jitMethod") == "external"
            let deadline = Date().addingTimeInterval(external ? 180 : 90)
            jitTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
                Task { @MainActor in
                    guard let self, self.stage == .waiting, attempt == self.generation else {
                        return
                    }
                    if UIApplication.shared.applicationState == .active && nce_debugger_attached() {
                        self.run()
                    } else if Date() > deadline {
                        self.finish("JIT 连接超时，请检查 LocalDevVPN 后重试")
                    }
                }
            }
            if external {
                let encoded = script.base64EncodedString().replacingOccurrences(of: "+", with: "-")
                    .replacingOccurrences(of: "/", with: "_").replacingOccurrences(of: "=", with: "")
                var components = URLComponents(string: "stikjit://enable-jit")!
                components.queryItems = [URLQueryItem(name: "bundle-id", value: Bundle.main.bundleIdentifier),
                                         URLQueryItem(name: "script-data", value: encoded)]
                guard let url = components.url, UIApplication.shared.canOpenURL(url) else {
                    finish("未找到 StikDebug，请在设置中选择 JIT 连接方式")
                    return
                }
                event?(.connecting("请在 StikDebug 中启用 JIT，然后返回 DroidOnDarwin"))
                UIApplication.shared.open(url)
            } else {
                guard let pairing = try? JITPairingFileStore.data() else {
                    finish("请先在设置中导入配对文件，或选择 StikDebug")
                    return
                }
                event?(.connecting("正在连接手机内的 JIT 助手"))
                BuiltInJIT.send(JITRequest(targetPID: getpid(), pairingData: pairing, scriptBase64: script.base64EncodedString())) { [weak self] result in
                    guard let self, attempt == generation, stage == .waiting else {
                        return
                    }
                    switch result {
                    case .success(let response):
                        if !response.success {
                            finish(response.message)
                        }
                    case .failure(let error): finish(error.localizedDescription)
                    }
                }
            }
        }
    #endif
}
