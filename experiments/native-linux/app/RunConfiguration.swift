// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

struct RunConfiguration: Codable, Equatable, Sendable {
    var nterp = translationCPUs > 0
    var expandedJIT = false
    var fastBaseline = false
    var aot = translationCPUs == 0
    var bootImage = false
    var directFault = true
    var faultPrefetch = false
    var sparseScan = true
    var performanceLogging = false
    var diagnostics = false
    var schedulerSliceUS = 0

    var translation: TranslationConfiguration? = translationCPUs > 0 ? .init() : nil
    static var translationCPUs: Int {
        Bundle.main.object(forInfoDictionaryKey: "DroidOnDarwinTranslationCPUs") as? Int ?? 0
    }

    static let keys: Set<String> = ["nterp", "expandedJIT", "fastBaseline", "aot", "bootImage", "directFault", "faultPrefetch", "sparseScan", "performanceLogging", "diagnostics", "schedulerSliceUS"]
    static let environmentKeys = ["NCE_ART_NTERP", "NCE_ART_EXPANDED_JIT", "NCE_ART_FAST_BASELINE", "NCE_ART_AOT", "NCE_ART_BOOT_IMAGE", "NCE_DIRECT_FAULT", "NCE_FAULT_PREFETCH", "NCE_SPARSE_SCAN", "NCE_MAP_PROFILE", "NCE_DIAGNOSTICS", "NCE_SCHED_SLICE_US"] + TranslationConfiguration.environmentKeys

    var schedulerSliceIsValid: Bool {
        schedulerSliceUS == 0 || (100 ... 100000).contains(schedulerSliceUS)
    }

    static func saved(_ defaults: UserDefaults = .standard, diagnostics: Bool = false) -> Self {
        Self(nterp: defaults.object(forKey: "artNterp") as? Bool ?? (translationCPUs > 0),
             expandedJIT: defaults.bool(forKey: "artExpandedJIT"),
             fastBaseline: defaults.bool(forKey: "artFastBaseline"),
             aot: translationCPUs == 0 && (defaults.object(forKey: "artAOT") as? Bool ?? true),
             bootImage: translationCPUs == 0 && defaults.bool(forKey: "artBootImage"),
             directFault: translationCPUs > 0 || (defaults.object(forKey: "directFault") as? Bool ?? true),
             faultPrefetch: translationCPUs == 0 && defaults.bool(forKey: "faultPrefetch"),
             sparseScan: translationCPUs > 0 || (defaults.object(forKey: "sparseScan") as? Bool ?? true),
             performanceLogging: defaults.bool(forKey: "performanceLogging"), diagnostics: diagnostics,
             schedulerSliceUS: defaults.integer(forKey: "schedulerSliceUS"),
             translation: translationCPUs > 0 ? .saved(defaults) : nil)
    }

    var environment: [String: String] {
        var values = ["NCE_ART_NTERP": nterp ? "1" : "0", "NCE_DIRECT_FAULT": directFault ? "1" : "0",
                      "NCE_ART_EXPANDED_JIT": expandedJIT ? "1" : "0",
                      "NCE_ART_FAST_BASELINE": fastBaseline ? "1" : "0",
                      "NCE_ART_AOT": aot ? "1" : "0",
                      "NCE_ART_BOOT_IMAGE": bootImage ? "1" : "0",
                      "NCE_FAULT_PREFETCH": faultPrefetch ? "1" : "0",
                      "NCE_SPARSE_SCAN": sparseScan ? "1" : "0", "NCE_MAP_PROFILE": performanceLogging ? "1" : "0",
                      "NCE_SCHED_SLICE_US": String(schedulerSliceUS)]
        if let translation {
            values.merge(translation.environment) { _, new in new }
            values["NCE_TRANSLATION_PARALLEL_PROFILE"] = performanceLogging ? "1" : "0"
            values["NCE_TRANSLATION_UACCESS_STOP"] = Self.translationCPUs == 2 ? "code" : "all"
        }
        if diagnostics {
            values["NCE_DIAGNOSTICS"] = "1"
        }
        return values
    }

    func validateResources(_ resources: URL) throws {
        guard schedulerSliceIsValid else {
            throw NSError(domain: "DroidOnDarwin.Configuration", code: 2,
                          userInfo: [NSLocalizedDescriptionKey: "调度时间片需要为 0 或 100–100000 微秒"])
        }
        let url = resources.appendingPathComponent("Runtime.json")
        let metadata = (try? Data(contentsOf: url)).flatMap { try? JSONSerialization.jsonObject(with: $0) as? [String: Any] }
        if let execution = metadata?["execution"] as? [String: Any], execution["engine"] as? String == "direct" {
            guard let translation, let cpus = execution["cpus"] as? Int, !aot, !bootImage else {
                throw NSError(domain: "DroidOnDarwin.Configuration", code: 4,
                              userInfo: [NSLocalizedDescriptionKey: "当前翻译运行库需要对应的缓存配置，预编译代码与公共启动镜像尚无匹配产物。"])
            }
            guard directFault, !faultPrefetch, sparseScan else {
                throw NSError(domain: "DroidOnDarwin.Configuration", code: 6,
                              userInfo: [NSLocalizedDescriptionKey: "当前翻译运行库使用独立访存设置，请保持原生数据页选项的默认值"])
            }
            try translation.validate(cpus: cpus)
        } else if translation != nil {
            throw NSError(domain: "DroidOnDarwin.Configuration", code: 5,
                          userInfo: [NSLocalizedDescriptionKey: "当前运行库无法应用翻译缓存配置"])
        }
        guard bootImage else {
            return
        }
        guard let build = metadata?["build"] as? [String: Any],
              let image = build["art_boot"] as? [String: Any],
              let files = image["files"] as? [String: String],
              Set(files.keys) == ["boot.art", "boot.oat", "boot.vdex"] else {
            throw NSError(domain: "DroidOnDarwin.Configuration", code: 1,
                          userInfo: [NSLocalizedDescriptionKey: "当前构建缺少公共启动镜像，请关闭该选项或安装包含镜像的版本"])
        }
    }

    static func fromEnvironment(_ values: [String: String]) -> Self {
        let slice = values["NCE_SCHED_SLICE_US"].flatMap(Int.init) ?? (values["NCE_SCHED_SLICE_US"] == nil ? 0 : -1)
        return Self(nterp: values["NCE_ART_NTERP"] == "1", expandedJIT: values["NCE_ART_EXPANDED_JIT"] == "1", fastBaseline: values["NCE_ART_FAST_BASELINE"] == "1", aot: values["NCE_ART_AOT"] != "0", bootImage: values["NCE_ART_BOOT_IMAGE"] == "1", directFault: values["NCE_DIRECT_FAULT"] != "0",
                    faultPrefetch: values["NCE_FAULT_PREFETCH"] == "1", sparseScan: values["NCE_SPARSE_SCAN"] != "0", performanceLogging: values["NCE_MAP_PROFILE"] == "1",
                    diagnostics: values["NCE_DIAGNOSTICS"] == "1", schedulerSliceUS: slice,
                    translation: values["NCE_TRANSLATION_APP"] == "1" ? .fromEnvironment(values) : nil)
    }
}
