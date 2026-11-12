// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

struct TranslationConfiguration: Codable, Equatable, Sendable {
    var onlineCPUs = max(RunConfiguration.translationCPUs, 1)
    var cacheMiB = 128
    var codeWatch = true
    var crossPage = true
    var reuseCodeChecks = false
    var routeEpochs = false
    var retainRoutes = false
    var memoryGroups = false
    var vectorTable = false
    var simdMath = false
    var linuxFastpath = false

    static let keys: Set<String> = ["onlineCPUs", "cacheMiB", "codeWatch", "crossPage", "reuseCodeChecks", "routeEpochs", "retainRoutes", "memoryGroups", "vectorTable", "simdMath", "linuxFastpath"]
    static let environmentKeys = ["NCE_TRANSLATION_APP", "NCE_TRANSLATION_GUEST_LAYOUT", "NCE_TRANSLATION_DATA_TBI", "NCE_TRANSLATION_ONLINE_CPUS", "NCE_TRANSLATION_BATCH", "NCE_TRANSLATION_LINUX_FASTPATH", "NCE_DIRECT_MAPPING_ENTRIES", "NCE_DIRECT_ROUTE_ENTRIES", "NCE_DIRECT_SELECTIVE_WRITES", "NCE_TRANSLATION_CACHE_MIB", "NCE_TRANSLATION_CACHE", "NCE_TRANSLATION_EXEC_CACHE", "NCE_TRANSLATION_TLB", "NCE_TRANSLATION_PREEMPTION", "NCE_TRANSLATION_MEMORY_MS", "NCE_TRANSLATION_PARALLEL_PROFILE", "NCE_TRANSLATION_UACCESS_STOP", "NCE_DIRECT_MEMORY", "NCE_DIRECT_NATIVE_EXCLUSIVE", "NCE_DIRECT_CODE_WATCH", "NCE_DIRECT_CROSS_PAGE", "NCE_DIRECT_REUSE_CODE_CHECKS", "NCE_DIRECT_ROUTE_EPOCHS", "NCE_DIRECT_RETAIN_ROUTES", "NCE_DIRECT_MEMORY_GROUPS", "NCE_DIRECT_VECTOR_TABLE", "NCE_DIRECT_SIMD_MATH"]

    func validate(cpus: Int) throws {
        guard onlineCPUs >= 1, onlineCPUs <= cpus else {
            throw NSError(domain: "DroidOnDarwin.Configuration", code: 8,
                          userInfo: [NSLocalizedDescriptionKey: "启用的 CPU 数量需要在 1 到运行库 CPU 数量之间。"])
        }
        guard !linuxFastpath || cpus == 1 else {
            throw NSError(domain: "DroidOnDarwin.Configuration", code: 7,
                          userInfo: [NSLocalizedDescriptionKey: "访存快速路径当前需要单核运行库。"])
        }
        guard [1, 2].contains(cpus), [16, 64, 128].contains(cacheMiB),
              !retainRoutes || (cpus == 1 && codeWatch && routeEpochs) else {
            throw NSError(domain: "DroidOnDarwin.Configuration", code: 3,
                          userInfo: [NSLocalizedDescriptionKey: "翻译缓存配置无效。跨调用复用需要单核运行库、代码页写入跟踪和跳转序号检查。"])
        }
    }

    static func saved(_ defaults: UserDefaults) -> Self {
        Self(onlineCPUs: defaults.object(forKey: "translationOnlineCPUs") as? Int ?? max(RunConfiguration.translationCPUs, 1),
             cacheMiB: defaults.object(forKey: "translationCacheMiB") as? Int ?? 128,
             codeWatch: defaults.object(forKey: "translationCodeWatch") as? Bool ?? true, crossPage: defaults.object(forKey: "translationCrossPage") as? Bool ?? true,
             reuseCodeChecks: defaults.bool(forKey: "translationReuseCodeChecks"), routeEpochs: defaults.bool(forKey: "translationRouteEpochs"),
             retainRoutes: defaults.bool(forKey: "translationRetainRoutes"), memoryGroups: defaults.bool(forKey: "translationMemoryGroups"),
             vectorTable: defaults.bool(forKey: "translationVectorTable"), simdMath: defaults.bool(forKey: "translationSimdMath"), linuxFastpath: defaults.bool(forKey: "translationLinuxFastpath"))
    }

    var environment: [String: String] {
        ["NCE_TRANSLATION_APP": "1", "NCE_TRANSLATION_ONLINE_CPUS": String(onlineCPUs), "NCE_TRANSLATION_BATCH": "50000", "NCE_TRANSLATION_LINUX_FASTPATH": linuxFastpath ? "1" : "0",
         "NCE_DIRECT_MAPPING_ENTRIES": "256", "NCE_DIRECT_ROUTE_ENTRIES": "256", "NCE_DIRECT_SELECTIVE_WRITES": "0",
         "NCE_TRANSLATION_CACHE_MIB": String(cacheMiB), "NCE_TRANSLATION_CACHE": "pages",
         "NCE_TRANSLATION_EXEC_CACHE": "pages", "NCE_TRANSLATION_TLB": "pages", "NCE_TRANSLATION_PREEMPTION": "timer",
         "NCE_TRANSLATION_MEMORY_MS": "0", "NCE_DIRECT_MEMORY": "flat", "NCE_DIRECT_NATIVE_EXCLUSIVE": "1",
         "NCE_DIRECT_CODE_WATCH": codeWatch ? "1" : "0", "NCE_DIRECT_CROSS_PAGE": crossPage ? "1" : "0",
         "NCE_DIRECT_REUSE_CODE_CHECKS": reuseCodeChecks ? "1" : "0", "NCE_DIRECT_ROUTE_EPOCHS": routeEpochs ? "1" : "0",
         "NCE_DIRECT_RETAIN_ROUTES": retainRoutes ? "1" : "0", "NCE_DIRECT_MEMORY_GROUPS": memoryGroups ? "1" : "0",
         "NCE_DIRECT_VECTOR_TABLE": vectorTable ? "1" : "0", "NCE_DIRECT_SIMD_MATH": simdMath ? "1" : "0"]
    }

    static func fromEnvironment(_ values: [String: String]) -> Self {
        Self(onlineCPUs: values["NCE_TRANSLATION_ONLINE_CPUS"].flatMap(Int.init) ?? -1,
             cacheMiB: values["NCE_TRANSLATION_CACHE_MIB"].flatMap(Int.init) ?? -1,
             codeWatch: values["NCE_DIRECT_CODE_WATCH"] == "1", crossPage: values["NCE_DIRECT_CROSS_PAGE"] == "1",
             reuseCodeChecks: values["NCE_DIRECT_REUSE_CODE_CHECKS"] == "1", routeEpochs: values["NCE_DIRECT_ROUTE_EPOCHS"] == "1",
             retainRoutes: values["NCE_DIRECT_RETAIN_ROUTES"] == "1", memoryGroups: values["NCE_DIRECT_MEMORY_GROUPS"] == "1",
             vectorTable: values["NCE_DIRECT_VECTOR_TABLE"] == "1", simdMath: values["NCE_DIRECT_SIMD_MATH"] == "1", linuxFastpath: values["NCE_TRANSLATION_LINUX_FASTPATH"] == "1")
    }
}
